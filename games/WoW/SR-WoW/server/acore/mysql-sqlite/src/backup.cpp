/* Working copies of kept databases (the characters): the server works on a file in memory, the kept file (in the page's IndexedDB-backed file
   system) is only ever replaced whole. At the first open the kept file is copied over the working one; afterwards a thread copies the working one
   back every `interval` seconds when something was committed, through SQLite's backup API (a consistent snapshot even while other connections write)
   into FILE.new, which is then renamed over FILE: a copy of the file system taken at any moment has a complete FILE. The page asks for a save before
   it goes away (mysql_sqlite_save_now) and waits for mysql_sqlite_saves() to move. */

#include "shim_internal.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define SHIM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define SHIM_EXPORT
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
    // never destroyed: the saver threads wait on them until the process ends (glibc's condition variable destructor waits for its waiters)
    std::mutex& g_lock = *new std::mutex();
    std::condition_variable& g_wake = *new std::condition_variable();
    bool g_save_requested = false;
    std::atomic<unsigned> g_saves{0};

    bool copy_database(sqlite3* from, char const* to_path, std::string* error)
    {
        sqlite3* to = nullptr;
        if (sqlite3_open_v2(to_path, &to, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
        {
            *error = std::string("cannot create ") + to_path;
            sqlite3_close(to);
            return false;
        }
        sqlite3_backup* b = sqlite3_backup_init(to, "main", from, "main");
        int rc = SQLITE_ERROR;
        if (b)
        {
            for (int tries = 0; tries < 200; ++tries)
            {
                rc = sqlite3_backup_step(b, -1);
                if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));   // a writer holds the database: try again shortly
            }
            sqlite3_backup_finish(b);
        }
        if (rc != SQLITE_DONE)
            *error = std::string("copy into ") + to_path + " failed: " + sqlite3_errmsg(to);
        sqlite3_close(to);
        return rc == SQLITE_DONE;
    }

    // a saver: the thread that keeps `kept` up to date with the working copy `live`
    struct Saver
    {
        std::string live, kept;
        int interval = 60;
        bool stop = false, finished = false;   // under g_lock
    };
    std::map<std::string, std::shared_ptr<Saver>>& g_savers = *new std::map<std::string, std::shared_ptr<Saver>>();

    void saver(std::shared_ptr<Saver> self)
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(self->live.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        {
            fprintf(stderr, "mysql-sqlite: cannot open %s for saving\n", self->live.c_str());
            sqlite3_close(db);
            std::lock_guard<std::mutex> lock(g_lock);
            self->finished = true;
            g_wake.notify_all();
            return;
        }
        sqlite3_busy_timeout(db, 5000);
        auto data_version = [&]() {
            // changes when another connection commits (PRAGMA data_version)
            sqlite3_stmt* st = nullptr;
            long long v = -1;
            if (sqlite3_prepare_v2(db, "PRAGMA data_version", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
                v = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            return v;
        };
        long long saved_version = -1;   // the first round always saves (the thread may start after the first changes)
        (void)data_version();
        for (;;)
        {
            bool stopping;
            {
                std::unique_lock<std::mutex> lock(g_lock);
                g_wake.wait_for(lock, std::chrono::seconds(self->interval), [&] { return g_save_requested || self->stop; });
                g_save_requested = false;
                stopping = self->stop;
            }
            long long v = data_version();
            if (v != saved_version)
            {
                std::string error, temp = self->kept + ".new";
                if (copy_database(db, temp.c_str(), &error) && rename(temp.c_str(), self->kept.c_str()) == 0)
                    saved_version = v;
                else
                    fprintf(stderr, "mysql-sqlite: saving %s failed: %s\n", self->kept.c_str(), error.empty() ? "rename" : error.c_str());
            }
            ++g_saves;
            if (stopping)
                break;
        }
        sqlite3_close(db);
        std::lock_guard<std::mutex> lock(g_lock);
        self->finished = true;
        g_wake.notify_all();
    }
}

namespace shim
{
    bool restore_working_copy(std::string const& live, std::string const& kept, std::string* error)
    {
        sqlite3* from = nullptr;
        if (sqlite3_open_v2(kept.c_str(), &from, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        {
            sqlite3_close(from);
            return true;   // nothing kept yet: the working copy is what the file system has
        }
        for (char const* suffix : {"-journal", "-wal", "-shm"})
            remove((live + suffix).c_str());
        bool ok = copy_database(from, live.c_str(), error);
        sqlite3_close(from);
        return ok;
    }

    void start_saver(std::string const& live, std::string const& kept, int interval)
    {
        auto self = std::make_shared<Saver>();
        self->live = live;
        self->kept = kept;
        self->interval = interval;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_savers[live] = self;
        }
        std::thread(saver, self).detach();
    }

    bool stop_saver(std::string const& live)
    {
        std::shared_ptr<Saver> self;
        {
            std::unique_lock<std::mutex> lock(g_lock);
            auto it = g_savers.find(live);
            if (it == g_savers.end())
                return false;
            self = it->second;
            g_savers.erase(it);
            self->stop = true;
            g_wake.notify_all();
            // the last save is on disk when this returns (a few seconds at most)
            g_wake.wait_for(lock, std::chrono::seconds(10), [&] { return self->finished; });
        }
        return true;
    }
}

extern "C"
{
    // the page: save every working copy now (it then waits for mysql_sqlite_saves to change before it writes its file systems to IndexedDB)
    SHIM_EXPORT void mysql_sqlite_save_now(void)
    {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_save_requested = true;
        }
        g_wake.notify_all();
    }

    SHIM_EXPORT unsigned mysql_sqlite_saves(void)
    {
        return g_saves.load();
    }
}
