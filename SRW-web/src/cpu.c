#include <emscripten.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "srw_debuglog.h"
#include "srw.h"
#include "imports.h"

/* the emulated stack of a guest thread; the program asks for 1 MB by default, threads get the same */
#define GUEST_STACK_SIZE (2 * 1024 * 1024)
#define CPU_AREA_SIZE 4096

static _Thread_local _cpu *thread_cpu = NULL;

uint32_t X86_InterruptFlag;

_cpu *x86_initialize_cpu(void)
{
    _cpu *cpu;
    void *stack_bottom;

    cpu = thread_cpu;
    if (cpu != NULL) return cpu;

    /* the cpu structure lives at the top of the block, the stack grows down below it */
    stack_bottom = calloc(1, GUEST_STACK_SIZE); /* a Windows stack starts out zeroed, and the program may rely on it */
    if (stack_bottom == NULL) exit(2);

    cpu = (_cpu *)(((uintptr_t)stack_bottom + GUEST_STACK_SIZE - CPU_AREA_SIZE) & ~(uintptr_t)127);
    cpu->stack_bottom = stack_bottom;
    cpu->stack_top = (void *)((uintptr_t)cpu & ~(uintptr_t)15);

    cpu->_st_top = 0;
    cpu->_st_sw_cond = 0;
    cpu->_st_cw = 0x037f;
    cpu->_mxcsr = 0x1f80;   /* all exceptions masked, round to nearest */
    cpu->_reserved1 = 0;

    esp = PTR2REG(cpu->stack_top);
    eflags = 0x3202;
    X86_InterruptFlag = 1;

    thread_cpu = cpu;
    return cpu;
}

void x86_deinitialize_cpu(void)
{
    _cpu *cpu;
    void *stack_bottom;

    cpu = thread_cpu;
    if (cpu == NULL) return;

    stack_bottom = cpu->stack_bottom;
    cpu->stack_bottom = NULL;
    thread_cpu = NULL;

    free(stack_bottom);
}

/* Fibers. The program runs its real main on one (ConvertThreadToFiber, CreateFiberEx, SwitchToFiber, DeleteFiber). A guest thread is
   its registers and its stack in guest memory, and an import returns by popping its return address from that stack, so switching
   fibers is switching the stack pointer and the registers a function has to keep (ebx, esi, edi, ebp) inside SwitchToFiber: the
   glue then returns to wherever the other fiber called SwitchToFiber from. */
/* how many calls from the runtime into the program are active on this thread (the program's own calls do not nest on the native
   stack, these do: a fiber switch inside one would leave the native frame of the call waiting for a return that never comes) */
static _Thread_local unsigned int guest_depth;

#define FIBER_CANARY 0xfeedfaceu
#define FIBER_SEH_HEAD SRW_SEH_HEAD   /* fs:[0], the head of the exception chain: one dword in the image, so one per fiber to be saved */

typedef struct {
    uint32_t saved_sp, saved_bx, saved_si, saved_di, saved_bp, saved_seh;
    uint32_t *stack;   /* the memory of the stack, NULL for the fiber of a thread (it runs on the thread's own stack) */
    uint32_t size;
} Fiber;

static _Thread_local Fiber *current_fiber;

/* the guest stack the calling thread runs on now (its current fiber's, else its own): [*low, *high) */
void SRW_GuestStackBounds(uint32_t *low, uint32_t *high)
{
    Fiber *fiber = current_fiber;
    _cpu *cpu = thread_cpu;

    if (fiber != NULL && fiber->stack != NULL)
    {
        *low = PTR2REG(fiber->stack);
        *high = PTR2REG(fiber->stack) + fiber->size;
    }
    else if (cpu != NULL)
    {
        *low = PTR2REG(cpu->stack_bottom);
        *high = PTR2REG(cpu->stack_top);
    }
    else *low = *high = 0;
}

