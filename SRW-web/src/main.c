#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/heap.h>

#include "srw.h"
#include "srw_debuglog.h"

/* the program has its own message loop and blocks, so it runs on its own thread */
static void *GameThread(void *arg)
{
    _cpu *cpu;

    (void) arg;

    cpu = x86_initialize_cpu();
    SRW_GilAcquire();

    /* a WebAssembly trap ends the thread with a bare message: this reports where it happened (the functions of the
       program are named loc_<address>, the stack lists them innermost first) */
    EM_ASM({
        if (typeof addEventListener === "function")
        {
            addEventListener("error", function (event)
            {
                var error = event.error;

                err("[Game] Uncaught error in the game thread: " + (error && error.stack ? error.stack : String(event.message)));
            });
        }
    });

#ifdef SRW_DIAGNOSTICS
    SRW_DebugLogInit();
#endif
    logprint("Starting the recompiled client");
    c_SRW_Entry(cpu);
    logprintf("The program returned with exit code %u", (unsigned int) eax);
    SRW_DumpProcTrace();

    x86_deinitialize_cpu();
    SRW_GilRelease();
    return NULL;
}

/* called by the dispatcher when the program jumps to something that is neither code of the program nor a function of
   this runtime (a return address of garbage, a function pointer that was never set...) */
void SRW_BadJump(_cpu *cpu, uint32_t target)
{
    uint32_t stack = esp, i;
    char line[512];
    size_t n;

    warnprintf("Jump to the bad address %x (esp %x, eax %x, ecx %x, edx %x, esi %x, edi %x, ebp %x)", (unsigned int) target, (unsigned int) stack,
               (unsigned int) eax, (unsigned int) ecx, (unsigned int) edx, (unsigned int) esi, (unsigned int) edi, (unsigned int) ebp);
    n = snprintf(line, sizeof(line), "The stack:");
    for (i = 0; i < 16 && n < sizeof(line) - 12; i++)
    {
        n += snprintf(line + n, sizeof(line) - n, " %x", (unsigned int) *(uint32_t *) REG2PTR(stack + 4 * i));
    }
    warnprint(line);
    SRW_DumpProcTrace();

    SRW_GilRelease();
    for (;;) sleep(3600);
}

uint32_t CCALL ExitProcess_c(uint32_t uExitCode);

