/* A SQLite VFS for a database that is streamed, not stored: the world database. Its main file is read in blocks from where the page keeps it
   (Module.wowData.read: the browser's Cache Storage, else the network; the same path as the game's archives) and is never written: pages that
   SQLite writes (the server's own updates, our custom SQL, the change log) go into an overlay in memory that every connection of the process
   shares. Journals and temporary files are ordinary files of the default VFS (the module's memory file system).
   Which files are streamed: /Port/files.txt (MYSQL_SQLITE_FILES overrides), lines "LOCAL_PATH<TAB>SIZE<TAB>URL" written by the page; a
   database with the `stream` option whose path is listed there is opened through this VFS. Without the page (Node, native tests) the URL is read
   as a local file. */

#include "shim_internal.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <unistd.h>
#include <unordered_map>

namespace
{
    constexpr int64_t BLOCK = 256 * 1024;      // the unit of the page's cache (its keys); reads are made of whole blocks
    constexpr int64_t CHUNK = 4096;            // the unit of the overlay (SQLite writes whole pages of the database: 4096 bytes)
    constexpr int READ_AHEAD = 8;              // blocks fetched at once when the reads go forward
    constexpr size_t CACHE_LIMIT = 48u << 20;  // blocks kept in memory (most reads happen once, at the server's start)

    struct Remote { std::string url; int64_t size; };

