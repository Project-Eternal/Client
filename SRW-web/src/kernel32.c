/* kernel32: process, memory, synchronization, time and locale functions */
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "srw_remote.h"
#include <emscripten.h>

#include "srw_debuglog.h"
#include "srw.h"
#include "imports.h"
#include "srw_object.h"
#include "srw_win32.h"

/* ------------------------------------------------------------------------------------------------------------------ */
/* last error */

static _Thread_local uint32_t last_error;

void SRW_SetLastError(uint32_t error)
{
    last_error = error;
}

uint32_t CCALL GetLastError_c(void)
{
    /* the program polls with cheap calls like this one: a thread waiting for the big lock gets its turn */
    SRW_GilPoll();
    return last_error;
}

uint32_t CCALL SetLastError_c(uint32_t dwErrCode)
{
    last_error = dwErrCode;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* process and version */

uint32_t CCALL GetVersion_c(void)
{
    /* Windows XP: 5.1, build 2600 */
    return (2600u << 16) | (1u << 8) | 5u;
}

uint32_t CCALL GetVersionExA_c(void *lpVersionInformation)
{
    SRW_OSVERSIONINFOA *info = lpVersionInformation;

    if (info == NULL || info->dwOSVersionInfoSize < sizeof(SRW_OSVERSIONINFOA)) return 0;

    info->dwMajorVersion = 5;
    info->dwMinorVersion = 1;
    info->dwBuildNumber = 2600;
    info->dwPlatformId = 2; /* VER_PLATFORM_WIN32_NT */
    info->szCSDVersion[0] = 0;
    return 1;
}

static const char command_line[] = "\"" SRW_PROGRAM_EXE "\"";
static const char module_path[] = SRW_PROGRAM_EXE;

uint32_t CCALL GetCommandLineA_c(void)
{
    return PTR2REG(command_line);
}

uint32_t CCALL GetModuleFileNameA_c(uint32_t hModule, void *lpFilename, uint32_t nSize)
{
    uint32_t length = sizeof(module_path) - 1;

    (void) hModule;

    if (nSize == 0) return 0;
    if (length >= nSize)
    {
        memcpy(lpFilename, module_path, nSize - 1);
        ((char *)lpFilename)[nSize - 1] = 0;
        SRW_SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
        return nSize;
    }
    memcpy(lpFilename, module_path, length + 1);
    return length;
}

uint32_t CCALL GetStartupInfoA_c(void *lpStartupInfo)
{
    memset(lpStartupInfo, 0, 68);
    *(uint32_t *)lpStartupInfo = 68;
    return 0;
}

uint32_t CCALL GetCurrentProcessId_c(void)
{
    return 1000;
}

uint32_t CCALL GetCurrentProcess_c(void)
{
    return 0xffffffffu;
}

uint32_t CCALL GetCurrentThread_c(void)
{
    return 0xfffffffeu;
}

uint32_t CCALL GetCurrentThreadId_c(void)
{
    return (uint32_t)(uintptr_t)pthread_self();
}

uint32_t CCALL ExitProcess_c(uint32_t uExitCode)
{
    warnprintf("ExitProcess(%u)", (unsigned int) uExitCode);
    /* the page keeps what has to survive the exit (the program's settings, the embedded server's characters) now, not at its next round */
    MAIN_THREAD_ASYNC_EM_ASM({ if (Module['srwOnExit']) Module['srwOnExit']($0); }, uExitCode);
#ifdef SRW_DIAGNOSTICS
    SRW_LogGuestStack("ExitProcess");
#endif
    SRW_ThreadExit();
    return 0;
}

uint32_t CCALL SetUnhandledExceptionFilter_c(void *lpTopLevelExceptionFilter)
{
    (void) lpTopLevelExceptionFilter;
    return 0;
}

uint32_t CCALL UnhandledExceptionFilter_c(void *ExceptionInfo)
{
    (void) ExceptionInfo;
    return 0; /* EXCEPTION_CONTINUE_SEARCH */
}

uint32_t CCALL SetHandleCount_c(uint32_t uNumber)
{
    return uNumber;
}

uint32_t CCALL GetStdHandle_c(uint32_t nStdHandle)
{
    (void) nStdHandle;
    return 0xffffffffu; /* a program without a console has none */
}

uint32_t CCALL GetFileType_c(uint32_t hFile)
{
    /* the program's own loader of loose files (the HTML pages of the login screen) gives up on a handle that is not a disk file */
    static int reports;
    uint32_t type = SRW_ObjectIs(hFile, SRW_OBJ_FILE) ? 1 : 0; /* FILE_TYPE_DISK, FILE_TYPE_UNKNOWN */

    if (SRW_LOG_ON(SRW_LOG_FILES) && reports++ < 10) logprintf("GetFileType(%x) = %u", (unsigned int) hFile, (unsigned int) type);
    return type;
}

uint32_t CCALL SetErrorMode_c(uint32_t uMode)
{
    (void) uMode;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* heap and virtual memory: guest addresses are addresses of the linear memory */

#define SRW_PROCESS_HEAP 0x00100000u

/* what the program holds (for the memory report of scons diagnostics=1): bytes and blocks of the heap functions and of VirtualAlloc */
#ifdef SRW_DIAGNOSTICS
#define SRW_STAT_ADD(counter, n) atomic_fetch_add(&counter, n)
#define SRW_STAT_SUB(counter, n) atomic_fetch_sub(&counter, n)
#else
#define SRW_STAT_ADD(counter, n) ((void) 0)
#define SRW_STAT_SUB(counter, n) ((void) 0)
#endif
static _Atomic int64_t stat_heap_bytes, stat_heap_blocks, stat_virtual_bytes, stat_virtual_blocks, stat_heap_allocs, stat_virtual_allocs;

void SRW_MemoryStats(int64_t *out)
{
    out[0] = stat_heap_bytes; out[1] = stat_heap_blocks; out[2] = stat_virtual_bytes; out[3] = stat_virtual_blocks;
    out[4] = stat_heap_allocs; out[5] = stat_virtual_allocs;
}

static uint32_t next_heap_handle = 0x00100010u;

uint32_t CCALL GetProcessHeap_c(void)
{
    return SRW_PROCESS_HEAP;
}

uint32_t CCALL HeapCreate_c(uint32_t flOptions, uint32_t dwInitialSize, uint32_t dwMaximumSize)
{
    (void) flOptions; (void) dwInitialSize; (void) dwMaximumSize;
    return atomic_fetch_add((_Atomic uint32_t *)&next_heap_handle, 16);
}

uint32_t CCALL HeapDestroy_c(uint32_t hHeap)
{
    (void) hHeap; /* the blocks are not tracked per heap */
    return 1;
}

uint32_t CCALL HeapAlloc_c(uint32_t hHeap, uint32_t dwFlags, uint32_t dwBytes)
{
    void *block;

    (void) hHeap;

    block = (dwFlags & 8) ? calloc(1, dwBytes ? dwBytes : 1) : malloc(dwBytes ? dwBytes : 1);
    if (block == NULL)
    {
        SRW_SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    }
    else
    {
        SRW_STAT_ADD(stat_heap_bytes, (int64_t) malloc_usable_size(block));
        SRW_STAT_ADD(stat_heap_blocks, 1);
        SRW_STAT_ADD(stat_heap_allocs, 1);
    }
    return PTR2REG(block);
}

uint32_t CCALL HeapReAlloc_c(uint32_t hHeap, uint32_t dwFlags, void *lpMem, uint32_t dwBytes)
{
    size_t old_size;
    void *block;

    (void) hHeap;

    old_size = malloc_usable_size(lpMem);
    block = realloc(lpMem, dwBytes ? dwBytes : 1);
    if (block == NULL)
    {
        SRW_SetLastError(8);
        return 0;
    }
    SRW_STAT_ADD(stat_heap_bytes, (int64_t) malloc_usable_size(block) - (int64_t) old_size);
    if ((dwFlags & 8) && dwBytes > old_size)
    {
        memset((char *)block + old_size, 0, dwBytes - old_size);
    }
    return PTR2REG(block);
}

uint32_t CCALL HeapFree_c(uint32_t hHeap, uint32_t dwFlags, void *lpMem)
{
    (void) hHeap; (void) dwFlags;
    if (lpMem != NULL)
    {
        SRW_STAT_SUB(stat_heap_bytes, (int64_t) malloc_usable_size(lpMem));
        SRW_STAT_SUB(stat_heap_blocks, 1);
    }
    free(lpMem);
    return 1;
}

uint32_t CCALL HeapSize_c(uint32_t hHeap, uint32_t dwFlags, void *lpMem)
{
    (void) hHeap; (void) dwFlags;
    return lpMem ? (uint32_t)malloc_usable_size(lpMem) : 0xffffffffu;
}

uint32_t CCALL HeapValidate_c(uint32_t hHeap, uint32_t dwFlags, void *lpMem)
{
    (void) hHeap; (void) dwFlags; (void) lpMem;
    return 1;
}

uint32_t CCALL VirtualAlloc_c(void *lpAddress, uint32_t dwSize, uint32_t flAllocationType, uint32_t flProtect)
{
    void *block;

    (void) flProtect;

    if (lpAddress != NULL)
    {
        /* committing inside a region reserved before: it is already there */
        return PTR2REG(lpAddress);
    }
    if (!(flAllocationType & 0x3000)) /* MEM_COMMIT | MEM_RESERVE */
    {
        SRW_SetLastError(87); /* ERROR_INVALID_PARAMETER */
        return 0;
    }
    block = memalign(65536, (dwSize + 4095u) & ~4095u);
    if (block == NULL)
    {
        SRW_SetLastError(8);
        return 0;
    }
    memset(block, 0, (dwSize + 4095u) & ~4095u);
    SRW_STAT_ADD(stat_virtual_bytes, (int64_t) malloc_usable_size(block));
    SRW_STAT_ADD(stat_virtual_blocks, 1);
    SRW_STAT_ADD(stat_virtual_allocs, 1);
    return PTR2REG(block);
}

uint32_t CCALL VirtualFree_c(void *lpAddress, uint32_t dwSize, uint32_t dwFreeType)
{
    (void) dwSize;
    if (dwFreeType & 0x8000) /* MEM_RELEASE */
    {
        if (lpAddress != NULL)
        {
            SRW_STAT_SUB(stat_virtual_bytes, (int64_t) malloc_usable_size(lpAddress));
            SRW_STAT_SUB(stat_virtual_blocks, 1);
        }
        free(lpAddress);
    }
    return 1;
}

uint32_t CCALL VirtualProtect_c(void *lpAddress, uint32_t dwSize, uint32_t flNewProtect, void *lpflOldProtect)
{
    (void) lpAddress; (void) dwSize; (void) flNewProtect;
    if (lpflOldProtect != NULL) *(uint32_t *)lpflOldProtect = 4; /* PAGE_READWRITE */
    return 1;
}

uint32_t CCALL GlobalMemoryStatus_c(void *lpBuffer)
{
    SRW_MEMORYSTATUS *status = lpBuffer;

    memset(status, 0, sizeof(*status));
    status->dwLength = sizeof(*status);
    status->dwTotalPhys = 1024u * 1024u * 1024u;
    status->dwAvailPhys = 768u * 1024u * 1024u;
    status->dwTotalPageFile = 2048u * 1024u * 1024u;
    status->dwAvailPageFile = 1536u * 1024u * 1024u;
    status->dwTotalVirtual = 2047u * 1024u * 1024u;
    status->dwAvailVirtual = 1536u * 1024u * 1024u;
    status->dwMemoryLoad = 25;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* thread local storage: a fixed number of slots per thread */

#define TLS_SLOTS 1088

/* The values of a thread are on the heap: the thread-local storage of the module sits right before the image of the
   program in memory, which has to start at 0x400000, so it is kept small. */
static _Thread_local uint32_t *tls_block;

static uint32_t *tls_values_of_thread(void)
{
    if (tls_block == NULL) tls_block = calloc(TLS_SLOTS, sizeof(uint32_t));
    return tls_block;
}
static _Atomic uint32_t tls_used[TLS_SLOTS];

uint32_t CCALL TlsAlloc_c(void)
{
    uint32_t index;

    for (index = 0; index < TLS_SLOTS; index++)
    {
        uint32_t expected = 0;

        if (atomic_compare_exchange_strong(&tls_used[index], &expected, 1))
        {
            /* the other threads still hold what the previous user of the slot left: it is zeroed on use */
            tls_values_of_thread()[index] = 0;
            return index;
        }
    }
    SRW_SetLastError(8);
    return 0xffffffffu;
}

uint32_t CCALL TlsFree_c(uint32_t dwTlsIndex)
{
    if (dwTlsIndex >= TLS_SLOTS) return 0;
    atomic_store(&tls_used[dwTlsIndex], 0);
    return 1;
}

uint32_t CCALL TlsGetValue_c(uint32_t dwTlsIndex)
{
    if (dwTlsIndex >= TLS_SLOTS)
    {
        SRW_SetLastError(87);
        return 0;
    }
    SRW_SetLastError(0);
    SRW_GilPoll();
    return tls_values_of_thread()[dwTlsIndex];
}

uint32_t CCALL TlsSetValue_c(uint32_t dwTlsIndex, void *lpTlsValue)
{
    if (dwTlsIndex >= TLS_SLOTS)
    {
        SRW_SetLastError(87);
        return 0;
    }
    tls_values_of_thread()[dwTlsIndex] = PTR2REG(lpTlsValue);
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* critical sections: the mutex hangs off the DebugInfo field of the structure in guest memory */

static pthread_mutex_t *critical_section_mutex(SRW_CRITICAL_SECTION *cs)
{
    uint32_t stored = ((uintptr_t) &cs->DebugInfo & 3) == 0 ? __atomic_load_n(&cs->DebugInfo, __ATOMIC_ACQUIRE) : *(volatile uint32_t *) &cs->DebugInfo;
    pthread_mutex_t *mutex = (pthread_mutex_t *)REG2PTR(stored);

    if (mutex == NULL)
    {
        pthread_mutexattr_t attr;
        pthread_mutex_t *created = malloc(sizeof(*created));

        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(created, &attr);
        pthread_mutexattr_destroy(&attr);

        /* two threads may enter a section nobody initialized at the same time: the first to store its mutex wins (a compare and
           swap; the field may sit at an address a WebAssembly atomic operation traps on, then a lock of its own does it) */
        if (((uintptr_t) &cs->DebugInfo & 3) == 0)
        {
            uint32_t expected = 0;

            if (__atomic_compare_exchange_n(&cs->DebugInfo, &expected, PTR2REG(created), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) mutex = created;
            else mutex = (pthread_mutex_t *) REG2PTR(expected);
        }
        else
        {
            static pthread_mutex_t unaligned_init = PTHREAD_MUTEX_INITIALIZER;

            pthread_mutex_lock(&unaligned_init);
            if (cs->DebugInfo == 0) cs->DebugInfo = PTR2REG(created);
            mutex = (pthread_mutex_t *) REG2PTR(cs->DebugInfo);
            pthread_mutex_unlock(&unaligned_init);
        }
        if (mutex != created)
        {
            pthread_mutex_destroy(created);
            free(created);
        }
    }
    return mutex;
}

uint32_t CCALL InitializeCriticalSection_c(void *lpCriticalSection)
{
    SRW_CRITICAL_SECTION *cs = lpCriticalSection;

    memset(cs, 0, sizeof(*cs));
    critical_section_mutex(cs);
    return 0;
}

uint32_t CCALL InitializeCriticalSectionAndSpinCount_c(void *lpCriticalSection, uint32_t dwSpinCount)
{
    (void) dwSpinCount;
    InitializeCriticalSection_c(lpCriticalSection);
    return 1;
}

uint32_t CCALL DeleteCriticalSection_c(void *lpCriticalSection)
{
    SRW_CRITICAL_SECTION *cs = lpCriticalSection;
    pthread_mutex_t *mutex = (pthread_mutex_t *)REG2PTR(cs->DebugInfo);

    if (mutex != NULL)
    {
        pthread_mutex_destroy(mutex);
        free(mutex);
        cs->DebugInfo = 0;
    }
    return 0;
}

uint32_t CCALL EnterCriticalSection_c(void *lpCriticalSection)
{
    pthread_mutex_t *mutex = critical_section_mutex(lpCriticalSection);

    SRW_GilPoll();
    SRW_STRESS_POINT();
    if (pthread_mutex_trylock(mutex) != 0)
    {
        /* another thread has it (and is not running): wait without holding the big lock */
        int held = SRW_GilHeld();

        if (held) SRW_GilRelease();
        pthread_mutex_lock(mutex);
        if (held) SRW_GilAcquire();
    }
    return 0;
}

uint32_t CCALL TryEnterCriticalSection_c(void *lpCriticalSection)
{
    return pthread_mutex_trylock(critical_section_mutex(lpCriticalSection)) == 0;
}

uint32_t CCALL LeaveCriticalSection_c(void *lpCriticalSection)
{
    pthread_mutex_unlock(critical_section_mutex(lpCriticalSection));
    SRW_GilPoll();
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* interlocked operations */

/* WebAssembly atomics trap on an operand that is not naturally aligned, x86 accepts it (the guest structures are packed
   as the compiler of the program left them): a global lock stands in for those, the same lock in all operations */
extern volatile char x86_unaligned_lock;   /* (llasm_cmps.c: the guest's locked instructions on unaligned operands take the same lock) */

static uint32_t interlocked_generic(void *address, uint32_t (*update)(uint32_t old, uint32_t argument, uint32_t argument2), uint32_t argument,
                                    uint32_t argument2, int return_new)
{
    uint32_t old, updated;

    while (__atomic_test_and_set(&x86_unaligned_lock, __ATOMIC_ACQUIRE)) { }
    memcpy(&old, address, 4);
    updated = update(old, argument, argument2);
    memcpy(address, &updated, 4);
    __atomic_clear(&x86_unaligned_lock, __ATOMIC_RELEASE);
    return return_new ? updated : old;
}

static uint32_t update_add(uint32_t old, uint32_t a, uint32_t b) { (void) b; return old + a; }
static uint32_t update_set(uint32_t old, uint32_t a, uint32_t b) { (void) old; (void) b; return a; }
static uint32_t update_compare(uint32_t old, uint32_t exchange, uint32_t comparand) { return old == comparand ? exchange : old; }

uint32_t CCALL InterlockedIncrement_c(void *lpAddend)
{
    if (((uintptr_t) lpAddend & 3) != 0) return interlocked_generic(lpAddend, update_add, 1, 0, 1);
    return __atomic_add_fetch((uint32_t *)lpAddend, 1, __ATOMIC_SEQ_CST);
}

uint32_t CCALL InterlockedDecrement_c(void *lpAddend)
{
    if (((uintptr_t) lpAddend & 3) != 0) return interlocked_generic(lpAddend, update_add, (uint32_t) -1, 0, 1);
    return __atomic_sub_fetch((uint32_t *)lpAddend, 1, __ATOMIC_SEQ_CST);
}

uint32_t CCALL InterlockedExchange_c(void *Target, uint32_t Value)
{
    if (((uintptr_t) Target & 3) != 0) return interlocked_generic(Target, update_set, Value, 0, 0);
    return __atomic_exchange_n((uint32_t *)Target, Value, __ATOMIC_SEQ_CST);
}

uint32_t CCALL InterlockedExchangeAdd_c(void *Addend, uint32_t Value)
{
    if (((uintptr_t) Addend & 3) != 0) return interlocked_generic(Addend, update_add, Value, 0, 0);
    return __atomic_fetch_add((uint32_t *)Addend, Value, __ATOMIC_SEQ_CST);
}

uint32_t CCALL InterlockedCompareExchange_c(void *Destination, uint32_t Exchange, uint32_t Comparand)
{
    if (((uintptr_t) Destination & 3) != 0) return interlocked_generic(Destination, update_compare, Exchange, Comparand, 0);
    __atomic_compare_exchange_n((uint32_t *)Destination, &Comparand, Exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return Comparand;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* time */

/* (no turn for other threads here: a program that calibrates its clock reads rdtsc and GetTickCount as one sample) */
uint32_t CCALL GetTickCount_c(void)
{
    /* milliseconds since the clock started (the double is far beyond 32 bits: convert through 64 bits so the count wraps) */
    return (uint32_t) (SRW_NowNs() / 1000000u);
}

uint32_t CCALL QueryPerformanceFrequency_c(void *lpFrequency)
{
    *(uint64_t *)lpFrequency = 1000000000u;
    return 1;
}

uint32_t CCALL QueryPerformanceCounter_c(void *lpPerformanceCount)
{
    SRW_GilPoll();
    *(uint64_t *)lpPerformanceCount = SRW_NowNs();
    return 1;
}

uint32_t CCALL GetSystemTimeAsFileTime_c(void *lpSystemTimeAsFileTime)
{
    struct timespec ts;
    uint64_t ticks;

    clock_gettime(CLOCK_REALTIME, &ts);
    ticks = ((uint64_t)ts.tv_sec + 11644473600u) * 10000000u + (uint64_t)ts.tv_nsec / 100u;
    ((uint32_t *)lpSystemTimeAsFileTime)[0] = (uint32_t)ticks;
    ((uint32_t *)lpSystemTimeAsFileTime)[1] = (uint32_t)(ticks >> 32);
    return 0;
}

#ifdef SRW_DIAGNOSTICS
uint32_t SRW_LastCaller(void);
int SRW_SampleOtherThreads(const char **names, int *counts, int n, int max);
#endif

uint32_t CCALL Sleep_c(uint32_t dwMilliseconds)
{
    int held = SRW_GilHeld();
    double start = emscripten_get_now();

    dwMilliseconds = SRW_SaneTimeout(dwMilliseconds, "Sleep");
    SRW_Note("Sleep", dwMilliseconds);
    if (held) SRW_GilRelease();
    SRW_PROF_ENTER(SRW_PROF_SLEEP);
#ifdef SRW_DIAGNOSTICS
    if (SRW_ProfOn && SRW_GilIsGame()) SRW_ProfNoteSleep(SRW_LastCaller());
#endif
    if (SRW_GilIsGame()) SRW_GameSleeps(1);
    usleep(dwMilliseconds * 1000u);
    if (SRW_GilIsGame()) SRW_GameSleeps(0);
    SRW_PROF_LEAVE();
    SRW_BlockedAdd(0, emscripten_get_now() - start);
#ifdef SRW_DIAGNOSTICS
    if (SRW_LOG_ON(SRW_LOG_THREADS) && SRW_GilIsGame())
    {
        /* the sleeps of the game thread by the procedure that sleeps: how often, for how long, how much longer than asked; the waits
           (runs of sleeps less than 3 ms apart) and what the other threads were doing meanwhile */
        static struct { uint32_t caller; uint32_t count; double asked, slept, longest; } table[12];
        static double reported, episode_start, last_end, episode_total, episode_longest;
        static unsigned episodes;
        static const char *sample_names[8];
        static int sample_counts[8], samples, sample_n;
        double now = emscripten_get_now(), slept = now - start;
        uint32_t caller = SRW_LastCaller();
        static int stacks;
        int i;

        if (start - last_end > 3.0)
        {
            if (episode_start != 0)
            {
                episode_total += last_end - episode_start;
                if (last_end - episode_start > episode_longest) episode_longest = last_end - episode_start;
            }
            episode_start = start;
            episodes++;
        }
        last_end = now;
        sample_n = SRW_SampleOtherThreads(sample_names, sample_counts, sample_n, 8);
        samples++;
        if (stacks < 6 && slept > 1.5)
        {
            _cpu *cpu = x86_initialize_cpu();
            uint32_t address = cpu->_esp, top = cpu->_esp + 4096, shown = 0;   /* (the game thread may run on a fiber: its stack is not the thread's) */
            char line[500];
            size_t n = (size_t) snprintf(line, sizeof(line), "Threads: the game thread sleeps, stack:");

            stacks++;
            for (; address + 4 <= top && shown < 24 && n < sizeof(line) - 12; address += 4)
            {
                uint32_t value = *(const volatile uint32_t *) (uintptr_t) address;

                if (value >= SRW_VaBase && value < SRW_VaBase + SRW_VaSize) { n += (size_t) snprintf(line + n, sizeof(line) - n, " %x", (unsigned int) value); shown++; }
            }
            logprint(line);
        }
        for (i = 0; i < 11 && table[i].count != 0 && table[i].caller != caller; i++) {}
        table[i].caller = caller;
        table[i].count++;
        table[i].asked += dwMilliseconds;
        table[i].slept += slept;
        if (slept > table[i].longest) table[i].longest = slept;
        if (reported == 0) reported = now;
        if (now - reported > 5000.0)
        {
            char line[600];
            size_t n;

            for (i = 0; i < 12 && table[i].count != 0; i++)
                logprintf("Threads: the game thread slept from %x: %u times, %.0f ms asked, %.0f ms slept, longest %.0f ms",
                          (unsigned int) table[i].caller, (unsigned int) table[i].count, table[i].asked, table[i].slept, table[i].longest);
            logprintf("Threads: those sleeps made %u waits, %.0f ms on average, the longest %.0f ms", episodes, episodes ? episode_total / episodes : 0.0, episode_longest);
            n = (size_t) snprintf(line, sizeof(line), "Threads: while it slept the other threads were (average number per sleep):");
            for (i = 0; i < sample_n && n < sizeof(line) - 60; i++)
                n += (size_t) snprintf(line + n, sizeof(line) - n, " %s %.1f,", sample_names[i] != NULL ? sample_names[i] : "running or waiting for the big lock", (double) sample_counts[i] / samples);
            logprint(line);
            memset(table, 0, sizeof(table));
            memset(sample_counts, 0, sizeof(sample_counts));
            sample_n = 0;
            samples = 0;
            episodes = 0;
            episode_total = episode_longest = 0;
            episode_start = 0;
            reported = now;
        }
    }
#endif
    if (held) SRW_GilAcquire();
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* code pages, environment and strings */

/* the code page of the program's ANSI functions: UTF-8, as on a Windows with "Use Unicode UTF-8 for worldwide language support" */
uint32_t CCALL GetACP_c(void)
{
    return 65001;
}

uint32_t CCALL GetOEMCP_c(void)
{
    return 437;
}

uint32_t CCALL GetCPInfo_c(uint32_t CodePage, void *lpCPInfo)
{
    SRW_CPINFO *info = lpCPInfo;

    memset(info, 0, sizeof(*info));
    info->MaxCharSize = (CodePage == 65001 || CodePage == 0) ? 4 : 1;
    info->DefaultChar[0] = '?';
    return 1;
}

static const char environment_a[] = "\0";
static const uint16_t environment_w[] = { 0, 0 };

uint32_t CCALL GetEnvironmentStrings_c(void)
{
    return PTR2REG(environment_a);
}

uint32_t CCALL GetEnvironmentStringsW_c(void)
{
    return PTR2REG(environment_w);
}

uint32_t CCALL FreeEnvironmentStringsA_c(void *lpszEnvironmentBlock)
{
    (void) lpszEnvironmentBlock;
    return 1;
}

uint32_t CCALL FreeEnvironmentStringsW_c(void *lpszEnvironmentBlock)
{
    (void) lpszEnvironmentBlock;
    return 1;
}
