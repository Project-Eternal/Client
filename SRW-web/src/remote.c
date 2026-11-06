#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <emscripten.h>
#include <emscripten/threading.h>

#include "srw.h"
#include "srw_remote.h"
#include "srw_win32.h"

#define BLOCK_SIZE (256 * 1024)
#define CACHE_BLOCKS 768 /* 192 MB */
#define READ_AHEAD_BLOCKS 8   /* 2 MB more for a sequential read */
#define MAX_RUN_BLOCKS 32     /* the most blocks a request asks for (8 MB) */
#define SEQUENTIAL_GAP 4096    /* a read this close after the previous one still continues it */
#define PREFETCH_BLOCKS 32    /* a long sequential read: the 8 MB after the read-ahead are fetched in the background (browser only) */
#define PREFETCH_PIECE 8      /* ... in pieces of 2 MB that are fetched at the same time */

struct SRW_RemoteFile {
    char *path;      /* as listed, e.g. Data/model.bin (relative to the program's root) */
    char *url;       /* where it is read from: the remote file system that provides it (data/ or patch/), resolved by the page */
    char *key;       /* lower case, for comparisons */
    uint64_t size;
    uint32_t id;
    uint64_t last_end; /* where the previous read ended, to notice sequential access */
    uint32_t sequential;        /* reads in a row that continued the previous one */
    uint32_t prefetched_until;  /* the blocks before this were asked for in the background */
    int is_archive;             /* a file the program reads sequentially only when it continues exactly (SRW_RemoteSequentialFile) */
    volatile int missing;       /* the host answered that the file is not there: set once, under the lock, read without it */
};

typedef struct {
    uint32_t file;   /* id + 1, 0 when unused */
    uint32_t index;
    uint32_t length;
    uint64_t used;
    uint8_t *data;
    int pending;     /* being fetched by a thread (the lock is not held during the wait): others wait on block_ready */
} Block;

volatile uint32_t srw_remote_reads, srw_remote_ms, srw_remote_kb, srw_remote_cached_kb;
volatile uint64_t srw_remote_asked;   /* bytes the program asked for with its reads (the slow frame report compares them with what was fetched) */

static SRW_RemoteFile *files;
static uint32_t file_count;
static int initialized;
static Block cache[CACHE_BLOCKS];
static uint64_t clock_ticks;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t block_ready = PTHREAD_COND_INITIALIZER;

/* Both functions run in the game's worker: synchronous XMLHttpRequest is allowed there. Under Node (the headless test)
   the root is a directory and files are read directly. */
EM_JS(int, srw_fetch_range, (const char *urlp, double offset, int length, uint8_t *dest), {
    var url = UTF8ToString(urlp);

    try
    {
        if (typeof XMLHttpRequest === "undefined")
        {
            var fs = require("fs");
            var joined = /^join:((?:[0-9]+:)+)(.*)$/.exec(url);

            if (joined)
            {
                /* a file in parts (the page's joinParts): the sizes of X.000, X.001... and the host path X; the range is cut at the parts */
                var sizes = joined[1].split(":").filter(Boolean).map(Number), at = 0, done = 0;

                for (var k = 0; k < sizes.length && done < length; k++)
                {
                    var from = Math.max(offset, at), to = Math.min(offset + length, at + sizes[k]);

                    if (from < to)
                    {
                        var part = fs.openSync(joined[2] + "." + ("00" + k).slice(-3), "r");
                        var n = fs.readSync(part, HEAPU8, dest + done, to - from, from - at);

                        fs.closeSync(part);
                        done += n;
                        if (n < to - from) break;
                    }
                    at += sizes[k];
                }
                return done;
            }
            var fd = fs.openSync(url, "r");
            var got = fs.readSync(fd, HEAPU8, dest, length, offset);

            fs.closeSync(fd);
            return got;
        }
        var xhr = new XMLHttpRequest();

        xhr.open("GET", url, false);
        xhr.responseType = "arraybuffer";
        xhr.setRequestHeader("Range", "bytes=" + offset + "-" + (offset + length - 1));
        xhr.send();
        if (xhr.status !== 206 && xhr.status !== 200)
        {
            return (xhr.status === 404 || xhr.status === 410 || xhr.status === 416) ? -2 : -1;
        }
        var bytes = new Uint8Array(xhr.response);

        if (xhr.status === 200)
        {
            // the server ignored the range and sent the whole file: take the part that was asked for
            bytes = bytes.subarray(offset, offset + length);
        }
        HEAPU8.set(bytes, dest);
        return bytes.length;
    }
    catch (e)
    {
        return (e && e.code === "ENOENT") ? -2 : -1;   /* (under Node a host file that does not exist: missing) */
    }
});