    std::unordered_map<std::string, Remote> load_list()
    {
        std::unordered_map<std::string, Remote> list;
        char const* path = getenv("MYSQL_SQLITE_FILES");
        std::ifstream in(path ? path : "/Port/files.txt");
        std::string line;
        while (std::getline(in, line))
        {
            size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos)
                continue;
            list[line.substr(0, a)] = Remote{line.substr(b + 1), atoll(line.substr(a + 1, b - a - 1).c_str())};
        }
        return list;
    }

    std::unordered_map<std::string, Remote> const& remote_list()
    {
        static std::unordered_map<std::string, Remote> const list = load_list();
        return list;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // reading the remote file

#ifdef __EMSCRIPTEN__
    struct PageRequest   // the layout web/prerun.js reads (as remote.c of the runtime)
    {
        volatile int32_t state;
        int32_t url;
        int32_t length;
        int32_t dest;
        double offset;
        int32_t block_size;
        int32_t file_size;
        int32_t file_size_high;
        int32_t got;
        int32_t cached;
        int32_t blocking;
    };

    bool page_available()
    {
        static int answer = -1;
        if (answer < 0)
            answer = MAIN_THREAD_EM_ASM_INT({ return (Module['wowData'] && Module['wowData'].read) ? 1 : 0; });
        return answer == 1 && !emscripten_is_main_browser_thread();
    }

    bool fetch_by_page(std::string const& url, int64_t offset, int64_t length, uint8_t* dest, int64_t size)
    {
        PageRequest r;
        memset(&r, 0, sizeof(r));
        r.url = int32_t(uintptr_t(url.c_str()));
        r.length = int32_t(length);
        r.dest = int32_t(uintptr_t(dest));
        r.offset = double(offset);
        r.block_size = int32_t(BLOCK);
        r.file_size = int32_t(uint32_t(size));
        r.file_size_high = int32_t(uint32_t(uint64_t(size) >> 32));
        MAIN_THREAD_ASYNC_EM_ASM({ Module['wowData'].read($0, function () { return wasmMemory.buffer; }, Module['wowData'].SERVER_CACHE); }, &r);
        while (__atomic_load_n(&r.state, __ATOMIC_SEQ_CST) == 0)
            emscripten_futex_wait((void*)&r.state, 0, 1000.0);
        return r.state > 0 && r.got == length;
    }
#endif

    bool fetch_range(std::string const& url, int64_t offset, int64_t length, uint8_t* dest, int64_t size)
    {
#ifdef __EMSCRIPTEN__
        if (page_available())
            return fetch_by_page(url, offset, length, dest, size);
#endif
        int fd = open(url.c_str(), O_RDONLY);
        if (fd < 0)
            return false;
        int64_t done = 0;
        while (done < length)
        {
            ssize_t n = pread(fd, dest + done, size_t(length - done), off_t(offset + done));
            if (n <= 0)
                break;
            done += n;
        }
        close(fd);
        return done == length;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // the shared state of one streamed file

    struct Shared
    {
        std::string path;
        Remote remote;
        std::mutex lock;
        int refs = 0;

        int64_t size = 0;           // the file's size as SQLite sees it (writes and truncation change it)
        int64_t base_limit = 0;     // bytes of the remote file still visible (a truncation hides the rest)
        std::unordered_map<int64_t, std::unique_ptr<uint8_t[]>> overlay;   // written chunks by index

        struct Block { std::unique_ptr<uint8_t[]> data; std::list<int64_t>::iterator lru; };
        std::unordered_map<int64_t, Block> blocks;
        std::list<int64_t> lru;     // most recent first
        int64_t last_fetched = -2;
        int64_t fetched_bytes = 0;

        // SQLite's lock levels shared by the connections of the process (as the unix VFS does between connections of one process)
        int shared_holders = 0;
        bool reserved = false, pending = false, exclusive = false;

        int64_t block_length(int64_t b) const { return std::min(BLOCK, remote.size - b * BLOCK); }

        uint8_t* block(int64_t b)
        {
            auto it = blocks.find(b);
            if (it != blocks.end())
            {
                lru.splice(lru.begin(), lru, it->second.lru);
                return it->second.data.get();
            }
            // fetch this block and, when the reads go forward, the next ones that are not here yet (one request)
            int64_t count = 1, last = (remote.size + BLOCK - 1) / BLOCK;
            if (b == last_fetched + 1)
                while (count < READ_AHEAD && b + count < last && !blocks.count(b + count))
                    ++count;
            int64_t offset = b * BLOCK, length = std::min(count * BLOCK, remote.size - offset);
            std::unique_ptr<uint8_t[]> buffer(new uint8_t[size_t(length)]);
            if (!fetch_range(remote.url, offset, length, buffer.get(), remote.size))
            {
                fprintf(stderr, "mysql-sqlite: reading %s at %lld failed\n", remote.url.c_str(), (long long)offset);
                return nullptr;
            }
            fetched_bytes += length;
            last_fetched = b + count - 1;
            for (int64_t k = 0; k < count; ++k)
            {
                int64_t n = block_length(b + k);
                Block entry;
                entry.data.reset(new uint8_t[size_t(n)]);
                memcpy(entry.data.get(), buffer.get() + k * BLOCK, size_t(n));
                lru.push_front(b + k);
                entry.lru = lru.begin();
                blocks[b + k] = std::move(entry);
            }
            while (blocks.size() * size_t(BLOCK) > CACHE_LIMIT && lru.size() > size_t(count))
            {
                blocks.erase(lru.back());
                lru.pop_back();
            }
            return blocks[b].data.get();
        }

        // the remote bytes [offset, offset + n) (zeros past what is visible)
        bool read_base(int64_t offset, int64_t n, uint8_t* out)
        {
            while (n > 0)
            {
                if (offset >= base_limit)
                {
                    memset(out, 0, size_t(n));
                    return true;
                }
                int64_t b = offset / BLOCK, in = offset - b * BLOCK;
                int64_t take = std::min({n, BLOCK - in, base_limit - offset});
                uint8_t* data = block(b);
                if (!data)
                    return false;
                memcpy(out, data + in, size_t(take));
                out += take;
                offset += take;
                n -= take;
            }
            return true;
        }
    };

    std::mutex g_files_lock;
    std::unordered_map<std::string, Shared*> g_files;

    struct StreamFile
    {
        sqlite3_file base;
        Shared* shared;
        int level;   // this connection's lock level
    };

    sqlite3_vfs* g_default = nullptr;

    // ------------------------------------------------------------------------------------------------------------------
    // the io methods of a streamed main database file

    int x_close(sqlite3_file* f)
    {
        StreamFile* sf = reinterpret_cast<StreamFile*>(f);
        std::lock_guard<std::mutex> guard(g_files_lock);
        // the shared state stays for the process (the overlay holds the database's changes)
        sf->shared->refs--;
        return SQLITE_OK;
    }

    int x_read(sqlite3_file* f, void* buffer, int amount, sqlite3_int64 offset)
    {
        Shared* s = reinterpret_cast<StreamFile*>(f)->shared;
        std::lock_guard<std::mutex> guard(s->lock);
        uint8_t* out = static_cast<uint8_t*>(buffer);
        int64_t end = offset + amount, avail = std::min<int64_t>(end, s->size);
        int64_t at = offset;
        while (at < avail)
        {
            int64_t chunk = at / CHUNK, in = at - chunk * CHUNK, take = std::min(CHUNK - in, avail - at);
            auto it = s->overlay.find(chunk);
            if (it != s->overlay.end())
                memcpy(out + (at - offset), it->second.get() + in, size_t(take));
            else if (!s->read_base(at, take, out + (at - offset)))
                return SQLITE_IOERR_READ;
            at += take;
        }
        if (avail < end)
        {
            memset(out + std::max<int64_t>(0, avail - offset), 0, size_t(end - std::max<int64_t>(avail, offset)));
            return SQLITE_IOERR_SHORT_READ;
        }
        return SQLITE_OK;
    }

    int x_write(sqlite3_file* f, void const* buffer, int amount, sqlite3_int64 offset)
    {
        Shared* s = reinterpret_cast<StreamFile*>(f)->shared;
        std::lock_guard<std::mutex> guard(s->lock);
        uint8_t const* in_bytes = static_cast<uint8_t const*>(buffer);
        int64_t at = offset, end = offset + amount;
        while (at < end)
        {
            int64_t chunk = at / CHUNK, in = at - chunk * CHUNK, take = std::min(CHUNK - in, end - at);
            auto it = s->overlay.find(chunk);
            if (it == s->overlay.end())
            {
                std::unique_ptr<uint8_t[]> data(new uint8_t[CHUNK]);
                if (take < CHUNK)
                {
                    // a partial chunk: the rest of it comes from what the file has now
                    int64_t from = chunk * CHUNK, visible = std::max<int64_t>(0, std::min(CHUNK, s->size - from));
                    memset(data.get(), 0, CHUNK);
                    if (visible > 0 && !s->read_base(from, visible, data.get()))
                        return SQLITE_IOERR_WRITE;
                }
                it = s->overlay.emplace(chunk, std::move(data)).first;
            }
            memcpy(it->second.get() + in, in_bytes + (at - offset), size_t(take));
            at += take;
        }
        s->size = std::max(s->size, end);
        return SQLITE_OK;
    }

    int x_truncate(sqlite3_file* f, sqlite3_int64 size)
    {
        Shared* s = reinterpret_cast<StreamFile*>(f)->shared;
        std::lock_guard<std::mutex> guard(s->lock);
        for (auto it = s->overlay.begin(); it != s->overlay.end();)
        {
            if (it->first * CHUNK >= size) it = s->overlay.erase(it);
            else
            {
                if ((it->first + 1) * CHUNK > size)
                    memset(it->second.get() + (size - it->first * CHUNK), 0, size_t((it->first + 1) * CHUNK - size));
                ++it;
            }
        }
        s->base_limit = std::min<int64_t>(s->base_limit, size);
        s->size = size;
        return SQLITE_OK;
    }

    int x_sync(sqlite3_file*, int) { return SQLITE_OK; }

    int x_file_size(sqlite3_file* f, sqlite3_int64* size)
    {
        Shared* s = reinterpret_cast<StreamFile*>(f)->shared;
        std::lock_guard<std::mutex> guard(s->lock);
        *size = s->size;
        return SQLITE_OK;
    }

    int x_lock(sqlite3_file* f, int level)
    {
        StreamFile* sf = reinterpret_cast<StreamFile*>(f);
        Shared* s = sf->shared;
        std::lock_guard<std::mutex> guard(s->lock);
        if (sf->level >= level)
            return SQLITE_OK;
        if (level == SQLITE_LOCK_SHARED)
        {
            if (s->pending || s->exclusive)
                return SQLITE_BUSY;
            s->shared_holders++;
        }
        else if (level == SQLITE_LOCK_RESERVED)
        {
            if (s->reserved || s->exclusive)
                return SQLITE_BUSY;
            s->reserved = true;
        }
        else if (level == SQLITE_LOCK_EXCLUSIVE)
        {
            // PENDING first: no new readers; EXCLUSIVE once this connection is the only reader left
            if (sf->level < SQLITE_LOCK_PENDING)
            {
                if (s->pending && sf->level < SQLITE_LOCK_RESERVED)
                    return SQLITE_BUSY;
                s->pending = true;
                sf->level = SQLITE_LOCK_PENDING;
            }
            if (s->shared_holders > 1)
                return SQLITE_BUSY;
            s->exclusive = true;
        }
        sf->level = level;
        return SQLITE_OK;
    }

    int x_unlock(sqlite3_file* f, int level)
    {
        StreamFile* sf = reinterpret_cast<StreamFile*>(f);
        Shared* s = sf->shared;
        std::lock_guard<std::mutex> guard(s->lock);
        if (sf->level <= level)
            return SQLITE_OK;
        if (sf->level >= SQLITE_LOCK_RESERVED)
        {
            s->reserved = false;
            s->pending = false;
            s->exclusive = false;
        }
        if (level == SQLITE_LOCK_NONE && sf->level >= SQLITE_LOCK_SHARED)
            s->shared_holders--;
        sf->level = level;
        return SQLITE_OK;
    }

    int x_check_reserved(sqlite3_file* f, int* out)
    {
        Shared* s = reinterpret_cast<StreamFile*>(f)->shared;
        std::lock_guard<std::mutex> guard(s->lock);
        *out = s->reserved || s->pending || s->exclusive;
        return SQLITE_OK;
    }

    int x_file_control(sqlite3_file*, int, void*) { return SQLITE_NOTFOUND; }
    int x_sector_size(sqlite3_file*) { return int(CHUNK); }
    int x_device_characteristics(sqlite3_file*) { return SQLITE_IOCAP_SAFE_APPEND | SQLITE_IOCAP_POWERSAFE_OVERWRITE; }

    sqlite3_io_methods const g_methods = {
        1, x_close, x_read, x_write, x_truncate, x_sync, x_file_size, x_lock, x_unlock, x_check_reserved, x_file_control,
        x_sector_size, x_device_characteristics, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

    // ------------------------------------------------------------------------------------------------------------------
    // the VFS: main databases that are listed are streamed, everything else goes to the default VFS

    int v_open(sqlite3_vfs*, char const* name, sqlite3_file* f, int flags, int* out_flags)
    {
        if (name && (flags & SQLITE_OPEN_MAIN_DB))
        {
            auto it = remote_list().find(name);
            if (it != remote_list().end())
            {
                std::lock_guard<std::mutex> guard(g_files_lock);
                Shared*& s = g_files[name];
                if (!s)
                {
                    s = new Shared();
                    s->path = name;
                    s->remote = it->second;
                    s->size = s->base_limit = it->second.size;
                }
                s->refs++;
                StreamFile* sf = reinterpret_cast<StreamFile*>(f);
                sf->base.pMethods = &g_methods;
                sf->shared = s;
                sf->level = SQLITE_LOCK_NONE;
                if (out_flags)
                    *out_flags = (flags & ~SQLITE_OPEN_CREATE) | SQLITE_OPEN_READWRITE;
                return SQLITE_OK;
            }
        }
        return g_default->xOpen(g_default, name, f, flags, out_flags);
    }

    int v_delete(sqlite3_vfs*, char const* name, int sync) { return g_default->xDelete(g_default, name, sync); }
    int v_access(sqlite3_vfs*, char const* name, int flags, int* out)
    {
        if (name && remote_list().count(name)) { *out = 1; return SQLITE_OK; }
        return g_default->xAccess(g_default, name, flags, out);
    }
    int v_full_pathname(sqlite3_vfs*, char const* name, int n, char* out) { return g_default->xFullPathname(g_default, name, n, out); }
    int v_randomness(sqlite3_vfs*, int n, char* out) { return g_default->xRandomness(g_default, n, out); }
    int v_sleep(sqlite3_vfs*, int us) { return g_default->xSleep(g_default, us); }
    int v_current_time(sqlite3_vfs*, double* out) { return g_default->xCurrentTime(g_default, out); }
    int v_last_error(sqlite3_vfs*, int n, char* out) { return g_default->xGetLastError ? g_default->xGetLastError(g_default, n, out) : 0; }
    int v_current_time64(sqlite3_vfs*, sqlite3_int64* out) { return g_default->xCurrentTimeInt64(g_default, out); }

    sqlite3_vfs g_vfs;
}

namespace shim
{
    char const* const STREAM_VFS = "acore-stream";

    bool is_streamed(std::string const& path)
    {
        return remote_list().count(path) != 0;
    }

    void register_stream_vfs()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            g_default = sqlite3_vfs_find(nullptr);
            memset(&g_vfs, 0, sizeof(g_vfs));
            g_vfs.iVersion = 2;
            g_vfs.szOsFile = std::max<int>(int(sizeof(StreamFile)), g_default->szOsFile);
            g_vfs.mxPathname = g_default->mxPathname;
            g_vfs.zName = STREAM_VFS;
            g_vfs.xOpen = v_open;
            g_vfs.xDelete = v_delete;
            g_vfs.xAccess = v_access;
            g_vfs.xFullPathname = v_full_pathname;
            g_vfs.xRandomness = v_randomness;
            g_vfs.xSleep = v_sleep;
            g_vfs.xCurrentTime = v_current_time;
            g_vfs.xGetLastError = v_last_error;
            g_vfs.xCurrentTimeInt64 = v_current_time64;
            sqlite3_vfs_register(&g_vfs, 0);
        });
    }

    long long streamed_bytes(std::string const& path)
    {
        std::lock_guard<std::mutex> guard(g_files_lock);
        auto it = g_files.find(path);
        return it == g_files.end() ? 0 : it->second->fetched_bytes;
    }
}
