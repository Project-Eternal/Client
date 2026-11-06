/* SQL files and AzerothCore's update bookkeeping on the shim (see include/mysql_sqlite.h).
   AzerothCore's own updater runs every file through the mysql command line program (a separate process), which a WebAssembly module cannot start;
   this does the same work through the shim: the setup tool builds the databases from AzerothCore's SQL with it, and the server applies the
   pending/custom directories at the first open of a database (DatabaseOptions::updates_source). */

#include "mysql_sqlite.h"
#include "shim_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace shim
{
    std::vector<std::string> split_statements(std::string const& s)
    {
        std::vector<std::string> out;
        std::string cur;
        size_t i = 0, n = s.size();
        auto flush = [&]() {
            size_t a = cur.find_first_not_of(" \t\r\n");
            if (a != std::string::npos)
            {
                size_t b = cur.find_last_not_of(" \t\r\n");
                out.push_back(cur.substr(a, b - a + 1));
            }
            cur.clear();
        };
        while (i < n)
        {
            char c = s[i];
            if (c == '\'' || c == '"' || c == '`')
            {
                size_t j = i + 1;
                while (j < n)
                {
                    if (s[j] == '\\' && c != '`' && j + 1 < n) { j += 2; continue; }
                    if (s[j] == c) { if (j + 1 < n && s[j + 1] == c) { j += 2; continue; } break; }
                    ++j;
                }
                j = std::min(n, j + 1);
                cur.append(s, i, j - i);
                i = j;
                continue;
            }
            if ((c == '-' && i + 1 < n && s[i + 1] == '-' && (i + 2 >= n || isspace(static_cast<unsigned char>(s[i + 2])))) || c == '#')
            {
                size_t j = s.find('\n', i);
                i = j == std::string::npos ? n : j;
                cur += ' ';
                continue;
            }
            if (c == '/' && i + 1 < n && s[i + 1] == '*')
            {
                // comments, MySQL's /*!NNNNN ... */ version comments too (session settings of the dumps)
                size_t j = s.find("*/", i + 2);
                i = j == std::string::npos ? n : j + 2;
                cur += ' ';
                continue;
            }
            if (c == ';')
            {
                flush();
                ++i;
                continue;
            }
            cur += c;
            ++i;
        }
        flush();
        return out;
    }

    // SHA-1 (FIPS 180-1), for the `hash` column of `updates` (AzerothCore: SHA1 of the file's bytes, upper case hex)
    std::string sha1_hex_upper(std::string const& bytes)
    {
        uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
        std::string m = bytes;
        uint64_t bits = uint64_t(bytes.size()) * 8;
        m += char(0x80);
        while (m.size() % 64 != 56) m += char(0);
        for (int k = 7; k >= 0; --k) m += char((bits >> (k * 8)) & 0xff);
        auto rol = [](uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
        for (size_t off = 0; off < m.size(); off += 64)
        {
            uint32_t w[80];
            for (int t = 0; t < 16; ++t)
                w[t] = uint32_t(uint8_t(m[off + t * 4])) << 24 | uint32_t(uint8_t(m[off + t * 4 + 1])) << 16 | uint32_t(uint8_t(m[off + t * 4 + 2])) << 8 | uint32_t(uint8_t(m[off + t * 4 + 3]));
            for (int t = 16; t < 80; ++t) w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int t = 0; t < 80; ++t)
            {
                uint32_t f, k;
                if (t < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
                else if (t < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
                else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
                uint32_t temp = rol(a, 5) + f + e + k + w[t];
                e = d; d = c; c = rol(b, 30); b = a; a = temp;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }
        char out[41];
        for (int i = 0; i < 5; ++i) snprintf(out + i * 8, 9, "%08X", h[i]);
        return std::string(out, 40);
    }
}

namespace
{
    bool read_file(std::string const& path, std::string* out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        *out = ss.str();
        return true;
    }

    std::string first_word(std::string const& s)
    {
        size_t i = 0;
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t j = i;
        while (j < s.size() && (isalpha(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
        std::string w = s.substr(i, j - i);
        for (char& c : w) c = char(tolower(static_cast<unsigned char>(c)));
        return w;
    }

    bool query(MYSQL* m, std::string const& sql)
    {
        return mysql_query(m, sql.c_str()) == 0;
    }

    // the rows of a text query as strings
    bool rows(MYSQL* m, std::string const& sql, std::vector<std::vector<std::string>>* out)
    {
        if (!query(m, sql))
            return false;
        MYSQL_RES* r = mysql_store_result(m);
        if (!r)
            return true;
        unsigned n = mysql_field_count(m);
        while (MYSQL_ROW row = mysql_fetch_row(r))
        {
            unsigned long* len = mysql_fetch_lengths(r);
            std::vector<std::string> v;
            for (unsigned i = 0; i < n; ++i)
                v.push_back(row[i] ? std::string(row[i], len[i]) : std::string());
            out->push_back(v);
        }
        mysql_free_result(r);
        return true;
    }

    std::string sql_text(std::string const& v)
    {
        std::string r = "'";
        for (char c : v) { if (c == '\'') r += '\''; r += c; }
        return r + "'";
    }

    // the shim's own error text on the connection (mysql_error reads it back)
    void fail(MYSQL* m, std::string const& text);

    int apply_text(MYSQL* m, std::string const& name, std::string const& text, int (*skip)(const char*, void*), void* context)
    {
        if (!query(m, "BEGIN"))
            return 1;
        for (std::string const& statement : shim::split_statements(text))
        {
            std::string w = first_word(statement);
            if (w == "begin" || w == "commit" || w == "rollback" || (w == "start" && statement.size() > 5))
                continue;   // the file is applied in one transaction of its own
            if (skip && skip(statement.c_str(), context))
                continue;
            if (!query(m, statement))
            {
                std::string error = std::string(mysql_error(m)) + " (" + name + ": " + statement.substr(0, 200) + (statement.size() > 200 ? "..." : "") + ")";
                mysql_query(m, "ROLLBACK");
                fail(m, error);
                return 1;
            }
        }
        if (!query(m, "COMMIT"))
            return 1;
        return 0;
    }
}

// the error setter needs the opaque connection type of mysql_sqlite.cpp
extern "C" void mysql_sqlite_set_error(MYSQL* mysql, unsigned number, const char* text);

namespace
{
    void fail(MYSQL* m, std::string const& text)
    {
        mysql_sqlite_set_error(m, 1105 /* ER_UNKNOWN_ERROR */, text.c_str());
    }
}

extern "C" int mysql_sqlite_apply_file(MYSQL* mysql, const char* path, int (*skip)(const char*, void*), void* context)
{
    std::string text;
    if (!read_file(path, &text))
    {
        fail(mysql, std::string("cannot read ") + path);
        return 1;
    }
    return apply_text(mysql, fs::path(path).filename().string(), text, skip, context);
}

extern "C" int mysql_sqlite_apply_updates(MYSQL* mysql, const char* source)
{
    enum State { RELEASED, ARCHIVED, CUSTOM, PENDING, MODULE };
    static char const* const state_names[] = {"RELEASED", "ARCHIVED", "CUSTOM", "PENDING", "MODULE"};
    auto state_of = [](std::string const& s) {
        for (int i = 0; i < 5; ++i)
            if (s == state_names[i]) return State(i);
        return RELEASED;
    };

    std::vector<std::vector<std::string>> include, applied_rows;
    if (!rows(mysql, "SELECT `path`, `state` FROM `updates_include`", &include) || !rows(mysql, "SELECT `name`, `hash`, `state` FROM `updates`", &applied_rows))
        return -1;

    // the available files by name (AzerothCore orders them by file name and refuses duplicates)
    std::map<std::string, std::pair<fs::path, State>> available;
    for (auto const& inc : include)
    {
        std::string dir = inc[0];
        if (!dir.empty() && dir[0] == '$')
            dir = std::string(source) + dir.substr(1);
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            continue;   // AzerothCore warns and skips too (the page has only the directories it ships)
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
        {
            if (it.depth() >= 10 || !it->is_regular_file() || it->path().extension() != ".sql")
                continue;
            std::string name = it->path().filename().string();
            if (available.count(name))
            {
                fail(mysql, "duplicate update file name " + name + " (" + it->path().string() + ")");
                return -1;
            }
            available[name] = {it->path(), state_of(inc[1])};
        }
    }

    struct Applied { std::string hash; State state; };
    std::map<std::string, Applied> applied;
    std::map<std::string, std::string> hash_to_name;
    for (auto const& r : applied_rows)
    {
        applied[r[0]] = Applied{r[1], state_of(r[2])};
        hash_to_name[r[1]] = r[0];
    }

    int count = 0;
    auto record = [&](std::string const& name, std::string const& hash, State state, long speed) {
        return query(mysql, "REPLACE INTO `updates` (`name`, `hash`, `state`, `speed`) VALUES (" + sql_text(name) + ", " + sql_text(hash) + ", '" + state_names[state] + "', " + std::to_string(speed) + ")");
    };
    auto handle = [&](std::string const& name, fs::path const& path, State state) -> bool {
        auto it = applied.find(name);
        if (it != applied.end() && it->second.state == ARCHIVED && state == ARCHIVED)
            return true;   // archived updates never change (ArchivedRedundancy 0)
        std::string text;
        if (!read_file(path.string(), &text))
        {
            fail(mysql, "cannot read " + path.string());
            return false;
        }
        std::string hash = shim::sha1_hex_upper(text);
        if (it == applied.end())
        {
            auto renamed = hash_to_name.find(hash);
            if (renamed != hash_to_name.end() && !available.count(renamed->second))
            {
                // the same file under a new name: the entry is renamed, nothing is applied
                fprintf(stderr, "mysql-sqlite: update %s was renamed to %s\n", renamed->second.c_str(), name.c_str());
                return query(mysql, "UPDATE `updates` SET `name` = " + sql_text(name) + " WHERE `name` = " + sql_text(renamed->second));
            }
        }
        else if (it->second.hash.empty())
            return record(name, hash, state, 0);   // AllowRehash: the hash is filled in, the file is not applied again
        else if (it->second.hash == hash)
        {
            if (it->second.state != state)
                return query(mysql, "UPDATE `updates` SET `state` = '" + std::string(state_names[state]) + "' WHERE `name` = " + sql_text(name));
            return true;
        }
        auto begin = std::chrono::steady_clock::now();
        if (apply_text(mysql, name, text, nullptr, nullptr) != 0)
            return false;
        long speed = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count());
        ++count;
        return record(name, hash, state, speed);
    };

    for (int pass = 0; pass < 2; ++pass)
        for (auto const& [name, file] : available)
        {
            bool late = file.second == PENDING || file.second == CUSTOM || file.second == MODULE;
            if (late == (pass == 1) && !handle(name, file.first, file.second))
                return -1;
        }
    return count;
}
