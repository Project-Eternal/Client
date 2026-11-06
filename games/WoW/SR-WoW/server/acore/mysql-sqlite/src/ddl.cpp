/* MySQL table definitions (CREATE TABLE, ALTER TABLE) on SQLite.

   The tables are written in one canonical form, so databases made from AzerothCore's SQL files and databases converted
   from a MySQL server are alike:
     CREATE TABLE `t` (
       `col` <declared MySQL type, display width dropped: "int unsigned", "varchar(100)", "TEXT" for enum/set/date/time types>[ NOT NULL][ DEFAULT x],
       `id` INTEGER PRIMARY KEY AUTOINCREMENT              (an AUTO_INCREMENT column that is the whole primary key)
       PRIMARY KEY (`a`,`b`)
     )[ WITHOUT ROWID]                                     (tables with a primary key and no AUTO_INCREMENT column)
     CREATE [UNIQUE ]INDEX `t__name` ON `t` (`a`,`b`)
   Foreign keys and CHECK constraints are dropped. The shim types its results by the declared types, which is why they are kept.
   ALTER TABLE changes what SQLite can change in place (indexes) directly and rebuilds the table for everything else (a column added AFTER another
   one, a changed type, a renamed column, another primary key): new definition, copy of the rows, the old indexes again. */

#include "shim_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <set>

namespace
{
    std::atomic<bool> g_nocase{false};

    // ------------------------------------------------------------------------------------------------------------------
    // tokens

    enum Kind { WORD, STR, NUM, PUNCT, END };

    struct Token
    {
        Kind kind;
        std::string text;    // WORD: the name without backticks; STR: the decoded value; NUM, PUNCT: as written
        bool quoted = false; // a `quoted` name
        size_t begin = 0, end = 0;
    };

