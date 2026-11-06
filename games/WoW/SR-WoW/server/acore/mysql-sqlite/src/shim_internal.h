// Parts of the shim shared between its source files (not part of the MySQL API).
#pragma once

#include <sqlite3.h>

#include <map>
#include <string>
#include <vector>

namespace shim
{
    // ---- ddl.cpp: MySQL table definitions on SQLite
    // CREATE TABLE and ALTER TABLE of MySQL (as in AzerothCore's dumps and updates) executed on db. Tables are written in one canonical form
    // (declared MySQL types kept, WITHOUT ROWID for keyed tables, indexes named TABLE__INDEX), and
    // ALTER TABLE rebuilds the table when SQLite cannot change it in place (column order, types, the primary key).
    // Returns false and sets *error when the statement is one of these but fails; *handled says whether it was one of these.
    bool run_ddl(sqlite3* db, std::string const& statement, bool* handled, std::string* error);
    // Columns whose MySQL collation compares case-insensitively get COLLATE NOCASE (as utf8mb4_*_ci does). Off by default; the setup tool sets it.
    void set_nocase_collation(bool on);

    // ---- updater.cpp: SQL files and AzerothCore's update bookkeeping
    // the statements of a SQL text (MySQL syntax: ';' outside quotes, -- # and /* */ comments dropped)
    std::vector<std::string> split_statements(std::string const& text);
    std::string sha1_hex_upper(std::string const& bytes);

    // ---- config.cpp: per-database settings (MYSQL_SQLITE_CONF, see the file's comment)
    struct DatabaseOptions
    {
        bool read_only = false;
        std::string updates_source;   // apply the update directories of `updates_include` ($ = this directory) at the first open
        std::string backup;           // the kept copy of a working database (characters): restored at the first open, written every backup_interval s
        int backup_interval = 60;
        bool stream = false;          // the file is streamed by the page (world): opened through the streaming VFS, changes stay in memory
        std::string changes;          // the change log of a streamed database (directory)
    };
    DatabaseOptions database_options(std::string const& name);

    // ---- backup.cpp: working copies of kept databases
    // copies the kept file over the working one (nothing when there is no kept file yet)
    bool restore_working_copy(std::string const& live, std::string const& kept, std::string* error);
    // a thread that writes the working copy back to the kept file (FILE.new, renamed) every interval seconds when it changed, and on request
    void start_saver(std::string const& live, std::string const& kept, int interval);
    // the saver of the working copy `live` writes it once more and ends (its last connection is closed: the realm is left); false when there was none
    bool stop_saver(std::string const& live);

    // ---- stream_vfs.cpp: databases streamed by the page
    extern char const* const STREAM_VFS;
    bool is_streamed(std::string const& path);   // listed in /Port/files.txt (MYSQL_SQLITE_FILES)
    void register_stream_vfs();
    long long streamed_bytes(std::string const& path);

    // ---- changelog.cpp: the change log of a streamed database
    // at the first open: replays the newest generation of DIR on db and starts the next one (merged)
    bool changelog_open(std::string const& name, std::string const& dir, sqlite3* db, std::string* error);
    // a listener of the commits of a change-logged database (database name, the rows as full images: see rows_of_changeset); called on the committing thread
    typedef void (*ChangeListener)(char const* database, char const* rows);
    void changelog_listen(ChangeListener listener);
    // the server changes realm: see changelog.cpp
    bool changelog_switch(std::string const& name, std::string const& dir, sqlite3* db, sqlite3_session* session, std::string* touched, std::string* error);
    sqlite3_session* changelog_attach(sqlite3* db);
    void changelog_after(std::string const& name, sqlite3* db, sqlite3_session*& session);
}