uint32_t CCALL ConvertThreadToFiber_c(void *lpParameter)
{
    Fiber *fiber;

    (void) lpParameter;
    if (current_fiber != NULL) return PTR2REG(current_fiber);
    fiber = calloc(1, sizeof(*fiber));
    current_fiber = fiber;
    return PTR2REG(fiber);
}

uint32_t CCALL CreateFiberEx_c(uint32_t dwStackCommitSize, uint32_t dwStackReserveSize, uint32_t dwFlags, void *lpStartAddress, void *lpParameter)
{
    Fiber *fiber = calloc(1, sizeof(*fiber));
    uint32_t size = dwStackReserveSize ? dwStackReserveSize : dwStackCommitSize ? dwStackCommitSize : 1024 * 1024;
    uint32_t *top;

    (void) dwFlags;
    if (size < 256 * 1024) size = 256 * 1024;
    fiber->size = size;
    fiber->stack = calloc(1, size);
    for (int i = 0; i < 4; i++) fiber->stack[i] = FIBER_CANARY;   /* the end of the stack: a program that runs into it is told */
    top = (uint32_t *) (((uintptr_t) fiber->stack + size - 64) & ~(uintptr_t) 15);
    /* what SwitchToFiber's return will find: the start address (popped by the return), the argument slot (dropped by `ret 4`),
       then the function sees its own return address (none) and its parameter */
    top[0] = (uint32_t) (uintptr_t) lpStartAddress;
    top[1] = 0;
    top[2] = 0;
    top[3] = (uint32_t) (uintptr_t) lpParameter;
    fiber->saved_sp = PTR2REG(top);
    return PTR2REG(fiber);
}

uint32_t CCALL SwitchToFiber_c(void *lpFiber)
{
    _cpu *cpu = thread_cpu;
    Fiber *from = current_fiber, *to = lpFiber;

    if (from == NULL || to == NULL)
    {
        warnprintf("SwitchToFiber(%x) without a fiber for the thread", (unsigned int) PTR2REG(lpFiber));
        return 0;
    }
    if (from == to) return 0;
    if (guest_depth != 0) warnprintf("SwitchToFiber inside a call from the runtime into the program (depth %u): not supported", guest_depth);
    if (from->stack != NULL)
    {
        uintptr_t sp = esp;

        if (sp < (uintptr_t) from->stack || sp >= (uintptr_t) from->stack + from->size || from->stack[0] != FIBER_CANARY || from->stack[3] != FIBER_CANARY)
            warnprintf("The stack of the fiber %x overflowed (esp %x, stack %x..%x)", (unsigned int) PTR2REG(from), (unsigned int) sp,
                       (unsigned int) PTR2REG(from->stack), (unsigned int) (PTR2REG(from->stack) + from->size));
    }
    from->saved_sp = esp;
    from->saved_bx = ebx;
    from->saved_si = esi;
    from->saved_di = edi;
    from->saved_bp = ebp;
    from->saved_seh = *(uint32_t *) REG2PTR(FIBER_SEH_HEAD);
    esp = to->saved_sp;
    ebx = to->saved_bx;
    esi = to->saved_si;
    edi = to->saved_di;
    ebp = to->saved_bp;
    *(uint32_t *) REG2PTR(FIBER_SEH_HEAD) = to->saved_seh;
    current_fiber = to;
    return 0;
}

uint32_t CCALL DeleteFiber_c(void *lpFiber)
{
    Fiber *fiber = lpFiber;

    if (fiber == NULL || fiber == current_fiber) return 0;
    free(fiber->stack);
    free(fiber);
    return 0;
}

/* software interrupts of the recompiled program: int 3 appears in a debugger check; nothing else is expected */
// The segment FS: the thread information block. fs:[0x2c] is the array of the thread's TLS blocks, indexed by the program's TLS
// index (always 0), fs:[0x18] its own address. fs:[0] (the exception chain) is patched out of the code by prepare_exe.py. One small
// block per thread, allocated on first use (a static array would move the image of the program).
#define TEB_SIZE 0x1000
#define TLS_BLOCK_SIZE 0x100   // the program's .tls section is 0x18 bytes of zeros