/* In the browser the range is read by the page (web/runtime.js: Module.srwData): fetch() runs there, several at a time, and the blocks
   are kept in the browser's Cache Storage, so the next start reads them from the disk. The calling thread waits on the state word
   of the request (the page sets it and wakes it with Atomics.notify). */
typedef struct {
    volatile int32_t state;   /* 0 waiting, 1 done, -1 failed, -2 the file is missing */
    int32_t url;              /* the address of the URL text */
    int32_t length;
    int32_t dest;
    double offset;
    int32_t block_size;       /* the blocks of the cache (the range is made of whole blocks, the last one may be short) */
    int32_t file_size;        /* (low 32 bits) part of the cache key: a file that changed on the server gets new keys */
    int32_t file_size_high;
    int32_t got;
    int32_t cached;           /* how many bytes came from the browser's Cache Storage */
    int32_t blocking;         /* the game's frames wait for it (the page shows its streaming indicator) */
} PageRequest;

/* the thread that presents the frames (d3d9-bridge.cpp tells it at every Present): its reads freeze the picture */
static volatile pthread_t game_thread;
static volatile int have_game_thread;

void SRW_RemoteNoteGameThread(void)
{
    game_thread = pthread_self();
    have_game_thread = 1;
    SRW_GilNoteGameThread();
}

static int use_page(void)
{
    static int answer = -1;

    if (answer < 0) answer = EM_ASM_INT({ return (typeof XMLHttpRequest !== "undefined" && typeof fetch === "function") ? 1 : 0; });
    return answer && !emscripten_is_main_browser_thread();
}

static int fetch_range_by_page(const char *url, uint64_t offset, uint32_t length, uint8_t *dest, uint64_t file_size)
{
    PageRequest request;

    memset(&request, 0, sizeof(request));
    request.url = (int32_t) (uintptr_t) url;
    request.length = (int32_t) length;
    request.dest = (int32_t) (uintptr_t) dest;
    request.offset = (double) offset;
    request.block_size = BLOCK_SIZE;
    request.file_size = (int32_t) (uint32_t) file_size;
    request.file_size_high = (int32_t) (uint32_t) (file_size >> 32);
    request.blocking = have_game_thread && pthread_equal(pthread_self(), game_thread);
    MAIN_THREAD_ASYNC_EM_ASM({ Module.srwData.read($0); }, &request);
    while (__atomic_load_n(&request.state, __ATOMIC_SEQ_CST) == 0) emscripten_futex_wait((void *) &request.state, 0, 1000.0);
    __sync_fetch_and_add(&srw_remote_cached_kb, (uint32_t) request.cached / 1024);
    return request.state > 0 ? request.got : request.state == -2 ? -2 : -1;
}

/* the page fetches blocks into its storage without anyone waiting (Module.srwData.prefetch): the next reads find them there */
static void prefetch_by_page(const char *url, uint64_t offset, uint32_t length, uint64_t file_size)
{
    char *copy = strdup(url);

    if (copy == NULL) return;
    MAIN_THREAD_ASYNC_EM_ASM({
        var url = UTF8ToString($0);
        _free($0);
        if (Module.srwData && Module.srwData.prefetch) Module.srwData.prefetch(url, $1, $2, $3, $4);
    }, copy, (double) offset, (int) length, (double) file_size, BLOCK_SIZE);
}