void CCALL SRW_Unsupported_c(uint32_t address)
{
    const uint8_t *code = (const uint8_t *) (uintptr_t) address;

    warnprintf("Unsupported instruction at %x: %02x %02x %02x %02x %02x %02x %02x %02x (the recompiler did not translate it)", (unsigned int) address,
               code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
#ifdef SRW_DIAGNOSTICS
    SRW_LogGuestStack("unsupported instruction");
#endif
    ExitProcess_c(0xdead0001u);
}

void CCALL SRW_Unimplemented_c(uint32_t id)
{
    warnprintf("%s is not implemented yet",
               (id < SRW_ImportCount) ? SRW_ImportNames[id] : "an unknown import");

    SRW_DumpProcTrace();

    /* nothing sensible can continue: park the thread instead of crashing the page */
    if (SRW_GilHeld()) SRW_GilRelease();
    for (;;)
    {
        sleep(1);
    }
}

#define PROC_TRACE_SIZE 128

static _Thread_local uint32_t proc_trace[PROC_TRACE_SIZE];
static _Thread_local uint32_t proc_trace_count;

#ifdef SRW_SNAPSHOT_ADDRESS
EM_JS(void, srw_dump_memory, (const char *pathp, uint32_t lo, uint32_t hi), {
    require("fs").writeFileSync(UTF8ToString(pathp), HEAPU8.slice(lo, hi));
});

/* Debugging the recompiler (Node only): at the COUNT-th entry of the procedure at ADDRESS the memory, the image and the stack are written to files
   and the registers printed (SNAP; SNAPX holds the start of the stack dump and of the memory dump, and SECOND when given); the next LENGTH procedure entries print theirs
   (TR), to be compared with an emulation of the original code. */
static void snapshot_hook(uint32_t address)
{
    static int hits, logging;
    static uint32_t logged;
    _cpu *cpu = x86_initialize_cpu();

    if (logging)
    {
#ifdef SRW_SNAPSHOT_SECOND
        if (logged == SRW_SNAPSHOT_SECOND) srw_dump_memory("snapshot-mem2.bin", SRW_SNAPSHOT_MEMORY_LO, SRW_SNAPSHOT_MEMORY_HI);
#endif
        if (logged < SRW_SNAPSHOT_LENGTH)
        {
            fprintf(stderr, "TR %x %x %x %x %x %x %x %x %x %x\n", (unsigned int) address, (unsigned int) eax, (unsigned int) ebx, (unsigned int) ecx,
                    (unsigned int) edx, (unsigned int) esi, (unsigned int) edi, (unsigned int) ebp, (unsigned int) esp, (unsigned int) eflags);
            logged++;
        }
        return;
    }
    if (address != SRW_SNAPSHOT_ADDRESS || ++hits != SRW_SNAPSHOT_COUNT) return;

    srw_dump_memory("snapshot-mem.bin", SRW_SNAPSHOT_MEMORY_LO, SRW_SNAPSHOT_MEMORY_HI);
    srw_dump_memory("snapshot-image.bin", 0x400000, 0x400000 + SRW_ImageSize);
    srw_dump_memory("snapshot-stack.bin", esp - 0x60000, esp + 0x2000);
#ifdef SRW_SNAPSHOT_SECOND
    fprintf(stderr, "SNAPX %x %x %x\n", (unsigned int) (esp - 0x60000), (unsigned int) SRW_SNAPSHOT_MEMORY_LO, (unsigned int) SRW_SNAPSHOT_SECOND);
#else
    fprintf(stderr, "SNAPX %x %x\n", (unsigned int) (esp - 0x60000), (unsigned int) SRW_SNAPSHOT_MEMORY_LO);
#endif
    fprintf(stderr, "SNAP %x %x %x %x %x %x %x %x %x\n", (unsigned int) address, (unsigned int) eax, (unsigned int) ebx, (unsigned int) ecx,
            (unsigned int) edx, (unsigned int) esi, (unsigned int) edi, (unsigned int) ebp, (unsigned int) esp);
    logging = 1;
}
#endif


/* What each thread that runs guest code is doing, for the watchdog: the last procedure entered and the last import called. */
typedef struct {
    volatile uint32_t used;
    volatile uint32_t last_proc, last_import, procs, imports;
    const char *volatile note;   /* what the thread waits for, NULL when it runs */
    volatile uint32_t note_arg, ticks;
    volatile uint32_t note_timeout;  /* the timeout of the wait it notes (milliseconds, INFINITE) */
    volatile double note_time;       /* when it began (emscripten_get_now) */
    volatile uint32_t ring[16];  /* the last procedures entered (build with traceprocs=1), ring[procs % 16] is the latest */
    volatile uint32_t last_caller;   /* the guest address that called the last import (diagnostics build) */
    volatile uint32_t callers[16];   /* the callers of the last imports, callers[imports % 16] is the latest */
    _cpu *volatile cpu;              /* the registers of the thread (its guest stack, for the freeze report) */
} ThreadStat;

#define MAX_THREAD_STATS 16
static ThreadStat thread_stats[MAX_THREAD_STATS];
static _Thread_local ThreadStat *my_stat;

static ThreadStat *thread_stat(void)
{
    if (my_stat == NULL)
    {
        int i;

        for (i = 0; i < MAX_THREAD_STATS; i++)
        {
            if (__sync_bool_compare_and_swap(&thread_stats[i].used, 0, 1))
            {
                my_stat = &thread_stats[i];
                break;
            }
        }
    }
    return my_stat;
}

/* the game thread presents a frame (d3d9-bridge.cpp): the watchdog reports a freeze when this stops */
static volatile uint32_t presents;
static ThreadStat *volatile game_stat;

void SRW_NotePresent(void)
{
    ThreadStat *st = my_stat != NULL ? my_stat : thread_stat();

    game_stat = st;
    presents++;
}


#ifdef SRW_DIAGNOSTICS
/* is there a call instruction right before this address of the program (a return address on the stack)? */
static int after_call(uint32_t address)
{
    const uint8_t *p = (const uint8_t *) (uintptr_t) address;

    if (address < SRW_VaBase + 8 || address >= SRW_VaBase + SRW_VaSize) return 0;
    if (p[-5] == 0xe8) return 1;                                                         /* call rel32 */
    if (p[-2] == 0xff && (p[-1] & 0x38) == 0x10) return 1;                               /* call reg / [reg] */
    if (p[-3] == 0xff && (p[-2] & 0x38) == 0x10 && (p[-2] & 0xc0) == 0x40) return 1;     /* call [reg+disp8] */
    if (p[-6] == 0xff && (p[-5] & 0x38) == 0x10) return 1;                               /* call [disp32] / [reg+disp32] */
    if (p[-4] == 0xff && (p[-3] & 0x38) == 0x10 && (p[-3] & 0xc7) == 0x44) return 1;     /* call [sib+disp8] */
    return 0;
}

int SRW_GuestReadable(uint32_t address, uint32_t length)
{
    return address >= 0x10000 && (uint64_t) address + length <= (uint64_t) emscripten_get_heap_size();   /* (64 bits: a length that wraps is no length) */
}

/* the return addresses on the calling thread's guest stack (diagnostics: who called an import) */
void SRW_LogGuestStack(const char *what)
{
    ThreadStat *st = my_stat != NULL ? my_stat : thread_stat();
    _cpu *cpu = st != NULL ? st->cpu : NULL;
    char line[1000];
    size_t n;
    uint32_t address, top, found = 0;

    if (cpu == NULL || cpu->stack_top == NULL) return;
    address = cpu->_esp;
    top = (uint32_t) (uintptr_t) cpu->stack_top;
    if (address < (uint32_t) (uintptr_t) cpu->stack_bottom || address >= top) return;
    n = (size_t) snprintf(line, sizeof(line), "%s: return addresses", what);
    for (; address + 4 <= top && found < 60 && n < sizeof(line) - 12; address += 4)
    {
        uint32_t value = *(const volatile uint32_t *) (uintptr_t) address;

        if (!after_call(value)) continue;
        n += (size_t) snprintf(line + n, sizeof(line) - n, " %x", (unsigned int) value);
        found++;
    }
    logprint(line);
}

/* what the game thread is waiting for, at a moment it waits in its main loop (autoinput's sched) */
void SRW_DescribeGameThread(void)
{
    ThreadStat *st = game_stat;
    int tries;

    for (tries = 0; st != NULL && st->cpu != NULL && tries < 2000; tries++)
    {
        uint32_t object;

        if (st->note != NULL && strncmp(st->note, "wait for an object", 18) == 0)
        {
            logprintf("Sched: the game thread waits for handle %x, timeout %d ms, waiting for %.1f ms", (unsigned int) st->note_arg,
                      (int) st->note_timeout, emscripten_get_now() - st->note_time);
            SRW_DescribeIdle(st->cpu->_esi, st->cpu->_edi);
            return;
        }
        if ((object = SRW_IdleObject()) != 0)
        {
            logprint("Sched: the object at the top of the game thread's queue (the thread runs):");
            SRW_DescribeIdle(object, 0);
            return;
        }
        usleep(500);
    }
    logprint("Sched: the game thread did not wait, nothing was queued");
}

/* where the game thread is when it stopped presenting frames: its last imports and their callers, and the return addresses on its stack */
void SRW_DescribeWaitObject(uint32_t handle);

static void freeze_report(ThreadStat *st, unsigned int seconds, uint32_t imports_in_5s, uint32_t ticks_in_5s, int first)
{
    char line[1200];
    size_t n;
    int k;
    _cpu *cpu = st->cpu;

    n = (size_t) snprintf(line, sizeof(line), "Freeze: no frame for %u s; the game thread %s%s, %u imports and %u wake-ups in 5 s, last import %s from %x; callers of the last imports (newest first):",
                          seconds, st->note != NULL ? "waits: " : "runs", st->note != NULL ? st->note : "", (unsigned int) imports_in_5s, (unsigned int) ticks_in_5s,
                          st->last_import < SRW_ImportCount ? SRW_ImportNames[st->last_import] : "?", (unsigned int) st->last_caller);
    for (k = 0; k < 16 && n < sizeof(line) - 12; k++) n += (size_t) snprintf(line + n, sizeof(line) - n, " %x", (unsigned int) st->callers[(st->imports - k) & 15]);
    logprint(line);
    if (cpu != NULL)
    {
        /* the registers as of the last block boundary, and the first dwords behind esi and edi (a container, an iterator...) */
        logprintf("Freeze: registers: eax %x ecx %x edx %x ebx %x esi %x edi %x ebp %x esp %x", (unsigned int) cpu->_eax, (unsigned int) cpu->_ecx, (unsigned int) cpu->_edx,
                  (unsigned int) cpu->_ebx, (unsigned int) cpu->_esi, (unsigned int) cpu->_edi, (unsigned int) cpu->_ebp, (unsigned int) cpu->_esp);
        {
            /* the frames of the functions that keep ebp (most of the program): return address and caller's ebp */
            char frames[400];
            size_t fn = (size_t) snprintf(frames, sizeof(frames), "Freeze: ebp chain, return addresses:");
            uint32_t frame = cpu->_ebp;
            int depth;

            for (depth = 0; depth < 24 && fn < sizeof(frames) - 12 && frame >= cpu->_esp && frame < cpu->_esp + 0x400000 && (frame & 3) == 0; depth++)
            {
                fn += (size_t) snprintf(frames + fn, sizeof(frames) - fn, " %x", (unsigned int) GUEST32(frame + 4));
                if (GUEST32(frame) <= frame) break;
                frame = GUEST32(frame);
            }
            logprint(frames);
        }
        if (SRW_GuestReadable(cpu->_esi, 16)) logprintf("Freeze: [esi] %x %x %x %x", (unsigned int) GUEST32(cpu->_esi), (unsigned int) GUEST32(cpu->_esi + 4), (unsigned int) GUEST32(cpu->_esi + 8), (unsigned int) GUEST32(cpu->_esi + 12));
        if (SRW_GuestReadable(cpu->_edi, 16)) logprintf("Freeze: [edi] %x %x %x %x", (unsigned int) GUEST32(cpu->_edi), (unsigned int) GUEST32(cpu->_edi + 4), (unsigned int) GUEST32(cpu->_edi + 8), (unsigned int) GUEST32(cpu->_edi + 12));
    }
    if (cpu != NULL && cpu->stack_top != NULL)
    {
        uint32_t address = cpu->_esp, top = (uint32_t) (uintptr_t) cpu->stack_top, found = 0;

        n = (size_t) snprintf(line, sizeof(line), "Freeze: stack of the game thread (esp %x, ebp %x): return addresses", (unsigned int) address, (unsigned int) cpu->_ebp);
        if (address < (uint32_t) (uintptr_t) cpu->stack_bottom || address >= top) top = address + 65536;   /* (a fiber's stack: look at what is above esp) */
        for (; address + 4 <= top && found < 40 && n < sizeof(line) - 12; address += 4)
        {
            uint32_t value = *(const volatile uint32_t *) (uintptr_t) address;

            if (!after_call(value)) continue;
            n += (size_t) snprintf(line + n, sizeof(line) - n, " %x", (unsigned int) value);
            found++;
        }
        logprint(line);
    }
    if (!first) return;
    /* what the game thread waits for (when it waits for an object), and what every other thread does: the one that should wake it */
    if (st->note != NULL && strncmp(st->note, "wait for an object", 18) == 0)
    {
        logprintf("Freeze: the game thread waits for handle %x (timeout %d ms, waiting for %.0f ms):", (unsigned int) st->note_arg,
                  (int) st->note_timeout, emscripten_get_now() - st->note_time);
        SRW_DescribeWaitObject(st->note_arg);
    }
    if (cpu != NULL) SRW_DescribeIdle(SRW_IdleObject() != 0 ? SRW_IdleObject() : cpu->_esi, cpu->_edi);
    for (k = 0; k < MAX_THREAD_STATS; k++)
    {
        const ThreadStat *other = &thread_stats[k];
        int c;

        if (!other->used || other == st) continue;
        n = (size_t) snprintf(line, sizeof(line), "Freeze: thread %d %s%s (handle %x), %u imports so far, last %s from %x, callers:", k,
                              other->note != NULL ? "waits: " : "runs", other->note != NULL ? other->note : "", (unsigned int) other->note_arg,
                              (unsigned int) other->imports, other->last_import < SRW_ImportCount ? SRW_ImportNames[other->last_import] : "?",
                              (unsigned int) other->last_caller);
        for (c = 0; c < 8 && n < sizeof(line) - 12; c++) n += (size_t) snprintf(line + n, sizeof(line) - n, " %x", (unsigned int) other->callers[(other->imports - c) & 15]);
        logprint(line);
    }
}
#endif

#ifdef SRW_DIAGNOSTICS
static void *Watchdog(void *arg)
{
    uint32_t before_procs[MAX_THREAD_STATS] = {0}, before_imports[MAX_THREAD_STATS] = {0}, before_ticks[MAX_THREAD_STATS] = {0};
    uint32_t before_reads = 0, before_ms = 0, before_kb = 0;
    (void) arg;
    for (;;)   /* the whole session: a freeze after the first ten minutes was not seen otherwise */
    {
        int i;
        static int ring_shown[MAX_THREAD_STATS], ring_running[MAX_THREAD_STATS];
        uint32_t reads = srw_remote_reads, ms = srw_remote_ms, kb = srw_remote_kb;

        sleep(5);
        reads = srw_remote_reads; ms = srw_remote_ms; kb = srw_remote_kb;
        const int show = SRW_LOG_ON(SRW_LOG_WATCHDOG);   /* (the counters go on while it is off, so the first report has real numbers) */
        {
            /* a freeze is reported whether the category is on or not: nobody can type a command then */
            static uint32_t last_presents, still, reports_this_stall, game_imports, game_ticks, loading_reported;
            uint32_t now_presents = presents;
            /* before the first frame the first thread is the one that matters (the loading of the program) */
            ThreadStat *game = game_stat != NULL ? game_stat : (presents == 0 && thread_stats[0].used ? &thread_stats[0] : NULL);

            if (now_presents != last_presents || game == NULL)
            {
                if (reports_this_stall > 0) logprintf("Freeze: frames again after %u s", (unsigned int) (still * 5));
                else if (loading_reported) logprintf("Loading: frames again after %u s", (unsigned int) (still * 5));
                still = 0;
                reports_this_stall = 0;
                loading_reported = 0;
            }
            else if (++still >= 2)
            {
                /* waiting for data that keeps coming is loading (a movie of 45 MB is read whole before it plays), not a freeze */
                const char *note = game->note;
                int loading = note != NULL && strcmp(note, "reading a data file from the server") == 0 && kb != before_kb;

                if (loading)
                {
                    if (!loading_reported) logprintf("Loading: no frame for %u s, the game reads data (%u KB in 5 s)", (unsigned int) (still * 5),
                                                     (unsigned int) (kb - before_kb));
                    loading_reported = 1;
                }
                else if (reports_this_stall < 6)
                {
                    reports_this_stall++;
                    freeze_report(game, still * 5, game->imports - game_imports, game->ticks - game_ticks, reports_this_stall == 1);
                }
            }
            last_presents = now_presents;
            if (game != NULL) { game_imports = game->imports; game_ticks = game->ticks; }
        }

        {
            static uint32_t before_cached;
            uint32_t cached = srw_remote_cached_kb;

            if (show) logprintf("Watchdog: streaming %u requests, %u KB (%u KB of it from the browser's storage), %u ms waiting in 5 s",
                                (unsigned int) (reads - before_reads), (unsigned int) (kb - before_kb), (unsigned int) (cached - before_cached), (unsigned int) (ms - before_ms));
            before_cached = cached;
        }
        before_reads = reads; before_ms = ms; before_kb = kb;
        for (i = 0; i < MAX_THREAD_STATS; i++)
        {
            const ThreadStat *st = &thread_stats[i];
            uint32_t procs = st->procs, imports = st->imports, ticks = st->ticks, last_import = st->last_import;
            const char *note = st->note;

            if (!st->used) continue;
            if (show) logprintf("Watchdog: thread %d %s%s%s: %u wake-ups, %u procedures, %u imports in 5 s%s%s", i,
                      (ticks == before_ticks[i] && procs == before_procs[i] && imports == before_imports[i]) ? "does not move" : "runs",
                      note != NULL ? ", now: " : "", note != NULL ? note : "", (unsigned int) (ticks - before_ticks[i]),
                      (unsigned int) (procs - before_procs[i]), (unsigned int) (imports - before_imports[i]),
                      (last_import < SRW_ImportCount && st->imports != 0) ? ", last import " : "",
                      (last_import < SRW_ImportCount && st->imports != 0) ? SRW_ImportNames[last_import] : "");
            if (show && ticks == before_ticks[i] && procs == before_procs[i] && imports == before_imports[i] && procs != 0 && (note == NULL || !ring_shown[i]))
            {
                ring_shown[i] = 1;
                char line[256];
                size_t n = (size_t) snprintf(line, sizeof(line), "Watchdog: thread %d, the last procedures (oldest first):", i);
                uint32_t k;

                for (k = 0; k < 16 && n < sizeof(line) - 12; k++)
                {
                    n += (size_t) snprintf(line + n, sizeof(line) - n, " %x", (unsigned int) st->ring[(procs + k) & 15]);
                }
                logprint(line);
            }
            else if (show && ticks == before_ticks[i] && procs - before_procs[i] > 1000000 && ring_running[i] < 3 && note == NULL)
            {
                /* runs but never gives up the big lock or blocks: a loop that waits for another thread */
                char line[256];
                size_t n = (size_t) snprintf(line, sizeof(line), "Watchdog: thread %d runs without a pause, the last procedures (oldest first):", i);
                uint32_t k;

                ring_running[i]++;
                for (k = 0; k < 16 && n < sizeof(line) - 12; k++)
                {
                    n += (size_t) snprintf(line + n, sizeof(line) - n, " %x", (unsigned int) st->ring[(procs + k) & 15]);
                }
                logprint(line);
            }
            before_procs[i] = procs;
            before_imports[i] = imports;
            before_ticks[i] = ticks;
        }
    }
    return NULL;
}
#endif

/* a thread gave up the big lock after more than 40 ms: what it was doing (category threads) */
/* the longest hold of the big lock by a thread other than the game thread since the last SRW_TakeLongHold (for the slow frame report) */
static volatile double long_hold_ms, long_hold_end, holds_total_ms;
static volatile uint32_t holds_count;
static char long_hold_text[120];

/* the longest hold, and how many holds of more than 10 ms there were and their time in all */
int SRW_TakeLongHold(double *milliseconds, double *ended, char *text, size_t size, uint32_t *count, double *total)
{
    int found = long_hold_ms > 0;

    *milliseconds = long_hold_ms;
    *ended = long_hold_end;
    *count = holds_count;
    *total = holds_total_ms;
    snprintf(text, size, "%s", long_hold_text);
    long_hold_ms = 0;
    holds_count = 0;
    holds_total_ms = 0;
    return found;
}

void SRW_GilHeldLong(double milliseconds)
{
    static int reports;
    ThreadStat *st = my_stat != NULL ? my_stat : thread_stat();

    if (!SRW_GilIsGame() && st != NULL)
    {
        holds_count++;
        holds_total_ms += milliseconds;
    }
    if (!SRW_GilIsGame() && milliseconds > long_hold_ms && st != NULL)
    {
        long_hold_ms = milliseconds;
        long_hold_end = emscripten_get_now();
        snprintf(long_hold_text, sizeof(long_hold_text), "last import %s from %x", st->last_import < SRW_ImportCount ? SRW_ImportNames[st->last_import] : "?", (unsigned int) st->last_caller);
    }

    if (SRW_LOG_ON(SRW_LOG_THREADS) && reports++ < 60 && st != NULL)
        logprintf("Threads: a thread kept the big lock for %.0f ms; its last import %s from %x, procedure %x", milliseconds,
                  st->last_import < SRW_ImportCount ? SRW_ImportNames[st->last_import] : "?", (unsigned int) st->last_caller, (unsigned int) st->last_proc);
}

/* what the other threads are doing now (diagnostics: the sleeps of the game thread): adds one to the count of the note each one is in (NULL: it runs
   or waits for the big lock); names/counts hold up to max different notes, n is how many there are */
int SRW_SampleOtherThreads(const char **names, int *counts, int n, int max)
{
    int i, k;

    for (i = 0; i < MAX_THREAD_STATS; i++)
    {
        ThreadStat *st = &thread_stats[i];
        const char *note;

        if (!st->used || st == my_stat) continue;
        note = st->note;
        for (k = 0; k < n && !(names[k] == note || (names[k] != NULL && note != NULL && strcmp(names[k], note) == 0)); k++) {}
        if (k == n)
        {
            if (n == max) continue;
            names[n] = note;
            counts[n++] = 0;
        }
        counts[k]++;
    }
    return n;
}

void SRW_Note(const char *what, uint32_t arg)
{
    ThreadStat *st = my_stat != NULL ? my_stat : thread_stat();

    if (st != NULL)
    {
        st->note = what;
        st->note_arg = arg;
        st->note_timeout = 0xffffffffu;
        st->note_time = emscripten_get_now();
        st->ticks++;
    }
}

/* the guest address that called the import that runs now (diagnostics build; 0 otherwise) */
uint32_t SRW_LastCaller(void)
{
    ThreadStat *st = my_stat != NULL ? my_stat : thread_stat();

    return st != NULL ? st->last_caller : 0;
}

/* the timeout of the wait just noted (SRW_Note) */
void SRW_NoteTimeout(uint32_t milliseconds)
{
    ThreadStat *st = my_stat != NULL ? my_stat : thread_stat();

    if (st != NULL) st->note_timeout = milliseconds;
}

/* the calling thread's last procedures entered, newest first (all zero unless built with traceprocs=1) */
void SRW_RecentProcs(uint32_t *out, int count)
{
    ThreadStat *st = my_stat != NULL ? my_stat : thread_stat();
    int i;

    for (i = 0; i < count; i++) out[i] = (st != NULL && st->procs > (uint32_t) i) ? st->ring[(st->procs - 1 - (uint32_t) i) & 15] : 0;
}

void CCALL SRW_Proc_c(uint32_t address)
{
    {
        ThreadStat *st = my_stat != NULL ? my_stat : thread_stat();

        if (st != NULL)
        {
            st->last_proc = address;
            st->ring[st->procs & 15] = address;
            st->procs++;
        }
    }
    if (!SRW_GilHeld())
    {
        static int reported;

        if (reported++ < 5) fprintf(stderr, "GIL: guest code runs without the big lock at %x\n", (unsigned int) address);
    }
#ifdef SRW_SNAPSHOT_ADDRESS
    snapshot_hook(address);
#endif
    proc_trace[proc_trace_count++ % PROC_TRACE_SIZE] = address;

#ifdef SRW_WATCH_MEMORY
    {
        static uint32_t last_value;
        static uint32_t last_proc;
        uint32_t now = *(volatile uint32_t *) SRW_WATCH_MEMORY;

        if (now != last_value)
        {
            fprintf(stderr, "MEMORY %x changed from %x to %x between procedures %x and %x\n", (unsigned int) SRW_WATCH_MEMORY, (unsigned int) last_value,
                    (unsigned int) now, (unsigned int) last_proc, (unsigned int) address);
            last_value = now;
        }
        last_proc = address;
    }
#endif
#ifdef SRW_HEARTBEAT
    /* a thread that keeps running procedures shows where every so often */
    if ((proc_trace_count & 0x3ffffff) == 0)
    {
        fprintf(stderr, "HEARTBEAT (%u procedures entered by this thread)\n", (unsigned int) proc_trace_count);
        SRW_DumpProcTrace();
    }
#endif
#ifdef SRW_WATCH_ENTRY
    if (address == SRW_WATCH_ENTRY)
    {
        static int shown;
        _cpu *cpu = x86_initialize_cpu();

        if (shown++ < 3)
            fprintf(stderr, "ENTRY %x eax=%x ebx=%x ecx=%x edx=%x esi=%x edi=%x ebp=%x esp=%x ret=%x\n", (unsigned int) address, (unsigned int) eax, (unsigned int) ebx,
                    (unsigned int) ecx, (unsigned int) edx, (unsigned int) esi, (unsigned int) edi, (unsigned int) ebp, (unsigned int) esp,
                    (unsigned int) *(uint32_t *) REG2PTR(esp));
#ifdef SRW_WATCH_DEREF
        if (shown <= 3)
        {
            uint32_t k;

            fprintf(stderr, "ENTRY #%d [ebx+%x]:", shown, SRW_WATCH_DEREF);
            for (k = 0; k < 16; k++) fprintf(stderr, " %x", (unsigned int) *(uint32_t *) REG2PTR(ebx + SRW_WATCH_DEREF + 4 * k));
            fprintf(stderr, "\n");
        }
#endif
    }
#endif
#ifdef SRW_PRINT_PROCS
    /* for faults that stop the thread without a chance to dump the ring buffer */
#ifdef SRW_PRINT_AFTER_ADDRESS
    /* printing starts when the given procedure has been entered the given number of times */
    static volatile uint32_t armed_hits;

    if (address == SRW_PRINT_AFTER_ADDRESS && armed_hits < SRW_PRINT_AFTER_COUNT) armed_hits++;
    if (armed_hits >= SRW_PRINT_AFTER_COUNT)
#endif
    fprintf(stderr, "proc %x t%x\n", (unsigned int) address, (unsigned int) pthread_self());
#endif
}

void SRW_DumpProcTrace(void)
{
    char line[PROC_TRACE_SIZE * 9 + 64];
    uint32_t first, i, length;

    if (proc_trace_count == 0) return;

    first = (proc_trace_count > PROC_TRACE_SIZE) ? proc_trace_count - PROC_TRACE_SIZE : 0;
    length = snprintf(line, sizeof(line), "The last procedures entered:");
    for (i = first; i < proc_trace_count; i++)
    {
        length += snprintf(line + length, sizeof(line) - length, " %x", (unsigned int) proc_trace[i % PROC_TRACE_SIZE]);
    }
    logprint(line);
}

/* an import is reported the first time it is called (all calls with printprocs) */
/* a Direct3D method without an implementation: the program is told the feature is not available */
uint32_t CCALL SRW_D3DStub_c(uint32_t id)
{
    static volatile uint8_t seen[1024];

    if (id < sizeof(seen) && !seen[id])
    {
        seen[id] = 1;
        logprintf("Direct3D method %s is not implemented", (id < SRW_ImportCount) ? SRW_ImportNames[id] : "?");
    }
    return 0x8876086au; /* D3DERR_NOTAVAILABLE */
}

/* The race detector for the parts of the runtime made for one thread: Direct3D (WebGL belongs to the thread that presents the frames)
   and the windows (one message queue, the window procedures run on the caller's thread). A call of those from another thread is
   reported once per function (with the guest caller): without the big lock it would run in parallel with the game thread. */
uint32_t CCALL GetCurrentThreadId_c(void);

static int game_only(const char *name)
{
    static const char *const window_functions[] = {
        "CreateWindowExA", "CreateWindowExW", "DestroyWindow", "SetWindowLongA", "SetWindowLongW", "GetWindowLongA", "GetWindowLongW",
        "SendMessageA", "SendMessageW", "DispatchMessageA", "DispatchMessageW", "PeekMessageA", "PeekMessageW", "GetMessageA", "GetMessageW",
        "SetTimer", "KillTimer", "ShowWindow", "SetWindowPos", "MoveWindow", "CallWindowProcA", "CallWindowProcW", "DefWindowProcA",
        "DefWindowProcW", "SetWindowTextA", "SetWindowTextW", "RegisterClassA", "RegisterClassW", "RegisterClassExA", "RegisterClassExW",
        "UpdateWindow", "SetFocus", "SetCapture", "ReleaseCapture", "ClipCursor", NULL
    };
    int i;

    if (strncmp(name, "IDirect3D", 9) == 0) return 1;
    for (i = 0; window_functions[i] != NULL; i++)
    {
        if (strcmp(name, window_functions[i]) == 0) return 1;
    }
    return 0;
}

static void check_affinity(uint32_t id, uint32_t caller)
{
    static uint8_t kind[4096];   /* per import: 0 not looked at, 1 any thread, 2 the game thread only, 3 reported */
    static volatile uint32_t game_thread;
    uint8_t k;

    if (id >= SRW_ImportCount || id >= 4096) return;
    if (SRW_GilIsGame())
    {
        if (game_thread == 0) game_thread = GetCurrentThreadId_c();
        return;
    }
    if (game_thread == 0) return;   /* (before the first frame the game thread is not known) */
    k = __atomic_load_n(&kind[id], __ATOMIC_RELAXED);
    if (k == 0)
    {
        k = game_only(SRW_ImportNames[id]) ? 2 : 1;
        __atomic_store_n(&kind[id], k, __ATOMIC_RELAXED);
    }
    if (k == 2 && __atomic_exchange_n(&kind[id], 3, __ATOMIC_RELAXED) == 2)
        logprintf("Race detector: %s called by thread %x (guest caller %x), not by the game thread %x", SRW_ImportNames[id], (unsigned int) GetCurrentThreadId_c(),
                  (unsigned int) caller, (unsigned int) game_thread);
}

void CCALL SRW_Trace_c(uint32_t id, uint32_t caller)
{
    static volatile uint8_t seen[1024];
    ThreadStat *st = my_stat != NULL ? my_stat : thread_stat();

    if (st != NULL)
    {
        if (st->cpu == NULL) st->cpu = x86_initialize_cpu();
        st->last_import = id;
        st->last_caller = caller;
        st->callers[(st->imports + 1) & 15] = caller;
        st->imports++;
    }

#ifdef SRW_PRINT_PROCS
    logprintf("import: %s", (id < SRW_ImportCount) ? SRW_ImportNames[id] : "?");
#endif
    check_affinity(id, caller);
    SRW_STRESS_POINT();
    (void) seen;
}

/* the image of the executable (data.ll): the program expects it at its own addresses */
extern char srw_image;

int main(int argc, char **argv)
{
    pthread_t thread;
    pthread_attr_t attr;

    (void) argc;
    (void) argv;

    if ((uintptr_t) &srw_image != 0x400000)
    {
        warnprintf("The image of the program is at %x instead of 400000, it cannot run", (unsigned int) (uintptr_t) &srw_image);
        return 1;
    }

    SRW_VaInit();
    SRW_GilConfigure();
    SRW_CpuConfigure();
    SRW_ProgramStart();

#ifdef SRW_DIAGNOSTICS
    {
        pthread_t watchdog;
        pthread_attr_t watchdog_attr;

        pthread_attr_init(&watchdog_attr);
        pthread_attr_setdetachstate(&watchdog_attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&watchdog, &watchdog_attr, Watchdog, NULL);
    }
#endif

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* the game draws from its own thread into an OffscreenCanvas of its own (Direct3D 9 layer); every frame goes to the page as an
       ImageBitmap */
    if (pthread_create(&thread, &attr, GameThread, NULL) != 0)
    {
        warnprint("The game thread could not be created");
        return 1;
    }

    /* the runtime stays alive (EXIT_RUNTIME=0) while the game thread runs */
    return 0;
}