static _Thread_local uint8_t *thread_block;

static uint8_t *fs_block(void)
{
    if (thread_block == NULL)
    {
        uint32_t *teb = calloc(1, TEB_SIZE + 16 + TLS_BLOCK_SIZE);
        uint32_t *slots = teb + TEB_SIZE / 4;   // slot 0 -> the block behind it

        slots[0] = (uint32_t)(uintptr_t) (slots + 4);
        teb[0x18 / 4] = (uint32_t)(uintptr_t) teb;
        teb[0x2c / 4] = (uint32_t)(uintptr_t) slots;
        thread_block = (uint8_t *) teb;
    }
    return thread_block;
}

uint32_t CCALL X86_ReadFsDword(uint32_t addr)
{
    return addr <= TEB_SIZE - 4 ? *(uint32_t *) (fs_block() + addr) : 0;
}

void CCALL X86_WriteFsDword(uint32_t addr, uint32_t value)
{
    if (addr <= TEB_SIZE - 4) *(uint32_t *) (fs_block() + addr) = value;
}

void CCALL X86_InterruptProcedure(const uint8_t IntNum, void *regs)
{
    static int reported = 0;

    (void) regs;

    if (IntNum != 3 && !reported)
    {
        reported = 1;
        warnprintf("Int 0x%02x is not supported", (unsigned int) IntNum);
    }
}

/* port I/O only appears in dead paths of the program (it runs in user mode); reads give all ones, writes are dropped */
uint32_t CCALL X86_InPortProcedure(const uint16_t PortNum, const uint32_t PortSize)
{
    (void)PortNum;
    return (PortSize >= 4) ? 0xffffffffu : (PortSize == 2) ? 0xffffu : 0xffu;
}

void CCALL X86_OutPortProcedure(const uint16_t PortNum, const uint32_t PortSize, const uint32_t _eax)
{
    (void)PortNum;
    (void)PortSize;
    (void)_eax;
}

uint32_t SRW_CallGuest(uint32_t address, unsigned int argument_count, const uint32_t *arguments)
{
    _cpu *cpu = x86_initialize_cpu();
    uint32_t saved_esp = esp;
    unsigned int i;
    uint32_t result;

    /* stdcall: arguments pushed right to left; the callee pops them, the return address is pushed by the dispatcher */
    for (i = argument_count; i > 0; i--)
    {
        esp -= 4;
        *(uint32_t *)REG2PTR(esp) = arguments[i - 1];
    }
    eax = address;
    guest_depth++;
    c_SRW_CallGuest(cpu);
    guest_depth--;
    result = eax;
    esp = saved_esp;
    return result;
}

/* a fastcall (MSVC: the first two arguments in ecx and edx, the rest on the stack, popped by the callee) */
uint32_t SRW_CallGuestFastcallArgs(uint32_t address, uint32_t first, uint32_t second, unsigned int argument_count, const uint32_t *arguments)
{
    _cpu *cpu = x86_initialize_cpu();
    uint32_t saved_esp = esp, result;
    unsigned int i;

    for (i = argument_count; i > 0; i--)
    {
        esp -= 4;
        *(uint32_t *)REG2PTR(esp) = arguments[i - 1];
    }
    ecx = first;
    edx = second;
    eax = address;
    guest_depth++;
    c_SRW_CallGuest(cpu);
    guest_depth--;
    result = eax;
    esp = saved_esp;
    return result;
}

