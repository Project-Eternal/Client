/* kernel32: events, mutexes, semaphores, threads and waiting.

   All state of these objects is guarded by one lock (held only for short moments). A waiting thread registers itself on the
   objects it waits for and sleeps on a condition of its own: a change of an object wakes only the threads that wait for it. */
#include <emscripten.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "srw_debuglog.h"
#include "srw.h"
#include "imports.h"
#include "srw_object.h"
#include "srw_win32.h"

#define INFINITE 0xffffffffu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xffffffffu
#define MAXIMUM_WAIT_OBJECTS 64

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;   /* (only for the start of a suspended thread) */

/* a thread waiting for an object: one node per object of the wait, all with the thread's condition */
typedef struct SRW_WaitNode {
    struct SRW_WaitNode *next, **link;
    pthread_cond_t *cond;
} SRW_WaitNode;

static pthread_cond_t *thread_cond(void)
{
    static _Thread_local pthread_cond_t cond;
    static _Thread_local int ready;

    if (!ready)
    {
        pthread_cond_init(&cond, NULL);
        ready = 1;
    }
    return &cond;
}

/* the object changed: its waiters look again (lock is held) */
static void wake(SRW_Object *object)
{
    SRW_WaitNode *node;

    for (node = object->waiters; node != NULL; node = node->next) pthread_cond_signal(node->cond);
}

typedef struct {
    SRW_Object object;
    int manual_reset;
    int signaled;
    uint32_t creator[4]; /* the procedures entered last by the creating thread (traceprocs builds), for the report of long waits */
    /* history for the report of long waits */
    uint32_t sets, sets_while_signaled, resets, taken, last_setter, last_taker;
    double last_set_ms, last_take_ms;
} EventObject;

typedef struct {
    SRW_Object object;
    uint32_t owner; /* thread id, 0 when free */
    uint32_t count;
} MutexObject;

typedef struct {
    SRW_Object object;
    int32_t count;
    int32_t maximum;
} SemaphoreObject;

typedef struct {
    SRW_Object object;
    pthread_t thread;
    int started;
    int finished;
    uint32_t exit_code;
    uint32_t id;
    uint32_t start;
    uint32_t parameter;
    int suspended;
} ThreadObject;

/* a wait is satisfied if the object is signaled; taking it changes its state. lock is held */
static int is_signaled(SRW_Object *object, uint32_t thread)
{
    switch (object->type)
    {
    case SRW_OBJ_EVENT:
        return ((EventObject *) object)->signaled;
    case SRW_OBJ_MUTEX:
    {
        MutexObject *mutex = (MutexObject *) object;

        return mutex->owner == 0 || mutex->owner == thread;
    }
    case SRW_OBJ_SEMAPHORE:
        return ((SemaphoreObject *) object)->count > 0;
    case SRW_OBJ_THREAD:
        return ((ThreadObject *) object)->finished;
    default:
        return 0;
    }
}

static void acquire(SRW_Object *object, uint32_t thread)
{
    switch (object->type)
    {
    case SRW_OBJ_EVENT:
    {
        EventObject *event = (EventObject *) object;

        if (!event->manual_reset) event->signaled = 0;
        event->taken++;
        event->last_taker = thread;
        event->last_take_ms = emscripten_get_now();
        break;
    }
    case SRW_OBJ_MUTEX:
    {
        MutexObject *mutex = (MutexObject *) object;

        mutex->owner = thread;
        mutex->count++;
        break;
    }
    case SRW_OBJ_SEMAPHORE:
        ((SemaphoreObject *) object)->count--;
        break;
    default:
        break;
    }
}

static int timespec_before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

