/* Builds AzerothCore's databases as SQLite files from AzerothCore's own SQL (what its updater does on a MySQL server with Updates.AutoSetup), through
   the shim: no MySQL server, no conversion step.
   usage: acore-db-setup SOURCE OUTDIR [--skip-locales] [--nocase] [auth|characters|world ...]
     SOURCE   the AzerothCore tree (data/sql/base/db_NAME/*.sql, then the update directories listed in each database's `updates_include`)
     OUTDIR   gets NAME.sqlite (replaced)
     --skip-locales  the rows of the *_locale tables are left out (a client with the default locale never reads them: more than half of the world)
     --nocase        text columns with a case-insensitive MySQL collation (utf8mb4_unicode_ci ...) compare case-insensitively (COLLATE NOCASE) */

#include "mysql_sqlite.h"
#include "../src/shim_internal.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int skip_locale_rows(const char* statement, void*)
{
    // INSERT INTO `something_locale` ...
    const char* p = statement;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
    if (strncasecmp(p, "INSERT", 6) != 0 && strncasecmp(p, "REPLACE", 7) != 0)
        return 0;
    const char* into = strcasestr(p, "INTO");
    if (!into)
        return 0;
    into += 4;
    while (*into == ' ' || *into == '`') ++into;
    const char* end = into;
    while (*end && *end != '`' && *end != ' ' && *end != '(') ++end;
    std::string table(into, size_t(end - into));
    return table.size() > 7 && table.compare(table.size() - 7, 7, "_locale") == 0;
}

static bool run(MYSQL* m, const char* sql)
{
    if (mysql_query(m, sql) == 0)
        return true;
    fprintf(stderr, "acore-db-setup: %s: %s\n", sql, mysql_error(m));
    return false;
}

static bool build(std::string const& source, std::string const& outdir, std::string const& name, bool skip_locales)
{
    auto started = std::chrono::steady_clock::now();
    std::string path = outdir + "/" + name + ".sqlite";
    for (const char* suffix : {"", "-journal", "-wal", "-shm"})
        fs::remove(path + suffix);
    {
        sqlite3* db = nullptr;
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) { fprintf(stderr, "acore-db-setup: cannot create %s\n", path.c_str()); return false; }
        sqlite3_exec(db, "PRAGMA page_size=4096; PRAGMA journal_mode=OFF;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    MYSQL* m = mysql_init(nullptr);
    if (!mysql_real_connect(m, outdir.c_str(), "", "", name.c_str(), 0, nullptr, 0))
    {
        fprintf(stderr, "acore-db-setup: %s\n", mysql_error(m));
        return false;
    }
    if (!run(m, "PRAGMA journal_mode=OFF") || !run(m, "PRAGMA synchronous=OFF") || !run(m, "PRAGMA cache_size=-262144"))
        return false;

    // the base: one file per table
    std::string base = source + "/data/sql/base/db_" + name;
    std::vector<fs::path> files;
    for (auto const& e : fs::directory_iterator(base))
        if (e.path().extension() == ".sql")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (fs::path const& f : files)
        if (mysql_sqlite_apply_file(m, f.c_str(), skip_locales ? skip_locale_rows : nullptr, nullptr) != 0)
        {
            fprintf(stderr, "acore-db-setup: %s\n", mysql_error(m));
            return false;
        }
    double base_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // the updates since the base (AzerothCore's updater)
    int applied = mysql_sqlite_apply_updates(m, source.c_str());
    if (applied < 0)
    {
        fprintf(stderr, "acore-db-setup: %s\n", mysql_error(m));
        return false;
    }
    if (skip_locales)
    {
        // rows the updates added to the locale tables
        if (mysql_query(m, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE '%\\_locale' ESCAPE '\\'") == 0)
        {
            std::vector<std::string> tables;
            if (MYSQL_RES* r = mysql_store_result(m))
            {
                while (MYSQL_ROW row = mysql_fetch_row(r))
                    tables.push_back(row[0]);
                mysql_free_result(r);
            }
            for (std::string const& t : tables)
                if (!run(m, ("DELETE FROM `" + t + "`").c_str()))
                    return false;
        }
    }
    if (!run(m, "ANALYZE"))
        return false;
    mysql_close(m);

    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_exec(db, "PRAGMA journal_mode=DELETE; VACUUM;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    printf("%s: %zu base files (%.0f s), %d updates, %.1f MB, %.0f s\n", path.c_str(), files.size(), base_seconds, applied,
           double(fs::file_size(path)) / 1048576.0, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return true;
}

int main(int argc, char** argv)
{
    std::vector<std::string> args, names;
    bool skip_locales = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--skip-locales")) skip_locales = true;
        else if (!strcmp(argv[i], "--nocase")) shim::set_nocase_collation(true);
        else args.push_back(argv[i]);
    }
    if (args.size() < 2)
    {
        fprintf(stderr, "usage: acore-db-setup SOURCE OUTDIR [--skip-locales] [--nocase] [auth|characters|world ...]\n");
        return 2;
    }
    names.assign(args.begin() + 2, args.end());
    if (names.empty())
        names = {"auth", "characters", "world"};
    fs::create_directories(args[1]);
    mysql_library_init(0, nullptr, nullptr);
    for (std::string const& n : names)
        if (!build(args[0], args[1], n, skip_locales))
            return 1;
    return 0;
}