uint32_t SRW_CallGuestFastcall(uint32_t address, uint32_t first, uint32_t second)
{
    _cpu *cpu = x86_initialize_cpu();
    uint32_t saved_esp = esp, result;

    ecx = first;
    edx = second;
    eax = address;
    c_SRW_CallGuest(cpu);
    result = eax;
    esp = saved_esp;
    return result;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* The big lock: with it (/biglock.txt 1, the setting bigLock) only one thread runs guest code at a time; it is given up whenever a
   thread blocks or polls, so threads interleave at library calls, in first-come first-served order.

   Without it (the default) the guest threads run in parallel as on Windows: the program's locked instructions are WebAssembly
   atomics (x86_lock_rmw, x86_cmpxchg32...), and the runtime guards its own state with locks of its own per area. Then every
   function here does nothing. The choice is made once at the start (SRW_GilConfigure). */
static int gil_enabled = 0;

void SRW_GilConfigure(void)
{
    FILE *f = fopen("/Port/biglock.txt", "r");
    int value;

    if (f != NULL)
    {
        if (fscanf(f, "%d", &value) == 1) gil_enabled = value != 0;
        fclose(f);
    }
    logprintf("Big lock %s", gil_enabled ? "on (one thread runs the program at a time)" : "off (the threads of the program run in parallel)");
#ifdef SRW_STRESS
    SRW_StressConfigure();
#endif
}

/* What cpuid reports: /sse.txt 1 (the setting sse) tells the program about MMX, SSE and SSE2 (llasm_sse.c), else no features.
   Read once at the start. */
extern uint32_t x86_cpu_features;

void SRW_CpuConfigure(void)
{
    FILE *f = fopen("/Port/sse.txt", "r");
    int value = 1;   /* on by default since 2026-10-09 (the user: the game expects it, and it runs faster); /sse.txt 0 turns it off */

    if (f != NULL)
    {
        if (fscanf(f, "%d", &value) != 1) value = 1;
        fclose(f);
    }
    /* FPU, TSC, CX8, MMX, FXSR, SSE, SSE2 (not CMOV: the one cmovz in the program is not translated) */
    x86_cpu_features = value ? (1u << 0) | (1u << 4) | (1u << 8) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26) : 0;
    logprintf("Processor features: %s", value ? "MMX, SSE, SSE2 (translated by llasm_sse.c)" : "none (the program takes its paths without SSE)");
}

int SRW_GilEnabled(void)
{
    return gil_enabled;
}

#ifdef SRW_STRESS
/* The stress build (scons stress=1, never for publish/): random pauses at the entry of the imports and of the runtime's locks
   make the interleavings of the threads vary from run to run, so a race shows up in a few runs instead of once a week.
   /stress.txt: the probability in per mille of a pause at a call, and the longest pause in microseconds ("50 200"). */
static int stress_permille, stress_max_us;

void SRW_StressConfigure(void)
{
    FILE *f = fopen("/Port/stress.txt", "r");

    if (f != NULL)
    {
        if (fscanf(f, "%d %d", &stress_permille, &stress_max_us) < 1) stress_permille = 0;
        if (stress_max_us <= 0) stress_max_us = 200;
        fclose(f);
    }
    warnprintf("STRESS BUILD: random pauses at %d per mille of the calls, up to %d us", stress_permille, stress_max_us);
}

void SRW_StressPoint(void)
{
    static _Thread_local uint32_t seed;
    uint32_t r;

    if (stress_permille <= 0) return;
    if (seed == 0) seed = (uint32_t) (uintptr_t) &seed ^ (uint32_t) (emscripten_get_now() * 1000.0) ^ 0x9e3779b9u;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    r = seed;
    if ((int) (r % 1000u) >= stress_permille) return;
    r = (r >> 10) % (uint32_t) (stress_max_us + 1);
    if (r < 20) sched_yield();
    else usleep(r);
}
#endif

static pthread_mutex_t gil_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gil_changed = PTHREAD_COND_INITIALIZER;
/* normal threads queue by ticket; audio threads (SRW_GilSetAudio: the sound system mixes in real time and has to get in at once) wait
   apart and are let in before them, and the thread that holds the lock gives it up at its next poll when one waits */