static int compare_files(const void *a, const void *b)
{
    return strcmp(((const SRW_RemoteFile *) a)->key, ((const SRW_RemoteFile *) b)->key);
}

static char *lower_copy(const char *text)
{
    char *copy = strdup(text);
    char *p;

    for (p = copy; *p != 0; p++)
    {
        *p = (char) tolower((unsigned char) (*p == '\\' ? '/' : *p));
    }
    return copy;
}

/* The page resolves the program's remote file systems (web/runtime.js, Module.srwData.prepareClient: patch/ wins over data/, init/ files are written
   into the local file system instead) and lists the result in /Port/files.txt: "path<TAB>size<TAB>url", paths relative to the game root. */
static void load_manifest(void)
{
    char line[2048];
    uint32_t capacity = 0;
    FILE *f = fopen("/Port/files.txt", "r");

    if (f == NULL)
    {
        warnprintf("No list of the remote files (/Port/files.txt): nothing is streamed");
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *tab1 = strchr(line, '\t'), *tab2, *end;
        uint64_t size;
        size_t n;

        if (tab1 == NULL) continue;
        *tab1 = 0;
        size = strtoull(tab1 + 1, &end, 10);
        tab2 = strchr(tab1 + 1, '\t');
        if (tab2 == NULL) continue;
        n = strlen(tab2 + 1);
        while (n > 0 && (tab2[n] == '\n' || tab2[n] == '\r')) tab2[n--] = 0;
        if (file_count == capacity)
        {
            capacity = capacity ? capacity * 2 : 256;
            files = realloc(files, capacity * sizeof(*files));
        }
        files[file_count].path = strdup(line);
        files[file_count].key = lower_copy(line);
        files[file_count].url = strdup(tab2 + 1);
        files[file_count].size = size;
        files[file_count].last_end = 0;
        files[file_count].sequential = 0;
        files[file_count].prefetched_until = 0;
        files[file_count].missing = 0;
        n = strlen(line);
        files[file_count].is_archive = SRW_RemoteSequentialFile(line);
        file_count++;
    }
    fclose(f);
    qsort(files, file_count, sizeof(*files), compare_files);
    {
        uint32_t i;

        for (i = 0; i < file_count; i++) files[i].id = i;
    }
    logprintf("%u remote files", (unsigned int) file_count);
}

static void ensure_init(void)
{
    pthread_mutex_lock(&lock);
    if (!initialized)
    {
        initialized = 1;
        load_manifest();
    }
    pthread_mutex_unlock(&lock);
}

/* a path relative to the program's root: the mapped paths of the file functions are under the install folder, the native readers give "/Data/..." */
static const char *strip_slash(const char *path)
{
    while (*path == '/') path++;
    if (strncasecmp(path, SRW_PROGRAM_NAME, sizeof(SRW_PROGRAM_NAME) - 1) == 0 && (path[sizeof(SRW_PROGRAM_NAME) - 1] == '/' || path[sizeof(SRW_PROGRAM_NAME) - 1] == 0)) path += sizeof(SRW_PROGRAM_NAME) - 1;
    while (*path == '/') path++;
    return path;
}

SRW_RemoteFile *SRW_RemoteFind(const char *mapped_path)
{
    SRW_RemoteFile key;
    char *lower = lower_copy(strip_slash(mapped_path));
    SRW_RemoteFile *found;

    ensure_init();
    key.key = lower;
    found = bsearch(&key, files, file_count, sizeof(*files), compare_files);
    free(lower);
    if (found != NULL && __atomic_load_n(&found->missing, __ATOMIC_ACQUIRE)) return NULL;
    return found;
}

uint64_t SRW_RemoteSize(const SRW_RemoteFile *file)
{
    return file->size;
}

