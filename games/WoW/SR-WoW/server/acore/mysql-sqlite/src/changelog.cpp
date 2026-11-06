/* The change log of a streamed database (the world): what the server changes in it (GM commands that add NPCs, objects, ...) is kept as SQLite
   changesets (the session extension: rows by primary key with their old and new values), appended to a file after every commit, and replayed on
   top of the base at the next start. Changesets do not depend on the file's pages, so a new base (a newer world database) takes them as well.
   Files in the log directory: changes.N, the generation N:  "ACCL1\n", then records  u32 length, u32 CRC-32, changeset  (little endian).
   A record that was cut off (the page closed while it was saved) fails its length or CRC and ends the replay. At the first open the newest
   generation is replayed and merged into one changeset (sqlite3changegroup: a row changed many times becomes one change) that is written as
   generation N+1 (changes.N+1.tmp, renamed: the file system never holds a half-written generation); older generations are deleted after that.
   Replaying twice gives the same result (an insert of a row that exists replaces it, a change of a missing row is dropped). */

#include "shim_internal.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
    char const MAGIC[] = "ACCL1\n";
    constexpr size_t MAGIC_LENGTH = 6;

    uint32_t crc32(uint8_t const* p, size_t n)
    {
        uint32_t c = 0xffffffffu;
        for (size_t i = 0; i < n; ++i)
        {
            c ^= p[i];
            for (int k = 0; k < 8; ++k)
                c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        }
        return ~c;
    }

    void put32(std::string& out, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out += char((v >> (8 * i)) & 0xff);
    }

    uint32_t get32(uint8_t const* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

    std::string record(void const* data, int n)
    {
        std::string r;
        put32(r, uint32_t(n));
        put32(r, crc32(static_cast<uint8_t const*>(data), size_t(n)));
        r.append(static_cast<char const*>(data), size_t(n));
        return r;
    }

    struct Log
    {
        std::string dir;
        std::mutex lock;
        int generation = 0;
        std::string file() const { return dir + "/changes." + std::to_string(generation); }
    };

    std::atomic<shim::ChangeListener> g_listener{nullptr};

    std::mutex g_logs_lock;
    std::map<std::string, Log*> g_logs;   // by database name

    // What the replay of a log did not apply (a change that does not fit the database is skipped, never fatal, and said so in
    // the server's log): the changes that SQLite drops without a word (a table the database lacks, fewer columns, another key) are found beforehand by prescan(),
    // the others (a row that is not there, a constraint, a foreign key) come from on_conflict. A bounded number of lines name the table and the key, then one total
    // per table. The records stay in the log either way: they apply again on a database where they fit.
    struct Replay
    {
        std::string name;
        sqlite3* db = nullptr;
        size_t lines = 0;
        size_t total = 0;
        std::map<std::string, std::map<std::string, size_t>> skipped;   // table -> reason -> number of changes
        std::map<std::string, int> tables;                              // "table/columns" -> 0 fits, else the reason (prescan)
        std::map<std::string, std::string> why;                         // "table/columns" -> text of the reason
    };

    constexpr size_t MAX_SKIP_LINES = 20;

    // the key of the row a change names, as "a,b" (the new row for an insert, the old one otherwise)
    std::string key_of(sqlite3_changeset_iter* it, int op, int columns)
    {
        unsigned char* pk = nullptr;
        int pkColumns = 0;
        std::string out;
        sqlite3changeset_pk(it, &pk, &pkColumns);
        for (int c = 0; pk && c < columns && c < pkColumns; ++c)
        {
            if (!pk[c])
                continue;
            sqlite3_value* v = nullptr;
            if (op == SQLITE_INSERT)
                sqlite3changeset_new(it, c, &v);
            else
                sqlite3changeset_old(it, c, &v);
            if (!out.empty())
                out += ',';
            if (v)
                out += reinterpret_cast<char const*>(sqlite3_value_text(v));
        }
        return out;
    }

    void note_skip(Replay* r, std::string const& table, std::string const& reason, std::string const& key)
    {
        ++r->total;
        ++r->skipped[table][reason];
        if (r->lines >= MAX_SKIP_LINES)
            return;
        ++r->lines;
        fprintf(stderr, "mysql-sqlite: %s: change skipped (%s): table %s%s%s\n", r->name.c_str(), reason.c_str(), table.c_str(), key.empty() ? "" : ", key ", key.c_str());
    }

    int on_conflict(void* context, int conflict, sqlite3_changeset_iter* it)
    {
        Replay* r = static_cast<Replay*>(context);
        switch (conflict)
        {
            case SQLITE_CHANGESET_DATA:       // the row is there with other values: ours win
            case SQLITE_CHANGESET_CONFLICT:   // an insert of a row that exists: ours win
                return SQLITE_CHANGESET_REPLACE;
            default:                          // NOTFOUND (the row is gone from the base), CONSTRAINT, FOREIGN_KEY: dropped
                if (r)
                {
                    char const* table = nullptr;
                    int columns = 0, op = 0, indirect = 0;
                    sqlite3changeset_op(it, &table, &columns, &op, &indirect);
                    if (conflict == SQLITE_CHANGESET_NOTFOUND)
                        note_skip(r, table, "the row is not in the database", key_of(it, op, columns));
                    else if (conflict == SQLITE_CHANGESET_CONSTRAINT)
                        note_skip(r, table, "a constraint of the table refuses it", key_of(it, op, columns));
                    else
                        note_skip(r, "(foreign keys)", "a foreign key refuses a change", "");
                }
                return SQLITE_CHANGESET_OMIT;
        }
    }

    // Looks at the tables a record changes before it is applied: SQLite drops the changes of a table that the database lacks, that has fewer columns than the change
    // or another primary key without any message. Each such change is noted (note_skip).
    void prescan(Replay* r, int n, void const* data)
    {
        sqlite3_changeset_iter* it = nullptr;
        if (n <= 0 || sqlite3changeset_start(&it, n, const_cast<void*>(data)) != SQLITE_OK)
            return;
        while (sqlite3changeset_next(it) == SQLITE_ROW)
        {
            char const* table = nullptr;
            int columns = 0, op = 0, indirect = 0, pkColumns = 0;
            unsigned char* pk = nullptr;
            sqlite3changeset_op(it, &table, &columns, &op, &indirect);
            sqlite3changeset_pk(it, &pk, &pkColumns);
            std::string id = std::string(table) + "/" + std::to_string(columns);
            if (!r->tables.count(id))
            {
                int state = 0;
                std::string text;
                std::vector<int> keys;
                sqlite3_stmt* st = nullptr;
                std::string sql = "PRAGMA table_info(\"" + std::string(table) + "\")";
                if (sqlite3_prepare_v2(r->db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK)
                {
                    while (sqlite3_step(st) == SQLITE_ROW)
                        keys.push_back(sqlite3_column_int(st, 5) > 0 ? 1 : 0);
                    sqlite3_finalize(st);
                }
                if (keys.empty())
                {
                    state = 1;
                    text = "the database has no such table";
                }
                else if (int(keys.size()) < columns)
                {
                    state = 2;
                    text = "the table has " + std::to_string(keys.size()) + " columns, the change needs " + std::to_string(columns);
                }
                else
                {
                    for (int c = 0; c < columns; ++c)
                        if ((pk && c < pkColumns ? (pk[c] ? 1 : 0) : 0) != keys[c])
                            state = 3;
                    if (state)
                        text = "the key columns of the table are not those of the change";
                }
                r->tables[id] = state;
                r->why[id] = text;
            }
            if (r->tables[id])
                note_skip(r, table, r->why[id], key_of(it, op, columns));
        }
        sqlite3changeset_finalize(it);
    }

    void report_skips(Replay const& r)
    {
        if (!r.total)
            return;
        for (auto const& t : r.skipped)
        {
            std::string detail;
            size_t sum = 0;
            for (auto const& why : t.second)
            {
                detail += (detail.empty() ? "" : "; ") + std::to_string(why.second) + " x " + why.first;
                sum += why.second;
            }
            fprintf(stderr, "mysql-sqlite: %s: %zu changes skipped in table %s (%s)\n", r.name.c_str(), sum, t.first.c_str(), detail.c_str());
        }
        fprintf(stderr, "mysql-sqlite: %s: %zu changes of the log skipped in all%s (they stay in the log)\n", r.name.c_str(), r.total,
                r.total > r.lines ? ", the first lines named above" : "");
    }

    // the rows a changeset touches: one line "OP<TAB>table<TAB>pk,pk,..." each (OP I insert, U update, D delete), the key columns in the table's order
    void describe_changeset(int n, void const* data, std::string* out)
    {
        sqlite3_changeset_iter* it = nullptr;
        if (n <= 0 || sqlite3changeset_start(&it, n, const_cast<void*>(data)) != SQLITE_OK)
            return;
        while (sqlite3changeset_next(it) == SQLITE_ROW)
        {
            char const* table = nullptr;
            int columns = 0, op = 0, indirect = 0;
            unsigned char* pk = nullptr;
            int pkColumns = 0;
            sqlite3changeset_op(it, &table, &columns, &op, &indirect);
            sqlite3changeset_pk(it, &pk, &pkColumns);
            *out += op == SQLITE_INSERT ? 'I' : op == SQLITE_DELETE ? 'D' : 'U';
            *out += '\t';
            *out += table;
            *out += '\t';
            bool first = true;
            for (int c = 0; c < columns && c < pkColumns; ++c)
            {
                if (!pk[c])
                    continue;
                sqlite3_value* v = nullptr;
                if (op == SQLITE_INSERT)
                    sqlite3changeset_new(it, c, &v);
                else
                    sqlite3changeset_old(it, c, &v);
                if (!first)
                    *out += ',';
                first = false;
                if (v)
                    *out += reinterpret_cast<char const*>(sqlite3_value_text(v));
            }
            *out += '\n';
        }
        sqlite3changeset_finalize(it);
    }

    std::string hex(unsigned char const* p, int n)
    {
        static char const digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(size_t(n) * 2);
        for (int i = 0; i < n; ++i)
        {
            out += digits[p[i] >> 4];
            out += digits[p[i] & 15];
        }
        return out;
    }

    // a value in the text form of the rows of a commit: N null, I<integer>, F<real>, T<hex of the text>, B<hex of the bytes>
    void put_value(std::string* out, sqlite3_value* v)
    {
        switch (sqlite3_value_type(v))
        {
            case SQLITE_INTEGER:  *out += 'I' + std::to_string(sqlite3_value_int64(v)); break;
            case SQLITE_FLOAT:    { char b[40]; snprintf(b, sizeof b, "F%.17g", sqlite3_value_double(v)); *out += b; break; }
            case SQLITE_TEXT:     *out += 'T' + hex(sqlite3_value_text(v), sqlite3_value_bytes(v)); break;
            case SQLITE_BLOB:     *out += 'B' + hex(static_cast<unsigned char const*>(sqlite3_value_blob(v)), sqlite3_value_bytes(v)); break;
            default:              *out += 'N'; break;
        }
    }

    // The rows of a committed change as full images, for the listener of the shared world changes: one line "P<TAB>table<TAB>column=value<TAB>..." (a key column is written "column*=value") for a row that exists
    // after the commit (inserted or changed: every column), one line "D<TAB>table<TAB>column=value..." for a row that is gone (its key columns). Values: see put_value.
    // A row touched twice in one commit is listed once.
    std::string rows_of_changeset(sqlite3* db, int n, void* data)
    {
        std::string out;
        sqlite3_changeset_iter* it = nullptr;
        if (n <= 0 || sqlite3changeset_start(&it, n, data) != SQLITE_OK)
            return out;
        std::set<std::string> seen;
        while (sqlite3changeset_next(it) == SQLITE_ROW)
        {
            char const* table = nullptr;
            int columns = 0, op = 0, indirect = 0, pkColumns = 0;
            unsigned char* pk = nullptr;
            sqlite3changeset_op(it, &table, &columns, &op, &indirect);
            sqlite3changeset_pk(it, &pk, &pkColumns);
            if (!pk || pkColumns <= 0)
                continue;
            // the key values: the new row for an insert, the old row otherwise
            std::string where, keyId;
            std::vector<std::pair<int, sqlite3_value*>> keys;
            for (int c = 0; c < columns && c < pkColumns; ++c)
                if (pk[c])
                {
                    sqlite3_value* v = nullptr;
                    if (op == SQLITE_INSERT)
                        sqlite3changeset_new(it, c, &v);
                    else
                        sqlite3changeset_old(it, c, &v);
                    if (!v)
                        goto next;
                    keys.emplace_back(c, v);
                    put_value(&keyId, v);
                    keyId += ',';
                }
            if (!seen.insert(std::string(table) + "\t" + keyId).second)
                continue;
            {
                // the row as it is now
                std::string sql = std::string("SELECT * FROM \"") + table + "\"";
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
                    continue;
                int cols = sqlite3_column_count(st);
                std::vector<std::string> names;
                for (int c = 0; c < cols; ++c)
                    names.push_back(sqlite3_column_name(st, c));
                sqlite3_finalize(st);
                where = " WHERE ";
                bool firstKey = true;
                for (auto const& k : keys)
                {
                    if (k.first >= cols)
                        continue;
                    where += (firstKey ? "" : " AND ") + std::string("\"") + names[k.first] + "\" = ?";
                    firstKey = false;
                }
                sql += where;
                if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
                    continue;
                int bind = 1;
                for (auto const& k : keys)
                    if (k.first < cols)
                        sqlite3_bind_value(st, bind++, k.second);
                bool exists = sqlite3_step(st) == SQLITE_ROW;
                std::string line = exists ? "P\t" : "D\t";
                line += table;
                for (int c = 0; c < cols; ++c)
                {
                    bool isKey = false;
                    for (auto const& k : keys)
                        isKey = isKey || k.first == c;
                    if (!exists && !isKey)
                        continue;
                    line += '\t' + names[c] + (isKey ? "*=" : "=");
                    if (exists)
                        put_value(&line, sqlite3_column_value(st, c));
                    else
                        for (auto const& k : keys)
                            if (k.first == c)
                                put_value(&line, k.second);
                }
                sqlite3_finalize(st);
                out += line + '\n';
            }
        next:;
        }
        sqlite3changeset_finalize(it);
        return out;
    }

    // the records of a generation file, up to the first damaged one
    std::vector<std::string> read_records(std::string const& path, bool* damaged)
    {
        std::vector<std::string> records;
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string bytes = ss.str();
        *damaged = false;
        if (bytes.size() < MAGIC_LENGTH || bytes.compare(0, MAGIC_LENGTH, MAGIC) != 0)
        {
            *damaged = !bytes.empty();
            return records;
        }
        size_t at = MAGIC_LENGTH;
        while (at < bytes.size())
        {
            uint8_t const* p = reinterpret_cast<uint8_t const*>(bytes.data()) + at;
            if (bytes.size() - at < 8) { *damaged = true; break; }
            uint32_t n = get32(p), crc = get32(p + 4);
            if (bytes.size() - at - 8 < n || crc32(p + 8, n) != crc) { *damaged = true; break; }
            records.emplace_back(bytes.data() + at + 8, n);
            at += 8 + n;
        }
        return records;
    }
}

namespace shim
{
    // `touched` (optional): the rows the replayed changes touch (describe_changeset)
    static bool open_log(std::string const& name, std::string const& dir, sqlite3* db, std::string* error, std::string* touched)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        Log* log = new Log();
        log->dir = dir;

        // the newest generation (leftovers of earlier ones and unfinished .tmp files are removed below)
        int newest = 0;
        std::vector<fs::path> others;
        std::vector<std::pair<int, fs::path>> generations;
        for (auto const& e : fs::directory_iterator(dir, ec))
        {
            std::string f = e.path().filename().string();
            if (f.rfind("changes.", 0) != 0)
                continue;
            std::string number = f.substr(8);
            if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
                others.push_back(e.path());   // changes.N.tmp: a merge that did not finish
            else
                generations.emplace_back(atoi(number.c_str()), e.path());
        }
        for (auto const& g : generations)
            newest = std::max(newest, g.first);
        for (auto const& g : generations)
            if (g.first != newest)
                others.push_back(g.second);

        std::vector<std::string> records;
        if (newest)
        {
            bool damaged = false;
            log->generation = newest;
            records = read_records(log->file(), &damaged);
            if (damaged)
                fprintf(stderr, "mysql-sqlite: %s: the change log ends with a damaged record (%zu complete ones are used)\n", name.c_str(), records.size());
        }

        // replay, then merge into one changeset for the next generation
        sqlite3_changegroup* group = nullptr;
        sqlite3changegroup_new(&group);
        size_t applied = 0;
        Replay replay;
        replay.name = name;
        replay.db = db;
        for (std::string& r : records)
        {
            prescan(&replay, int(r.size()), r.data());
            int rc = sqlite3changeset_apply(db, int(r.size()), r.data(), nullptr, on_conflict, &replay);
            if (rc != SQLITE_OK)
                fprintf(stderr, "mysql-sqlite: %s: a change of the log could not be applied: %s\n", name.c_str(), sqlite3_errmsg(db));
            else
                ++applied;
            sqlite3changegroup_add(group, int(r.size()), r.data());
        }
        report_skips(replay);
        int n = 0;
        void* merged = nullptr;
        sqlite3changegroup_output(group, &n, &merged);
        sqlite3changegroup_delete(group);
        if (touched)
            describe_changeset(n, merged, touched);

        log->generation = newest + 1;
        std::string contents(MAGIC, MAGIC_LENGTH);
        if (n > 0)
            contents += record(merged, n);
        sqlite3_free(merged);
        std::string temp = log->file() + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(contents.data(), std::streamsize(contents.size()));
            if (!out)
            {
                *error = "cannot write " + temp;
                delete log;
                return false;
            }
        }
        if (rename(temp.c_str(), log->file().c_str()) != 0)
        {
            *error = "cannot rename " + temp;
            delete log;
            return false;
        }
        if (newest)
            remove((fs::path(dir) / ("changes." + std::to_string(newest))).c_str());
        for (fs::path const& p : others)
            fs::remove(p, ec);
        if (!records.empty())
            fprintf(stderr, "mysql-sqlite: %s: %zu changes of the log applied (%d bytes after merging)\n", name.c_str(), applied, n);
        g_logs[name] = log;
        return true;
    }

    bool changelog_open(std::string const& name, std::string const& dir, sqlite3* db, std::string* error)
    {
        std::lock_guard<std::mutex> guard(g_logs_lock);
        if (g_logs.count(name))
            return true;
        return open_log(name, dir, db, error, nullptr);
    }

    // The server changes realm: the changes of the realm that is left are taken out of the database (the inverse of its log is applied), the log of the realm that is
    // entered is replayed, and new changes go to that one. `session` is the session of the connection `db`, which must not record these. `touched` gets the rows both
    // logs touch. The world is idle (no player, no update of the world is running).
    bool changelog_switch(std::string const& name, std::string const& dir, sqlite3* db, sqlite3_session* session, std::string* touched, std::string* error)
    {
        std::lock_guard<std::mutex> guard(g_logs_lock);
        auto it = g_logs.find(name);
        if (it != g_logs.end() && it->second->dir == dir)
            return true;
        if (session)
            sqlite3session_enable(session, 0);
        bool ok = true;
        if (it != g_logs.end())
        {
            Log* old = it->second;
            std::lock_guard<std::mutex> logGuard(old->lock);
            bool damaged = false;
            std::vector<std::string> records = read_records(old->file(), &damaged);
            sqlite3_changegroup* group = nullptr;
            sqlite3changegroup_new(&group);
            for (std::string& r : records)
                sqlite3changegroup_add(group, int(r.size()), r.data());
            int n = 0, inverted = 0;
            void* merged = nullptr;
            void* inverse = nullptr;
            sqlite3changegroup_output(group, &n, &merged);
            sqlite3changegroup_delete(group);
            if (n > 0)
            {
                describe_changeset(n, merged, touched);
                if (sqlite3changeset_invert(n, merged, &inverted, &inverse) == SQLITE_OK)
                    sqlite3changeset_apply(db, inverted, inverse, nullptr, on_conflict, nullptr);
                sqlite3_free(inverse);
            }
            sqlite3_free(merged);
            fprintf(stderr, "mysql-sqlite: %s: the changes of %s taken out (%zu records)\n", name.c_str(), old->dir.c_str(), records.size());
            g_logs.erase(it);
            delete old;
        }
        ok = open_log(name, dir, db, error, touched);
        if (session)
            sqlite3session_enable(session, 1);
        return ok;
    }

    void changelog_listen(ChangeListener listener) { g_listener.store(listener); }

    sqlite3_session* changelog_attach(sqlite3* db)
    {
        sqlite3_session* s = nullptr;
        if (sqlite3session_create(db, "main", &s) != SQLITE_OK)
            return nullptr;
        int on = 1;
        sqlite3session_object_config(s, SQLITE_SESSION_OBJCONFIG_ROWID, &on);   // tables without a primary key are recorded by rowid
        sqlite3session_attach(s, nullptr);
        return s;
    }

    // after a statement: a committed change of this connection goes into the log
    void changelog_after(std::string const& name, sqlite3* db, sqlite3_session*& s)
    {
        if (!s || !sqlite3_get_autocommit(db) || sqlite3session_isempty(s))
            return;
        int n = 0;
        void* data = nullptr;
        if (sqlite3session_changeset(s, &n, &data) == SQLITE_OK && n > 0)
        {
            Log* log = nullptr;
            {
                std::lock_guard<std::mutex> guard(g_logs_lock);
                auto it = g_logs.find(name);
                log = it == g_logs.end() ? nullptr : it->second;
            }
            if (log)
            {
                std::lock_guard<std::mutex> guard(log->lock);
                std::string r = record(data, n);
                if (FILE* f = fopen(log->file().c_str(), "ab"))
                {
                    fwrite(r.data(), 1, r.size(), f);
                    fclose(f);
                }
                else
                    fprintf(stderr, "mysql-sqlite: cannot append to %s\n", log->file().c_str());
            }
        }
        if (n > 0)
            if (shim::ChangeListener listener = g_listener.load())
            {
                std::string rows = rows_of_changeset(db, n, data);
                if (!rows.empty())
                    listener(name.c_str(), rows.c_str());
            }
        sqlite3_free(data);
        // a new session for the next changes (a session accumulates everything since it was created)
        sqlite3session_delete(s);
        s = changelog_attach(db);
    }
}