static uint32_t gil_next_ticket, gil_serving;   /* the tickets of normal threads: the next to hand out, the next to be let in */
static int gil_busy;
static int gil_audio_waiting;
/* the "game first" mode (SRW_GilSetMode, the chat command .sched): the thread that presents the frames is let in before the ordinary threads (after the audio
   ones), and an ordinary thread that holds the lock gives it up at its next poll when the game thread waits. The game thread still lets the ordinary threads
   in when one has waited more than the budget (loading goes on at a lower speed instead of stopping while the game runs); gil_normal_turn is the
   game thread's offer to the one that waits. */
static volatile int gil_mode_game = 0;   /* off by default: .sched game starved the loader threads (zones loaded only partly); first come first served */
static double gil_budget_ms = 8.0;       /* the game thread lets a waiting worker in after this long (when it does not block by itself) */
static double gil_min_slice_ms = 2.0;    /* a worker keeps the lock this long before it gives it to the waiting game thread: its slices must not shrink to nothing */
static int gil_game_waiting, gil_normal_waiting, gil_normal_turn;
static double gil_phase_ms = 6.0;       /* a turn of the worker threads lasts until all that wait have run, at most this long */
static double gil_turn_start;
static volatile double gil_normal_since;   /* when the first of the waiting ordinary threads began to wait */

void SRW_GilSetMode(int game_first, double budget_ms, double min_slice_ms)
{
    gil_mode_game = game_first;
    if (budget_ms > 0) gil_budget_ms = budget_ms;
    if (min_slice_ms > 0) gil_min_slice_ms = min_slice_ms;
}

int SRW_GilGetMode(void)
{
    return gil_mode_game;
}
static _Thread_local int gil_held;
static _Thread_local int gil_audio;
static _Thread_local int gil_game;   /* the thread that presents the frames (SRW_GilNoteGameThread) */
static _Thread_local uint32_t gil_polls;
static double gil_hold_start;   /* when the thread that holds the lock got it */
#ifdef SRW_DIAGNOSTICS
extern void SRW_GilHeldLong(double milliseconds);
/* the time the threads other than the game thread held the lock since the last SRW_TakeSliceStats (only the holder writes these) */
static double slice_total, slice_max;
static uint32_t slice_count;
/* slices by thread (the tag is the start procedure of the thread, 0 for the main thread) and the gaps between a release and the acquire that waited for it */
static struct { uint32_t tag; uint32_t slices; double ms; } slice_threads[8];
static double last_release_ms, gap_total;
static uint32_t gap_count;
static _Thread_local uint32_t gil_tag;
static _Thread_local int gil_role;   /* SRW_ThreadRole of the thread's start procedure */
/* where the worker threads were when they gave up the lock: the time of the slice by the guest address of the call that ended it (256 byte buckets) */
static struct { uint32_t place; double ms; } where_table[64];
extern uint32_t SRW_LastCaller(void);

/* a profile of the worker threads while .prof is on: the time between two polls of a thread goes to the guest address of the later poll (16 byte buckets) */
static struct { uint32_t place; double ms; } hot_table[1024];
static _Thread_local double poll_last_ms;

int SRW_TakeHot(uint32_t *places, double *ms, int max)
{
    int i, k, n = 0;

    for (i = 0; i < 1024; i++)
    {
        if (hot_table[i].ms == 0) continue;
        for (k = n < max ? n : max - 1; k > 0 && hot_table[i].ms > ms[k - 1]; k--)
        {
            places[k] = places[k - 1];
            ms[k] = ms[k - 1];
        }
        if (k < max && (n < max || hot_table[i].ms > ms[max - 1]))
        {
            places[k] = hot_table[i].place;
            ms[k] = hot_table[i].ms;
            if (n < max) n++;
        }
    }
    memset(hot_table, 0, sizeof(hot_table));
    return n;
}