    std::string lower(std::string s)
    {
        for (char& c : s)
            c = char(tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::vector<Token> tokenize(std::string const& s)
    {
        std::vector<Token> out;
        size_t i = 0, n = s.size();
        while (i < n)
        {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (isspace(c)) { ++i; continue; }
            Token t;
            t.begin = i;
            if (c == '`')
            {
                size_t j = i + 1;
                std::string name;
                while (j < n)
                {
                    if (s[j] == '`') { if (j + 1 < n && s[j + 1] == '`') { name += '`'; j += 2; continue; } break; }
                    name += s[j++];
                }
                t.kind = WORD; t.text = name; t.quoted = true; i = std::min(n, j + 1);
            }
            else if (c == '\'' || c == '"')
            {
                char q = char(c);
                size_t j = i + 1;
                std::string value;
                while (j < n)
                {
                    if (s[j] == '\\' && j + 1 < n)
                    {
                        char e = s[j + 1];
                        switch (e)
                        {
                            case 'n': value += '\n'; break;
                            case 'r': value += '\r'; break;
                            case 't': value += '\t'; break;
                            case '0': value += '\0'; break;
                            case 'b': value += '\b'; break;
                            case 'Z': value += '\x1a'; break;
                            default: value += e; break;
                        }
                        j += 2;
                        continue;
                    }
                    if (s[j] == q) { if (j + 1 < n && s[j + 1] == q) { value += q; j += 2; continue; } break; }
                    value += s[j++];
                }
                t.kind = STR; t.text = value; i = std::min(n, j + 1);
            }
            else if (isdigit(c) || (c == '.' && i + 1 < n && isdigit(static_cast<unsigned char>(s[i + 1]))))
            {
                size_t j = i;
                while (j < n && (isalnum(static_cast<unsigned char>(s[j])) || s[j] == '.' || ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E'))))
                    ++j;
                t.kind = NUM; t.text = s.substr(i, j - i); i = j;
            }
            else if (isalpha(c) || c == '_' || c == '$' || c >= 0x80)
            {
                size_t j = i;
                while (j < n && (isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '$' || static_cast<unsigned char>(s[j]) >= 0x80))
                    ++j;
                t.kind = WORD; t.text = s.substr(i, j - i); i = j;
            }
            else
            {
                t.kind = PUNCT; t.text = std::string(1, char(c)); ++i;
            }
            t.end = i;
            out.push_back(t);
        }
        Token e;
        e.kind = END;
        e.begin = e.end = n;
        out.push_back(e);
        return out;
    }

    struct Parser
    {
        std::vector<Token> t;
        size_t p = 0;

        Token const& peek(size_t k = 0) const { return t[std::min(p + k, t.size() - 1)]; }
        bool is(char const* word, size_t k = 0) const { Token const& x = peek(k); return x.kind == WORD && !x.quoted && lower(x.text) == word; }
        bool punct(char ch, size_t k = 0) const { Token const& x = peek(k); return x.kind == PUNCT && x.text[0] == ch; }
        bool accept(char const* word) { if (is(word)) { ++p; return true; } return false; }
        bool accept_punct(char ch) { if (punct(ch)) { ++p; return true; } return false; }
        bool at_end() const { return peek().kind == END || punct(';'); }
        Token const& next() { Token const& x = peek(); if (p < t.size() - 1) ++p; return x; }
        // skips a balanced ( ... )
        void skip_parens()
        {
            if (!punct('('))
                return;
            int depth = 0;
            do
            {
                if (punct('(')) ++depth;
                else if (punct(')')) --depth;
                next();
            } while (depth > 0 && peek().kind != END);
        }
        // a name list ( `a`, `b`(10) ASC, ... )
        std::vector<std::string> name_list()
        {
            std::vector<std::string> names;
            if (!accept_punct('('))
                return names;
            while (!punct(')') && peek().kind != END)
            {
                Token const& x = next();
                if (x.kind == WORD)
                    names.push_back(x.text);
                if (punct('(')) skip_parens();          // a prefix length
                while (is("asc") || is("desc")) next();
                accept_punct(',');
            }
            accept_punct(')');
            return names;
        }
    };

    // ------------------------------------------------------------------------------------------------------------------
    // the table model

    struct Column
    {
        std::string name;
        std::string rest;        // the canonical definition after the name, without AUTO_INCREMENT: "int unsigned NOT NULL DEFAULT 0"
        bool auto_inc = false;
        bool numeric = false;    // for the implicit default of a NOT NULL column without one
        bool not_null = false;
        bool has_default = false;
    };

    struct Index
    {
        std::string name;        // MySQL's name (the SQLite index is TABLE__NAME)
        bool unique = false;
        std::vector<std::string> cols;
    };

    struct Table
    {
        std::string name;
        std::vector<Column> cols;
        std::vector<std::string> pk;
        std::vector<Index> indexes;

        int find(std::string const& col) const
        {
            for (size_t i = 0; i < cols.size(); ++i)
                if (lower(cols[i].name) == lower(col))
                    return int(i);
            return -1;
        }
    };

    std::string quote_name(std::string const& n)
    {
        std::string r = "`";
        for (char c : n) { if (c == '`') r += '`'; r += c; }
        return r + "`";
    }

    std::string quote_text(std::string const& v)
    {
        std::string r = "'";
        for (char c : v) { if (c == '\'') r += '\''; r += c; }
        return r + "'";
    }

    std::string name_list_sql(std::vector<std::string> const& names)
    {
        std::string r;
        for (size_t i = 0; i < names.size(); ++i)
            r += (i ? "," : "") + quote_name(names[i]);
        return r;
    }

    std::string create_table_sql(Table const& t)
    {
        bool has_auto = false;
        std::string auto_name;
        for (Column const& c : t.cols)
            if (c.auto_inc && !has_auto) { has_auto = true; auto_name = c.name; }
        bool integer_pk = has_auto && t.pk.size() == 1 && lower(t.pk[0]) == lower(auto_name);

        std::vector<std::string> lines;
        for (Column const& c : t.cols)
        {
            if (integer_pk && lower(c.name) == lower(auto_name))
                lines.push_back(quote_name(c.name) + " INTEGER PRIMARY KEY AUTOINCREMENT");
            else
                lines.push_back(quote_name(c.name) + " " + (c.rest.empty() ? std::string("int unsigned NOT NULL") : c.rest));
        }
        if (!t.pk.empty() && !integer_pk)
            lines.push_back("PRIMARY KEY (" + name_list_sql(t.pk) + ")");
        std::string sql = "CREATE TABLE " + quote_name(t.name) + " (\n  ";
        for (size_t i = 0; i < lines.size(); ++i)
            sql += (i ? ",\n  " : "") + lines[i];
        sql += "\n)";
        if (!t.pk.empty() && !has_auto)
            sql += " WITHOUT ROWID";
        return sql;
    }

    std::string create_index_sql(std::string const& table, Index const& x)
    {
        return std::string("CREATE ") + (x.unique ? "UNIQUE " : "") + "INDEX " + quote_name(table + "__" + x.name) + " ON " + quote_name(table) + " (" + name_list_sql(x.cols) + ")";
    }

    // ------------------------------------------------------------------------------------------------------------------
    // column definitions

    bool numeric_type(std::string const& base)
    {
        static char const* const names[] = {"tinyint", "smallint", "mediumint", "int", "integer", "bigint", "float", "double", "real", "decimal", "numeric", "dec", "fixed", "bit", "bool", "boolean", "serial"};
        for (char const* n : names)
            if (base == n)
                return true;
        return false;
    }

    bool is_number(std::string const& s)
    {
        // written unquoted: -?\d+(\.\d+)?
        size_t i = 0;
        if (i < s.size() && s[i] == '-') ++i;
        size_t digits = i;
        while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i == digits) return false;
        if (i < s.size() && s[i] == '.')
        {
            size_t f = ++i;
            while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) ++i;
            if (i == f) return false;
        }
        return i == s.size();
    }

    // MySQL normalizes the numeric default of a column to the column's type ('0.0' -> 0, '1.50' -> 1.5); MariaDB's information_schema then shows that
    std::string normalize_number(std::string v, bool integral)
    {
        if (!is_number(v))
            return v;
        if (integral)
        {
            size_t dot = v.find('.');
            if (dot != std::string::npos) v.resize(dot);
            if (v == "-0") v = "0";
            return v;
        }
        size_t dot = v.find('.');
        if (dot != std::string::npos)
        {
            while (!v.empty() && v.back() == '0') v.pop_back();
            if (!v.empty() && v.back() == '.') v.pop_back();
        }
        if (v == "-0") v = "0";
        return v;
    }

    struct ColumnParse
    {
        Column col;
        bool inline_pk = false;
        bool inline_unique = false;
        std::string position;    // "" (append), "\x01FIRST" or the name of the column to follow
    };

    // `name` TYPE[(args)] [attributes...] up to the ',' or ')' that ends it; table_ci: the table's default collation compares case-insensitively
    bool parse_column(Parser& ps, bool table_ci, ColumnParse* out, std::string* error)
    {
        Token const& nameTok = ps.next();
        if (nameTok.kind != WORD) { *error = "column name expected"; return false; }
        out->col.name = nameTok.text;

        Token const& typeTok = ps.next();
        if (typeTok.kind != WORD) { *error = "type expected for column " + out->col.name; return false; }
        std::string base = lower(typeTok.text);
        if (base == "double" && ps.is("precision")) ps.next();
        if ((base == "character" || base == "char") && ps.is("varying")) { ps.next(); base = "varchar"; }
        if (base == "national") { base = lower(ps.next().text); }
        std::string args;    // "(100)" "(10,2)" as written (enum lists are skipped: enum is TEXT)
        if (ps.punct('('))
        {
            size_t from = ps.p;
            ps.skip_parens();
            for (size_t k = from; k < ps.p; ++k)
            {
                Token const& x = ps.t[k];
                args += x.kind == STR ? quote_text(x.text) : x.text;
            }
        }

        bool is_unsigned = false, not_null = false, explicit_null = false, auto_inc = false, has_default = false, ci = table_ci, collate_given = false;
        Token defaultTok;
        bool default_negative = false, default_now = false, default_null = false;
        while (!ps.at_end() && !ps.punct(',') && !ps.punct(')'))
        {
            if (ps.accept("unsigned")) { is_unsigned = true; continue; }
            if (ps.accept("signed") || ps.accept("zerofill") || ps.accept("binary")) continue;
            if (ps.is("character") && ps.is("set", 1)) { ps.next(); ps.next(); ps.next(); continue; }
            if (ps.accept("charset")) { ps.next(); continue; }
            if (ps.accept("collate"))
            {
                std::string coll = lower(ps.next().text);
                ci = coll.size() > 3 && coll.compare(coll.size() - 3, 3, "_ci") == 0;
                collate_given = true;
                continue;
            }
            if (ps.is("not") && ps.is("null", 1)) { ps.next(); ps.next(); not_null = true; continue; }
            if (ps.accept("null")) { explicit_null = true; continue; }
            if (ps.accept("default"))
            {
                has_default = true;
                if (ps.punct('-')) { ps.next(); default_negative = true; }
                if (ps.punct('('))
                {
                    // an expression default (MySQL 8): only (CURRENT_TIMESTAMP) style ones are meaningful here
                    size_t from = ps.p;
                    ps.skip_parens();
                    std::string inner;
                    for (size_t k = from; k < ps.p; ++k) inner += lower(ps.t[k].text);
                    default_now = inner.find("current_timestamp") != std::string::npos || inner.find("now") != std::string::npos;
                    continue;
                }
                defaultTok = ps.next();
                std::string w = lower(defaultTok.text);
                if (defaultTok.kind == WORD && (w == "current_timestamp" || w == "now" || w == "localtime" || w == "localtimestamp"))
                {
                    default_now = true;
                    if (ps.punct('(')) ps.skip_parens();
                }
                else if (defaultTok.kind == WORD && w == "null")
                    default_null = true;
                else if (defaultTok.kind == WORD && (w == "b" || w == "x" || w == "_binary") && ps.peek().kind == STR)
                    defaultTok = ps.next();
                continue;
            }
            if (ps.accept("auto_increment")) { auto_inc = true; continue; }
            if (ps.is("on") && ps.is("update", 1))
            {
                ps.next(); ps.next(); ps.next();
                if (ps.punct('(')) ps.skip_parens();
                continue;
            }
            if (ps.accept("comment")) { ps.next(); continue; }
            if (ps.is("primary") && ps.is("key", 1)) { ps.next(); ps.next(); out->inline_pk = true; continue; }
            if (ps.accept("unique")) { ps.accept("key"); out->inline_unique = true; continue; }
            if (ps.accept("key")) { out->inline_pk = true; continue; }
            if (ps.accept("first")) { out->position = "\x01FIRST"; continue; }
            if (ps.accept("after")) { out->position = ps.next().text; continue; }
            if (ps.accept("check") || ps.accept("generated") || ps.accept("as")) { if (ps.punct('(')) ps.skip_parens(); continue; }
            if (ps.accept("column_format") || ps.accept("storage") || ps.accept("srid")) { ps.next(); continue; }
            if (ps.accept("references"))
            {
                ps.next();
                if (ps.punct('(')) ps.skip_parens();
                while (ps.is("on") || ps.is("match")) { ps.next(); ps.next(); if (ps.is("null") || ps.is("action") || ps.is("default")) ps.next(); }
                continue;
            }
            ps.next();   // VISIBLE, INVISIBLE, STORED, VIRTUAL ...
        }
        (void)explicit_null;

        // the declared type as the canonical form keeps it
        if (base == "integer") base = "int";
        if (base == "bool" || base == "boolean") base = "tinyint";
        if (base == "real") base = "double";
        if (base == "dec" || base == "numeric" || base == "fixed") base = "decimal";
        std::string type;
        bool numeric = numeric_type(base);
        bool text_like = false;
        if (base == "enum" || base == "set" || base == "date" || base == "datetime" || base == "timestamp" || base == "time" || base == "year")
        {
            type = "TEXT";
            text_like = base == "enum" || base == "set";
            numeric = base == "year";
        }
        else if (base == "tinyint" || base == "smallint" || base == "mediumint" || base == "int" || base == "bigint" || base == "float" || base == "double")
            type = base + (is_unsigned ? " unsigned" : "");
        else if (base == "decimal")
            type = "decimal" + (args.empty() ? std::string("(10,0)") : args) + (is_unsigned ? " unsigned" : "");
        else if (base == "char" || base == "binary")
            type = base + (args.empty() ? std::string("(1)") : args);
        else if (base == "json")
            type = "longtext";
        else
            type = base + args;   // varchar(100), varbinary(16), text, longtext, blob, bit(1) ...
        text_like = text_like || base == "char" || base == "varchar" || base.find("text") != std::string::npos;

        bool integral = base == "tinyint" || base == "smallint" || base == "mediumint" || base == "int" || base == "bigint" || base == "year" || base == "bit";
        std::string rest = type;
        if (not_null || auto_inc)
            rest += " NOT NULL";
        if (default_now)
            rest += " DEFAULT CURRENT_TIMESTAMP";
        else if (has_default && !default_null)
        {
            std::string v = (default_negative ? "-" : "") + defaultTok.text;
            if (numeric && (defaultTok.kind == NUM || defaultTok.kind == STR))
            {
                v = normalize_number(v, integral);
                rest += is_number(v) ? " DEFAULT " + v : " DEFAULT " + quote_text(v);
            }
            else
                rest += " DEFAULT " + quote_text(v);
        }
        else if (!not_null && !auto_inc)
            rest += " DEFAULT NULL";
        if (g_nocase && text_like && ci && (collate_given || table_ci))
            rest += " COLLATE NOCASE";

        out->col.rest = rest;
        out->col.auto_inc = auto_inc;
        out->col.numeric = numeric;
        out->col.not_null = not_null || auto_inc;
        out->col.has_default = has_default || default_now;
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // reading a table back from the database (its canonical CREATE TABLE text and its indexes)

    bool exec(sqlite3* db, std::string const& sql, std::string* error)
    {
        char* msg = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK)
        {
            *error = std::string(msg ? msg : "error") + " in: " + sql.substr(0, 300);
            sqlite3_free(msg);
            return false;
        }
        return true;
    }

    bool table_exists(sqlite3* db, std::string const& name)
    {
        sqlite3_stmt* st = nullptr;
        bool found = false;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 COLLATE NOCASE", -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            found = sqlite3_step(st) == SQLITE_ROW;
            sqlite3_finalize(st);
        }
        return found;
    }

    bool load_table(sqlite3* db, std::string const& name, Table* t, std::string* error)
    {
        sqlite3_stmt* st = nullptr;
        std::string sql;
        if (sqlite3_prepare_v2(db, "SELECT name, sql FROM sqlite_master WHERE type='table' AND name=?1 COLLATE NOCASE", -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW)
            {
                t->name = reinterpret_cast<char const*>(sqlite3_column_text(st, 0));
                sql = reinterpret_cast<char const*>(sqlite3_column_text(st, 1));
            }
            sqlite3_finalize(st);
        }
        if (sql.empty()) { *error = "no such table: " + name; return false; }

        // the canonical form: "CREATE TABLE `t` (\n  line,\n  line\n)[ WITHOUT ROWID]"
        size_t open = sql.find("(\n  "), close = sql.rfind("\n)");
        if (open == std::string::npos || close == std::string::npos || close < open)
        {
            *error = "table " + name + " was not created by the shim (its definition cannot be changed): " + sql.substr(0, 200);
            return false;
        }
        std::string body = sql.substr(open + 4, close - open - 4);
        size_t from = 0;
        while (from <= body.size())
        {
            size_t to = body.find(",\n  ", from);
            std::string line = body.substr(from, to == std::string::npos ? std::string::npos : to - from);
            from = to == std::string::npos ? body.size() + 1 : to + 4;
            if (line.rfind("PRIMARY KEY (", 0) == 0)
            {
                Parser ps;
                ps.t = tokenize(line.substr(12));
                t->pk = ps.name_list();
                continue;
            }
            Parser ps;
            ps.t = tokenize(line);
            Column c;
            c.name = ps.next().text;
            c.rest = line.substr(ps.peek().begin);
            if (c.rest == "INTEGER PRIMARY KEY AUTOINCREMENT")
            {
                c.auto_inc = true;
                c.rest.clear();
                c.numeric = true;
                c.not_null = true;
                t->pk = {c.name};
            }
            else
            {
                std::string low = lower(c.rest);
                c.not_null = low.find(" not null") != std::string::npos;
                c.has_default = low.find(" default ") != std::string::npos;
                std::string base = low.substr(0, low.find_first_of(" ("));
                c.numeric = numeric_type(base);
            }
            t->cols.push_back(c);
        }

        if (sqlite3_prepare_v2(db, "SELECT name, sql FROM sqlite_master WHERE type='index' AND tbl_name=?1 COLLATE NOCASE AND sql IS NOT NULL ORDER BY rowid", -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, t->name.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                std::string iname = reinterpret_cast<char const*>(sqlite3_column_text(st, 0));
                std::string isql = reinterpret_cast<char const*>(sqlite3_column_text(st, 1));
                Index x;
                std::string prefix = t->name + "__";
                x.name = iname.rfind(prefix, 0) == 0 ? iname.substr(prefix.size()) : iname;
                x.unique = isql.rfind("CREATE UNIQUE", 0) == 0;
                Parser ps;
                ps.t = tokenize(isql.substr(isql.rfind('(')));
                x.cols = ps.name_list();
                t->indexes.push_back(x);
            }
            sqlite3_finalize(st);
        }
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // CREATE TABLE

    // the table-level entries of a definition: PRIMARY KEY (..), [UNIQUE|FULLTEXT|SPATIAL] KEY|INDEX [name] (..), CONSTRAINT ... (dropped)
    bool parse_table_entry(Parser& ps, Table* t, bool* was_entry)
    {
        *was_entry = true;
        if (ps.is("constraint"))
        {
            ps.next();
            if (!ps.is("primary") && !ps.is("unique") && !ps.is("foreign") && !ps.is("check")) ps.next();   // its name
        }
        if (ps.is("primary") && ps.is("key", 1))
        {
            ps.next(); ps.next();
            if (ps.is("using")) { ps.next(); ps.next(); }
            t->pk = ps.name_list();
            while (!ps.at_end() && !ps.punct(',') && !ps.punct(')')) ps.next();   // USING BTREE, COMMENT '...'
            return true;
        }
        bool unique = false;
        if (ps.accept("unique")) { unique = true; if (!ps.accept("key")) ps.accept("index"); }
        else if (ps.accept("fulltext") || ps.accept("spatial")) { if (!ps.accept("key")) ps.accept("index"); }
        else if (!ps.accept("key") && !ps.accept("index"))
        {
            if (ps.accept("foreign") || ps.accept("check"))
            {
                while (!ps.at_end() && !ps.punct(',') && !ps.punct(')'))
                {
                    if (ps.punct('(')) ps.skip_parens();
                    else ps.next();
                }
                return true;
            }
            *was_entry = false;
            return true;
        }
        Index x;
        x.unique = unique;
        if (!ps.punct('(')) x.name = ps.next().text;
        if (ps.is("using")) { ps.next(); ps.next(); }
        x.cols = ps.name_list();
        if (x.name.empty() && !x.cols.empty()) x.name = x.cols[0];
        while (!ps.at_end() && !ps.punct(',') && !ps.punct(')')) ps.next();   // USING BTREE, COMMENT '...'
        t->indexes.push_back(x);
        return true;
    }

    bool create_table(sqlite3* db, Parser& ps, std::string* error)
    {
        // CREATE [TEMPORARY] TABLE [IF NOT EXISTS] name ( ... ) options
        ps.accept("temporary");
        ps.accept("table");
        bool if_not_exists = false;
        if (ps.is("if") && ps.is("not", 1) && ps.is("exists", 2)) { ps.next(); ps.next(); ps.next(); if_not_exists = true; }
        Table t;
        t.name = ps.next().text;
        if (ps.punct('.')) { ps.next(); t.name = ps.next().text; }   // db.table
        if (table_exists(db, t.name))
        {
            if (if_not_exists)
                return true;
            *error = "table " + t.name + " already exists";
            return false;
        }
        if (ps.is("like")) { *error = "CREATE TABLE ... LIKE is not supported"; return false; }

        // the table's default collation is at the end: find it first (it decides the columns that have no COLLATE of their own)
        bool table_ci = true;   // AzerothCore's tables: utf8mb4_unicode_ci / general_ci / 0900_ai_ci
        {
            Parser look = ps;
            look.skip_parens();
            while (!look.at_end())
            {
                if (look.accept("collate")) { look.accept_punct('='); std::string c = lower(look.next().text); table_ci = c.size() > 3 && c.compare(c.size() - 3, 3, "_ci") == 0; }
                else look.next();
            }
        }

        if (!ps.accept_punct('(')) { *error = "( expected after CREATE TABLE " + t.name; return false; }
        while (!ps.punct(')') && !ps.at_end())
        {
            bool was_entry = false;
            parse_table_entry(ps, &t, &was_entry);
            if (!was_entry)
            {
                ColumnParse c;
                if (!parse_column(ps, table_ci, &c, error))
                    return false;
                if (c.inline_pk) t.pk = {c.col.name};
                if (c.inline_unique) t.indexes.push_back(Index{c.col.name, true, {c.col.name}});
                t.cols.push_back(c.col);
            }
            ps.accept_punct(',');
        }
        if (!ps.accept_punct(')')) { *error = "unterminated CREATE TABLE " + t.name; return false; }
        if (!exec(db, create_table_sql(t), error))
            return false;
        for (Index const& x : t.indexes)
            if (!exec(db, create_index_sql(t.name, x), error))
                return false;
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // ALTER TABLE

    // the implicit value MySQL gives an existing row for a new NOT NULL column without a default
    std::string implicit_default(Column const& c)
    {
        return c.numeric ? "0" : "''";
    }

    bool rebuild(sqlite3* db, Table const& before, Table const& after, std::map<std::string, std::string> const& source_of, std::string* error)
    {
        // source_of: new column name -> old column name (renamed, kept); columns not in it are new
        std::string tmp = "__shim_rebuild_" + after.name;
        std::string into, from;
        for (Column const& c : after.cols)
        {
            auto it = source_of.find(lower(c.name));
            if (it != source_of.end())
            {
                into += (into.empty() ? "" : ",") + quote_name(c.name);
                from += (from.empty() ? "" : ",") + quote_name(it->second);
            }
            else if (c.not_null && !c.has_default && !c.auto_inc)
            {
                into += (into.empty() ? "" : ",") + quote_name(c.name);
                from += (from.empty() ? "" : ",") + implicit_default(c);
            }
        }
        if (!exec(db, "CREATE TEMP TABLE " + quote_name(tmp) + " AS SELECT * FROM " + quote_name(before.name), error)) return false;
        if (!exec(db, "DROP TABLE " + quote_name(before.name), error)) return false;
        if (!exec(db, create_table_sql(after), error)) return false;
        if (!into.empty() && !exec(db, "INSERT INTO " + quote_name(after.name) + " (" + into + ") SELECT " + from + " FROM temp." + quote_name(tmp), error)) return false;
        if (!exec(db, "DROP TABLE temp." + quote_name(tmp), error)) return false;
        for (Index const& x : after.indexes)
            if (!exec(db, create_index_sql(after.name, x), error))
                return false;
        return true;
    }

    void place(Table& t, Column const& c, std::string const& position, int replace_at)
    {
        if (replace_at >= 0)
            t.cols.erase(t.cols.begin() + replace_at);
        size_t at = replace_at >= 0 && position.empty() ? size_t(replace_at) : t.cols.size();
        if (position == "\x01FIRST") at = 0;
        else if (!position.empty())
        {
            int a = t.find(position);
            if (a >= 0) at = size_t(a) + 1;
        }
        t.cols.insert(t.cols.begin() + std::min(at, t.cols.size()), c);
    }

    bool alter_table(sqlite3* db, Parser& ps, std::string* error)
    {
        ps.accept("ignore");
        ps.accept("table");
        std::string name = ps.next().text;
        if (ps.punct('.')) { ps.next(); name = ps.next().text; }
        Table before;
        if (!load_table(db, name, &before, error))
            return false;
        Table t = before;
        std::map<std::string, std::string> source_of;   // lower(new name) -> old name
        for (Column const& c : t.cols)
            source_of[lower(c.name)] = c.name;
        bool needs_rebuild = false;
        std::vector<std::string> drop_indexes;
        std::vector<Index> add_indexes;
        std::string rename_to;

        auto drop_column_from_indexes = [&](std::string const& col) {
            for (Index& x : t.indexes)
                x.cols.erase(std::remove_if(x.cols.begin(), x.cols.end(), [&](std::string const& c) { return lower(c) == lower(col); }), x.cols.end());
            t.indexes.erase(std::remove_if(t.indexes.begin(), t.indexes.end(), [](Index const& x) { return x.cols.empty(); }), t.indexes.end());
            t.pk.erase(std::remove_if(t.pk.begin(), t.pk.end(), [&](std::string const& c) { return lower(c) == lower(col); }), t.pk.end());
        };
        auto rename_in_indexes = [&](std::string const& from, std::string const& to) {
            for (Index& x : t.indexes)
                for (std::string& c : x.cols)
                    if (lower(c) == lower(from)) c = to;
            for (std::string& c : t.pk)
                if (lower(c) == lower(from)) c = to;
        };

        while (!ps.at_end())
        {
            if (ps.accept("add"))
            {
                bool was_entry = false;
                Table entries;
                if (ps.is("column")) ps.next();
                else parse_table_entry(ps, &entries, &was_entry);
                if (was_entry)
                {
                    if (!entries.pk.empty()) { t.pk = entries.pk; needs_rebuild = true; }
                    for (Index const& x : entries.indexes) add_indexes.push_back(x);
                }
                else
                {
                    ColumnParse c;
                    if (!parse_column(ps, true, &c, error))
                        return false;
                    if (t.find(c.col.name) >= 0) { *error = "duplicate column " + c.col.name; return false; }
                    place(t, c.col, c.position, -1);
                    if (c.inline_pk) t.pk = {c.col.name};
                    needs_rebuild = true;
                }
            }
            else if (ps.accept("drop"))
            {
                if (ps.is("primary") && ps.is("key", 1)) { ps.next(); ps.next(); t.pk.clear(); needs_rebuild = true; }
                else if (ps.accept("index") || ps.accept("key")) drop_indexes.push_back(ps.next().text);
                else if (ps.accept("foreign") || ps.accept("check") || ps.accept("constraint")) { ps.accept("key"); ps.next(); }
                else
                {
                    ps.accept("column");
                    std::string col = ps.next().text;
                    int at = t.find(col);
                    if (at < 0) { *error = "no such column " + col + " in " + name; return false; }
                    t.cols.erase(t.cols.begin() + at);
                    source_of.erase(lower(col));
                    drop_column_from_indexes(col);
                    needs_rebuild = true;
                }
            }
            else if (ps.accept("change") || ps.accept("modify"))
            {
                bool change = lower(ps.t[ps.p - 1].text) == "change";
                ps.accept("column");
                std::string old_name = change ? ps.next().text : ps.peek().text;
                int at = t.find(old_name);
                if (at < 0) { *error = "no such column " + old_name + " in " + name; return false; }
                ColumnParse c;
                if (!parse_column(ps, true, &c, error))
                    return false;
                std::string old_real = t.cols[size_t(at)].name;
                auto src = source_of.find(lower(old_real));
                std::string origin = src != source_of.end() ? src->second : old_real;
                source_of.erase(lower(old_real));
                source_of[lower(c.col.name)] = origin;
                if (lower(old_real) != lower(c.col.name))
                    rename_in_indexes(old_real, c.col.name);
                place(t, c.col, c.position, at);
                if (c.inline_pk) t.pk = {c.col.name};
                needs_rebuild = true;
            }
            else if (ps.accept("rename"))
            {
                if (ps.accept("column"))
                {
                    std::string from = ps.next().text;
                    ps.accept("to");
                    std::string to = ps.next().text;
                    int at = t.find(from);
                    if (at < 0) { *error = "no such column " + from + " in " + name; return false; }
                    auto src = source_of.find(lower(from));
                    std::string origin = src != source_of.end() ? src->second : from;
                    source_of.erase(lower(from));
                    source_of[lower(to)] = origin;
                    t.cols[size_t(at)].name = to;
                    rename_in_indexes(from, to);
                    needs_rebuild = true;
                }
                else if (ps.accept("index") || ps.accept("key"))
                {
                    std::string from = ps.next().text;
                    ps.accept("to");
                    std::string to = ps.next().text;
                    for (Index& x : t.indexes)
                        if (lower(x.name) == lower(from)) { drop_indexes.push_back(x.name); Index y = x; y.name = to; add_indexes.push_back(y); }
                }
                else
                {
                    if (!ps.accept("to")) ps.accept("as");
                    rename_to = ps.next().text;
                }
            }
            else
            {
                // ENGINE=, [DEFAULT] CHARSET=, COLLATE=, COMMENT=, AUTO_INCREMENT=, ROW_FORMAT=, CONVERT TO CHARACTER SET ..., ALGORITHM=, LOCK=,
                // DISABLE/ENABLE KEYS, FORCE: nothing to do in SQLite
                while (!ps.at_end() && !ps.punct(','))
                {
                    if (ps.punct('(')) ps.skip_parens();
                    else ps.next();
                }
            }
            if (!ps.accept_punct(','))
                break;
        }

        // the index changes on the model (a rebuild recreates t.indexes)
        for (std::string const& d : drop_indexes)
        {
            auto it = std::find_if(t.indexes.begin(), t.indexes.end(), [&](Index const& x) { return lower(x.name) == lower(d); });
            if (it == t.indexes.end()) { *error = "no such index " + d + " on " + name; return false; }
            t.indexes.erase(it);
        }
        for (Index const& x : add_indexes)
            t.indexes.push_back(x);

        if (needs_rebuild)
        {
            if (!rebuild(db, before, t, source_of, error))
                return false;
        }
        else
        {
            for (std::string const& d : drop_indexes)
                if (!exec(db, "DROP INDEX " + quote_name(before.name + "__" + d), error))
                    return false;
            for (Index const& x : add_indexes)
                if (!exec(db, create_index_sql(t.name, x), error))
                    return false;
        }
        if (!rename_to.empty())
        {
            // a renamed table: definition and indexes under the new name (SQLite's own RENAME would keep the old index names)
            Table renamed;
            if (!load_table(db, t.name, &renamed, error))
                return false;
            Table target = renamed;
            target.name = rename_to;
            std::map<std::string, std::string> same;
            for (Column const& c : renamed.cols)
                same[lower(c.name)] = c.name;
            Table source = renamed;
            if (!exec(db, create_table_sql(target), error)) return false;
            if (!exec(db, "INSERT INTO " + quote_name(rename_to) + " SELECT * FROM " + quote_name(source.name), error)) return false;
            if (!exec(db, "DROP TABLE " + quote_name(source.name), error)) return false;
            for (Index const& x : target.indexes)
                if (!exec(db, create_index_sql(rename_to, x), error))
                    return false;
        }
        return true;
    }

    // DROP INDEX name ON table (MySQL's form; SQLite's DROP INDEX has no ON)
    bool drop_index(sqlite3* db, Parser& ps, std::string* error)
    {
        ps.accept("index");
        std::string index = ps.next().text;
        if (!ps.accept("on")) { *error = "DROP INDEX without ON"; return false; }
        std::string table = ps.next().text;
        return exec(db, "DROP INDEX " + quote_name(table + "__" + index), error);
    }

    // CREATE [UNIQUE] INDEX name ON table (cols)
    bool create_index(sqlite3* db, Parser& ps, std::string* error)
    {
        Index x;
        x.unique = ps.accept("unique");
        ps.accept("fulltext");
        ps.accept("index");
        x.name = ps.next().text;
        if (ps.is("using")) { ps.next(); ps.next(); }
        if (!ps.accept("on")) { *error = "CREATE INDEX without ON"; return false; }
        std::string table = ps.next().text;
        x.cols = ps.name_list();
        return exec(db, create_index_sql(table, x), error);
    }
}

namespace shim
{
    void set_nocase_collation(bool on) { g_nocase = on; }

    bool run_ddl(sqlite3* db, std::string const& statement, bool* handled, std::string* error)
    {
        *handled = false;
        Parser ps;
        ps.t = tokenize(statement);
        if (ps.is("create"))
        {
            if (ps.is("table", 1) || (ps.is("temporary", 1) && ps.is("table", 2)))
            {
                *handled = true;
                ps.next();
                return create_table(db, ps, error);
            }
            if (ps.is("index", 1) || (ps.is("unique", 1) && ps.is("index", 2)) || (ps.is("fulltext", 1) && ps.is("index", 2)))
            {
                *handled = true;
                ps.next();
                return create_index(db, ps, error);
            }
            return true;
        }
        if (ps.is("alter") && ps.is("table", 1))
        {
            *handled = true;
            ps.next();
            return alter_table(db, ps, error);
        }
        if (ps.is("alter") && ps.is("ignore", 1) && ps.is("table", 2))
        {
            *handled = true;
            ps.next();
            return alter_table(db, ps, error);
        }
        if (ps.is("drop") && ps.is("index", 1))
        {
            // only MySQL's form (with ON); SQLite's own form goes through unchanged
            bool mysql_form = false;
            for (Token const& x : ps.t)
                mysql_form = mysql_form || (x.kind == WORD && !x.quoted && lower(x.text) == "on");
            if (mysql_form)
            {
                *handled = true;
                ps.next();
                return drop_index(db, ps, error);
            }
        }
        return true;
    }
}
