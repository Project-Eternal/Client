/* Per-database settings of the shim: the file named by the environment variable MYSQL_SQLITE_CONF, one line per database:
     NAME option option=value ...          # comment
   ${VAR} is replaced by the environment variable VAR (the page sets AC_REALM_ID and the server sets it again when it changes realm: one file serves every
   realm; the file is read at every connection). Options:
     readonly            open the file read-only
     updates=DIR         at the first open in this process, apply the update directories of the database's `updates_include` table with $ = DIR
                         (AzerothCore's updater rules, see mysql_sqlite_apply_updates; the page ships only data/sql/custom/...)
     backup=FILE         the database is a working copy of FILE: at the first open FILE is copied over it (SQLite's backup API), then the working copy
     interval=SECONDS    is written back to FILE every SECONDS (default 60) when it changed, as FILE.new renamed over FILE (never a half-written FILE)
     stream              the file is streamed by the page (read-only, pages fetched on demand); changes stay in memory
     changes=DIR         the change log of a streamed database (replayed at the first open, appended to at every commit)
   The environment variable MYSQL_SQLITE_READONLY (comma separated names) still marks databases read-only. */

#include "shim_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

namespace
{
    std::string expand(std::string const& s)
    {
        std::string out;
        for (size_t i = 0; i < s.size();)
        {
            if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{')
            {
                size_t e = s.find('}', i);
                if (e != std::string::npos)
                {
                    char const* v = getenv(s.substr(i + 2, e - i - 2).c_str());
                    out += v ? v : "";
                    i = e + 1;
                    continue;
                }
            }
            out += s[i++];
        }
        return out;
    }

    std::map<std::string, shim::DatabaseOptions> load()
    {
        std::map<std::string, shim::DatabaseOptions> all;
        if (char const* path = getenv("MYSQL_SQLITE_CONF"))
        {
            std::ifstream in(path);
            std::string line;
            while (std::getline(in, line))
            {
                line = line.substr(0, line.find('#'));
                std::istringstream words(expand(line));
                std::string name, word;
                if (!(words >> name))
                    continue;
                shim::DatabaseOptions& o = all[name];
                while (words >> word)
                {
                    size_t eq = word.find('=');
                    std::string key = word.substr(0, eq), value = eq == std::string::npos ? std::string() : word.substr(eq + 1);
                    if (key == "readonly") o.read_only = true;
                    else if (key == "updates") o.updates_source = value;
                    else if (key == "backup") o.backup = value;
                    else if (key == "interval") o.backup_interval = std::max(1, atoi(value.c_str()));
                    else if (key == "stream") o.stream = true;
                    else if (key == "changes") o.changes = value;
                    else fprintf(stderr, "mysql-sqlite: %s: unknown option %s for %s\n", path, key.c_str(), name.c_str());
                }
            }
        }
        if (char const* list = getenv("MYSQL_SQLITE_READONLY"))
        {
            std::istringstream names(list);
            std::string name;
            while (std::getline(names, name, ','))
                if (!name.empty())
                    all[name].read_only = true;
        }
        return all;
    }
}

namespace shim
{
    // read at every call: ${VAR} follows the environment, which the server changes when it changes realm (AC_REALM_ID)
    DatabaseOptions database_options(std::string const& name)
    {
        std::map<std::string, DatabaseOptions> const all = load();
        auto it = all.find(name);
        return it != all.end() ? it->second : DatabaseOptions();
    }
}