static void note_poll(void)
{
    double now = emscripten_get_now();
    uint32_t place = SRW_LastCaller();
    unsigned slot, tries;

    place &= ~0xfu;
    slot = (place >> 4) & 1023;

    if (poll_last_ms != 0)
    {
        for (tries = 0; tries < 8 && hot_table[slot].ms != 0 && hot_table[slot].place != place; tries++) slot = (slot + 1) & 1023;
        hot_table[slot].place = place;
        hot_table[slot].ms += now - poll_last_ms;
    }
    poll_last_ms = now;
}

int SRW_TakeWhere(uint32_t *places, double *ms, int max)
{
    int i, k, n = 0;

    for (i = 0; i < 64 && where_table[i].ms != 0; i++)
    {
        for (k = n < max ? n : max - 1; k > 0 && where_table[i].ms > ms[k - 1]; k--)
        {
            places[k] = places[k - 1];
            ms[k] = ms[k - 1];
        }
        if (k < max && (n < max || where_table[i].ms > ms[max - 1]))
        {
            places[k] = where_table[i].place;
            ms[k] = where_table[i].ms;
            if (n < max) n++;
        }
    }
    memset(where_table, 0, sizeof(where_table));
    return n;
}

void SRW_GilSetTag(uint32_t tag)
{
    gil_tag = tag;
    gil_role = SRW_ThreadRole(tag);
}

/* the three busiest threads since the last call, and the gaps: how many handoffs of the lock that a thread waited for, the time between the release and the
   moment the waiting thread had the lock (in all) */
int SRW_TakeThreadSlices(uint32_t *tags, uint32_t *slices, double *ms, int max, uint32_t *gaps, double *gap_ms)
{
    int i, k, n = 0;

    for (i = 0; i < 8 && slice_threads[i].slices != 0; i++)
    {
        for (k = n; k > 0 && slice_threads[i].ms > ms[k - 1]; k--)
        {
            if (k < max) { tags[k] = tags[k - 1]; slices[k] = slices[k - 1]; ms[k] = ms[k - 1]; }
        }
        if (k < max)
        {
            tags[k] = slice_threads[i].tag; slices[k] = slice_threads[i].slices; ms[k] = slice_threads[i].ms;
            if (n < max) n++;
        }
    }
    *gaps = gap_count;
    *gap_ms = gap_total;
    memset(slice_threads, 0, sizeof(slice_threads));
    gap_count = 0;
    gap_total = 0;
    return n;
}

void SRW_TakeSliceStats(double *total, uint32_t *count, double *longest)
{
    *total = slice_total;
    *count = slice_count;
    *longest = slice_max;
    slice_total = slice_max = 0;
    slice_count = 0;
}
#else
void SRW_GilSetTag(uint32_t tag) { (void) tag; }   /* (the lock's statistics are kept in the diagnostics build only) */
#endif

int SRW_GilHeld(void)
{
    return gil_held;
}

/* One clock for all threads: the latest time seen by any thread is the floor for all, because the workers of a page read
   performance.now() with a correction of their own and the program subtracts time stamps taken by different threads. */
