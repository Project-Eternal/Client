/* The MySQL C client API (the subset AzerothCore uses, see include/mysql.h) on top of SQLite.

   - A "database" is a file: the connection string's host is the directory, the database name the file (<dir>/<name>.sqlite).
     Per-database settings (read-only, update directories, a kept working copy, streaming and a change log): src/config.cpp.
   - MySQL statements SQLite has no form for: table definitions (src/ddl.cpp), user variables, SET/LOCK (ignored); SQL files and AzerothCore's
     update bookkeeping: src/updater.cpp (include/mysql_sqlite.h).
   - Text queries (mysql_query) give rows as strings, as the MySQL text protocol does. Prepared statements give typed values that
     mysql_stmt_fetch writes into the caller's buffers with the width of the column type (declared type of the SQLite column;
     computed columns are typed by their first value, COUNT is BIGINT, SUM and AVG are DECIMAL), as the binary protocol does.
   - The SQL is translated from the MySQL dialect on the way (translate_sql).
   - Not a MySQL server: no users, no network, one process. */

#include "mysql.h"
#include "mysql_sqlite.h"
#include "shim_internal.h"
#include "errmsg.h"
#include "mysqld_error.h"

#include <sqlite3.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // ---------------------------------------------------------------------------------------------------------------------
    // values

    enum CellKind : uint32_t { K_NULL = 0, K_INT = 1, K_REAL = 2, K_TEXT = 3, K_BLOB = 4 };

    struct Cell
    {
        uint32_t kind_len;   // kind in the top 3 bits, length in the rest
        union { int64_t i; double d; uint64_t off; } u;

        CellKind kind() const { return CellKind(kind_len >> 29); }
        uint32_t len() const { return kind_len & 0x1fffffffu; }
    };

    struct Rows
    {
        std::vector<Cell> cells;
        std::string arena;     // text and blob bytes
        uint32_t ncols = 0;
        uint64_t nrows = 0;

        void clear() { cells.clear(); cells.shrink_to_fit(); arena.clear(); arena.shrink_to_fit(); nrows = 0; }

        void append_row(sqlite3_stmt* st)
        {
            for (uint32_t c = 0; c < ncols; ++c)
            {
                Cell cell;
                switch (sqlite3_column_type(st, int(c)))
                {
                    case SQLITE_INTEGER: cell.kind_len = K_INT << 29; cell.u.i = sqlite3_column_int64(st, int(c)); break;
                    case SQLITE_FLOAT: cell.kind_len = K_REAL << 29; cell.u.d = sqlite3_column_double(st, int(c)); break;
                    case SQLITE_TEXT:
                    {
                        uint32_t n = uint32_t(sqlite3_column_bytes(st, int(c)));
                        cell.kind_len = (K_TEXT << 29) | n;
                        cell.u.off = arena.size();
                        arena.append(reinterpret_cast<char const*>(sqlite3_column_text(st, int(c))), n);
                        break;
                    }
                    case SQLITE_BLOB:
                    {
                        uint32_t n = uint32_t(sqlite3_column_bytes(st, int(c)));
                        cell.kind_len = (K_BLOB << 29) | n;
                        cell.u.off = arena.size();
                        if (n)
                            arena.append(static_cast<char const*>(sqlite3_column_blob(st, int(c))), n);
                        break;
                    }
                    default: cell.kind_len = K_NULL << 29; cell.u.i = 0; break;
                }
                cells.push_back(cell);
            }
            ++nrows;
        }

        std::string text(Cell const& c) const
        {
            switch (c.kind())
            {
                case K_INT: return std::to_string(c.u.i);
                case K_REAL:
                {
                    char buf[64];
                    auto r = std::to_chars(buf, buf + sizeof(buf), c.u.d);
                    return std::string(buf, r.ptr);
                }
                case K_TEXT:
                case K_BLOB: return arena.substr(size_t(c.u.off), c.len());
                default: return {};
            }
        }

        int64_t as_int(Cell const& c) const
        {
            switch (c.kind())
            {
                case K_INT: return c.u.i;
                case K_REAL: return int64_t(c.u.d);
                case K_TEXT: return strtoll(arena.substr(size_t(c.u.off), c.len()).c_str(), nullptr, 10);
                default: return 0;
            }
        }

        double as_real(Cell const& c) const
        {
            switch (c.kind())
            {
                case K_INT: return double(c.u.i);
                case K_REAL: return c.u.d;
                case K_TEXT: return strtod(arena.substr(size_t(c.u.off), c.len()).c_str(), nullptr);
                default: return 0;
            }
        }
    };

    // ---------------------------------------------------------------------------------------------------------------------
    // column metadata

    struct Meta
    {
        std::vector<MYSQL_FIELD> fields;
        std::deque<std::string> names;   // storage of the names the fields point to
        std::vector<bool> typed;         // the type came from a declared column type

        char* keep(char const* s)
        {
            names.emplace_back(s ? s : "");
            return names.back().data();
        }
    };

    std::string lower(char const* s)
    {
        std::string r = s ? s : "";
        for (char& c : r)
            c = char(tolower(static_cast<unsigned char>(c)));
        return r;
    }

    // MySQL type of a declared SQLite column type (the converter keeps the MySQL names: "tinyint unsigned", "varchar(32)", ...)
    bool type_from_decl(char const* decl, MYSQL_FIELD* f)
    {
        if (!decl || !*decl)
            return false;

        std::string d = lower(decl);
        auto has = [&](char const* s) { return d.find(s) != std::string::npos; };
        bool is_unsigned = has("unsigned");
        f->flags = is_unsigned ? UNSIGNED_FLAG : 0;

        if (has("tinyint") || has("boolean") || d == "bool") f->type = MYSQL_TYPE_TINY;
        else if (has("smallint")) f->type = MYSQL_TYPE_SHORT;
        else if (has("mediumint")) f->type = MYSQL_TYPE_INT24;
        else if (has("bigint")) f->type = MYSQL_TYPE_LONGLONG;
        else if (d == "integer") f->type = MYSQL_TYPE_LONGLONG;   // a rowid alias (AUTOINCREMENT): its MySQL width is not kept
        else if (has("int")) f->type = MYSQL_TYPE_LONG;
        else if (has("float")) f->type = MYSQL_TYPE_FLOAT;
        else if (has("double") || has("real")) f->type = MYSQL_TYPE_DOUBLE;
        else if (has("decimal") || has("numeric")) f->type = MYSQL_TYPE_NEWDECIMAL;
        else if (has("bit")) f->type = MYSQL_TYPE_LONGLONG;
        else if (has("datetime")) f->type = MYSQL_TYPE_DATETIME;
        else if (has("timestamp")) f->type = MYSQL_TYPE_TIMESTAMP;
        else if (has("date")) f->type = MYSQL_TYPE_DATE;
        else if (has("time")) f->type = MYSQL_TYPE_TIME;   // after "datetime" and "timestamp"; "tinytext" etc. are caught below
        else if (has("year")) f->type = MYSQL_TYPE_YEAR;
        else if (has("blob") || has("text")) { f->type = MYSQL_TYPE_BLOB; f->flags |= BLOB_FLAG | (has("blob") ? BINARY_FLAG : 0); }
        else if (has("binary")) { f->type = MYSQL_TYPE_VAR_STRING; f->flags |= BINARY_FLAG; }
        else if (has("char") || has("enum") || has("set")) f->type = MYSQL_TYPE_VAR_STRING;
        else return false;

        return true;
    }

    // the type of a computed column (no declared type) from its name and its first value
    void type_from_value(Meta& m, size_t col, Rows const& rows)
    {
        MYSQL_FIELD& f = m.fields[col];
        std::string name = lower(f.name);
        if (name.rfind("count(", 0) == 0) { f.type = MYSQL_TYPE_LONGLONG; return; }
        if (name.rfind("sum(", 0) == 0 || name.rfind("avg(", 0) == 0) { f.type = MYSQL_TYPE_NEWDECIMAL; return; }

        f.type = MYSQL_TYPE_VAR_STRING;
        for (uint64_t r = 0; r < rows.nrows; ++r)
        {
            Cell const& c = rows.cells[r * rows.ncols + col];
            if (c.kind() == K_NULL)
                continue;
            switch (c.kind())
            {
                case K_INT: f.type = MYSQL_TYPE_LONGLONG; break;
                case K_REAL: f.type = MYSQL_TYPE_DOUBLE; break;
                case K_BLOB: f.type = MYSQL_TYPE_BLOB; f.flags |= BLOB_FLAG | BINARY_FLAG; break;
                default: break;
            }
            return;
        }
    }

    void build_meta(Meta& m, sqlite3_stmt* st)
    {
        int n = sqlite3_column_count(st);
        m.fields.assign(size_t(n), MYSQL_FIELD());
        m.typed.assign(size_t(n), false);
        for (int i = 0; i < n; ++i)
        {
            MYSQL_FIELD& f = m.fields[size_t(i)];
            memset(&f, 0, sizeof(f));
            f.name = m.keep(sqlite3_column_name(st, i));
            f.org_name = m.keep(sqlite3_column_origin_name(st, i));
            f.table = m.keep(sqlite3_column_table_name(st, i));
            f.org_table = m.keep(sqlite3_column_table_name(st, i));
            f.db = m.keep(sqlite3_column_database_name(st, i));
            f.catalog = m.keep("def");
            f.name_length = unsigned(strlen(f.name));
            f.org_name_length = unsigned(strlen(f.org_name));
            f.table_length = f.org_table_length = unsigned(strlen(f.table));
            f.db_length = unsigned(strlen(f.db));
            f.type = MYSQL_TYPE_VAR_STRING;
            m.typed[size_t(i)] = type_from_decl(sqlite3_column_decltype(st, i), &f);
        }
    }

    // the types of the computed columns and the longest value of every column, once all rows are known
    void finish_meta(Meta& m, Rows const& rows)
    {
        for (size_t c = 0; c < m.fields.size(); ++c)
        {
            if (!m.typed[c])
                type_from_value(m, c, rows);

            unsigned long longest = 0;
            enum_field_types t = m.fields[c].type;
            bool bytes = t == MYSQL_TYPE_VAR_STRING || t == MYSQL_TYPE_STRING || t == MYSQL_TYPE_BLOB || t == MYSQL_TYPE_TINY_BLOB
                || t == MYSQL_TYPE_MEDIUM_BLOB || t == MYSQL_TYPE_LONG_BLOB || t == MYSQL_TYPE_VARCHAR;
            for (uint64_t r = 0; bytes && r < rows.nrows; ++r)   // only strings need their longest value (numbers have a fixed size)
            {
                Cell const& cell = rows.cells[r * rows.ncols + c];
                unsigned long n = cell.kind() == K_TEXT || cell.kind() == K_BLOB ? cell.len() : cell.kind() == K_NULL ? 0 : unsigned(rows.text(cell).size());
                longest = std::max(longest, n);
            }
            m.fields[c].max_length = longest;
            m.fields[c].length = std::max<unsigned long>(m.fields[c].length, longest);
        }
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // the SQL dialect

    bool is_word_char(char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; }

    // case insensitive match of a keyword phrase (words separated by one or more blanks in the text) at i; returns the end or npos
    size_t match_phrase(std::string const& s, size_t i, char const* phrase)
    {
        if (i > 0 && is_word_char(s[i - 1]))
            return std::string::npos;
        size_t p = i;
        for (char const* w = phrase; *w;)
        {
            if (*w == ' ')
            {
                if (p >= s.size() || !isspace(static_cast<unsigned char>(s[p])))
                    return std::string::npos;
                while (p < s.size() && isspace(static_cast<unsigned char>(s[p])))
                    ++p;
                ++w;
                continue;
            }
            if (p >= s.size() || tolower(static_cast<unsigned char>(s[p])) != tolower(static_cast<unsigned char>(*w)))
                return std::string::npos;
            ++p;
            ++w;
        }
        if (p < s.size() && is_word_char(s[p]))
            return std::string::npos;
        return p;
    }

    std::vector<std::string> primary_key_columns(sqlite3* db, std::string const& table)
    {
        std::vector<std::pair<int, std::string>> pk;
        sqlite3_stmt* st = nullptr;
        std::string sql = "PRAGMA table_info(`" + table + "`)";
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK)
        {
            while (sqlite3_step(st) == SQLITE_ROW)
                if (int order = sqlite3_column_int(st, 5))
                    pk.emplace_back(order, reinterpret_cast<char const*>(sqlite3_column_text(st, 1)));
            sqlite3_finalize(st);
        }
        std::sort(pk.begin(), pk.end());
        std::vector<std::string> names;
        for (auto const& p : pk)
            names.push_back(p.second);
        return names;
    }

    // MySQL dialect -> SQLite. Quoted text and comments are copied as they are.
    // the columns that identify a row of a table: its primary key, else rowid
    std::vector<std::string> row_key(sqlite3* db, std::string const& table)
    {
        std::vector<std::string> pk = primary_key_columns(db, table);
        if (pk.empty())
            pk.push_back("rowid");
        return pk;
    }

    std::string key_list(std::vector<std::string> const& pk, std::string const& prefix)
    {
        std::string r;
        for (size_t i = 0; i < pk.size(); ++i)
            r += (i ? "," : "") + prefix + (pk[i] == "rowid" ? "rowid" : "`" + pk[i] + "`");
        return r;
    }

    // only a few statement shapes need the regular expressions below (and std::regex recurses deeply on long texts: in wasm a thread's stack has no guard
    // page and an overflow corrupts the heap), so everything else is recognised by its first words
    bool needs_rewrite(std::string const& s)
    {
        size_t i = 0;
        auto word = [&](size_t from, size_t* end) {
            while (from < s.size() && isspace(static_cast<unsigned char>(s[from]))) ++from;
            size_t j = from;
            while (j < s.size() && (isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            if (end) *end = j;
            return lower(s.substr(from, j - from).c_str());
        };
        size_t e = 0;
        std::string first = word(i, &e);
        if (first == "desc" || first == "describe" || first == "show")
            return true;
        if (first == "delete")
            return word(e, nullptr) != "from";                  // DELETE alias FROM ...
        if (first == "update")
        {
            std::string low = lower(s.c_str());
            size_t set = low.find(" set ");
            return low.find(" join ") != std::string::npos || (set != std::string::npos && low.substr(0, set).find(',') != std::string::npos);   // UPDATE t1 a, t2 b SET / UPDATE t JOIN ...
        }
        return false;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // MySQL statements SQLite has no syntax for, recognised by their words (no std::regex: its matcher recurses deeply and, in wasm, corrupted the heap)

    struct Tok { std::string text; size_t begin, end; };

    std::vector<Tok> tokenize(std::string const& s)
    {
        std::vector<Tok> t;
        for (size_t i = 0; i < s.size();)
        {
            char c = s[i];
            if (isspace(static_cast<unsigned char>(c))) { ++i; continue; }
            if (c == ',' || c == ';') { t.push_back({std::string(1, c), i, i + 1}); ++i; continue; }
            size_t j = i;
            while (j < s.size() && !isspace(static_cast<unsigned char>(s[j])) && s[j] != ',' && s[j] != ';')
            {
                if (s[j] == '\'' || s[j] == '"' || s[j] == '`')
                {
                    char q = s[j++];
                    while (j < s.size() && s[j] != q) { if (s[j] == '\\' && q != '`') ++j; ++j; }
                }
                ++j;
            }
            j = std::min(j, s.size());
            t.push_back({s.substr(i, j - i), i, j});
            i = j;
        }
        return t;
    }

    std::string strip_ticks(std::string x)
    {
        x.erase(std::remove(x.begin(), x.end(), '`'), x.end());
        return x;
    }

    std::string low(std::string const& x) { return lower(x.c_str()); }

    bool is_join_word(std::string const& w)
    {
        return w == "join" || w == "left" || w == "inner" || w == "right" || w == "cross" || w == "natural";
    }

    // drops "alias." in front of column names
    std::string drop_alias(std::string const& text, std::string const& alias)
    {
        std::string out, a = low(alias) + ".";
        for (size_t i = 0; i < text.size();)
        {
            if ((i == 0 || !is_word_char(text[i - 1])) && low(text.substr(i, a.size())) == a)
            {
                i += a.size();
                continue;
            }
            out += text[i++];
        }
        return out;
    }

    std::string pk_condition(std::vector<std::string> const& pk)
    {
        std::string cols = key_list(pk, "");
        return pk.size() > 1 ? "(" + cols + ")" : cols;
    }

    // DESC/SHOW COLUMNS, SHOW TABLES, DELETE alias FROM t [alias] JOIN ..., UPDATE t1 a, t2 b SET ..., UPDATE t [a] JOIN ... SET ...
    std::string rewrite_multi_table(sqlite3* db, std::string const& s)
    {
        if (!needs_rewrite(s))
            return s;
        std::vector<Tok> t = tokenize(s);
        auto at = [&](size_t i) { return i < t.size() ? low(t[i].text) : std::string(); };
        std::string first = at(0);

        if (first == "desc" || first == "describe" || (first == "show" && (at(1) == "columns" || (at(1) == "full" && at(2) == "columns"))))
        {
            size_t i = first == "show" ? (at(1) == "full" ? 4 : 3) : 1;   // after "FROM" for SHOW COLUMNS
            if (i < t.size())
                return "SELECT name AS Field, type AS Type, CASE WHEN `notnull` THEN 'NO' ELSE 'YES' END AS `Null`, CASE WHEN pk > 0 THEN 'PRI' ELSE '' END AS `Key`, "
                       "dflt_value AS `Default`, '' AS Extra FROM pragma_table_info('" + strip_ticks(t[i].text) + "') ORDER BY cid";
        }
        if (first == "show" && at(1) == "tables")
        {
            std::string like = at(2) == "like" && t.size() > 3 ? t[3].text : std::string();   // quoted pattern as written
            return "SELECT name AS Tables_in_db FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'" + (like.empty() ? std::string() : " AND name LIKE " + like) + " ORDER BY name";
        }

        if (first == "delete" && at(1) != "from" && at(2) == "from")
        {
            std::string target = strip_ticks(t[1].text);
            if (target.size() > 2 && target.compare(target.size() - 2, 2, ".*") == 0)
                target.resize(target.size() - 2);
            std::string table = strip_ticks(t[3].text), alias = table;
            size_t i = 4;
            if (at(i) == "as") ++i;
            if (i < t.size() && !is_join_word(at(i)) && at(i) != "where" && at(i) != ";")
                alias = strip_ticks(t[i++].text);
            if (low(target) == low(alias) && i < t.size())
            {
                auto pk = row_key(db, table);
                std::string rest = s.substr(t[i].begin);
                return "DELETE FROM `" + table + "` WHERE " + pk_condition(pk) + " IN (SELECT " + key_list(pk, "`" + alias + "`.") + " FROM `" + table + "` `" + alias + "` " + rest + ")";
            }
            return s;
        }

        if (first == "update" && t.size() > 3)
        {
            std::string t1 = strip_ticks(t[1].text), a1 = t1;
            size_t i = 2;
            if (at(i) == "as") ++i;
            if (i < t.size() && at(i) != "," && !is_join_word(at(i)) && at(i) != "set")
                a1 = strip_ticks(t[i++].text);
            std::string from_clause;                                  // what follows "FROM t1 a1" inside the subquery
            size_t set_index = 0;
            if (at(i) == ",")
            {
                std::string t2 = strip_ticks(t[i + 1].text), a2 = t2;
                i += 2;
                if (at(i) == "as") ++i;
                if (i < t.size() && at(i) != "set")
                    a2 = strip_ticks(t[i++].text);
                from_clause = ", `" + t2 + "` `" + a2 + "`";
                set_index = i;
            }
            else if (is_join_word(at(i)))
            {
                size_t join_begin = t[i].begin;
                while (i < t.size() && at(i) != "set") ++i;
                if (i >= t.size())
                    return s;
                from_clause = " " + s.substr(join_begin, t[i].begin - join_begin);
                set_index = i;
            }
            else
                return s;
            if (at(set_index) != "set")
                return s;
            size_t where_index = set_index + 1;
            while (where_index < t.size() && at(where_index) != "where") ++where_index;
            size_t assigns_end = where_index < t.size() ? t[where_index].begin : s.size();
            std::string assigns = drop_alias(s.substr(t[set_index].end, assigns_end - t[set_index].end), a1);
            std::string where = where_index < t.size() ? s.substr(t[where_index].end) : std::string();
            while (!where.empty() && (isspace(static_cast<unsigned char>(where.back())) || where.back() == ';')) where.pop_back();
            auto pk = row_key(db, t1);
            return "UPDATE `" + t1 + "` SET " + assigns + " WHERE " + pk_condition(pk) + " IN (SELECT " + key_list(pk, "`" + a1 + "`.") + " FROM `" + t1 + "` `" + a1 + "`" + from_clause
                   + (where.empty() ? std::string() : " WHERE " + where) + ")";
        }
        return s;
    }

    std::string translate_sql(sqlite3* db, std::string_view in)
    {
        std::string s = rewrite_multi_table(db, std::string(in));
        std::string out;
        out.reserve(s.size() + 32);
        std::string insert_table;       // INSERT [IGNORE] INTO <table>
        bool in_upsert = false;

        for (size_t i = 0; i < s.size();)
        {
            char c = s[i];
            if (c == '\'' || c == '"' || c == '`')
            {
                size_t j = i + 1;
                while (j < s.size())
                {
                    if (s[j] == '\\' && c != '`' && j + 1 < s.size()) { j += 2; continue; }
                    if (s[j] == c) { if (j + 1 < s.size() && s[j + 1] == c) { j += 2; continue; } break; }
                    ++j;
                }
                // MySQL strings may use backslash escapes: convert them to what SQLite reads
                if (c == '\'')
                {
                    out += '\'';
                    for (size_t k = i + 1; k < j && k < s.size(); ++k)
                    {
                        if (s[k] == '\\' && k + 1 < j)
                        {
                            char e = s[k + 1];
                            switch (e)
                            {
                                case 'n': out += '\n'; break;
                                case 'r': out += '\r'; break;
                                case 't': out += '\t'; break;
                                case '0': out += '\0'; break;
                                case '\'': out += "''"; break;
                                default: out += e; break;   // \\ \" \% ...
                            }
                            ++k;
                        }
                        else
                            out += s[k];
                    }
                    out += '\'';
                }
                else
                    out.append(s, i, j - i + 1);
                i = j + 1;
                continue;
            }
            if (c == '.' && i + 1 < s.size() && isalpha(static_cast<unsigned char>(s[i + 1])))
            {
                // table.order, table.group ...: words SQLite reserves are plain column names in MySQL after a dot
                size_t j = i + 1;
                while (j < s.size() && is_word_char(s[j])) ++j;
                std::string word = lower(s.substr(i + 1, j - i - 1).c_str());
                static char const* const reserved[] = {"order", "group", "key", "index", "limit", "default", "check", "references", "table", "values", "window", "over", "row", "rows", "offset"};
                bool quote = false;
                for (char const* r : reserved)
                    quote = quote || word == r;
                if (quote)
                {
                    out += ".`" + s.substr(i + 1, j - i - 1) + "`";
                    i = j;
                    continue;
                }
            }
            if (c == '-' && i + 2 < s.size() && s[i + 1] == '-' && s[i + 2] == ' ')
            {
                size_t j = s.find('\n', i);
                j = j == std::string::npos ? s.size() : j;
                out.append(s, i, j - i);
                i = j;
                continue;
            }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
            {
                size_t j = s.find("*/", i + 2);
                j = j == std::string::npos ? s.size() : j + 2;
                out.append(s, i, j - i);
                i = j;
                continue;
            }

            if (is_word_char(c) && (i == 0 || !is_word_char(s[i - 1])))
            {
                size_t e;
                if ((e = match_phrase(s, i, "INSERT IGNORE")) != std::string::npos)
                {
                    out += "INSERT OR IGNORE";
                    i = e;
                    continue;
                }
                if ((e = match_phrase(s, i, "START TRANSACTION")) != std::string::npos)
                {
                    out += "BEGIN";
                    i = e;
                    continue;
                }
                if ((e = match_phrase(s, i, "TRUNCATE TABLE")) != std::string::npos || (e = match_phrase(s, i, "TRUNCATE")) != std::string::npos)
                {
                    out += "DELETE FROM";
                    i = e;
                    continue;
                }
                if ((e = match_phrase(s, i, "ON DUPLICATE KEY UPDATE")) != std::string::npos)
                {
                    std::vector<std::string> pk = insert_table.empty() ? std::vector<std::string>() : primary_key_columns(db, insert_table);
                    if (pk.empty())
                    {
                        out += "ON CONFLICT DO UPDATE SET";   // will fail in SQLite with a message that names the problem
                    }
                    else
                    {
                        out += "ON CONFLICT(";
                        for (size_t k = 0; k < pk.size(); ++k)
                            out += (k ? ",`" : "`") + pk[k] + "`";
                        out += ") DO UPDATE SET";
                    }
                    in_upsert = true;
                    i = e;
                    continue;
                }
                if (in_upsert && (e = match_phrase(s, i, "VALUES")) != std::string::npos)
                {
                    // VALUES(col) -> excluded.col
                    size_t p = e;
                    while (p < s.size() && isspace(static_cast<unsigned char>(s[p]))) ++p;
                    if (p < s.size() && s[p] == '(')
                    {
                        size_t q = s.find(')', p);
                        if (q != std::string::npos)
                        {
                            std::string col = s.substr(p + 1, q - p - 1);
                            col.erase(std::remove_if(col.begin(), col.end(), [](char ch) { return ch == '`' || isspace(static_cast<unsigned char>(ch)); }), col.end());
                            out += "excluded.`" + col + "`";
                            i = q + 1;
                            continue;
                        }
                    }
                }
                if (insert_table.empty() && ((e = match_phrase(s, i, "INSERT INTO")) != std::string::npos || (e = match_phrase(s, i, "INSERT OR IGNORE INTO")) != std::string::npos
                    || (e = match_phrase(s, i, "REPLACE INTO")) != std::string::npos))
                {
                    size_t p = e;
                    while (p < s.size() && isspace(static_cast<unsigned char>(s[p]))) ++p;
                    size_t q = p;
                    while (q < s.size() && (is_word_char(s[q]) || s[q] == '`' || s[q] == '.')) ++q;
                    insert_table = s.substr(p, q - p);
                    insert_table.erase(std::remove(insert_table.begin(), insert_table.end(), '`'), insert_table.end());
                    out.append(s, i, e - i);
                    i = e;
                    continue;
                }
                size_t j = i;
                while (j < s.size() && is_word_char(s[j])) ++j;
                out.append(s, i, j - i);
                i = j;
                continue;
            }
            out += c;
            ++i;
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // SQL functions of MySQL that SQLite lacks

    void fn_now(sqlite3_context* ctx, int, sqlite3_value**)
    {
        time_t t = time(nullptr);
        struct tm tmv;
        gmtime_r(&t, &tmv);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        sqlite3_result_text(ctx, buf, -1, SQLITE_TRANSIENT);
    }

    void fn_unix_timestamp(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        if (argc == 0)
        {
            sqlite3_result_int64(ctx, int64_t(time(nullptr)));
            return;
        }
        if (sqlite3_value_type(argv[0]) == SQLITE_NULL)
        {
            sqlite3_result_null(ctx);
            return;
        }
        if (sqlite3_value_type(argv[0]) == SQLITE_INTEGER || sqlite3_value_type(argv[0]) == SQLITE_FLOAT)
        {
            sqlite3_result_int64(ctx, sqlite3_value_int64(argv[0]));
            return;
        }
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        char const* text = reinterpret_cast<char const*>(sqlite3_value_text(argv[0]));
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
        if (!text || sscanf(text, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3)
        {
            sqlite3_result_int64(ctx, 0);
            return;
        }
        tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d; tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = sec;
        sqlite3_result_int64(ctx, int64_t(timegm(&tmv)));
    }

    void fn_from_unixtime(sqlite3_context* ctx, int, sqlite3_value** argv)
    {
        if (sqlite3_value_type(argv[0]) == SQLITE_NULL)
        {
            sqlite3_result_null(ctx);
            return;
        }
        time_t t = time_t(sqlite3_value_int64(argv[0]));
        struct tm tmv;
        gmtime_r(&t, &tmv);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        sqlite3_result_text(ctx, buf, -1, SQLITE_TRANSIENT);
    }

    void fn_greatest(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        double best = -INFINITY;
        for (int i = 0; i < argc; ++i)
            best = std::max(best, sqlite3_value_double(argv[i]));
        sqlite3_result_double(ctx, best);
    }

    void fn_least(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        double best = INFINITY;
        for (int i = 0; i < argc; ++i)
            best = std::min(best, sqlite3_value_double(argv[i]));
        sqlite3_result_double(ctx, best);
    }

    void fn_rand(sqlite3_context* ctx, int, sqlite3_value**)
    {
        sqlite3_result_double(ctx, double(rand()) / double(RAND_MAX));
    }

    void fn_last_insert_id(sqlite3_context* ctx, int, sqlite3_value**)
    {
        sqlite3_result_int64(ctx, sqlite3_last_insert_rowid(sqlite3_context_db_handle(ctx)));
    }

    void fn_substring(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        // SUBSTRING(s, pos [, len]), 1 based, a negative position counts from the end
        std::string s = reinterpret_cast<char const*>(sqlite3_value_text(argv[0]) ? sqlite3_value_text(argv[0]) : reinterpret_cast<unsigned char const*>(""));
        sqlite3_int64 pos = sqlite3_value_int64(argv[1]);
        if (pos > 0) --pos; else if (pos < 0) pos = std::max<sqlite3_int64>(0, sqlite3_int64(s.size()) + pos);
        if (pos > sqlite3_int64(s.size())) { sqlite3_result_text(ctx, "", 0, SQLITE_STATIC); return; }
        std::string r = argc > 2 ? s.substr(size_t(pos), size_t(std::max<sqlite3_int64>(0, sqlite3_value_int64(argv[2])))) : s.substr(size_t(pos));
        sqlite3_result_text(ctx, r.c_str(), int(r.size()), SQLITE_TRANSIENT);
    }


    // MySQL's DATE_FORMAT / FROM_UNIXTIME(x, format) codes
    std::string mysql_format_time(struct tm const& t, std::string const& fmt)
    {
        static char const* const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        static char const* const months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
        std::string out;
        char buf[64];
        for (size_t i = 0; i < fmt.size(); ++i)
        {
            if (fmt[i] != '%' || i + 1 >= fmt.size()) { out += fmt[i]; continue; }
            switch (fmt[++i])
            {
                case 'Y': snprintf(buf, sizeof(buf), "%04d", t.tm_year + 1900); out += buf; break;
                case 'y': snprintf(buf, sizeof(buf), "%02d", (t.tm_year + 1900) % 100); out += buf; break;
                case 'm': snprintf(buf, sizeof(buf), "%02d", t.tm_mon + 1); out += buf; break;
                case 'c': out += std::to_string(t.tm_mon + 1); break;
                case 'd': snprintf(buf, sizeof(buf), "%02d", t.tm_mday); out += buf; break;
                case 'e': out += std::to_string(t.tm_mday); break;
                case 'H': snprintf(buf, sizeof(buf), "%02d", t.tm_hour); out += buf; break;
                case 'k': out += std::to_string(t.tm_hour); break;
                case 'h': case 'I': snprintf(buf, sizeof(buf), "%02d", t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12); out += buf; break;
                case 'i': snprintf(buf, sizeof(buf), "%02d", t.tm_min); out += buf; break;
                case 's': case 'S': snprintf(buf, sizeof(buf), "%02d", t.tm_sec); out += buf; break;
                case 'T': snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec); out += buf; break;
                case 'p': out += t.tm_hour < 12 ? "AM" : "PM"; break;
                case 'W': out += days[t.tm_wday % 7]; break;
                case 'a': out += std::string(days[t.tm_wday % 7]).substr(0, 3); break;
                case 'M': out += months[t.tm_mon % 12]; break;
                case 'b': out += std::string(months[t.tm_mon % 12]).substr(0, 3); break;
                case 'j': snprintf(buf, sizeof(buf), "%03d", t.tm_yday + 1); out += buf; break;
                case '%': out += '%'; break;
                default: out += fmt[i]; break;
            }
        }
        return out;
    }

    // a DATETIME text ("2026-10-11 12:00:00"), a DATE or a number of seconds -> struct tm (UTC)
    bool value_to_tm(sqlite3_value* v, struct tm* t)
    {
        memset(t, 0, sizeof(*t));
        if (sqlite3_value_type(v) == SQLITE_NULL)
            return false;
        if (sqlite3_value_type(v) == SQLITE_INTEGER || sqlite3_value_type(v) == SQLITE_FLOAT)
        {
            time_t sec = time_t(sqlite3_value_int64(v));
            gmtime_r(&sec, t);
            return true;
        }
        char const* text = reinterpret_cast<char const*>(sqlite3_value_text(v));
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
        if (!text || sscanf(text, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3)
            return false;
        t->tm_year = y - 1900; t->tm_mon = mo - 1; t->tm_mday = d; t->tm_hour = h; t->tm_min = mi; t->tm_sec = sec;
        time_t norm = timegm(t);   // fills tm_wday and tm_yday
        gmtime_r(&norm, t);
        return true;
    }

    void fn_date_format(sqlite3_context* ctx, int, sqlite3_value** argv)
    {
        struct tm t;
        if (!value_to_tm(argv[0], &t)) { sqlite3_result_null(ctx); return; }
        std::string r = mysql_format_time(t, reinterpret_cast<char const*>(sqlite3_value_text(argv[1])));
        sqlite3_result_text(ctx, r.c_str(), int(r.size()), SQLITE_TRANSIENT);
    }

    void fn_from_unixtime_fmt(sqlite3_context* ctx, int, sqlite3_value** argv)
    {
        struct tm t;
        if (!value_to_tm(argv[0], &t)) { sqlite3_result_null(ctx); return; }
        std::string r = mysql_format_time(t, reinterpret_cast<char const*>(sqlite3_value_text(argv[1])));
        sqlite3_result_text(ctx, r.c_str(), int(r.size()), SQLITE_TRANSIENT);
    }

    template <int Part>
    void fn_date_part(sqlite3_context* ctx, int, sqlite3_value** argv)
    {
        struct tm t;
        if (!value_to_tm(argv[0], &t)) { sqlite3_result_null(ctx); return; }
        int v = Part == 0 ? t.tm_year + 1900 : Part == 1 ? t.tm_mon + 1 : Part == 2 ? t.tm_mday : Part == 3 ? t.tm_hour : Part == 4 ? t.tm_min : t.tm_sec;
        sqlite3_result_int(ctx, v);
    }

    void fn_datediff(sqlite3_context* ctx, int, sqlite3_value** argv)
    {
        struct tm a, b;
        if (!value_to_tm(argv[0], &a) || !value_to_tm(argv[1], &b)) { sqlite3_result_null(ctx); return; }
        a.tm_hour = a.tm_min = a.tm_sec = b.tm_hour = b.tm_min = b.tm_sec = 0;
        sqlite3_result_int64(ctx, (int64_t(timegm(&a)) - int64_t(timegm(&b))) / 86400);
    }

    void fn_curdate(sqlite3_context* ctx, int, sqlite3_value**)
    {
        time_t t = time(nullptr);
        struct tm tmv;
        gmtime_r(&t, &tmv);
        char buf[16];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
        sqlite3_result_text(ctx, buf, -1, SQLITE_TRANSIENT);
    }

    void register_functions(sqlite3* db)
    {
        int const flags = SQLITE_UTF8;
        sqlite3_create_function(db, "now", 0, flags, nullptr, fn_now, nullptr, nullptr);
        sqlite3_create_function(db, "unix_timestamp", 0, flags, nullptr, fn_unix_timestamp, nullptr, nullptr);
        sqlite3_create_function(db, "unix_timestamp", 1, flags, nullptr, fn_unix_timestamp, nullptr, nullptr);
        sqlite3_create_function(db, "from_unixtime", 1, flags, nullptr, fn_from_unixtime, nullptr, nullptr);
        sqlite3_create_function(db, "from_unixtime", 2, flags, nullptr, fn_from_unixtime_fmt, nullptr, nullptr);
        sqlite3_create_function(db, "date_format", 2, flags, nullptr, fn_date_format, nullptr, nullptr);
        sqlite3_create_function(db, "year", 1, flags, nullptr, fn_date_part<0>, nullptr, nullptr);
        sqlite3_create_function(db, "month", 1, flags, nullptr, fn_date_part<1>, nullptr, nullptr);
        sqlite3_create_function(db, "day", 1, flags, nullptr, fn_date_part<2>, nullptr, nullptr);
        sqlite3_create_function(db, "dayofmonth", 1, flags, nullptr, fn_date_part<2>, nullptr, nullptr);
        sqlite3_create_function(db, "hour", 1, flags, nullptr, fn_date_part<3>, nullptr, nullptr);
        sqlite3_create_function(db, "minute", 1, flags, nullptr, fn_date_part<4>, nullptr, nullptr);
        sqlite3_create_function(db, "second", 1, flags, nullptr, fn_date_part<5>, nullptr, nullptr);
        sqlite3_create_function(db, "datediff", 2, flags, nullptr, fn_datediff, nullptr, nullptr);
        sqlite3_create_function(db, "curdate", 0, flags, nullptr, fn_curdate, nullptr, nullptr);
        sqlite3_create_function(db, "greatest", -1, flags, nullptr, fn_greatest, nullptr, nullptr);
        sqlite3_create_function(db, "least", -1, flags, nullptr, fn_least, nullptr, nullptr);
        sqlite3_create_function(db, "rand", 0, flags, nullptr, fn_rand, nullptr, nullptr);
        sqlite3_create_function(db, "last_insert_id", 0, flags, nullptr, fn_last_insert_id, nullptr, nullptr);
        sqlite3_create_function(db, "substring", 2, flags, nullptr, fn_substring, nullptr, nullptr);
        sqlite3_create_function(db, "substring", 3, flags, nullptr, fn_substring, nullptr, nullptr);
    }

    // ---------------------------------------------------------------------------------------------------------------------
    // errors

    unsigned map_error(int rc, std::string const& message)
    {
        int primary = rc & 0xff;
        if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED)
            return ER_LOCK_DEADLOCK;
        if (primary == SQLITE_CONSTRAINT)
            return (rc == SQLITE_CONSTRAINT_UNIQUE || rc == SQLITE_CONSTRAINT_PRIMARYKEY) ? ER_DUP_ENTRY : CR_UNKNOWN_ERROR;
        if (message.find("no such table") != std::string::npos)
            return ER_NO_SUCH_TABLE;
        if (message.find("no such column") != std::string::npos || message.find("has no column named") != std::string::npos)
            return ER_BAD_FIELD_ERROR;
        if (message.find("values for") != std::string::npos && message.find("columns") != std::string::npos)
            return ER_WRONG_VALUE_COUNT;
        if (message.find("syntax error") != std::string::npos || message.find("incomplete input") != std::string::npos
            || message.find("misuse") != std::string::npos || message.find("no such function") != std::string::npos
            || message.find("ON CONFLICT") != std::string::npos)
            return ER_PARSE_ERROR;
        return CR_UNKNOWN_ERROR;
    }
}

// -------------------------------------------------------------------------------------------------------------------------
// MYSQL_SQLITE_TRACE=file: every executed statement with its database, thread, result and number of changed rows

namespace
{
    void trace_sql(char const* db, char const* kind, std::string const& sql, int rc, long long changes, std::string const& error)
    {
        static char const* const path = getenv("MYSQL_SQLITE_TRACE");
        if (!path)
            return;
        static std::mutex lock;
        std::lock_guard<std::mutex> guard(lock);
        if (FILE* f = fopen(path, "a"))
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            fprintf(f, "%ld.%03ld [%s %s thread %zu] rc=%d changes=%lld %s%s%s\n", long(ts.tv_sec), long(ts.tv_nsec / 1000000), db, kind,
                    std::hash<std::thread::id>()(std::this_thread::get_id()) % 100000, rc, changes, sql.substr(0, 400).c_str(), error.empty() ? "" : "  ERROR: ", error.c_str());
            fclose(f);
        }
    }
}

// -------------------------------------------------------------------------------------------------------------------------
// MySQL user variables: SET @x := expr stores the value, later statements of the connection get it in place of @x (an unknown one is NULL)

namespace
{
    std::string first_word_of(std::string const& s)
    {
        size_t i = 0;
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t j = i;
        while (j < s.size() && (isalpha(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
        return lower(s.substr(i, j - i).c_str());
    }

    std::string substitute_variables(char const* q, std::map<std::string, std::string> const& vars)
    {
        std::string s = q ? q : "";
        if (s.find('@') == std::string::npos)
            return s;
        std::string out;
        bool is_set = first_word_of(s) == "set";
        for (size_t i = 0; i < s.size();)
        {
            char c = s[i];
            if (c == '\'' || c == '"' || c == '`')
            {
                size_t j = i + 1;
                while (j < s.size())
                {
                    if (s[j] == '\\' && c != '`' && j + 1 < s.size()) { j += 2; continue; }
                    if (s[j] == c) { if (j + 1 < s.size() && s[j + 1] == c) { j += 2; continue; } break; }
                    ++j;
                }
                j = std::min(s.size(), j + 1);
                out.append(s, i, j - i);
                i = j;
                continue;
            }
            if (c == '@' && i + 1 < s.size() && s[i + 1] != '@' && is_word_char(s[i + 1]) && (i == 0 || s[i - 1] != '@'))
            {
                size_t j = i + 1;
                while (j < s.size() && is_word_char(s[j])) ++j;
                std::string name = lower(s.substr(i + 1, j - i - 1).c_str());
                // the target of an assignment stays (SET @x := @x + 1: only the right side is replaced)
                size_t k = j;
                while (k < s.size() && isspace(static_cast<unsigned char>(s[k]))) ++k;
                bool target = is_set && k < s.size() && (s[k] == '=' || (s[k] == ':' && k + 1 < s.size() && s[k + 1] == '='));
                if (target)
                    out.append(s, i, j - i);
                else
                {
                    auto it = vars.find(name);
                    out += it != vars.end() ? it->second : "NULL";
                }
                i = j;
                continue;
            }
            out += c;
            ++i;
        }
        return out;
    }

    // SET @a := expr [, @b = expr ...] (other SET statements are session settings: ignored)
    bool assign_variables(sqlite3* db, std::string const& s, std::map<std::string, std::string>& vars, std::string* error)
    {
        size_t i = s.find_first_of(" \t\r\n");
        while (i != std::string::npos && i < s.size())
        {
            while (i < s.size() && (isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
            if (i >= s.size() || s[i] != '@')
                return true;
            size_t j = i + 1;
            while (j < s.size() && is_word_char(s[j])) ++j;
            std::string name = lower(s.substr(i + 1, j - i - 1).c_str());
            while (j < s.size() && isspace(static_cast<unsigned char>(s[j]))) ++j;
            if (j < s.size() && s[j] == ':') ++j;
            if (j >= s.size() || s[j] != '=')
            {
                *error = "cannot read the assignment of @" + name;
                return false;
            }
            ++j;
            // the expression runs to the next comma outside parentheses and quotes
            size_t e = j;
            int depth = 0;
            while (e < s.size())
            {
                char c = s[e];
                if (c == '\'' || c == '"')
                {
                    ++e;
                    while (e < s.size() && s[e] != c) { if (s[e] == '\\') ++e; ++e; }
                }
                else if (c == '(') ++depth;
                else if (c == ')') --depth;
                else if ((c == ',' && depth == 0) || c == ';') break;
                ++e;
            }
            std::string expr = s.substr(j, e - j);
            std::string sql = "SELECT " + translate_sql(db, expr);
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
            {
                *error = std::string(sqlite3_errmsg(db)) + " in: " + sql;
                return false;
            }
            std::string value = "NULL";
            if (sqlite3_step(st) == SQLITE_ROW)
            {
                switch (sqlite3_column_type(st, 0))
                {
                    case SQLITE_INTEGER: value = std::to_string(sqlite3_column_int64(st, 0)); break;
                    case SQLITE_FLOAT:
                    {
                        char buf[40];
                        snprintf(buf, sizeof(buf), "%.17g", sqlite3_column_double(st, 0));
                        value = buf;
                        break;
                    }
                    case SQLITE_TEXT:
                    case SQLITE_BLOB:
                    {
                        std::string v(reinterpret_cast<char const*>(sqlite3_column_blob(st, 0)), size_t(sqlite3_column_bytes(st, 0)));
                        value = "'";
                        for (char c : v) { if (c == '\'') value += '\''; else if (c == '\\') value += '\\'; value += c; }
                        value += "'";
                        break;
                    }
                    default: break;
                }
            }
            sqlite3_finalize(st);
            vars[name] = value;
            i = e;
        }
        return true;
    }
}

// -------------------------------------------------------------------------------------------------------------------------
// the opaque parts of the handles

struct ShimResult
{
    Meta own_meta;
    Meta* meta = &own_meta;      // the statement's metadata for the result of mysql_stmt_result_metadata
    bool owns = true;
    Rows rows;
    uint64_t cursor = 0;
    std::string rowbuf;
    std::vector<char*> rowptrs;
    std::vector<unsigned long> lengths;
};

struct ShimMysql
{
    sqlite3* db = nullptr;
    unsigned err_no = 0;
    std::string err;
    my_ulonglong affected = 0;
    unsigned field_count = 0;
    ShimResult* pending = nullptr;
    std::string path;
    std::string dir;
    std::string name;
    std::map<std::string, std::string> vars;   // MySQL user variables (SET @x := ...) as SQL literals, per connection like a MySQL session
    sqlite3_session* session = nullptr;        // a streamed database's change log records this connection's changes (src/changelog.cpp)
    bool counted = false;                      // counted among the open connections to `path` (see mysql_close)
};

struct ShimStmt
{
    sqlite3_stmt* st = nullptr;
    std::string sql;
    Meta meta;
    Rows rows;
    uint64_t cursor = 0;
    bool stored = false;
    unsigned err_no = 0;
    std::string err;
    my_ulonglong affected = 0;
};

namespace
{
    void set_error(ShimMysql* m, unsigned no, std::string const& text)
    {
        m->err_no = no;
        m->err = text;
    }

    void clear_error(ShimMysql* m)
    {
        m->err_no = 0;
        m->err.clear();
    }

    void drop_pending(ShimMysql* m)
    {
        delete m->pending;
        m->pending = nullptr;
    }
}

// -------------------------------------------------------------------------------------------------------------------------
// library and connection

namespace
{
    std::mutex g_first_open_lock;
    std::map<std::string, ShimMysql*> g_change_connection;   // by database: a connection that records into the change log (the server's change of realm switches the log through it)
    std::mutex g_change_connection_lock;
    std::set<std::string> g_opened;   // the databases that were prepared (their first connection): a database is its name and its file, so a realm's file is another one
    std::map<std::string, int> g_connections;   // the open connections to each file; when the last one is closed the file is let go (see mysql_close)
}

extern "C"
{

void mysql_sqlite_set_error(MYSQL* mysql, unsigned number, const char* text)
{
    set_error(mysql->impl, number, text ? text : "");
}

int mysql_library_init(int, char**, char**)
{
    sqlite3_initialize();
    return 0;
}

void mysql_library_end(void)
{
    sqlite3_shutdown();
}

unsigned int mysql_thread_safe(void) { return 1; }
unsigned long mysql_get_client_version(void) { return MYSQL_VERSION_ID; }
const char* mysql_get_client_info(void) { return MYSQL_SERVER_VERSION " (SQLite " SQLITE_VERSION " shim)"; }

MYSQL* mysql_init(MYSQL* mysql)
{
    if (!mysql)
        mysql = new MYSQL();
    mysql->impl = new ShimMysql();
    return mysql;
}

int mysql_options(MYSQL*, enum mysql_option, const void*) { return 0; }

MYSQL* mysql_real_connect(MYSQL* mysql, const char* host, const char*, const char*, const char* db, unsigned int, const char*, unsigned long)
{
    ShimMysql* m = mysql->impl;
    m->dir = host && *host ? host : ".";
    m->name = db ? db : "";
    m->path = m->dir + "/" + m->name + ".sqlite";

    // the database's settings (MYSQL_SQLITE_CONF, src/config.cpp); the first connection to a database prepares it while later ones wait:
    // a working copy gets its kept file, then the update directories are applied
    shim::DatabaseOptions const options = shim::database_options(m->name);
    bool read_only = options.read_only;
    std::unique_lock<std::mutex> first_open(g_first_open_lock);
    std::string const opened_key = m->name + "\n" + m->path;
    bool first = g_opened.insert(opened_key).second;
    if (first && !options.backup.empty())
    {
        std::string error;
        if (!shim::restore_working_copy(m->path, options.backup, &error))
        {
            g_opened.erase(opened_key);
            set_error(m, CR_CONN_HOST_ERROR, error);
            return nullptr;
        }
    }


    // a streamed database (the world): its pages come from the page's cache, its changes stay in memory (src/stream_vfs.cpp)
    bool streamed = options.stream && shim::is_streamed(m->path);
    if (streamed)
        shim::register_stream_vfs();
    int flags = (read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE) | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(m->path.c_str(), &m->db, flags, streamed ? shim::STREAM_VFS : nullptr);
    if (rc != SQLITE_OK)
    {
        if (first)
            g_opened.erase(opened_key);
        set_error(m, rc == SQLITE_CANTOPEN ? ER_BAD_DB_ERROR : CR_CONN_HOST_ERROR, std::string("cannot open ") + m->path + ": " + (m->db ? sqlite3_errmsg(m->db) : "out of memory"));
        if (m->db)
        {
            sqlite3_close(m->db);
            m->db = nullptr;
        }
        return nullptr;
    }

#ifdef __EMSCRIPTEN__
    // in the wasm build SQLite's lookaside allocator and SQLITE_PREPARE_PERSISTENT corrupt the heap together: no lookaside
    sqlite3_db_config(m->db, SQLITE_DBCONFIG_LOOKASIDE, nullptr, 0, 0);
#endif
    sqlite3_busy_timeout(m->db, 10000);
    register_functions(m->db);
    sqlite3_exec(m->db, "PRAGMA temp_store=MEMORY; PRAGMA cache_size=-16384;", nullptr, nullptr, nullptr);
    if (streamed)   // its changes live in memory anyway (the overlay); a journal file would need a local database file (the unix VFS copies its mode)
        sqlite3_exec(m->db, "PRAGMA journal_mode=MEMORY;", nullptr, nullptr, nullptr);
    if (!read_only)
        sqlite3_exec(m->db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);   // the journal mode stays what the file has (WAL for auth and characters, set once by the converter)
    clear_error(m);
    if (first && !options.updates_source.empty() && !read_only)
    {
        int applied = mysql_sqlite_apply_updates(mysql, options.updates_source.c_str());
        if (applied < 0)
        {
            fprintf(stderr, "mysql-sqlite: updating %s failed: %s\n", m->name.c_str(), m->err.c_str());
            g_opened.erase(opened_key);
            sqlite3_close(m->db);
            m->db = nullptr;
            return nullptr;
        }
        if (applied > 0)
            fprintf(stderr, "mysql-sqlite: %s: %d update files applied\n", m->name.c_str(), applied);
    }
    if (first && !options.backup.empty())
        shim::start_saver(m->path, options.backup, options.backup_interval);
    if (!options.changes.empty() && !read_only)
    {
        // the change log: replayed by the first connection (after the update directories), then every connection records its commits
        std::string error;
        if (first && !shim::changelog_open(m->name, options.changes, m->db, &error))
            fprintf(stderr, "mysql-sqlite: %s: the change log cannot be used: %s\n", m->name.c_str(), error.c_str());
        m->session = shim::changelog_attach(m->db);
        std::lock_guard<std::mutex> guard(g_change_connection_lock);
        g_change_connection[m->name] = m;
    }
    if (first && streamed)
        fprintf(stderr, "mysql-sqlite: %s is streamed (%lld bytes read so far)\n", m->path.c_str(), shim::streamed_bytes(m->path));
    ++g_connections[m->path];
    m->counted = true;
    return mysql;
}

void mysql_close(MYSQL* mysql)
{
    if (!mysql)
        return;
    if (ShimMysql* m = mysql->impl)
    {
        drop_pending(m);
        {
            std::lock_guard<std::mutex> guard(g_change_connection_lock);
            auto registered = g_change_connection.find(m->name);
            if (registered != g_change_connection.end() && registered->second == m)
                g_change_connection.erase(registered);
        }
        if (m->session)
            sqlite3session_delete(m->session);
        if (m->db)
            sqlite3_close_v2(m->db);
        if (m->counted)
        {
            // The last connection to a file is closed (the server leaves a realm): its working copy is saved once more and let go, and it is prepared again when
            // a connection to it is made again.
            std::string const path = m->path, key = m->name + "\n" + m->path;
            bool last;
            {
                std::lock_guard<std::mutex> guard(g_first_open_lock);
                last = --g_connections[path] == 0;
                if (last)
                {
                    g_connections.erase(path);
                    g_opened.erase(key);
                }
            }
            if (last)
                shim::stop_saver(path);
        }
        delete m;
    }
    delete mysql;
}

int mysql_ping(MYSQL* mysql) { return mysql && mysql->impl && mysql->impl->db ? 0 : 1; }
int mysql_autocommit(MYSQL*, bool) { return 0; }
int mysql_set_character_set(MYSQL*, const char*) { return 0; }
unsigned int mysql_errno(MYSQL* mysql) { return mysql && mysql->impl ? mysql->impl->err_no : 0; }
const char* mysql_error(MYSQL* mysql) { return mysql && mysql->impl ? mysql->impl->err.c_str() : ""; }
unsigned long mysql_get_server_version(MYSQL*) { return MYSQL_VERSION_ID; }
const char* mysql_get_server_info(MYSQL*) { return MYSQL_SERVER_VERSION; }

unsigned long mysql_real_escape_string(MYSQL*, char* to, const char* from, unsigned long length)
{
    // SQLite has one escape: the quote doubled; a backslash is an ordinary character in SQLite strings, but translate_sql reads
    // backslash escapes in MySQL strings, so a literal backslash is doubled here to survive that reading
    char* out = to;
    for (unsigned long i = 0; i < length; ++i)
    {
        char c = from[i];
        if (c == '\'') { *out++ = '\''; *out++ = '\''; }
        else if (c == '\\') { *out++ = '\\'; *out++ = '\\'; }
        else *out++ = c;
    }
    *out = 0;
    return unsigned(out - to);
}

// -------------------------------------------------------------------------------------------------------------------------
// text queries

int mysql_query(MYSQL* mysql, const char* q)
{
    ShimMysql* m = mysql->impl;
    drop_pending(m);
    m->affected = 0;
    m->field_count = 0;
    clear_error(m);

    // statements of MySQL that SQLite has no form for: user variables, session settings, table locks, table definitions
    std::string text = substitute_variables(q, m->vars);
    std::string word = first_word_of(text);
    if (word == "set")
    {
        std::string error;
        if (!assign_variables(m->db, text, m->vars, &error))
        {
            set_error(m, ER_PARSE_ERROR, error);
            return 1;
        }
        return 0;   // SET NAMES, SET FOREIGN_KEY_CHECKS ...: nothing to do
    }
    if (word == "lock" || word == "unlock")
        return 0;
    {
        bool handled = false;
        std::string error;
        bool ok = shim::run_ddl(m->db, text, &handled, &error);
        trace_sql(m->name.c_str(), "ddl", handled ? text : std::string(), ok ? SQLITE_OK : SQLITE_ERROR, 0, error);
        if (!ok)
        {
            set_error(m, map_error(SQLITE_ERROR, error), error);
            return 1;
        }
        if (handled)
        {
            shim::changelog_after(m->name, m->db, m->session);
            return 0;
        }
    }

    std::string sql = translate_sql(m->db, text);
    char const* tail = sql.c_str();
    while (tail && *tail)
    {
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(m->db, tail, -1, &st, &tail);
        if (rc != SQLITE_OK)
        {
            set_error(m, map_error(rc, sqlite3_errmsg(m->db)), sqlite3_errmsg(m->db));
            return 1;
        }
        if (!st)
            continue;   // blank or a comment

        ShimResult* result = nullptr;
        if (sqlite3_column_count(st) > 0)
        {
            result = new ShimResult();
            build_meta(result->own_meta, st);
            result->rows.ncols = uint32_t(sqlite3_column_count(st));
        }
        while ((rc = sqlite3_step(st)) == SQLITE_ROW)
            if (result)
                result->rows.append_row(st);

        if (rc != SQLITE_DONE)
        {
            set_error(m, map_error(sqlite3_extended_errcode(m->db), sqlite3_errmsg(m->db)), sqlite3_errmsg(m->db));
            sqlite3_finalize(st);
            delete result;
            return 1;
        }
        trace_sql(m->name.c_str(), "text", sqlite3_sql(st) ? sqlite3_sql(st) : "", rc, sqlite3_changes64(m->db), "");
        sqlite3_finalize(st);

        drop_pending(m);
        if (result)
        {
            finish_meta(result->own_meta, result->rows);
            m->pending = result;
            m->affected = result->rows.nrows;
            m->field_count = unsigned(result->own_meta.fields.size());
        }
        else
        {
            m->affected = my_ulonglong(sqlite3_changes64(m->db));
            m->field_count = 0;
        }
    }
    shim::changelog_after(m->name, m->db, m->session);
    return 0;
}

MYSQL_RES* mysql_store_result(MYSQL* mysql)
{
    ShimMysql* m = mysql->impl;
    if (!m->pending)
        return nullptr;
    MYSQL_RES* res = new MYSQL_RES();
    res->handle = mysql;
    res->impl = m->pending;
    m->pending = nullptr;
    return res;
}

my_ulonglong mysql_affected_rows(MYSQL* mysql) { return mysql->impl->affected; }
unsigned int mysql_field_count(MYSQL* mysql) { return mysql->impl->field_count; }
bool mysql_more_results(MYSQL*) { return false; }
int mysql_next_result(MYSQL*) { return -1; }

MYSQL_FIELD* mysql_fetch_fields(MYSQL_RES* result)
{
    return result->impl->meta->fields.data();
}

MYSQL_ROW mysql_fetch_row(MYSQL_RES* result)
{
    ShimResult* r = result->impl;
    if (r->cursor >= r->rows.nrows)
        return nullptr;

    uint32_t n = r->rows.ncols;
    Cell const* cells = &r->rows.cells[r->cursor++ * n];
    r->rowbuf.clear();
    r->lengths.assign(n, 0);
    std::vector<size_t> starts(n);
    std::vector<bool> is_null(n);
    for (uint32_t c = 0; c < n; ++c)
    {
        starts[c] = r->rowbuf.size();
        is_null[c] = cells[c].kind() == K_NULL;
        std::string v = r->rows.text(cells[c]);
        r->lengths[c] = v.size();
        r->rowbuf += v;
        r->rowbuf += '\0';
    }
    r->rowptrs.assign(n, nullptr);
    for (uint32_t c = 0; c < n; ++c)
        r->rowptrs[c] = is_null[c] ? nullptr : &r->rowbuf[starts[c]];
    return r->rowptrs.data();
}

unsigned long* mysql_fetch_lengths(MYSQL_RES* result)
{
    return result->impl->lengths.data();
}

void mysql_free_result(MYSQL_RES* result)
{
    if (!result)
        return;
    if (result->impl && result->impl->owns)
        delete result->impl;
    delete result;
}

// -------------------------------------------------------------------------------------------------------------------------
// prepared statements

MYSQL_STMT* mysql_stmt_init(MYSQL* mysql)
{
    MYSQL_STMT* stmt = new MYSQL_STMT();
    stmt->mysql = mysql;
    stmt->params = nullptr;
    stmt->bind = nullptr;
    stmt->param_count = 0;
    stmt->field_count = 0;
    stmt->bind_result_done = false;
    stmt->impl = new ShimStmt();
    return stmt;
}

int mysql_stmt_prepare(MYSQL_STMT* stmt, const char* query, unsigned long length)
{
    ShimStmt* s = stmt->impl;
    ShimMysql* m = stmt->mysql->impl;

    s->sql = translate_sql(m->db, std::string_view(query, length));
    int rc = sqlite3_prepare_v3(m->db, s->sql.c_str(), -1,
#ifdef __EMSCRIPTEN__
                                  0,   // not SQLITE_PREPARE_PERSISTENT, see mysql_real_connect
#else
                                  SQLITE_PREPARE_PERSISTENT,
#endif
                                  &s->st, nullptr);
    if (rc != SQLITE_OK || !s->st)
    {
        s->err_no = map_error(rc, sqlite3_errmsg(m->db));
        s->err = std::string(sqlite3_errmsg(m->db)) + " [" + s->sql + "]";
        return 1;
    }
    stmt->param_count = unsigned(sqlite3_bind_parameter_count(s->st));
    stmt->field_count = unsigned(sqlite3_column_count(s->st));
    build_meta(s->meta, s->st);
    s->rows.ncols = unsigned(stmt->field_count);
    return 0;
}

unsigned long mysql_stmt_param_count(MYSQL_STMT* stmt) { return stmt->param_count; }
int mysql_stmt_attr_set(MYSQL_STMT*, enum enum_stmt_attr_type, const void*) { return 0; }

bool mysql_stmt_bind_param(MYSQL_STMT* stmt, MYSQL_BIND* bnd)
{
    stmt->params = bnd;
    return false;
}

bool mysql_stmt_bind_named_param(MYSQL_STMT* stmt, MYSQL_BIND* bnd, unsigned int, const char**)
{
    stmt->params = bnd;   // the parameters are positional (acore passes no names)
    return false;
}

int mysql_stmt_execute(MYSQL_STMT* stmt)
{
    ShimStmt* s = stmt->impl;
    ShimMysql* m = stmt->mysql->impl;

    if (!s->st)
    {
        s->err_no = CR_UNKNOWN_ERROR;
        s->err = "statement was not prepared";
        return 1;
    }
    s->rows.clear();
    s->rows.ncols = unsigned(stmt->field_count);
    s->cursor = 0;
    s->stored = false;
    s->err_no = 0;
    s->err.clear();
    s->affected = 0;

    sqlite3_reset(s->st);
    sqlite3_clear_bindings(s->st);
    for (unsigned long i = 0; i < stmt->param_count; ++i)
    {
        MYSQL_BIND const& b = stmt->params[i];
        int idx = int(i) + 1;
        switch (b.buffer_type)
        {
            case MYSQL_TYPE_NULL: sqlite3_bind_null(s->st, idx); break;
            case MYSQL_TYPE_TINY: sqlite3_bind_int64(s->st, idx, b.is_unsigned ? int64_t(*static_cast<uint8_t const*>(b.buffer)) : int64_t(*static_cast<int8_t const*>(b.buffer))); break;
            case MYSQL_TYPE_SHORT: { int16_t v; memcpy(&v, b.buffer, 2); sqlite3_bind_int64(s->st, idx, b.is_unsigned ? int64_t(uint16_t(v)) : int64_t(v)); break; }
            case MYSQL_TYPE_LONG: case MYSQL_TYPE_INT24: { int32_t v; memcpy(&v, b.buffer, 4); sqlite3_bind_int64(s->st, idx, b.is_unsigned ? int64_t(uint32_t(v)) : int64_t(v)); break; }
            case MYSQL_TYPE_LONGLONG: { int64_t v; memcpy(&v, b.buffer, 8); sqlite3_bind_int64(s->st, idx, v); break; }
            case MYSQL_TYPE_FLOAT: { float v; memcpy(&v, b.buffer, 4); sqlite3_bind_double(s->st, idx, double(v)); break; }
            case MYSQL_TYPE_DOUBLE: { double v; memcpy(&v, b.buffer, 8); sqlite3_bind_double(s->st, idx, v); break; }
            case MYSQL_TYPE_BLOB: case MYSQL_TYPE_TINY_BLOB: case MYSQL_TYPE_MEDIUM_BLOB: case MYSQL_TYPE_LONG_BLOB:
                sqlite3_bind_blob64(s->st, idx, b.buffer ? b.buffer : "", b.length ? *b.length : b.buffer_length, SQLITE_TRANSIENT);
                break;
            default:
                sqlite3_bind_text64(s->st, idx, b.buffer ? static_cast<char const*>(b.buffer) : "", b.length ? *b.length : b.buffer_length, SQLITE_TRANSIENT, SQLITE_UTF8);
                break;
        }
    }

    int rc;
    while ((rc = sqlite3_step(s->st)) == SQLITE_ROW)
        if (stmt->field_count)
            s->rows.append_row(s->st);

    if (rc != SQLITE_DONE)
    {
        s->err_no = map_error(sqlite3_extended_errcode(m->db), sqlite3_errmsg(m->db));
        s->err = std::string(sqlite3_errmsg(m->db)) + " [" + s->sql + "]";
        m->err_no = s->err_no;
        m->err = s->err;
        trace_sql(m->name.c_str(), "prep", s->sql, rc, 0, s->err);
        sqlite3_reset(s->st);
        return 1;
    }

    s->affected = stmt->field_count ? s->rows.nrows : my_ulonglong(sqlite3_changes64(m->db));
    trace_sql(m->name.c_str(), "prep", s->sql, rc, stmt->field_count ? long(s->rows.nrows) : sqlite3_changes64(m->db), "");
    sqlite3_reset(s->st);
    shim::changelog_after(m->name, m->db, m->session);
    if (stmt->field_count)
        finish_meta(s->meta, s->rows);
    return 0;
}

MYSQL_RES* mysql_stmt_result_metadata(MYSQL_STMT* stmt)
{
    if (!stmt->field_count)
        return nullptr;
    MYSQL_RES* res = new MYSQL_RES();
    res->handle = stmt->mysql;
    res->impl = new ShimResult();
    res->impl->meta = &stmt->impl->meta;   // shared with the statement: store_result updates max_length in place
    res->impl->owns = true;                // the ShimResult is ours, the metadata is not (see below)
    res->impl->rows.ncols = 0;
    return res;
}

unsigned int mysql_stmt_field_count(MYSQL_STMT* stmt) { return unsigned(stmt->field_count); }
my_ulonglong mysql_stmt_affected_rows(MYSQL_STMT* stmt) { return stmt->impl->affected; }
my_ulonglong mysql_stmt_num_rows(MYSQL_STMT* stmt) { return stmt->impl->rows.nrows; }

int mysql_stmt_store_result(MYSQL_STMT* stmt)
{
    stmt->impl->stored = true;
    stmt->impl->cursor = 0;
    return 0;
}

bool mysql_stmt_bind_result(MYSQL_STMT* stmt, MYSQL_BIND* bnd)
{
    delete[] stmt->bind;
    stmt->bind = new MYSQL_BIND[stmt->field_count ? stmt->field_count : 1];
    for (unsigned long i = 0; i < stmt->field_count; ++i)
        stmt->bind[i] = bnd[i];
    stmt->bind_result_done = true;
    return false;
}

int mysql_stmt_fetch(MYSQL_STMT* stmt)
{
    ShimStmt* s = stmt->impl;
    if (s->cursor >= s->rows.nrows)
        return MYSQL_NO_DATA;

    uint32_t n = s->rows.ncols;
    Cell const* cells = &s->rows.cells[s->cursor++ * n];
    bool truncated = false;

    for (uint32_t c = 0; c < n; ++c)
    {
        MYSQL_BIND& b = stmt->bind[c];
        Cell const& cell = cells[c];
        if (b.error)
            *b.error = false;

        if (cell.kind() == K_NULL)
        {
            if (b.is_null) *b.is_null = true;
            if (b.length) *b.length = 0;
            continue;
        }
        if (b.is_null)
            *b.is_null = false;

        switch (b.buffer_type)
        {
            case MYSQL_TYPE_TINY: { int8_t v = int8_t(s->rows.as_int(cell)); memcpy(b.buffer, &v, 1); if (b.length) *b.length = 1; break; }
            case MYSQL_TYPE_SHORT: case MYSQL_TYPE_YEAR: { int16_t v = int16_t(s->rows.as_int(cell)); memcpy(b.buffer, &v, 2); if (b.length) *b.length = 2; break; }
            case MYSQL_TYPE_LONG: case MYSQL_TYPE_INT24: { int32_t v = int32_t(s->rows.as_int(cell)); memcpy(b.buffer, &v, 4); if (b.length) *b.length = 4; break; }
            case MYSQL_TYPE_LONGLONG: case MYSQL_TYPE_BIT: { int64_t v = s->rows.as_int(cell); memcpy(b.buffer, &v, 8); if (b.length) *b.length = 8; break; }
            case MYSQL_TYPE_FLOAT: { float v = float(s->rows.as_real(cell)); memcpy(b.buffer, &v, 4); if (b.length) *b.length = 4; break; }
            case MYSQL_TYPE_DOUBLE: { double v = s->rows.as_real(cell); memcpy(b.buffer, &v, 8); if (b.length) *b.length = 8; break; }
            case MYSQL_TYPE_DATE: case MYSQL_TYPE_TIME: case MYSQL_TYPE_DATETIME: case MYSQL_TYPE_TIMESTAMP:
            {
                MYSQL_TIME t;
                memset(&t, 0, sizeof(t));
                std::string text = s->rows.text(cell);
                sscanf(text.c_str(), "%u-%u-%u %u:%u:%u", &t.year, &t.month, &t.day, &t.hour, &t.minute, &t.second);
                memcpy(b.buffer, &t, sizeof(t));
                if (b.length) *b.length = sizeof(t);
                break;
            }
            default:   // strings, blobs, decimals: bytes
            {
                std::string v;
                if (cell.kind() == K_TEXT || cell.kind() == K_BLOB)
                {
                    char const* p = s->rows.arena.data() + cell.u.off;
                    size_t len = cell.len();
                    size_t copy = std::min<size_t>(len, b.buffer_length);
                    if (copy) memcpy(b.buffer, p, copy);
                    if (b.length) *b.length = len;
                    if (copy < len) { truncated = true; if (b.error) *b.error = true; }
                }
                else
                {
                    v = s->rows.text(cell);
                    size_t copy = std::min<size_t>(v.size(), b.buffer_length);
                    if (copy) memcpy(b.buffer, v.data(), copy);
                    if (b.length) *b.length = v.size();
                    if (copy < v.size()) { truncated = true; if (b.error) *b.error = true; }
                }
                break;
            }
        }
    }
    return truncated ? MYSQL_DATA_TRUNCATED : 0;
}

bool mysql_stmt_free_result(MYSQL_STMT* stmt)
{
    stmt->impl->rows.clear();
    stmt->impl->cursor = 0;
    stmt->impl->stored = false;
    return false;
}

bool mysql_stmt_close(MYSQL_STMT* stmt)
{
    if (!stmt)
        return false;
    if (stmt->impl)
    {
        if (stmt->impl->st)
            sqlite3_finalize(stmt->impl->st);
        delete stmt->impl;
    }
    delete[] stmt->bind;   // our copy of the bind array; the buffers it points to belong to the caller
    delete stmt;
    return false;
}

unsigned int mysql_stmt_errno(MYSQL_STMT* stmt) { return stmt->impl->err_no; }
const char* mysql_stmt_error(MYSQL_STMT* stmt) { return stmt->impl->err.c_str(); }

}   // extern "C"

// The server changes realm (mod-p2p, World Changes): the world database's change log follows the realm named by the environment (AC_REALM_ID). The changes of the realm that
// is left are taken out of the database, those of the realm that is entered are put in, and the rows both touch are reported, one line "OP<TAB>table<TAB>key,key" each, in
// a string that stays valid until the next call (nullptr: the database has no change log or the switch failed). The world must be idle.
extern "C" char const* mysql_sqlite_changes_switch(char const* database)
{
    static std::string report;
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    report.clear();
    ShimMysql* m = nullptr;
    {
        std::lock_guard<std::mutex> g(g_change_connection_lock);
        auto it = g_change_connection.find(database ? database : "");
        m = it == g_change_connection.end() ? nullptr : it->second;
    }
    if (!m || !m->db)
        return nullptr;
    shim::DatabaseOptions const options = shim::database_options(m->name);
    if (options.changes.empty())
        return nullptr;
    std::string error;
    if (!shim::changelog_switch(m->name, options.changes, m->db, m->session, &report, &error))
    {
        fprintf(stderr, "mysql-sqlite: %s: the change log could not be switched: %s\n", m->name.c_str(), error.c_str());
        return nullptr;
    }
    return report.c_str();
}

// The shared world changes (mod-p2p): `listener` is called with the name of the database and the rows of every commit that goes into a change log, as full images (one line each;
// see rows_of_changeset in src/changelog.cpp), on the thread that committed. A change that the listener's owner applies itself must be recognized by it (a thread-local flag).
extern "C" void mysql_sqlite_changes_listen(void (*listener)(char const* database, char const* rows))
{
    shim::changelog_listen(listener);
}