static void deadline_from_now(uint32_t milliseconds, struct timespec *deadline)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += milliseconds / 1000u;
    deadline->tv_nsec += (long) (milliseconds % 1000u) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L)
    {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

/* one line about an object that a thread has waited for a long time: type and state (lock is held) */
static void describe_wait(uint32_t thread, uint32_t handle, SRW_Object *object, uint32_t milliseconds, double waited)
{
    switch (object->type)
    {
    case SRW_OBJ_EVENT:
        logprintf("Thread %x has waited %.0f ms (limit %x) for event %x (%s reset, %s), created after procedures %x %x %x %x; set %u times (%u while already signaled), reset %u, taken %u; last set %.0f ms ago by thread %x, last taken %.0f ms ago by thread %x", (unsigned int) thread, waited,
                  (unsigned int) milliseconds, (unsigned int) handle, ((EventObject *) object)->manual_reset ? "manual" : "auto",
                  ((EventObject *) object)->signaled ? "signaled" : "not signaled", (unsigned int) ((EventObject *) object)->creator[0],
                  (unsigned int) ((EventObject *) object)->creator[1], (unsigned int) ((EventObject *) object)->creator[2], (unsigned int) ((EventObject *) object)->creator[3],
                  (unsigned int) ((EventObject *) object)->sets, (unsigned int) ((EventObject *) object)->sets_while_signaled, (unsigned int) ((EventObject *) object)->resets,
                  (unsigned int) ((EventObject *) object)->taken, ((EventObject *) object)->sets ? emscripten_get_now() - ((EventObject *) object)->last_set_ms : -1.0,
                  (unsigned int) ((EventObject *) object)->last_setter, ((EventObject *) object)->taken ? emscripten_get_now() - ((EventObject *) object)->last_take_ms : -1.0,
                  (unsigned int) ((EventObject *) object)->last_taker);
        break;
    case SRW_OBJ_MUTEX:
        logprintf("Thread %x has waited %.0f ms (limit %x) for mutex %x owned by thread %x (count %u)", (unsigned int) thread, waited,
                  (unsigned int) milliseconds, (unsigned int) handle, (unsigned int) ((MutexObject *) object)->owner, (unsigned int) ((MutexObject *) object)->count);
        break;
    case SRW_OBJ_SEMAPHORE:
        logprintf("Thread %x has waited %.0f ms (limit %x) for semaphore %x (count %d of %d)", (unsigned int) thread, waited, (unsigned int) milliseconds,
                  (unsigned int) handle, (int) ((SemaphoreObject *) object)->count, (int) ((SemaphoreObject *) object)->maximum);
        break;
    case SRW_OBJ_THREAD:
        logprintf("Thread %x has waited %.0f ms (limit %x) for thread %x (%s)", (unsigned int) thread, waited, (unsigned int) milliseconds,
                  (unsigned int) handle, ((ThreadObject *) object)->finished ? "finished" : "running");
        break;
    default:
        logprintf("Thread %x has waited %.0f ms for handle %x of type %d", (unsigned int) thread, waited, (unsigned int) handle, (int) object->type);
        break;
    }
}

static void log_long_wait(uint32_t thread, uint32_t handle, SRW_Object *object, uint32_t milliseconds, double waited)
{
    if (SRW_LOG_ON(SRW_LOG_THREADS)) describe_wait(thread, handle, object, milliseconds, waited);
}

/* the state and the history of a waitable object (the watchdog's freeze report), whatever the log categories say */
void SRW_DescribeWaitObject(uint32_t handle)
{
    SRW_Object *object;

    object = SRW_ObjectRef(handle, 0);
    pthread_mutex_lock(&lock);
    if (object != NULL) describe_wait(0, handle, object, 0, 0);
    else logprintf("Handle %x is not an object", (unsigned int) handle);
    pthread_mutex_unlock(&lock);
    SRW_ObjectUnref(object);
}

/* returns the index of the object that satisfied the wait, or -1 on timeout, -2 for an invalid handle */
static int wait_for(uint32_t count, const uint32_t *handles, int wait_all, uint32_t milliseconds)
{
    SRW_Object *objects[MAXIMUM_WAIT_OBJECTS];
    SRW_WaitNode nodes[MAXIMUM_WAIT_OBJECTS];
    pthread_cond_t *cond = NULL;
    int registered = 0;
    uint32_t i, thread = GetCurrentThreadId_c();
    struct timespec deadline;
    int result = -1, held;
    double start = emscripten_get_now();

    for (i = 0; i < count; i++)
    {
        objects[i] = SRW_ObjectRef(handles[i], 0);   /* (the objects stay while this thread waits, even if another closes the handles) */
        if (objects[i] == NULL)
        {
            while (i > 0) SRW_ObjectUnref(objects[--i]);
            return -2;
        }
    }
    milliseconds = SRW_SaneTimeout(milliseconds, "wait");
    if (milliseconds != INFINITE && milliseconds != 0) deadline_from_now(milliseconds, &deadline);

    /* a thread that may block does not keep the big lock; one that only polls gives others a turn */
    held = SRW_GilHeld();
    if (milliseconds == 0)
    {
        SRW_GilPoll();
    }
    else if (held)
    {
        SRW_Note(count == 1 ? "wait for an object" : "wait for objects", handles[0]);
        SRW_NoteTimeout(milliseconds);
        SRW_GilRelease();
    }

    if (milliseconds != 0) SRW_PROF_ENTER(SRW_PROF_WAIT);
    pthread_mutex_lock(&lock);
    for (;;)
    {
        if (wait_all)
        {
            for (i = 0; i < count; i++)
            {
                if (!is_signaled(objects[i], thread)) break;
            }
            if (i == count)
            {
                for (i = 0; i < count; i++) acquire(objects[i], thread);
                result = 0;
                break;
            }
        }
        else
        {
            for (i = 0; i < count; i++)
            {
                if (is_signaled(objects[i], thread))
                {
                    acquire(objects[i], thread);
                    result = (int) i;
                    break;
                }
            }
            if (result >= 0) break;
        }
        if (milliseconds == 0) break;
        if (!registered)
        {
            cond = thread_cond();
            for (i = 0; i < count; i++)
            {
                nodes[i].cond = cond;
                nodes[i].next = objects[i]->waiters;
                nodes[i].link = &objects[i]->waiters;
                if (objects[i]->waiters != NULL) objects[i]->waiters->link = &nodes[i].next;
                objects[i]->waiters = &nodes[i];
            }
            registered = 1;
        }
        if (milliseconds == INFINITE || milliseconds >= 5000)
        {
            /* a wait of many seconds is reported every 5 s (at most 60 lines in all) */
            static int long_waits;
            struct timespec slice;

            deadline_from_now(5000, &slice);
            if (pthread_cond_timedwait(cond, &lock, milliseconds == INFINITE || timespec_before(&slice, &deadline) ? &slice : &deadline) == ETIMEDOUT
                && emscripten_get_now() - start >= 5000 && long_waits < 60)
            {
                long_waits++;
                for (i = 0; i < count; i++) log_long_wait(thread, handles[i], objects[i], milliseconds, emscripten_get_now() - start);
            }
            if (milliseconds != INFINITE && emscripten_get_now() - start >= milliseconds) break;
        }
        else if (pthread_cond_timedwait(cond, &lock, &deadline) == ETIMEDOUT)
        {
            break;
        }
    }
    if (registered)
    {
        for (i = 0; i < count; i++)
        {
            *nodes[i].link = nodes[i].next;
            if (nodes[i].next != NULL) nodes[i].next->link = nodes[i].link;
        }
    }
    pthread_mutex_unlock(&lock);
    for (i = 0; i < count; i++) SRW_ObjectUnref(objects[i]);
    if (milliseconds != 0) { SRW_PROF_LEAVE(); SRW_BlockedAdd(1, emscripten_get_now() - start); }
    if (held && milliseconds != 0) SRW_GilAcquire();
    return result;
}

uint32_t CCALL WaitForSingleObject_c(uint32_t hHandle, uint32_t dwMilliseconds)
{
    int result = wait_for(1, &hHandle, 0, dwMilliseconds);

    if (result == -2)
    {
        SRW_SetLastError(6);
        return WAIT_FAILED;
    }
    return (result < 0) ? WAIT_TIMEOUT : WAIT_OBJECT_0;
}

uint32_t CCALL WaitForMultipleObjects_c(uint32_t nCount, void *lpHandles, uint32_t bWaitAll, uint32_t dwMilliseconds)
{
    int result;

    if (nCount == 0 || nCount > MAXIMUM_WAIT_OBJECTS)
    {
        SRW_SetLastError(87);
        return WAIT_FAILED;
    }
    result = wait_for(nCount, lpHandles, bWaitAll != 0, dwMilliseconds);
    if (result == -2)
    {
        SRW_SetLastError(6);
        return WAIT_FAILED;
    }
    return (result < 0) ? WAIT_TIMEOUT : WAIT_OBJECT_0 + (uint32_t) result;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* events, mutexes, semaphores */

uint32_t CCALL CreateEventA_c(void *lpEventAttributes, uint32_t bManualReset, uint32_t bInitialState, const char *lpName)
{
    EventObject *event = calloc(1, sizeof(*event));
    uint32_t handle;

    (void) lpEventAttributes;
    (void) lpName;

    if (event == NULL) return 0;
    event->object.type = SRW_OBJ_EVENT;
    event->manual_reset = bManualReset != 0;
    event->signaled = bInitialState != 0;
    SRW_RecentProcs(event->creator, 4);
    handle = SRW_ObjectCreate(&event->object);
    if (handle == 0) free(event);
    return handle;
}

static uint32_t set_event_state_ex(uint32_t handle, int signaled, int poll)
{
    EventObject *event = (EventObject *) SRW_ObjectRef(handle, SRW_OBJ_EVENT);

    if (event == NULL)
    {
        SRW_SetLastError(6);
        return 0;
    }
    SRW_STRESS_POINT();
    pthread_mutex_lock(&lock);
    if (signaled)
    {
        event->sets++;
        if (event->signaled) event->sets_while_signaled++;
        event->last_setter = GetCurrentThreadId_c();
        event->last_set_ms = emscripten_get_now();
    }
    else
    {
        event->resets++;
    }
    event->signaled = signaled;
    if (signaled) wake(&event->object);
    pthread_mutex_unlock(&lock);
    SRW_ObjectUnref(&event->object);
    if (poll) SRW_GilPoll();
    return 1;
}

static uint32_t set_event_state(uint32_t handle, int signaled)
{
    return set_event_state_ex(handle, signaled, 1);
}

/* for native threads that do not hold the big lock (the socket event thread) */
void SRW_SetEventNative(uint32_t handle) { set_event_state_ex(handle, 1, 0); }
void SRW_ResetEventNative(uint32_t handle) { set_event_state_ex(handle, 0, 0); }

uint32_t CCALL SetEvent_c(uint32_t hEvent)
{
    return set_event_state(hEvent, 1);
}

uint32_t CCALL ResetEvent_c(uint32_t hEvent)
{
    return set_event_state(hEvent, 0);
}

uint32_t CCALL CreateMutexA_c(void *lpMutexAttributes, uint32_t bInitialOwner, const char *lpName)
{
    MutexObject *mutex = calloc(1, sizeof(*mutex));
    uint32_t handle;

    (void) lpMutexAttributes;
    (void) lpName;

    if (mutex == NULL) return 0;
    mutex->object.type = SRW_OBJ_MUTEX;
    if (bInitialOwner)
    {
        mutex->owner = GetCurrentThreadId_c();
        mutex->count = 1;
    }
    handle = SRW_ObjectCreate(&mutex->object);
    if (handle == 0) free(mutex);
    return handle;
}

uint32_t CCALL OpenMutexA_c(uint32_t dwDesiredAccess, uint32_t bInheritHandle, const char *lpName)
{
    (void) dwDesiredAccess;
    (void) bInheritHandle;
    (void) lpName;
    SRW_SetLastError(2); /* named objects are never shared with another process */
    return 0;
}

uint32_t CCALL ReleaseMutex_c(uint32_t hMutex)
{
    MutexObject *mutex = (MutexObject *) SRW_ObjectRef(hMutex, SRW_OBJ_MUTEX);
    uint32_t result = 0;

    if (mutex == NULL)
    {
        SRW_SetLastError(6);
        return 0;
    }
    pthread_mutex_lock(&lock);
    if (mutex->owner == GetCurrentThreadId_c() && mutex->count > 0)
    {
        if (--mutex->count == 0)
        {
            mutex->owner = 0;
            wake(&mutex->object);
        }
        result = 1;
    }
    pthread_mutex_unlock(&lock);
    SRW_ObjectUnref(&mutex->object);
    if (!result) SRW_SetLastError(288); /* ERROR_NOT_OWNER */
    return result;
}

uint32_t CCALL CreateSemaphoreA_c(void *lpSemaphoreAttributes, uint32_t lInitialCount, uint32_t lMaximumCount, const char *lpName)
{
    SemaphoreObject *semaphore;
    uint32_t handle;

    (void) lpSemaphoreAttributes;
    (void) lpName;

    if ((int32_t) lMaximumCount <= 0 || (int32_t) lInitialCount < 0 || lInitialCount > lMaximumCount)
    {
        SRW_SetLastError(87);
        return 0;
    }
    semaphore = calloc(1, sizeof(*semaphore));
    if (semaphore == NULL) return 0;
    semaphore->object.type = SRW_OBJ_SEMAPHORE;
    semaphore->count = (int32_t) lInitialCount;
    semaphore->maximum = (int32_t) lMaximumCount;
    handle = SRW_ObjectCreate(&semaphore->object);
    if (handle == 0) free(semaphore);
    return handle;
}

uint32_t CCALL ReleaseSemaphore_c(uint32_t hSemaphore, uint32_t lReleaseCount, void *lpPreviousCount)
{
    SemaphoreObject *semaphore = (SemaphoreObject *) SRW_ObjectRef(hSemaphore, SRW_OBJ_SEMAPHORE);
    uint32_t result = 0;

    if (semaphore == NULL)
    {
        SRW_SetLastError(6);
        return 0;
    }
    pthread_mutex_lock(&lock);
    if ((int32_t) lReleaseCount > 0 && semaphore->count + (int32_t) lReleaseCount <= semaphore->maximum)
    {
        if (lpPreviousCount != NULL) *(int32_t *) lpPreviousCount = semaphore->count;
        semaphore->count += (int32_t) lReleaseCount;
        wake(&semaphore->object);
        result = 1;
    }
    pthread_mutex_unlock(&lock);
    SRW_ObjectUnref(&semaphore->object);
    if (!result) SRW_SetLastError(298); /* ERROR_TOO_MANY_POSTS */
    return result;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* threads */

static _Thread_local ThreadObject *current_thread;

static void *thread_main(void *argument)
{
    ThreadObject *thread = argument;
    uint32_t code;

    x86_initialize_cpu();
    current_thread = thread;
    if (SRW_ThreadRole(thread->start) == SRW_THREAD_AUDIO) SRW_GilSetAudio();
    SRW_GilSetTag(thread->start);
    SRW_GilAcquire();

    pthread_mutex_lock(&lock);
    while (thread->suspended) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);

    code = SRW_CallGuest(thread->start, 1, &thread->parameter);

    pthread_mutex_lock(&lock);
    thread->exit_code = code;
    thread->finished = 1;
    wake(&thread->object);
    pthread_mutex_unlock(&lock);

    x86_deinitialize_cpu();
    SRW_GilRelease();
    return NULL;
}

uint32_t CCALL ExitThread_c(uint32_t dwExitCode)
{
    ThreadObject *thread = current_thread;

    if (thread == NULL) return ExitProcess_c(dwExitCode);
    pthread_mutex_lock(&lock);
    thread->exit_code = dwExitCode;
    thread->finished = 1;
    wake(&thread->object);
    pthread_mutex_unlock(&lock);
    x86_deinitialize_cpu();
    SRW_GilRelease();
    pthread_exit(NULL);
    return 0;
}

static void destroy_thread(SRW_Object *object)
{
    ThreadObject *thread = (ThreadObject *) object;

    if (thread->started) pthread_detach(thread->thread);
}

uint32_t CCALL CreateThread_c(void *lpThreadAttributes, uint32_t dwStackSize, void *lpStartAddress, uint32_t lpParameter,
                              uint32_t dwCreationFlags, void *lpThreadId)
{
    static uint32_t next_id = 0x1000;
    ThreadObject *thread;
    uint32_t handle;

    (void) lpThreadAttributes;
    (void) dwStackSize;

    thread = calloc(1, sizeof(*thread));
    if (thread == NULL) return 0;
    thread->object.type = SRW_OBJ_THREAD;
    thread->object.destroy = destroy_thread;
    thread->start = (uint32_t) (uintptr_t) lpStartAddress;
    thread->parameter = lpParameter;
    thread->suspended = (dwCreationFlags & 4) != 0; /* CREATE_SUSPENDED */
    thread->id = __atomic_add_fetch(&next_id, 1, __ATOMIC_SEQ_CST);

    handle = SRW_ObjectCreate(&thread->object);
    if (handle == 0)
    {
        free(thread);
        return 0;
    }
    /* the table keeps the object alive while the thread runs */
    SRW_ObjectDuplicate(handle);

    SRW_LOG(SRW_LOG_THREADS, "CreateThread(start %x, parameter %x) -> %x", (unsigned int) thread->start, (unsigned int) lpParameter, (unsigned int) handle);
    if (pthread_create(&thread->thread, NULL, thread_main, thread) != 0)
    {
        SRW_ObjectClose(handle);
        SRW_ObjectClose(handle);
        SRW_SetLastError(8);
        return 0;
    }
    thread->started = 1;
    if (lpThreadId != NULL) *(uint32_t *) lpThreadId = thread->id;
    return handle;
}

uint32_t CCALL ResumeThread_c(uint32_t hThread)
{
    ThreadObject *thread = (ThreadObject *) SRW_ObjectRef(hThread, SRW_OBJ_THREAD);
    uint32_t previous;

    if (thread == NULL) return 0xffffffffu;
    pthread_mutex_lock(&lock);
    previous = thread->suspended ? 1 : 0;
    thread->suspended = 0;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    SRW_ObjectUnref(&thread->object);
    return previous;
}

uint32_t CCALL SuspendThread_c(uint32_t hThread)
{
    (void) hThread; /* nothing in the program suspends a running thread */
    return 0;
}

uint32_t CCALL TerminateThread_c(uint32_t hThread, uint32_t dwExitCode)
{
    (void) hThread;
    (void) dwExitCode;
    warnprint("TerminateThread is not supported, the thread keeps running");
    return 1;
}

uint32_t CCALL SetThreadPriority_c(uint32_t hThread, uint32_t nPriority)
{
    (void) hThread;
    (void) nPriority;
    return 1;
}

uint32_t CCALL GetThreadPriority_c(uint32_t hThread)
{
    (void) hThread;
    return 0; /* THREAD_PRIORITY_NORMAL */
}

uint32_t CCALL SetThreadAffinityMask_c(uint32_t hThread, uint32_t dwThreadAffinityMask)
{
    (void) hThread;
    (void) dwThreadAffinityMask;
    return 1;
}

uint32_t CCALL GetExitCodeProcess_c(uint32_t hProcess, void *lpExitCode)
{
    (void) hProcess;
    *(uint32_t *) lpExitCode = 259; /* STILL_ACTIVE */
    return 1;
}

uint32_t CCALL TerminateProcess_c(uint32_t hProcess, uint32_t uExitCode)
{
    (void) hProcess;
    return ExitProcess_c(uExitCode);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* handles */

uint32_t CCALL CloseHandle_c(uint32_t hObject)
{
    if (!SRW_ObjectClose(hObject))
    {
        SRW_SetLastError(6);
        return 0;
    }
    return 1;
}

uint32_t CCALL DuplicateHandle_c(uint32_t hSourceProcessHandle, uint32_t hSourceHandle, uint32_t hTargetProcessHandle, void *lpTargetHandle,
                                 uint32_t dwDesiredAccess, uint32_t bInheritHandle, uint32_t dwOptions)
{
    uint32_t handle;

    (void) hSourceProcessHandle;
    (void) hTargetProcessHandle;
    (void) dwDesiredAccess;
    (void) bInheritHandle;

    handle = SRW_ObjectDuplicate(hSourceHandle);
    if (handle == 0)
    {
        SRW_SetLastError(6);
        return 0;
    }
    if ((dwOptions & 1) != 0) SRW_ObjectClose(hSourceHandle); /* DUPLICATE_CLOSE_SOURCE */
    if (lpTargetHandle != NULL) *(uint32_t *) lpTargetHandle = handle;
    return 1;
}

/* I/O completion ports are used by the network code of the program only when it has sockets that complete
   asynchronously; nothing here provides that */
uint32_t CCALL CreateIoCompletionPort_c(uint32_t FileHandle, uint32_t ExistingCompletionPort, uint32_t CompletionKey, uint32_t NumberOfConcurrentThreads)
{
    (void) FileHandle;
    (void) ExistingCompletionPort;
    (void) CompletionKey;
    (void) NumberOfConcurrentThreads;
    SRW_SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return 0;
}

uint32_t CCALL GetQueuedCompletionStatus_c(uint32_t CompletionPort, void *lpNumberOfBytes, void *lpCompletionKey, void *lpOverlapped, uint32_t dwMilliseconds)
{
    (void) CompletionPort;
    (void) lpNumberOfBytes;
    (void) lpCompletionKey;
    (void) lpOverlapped;
    if (dwMilliseconds != INFINITE) usleep(dwMilliseconds * 1000u);
    SRW_SetLastError(WAIT_TIMEOUT);
    return 0;
}

uint32_t CCALL PostQueuedCompletionStatus_c(uint32_t CompletionPort, uint32_t dwNumberOfBytesTransferred, void *dwCompletionKey, void *lpOverlapped)
{
    (void) CompletionPort;
    (void) dwNumberOfBytesTransferred;
    (void) dwCompletionKey;
    (void) lpOverlapped;
    return 0;
}