uint64_t SRW_NowNs(void)
{
    static volatile uint64_t latest;
    uint64_t now = (uint64_t) (emscripten_get_now() * 1000000.0), seen = __atomic_load_n(&latest, __ATOMIC_RELAXED);

    while (now > seen)
    {
        if (__atomic_compare_exchange_n(&latest, &seen, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return now;
    }
    return seen;
}

/* A wait of more than a day and a half is a negative delay that wrapped around (the program never means that): wait a millisecond */
uint32_t SRW_SaneTimeout(uint32_t milliseconds, const char *what)
{
    static int reports;

    if (milliseconds == 0xffffffffu || milliseconds < 0x08000000u) return milliseconds;
    if (SRW_LOG_ON(SRW_LOG_THREADS) && reports++ < 10) logprintf("%s of %u ms treated as 1 ms (a delay that went negative)", what, (unsigned int) milliseconds);
    return 1;
}

/* per thread: milliseconds spent sleeping, waiting for objects and waiting for the big lock (reported with the Direct3D calls) */
static _Thread_local double blocked_ms[3];

void SRW_BlockedAdd(int which, double ms)
{
    blocked_ms[which] += ms;
}

void SRW_BlockedPeek(double *out)
{
    int i;

    for (i = 0; i < 3; i++) out[i] = blocked_ms[i];
}

void SRW_BlockedTake(double *out)
{
    int i;

    for (i = 0; i < 3; i++)
    {
        out[i] = blocked_ms[i];
        blocked_ms[i] = 0;
    }
}

void SRW_GilSetAudio(void)
{
    gil_audio = 1;
}

/* the calling thread presents the frames (the game first mode lets it in before the worker threads) */
void SRW_GilNoteGameThread(void)
{
    gil_game = 1;
}

int SRW_GilIsGame(void)
{
    return gil_game;
}

int SRW_GilIsAudio(void)
{
    return gil_audio;
}

/* the conditions to wait for (gil_mutex is held) */
static int blocked_normal(uint32_t ticket)
{
    return gil_busy || gil_audio_waiting > 0 || ticket != gil_serving || (gil_mode_game && gil_game_waiting > 0 && !gil_normal_turn);
}
static int blocked_game_first(uint32_t ticket)
{
    (void) ticket;
    return gil_busy || gil_audio_waiting > 0 || gil_normal_turn;
}

void SRW_GilAcquire(void)
{
    double start;
    int waited_for_lock;

    if (!gil_enabled) return;
    start = emscripten_get_now();

    SRW_PROF_ENTER(SRW_PROF_LOCK);

    pthread_mutex_lock(&gil_mutex);
    waited_for_lock = gil_busy;
    if (gil_audio)
    {
        gil_audio_waiting++;
        while (gil_busy) pthread_cond_wait(&gil_changed, &gil_mutex);
        gil_audio_waiting--;
    }
    else if (gil_game && gil_mode_game)
    {
        /* first among the ordinary threads: no ticket; an offer of the turn that nobody takes within 5 ms is withdrawn */
        gil_game_waiting++;
        while (blocked_game_first(0))
        {
            struct timespec limit;

            clock_gettime(CLOCK_REALTIME, &limit);
            limit.tv_nsec += 5 * 1000000;
            if (limit.tv_nsec >= 1000000000) { limit.tv_sec++; limit.tv_nsec -= 1000000000; }
            if (pthread_cond_timedwait(&gil_changed, &gil_mutex, &limit) == ETIMEDOUT && !gil_busy && gil_audio_waiting == 0) gil_normal_turn = 0;
        }
        gil_game_waiting--;
    }
    else
    {
        uint32_t ticket = gil_next_ticket++;

        if (!gil_game)
        {
            if (gil_normal_waiting++ == 0) gil_normal_since = emscripten_get_now();
        }
        while (blocked_normal(ticket)) pthread_cond_wait(&gil_changed, &gil_mutex);
        gil_serving++;
        if (!gil_game)
        {
            gil_normal_waiting--;
            gil_normal_since = emscripten_get_now();   /* (the next waiter has waited from now at the least) */
        }
    }
    gil_busy = 1;
    pthread_mutex_unlock(&gil_mutex);
#ifdef SRW_DIAGNOSTICS
    if (waited_for_lock && last_release_ms != 0)
    {
        gap_total += emscripten_get_now() - last_release_ms;
        gap_count++;
    }
#endif
    SRW_PROF_LEAVE();
    blocked_ms[2] += emscripten_get_now() - start;
    gil_held = 1;
    gil_hold_start = emscripten_get_now();
#ifdef SRW_DIAGNOSTICS
    poll_last_ms = gil_hold_start;
#endif
    SRW_Note(NULL, 0);
}

void SRW_GilRelease(void)
{
    if (!gil_enabled) return;
#ifdef SRW_DIAGNOSTICS
    if (gil_held)
    {
        double held = emscripten_get_now() - gil_hold_start;

        if (!gil_game)
        {
            int t;

            slice_total += held;
            slice_count++;
            if (held > slice_max) slice_max = held;
            for (t = 0; t < 7 && slice_threads[t].slices != 0 && slice_threads[t].tag != gil_tag; t++) {}
            slice_threads[t].tag = gil_tag;
            slice_threads[t].slices++;
            slice_threads[t].ms += held;
            if (gil_role != SRW_THREAD_AUDIO)
            {
                uint32_t place = SRW_LastCaller() & ~0xffu;

                for (t = 0; t < 63 && where_table[t].ms != 0 && where_table[t].place != place; t++) {}
                where_table[t].place = place;
                where_table[t].ms += held;
            }
        }
        if (held > 10.0) SRW_GilHeldLong(held);
    }
    last_release_ms = emscripten_get_now();
#endif
    gil_held = 0;
    pthread_mutex_lock(&gil_mutex);
    gil_busy = 0;
    if (gil_normal_turn && !gil_game && (gil_normal_waiting == 0 || emscripten_get_now() - gil_turn_start > gil_phase_ms)) gil_normal_turn = 0;   /* the workers have run: the game thread again */
    pthread_cond_broadcast(&gil_changed);
    pthread_mutex_unlock(&gil_mutex);
}

/* lets a waiting thread run, if there is one (in the game first mode: the game thread lets an ordinary one in only after the budget) */
void SRW_GilYield(void)
{
    int waiting;

    if (!gil_held) return;
    pthread_mutex_lock(&gil_mutex);
    if (gil_mode_game && gil_game && gil_audio_waiting == 0)
    {
        waiting = gil_normal_waiting > 0 && emscripten_get_now() - gil_normal_since > gil_budget_ms;
        if (waiting) { gil_normal_turn = 1; gil_turn_start = emscripten_get_now(); }
    }
    else waiting = gil_audio_waiting > 0 || gil_next_ticket != gil_serving || (gil_mode_game && gil_game_waiting > 0 && !gil_normal_turn);
    pthread_mutex_unlock(&gil_mutex);
    if (waiting)
    {
        SRW_GilRelease();
        SRW_GilAcquire();
    }
}

/* for calls that a program uses to poll (the time, message queues): yield now and then */
void SRW_GilPoll(void)
{
    SRW_SafePoint(0);
    if (!gil_held) return;
#ifdef SRW_DIAGNOSTICS
    if (SRW_ProfOn && !gil_game && gil_role == SRW_THREAD_WATCHED) note_poll();
#endif
    if (__atomic_load_n(&gil_audio_waiting, __ATOMIC_RELAXED) > 0) { SRW_GilYield(); return; }
    if (gil_mode_game)
    {
        if (gil_game)
        {
            if ((++gil_polls & 3) == 0 && __atomic_load_n(&gil_normal_waiting, __ATOMIC_RELAXED) > 0) SRW_GilYield();
            return;
        }
        if (__atomic_load_n(&gil_game_waiting, __ATOMIC_RELAXED) > 0 && !__atomic_load_n(&gil_normal_turn, __ATOMIC_RELAXED))
        {
            if (emscripten_get_now() - gil_hold_start >= gil_min_slice_ms) SRW_GilYield();
            return;
        }
    }
    if ((++gil_polls & 15) == 0) SRW_GilYield();
}

/* ends the calling thread for good */
void SRW_ThreadExit(void)
{
    if (gil_held) SRW_GilRelease();
    pthread_exit(NULL);
}