int SRW_RemoteIsDirectory(const char *mapped_path)
{
    char *lower = lower_copy(strip_slash(mapped_path));
    size_t n = strlen(lower);
    uint32_t i;
    int found = 0;

    ensure_init();
    if (n == 0)
    {
        free(lower);
        return file_count > 0;
    }
    for (i = 0; i < file_count && !found; i++)
    {
        found = !__atomic_load_n(&files[i].missing, __ATOMIC_ACQUIRE) && strncmp(files[i].key, lower, n) == 0 && files[i].key[n] == '/';
    }
    free(lower);
    return found;
}

void SRW_RemoteList(const char *mapped_path, void (*entry)(const char *name, int64_t size, void *context), void *context)
{
    char *lower = lower_copy(strip_slash(mapped_path));
    size_t n = strlen(lower);
    uint32_t i, j;

    ensure_init();
    for (i = 0; i < file_count; i++)
    {
        const char *rest, *slash;
        char name[260];

        if (__atomic_load_n(&files[i].missing, __ATOMIC_ACQUIRE)) continue;
        if (n > 0 && !(strncmp(files[i].key, lower, n) == 0 && files[i].key[n] == '/')) continue;
        rest = files[i].path + (n > 0 ? n + 1 : 0);
        slash = strchr(rest, '/');
        if (slash == NULL)
        {
            entry(rest, (int64_t) files[i].size, context);
        }
        else
        {
            size_t length = (size_t) (slash - rest);

            if (length >= sizeof(name)) continue;
            memcpy(name, rest, length);
            name[length] = 0;
            /* report a subdirectory once: skip if an earlier entry had the same first component */
            for (j = 0; j < i; j++)
            {
                if (!__atomic_load_n(&files[j].missing, __ATOMIC_ACQUIRE) && strncmp(files[j].key, files[i].key, (size_t) (slash - files[i].path) + 1) == 0) break;
            }
            if (j == i) entry(name, -1, context);
        }
    }
    free(lower);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the block cache */

static void encode_url(const SRW_RemoteFile *file, char *out, size_t size)
{
    snprintf(out, size, "%s", file->url);
}

/* the block, fetched if needed. lock is held; the network is used with the lock held, one request at a time */
/* the cache block with the given index of a file, or NULL (no side effects) */
static Block *find_block(const SRW_RemoteFile *file, uint32_t index)
{
    uint32_t i;

    for (i = 0; i < CACHE_BLOCKS; i++)
    {
        if (cache[i].file == file->id + 1 && cache[i].index == index) return &cache[i];
    }
    return NULL;
}

static Block *pick_victim(void)
{
    Block *victim = NULL;
    uint32_t i;

    for (i = 0; i < CACHE_BLOCKS; i++)
    {
        if (cache[i].pending) continue;
        if (victim == NULL || cache[i].file == 0 || (victim->file != 0 && cache[i].used < victim->used)) victim = &cache[i];
    }
    return victim;
}

/* Gets the blocks first .. first+count-1 of a file (none of them is in the cache) with a single request: the wait for the
   server is the cost, not the size. lock is held on entry and on return, but not during the wait: the blocks are reserved
   (pending) meanwhile, so other threads can read other blocks at the same time and wait for these. */
static int fetch_run(SRW_RemoteFile *file, uint32_t first, uint32_t count)
{
    uint64_t start = (uint64_t) first * BLOCK_SIZE, end = start + (uint64_t) count * BLOCK_SIZE;
    uint32_t total, k;
    uint8_t *buffer;
    Block *reserved[MAX_RUN_BLOCKS + READ_AHEAD_BLOCKS];
    char url[1200];
    double began;
    int got, ok;

    if (__atomic_load_n(&file->missing, __ATOMIC_ACQUIRE)) return 0;   /* (never asks the page again for a file it marked) */
    if (count > sizeof(reserved) / sizeof(reserved[0])) count = sizeof(reserved) / sizeof(reserved[0]);
    if (end > file->size) end = file->size;
    total = (uint32_t) (end - start);
    buffer = malloc(total);
    if (buffer == NULL) return 0;
    for (k = 0; k < count; k++)
    {
        Block *victim = pick_victim();

        if (victim->data == NULL) victim->data = malloc(BLOCK_SIZE);
        victim->file = file->id + 1;
        victim->index = first + k;
        victim->length = 0;
        victim->pending = 1;
        victim->used = ++clock_ticks;
        reserved[k] = victim;
    }
    encode_url(file, url, sizeof(url));
    pthread_mutex_unlock(&lock);

    began = emscripten_get_now();
    SRW_Note("reading a data file from the server", (uint32_t) (start >> 18));
    got = use_page() ? fetch_range_by_page(url, start, total, buffer, file->size) : srw_fetch_range(url, (double) start, (int) total, buffer);
    __sync_fetch_and_add(&srw_remote_reads, 1);
    __sync_fetch_and_add(&srw_remote_ms, (uint32_t) (emscripten_get_now() - began));
    __sync_fetch_and_add(&srw_remote_kb, total / 1024);
    ok = got >= 0 && (uint32_t) got == total;
    if (!ok) warnprintf("Reading %s at %llu failed%s", file->path, (unsigned long long) start, got == SRW_REMOTE_MISSING ? " (the file is missing)" : "");

    pthread_mutex_lock(&lock);
    if (got == SRW_REMOTE_MISSING) __atomic_store_n(&file->missing, 1, __ATOMIC_RELEASE);
    for (k = 0; k < count; k++)
    {
        Block *block = reserved[k];
        uint32_t length = total - k * BLOCK_SIZE;

        if (length > BLOCK_SIZE) length = BLOCK_SIZE;
        block->pending = 0;
        if (!ok || block->data == NULL)
        {
            block->file = 0;
            continue;
        }
        memcpy(block->data, buffer + (size_t) k * BLOCK_SIZE, length);
        block->length = length;
    }
    pthread_cond_broadcast(&block_ready);
    free(buffer);
    return ok;
}

/* the block, fetched if needed (lock is held; it is released while a fetch or another thread's fetch is waited for) */
static Block *get_block(SRW_RemoteFile *file, uint32_t index)
{
    for (;;)
    {
        Block *block = find_block(file, index);

        if (block != NULL && block->pending)
        {
            pthread_cond_wait(&block_ready, &lock);
            continue;
        }
        if (block != NULL)
        {
            block->used = ++clock_ticks;
            return block;
        }
        if (!fetch_run(file, index, 1)) return NULL;
    }
}

static int64_t remote_read(SRW_RemoteFile *file, uint64_t offset, void *buffer, uint32_t size);

/* (the time the game thread spends here is the "data reads" of the frame time pie chart) */
int64_t SRW_RemoteRead(SRW_RemoteFile *file, uint64_t offset, void *buffer, uint32_t size)
{
    int64_t result;

    __sync_fetch_and_add(&srw_remote_asked, size);
    SRW_PROF_ENTER(SRW_PROF_DATA);
    result = remote_read(file, offset, buffer, size);
    SRW_PROF_LEAVE();
    return result;
}

static int64_t remote_read(SRW_RemoteFile *file, uint64_t offset, void *buffer, uint32_t size)
{
#ifdef SRW_TRACE_FILES
    logprintf("Read %s at %llu, %u bytes", file->path, (unsigned long long) offset, (unsigned int) size);
#endif
    uint8_t *out = buffer;
    uint32_t done = 0;
    int held = SRW_GilHeld();

    if (offset >= file->size) return 0;
    if (offset + size > file->size) size = (uint32_t) (file->size - offset);

    /* the network wait must not keep the big lock; the block cache has a lock of its own which is never held while
       waiting for the big lock */
    if (held) SRW_GilRelease();
    pthread_mutex_lock(&lock);
    if (size > 0)
    {
        /* the blocks of this read that are missing come in runs, one request for each run; a sequential read also gets the
           blocks after it (read ahead), because the next read is likely to want them */
        uint32_t first = (uint32_t) (offset / BLOCK_SIZE), last = (uint32_t) ((offset + size - 1) / BLOCK_SIZE);
        uint32_t final_block = (uint32_t) ((file->size - 1) / BLOCK_SIZE), index;
        uint32_t limit = last;

        /* sequential: this read starts where the last one ended, or a little after it (a movie is read chunk by chunk, skipping
           the 8-byte chunk headers) */
        if (offset == file->last_end || (!file->is_archive && offset > file->last_end && offset - file->last_end <= SEQUENTIAL_GAP))
        {
            limit = last + READ_AHEAD_BLOCKS;
            if (limit > final_block) limit = final_block;
            file->sequential++;
        }
        else
        {
            file->sequential = 0;
            file->prefetched_until = 0;
        }
        /* a long sequential read (a movie: the program reads all 45 MB of it before it plays): the next blocks are fetched in the
           background as well, several requests at a time, while this read waits for its own */
        if (file->sequential >= 4 && use_page() && !file->missing)
        {
            uint32_t from = file->prefetched_until > limit + 1 ? file->prefetched_until : limit + 1;
            uint32_t until = limit + 1 + PREFETCH_BLOCKS;
            char url[1200];

            if (until > final_block + 1) until = final_block + 1;
            /* (the window is topped up a whole piece at a time) */
            if (until - from < PREFETCH_PIECE && until != final_block + 1) until = from;
            if (from < until) encode_url(file, url, sizeof(url));
            for (; from < until; from += PREFETCH_PIECE)
            {
                uint32_t count = until - from < PREFETCH_PIECE ? until - from : PREFETCH_PIECE;
                uint64_t start = (uint64_t) from * BLOCK_SIZE, end = start + (uint64_t) count * BLOCK_SIZE;

                if (end > file->size) end = file->size;
                prefetch_by_page(url, start, (uint32_t) (end - start), file->size);
                file->prefetched_until = from + count;
            }
        }
        index = first;
        while (index <= limit)
        {
            uint32_t run_start, run_length;

            if (find_block(file, index) != NULL)
            {
                index++;
                continue;
            }
            /* (fetch_run releases the lock: another thread may have taken any block meanwhile, so the run is measured now) */
            /* a run of missing blocks; blocks beyond the read only count when the run reaches the end of the read */
            run_start = index;
            while (index <= limit && find_block(file, index) == NULL && index - run_start < MAX_RUN_BLOCKS)
            {
                index++;
            }
            run_length = index - run_start;
            if (run_start > last) break;   /* only read ahead */
            if (!fetch_run(file, run_start, run_length))
            {
                pthread_mutex_unlock(&lock);
                if (held) SRW_GilAcquire();
                return file->missing ? SRW_REMOTE_MISSING : -1;
            }
        }
    }
    while (done < size)
    {
        uint64_t position = offset + done;
        uint32_t index = (uint32_t) (position / BLOCK_SIZE);
        uint32_t inside = (uint32_t) (position % BLOCK_SIZE);
        Block *block = get_block(file, index);
        uint32_t count;

        if (block == NULL)
        {
            pthread_mutex_unlock(&lock);
            if (held) SRW_GilAcquire();
            return done > 0 ? (int64_t) done : file->missing ? SRW_REMOTE_MISSING : -1;
        }
        count = block->length - inside;
        if (count > size - done) count = size - done;
        memcpy(out + done, block->data + inside, count);
        done += count;
    }
    file->last_end = offset + size;
    pthread_mutex_unlock(&lock);
    if (held) SRW_GilAcquire();
    return (int64_t) done;
}

const char *SRW_RemotePath(const SRW_RemoteFile *file)
{
    return file->path;
}
