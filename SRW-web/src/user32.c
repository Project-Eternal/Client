/* user32: windows, the message queue, input state and the display, on top of the browser page.

   The program has one main window that covers the canvas. Windows are records; the queue is fed by the page's input
   events (which arrive on the browser's main thread) and read by the game thread. */
#include <ctype.h>
#include <zlib.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/html5.h>

#include "srw_debuglog.h"

uint32_t SRW_LastCaller(void);
#include "srw.h"
#include "imports.h"
#include "srw_object.h"
#include "srw_win32.h"

/* messages */
enum {
    WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003, WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007,
    WM_KILLFOCUS = 0x0008, WM_PAINT = 0x000f, WM_CLOSE = 0x0010, WM_QUIT = 0x0012, WM_ERASEBKGND = 0x0014,
    WM_SHOWWINDOW = 0x0018, WM_ACTIVATEAPP = 0x001c, WM_SETCURSOR = 0x0020, WM_MOUSEACTIVATE = 0x0021,
    WM_GETMINMAXINFO = 0x0024, WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082, WM_NCHITTEST = 0x0084,
    WM_NCACTIVATE = 0x0086, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104,
    WM_SYSKEYUP = 0x0105, WM_TIMER = 0x0113, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
    WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205, WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208, WM_MOUSEWHEEL = 0x020a
};

#define WS_VISIBLE 0x10000000u
#define PM_REMOVE 1u
#define MAX_WINDOWS 64
#define MAX_TIMERS 16
#define QUEUE_SIZE 1024
#define WINDOW_BASE 0x10000u

#pragma pack(push, 1)
typedef struct {
    uint32_t hwnd, message, wParam, lParam, time;
    int32_t x, y;
} Msg;

typedef struct {
    int32_t left, top, right, bottom;
} Rect;

typedef struct {
    uint32_t cbSize, style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground, lpszMenuName,
             lpszClassName, hIconSm;
} WndClassEx;

typedef struct {
    uint32_t lpCreateParams, hInstance, hMenu, hwndParent;
    int32_t cy, cx, y, x;
    uint32_t style, lpszName, lpszClass, dwExStyle;
} CreateStruct;
#pragma pack(pop)

typedef struct {
    char name[64];
    uint32_t wndproc;
    uint32_t style;
} WindowClass;

typedef struct {
    int used;
    WindowClass *class;
    uint32_t style, exstyle, parent, userdata, wndproc;
    int32_t x, y, width, height;
    char title[128];
    int visible, enabled;
    uint32_t extra_long[8];
} Window;

static WindowClass classes[32];
static Window windows[MAX_WINDOWS];
static uint32_t main_window;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_changed = PTHREAD_COND_INITIALIZER;

static Msg queue[QUEUE_SIZE];
static unsigned int queue_head, queue_count;
static int quit_posted;
static uint32_t quit_code;

static uint8_t key_down[256];
volatile int SRW_HasPage = 1;   /* a page (the browser's DOM) brings the input and takes the pointer; 0 under Node (set when the input is installed) */
static int app_active = 1;   /* what the program was last told about being in the foreground (SRW_SetActive) */
static int32_t cursor_x, cursor_y;
static int32_t screen_width = 1024, screen_height = 768;

typedef struct {
    uint32_t hwnd, id, interval, next;
} Timer;
static Timer timers[MAX_TIMERS];
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;   /* the timers (a lock of their own: post takes the queue's) */

static Window *window_of(uint32_t hwnd)
{
    uint32_t index = (hwnd - WINDOW_BASE) / 4;

    if (hwnd < WINDOW_BASE || hwnd % 4 != 0 || index >= MAX_WINDOWS || !windows[index].used) return NULL;
    return &windows[index];
}

static uint32_t handle_of(const Window *window)
{
    return WINDOW_BASE + (uint32_t) (window - windows) * 4;
}

/* The sizes of the display, in CSS pixels: the canvas fills the page's viewport (window.innerWidth/innerHeight, smaller while the
   browser shows its bars) and is the "desktop" (the current display mode, the work area); the screen (screen.width/height) is the
   other mode Direct3D reports (the page in full screen). Under Node: the defaults. */
static int32_t display_screen_width, display_screen_height;
static void query_screen_size(void);

void SRW_DisplaySizes(int32_t *canvas_width, int32_t *canvas_height, int32_t *screen_width_out, int32_t *screen_height_out)
{
    int width = MAIN_THREAD_EM_ASM_INT({
        return (typeof screen !== "undefined" && screen.width) ? (screen.width << 16) | (screen.height & 0xffff) : 0;
    });

    query_screen_size();
    if (width > 0)
    {
        display_screen_width = width >> 16;
        display_screen_height = width & 0xffff;
    }
    *canvas_width = screen_width;
    *canvas_height = screen_height;
    *screen_width_out = display_screen_width > 0 ? display_screen_width : screen_width;
    *screen_height_out = display_screen_height > 0 ? display_screen_height : screen_height;
}

/* the size of the page area the game is drawn into */
static void query_screen_size(void)
{
    int width = MAIN_THREAD_EM_ASM_INT({
        return (typeof window !== "undefined") ? window.innerWidth : 0;
    });
    int height = MAIN_THREAD_EM_ASM_INT({
        return (typeof window !== "undefined") ? window.innerHeight : 0;
    });

    if (width > 0 && height > 0)
    {
        screen_width = width;
        screen_height = height;
    }
#ifdef SRW_DIAGNOSTICS
    else
    {
        /* headless (no page): the canvas' size at the start from /canvas.txt ("W H"), once */
        static int read_once;

        if (!read_once++)
        {
            FILE *f = fopen("/Port/canvas.txt", "r");

            if (f != NULL)
            {
                if (fscanf(f, "%d %d", &width, &height) == 2 && width > 0 && height > 0)
                {
                    screen_width = width;
                    screen_height = height;
                }
                fclose(f);
            }
        }
    }
#endif
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the queue */

#ifdef SRW_DIAGNOSTICS
static void start_autoinput(void);
void SRW_SetActive(int active);
#endif
static void run_game_calls(void);
static void post(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    pthread_mutex_lock(&lock);
    if (queue_count < QUEUE_SIZE)
    {
        Msg *m = &queue[(queue_head + queue_count++) % QUEUE_SIZE];

        m->hwnd = hwnd;
        m->message = message;
        m->wParam = wparam;
        m->lParam = lparam;
        m->time = GetTickCount_c();
        m->x = cursor_x;
        m->y = cursor_y;
        pthread_cond_broadcast(&queue_changed);
    }
    pthread_mutex_unlock(&lock);
}

static void install_input(void);

static uint32_t call_proc(Window *window, uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    uint32_t args[4];
    uint32_t proc = window->wndproc != 0 ? window->wndproc : window->class->wndproc;

    args[0] = hwnd;
    args[1] = message;
    args[2] = wparam;
    args[3] = lparam;
    return SRW_CallGuest(proc, 4, args);
}

static uint32_t send(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    Window *window = window_of(hwnd);

    if (window == NULL) return 0;
    return call_proc(window, hwnd, message, wparam, lparam);
}

/* timers that are due become WM_TIMER messages; returns milliseconds until the next one, or -1 */
static int poll_timers(void)
{
    uint32_t now = GetTickCount_c(), due[MAX_TIMERS][2];
    int wait = -1, i, n = 0;

    pthread_mutex_lock(&timer_lock);
    for (i = 0; i < MAX_TIMERS; i++)
    {
        if (timers[i].interval == 0) continue;
        if ((int32_t) (now - timers[i].next) >= 0)
        {
            due[n][0] = timers[i].hwnd;
            due[n][1] = timers[i].id;
            n++;
            timers[i].next = now + timers[i].interval;
        }
        if (wait < 0 || (int) (timers[i].next - now) < wait) wait = (int) (timers[i].next - now);
    }
    pthread_mutex_unlock(&timer_lock);
    for (i = 0; i < n; i++) post(due[i][0], WM_TIMER, due[i][1], 0);
    return wait;
}

/* takes a message from the queue; wait: 0 no, 1 until something arrives */
static int next_message(Msg *out, uint32_t hwnd, uint32_t min, uint32_t max, int remove, int wait)
{
    unsigned int i;
    int found = 0, held;

    for (;;)
    {
        int timer_wait;

        run_game_calls();
        timer_wait = poll_timers();

        pthread_mutex_lock(&lock);
        for (i = 0; i < queue_count; i++)
        {
            Msg *m = &queue[(queue_head + i) % QUEUE_SIZE];

            if ((hwnd != 0 && m->hwnd != hwnd) || (min != 0 && m->message < min) || (max != 0 && m->message > max)) continue;
            *out = *m;
            if (remove)
            {
                unsigned int j;

                for (j = i; j + 1 < queue_count; j++)
                {
                    queue[(queue_head + j) % QUEUE_SIZE] = queue[(queue_head + j + 1) % QUEUE_SIZE];
                }
                queue_count--;
            }
            found = 1;
            break;
        }
        if (!found && quit_posted && remove)
        {
            memset(out, 0, sizeof(*out));
            out->message = WM_QUIT;
            out->wParam = quit_code;
            found = 1;
        }
        if (!found && wait)
        {
            struct timespec deadline;

            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += (long) ((timer_wait < 0 ? 100 : timer_wait) + 1) * 1000000L;
            deadline.tv_sec += deadline.tv_nsec / 1000000000L;
            deadline.tv_nsec %= 1000000000L;
            pthread_mutex_unlock(&lock);
            held = SRW_GilHeld();
            if (held) SRW_Note("wait for a message", 0);
            if (held) SRW_GilRelease();
            pthread_mutex_lock(&lock);
            pthread_cond_timedwait(&queue_changed, &lock, &deadline);
            pthread_mutex_unlock(&lock);
            if (held) SRW_GilAcquire();
            goto next_round;
        }
        pthread_mutex_unlock(&lock);
        if (!wait) SRW_GilPoll();
        if (found || !wait) return found;
    next_round:;
    }
}

uint32_t CCALL GetMessageA_c(void *lpMsg, uint32_t hWnd, uint32_t wMsgFilterMin, uint32_t wMsgFilterMax)
{
    Msg *msg = lpMsg;

    next_message(msg, hWnd, wMsgFilterMin, wMsgFilterMax, 1, 1);
    return msg->message != WM_QUIT;
}

static void update_text_entry(void);

uint32_t CCALL PeekMessageA_c(void *lpMsg, uint32_t hWnd, uint32_t wMsgFilterMin, uint32_t wMsgFilterMax, uint32_t wRemoveMsg)
{
    update_text_entry();
    return next_message(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, (wRemoveMsg & PM_REMOVE) != 0, 0);
}

/* waits for a handle or for a message of the thread's queue */
uint32_t CCALL MsgWaitForMultipleObjects_c(uint32_t nCount, void *lpHandles, uint32_t bWaitAll, uint32_t dwMilliseconds, uint32_t dwWakeMask)
{
    uint64_t end = SRW_NowNs() + (uint64_t) dwMilliseconds * 1000000u;
    Msg probe;

    (void) dwWakeMask;
    for (;;)
    {
        uint32_t result;

        if (next_message(&probe, 0, 0, 0, 0, 0)) return nCount;   /* WAIT_OBJECT_0 + nCount */
        if (nCount > 0)
        {
            result = WaitForMultipleObjects_c(nCount, lpHandles, bWaitAll, 1);
            if (result != 0x102) return result;
        }
        else
        {
            Sleep_c(1);
        }
        if (dwMilliseconds != 0xffffffffu && SRW_NowNs() >= end) return 0x102;
    }
}

uint32_t CCALL TranslateMessage_c(void *lpMsg)
{
    (void) lpMsg; /* WM_CHAR is generated directly from the page's character events */
    return 0;
}

uint32_t CCALL DispatchMessageA_c(void *lpMsg)
{
    Msg *msg = lpMsg;
    Window *window = window_of(msg->hwnd);

    if (window == NULL) return 0;
    if (msg->message == WM_TIMER && msg->lParam != 0) return SRW_CallGuest(msg->lParam, 4, (uint32_t[]) {msg->hwnd, WM_TIMER, msg->wParam, msg->time});
    return call_proc(window, msg->hwnd, msg->message, msg->wParam, msg->lParam);
}

uint32_t CCALL PostMessageA_c(uint32_t hWnd, uint32_t Msg_, uint32_t wParam, uint32_t lParam)
{
    post(hWnd, Msg_, wParam, lParam);
    return 1;
}

uint32_t CCALL SendMessageA_c(uint32_t hWnd, uint32_t Msg_, uint32_t wParam, uint32_t lParam)
{
    return send(hWnd, Msg_, wParam, lParam);
}

uint32_t CCALL PostQuitMessage_c(uint32_t nExitCode)
{
    pthread_mutex_lock(&lock);
    quit_posted = 1;
    quit_code = nExitCode;
    pthread_cond_broadcast(&queue_changed);
    pthread_mutex_unlock(&lock);
    return 0;
}

uint32_t CCALL SetTimer_c(uint32_t hWnd, uint32_t nIDEvent, uint32_t uElapse, void *lpTimerFunc)
{
    int i, free_slot = -1;

    uint32_t id;

    (void) lpTimerFunc;
    pthread_mutex_lock(&timer_lock);
    for (i = 0; i < MAX_TIMERS; i++)
    {
        if (timers[i].interval != 0 && timers[i].hwnd == hWnd && timers[i].id == nIDEvent) free_slot = i;
    }
    for (i = 0; free_slot < 0 && i < MAX_TIMERS; i++)
    {
        if (timers[i].interval == 0) free_slot = i;
    }
    if (free_slot < 0)
    {
        pthread_mutex_unlock(&timer_lock);
        return 0;
    }
    timers[free_slot].hwnd = hWnd;
    timers[free_slot].id = (nIDEvent != 0) ? nIDEvent : 0x100 + (uint32_t) free_slot;
    timers[free_slot].interval = uElapse ? uElapse : 1;
    timers[free_slot].next = GetTickCount_c() + timers[free_slot].interval;
    id = timers[free_slot].id;
    pthread_mutex_unlock(&timer_lock);
    return id;
}

uint32_t CCALL KillTimer_c(uint32_t hWnd, uint32_t uIDEvent)
{
    int i, found = 0;

    pthread_mutex_lock(&timer_lock);
    for (i = 0; i < MAX_TIMERS; i++)
    {
        if (timers[i].interval != 0 && timers[i].hwnd == hWnd && timers[i].id == uIDEvent)
        {
            timers[i].interval = 0;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&timer_lock);
    return found;
}

uint32_t CCALL WaitForInputIdle_c(uint32_t hProcess, uint32_t dwMilliseconds)
{
    (void) hProcess;
    (void) dwMilliseconds;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* classes and windows */

uint32_t CCALL RegisterClassExA_c(void *lpwcx)
{
    const WndClassEx *wc = lpwcx;
    const char *name = (const char *) (uintptr_t) wc->lpszClassName;
    int i;

    if (name == NULL || (uintptr_t) name < 0x10000) return 0;
    for (i = 0; i < 32; i++)
    {
        if (classes[i].name[0] == 0)
        {
            snprintf(classes[i].name, sizeof(classes[i].name), "%s", name);
            classes[i].wndproc = wc->lpfnWndProc;
            classes[i].style = wc->style;
            return 0xc000 + (uint32_t) i;
        }
    }
    return 0;
}

uint32_t CCALL UnregisterClassA_c(const char *lpClassName, uint32_t hInstance)
{
    int i;

    (void) hInstance;
    for (i = 0; i < 32; i++)
    {
        if (strcasecmp(classes[i].name, lpClassName) == 0)
        {
            classes[i].name[0] = 0;
            return 1;
        }
    }
    return 0;
}

static WindowClass *find_class(const char *name)
{
    int i;

    if ((uintptr_t) name >= 0xc000 && (uintptr_t) name < 0xc020) return &classes[(uintptr_t) name - 0xc000];
    for (i = 0; i < 32; i++)
    {
        if (classes[i].name[0] != 0 && strcasecmp(classes[i].name, name) == 0) return &classes[i];
    }
    return NULL;
}

/* (dwExStyle, lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam) */
uint32_t CCALL CreateWindowExA_c(const uint32_t *args)
{
    WindowClass *class = ((uintptr_t) args[1] != 0) ? find_class((const char *) (uintptr_t) args[1]) : NULL;
    Window *window = NULL;
    CreateStruct cs;
    uint32_t hwnd, i;

    if (class == NULL)
    {
        logprintf("CreateWindow with unknown class \"%s\"", (args[1] >= 0x10000) ? (const char *) (uintptr_t) args[1] : "?");
        SRW_SetLastError(1407); /* ERROR_CANNOT_FIND_WND_CLASS */
        return 0;
    }
    pthread_mutex_lock(&lock);
    for (i = 0; i < MAX_WINDOWS; i++)
    {
        if (!windows[i].used)
        {
            window = &windows[i];
            memset(window, 0, sizeof(*window));
            window->used = 1;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    if (window == NULL) return 0;

    query_screen_size();
    window->class = class;
    window->exstyle = args[0];
    window->style = args[3];
    window->x = (args[4] == 0x80000000u) ? 0 : (int32_t) args[4];
    window->y = (args[5] == 0x80000000u) ? 0 : (int32_t) args[5];
    window->width = (args[6] == 0x80000000u) ? screen_width : (int32_t) args[6];
    window->height = (args[7] == 0x80000000u) ? screen_height : (int32_t) args[7];
    window->parent = args[8];
    window->enabled = 1;
    if (args[2] >= 0x10000) snprintf(window->title, sizeof(window->title), "%s", (const char *) (uintptr_t) args[2]);
    hwnd = handle_of(window);
    if (window->parent == 0)
        SRW_LOG(SRW_LOG_INPUT, "CreateWindow \"%s\": %dx%d at %d,%d, style %x, ex style %x", window->title, (int) window->width, (int) window->height,
                (int) window->x, (int) window->y, (unsigned int) window->style, (unsigned int) window->exstyle);
    if (main_window == 0 && window->parent == 0)
    {
        main_window = hwnd;
        install_input();
#ifdef SRW_DIAGNOSTICS
        start_autoinput();
#endif
    }

    memset(&cs, 0, sizeof(cs));
    cs.lpCreateParams = args[11];
    cs.hInstance = args[10];
    cs.hMenu = args[9];
    cs.hwndParent = args[8];
    cs.cy = window->height;
    cs.cx = window->width;
    cs.y = window->y;
    cs.x = window->x;
    cs.style = window->style;
    cs.lpszName = args[2];
    cs.lpszClass = args[1];
    cs.dwExStyle = args[0];

    if (call_proc(window, hwnd, WM_NCCREATE, 0, (uint32_t) (uintptr_t) &cs) == 0 ||
        call_proc(window, hwnd, WM_CREATE, 0, (uint32_t) (uintptr_t) &cs) == 0xffffffffu)
    {
        window->used = 0;
        if (main_window == hwnd) main_window = 0;
        return 0;
    }
    if ((window->style & WS_VISIBLE) != 0) ShowWindow_c(hwnd, 5);
    return hwnd;
}

uint32_t CCALL DestroyWindow_c(uint32_t hWnd)
{
    Window *window = window_of(hWnd);

    if (window == NULL) return 0;
    call_proc(window, hWnd, WM_DESTROY, 0, 0);
    call_proc(window, hWnd, WM_NCDESTROY, 0, 0);
    window->used = 0;
    if (main_window == hWnd) main_window = 0;
    return 1;
}

uint32_t CCALL DefWindowProcA_c(uint32_t hWnd, uint32_t Msg_, uint32_t wParam, uint32_t lParam)
{
    (void) wParam;
    (void) lParam;
    switch (Msg_)
    {
    case WM_CLOSE:
        DestroyWindow_c(hWnd);
        return 0;
    case WM_NCCREATE:
    case WM_ERASEBKGND:
    case WM_NCACTIVATE:
    case WM_MOUSEACTIVATE:
        return 1;
    case WM_NCHITTEST:
        return 1; /* HTCLIENT */
    case WM_SETCURSOR:
        return 0;
    default:
        return 0;
    }
}

/* Guest code that only the game thread may run (the program's data structures and scripting are not made for other threads, and
   neither are the questions of a server running beside it): with the big lock the calling thread just takes it, without it the call
   is queued and the game thread runs it at its next look at the message queue (between two frames). The caller waits for the result. */
typedef struct GameCall {
    void (*function)(void *);
    void *argument;
    int state;   /* 0 queued, 1 running, 2 done, 3 taken back (not run) */
    struct GameCall *next;
} GameCall;

static pthread_mutex_t game_call_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t game_call_changed = PTHREAD_COND_INITIALIZER;
static GameCall *game_call_head, *game_call_tail;
static volatile int game_calls_waiting;
static volatile double game_thread_looked;   /* when the game thread last looked at its queue (emscripten_get_now) */

/* timeout_ms < 0: waits as long as it takes. Otherwise the call is taken back when the game thread has not started it within timeout_ms, or
   not queued at all when the game thread has not looked at its queue for that long (a loading screen, a long frame): returns 0 then. */
int SRW_RunOnGameThreadWithin(void (*function)(void *), void *argument, double timeout_ms)
{
    GameCall call = {function, argument, 0, NULL};
    struct timespec until;

    if (SRW_GilEnabled())
    {
        SRW_GilAcquire();
        function(argument);
        SRW_GilRelease();
        return 1;
    }
    if (timeout_ms >= 0 && emscripten_get_now() - game_thread_looked > timeout_ms) return 0;
    pthread_mutex_lock(&game_call_lock);
    if (game_call_tail != NULL) game_call_tail->next = &call;
    else game_call_head = &call;
    game_call_tail = &call;
    game_calls_waiting = 1;
    pthread_mutex_unlock(&game_call_lock);
    post(main_window, 0 /* WM_NULL */, 0, 0);   /* (wakes a game thread that waits for messages) */
    if (timeout_ms >= 0)
    {
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += (time_t) (timeout_ms / 1000);
        until.tv_nsec += (long) (fmod(timeout_ms, 1000.0) * 1e6);
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&game_call_lock);
    while (call.state < 2)
    {
        if (timeout_ms < 0 || call.state == 1) pthread_cond_wait(&game_call_changed, &game_call_lock);
        else if (pthread_cond_timedwait(&game_call_changed, &game_call_lock, &until) != 0 && call.state == 0)
        {
            /* not started in time: out of the queue */
            GameCall **link = &game_call_head, *previous = NULL;

            while (*link != NULL && *link != &call) { previous = *link; link = &(*link)->next; }
            if (*link == &call)
            {
                *link = call.next;
                if (game_call_tail == &call) game_call_tail = previous;
            }
            call.state = 3;
        }
    }
    pthread_mutex_unlock(&game_call_lock);
    return call.state == 2;
}

void SRW_RunOnGameThread(void (*function)(void *), void *argument)
{
    SRW_RunOnGameThreadWithin(function, argument, -1);
}

#define run_on_game_thread SRW_RunOnGameThread

static void run_game_calls(void)
{
    game_thread_looked = emscripten_get_now();
    SRW_SafePoint(1);
    if (!game_calls_waiting) return;
    pthread_mutex_lock(&game_call_lock);
    while (game_call_head != NULL)
    {
        GameCall *call = game_call_head;

        game_call_head = call->next;
        if (game_call_head == NULL) game_call_tail = NULL;
        call->state = 1;
        pthread_mutex_unlock(&game_call_lock);
        call->function(call->argument);
        pthread_mutex_lock(&game_call_lock);
        call->state = 2;
        pthread_cond_broadcast(&game_call_changed);
    }
    game_calls_waiting = 0;
    pthread_mutex_unlock(&game_call_lock);
}

#ifdef SRW_DIAGNOSTICS
/* Scripted input for runs without a person (Node): /autoinput.txt, one command per line, given to the main window from a thread of its own:
   wait MS | key VK (hex: 0d Enter, 09 Tab, 1b Escape...) | type TEXT | say TEXT (Enter, the text, Enter: chat and /script) | log TEXT |
   active 0|1 (background or foreground, as the page tells it) | hang MS (holds the big lock: a simulated freeze) |
   sched (the program's scheduler state while the game thread waits in its main loop) | resize W H (the canvas' new size).
   Lines with # are comments. */
static EM_BOOL on_key(int type, const EmscriptenKeyboardEvent *e, void *user);
static EM_BOOL on_mouse(int type, const EmscriptenMouseEvent *e, void *user);
static EM_BOOL on_focus(int type, const EmscriptenFocusEvent *e, void *user);
static EM_BOOL on_visibility(int type, const EmscriptenVisibilityChangeEvent *e, void *user);
static EM_BOOL on_pointer_lock(int type, const EmscriptenPointerlockChangeEvent *e, void *user);
static void release_input(const char *why);
static uint8_t mouse_held;
static void autoinput_clipboard(const char *text, int set);
static void inject_text(const char *utf8, uint32_t lparam);
static int utf8_next(const unsigned char *p, uint32_t *code);

static void autoinput_key(uint32_t vk, int character)
{
    uint32_t lparam = 1 | ((vk & 0xff) << 16);

    key_down[vk & 0xff] = 1;
    post(main_window, WM_KEYDOWN, vk, lparam);
    if (character) post(main_window, WM_CHAR, (uint32_t) character, lparam);
    usleep(30000);
    key_down[vk & 0xff] = 0;
    post(main_window, WM_KEYUP, vk, lparam | (1u << 30) | (1u << 31));
    usleep(30000);
}

/* type TEXT: through the entry point all text takes (SRW_InjectText), one character at a time */
static void autoinput_type(const char *text)
{
    while (*text)
    {
        uint32_t code;
        int used = utf8_next((const unsigned char *) text, &code);
        char one[5] = {0, 0, 0, 0, 0};

        memcpy(one, text, (size_t) used);
        inject_text(one, 1);
        text += used;
        usleep(10000);
    }
}

static void hang_call(void *argument)
{
    usleep((useconds_t) (uintptr_t) argument * 1000);
}

static void *autoinput_main(void *arg)
{
    FILE *f = (FILE *) arg;
    char line[1024];

    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *end = line + strlen(line), *text;

        while (end > line && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
        if (line[0] == '#' || line[0] == 0) continue;
        text = strchr(line, ' ');
        text = (text != NULL) ? text + 1 : end;
        while (main_window == 0) usleep(100000);
        logprintf("Autoinput: %s", line);
        if (strncmp(line, "unlessready ", 12) == 0)   /* unlessready COMMAND: the command only before the program is ready (a login is faster or slower than a script thinks) */
        {
            if (SRW_ProgramReady()) continue;
            memmove(line, text, strlen(text) + 1);
            end = line + strlen(line);
            text = strchr(line, ' ');
            text = (text != NULL) ? text + 1 : end;
        }
        if (strncmp(line, "wait ", 5) == 0) usleep((useconds_t) strtoul(text, NULL, 10) * 1000);
        else if (strncmp(line, "waitready ", 10) == 0)   /* waitready MS: until the program is ready (at most MS), then 3 s more */
        {
            double until = emscripten_get_now() + (double) strtoul(text, NULL, 10);

            while (!SRW_ProgramReady() && emscripten_get_now() < until) usleep(100000);
            if (SRW_ProgramReady()) usleep(3000000);
            logprintf("Autoinput: %s", SRW_ProgramReady() ? "ready" : "not ready yet");
        }
        else if (strncmp(line, "key ", 4) == 0)
        {
            uint32_t vk = (uint32_t) strtoul(text, NULL, 16);

            autoinput_key(vk, (vk == 0x0d || vk == 0x09 || vk == 0x1b || vk == 0x08) ? (int) vk : 0);
        }
        else if (strncmp(line, "type ", 5) == 0) autoinput_type(text);
        else if (strncmp(line, "typew ", 6) == 0)   /* typew HEX...: one WM_CHAR per number (experiment: a code unit above 0xff) */
        {
            char *rest = text;

            for (;;)
            {
                uint32_t value = (uint32_t) strtoul(rest, &rest, 16);

                if (value == 0) break;
                post(main_window, WM_CHAR, value, 1);
                usleep(10000);
            }
        }
        else if (strncmp(line, "typek ", 6) == 0)
        {
            /* typek TEXT: every character as a key press (key down, character, key up) */
            for (; *text; text++) autoinput_key((uint32_t) toupper((unsigned char) *text), (unsigned char) *text);
        }
        else if (strncmp(line, "evkey ", 6) == 0)   /* evkey down|up KEY KEYCODE [FLAGS]: the page's key event (on_key) as the browser makes it; KEY is the event's key ("a", "ř", "Dead", "Enter"), FLAGS letters: s shift, c ctrl, a alt, m meta, r repeat, R right side */
        {
            char direction[8] = "", flags[16] = "";
            EmscriptenKeyboardEvent e;
            unsigned int code = 0;

            memset(&e, 0, sizeof(e));
            if (sscanf(text, "%7s %31s %u %15s", direction, e.key, &code, flags) >= 3)
            {
                e.keyCode = code;
                e.shiftKey = strchr(flags, 's') != NULL;
                e.ctrlKey = strchr(flags, 'c') != NULL;
                e.altKey = strchr(flags, 'a') != NULL;
                e.metaKey = strchr(flags, 'm') != NULL;
                e.repeat = strchr(flags, 'r') != NULL;
                e.location = strchr(flags, 'R') != NULL ? 2 : 0;
                on_key(strcmp(direction, "up") == 0 ? EMSCRIPTEN_EVENT_KEYUP : EMSCRIPTEN_EVENT_KEYDOWN, &e, NULL);
            }
        }
        else if (strncmp(line, "evmouse ", 8) == 0)   /* evmouse down|up|move BUTTON BUTTONS [FLAGS]: the page's mouse event (on_mouse): the button that changed (0 left, 1 middle, 2 right), the `buttons` bits it carries (1 left, 2 right, 4 middle), FLAGS s c a */
        {
            char direction[8] = "", flags[16] = "";
            EmscriptenMouseEvent e;
            unsigned int button = 0, buttons = 0;

            memset(&e, 0, sizeof(e));
            if (sscanf(text, "%7s %u %u %15s", direction, &button, &buttons, flags) >= 3)
            {
                e.button = (unsigned short) button;
                e.buttons = (unsigned short) buttons;
                e.shiftKey = strchr(flags, 's') != NULL;
                e.ctrlKey = strchr(flags, 'c') != NULL;
                e.altKey = strchr(flags, 'a') != NULL;
                e.clientX = cursor_x;
                e.clientY = cursor_y;
                on_mouse(strcmp(direction, "down") == 0 ? EMSCRIPTEN_EVENT_MOUSEDOWN : strcmp(direction, "up") == 0 ? EMSCRIPTEN_EVENT_MOUSEUP : EMSCRIPTEN_EVENT_MOUSEMOVE, &e, NULL);
            }
        }
        else if (strcmp(line, "blur") == 0) on_focus(EMSCRIPTEN_EVENT_BLUR, NULL, NULL);   /* the window loses the focus (the page's blur event) */
        else if (strcmp(line, "hidden") == 0)   /* the tab is hidden */
        {
            EmscriptenVisibilityChangeEvent e;

            memset(&e, 0, sizeof(e));
            e.hidden = 1;
            on_visibility(EMSCRIPTEN_EVENT_VISIBILITYCHANGE, &e, NULL);
        }
        else if (strcmp(line, "unlock") == 0)   /* the pointer lock is lost */
        {
            EmscriptenPointerlockChangeEvent e;

            memset(&e, 0, sizeof(e));
            on_pointer_lock(EMSCRIPTEN_EVENT_POINTERLOCKCHANGE, &e, NULL);
        }
        else if (strncmp(line, "keystate ", 9) == 0)   /* keystate VK-hex: what the program would read for the key (GetKeyState), and the buttons it believes down */
            logprintf("Keystate %x = %d, mouse buttons held %d", (unsigned int) strtoul(text, NULL, 16), key_down[strtoul(text, NULL, 16) & 0xff], (int) mouse_held);
        else if (strncmp(line, "clip ", 5) == 0) autoinput_clipboard(text, 1);   /* clip TEXT: the clipboard's text (\n is a line feed), as a paste event brings it */
        else if (strcmp(line, "clipunits") == 0) autoinput_clipboard(NULL, 2);    /* the clipboard as the program gets it for CF_UNICODETEXT: the count of UTF-16 units */
        else if (strcmp(line, "clipshow") == 0) autoinput_clipboard(NULL, 0);     /* the clipboard's text as the program left it (hex) */
        else if (strncmp(line, "mem ", 4) == 0)
        {
            /* mem ADDR [COUNT]: dwords of the program's memory */
            char *rest;
            uint32_t address = (uint32_t) strtoul(text, &rest, 16), count = (uint32_t) strtoul(rest, NULL, 10), k;

            if (count == 0) count = 1;
            for (k = 0; k < count && k < 16; k++) logprintf("Mem %x = %x", (unsigned int) (address + 4 * k), (unsigned int) *(uint32_t *) (uintptr_t) (address + 4 * k));
        }
        else if (strncmp(line, "active ", 7) == 0) SRW_SetActive(atoi(text));   /* as the page does: foreground (1) or background (0) */
        else if (strncmp(line, "hang ", 5) == 0)
        {
            /* the game stops for that long (the game thread sleeps, or this thread keeps the big lock): to see the watchdog's freeze report */
            run_on_game_thread(hang_call, (void *) (uintptr_t) strtoul(text, NULL, 10));
        }
        else if (strncmp(line, "resize ", 7) == 0)   /* the page's canvas changed its size: resize W H */
        {
            int width = 0, height = 0;

            if (sscanf(text, "%d %d", &width, &height) == 2) SRW_CanvasResized(width, height);
        }
        else if (strncmp(line, "canvas ", 7) == 0)   /* the canvas' size without telling the game: canvas W H */
        {
            int width = 0, height = 0;

            if (sscanf(text, "%d %d", &width, &height) == 2)
            {
                screen_width = width;
                screen_height = height;
            }
        }
        else if (strncmp(line, "mouse ", 6) == 0)   /* a mouse move to X Y (as on_mouse posts it) */
        {
            int x = 0, y = 0;

            if (sscanf(text, "%d %d", &x, &y) == 2)
            {
                cursor_x = x;
                cursor_y = y;
                post(main_window, WM_SETCURSOR, main_window, 1u | (WM_MOUSEMOVE << 16));
                post(main_window, WM_MOUSEMOVE, 0, ((uint32_t) y << 16) | ((uint32_t) x & 0xffff));
            }
        }
        else if (strncmp(line, "rbutton ", 8) == 0)   /* rbutton down|up: the right button at the current pointer (the mouse look) */
        {
            int down = strncmp(text, "down", 4) == 0;

            if (down) key_down[0x02] = 1; else key_down[0x02] = 0;
            post(main_window, WM_SETCURSOR, main_window, 1u | ((down ? WM_RBUTTONDOWN : WM_RBUTTONUP) << 16));
            post(main_window, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, down ? 0x02 : 0, ((uint32_t) cursor_y << 16) | ((uint32_t) cursor_x & 0xffff));
        }
        else if (strncmp(line, "lbutton ", 8) == 0)   /* lbutton down|up: the left button at the current pointer (a click: down, wait, up) */
        {
            int down = strncmp(text, "down", 4) == 0;

            if (down) key_down[0x01] = 1; else key_down[0x01] = 0;
            post(main_window, WM_SETCURSOR, main_window, 1u | ((down ? WM_LBUTTONDOWN : WM_LBUTTONUP) << 16));
            post(main_window, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? 0x01 : 0, ((uint32_t) cursor_y << 16) | ((uint32_t) cursor_x & 0xffff));
        }
        else if (strncmp(line, "mouserel ", 9) == 0)   /* mouserel DX DY: a relative move of the pointer, as the page's mouse events with the pointer locked make */
        {
            int dx = 0, dy = 0;

            if (sscanf(text, "%d %d", &dx, &dy) == 2)
            {
                cursor_x += dx;
                cursor_y += dy;
                post(main_window, WM_MOUSEMOVE, 0x02, ((uint32_t) cursor_y << 16) | ((uint32_t) cursor_x & 0xffff));
            }
        }
        else if (strncmp(line, "gil ", 4) == 0) {   /* gil game [budget ms [minimum slice ms]] | fair: the scheduling of the big lock */
            const char *game = strstr(line, "game");
            char *rest = NULL;
            double budget = game != NULL ? strtod(game + 4, &rest) : 0;
            double slice = (game != NULL && rest != NULL) ? strtod(rest, NULL) : 0;

            SRW_GilSetMode(game != NULL, budget, slice);
        }
        else if (strncmp(line, "prof", 4) == 0) SRW_ProfSet(strstr(line, "off") == NULL);   /* prof [on|off]: the frame time chart (logged in the perf category here, there is no page) */
        else if (strcmp(line, "sched") == 0) SRW_DescribeGameThread();
        else if (strcmp(line, "quit") == 0)   /* the process ends at once (a clean exit: node --cpu-prof writes its profiles then) */
        {
            logprint("Autoinput: quit");
            emscripten_force_exit(0);
        }
        else if (strncmp(line, "say ", 4) == 0)
        {
            autoinput_key(0x0d, 0x0d);
            usleep(200000);
            autoinput_type(text);
            usleep(100000);
            autoinput_key(0x0d, 0x0d);
        }
        else SRW_AutoinputCommand(line, text);
    }
    fclose(f);
    logprintf("Autoinput: the end of the script");
    return NULL;
}

static void start_autoinput(void)
{
    static int started;   /* (a restart of the graphics makes a new main window: the script must not start again) */
    FILE *f;
    pthread_t thread;

    if (started) return;
    started = 1;
    f = fopen("/Port/autoinput.txt", "r");
    if (f == NULL) return;
    if (pthread_create(&thread, NULL, autoinput_main, f) != 0) fclose(f);
    else pthread_detach(thread);
}
#endif

/* A sound system that registers for the activation event only when it starts (seconds after the window was shown and told it is active)
   takes itself for a program in the background and mutes every sound until the next activation change. It calls this to be told again. */
void SRW_ReactivateWindow(void)
{
    if (main_window == 0) return;
    post(main_window, WM_ACTIVATEAPP, app_active, 0);
    post(main_window, WM_ACTIVATE, app_active, 0);
}

/* The page says whether the game is in the foreground (web/controls.js: the Game pane is shown and, once the player has done anything in
   it, the tab is visible and has the focus). In the background the program throttles itself (a few frames per second) and mutes its
   sound unless "sound in background" is on, as on Windows. */
static void release_input(const char *why);

EMSCRIPTEN_KEEPALIVE void SRW_SetActive(int active)
{
    active = active != 0;
    if (active == app_active) return;
    app_active = active;
    if (!active) release_input("the game went to the background");   /* the program sees its keys and buttons up, as on Windows */
    SRW_LOG(SRW_LOG_INPUT, "The game is told it is %s", active ? "in the foreground" : "in the background");
    if (main_window == 0) return;
    post(main_window, WM_NCACTIVATE, (uint32_t) active, 0);
    post(main_window, WM_ACTIVATE, (uint32_t) active, 0);
    post(main_window, WM_ACTIVATEAPP, (uint32_t) active, 0);
    post(main_window, active ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}

/* A windowed program (a bordered or a maximized borderless window alike) renders at the canvas' size, as a window resized by the
   user on Windows: the window gets the size and a WM_SIZE, and the program resets its device to it. A program in full screen keeps
   its resolution (the picture is scaled to the canvas). */
static volatile int device_windowed;   /* the last device is windowed (SRW_NoteDevice) */

#ifdef SRW_DIAGNOSTICS
volatile double SRW_ResizeStarted;   /* when the last WM_SIZE for a new canvas size was posted (0: none to time) */
#endif

static void resize_main_window(int width, int height, const char *why)
{
    Window *window = window_of(main_window);

    if (window == NULL || !window->visible || width < 320 || height < 240) return;
    if (window->width == width && window->height == height) return;
    SRW_LOG(SRW_LOG_INPUT, "The canvas is %dx%d (%s): the window is resized from %dx%d", width, height, why, (int) window->width, (int) window->height);
    window->width = width;
    window->height = height;
#ifdef SRW_DIAGNOSTICS
    SRW_ResizeStarted = emscripten_get_now();   /* (d3d9-bridge.cpp times the Reset and the frames after it) */
#endif
    post(main_window, WM_SIZE, 0, ((uint32_t) height << 16) | ((uint32_t) width & 0xffff));
}

/* A program can ask for a fixed aspect (SRW_WindowAspect), for example when rebuilding its interface for a new one is slow. The window then
   gets the largest size of that aspect within the canvas (the page letterboxes the picture). The program compares aspects with 0.001: the
   size is chosen among a few near ones for the aspect closest to the wanted one. */
static void fit_to_ui(int width, int height, int *out_width, int *out_height)
{
    float aspect;
    int best_w = width, best_h = height, h, w;
    double best_error = 1e9;

    *out_width = width;
    *out_height = height;
    if (!SRW_WindowAspect(&aspect) || height <= 0 || fabs((double) width / height - aspect) < 0.0005) return;
    if ((double) width / height > aspect)
    {
        for (h = height; h > height - 64 && h >= 240; h--)   /* the canvas is wider: the full height, bars at the sides */
        {
            double e;

            w = (int) lround(h * aspect);
            e = fabs((double) w / h - aspect);
            if (w <= width && e < best_error) { best_error = e; best_w = w; best_h = h; }
            if (e < 0.0001) break;
        }
    }
    else
    {
        for (w = width; w > width - 64 && w >= 320; w--)   /* the canvas is taller: the full width, bars above and below */
        {
            double e;

            h = (int) lround(w / aspect);
            e = fabs((double) w / h - aspect);
            if (h <= height && e < best_error) { best_error = e; best_w = w; best_h = h; }
            if (e < 0.0001) break;
        }
    }
    *out_width = best_w;
    *out_height = best_h;
}

EMSCRIPTEN_KEEPALIVE void SRW_CanvasResized(int width, int height)
{
    int w, h;

    screen_width = width;
    screen_height = height;
    fit_to_ui(width, height, &w, &h);
    if (device_windowed) resize_main_window(w, h, w != width || h != height ? "resized, the UI's aspect kept" : "resized");
}

/* the canvas' size, and the window resized to all of it (the UI is rebuilt) */
void SRW_CanvasSize(int *width, int *height)
{
    *width = screen_width;
    *height = screen_height;
}

void SRW_ResizeToCanvas(void)
{
    if (device_windowed) resize_main_window(screen_width, screen_height, "the UI is rebuilt for the canvas' aspect");
}

/* A device was created (d3d9-bridge.cpp; not for Reset): a windowed one of another size than the canvas (the program creates it at
   its settings, also after a restart of the graphics) is resized to the canvas at once (once per device: no loop if the program keeps its size). */
void SRW_NoteDevice(int windowed, int width, int height)
{
    Window *window = window_of(main_window);

    device_windowed = windowed;
    if (!windowed || window == NULL) return;
    query_screen_size();
    {
        int w, h;

        fit_to_ui(screen_width, screen_height, &w, &h);
        if (w == width && h == height) return;
        window->width = width;    /* (the window's size is the device's: the canvas' size is posted even if the window had it) */
        window->height = height;
        resize_main_window(w, h, "a new device");
    }
}

uint32_t CCALL ShowWindow_c(uint32_t hWnd, uint32_t nCmdShow)
{
    Window *window = window_of(hWnd);
    int was;

    if (window == NULL) return 0;
    was = window->visible;
    window->visible = (nCmdShow != 0);
    if (window->visible && !was)
    {
        call_proc(window, hWnd, WM_SHOWWINDOW, 1, 0);
        call_proc(window, hWnd, WM_SIZE, 0, ((uint32_t) window->height << 16) | (uint32_t) (window->width & 0xffff));
        post(hWnd, WM_NCACTIVATE, (uint32_t) app_active, 0);
        post(hWnd, WM_ACTIVATE, (uint32_t) app_active, 0);
        post(hWnd, WM_ACTIVATEAPP, (uint32_t) app_active, 0);
        post(hWnd, app_active ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
        post(hWnd, WM_PAINT, 0, 0);
    }
    return was;
}

/* Windows tells the window procedure at once (from inside the call) that the window has been moved or resized: the program learns its new size that way */
static void window_changed(Window *window, uint32_t hWnd, int moved, int resized)
{
    if (moved) call_proc(window, hWnd, WM_MOVE, 0, ((uint32_t) window->y << 16) | ((uint32_t) window->x & 0xffff));
    if (resized) call_proc(window, hWnd, WM_SIZE, 0, ((uint32_t) window->height << 16) | ((uint32_t) window->width & 0xffff));
}

uint32_t CCALL MoveWindow_c(uint32_t hWnd, uint32_t X, uint32_t Y, uint32_t nWidth, uint32_t nHeight, uint32_t bRepaint)
{
    Window *window = window_of(hWnd);
    int moved, resized;

    (void) bRepaint;
    if (window == NULL) return 0;
    moved = window->x != (int32_t) X || window->y != (int32_t) Y;
    resized = window->width != (int32_t) nWidth || window->height != (int32_t) nHeight;
    window->x = (int32_t) X;
    window->y = (int32_t) Y;
    window->width = (int32_t) nWidth;
    window->height = (int32_t) nHeight;
    if (window->visible) window_changed(window, hWnd, moved, resized);
    return 1;
}

uint32_t CCALL SetWindowPos_c(uint32_t hWnd, uint32_t hWndInsertAfter, uint32_t X, uint32_t Y, uint32_t cx, uint32_t cy, uint32_t uFlags)
{
    Window *window = window_of(hWnd);
    int moved = 0, resized = 0;

    (void) hWndInsertAfter;
    if (window == NULL) return 0;
    if ((uFlags & 2) == 0) /* SWP_NOMOVE */
    {
        moved = window->x != (int32_t) X || window->y != (int32_t) Y;
        window->x = (int32_t) X;
        window->y = (int32_t) Y;
    }
    if ((uFlags & 1) == 0) /* SWP_NOSIZE */
    {
        resized = window->width != (int32_t) cx || window->height != (int32_t) cy;
        window->width = (int32_t) cx;
        window->height = (int32_t) cy;
    }
    if ((uFlags & 0x40) != 0) window->visible = 1;  /* SWP_SHOWWINDOW */
    if ((uFlags & 0x80) != 0) window->visible = 0;  /* SWP_HIDEWINDOW */
    if (window->visible) window_changed(window, hWnd, moved, resized);
    return 1;
}

uint32_t CCALL BringWindowToTop_c(uint32_t hWnd)
{
    return window_of(hWnd) != NULL;
}

uint32_t CCALL SetParent_c(uint32_t hWndChild, uint32_t hWndNewParent)
{
    Window *window = window_of(hWndChild);
    uint32_t old;

    if (window == NULL) return 0;
    old = window->parent;
    window->parent = hWndNewParent;
    return old;
}

uint32_t CCALL GetParent_c(uint32_t hWnd)
{
    Window *window = window_of(hWnd);

    return (window != NULL) ? window->parent : 0;
}

uint32_t CCALL GetWindow_c(uint32_t hWnd, uint32_t uCmd)
{
    (void) hWnd;
    (void) uCmd;
    return 0;
}

uint32_t CCALL IsWindow_c(uint32_t hWnd)
{
    return window_of(hWnd) != NULL;
}

uint32_t CCALL IsWindowEnabled_c(uint32_t hWnd)
{
    Window *window = window_of(hWnd);

    return window != NULL && window->enabled;
}

uint32_t CCALL IsWindowVisible_c(uint32_t hWnd)
{
    Window *window = window_of(hWnd);

    return window != NULL && window->visible;
}

uint32_t CCALL IsIconic_c(uint32_t hWnd)
{
    (void) hWnd;
    return 0;
}

uint32_t CCALL IsZoomed_c(uint32_t hWnd)
{
    (void) hWnd;
    return 0;
}

uint32_t CCALL GetActiveWindow_c(void)
{
    return main_window;
}

uint32_t CCALL GetFocus_c(void)
{
    return main_window;
}

uint32_t CCALL GetForegroundWindow_c(void)
{
    return main_window;
}

uint32_t CCALL SetFocus_c(uint32_t hWnd)
{
    (void) hWnd;
    return main_window;
}

uint32_t CCALL WindowFromPoint_c(uint32_t x, uint32_t y)
{
    (void) x;
    (void) y;
    return main_window;
}

/* for the Direct3D bridge: the program size of a window (the main window when the handle is not one) */
int SRW_WindowClientSize(uint32_t hwnd, int *width, int *height)
{
    Window *window = window_of(hwnd);

    if (window == NULL) window = window_of(main_window);
    if (window == NULL) return 0;
    *width = window->width;
    *height = window->height;
    return 1;
}

/* the desktop window: the current display mode (the canvas) */
#define DESKTOP_WINDOW 0x10010

uint32_t CCALL GetDesktopWindow_c(void)
{
    return DESKTOP_WINDOW;
}

static int desktop_rect(uint32_t hWnd, Rect *rect)
{
    int32_t canvas_width, canvas_height, screen_width, screen_height;

    if (hWnd != DESKTOP_WINDOW) return 0;
    SRW_DisplaySizes(&canvas_width, &canvas_height, &screen_width, &screen_height);
    rect->left = 0;
    rect->top = 0;
    rect->right = canvas_width;
    rect->bottom = canvas_height;
    return 1;
}

uint32_t CCALL GetClientRect_c(uint32_t hWnd, uint32_t lpRect)
{
    Window *window = window_of(hWnd);
    Rect *rect = (Rect *) (uintptr_t) lpRect;

    if (desktop_rect(hWnd, rect)) return 1;
    if (window == NULL) return 0;
    rect->left = 0;
    rect->top = 0;
    rect->right = window->width;
    rect->bottom = window->height;
    return 1;
}

uint32_t CCALL GetWindowRect_c(uint32_t hWnd, void *lpRect)
{
    Window *window = window_of(hWnd);
    Rect *rect = lpRect;

    if (desktop_rect(hWnd, rect)) return 1;
    if (window == NULL) return 0;
    rect->left = window->x;
    rect->top = window->y;
    rect->right = window->x + window->width;
    rect->bottom = window->y + window->height;
    return 1;
}

uint32_t CCALL AdjustWindowRectEx_c(void *lpRect, uint32_t dwStyle, uint32_t bMenu, uint32_t dwExStyle)
{
    (void) lpRect;
    (void) dwStyle;
    (void) bMenu;
    (void) dwExStyle;
    return 1; /* the window has no frame */
}

uint32_t CCALL GetWindowPlacement_c(uint32_t hWnd, void *lpwndpl)
{
    Window *window = window_of(hWnd);
    uint32_t *p = lpwndpl;

    if (window == NULL) return 0;
    memset(p, 0, 44);
    p[0] = 44;
    p[2] = window->visible ? 1 : 0; /* SW_SHOWNORMAL */
    p[7] = (uint32_t) window->x;
    p[8] = (uint32_t) window->y;
    p[9] = (uint32_t) (window->x + window->width);
    p[10] = (uint32_t) (window->y + window->height);
    return 1;
}

uint32_t CCALL SetWindowPlacement_c(uint32_t hWnd, void *lpwndpl)
{
    Window *window = window_of(hWnd);
    const int32_t *p = lpwndpl;

    if (window == NULL) return 0;
    window->x = p[7];
    window->y = p[8];
    window->width = p[9] - p[7];
    window->height = p[10] - p[8];
    return 1;
}

uint32_t CCALL GetWindowInfo_c(uint32_t hwnd, void *pwi)
{
    Window *window = window_of(hwnd);
    uint32_t *p = pwi;

    if (window == NULL) return 0;
    memset(p, 0, 60);
    p[0] = 60;
    p[1] = (uint32_t) window->x;
    p[2] = (uint32_t) window->y;
    p[3] = (uint32_t) (window->x + window->width);
    p[4] = (uint32_t) (window->y + window->height);
    p[5] = (uint32_t) window->x;
    p[6] = (uint32_t) window->y;
    p[7] = (uint32_t) (window->x + window->width);
    p[8] = (uint32_t) (window->y + window->height);
    p[9] = window->style;
    p[10] = window->exstyle;
    p[11] = 1;
    return 1;
}

uint32_t CCALL ClientToScreen_c(uint32_t hWnd, void *lpPoint)
{
    (void) hWnd;
    (void) lpPoint; /* the program area starts at the top left of the page */
    return 1;
}

uint32_t CCALL ScreenToClient_c(uint32_t hWnd, void *lpPoint)
{
    (void) hWnd;
    (void) lpPoint;
    return 1;
}

uint32_t CCALL MapWindowPoints_c(uint32_t hWndFrom, uint32_t hWndTo, void *lpPoints, uint32_t cPoints)
{
    (void) hWndFrom;
    (void) hWndTo;
    (void) lpPoints;
    (void) cPoints;
    return 0;
}

uint32_t CCALL GetWindowTextA_c(uint32_t hWnd, void *lpString, uint32_t nMaxCount)
{
    Window *window = window_of(hWnd);
    char *out = lpString;

    if (window == NULL || nMaxCount == 0) return 0;
    snprintf(out, nMaxCount, "%s", window->title);
    return (uint32_t) strlen(out);
}

uint32_t CCALL GetWindowTextLengthA_c(uint32_t hWnd)
{
    Window *window = window_of(hWnd);

    return (window != NULL) ? (uint32_t) strlen(window->title) : 0;
}

uint32_t CCALL SetWindowTextA_c(uint32_t hWnd, const char *lpString)
{
    Window *window = window_of(hWnd);

    if (window == NULL) return 0;
    snprintf(window->title, sizeof(window->title), "%s", lpString);
    MAIN_THREAD_EM_ASM({
        if (typeof document !== 'undefined') document.title = UTF8ToString($0);
    }, window->title);
    return 1;
}

uint32_t CCALL GetClassLongA_c(uint32_t hWnd, uint32_t nIndex)
{
    (void) hWnd;
    (void) nIndex;
    return 0;
}

uint32_t CCALL SetClassLongA_c(uint32_t hWnd, uint32_t nIndex, uint32_t dwNewLong)
{
    (void) hWnd;
    (void) nIndex;
    (void) dwNewLong;
    return 0;
}

uint32_t CCALL InvalidateRect_c(uint32_t hWnd, void *lpRect, uint32_t bErase)
{
    (void) lpRect;
    (void) bErase;
    return window_of(hWnd) != NULL;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* painting and other drawing: the program draws through Direct3D; the few GDI calls draw nothing */

uint32_t CCALL BeginPaint_c(uint32_t hWnd, void *lpPaint)
{
    Window *window = window_of(hWnd);

    memset(lpPaint, 0, 64);
    if (window != NULL)
    {
        int32_t *p = lpPaint;

        p[0] = 0x20001; /* the device context */
        p[3] = 0;
        p[4] = 0;
        p[5] = window->width;
        p[6] = window->height;
    }
    return 0x20001;
}

uint32_t CCALL EndPaint_c(uint32_t hWnd, void *lpPaint)
{
    (void) hWnd;
    (void) lpPaint;
    return 1;
}

uint32_t CCALL GetDC_c(uint32_t hWnd)
{
    (void) hWnd;
    return 0x20001;
}

uint32_t CCALL ReleaseDC_c(uint32_t hWnd, uint32_t hDC)
{
    (void) hWnd;
    (void) hDC;
    return 1;
}

uint32_t CCALL FillRect_c(uint32_t hDC, void *lprc, uint32_t hbr)
{
    (void) hDC;
    (void) lprc;
    (void) hbr;
    return 1;
}

uint32_t CCALL FrameRect_c(uint32_t hDC, void *lprc, uint32_t hbr)
{
    (void) hDC;
    (void) lprc;
    (void) hbr;
    return 1;
}

uint32_t CCALL InvertRect_c(uint32_t hDC, void *lprc)
{
    (void) hDC;
    (void) lprc;
    return 1;
}

uint32_t CCALL DrawTextA_c(uint32_t hDC, const char *lpchText, uint32_t cchText, void *lprc, uint32_t format)
{
    (void) hDC;
    (void) lpchText;
    (void) cchText;
    (void) lprc;
    (void) format;
    return 0;
}

uint32_t CCALL DrawIconEx_c(const uint32_t *args)
{
    (void) args;
    return 1;
}

uint32_t CCALL DrawMenuBar_c(uint32_t hWnd)
{
    (void) hWnd;
    return 1;
}

uint32_t CCALL GetSysColor_c(uint32_t nIndex)
{
    (void) nIndex;
    return 0x00c0c0c0;
}

uint32_t CCALL GetSysColorBrush_c(uint32_t nIndex)
{
    return 0x30000 + nIndex;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* resources, dialogs, menus: nothing is available, but nothing is required either */

/* the handle of a cursor is 0x40000 + the number of the system cursor (IDC_ARROW is 32512...), a cursor of the program by name is the arrow */
uint32_t CCALL LoadCursorA_c(uint32_t hInstance, const char *lpCursorName)
{
    (void) hInstance;
    return 0x40000 + ((uintptr_t) lpCursorName < 0x10000 ? (uint32_t) (uintptr_t) lpCursorName : 32512u);
}

uint32_t CCALL LoadImageA_c(uint32_t hinst, const char *lpszName, uint32_t uType, uint32_t cxDesired, uint32_t cyDesired, uint32_t fuLoad)
{
    (void) hinst;
    (void) cxDesired;
    (void) cyDesired;
    (void) fuLoad;
    return 0x50000 + uType * 0x1000 + ((uintptr_t) lpszName & 0xfff);
}

uint32_t CCALL LoadBitmapA_c(uint32_t hInstance, const char *lpBitmapName)
{
    (void) hInstance;
    return 0x60000 + ((uintptr_t) lpBitmapName & 0xffff);
}

uint32_t CCALL LoadStringA_c(uint32_t hInstance, uint32_t uID, void *lpBuffer, uint32_t cchBufferMax)
{
    (void) hInstance;
    (void) uID;
    if (cchBufferMax > 0) *(char *) lpBuffer = 0;
    return 0;
}

uint32_t CCALL DestroyIcon_c(uint32_t hIcon)
{
    (void) hIcon;
    return 1;
}

uint32_t CCALL CreateMenu_c(void)
{
    return 0x70001;
}

uint32_t CCALL CreatePopupMenu_c(void)
{
    return 0x70002;
}

uint32_t CCALL DestroyMenu_c(uint32_t hMenu)
{
    (void) hMenu;
    return 1;
}

uint32_t CCALL DeleteMenu_c(uint32_t hMenu, uint32_t uPosition, uint32_t uFlags)
{
    (void) hMenu;
    (void) uPosition;
    (void) uFlags;
    return 1;
}

uint32_t CCALL GetMenu_c(uint32_t hWnd)
{
    (void) hWnd;
    return 0;
}

uint32_t CCALL SetMenu_c(uint32_t hWnd, uint32_t hMenu)
{
    (void) hWnd;
    (void) hMenu;
    return 1;
}

uint32_t CCALL GetMenuItemCount_c(uint32_t hMenu)
{
    (void) hMenu;
    return 0;
}

uint32_t CCALL GetMenuItemInfoA_c(uint32_t hMenu, uint32_t item, uint32_t fByPosition, void *lpmii)
{
    (void) hMenu;
    (void) item;
    (void) fByPosition;
    (void) lpmii;
    return 0;
}

uint32_t CCALL SetMenuItemInfoA_c(uint32_t hMenu, uint32_t item, uint32_t fByPositon, void *lpmii)
{
    (void) hMenu;
    (void) item;
    (void) fByPositon;
    (void) lpmii;
    return 1;
}

uint32_t CCALL InsertMenuItemA_c(uint32_t hMenu, uint32_t item, uint32_t fByPosition, void *lpmii)
{
    (void) hMenu;
    (void) item;
    (void) fByPosition;
    (void) lpmii;
    return 1;
}

uint32_t CCALL TrackPopupMenu_c(uint32_t hMenu, uint32_t uFlags, uint32_t x, uint32_t y, uint32_t nReserved, uint32_t hWnd, void *prcRect)
{
    (void) hMenu; (void) uFlags; (void) x; (void) y; (void) nReserved; (void) hWnd; (void) prcRect;
    return 0;
}

uint32_t CCALL CreateAcceleratorTableA_c(void *lpaccl, uint32_t cEntries)
{
    (void) lpaccl;
    (void) cEntries;
    return 0x80001;
}

uint32_t CCALL DestroyAcceleratorTable_c(uint32_t hAccel)
{
    (void) hAccel;
    return 1;
}

uint32_t CCALL TranslateAcceleratorA_c(uint32_t hWnd, uint32_t hAccTable, void *lpMsg)
{
    (void) hWnd;
    (void) hAccTable;
    (void) lpMsg;
    return 0;
}

uint32_t CCALL CreateDialogIndirectParamA_c(uint32_t hInstance, void *lpTemplate, uint32_t hWndParent, void *lpDialogFunc, uint32_t dwInitParam)
{
    (void) hInstance; (void) lpTemplate; (void) hWndParent; (void) lpDialogFunc; (void) dwInitParam;
    return 0;
}

uint32_t CCALL IsDialogMessageA_c(uint32_t hDlg, void *lpMsg)
{
    (void) hDlg;
    (void) lpMsg;
    return 0;
}

uint32_t CCALL MessageBoxA_c(uint32_t hWnd, const char *lpText, const char *lpCaption, uint32_t uType)
{
    (void) hWnd;
    (void) uType;
    warnprintf("%s: %s", lpCaption != NULL ? lpCaption : SRW_PROGRAM_NAME, lpText != NULL ? lpText : "");
    return 1; /* IDOK */
}

uint32_t CCALL MessageBeep_c(uint32_t uType)
{
    (void) uType;
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the clipboard, connected to the browser's: Ctrl+V is held back until the page's paste event has brought the text (SRW_ClipboardPasted, called
   by web/prerun.js on the main thread), so that the text is there when the game asks for it; what the game copies goes to
   navigator.clipboard.writeText. The text is kept as UTF-8; CF_TEXT (1) is UTF-8 too (the program's code page, see GetACP), CF_UNICODETEXT (13) UTF-16. */

static pthread_mutex_t clipboard_lock = PTHREAD_MUTEX_INITIALIZER;
static char *clipboard_text;          /* UTF-8, or NULL */
static void *clipboard_handle;        /* the memory given out by the last GetClipboardData (freed by the next one) */
static uint32_t pending_paste_lparam; /* the Ctrl+V key down that waits for the paste event (0: none) */
static struct { uint32_t vk, message, lparam, side; } deferred_key_up[8];   /* key ups that came while it waited (main thread only) */
static unsigned int deferred_key_ups;

/* decodes one UTF-8 character; returns the number of bytes used */
static int utf8_next(const unsigned char *p, uint32_t *code)
{
    if (p[0] < 0x80) { *code = p[0]; return 1; }
    if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) { *code = ((p[0] & 0x1fu) << 6) | (p[1] & 0x3fu); return 2; }
    if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) { *code = ((p[0] & 0x0fu) << 12) | ((p[1] & 0x3fu) << 6) | (p[2] & 0x3fu); return 3; }
    if ((p[0] & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80)
    {
        *code = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3fu) << 12) | ((p[2] & 0x3fu) << 6) | (p[3] & 0x3fu);
        return 4;
    }
    *code = '?';
    return 1;
}

/* the page's paste event (main thread): the text (left in Module.srwPasteText, null when none came), then the Ctrl+V that waited for it */
EMSCRIPTEN_KEEPALIVE void SRW_ClipboardPasted(void)
{
    uint32_t lparam;
    int length = EM_ASM_INT({
        var text = Module.srwPasteText;
        return (typeof text === "string") ? lengthBytesUTF8(text) + 1 : 0;
    });
    char *text = length > 0 ? malloc((size_t) length) : NULL;

    if (text != NULL)
        EM_ASM({ stringToUTF8(Module.srwPasteText, $0, $1); }, text, length);
    EM_ASM({ Module.srwPasteText = null; });
    pthread_mutex_lock(&clipboard_lock);
    if (text != NULL)
    {
        free(clipboard_text);
        clipboard_text = text;
    }
    lparam = pending_paste_lparam;
    pending_paste_lparam = 0;
    pthread_mutex_unlock(&clipboard_lock);
    if (lparam != 0 && main_window != 0)
    {
        unsigned int i;

        post(main_window, WM_KEYDOWN, 0x56, lparam);
        for (i = 0; i < deferred_key_ups; i++)
        {
            uint32_t vk = deferred_key_up[i].vk;

            key_down[vk] = 0;
            if (vk == 0x10 || vk == 0x11 || vk == 0x12) key_down[0xa0 + (vk - 0x10) * 2 + deferred_key_up[i].side] = 0;
            post(main_window, deferred_key_up[i].message, vk, deferred_key_up[i].lparam);
        }
    }
    deferred_key_ups = 0;
}

/* no paste event came (the browser did not fire one): ask the asynchronous clipboard API, which may show a permission prompt */
static void paste_fallback(void *user)
{
    uint32_t pending;

    (void) user;
    pthread_mutex_lock(&clipboard_lock);
    pending = pending_paste_lparam;
    pthread_mutex_unlock(&clipboard_lock);
    if (pending == 0) return;
    EM_ASM({
        var done = function (text) {
            Module.srwPasteText = text;
            Module.ccall("SRW_ClipboardPasted", null, [], []);
        };
        if (navigator.clipboard && navigator.clipboard.readText) navigator.clipboard.readText().then(done, function () { done(null); });
        else done(null);
    });
}

uint32_t CCALL OpenClipboard_c(uint32_t hWndNewOwner)
{
    (void) hWndNewOwner;
    return 1;
}

uint32_t CCALL CloseClipboard_c(void)
{
    return 1;
}

uint32_t CCALL EmptyClipboard_c(void)
{
    pthread_mutex_lock(&clipboard_lock);
    free(clipboard_text);
    clipboard_text = NULL;
    pthread_mutex_unlock(&clipboard_lock);
    return 1;
}

/* the game copies: CF_TEXT (UTF-8) or CF_UNICODETEXT (UTF-16) in global memory (the handle is the pointer) */
uint32_t CCALL SetClipboardData_c(uint32_t uFormat, uint32_t hMem)
{
    char *utf8;
    size_t n = 0, capacity;

    if (hMem == 0 || (uFormat != 1 && uFormat != 13)) return hMem;
    if (uFormat == 1)
    {
        /* CF_TEXT is in the code page of the program's ANSI functions: UTF-8 here; bytes that are not valid UTF-8 are taken as Latin-1 characters */
        const unsigned char *p = (const unsigned char *) (uintptr_t) hMem;

        capacity = strlen((const char *) p) * 2 + 1;
        utf8 = malloc(capacity);
        if (utf8 == NULL) return 0;
        while (*p)
        {
            uint32_t c;
            int used = utf8_next(p, &c);

            if (c == '?' && p[0] != '?')   /* (not a valid sequence) */
            {
                utf8[n++] = (char) (0xc0 | (*p >> 6));
                utf8[n++] = (char) (0x80 | (*p & 0x3f));
                p++;
                continue;
            }
            while (used-- > 0) utf8[n++] = (char) *p++;
        }
    }
    else
    {
        const uint16_t *p = (const uint16_t *) (uintptr_t) hMem;
        size_t length = 0;

        while (p[length]) length++;
        capacity = length * 3 + 1;
        utf8 = malloc(capacity);
        if (utf8 == NULL) return 0;
        for (; *p; p++)
        {
            uint32_t c = *p;

            if (c >= 0xd800 && c < 0xdc00 && p[1] >= 0xdc00 && p[1] < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (*++p - 0xdc00);
            if (c < 0x80) utf8[n++] = (char) c;
            else if (c < 0x800) { utf8[n++] = (char) (0xc0 | (c >> 6)); utf8[n++] = (char) (0x80 | (c & 0x3f)); }
            else if (c < 0x10000) { utf8[n++] = (char) (0xe0 | (c >> 12)); utf8[n++] = (char) (0x80 | ((c >> 6) & 0x3f)); utf8[n++] = (char) (0x80 | (c & 0x3f)); }
            else
            {
                utf8[n++] = (char) (0xf0 | (c >> 18));
                utf8[n++] = (char) (0x80 | ((c >> 12) & 0x3f));
                utf8[n++] = (char) (0x80 | ((c >> 6) & 0x3f));
                utf8[n++] = (char) (0x80 | (c & 0x3f));
            }
        }
    }
    utf8[n] = 0;
    pthread_mutex_lock(&clipboard_lock);
    free(clipboard_text);
    clipboard_text = strdup(utf8);
    pthread_mutex_unlock(&clipboard_lock);
    /* the page's clipboard (needs the user activation of the key press, which the browser keeps for a few seconds) */
    MAIN_THREAD_EM_ASM({
        var text = UTF8ToString($0);
        if (typeof navigator !== "undefined" && navigator.clipboard && navigator.clipboard.writeText)
            navigator.clipboard.writeText(text).catch(function (e) { console.warn("[Game] The clipboard could not be written: " + e); });
    }, utf8);
    free(utf8);
    return hMem;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* scroll bars, properties and other window furniture the program does not really use */

uint32_t CCALL GetScrollInfo_c(uint32_t hwnd, uint32_t nBar, void *lpsi)
{
    (void) hwnd;
    (void) nBar;
    (void) lpsi;
    return 0;
}

uint32_t CCALL SetScrollInfo_c(uint32_t hwnd, uint32_t nBar, void *lpsi, uint32_t redraw)
{
    (void) hwnd; (void) nBar; (void) lpsi; (void) redraw;
    return 0;
}

uint32_t CCALL SetScrollPos_c(uint32_t hWnd, uint32_t nBar, uint32_t nPos, uint32_t bRedraw)
{
    (void) hWnd; (void) nBar; (void) nPos; (void) bRedraw;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* input */

uint32_t CCALL GetKeyState_c(uint32_t nVirtKey)
{
    return (nVirtKey < 256 && key_down[nVirtKey]) ? 0x8000u : 0u;
}

uint32_t CCALL GetAsyncKeyState_c(uint32_t vKey)
{
    return (vKey < 256 && key_down[vKey]) ? 0x8000u : 0u;
}

uint32_t CCALL GetKeyboardLayout_c(uint32_t idThread)
{
    (void) idThread;
    return 0x04090409;
}

uint32_t CCALL MapVirtualKeyA_c(uint32_t uCode, uint32_t uMapType)
{
    (void) uMapType;
    return uCode; /* the scan code is not distinguished from the key code */
}

uint32_t CCALL GetCursorPos_c(void *lpPoint)
{
    int32_t *p = lpPoint;
    static int logged;

    if (SRW_LOG_ON(SRW_LOG_INPUT) && logged++ < 30) logprintf("GetCursorPos() = %d, %d", (int) cursor_x, (int) cursor_y);

    p[0] = cursor_x;
    p[1] = cursor_y;
    return 1;
}

uint32_t CCALL SetCursorPos_c(uint32_t X, uint32_t Y)
{
    SRW_LOG(SRW_LOG_INPUT, "SetCursorPos(%d, %d) (the pointer was at %d, %d)", (int) X, (int) Y, (int) cursor_x, (int) cursor_y);
    cursor_x = (int32_t) X;
    cursor_y = (int32_t) Y;
    SRW_CursorWarped();
    return 1;
}

uint32_t CCALL ClipCursor_c(void *lpRect)
{
    (void) lpRect;
    return 1;
}

uint32_t CCALL SetCursor_c(uint32_t hCursor)
{
    static uint32_t previous;
    uint32_t before = previous;

    previous = hCursor;
    SRW_CursorWin32Set(hCursor >= 0x40000 && hCursor < 0x50000 ? hCursor - 0x40000 : 0);
    return before;
}

uint32_t CCALL ShowCursor_c(uint32_t bShow)
{
    return (uint32_t) SRW_CursorWin32Show(bShow != 0);
}

uint32_t CCALL SetCapture_c(uint32_t hWnd)
{
    (void) hWnd;
    return 0;
}

uint32_t CCALL ReleaseCapture_c(void)
{
    return 1;
}

/* SendInput: only mouse moves (MOUSEEVENTF_MOVE, absolute as 0..65535 over the screen or relative) are made, as a warp of the pointer
   like SetCursorPos. Buttons and keys are not made. */
uint32_t CCALL SendInput_c(uint32_t nInputs, void *pInputs, uint32_t cbSize)
{
    uint32_t i;

    if (pInputs == NULL || cbSize < 28) return 0;
    query_screen_size();
    for (i = 0; i < nInputs; i++)
    {
        const uint32_t *input = (const uint32_t *) ((const uint8_t *) pInputs + (size_t) i * cbSize);

        if (input[0] != 0) continue;   /* INPUT_MOUSE */
        if (input[4] & 0x0001)         /* MOUSEEVENTF_MOVE */
        {
            int32_t dx = (int32_t) input[1], dy = (int32_t) input[2];

            if (input[4] & 0x8000)     /* MOUSEEVENTF_ABSOLUTE */
            {
                cursor_x = (int32_t) (((int64_t) dx * screen_width + 32767) / 65535);
                cursor_y = (int32_t) (((int64_t) dy * screen_height + 32767) / 65535);
            }
            else
            {
                cursor_x += dx;
                cursor_y += dy;
            }
            SRW_CursorWarped();
        }
    }
    return nInputs;
}

uint32_t CCALL EnableWindow_c(uint32_t hWnd, uint32_t bEnable)
{
    Window *window = window_of(hWnd);
    int was;

    if (window == NULL) return 0;
    was = window->enabled;
    window->enabled = bEnable != 0;
    return !was;
}

/* Page events arrive on the browser's main thread and go into the queue of the window. Coordinates are relative to
   the page area, which is the program area of the main window. */

/* ------------------------------------------------------------------------------------------------------------------------------------------ */
/* Text. All text enters through SRW_InjectText: typed characters, an input method's result, the scripted input. A character is
   delivered as one WM_CHAR whose wParam is its UTF-16 code unit. A character beyond the Basic Multilingual Plane becomes a question
   mark. Control characters never come this way (Backspace, Tab, Enter and Escape are made from their key codes by on_key). */
static void inject_text(const char *utf8, uint32_t lparam)
{
    const unsigned char *p = (const unsigned char *) utf8;
    uint32_t c;

    if (p == NULL || main_window == 0) return;
    while (*p)
    {
        p += utf8_next(p, &c);
        if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0) || (c >= 0xd800 && c < 0xe000) || c == 0xfffe || c == 0xffff) continue;
        if (c > 0xffff) c = '?';
        post(main_window, WM_CHAR, c, lparam);
    }
}

EMSCRIPTEN_KEEPALIVE void SRW_InjectText(const char *utf8)
{
    inject_text(utf8, 1);
}

/* A key press from the page (the virtual keyboard): key down, the character when there is one, key up. */
EMSCRIPTEN_KEEPALIVE void SRW_InjectKey(uint32_t vk, uint32_t character)
{
    uint32_t lparam = 1 | ((vk & 0xff) << 16);

    if (main_window == 0) return;
    post(main_window, WM_KEYDOWN, vk, lparam);
    if (character != 0) post(main_window, WM_CHAR, character, lparam);
    post(main_window, WM_KEYUP, vk, lparam | (1u << 30) | (1u << 31));
}

/* The right button pressed or released at the cursor's position, for the page's touch gestures. */
EMSCRIPTEN_KEEPALIVE void SRW_InjectRightButton(int down)
{
    uint32_t message = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;

    if (main_window == 0) return;
    if (down) mouse_held |= 2; else mouse_held &= (uint8_t) ~2;
    SRW_CursorButtons((int) mouse_held);
    post(main_window, WM_SETCURSOR, main_window, 1u | (message << 16));
    post(main_window, message, (down ? 0x02u : 0) | (mouse_held & 1 ? 0x01u : 0) | (key_down[0x10] ? 0x04u : 0) | (key_down[0x11] ? 0x08u : 0),
         ((uint32_t) cursor_y << 16) | ((uint32_t) cursor_x & 0xffff));
}

/* The text box of the program that has the focus, told to the page when it changes (the page opens or closes the virtual keyboard). */
static volatile int input_method_enabled;
static struct { int active, has_rect, left, top, right, bottom; } text_entry_sent;

void SRW_NoteInputMethod(int enabled)
{
    input_method_enabled = enabled;
}

static void update_text_entry(void)
{
    int left = 0, top = 0, right = 0, bottom = 0, active, has_rect;

    has_rect = SRW_TextBox(&left, &top, &right, &bottom);
    if (has_rect < 0)
    {
        active = input_method_enabled;
        has_rect = 0;
    }
    else
    {
        active = has_rect;
        if (!active || right <= left || bottom <= top)
        {
            left = top = right = bottom = 0;
            has_rect = 0;
        }
    }
    if (active == text_entry_sent.active && has_rect == text_entry_sent.has_rect && left == text_entry_sent.left && top == text_entry_sent.top &&
        right == text_entry_sent.right && bottom == text_entry_sent.bottom) return;
    text_entry_sent.active = active;
    text_entry_sent.has_rect = has_rect;
    text_entry_sent.left = left;
    text_entry_sent.top = top;
    text_entry_sent.right = right;
    text_entry_sent.bottom = bottom;
    SRW_LOG(SRW_LOG_INPUT, "Text entry %s%s: %d %d %d %d", active ? "on" : "off", has_rect ? " with a rectangle" : "", left, top, right, bottom);
    if (!SRW_HasPage) return;
    MAIN_THREAD_ASYNC_EM_ASM({ if (Module.srwKeyboard) Module.srwKeyboard.textEntry($0, $1, $2, $3, $4, $5); }, active, has_rect, left, top, right, bottom);
}

/* ------------------------------------------------------------------------------------------------------------------------------------------ */
/* Held keys and buttons. In the page the key up of a key, or the button up of a button, can go missing (another window, a dialog, a
   hidden tab, the browser's own menu). On a loss of the focus every held key and button is released (release_input), and every key and
   mouse event is compared with the modifier and button state it carries (reconcile_*). */
static uint8_t mouse_held;          /* the buttons the program believes down, as the page's `buttons` bits: 1 left, 2 right, 4 middle */
static uint8_t swallow_key_up[256]; /* the key ups of Alt+Enter, which the page kept to itself */

static uint32_t key_up_lparam(uint32_t vk)
{
    int extended = vk == 0x2d || vk == 0x2e || (vk >= 0x21 && vk <= 0x28);

    return 1 | ((vk & 0xff) << 16) | (extended ? 1u << 24 : 0) | (1u << 30) | (1u << 31);
}

static void release_key(uint32_t vk)
{
    key_down[vk] = 0;
    if (vk == 0x10 || vk == 0x11 || vk == 0x12)
    {
        key_down[0xa0 + (vk - 0x10) * 2] = 0;       /* (VK_LSHIFT, VK_LCONTROL, VK_LMENU) */
        key_down[0xa0 + (vk - 0x10) * 2 + 1] = 0;   /* (the right ones) */
    }
    post(main_window, WM_KEYUP, vk, key_up_lparam(vk));
}

static void release_button(unsigned int bit)
{
    static const struct { unsigned int bit; uint32_t message, wparam; } table[3] = {{1, WM_LBUTTONUP, 0}, {2, WM_RBUTTONUP, 0}, {4, WM_MBUTTONUP, 0}};
    unsigned int i;

    for (i = 0; i < 3; i++)
    {
        if (table[i].bit != bit) continue;
        mouse_held &= (uint8_t) ~bit;
        post(main_window, WM_SETCURSOR, main_window, 1u | (table[i].message << 16));
        /* (wParam as Windows has it after the release: MK_LBUTTON 0x01, MK_RBUTTON 0x02, MK_SHIFT 0x04, MK_CONTROL 0x08, MK_MBUTTON 0x10 of what is still down) */
        post(main_window, table[i].message, (mouse_held & 1 ? 0x01u : 0) | (mouse_held & 2 ? 0x02u : 0) | (mouse_held & 4 ? 0x10u : 0) |
             (key_down[0x10] ? 0x04u : 0) | (key_down[0x11] ? 0x08u : 0), ((uint32_t) cursor_y << 16) | ((uint32_t) cursor_x & 0xffff));
    }
}

/* everything held is released, the program is told; tables cleared. Main thread (the page's events). */
static void release_input(const char *why)
{
    unsigned int vk, bit, released = 0;

    pthread_mutex_lock(&clipboard_lock);
    pending_paste_lparam = 0;   /* a Ctrl+V that waits for its text is dropped with the rest */
    pthread_mutex_unlock(&clipboard_lock);
    deferred_key_ups = 0;
    memset(swallow_key_up, 0, sizeof(swallow_key_up));
    for (bit = 1; bit <= 4; bit <<= 1)
        if (mouse_held & bit)
        {
            if (main_window != 0) release_button(bit);
            released++;
        }
    mouse_held = 0;
    SRW_CursorButtons(0);
    for (vk = 1; vk < 256; vk++)
    {
        if (!key_down[vk]) continue;
        if (vk == 0x01 || vk == 0x02 || vk == 0x04) { key_down[vk] = 0; continue; }   /* (the scripted input's buttons) */
        if (vk >= 0xa0 && vk <= 0xa5) { key_down[vk] = 0; continue; }                 /* (the sides of Shift, Ctrl, Alt: their key up is the generic one) */
        if (main_window != 0) release_key(vk);
        else key_down[vk] = 0;
        released++;
    }
    if (released != 0) SRW_LOG(SRW_LOG_INPUT, "Input released (%s): %u keys and buttons", why, released);
}

/* the modifier keys and buttons an event says are down, against what the program believes; the missing releases are posted first.
   `except` is the key (or 0) or the button bit (or 0) that the event itself releases: its own up message comes with it. */
static void reconcile_keys(int shift, int ctrl, int alt, uint32_t except)
{
    static const uint32_t modifiers[3] = {0x10, 0x11, 0x12};
    const int down[3] = {shift, ctrl, alt};
    int i;

    if (main_window == 0) return;
    for (i = 0; i < 3; i++)
        if (!down[i] && key_down[modifiers[i]] && modifiers[i] != except)
        {
            SRW_LOG(SRW_LOG_INPUT, "A modifier key (%x) was held that the browser says is up: released", (unsigned int) modifiers[i]);
            release_key(modifiers[i]);
        }
}

static void reconcile_buttons(unsigned int buttons, unsigned int except_bit)
{
    unsigned int bit;

    if (main_window == 0) return;
    for (bit = 1; bit <= 4; bit <<= 1)
        if ((mouse_held & bit) && !(buttons & bit) && bit != except_bit)
        {
            SRW_LOG(SRW_LOG_INPUT, "A mouse button (%u) was held that the browser says is up: released", bit);
            release_button(bit);
            SRW_CursorButtons((int) (mouse_held & 7));
        }
}

/* called by the page: the program should not believe anything is held (the tab was hidden, and so on) */
EMSCRIPTEN_KEEPALIVE void SRW_ReleaseInput(void)
{
    release_input("the page asked");
}

static uint32_t modifiers_word(const EmscriptenMouseEvent *e)
{
    return (e->ctrlKey ? 0x08 : 0) | (e->shiftKey ? 0x04 : 0);
}

/* The pointer position in the pixels of the game's window (the back buffer), whatever size the page shows the canvas in */
/* The canvas keeps the game's aspect (CSS object-fit: contain): positions are taken within the picture, not the whole element
   (Module.srwCanvasArea in web/prerun.js); a click on the black bars lands on the nearest edge */
EM_JS(void, srw_mouse_position, (int client_x, int client_y, int32_t *out), {
    var area = Module['srwCanvasArea'] ? Module['srwCanvasArea']() : null;
    var x = client_x, y = client_y;

    if (area)
    {
        x = Math.min(Math.max((client_x - area.left) / area.scale, 0), area.width - 1);
        y = Math.min(Math.max((client_y - area.top) / area.scale, 0), area.height - 1);
    }
    HEAP32[out >> 2] = Math.floor(x);
    HEAP32[(out >> 2) + 1] = Math.floor(y);
});

/* how many pixels of the back buffer a pixel of the page is (times 1000) */
EM_JS(void, srw_mouse_ratio, (int32_t *out), {
    var area = Module['srwCanvasArea'] ? Module['srwCanvasArea']() : null;
    var ratio = area ? Math.round(1000 / area.scale) : 1000;

    HEAP32[out >> 2] = ratio;
    HEAP32[(out >> 2) + 1] = ratio;
});

static EM_BOOL on_mouse(int type, const EmscriptenMouseEvent *e, void *user)
{
    uint32_t message = 0;
    uint32_t wparam;
    uint32_t hwnd = main_window;

    (void) user;
    if (hwnd == 0) return 0;

    {
        /* what the event says is down, against what the program believes: a release that went missing (the browser's menu took it) is made up first */
        unsigned int except = 0;

        if (type == EMSCRIPTEN_EVENT_MOUSEUP) except = e->button == 0 ? 1 : e->button == 2 ? 2 : e->button == 1 ? 4 : 0;
        reconcile_buttons(e->buttons & 7, except);
        reconcile_keys(e->shiftKey, e->ctrlKey, e->altKey, 0);
    }
    {
        /* Camera control: once the program hides the pointer or warps it, only the movements count, also before the browser's pointer lock
           takes effect. Fractions of a pixel are carried to the next event. */
        static float carry_x, carry_y;
        static double locked_since;
        static int was_locked;
        int locked = SRW_CursorLocked();

        if (locked && !was_locked) locked_since = emscripten_get_now();
        was_locked = locked;
        if (locked || SRW_CursorRelative())
        {
            int32_t ratio[2];
            float dx, dy;

            /* the first events after the lock comes into effect can carry a bogus large movement (Chrome): dropped */
            if (type == EMSCRIPTEN_EVENT_MOUSEMOVE && locked && emscripten_get_now() - locked_since < 150.0 && (e->movementX > 120 || e->movementX < -120 || e->movementY > 120 || e->movementY < -120))
            {
                return 1;
            }
            srw_mouse_ratio(ratio);
            dx = (float) e->movementX * (float) ratio[0] / 1000.0f + carry_x;
            dy = (float) e->movementY * (float) ratio[1] / 1000.0f + carry_y;
            cursor_x += (int32_t) dx;
            cursor_y += (int32_t) dy;
            carry_x = dx - (float) (int32_t) dx;
            carry_y = dy - (float) (int32_t) dy;
        }
        else
        {
            int32_t position[2];

            carry_x = carry_y = 0;
            srw_mouse_position((int) e->clientX, (int) e->clientY, position);
            cursor_x = position[0];
            cursor_y = position[1];
        }
    }
    if (type != EMSCRIPTEN_EVENT_MOUSEMOVE) SRW_CursorButtons((int) (e->buttons & 7));
    wparam = modifiers_word(e) | ((e->buttons & 1) ? 0x01 : 0) | ((e->buttons & 2) ? 0x02 : 0) | ((e->buttons & 16) ? 0x10 : 0);
    if (type == EMSCRIPTEN_EVENT_MOUSEMOVE)
    {
        message = WM_MOUSEMOVE;
    }
    else
    {
        static const uint32_t down[3] = {WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN};
        static const uint32_t up[3] = {WM_LBUTTONUP, WM_MBUTTONUP, WM_RBUTTONUP};

        if (e->button > 2) return 0;
        message = (type == EMSCRIPTEN_EVENT_MOUSEDOWN) ? down[e->button] : up[e->button];
        {
            uint8_t bit = e->button == 0 ? 1 : e->button == 2 ? 2 : 4;

            if (type == EMSCRIPTEN_EVENT_MOUSEDOWN) mouse_held |= bit; else mouse_held &= (uint8_t) ~bit;
        }
    }
    /* as Windows does before every mouse message: WM_SETCURSOR with HTCLIENT, where the program shows its Direct3D cursor again */
    post(hwnd, WM_SETCURSOR, hwnd, 1u | (message << 16));
    post(hwnd, message, wparam, ((uint32_t) cursor_y << 16) | ((uint32_t) cursor_x & 0xffff));
    return 1;
}

static EM_BOOL on_wheel(int type, const EmscriptenWheelEvent *e, void *user)
{
    int32_t delta = (e->deltaY < 0) ? 120 : (e->deltaY > 0) ? -120 : 0;

    (void) type;
    (void) user;
    if (main_window == 0 || delta == 0) return 0;
    post(main_window, WM_MOUSEWHEEL, ((uint32_t) delta << 16) | modifiers_word(&e->mouse),
         ((uint32_t) cursor_y << 16) | ((uint32_t) cursor_x & 0xffff));
    return 1;
}

static uint32_t virtual_key(const EmscriptenKeyboardEvent *e)
{
    /* the browser's key codes follow the Windows virtual key codes, apart from punctuation */
    switch (e->keyCode)
    {
    case 186: return 0xba;
    case 187: return 0xbb;
    case 188: return 0xbc;
    case 189: return 0xbd;
    case 190: return 0xbe;
    case 191: return 0xbf;
    case 192: return 0xc0;
    case 219: return 0xdb;
    case 220: return 0xdc;
    case 221: return 0xdd;
    case 222: return 0xde;
    case 224: return 0x5b; /* the Meta key */
    default: return e->keyCode & 0xff;
    }
}

/* Alt+Enter is the page's: it switches full screen (web/start.js), the program never sees the combination. The key down is a user gesture, which
   the browser requires for full screen. */
static void toggle_fullscreen(void)
{
    EM_ASM({
        if (typeof Game_ToggleFullscreen === "function") Game_ToggleFullscreen();
    });
}

/* Is the browser's `key` text typed text (one character, or several made by a dead key) and not the name of a key ("Enter", "F5", "Dead")?
   A name is made of ASCII letters and digits only; a single character is text whatever it is. */
static int key_is_text(const unsigned char *key)
{
    uint32_t c;
    int used;

    if (key[0] == 0) return 0;
    used = utf8_next(key, &c);
    if (key[used] == 0) return 1;
    for (; *key; key++)
        if (*key >= 0x80 || !((*key >= '0' && *key <= '9') || (*key >= 'A' && *key <= 'Z') || (*key >= 'a' && *key <= 'z'))) return 1;
    return 0;
}

static EM_BOOL on_key(int type, const EmscriptenKeyboardEvent *e, void *user)
{
    uint32_t vk = virtual_key(e), lparam, message;
    int extended = (e->location == 3) || vk == 0x2d || vk == 0x2e || (vk >= 0x21 && vk <= 0x28);

    (void) user;
    if (main_window == 0) return 0;
    if (MAIN_THREAD_EM_ASM_INT({ return Module.srwKeyboard && Module.srwKeyboard.owns() ? 1 : 0; })) return 0;

    lparam = 1 | ((vk & 0xff) << 16) | (extended ? 1u << 24 : 0);
    if (type == EMSCRIPTEN_EVENT_KEYDOWN)
    {
        /* Alt+Enter: full screen, of the page; the program sees neither the keys nor their characters, and the Alt it saw go down goes up */
        if (vk == 0x0d && e->altKey && !e->ctrlKey && !e->metaKey)
        {
            if (!e->repeat)
            {
                if (key_down[0x12]) release_key(0x12);
                swallow_key_up[0x0d] = swallow_key_up[0x12] = 1;
                toggle_fullscreen();
            }
            return 1;
        }
        if (vk == 0x12) swallow_key_up[0x12] = 0;
        if (vk == 0x0d) swallow_key_up[0x0d] = 0;
        reconcile_keys(e->shiftKey, e->ctrlKey, e->altKey, 0);
        /* a key that is already down and not repeating: its key up was lost */
        if (!e->repeat && vk != 0 && key_down[vk] && vk != 0x10 && vk != 0x11 && vk != 0x12)
        {
            SRW_LOG(SRW_LOG_INPUT, "The key %x went down again without a key up: released first", (unsigned int) vk);
            release_key(vk);
        }
        if (e->repeat) lparam |= 1u << 30;
        key_down[vk] = 1;
        if (vk == 0x10 || vk == 0x11 || vk == 0x12) key_down[0xa0 + (vk - 0x10) * 2 + (e->location == 2 ? 1 : 0)] = 1;
        message = e->altKey ? WM_SYSKEYDOWN : WM_KEYDOWN;
    }
    else if (type == EMSCRIPTEN_EVENT_KEYUP)
    {
        if (swallow_key_up[vk])
        {
            /* (the end of Alt+Enter) */
            swallow_key_up[vk] = 0;
            return vk != 0x7b;
        }
        reconcile_keys(e->shiftKey, e->ctrlKey, e->altKey, vk);
        if (pending_paste_lparam != 0 && deferred_key_ups < 8)
        {
            /* a Ctrl+V waits for its text: its key ups come after it (the game checks Ctrl while it handles the V) */
            deferred_key_up[deferred_key_ups].vk = vk;
            deferred_key_up[deferred_key_ups].message = e->altKey ? WM_SYSKEYUP : WM_KEYUP;
            deferred_key_up[deferred_key_ups].lparam = lparam | (1u << 30) | (1u << 31);
            deferred_key_up[deferred_key_ups].side = e->location == 2;
            deferred_key_ups++;
            return 0;
        }
        key_down[vk] = 0;
        if (vk == 0x10 || vk == 0x11 || vk == 0x12) key_down[0xa0 + (vk - 0x10) * 2 + (e->location == 2 ? 1 : 0)] = 0;
        lparam |= (1u << 30) | (1u << 31);
        message = e->altKey ? WM_SYSKEYUP : WM_KEYUP;
    }
    else
    {
        /* characters are made from the keydown events */
        return 0;
    }
    if (type == EMSCRIPTEN_EVENT_KEYDOWN && vk == 0x56 && e->ctrlKey && !e->altKey && !e->repeat)
    {
        /* Ctrl+V waits for the page's paste event, which brings the text (SRW_ClipboardPasted); 300 ms later the asynchronous API is asked */
        pthread_mutex_lock(&clipboard_lock);
        pending_paste_lparam = lparam;
        pthread_mutex_unlock(&clipboard_lock);
        emscripten_set_timeout(paste_fallback, 300, NULL);
        return 0;   /* the browser's default action is what fires the paste event */
    }
    post(main_window, message, vk, lparam);

    /* What TranslateMessage does on Windows: a key that types something also gives WM_CHAR. It is made from the keydown, as keypress
       events are not sent for a consumed keydown. A dead key has the key "Dead", which is not text: the composed letter comes in the
       `key` of the keydown that follows. */
    if (type == EMSCRIPTEN_EVENT_KEYDOWN && !e->metaKey && ((!e->ctrlKey && !e->altKey) || (e->ctrlKey && e->altKey)))
    {
        const unsigned char *key = (const unsigned char *) e->key;

        if (key_is_text(key)) inject_text(e->key, lparam & ~(1u << 31));
        else if (vk == 0x08 || vk == 0x09 || vk == 0x0d || vk == 0x1b) post(main_window, WM_CHAR, vk, lparam & ~(1u << 31));
    }

    /* keep the browser from acting on keys the game uses (scrolling, tab focus, F-keys) */
    return vk != 0x7b && !(e->ctrlKey && (vk == 0x52 || vk == 0x43 || vk == 0x58 || vk == 0x56));   /* F12, Ctrl+R, and the clipboard keys reach the browser */
}

/* The tab becomes hidden: nothing stays held (a key up or a button up does not reach a hidden page) */
static EM_BOOL on_visibility(int type, const EmscriptenVisibilityChangeEvent *e, void *user)
{
    (void) type;
    (void) user;
    if (e->hidden) release_input("the tab was hidden");
    return 0;
}

/* The pointer lock was lost while a button was held (Escape, the browser's own reasons): the release of the button may not come to the page */
static EM_BOOL on_pointer_lock(int type, const EmscriptenPointerlockChangeEvent *e, void *user)
{
    (void) type;
    (void) user;
    if (!e->isActive && mouse_held != 0) release_input("the pointer lock was lost with a button held");
    return 0;
}

/* The browser's focus changes release the keys; whether the program is in the background is decided by the page (SRW_SetActive). */
static EM_BOOL on_focus(int type, const EmscriptenFocusEvent *e, void *user)
{
    static int reports;

    (void) e;
    (void) user;
    if (main_window != 0)
    {
        int gained = (type == EMSCRIPTEN_EVENT_FOCUSIN || type == EMSCRIPTEN_EVENT_FOCUS);

        if (!gained) release_input("the window lost the focus");
        if (SRW_LOG_ON(SRW_LOG_INPUT) && reports++ < 20) logprintf("The page %s the focus", gained ? "got" : "lost");
    }
    return 0;
}

/* registers the callbacks on the main thread; called once, when the program's main window is created */
static void install_input(void)
{
    static int installed;
    const char *target = "#canvas";

    if (installed) return;
    installed = 1;
    /* there is no page to take input from when run under Node */
    SRW_HasPage = 0;
    if (!MAIN_THREAD_EM_ASM_INT({ return (typeof document !== "undefined" && typeof window !== "undefined") ? 1 : 0; })) return;
    SRW_HasPage = 1;
    emscripten_set_mousemove_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_mouse, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
    emscripten_set_mousedown_callback_on_thread(target, NULL, 1, on_mouse, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
    emscripten_set_mouseup_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_mouse, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
    emscripten_set_wheel_callback_on_thread(target, NULL, 1, on_wheel, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
    emscripten_set_keydown_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_key, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
    emscripten_set_keyup_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_key, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
    emscripten_set_keypress_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_key, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
    emscripten_set_blur_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_focus, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
    emscripten_set_focus_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, 1, on_focus, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
    emscripten_set_visibilitychange_callback_on_thread(NULL, 1, on_visibility, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
    emscripten_set_pointerlockchange_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, 1, on_pointer_lock, EM_CALLBACK_THREAD_CONTEXT_MAIN_BROWSER_THREAD);
}

void SRW_InstallInput(void)
{
    install_input();
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the display */

uint32_t CCALL GetSystemMetrics_c(uint32_t nIndex)
{
    query_screen_size();
    switch (nIndex)
    {
    case 0: case 16: case 78: return (uint32_t) screen_width; /* SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN */
    case 1: case 17: case 79: return (uint32_t) screen_height;
    case 2: case 3: return 17;
    case 4: case 15: return 19;
    case 5: case 6: return 1;
    case 7: case 8: case 32: case 33: return 3;
    case 11: case 12: case 13: case 14: return 32;
    case 19: return 1;  /* SM_MOUSEPRESENT */
    case 34: case 35: return 100;
    case 36: case 37: return 4;
    case 43: return 3;
    case 45: case 46: return 2;
    case 80: return 1;  /* SM_CMONITORS */
    default: return 0;
    }
}

uint32_t CCALL SystemParametersInfoA_c(uint32_t uiAction, uint32_t uiParam, void *pvParam, uint32_t fWinIni)
{
    (void) uiParam;
    (void) fWinIni;
    if (uiAction == 0x30 && pvParam != NULL) /* SPI_GETWORKAREA */
    {
        Rect *rect = pvParam;

        query_screen_size();
        rect->left = 0;
        rect->top = 0;
        rect->right = screen_width;
        rect->bottom = screen_height;
    }
    return 1;
}

static void fill_devmode(uint8_t *mode, int width, int height, int bits, int frequency)
{
    memset(mode, 0, 156);
    strcpy((char *) mode, "WebGL Display");
    *(uint16_t *) (mode + 36) = 156;                /* dmSize */
    *(uint32_t *) (mode + 40) = 0x5c0000;           /* dmFields: bits, width, height, frequency */
    *(uint32_t *) (mode + 104) = (uint32_t) bits;   /* dmBitsPerPel */
    *(uint32_t *) (mode + 108) = (uint32_t) width;
    *(uint32_t *) (mode + 112) = (uint32_t) height;
    *(uint32_t *) (mode + 120) = (uint32_t) frequency;
}

/* The display modes: common sizes, then the canvas (the "desktop", also the current mode) and the screen when they are not among them
   (the program's list of resolutions comes from here: the canvas' size has to be one of them) */
uint32_t CCALL EnumDisplaySettingsA_c(const char *lpszDeviceName, uint32_t iModeNum, void *lpDevMode)
{
    static const int sizes[][2] = {{800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1366, 768}, {1600, 900}, {1920, 1080}};
    int modes[9][2], count = 0, extra[2][2], i, j;

    (void) lpszDeviceName;
    query_screen_size();
    if (iModeNum == 0xffffffffu || iModeNum == 0xfffffffeu) /* ENUM_CURRENT_SETTINGS, ENUM_REGISTRY_SETTINGS */
    {
        fill_devmode(lpDevMode, screen_width, screen_height, 32, 60);
        return 1;
    }
    for (i = 0; i < (int) (sizeof(sizes) / sizeof(sizes[0])); i++)
    {
        modes[count][0] = sizes[i][0];
        modes[count++][1] = sizes[i][1];
    }
    extra[0][0] = screen_width;
    extra[0][1] = screen_height;
    extra[1][0] = display_screen_width > 0 ? display_screen_width : screen_width;
    extra[1][1] = display_screen_height > 0 ? display_screen_height : screen_height;
    for (i = 0; i < 2; i++)
    {
        for (j = 0; j < count && (modes[j][0] != extra[i][0] || modes[j][1] != extra[i][1]); j++)
            ;
        if (j == count)
        {
            modes[count][0] = extra[i][0];
            modes[count++][1] = extra[i][1];
        }
    }
    if (iModeNum >= (uint32_t) count) return 0;
    fill_devmode(lpDevMode, modes[iModeNum][0], modes[iModeNum][1], 32, 60);
    return 1;
}

uint32_t CCALL ChangeDisplaySettingsExA_c(const char *lpszDeviceName, void *lpDevMode, uint32_t hwnd, uint32_t dwflags, void *lParam)
{
    (void) lpszDeviceName;
    (void) lpDevMode;
    (void) hwnd;
    (void) dwflags;
    (void) lParam;
    return 0; /* DISP_CHANGE_SUCCESSFUL: the page area is what it is */
}

uint32_t CCALL EnumDisplayDevicesA_c(const char *lpDevice, uint32_t iDevNum, void *lpDisplayDevice, uint32_t dwFlags)
{
    uint8_t *device = lpDisplayDevice;

    (void) lpDevice;
    (void) dwFlags;
    if (iDevNum != 0) return 0;
    memset(device + 4, 0, 420);
    strcpy((char *) device + 4, "\\\\.\\DISPLAY1");
    strcpy((char *) device + 36, "WebGL Display");
    *(uint32_t *) (device + 164) = 5; /* attached, primary */
    return 1;
}

uint32_t CCALL MonitorFromRect_c(void *lprc, uint32_t dwFlags)
{
    (void) lprc;
    (void) dwFlags;
    return 0x90001;
}

uint32_t CCALL GetMonitorInfoA_c(uint32_t hMonitor, void *lpmi)
{
    uint32_t *p = lpmi;

    (void) hMonitor;
    query_screen_size();
    p[1] = 0;
    p[2] = 0;
    p[3] = (uint32_t) screen_width;
    p[4] = (uint32_t) screen_height;
    p[5] = 0;
    p[6] = 0;
    p[7] = (uint32_t) screen_width;
    p[8] = (uint32_t) screen_height;
    p[9] = 1; /* MONITORINFOF_PRIMARY */
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* wsprintfA: the program passes its arguments on the stack (cdecl), args points at the first, the buffer */

uint32_t CCALL wsprintfA_c(const uint32_t *args)
{
    char *out = (char *) (uintptr_t) args[0];
    const char *format = (const char *) (uintptr_t) args[1];
    const uint32_t *next = args + 2;
    size_t length = 0;

    while (*format != 0 && length < 1023)
    {
        char spec[32], piece[1100];
        int n = 0;

        if (*format != '%')
        {
            out[length++] = *format++;
            continue;
        }
        spec[n++] = *format++;
        if (*format == '%')
        {
            out[length++] = '%';
            format++;
            continue;
        }
        /* flags, width, precision */
        while (*format != 0 && strchr("-+ #0", *format) != NULL && n < 20) spec[n++] = *format++;
        if (*format == '*')
        {
            n += snprintf(spec + n, sizeof(spec) - (size_t) n, "%d", (int) *next++);
            format++;
        }
        while (isdigit((unsigned char) *format) && n < 24) spec[n++] = *format++;
        if (*format == '.')
        {
            spec[n++] = *format++;
            if (*format == '*')
            {
                n += snprintf(spec + n, sizeof(spec) - (size_t) n, "%d", (int) *next++);
                format++;
            }
            while (isdigit((unsigned char) *format) && n < 28) spec[n++] = *format++;
        }
        while (*format == 'l' || *format == 'h') format++;
        if (*format == 0) break;
        switch (*format)
        {
        case 'd': case 'i':
            spec[n++] = 'd';
            spec[n] = 0;
            snprintf(piece, sizeof(piece), spec, (int) *next++);
            break;
        case 'u': case 'x': case 'X': case 'o':
            spec[n++] = *format;
            spec[n] = 0;
            snprintf(piece, sizeof(piece), spec, (unsigned int) *next++);
            break;
        case 'c':
            spec[n++] = 'c';
            spec[n] = 0;
            snprintf(piece, sizeof(piece), spec, (int) (*next++ & 0xff));
            break;
        case 's':
        {
            const char *text = (const char *) (uintptr_t) *next++;

            spec[n++] = 's';
            spec[n] = 0;
            snprintf(piece, sizeof(piece), spec, text != NULL ? text : "(null)");
            break;
        }
        default:
            piece[0] = 0;
            break;
        }
        format++;
        {
            size_t piece_length = strlen(piece);

            if (length + piece_length > 1023) piece_length = 1023 - length;
            memcpy(out + length, piece, piece_length);
            length += piece_length;
        }
    }
    out[length] = 0;
    return (uint32_t) length;
}

/* SRW_MainWindow: used by the display layer to find where to draw */
uint32_t SRW_MainWindow(void)
{
    return main_window;
}

uint32_t SRW_ScreenWidth(void)
{
    return (uint32_t) screen_width;
}

uint32_t SRW_ScreenHeight(void)
{
    return (uint32_t) screen_height;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* window words and properties, subclassing */

typedef struct {
    uint32_t window;
    char name[48];
    uint32_t value;
} Property;

static Property properties[64];

uint32_t CCALL GetWindowLongA_c(uint32_t hWnd, uint32_t nIndex)
{
    Window *window = window_of(hWnd);
    int32_t index = (int32_t) nIndex;

    if (window == NULL) return 0;
    switch (index)
    {
    case -4: return window->wndproc != 0 ? window->wndproc : window->class->wndproc;
    case -16: return window->style;
    case -20: return window->exstyle;
    case -21: return window->userdata;
    case -6: return 0x400000; /* GWL_HINSTANCE */
    case -8: return window->parent;
    default:
        return (index >= 0 && index < 32) ? window->extra_long[index / 4] : 0;
    }
}

uint32_t CCALL SetWindowLongA_c(uint32_t hWnd, uint32_t nIndex, uint32_t dwNewLong)
{
    Window *window = window_of(hWnd);
    int32_t index = (int32_t) nIndex;
    uint32_t old;

    if (window == NULL) return 0;
    old = GetWindowLongA_c(hWnd, nIndex);
    switch (index)
    {
    case -4: window->wndproc = dwNewLong; break;
    case -16: window->style = dwNewLong; break;
    case -20: window->exstyle = dwNewLong; break;
    case -21: window->userdata = dwNewLong; break;
    default:
        if (index >= 0 && index < 32) window->extra_long[index / 4] = dwNewLong;
        break;
    }
    return old;
}

uint32_t CCALL CallWindowProcA_c(uint32_t lpPrevWndFunc, uint32_t hWnd, uint32_t Msg_, uint32_t wParam, uint32_t lParam)
{
    uint32_t args[4];

    args[0] = hWnd;
    args[1] = Msg_;
    args[2] = wParam;
    args[3] = lParam;
    return SRW_CallGuest(lpPrevWndFunc, 4, args);
}

uint32_t CCALL SetPropA_c(uint32_t hWnd, const char *lpString, uint32_t hData)
{
    int i, free_slot = -1;

    if ((uintptr_t) lpString < 0x10000) return 0;
    for (i = 0; i < 64; i++)
    {
        if (properties[i].window == hWnd && strcmp(properties[i].name, lpString) == 0)
        {
            properties[i].value = hData;
            return 1;
        }
        if (properties[i].window == 0 && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return 0;
    properties[free_slot].window = hWnd;
    snprintf(properties[free_slot].name, sizeof(properties[free_slot].name), "%s", lpString);
    properties[free_slot].value = hData;
    return 1;
}

uint32_t CCALL GetPropA_c(uint32_t hWnd, const char *lpString)
{
    int i;

    if ((uintptr_t) lpString < 0x10000) return 0;
    for (i = 0; i < 64; i++)
    {
        if (properties[i].window == hWnd && strcmp(properties[i].name, lpString) == 0) return properties[i].value;
    }
    return 0;
}

uint32_t CCALL RemovePropA_c(uint32_t hWnd, const char *lpString)
{
    int i;

    if ((uintptr_t) lpString < 0x10000) return 0;
    for (i = 0; i < 64; i++)
    {
        if (properties[i].window == hWnd && strcmp(properties[i].name, lpString) == 0)
        {
            uint32_t value = properties[i].value;

            memset(&properties[i], 0, sizeof(properties[i]));
            return value;
        }
    }
    return 0;
}

/* The clipboard as the program takes it. CF_TEXT is the text in the code page of the program's ANSI functions, UTF-8 here (GetACP);
   CF_UNICODETEXT is UTF-16 with a carriage return before every line feed. The text is converted in two passes (count, then convert),
   because the number of UTF-16 units is not bounded by the number of bytes. */
static size_t utf16_units(const unsigned char *p, size_t *chars_out)
{
    size_t units = 0, chars = 0;
    uint32_t c, previous = 0;

    while (*p)
    {
        p += utf8_next(p, &c);
        if (c == '\n' && previous != '\r') units++;
        units += c > 0xffff ? 2 : 1;
        previous = c;
        chars++;
    }
    if (chars_out != NULL) *chars_out = chars;
    return units;
}

#ifdef SRW_DIAGNOSTICS
uint32_t CCALL GetClipboardData_c(uint32_t uFormat);

static void autoinput_clipboard(const char *text, int set)
{
    char *copy = NULL;

    if (set == 2)   /* the clipboard as CF_UNICODETEXT: the number of UTF-16 units and the last ones */
    {
        const uint16_t *units = (const uint16_t *) (uintptr_t) GetClipboardData_c(13);
        size_t n = 0;

        while (units != NULL && units[n]) n++;
        logprintf("Clipboard as UTF-16: %d units, the last four: %04x %04x %04x %04x", (int) n, n > 3 ? units[n - 4] : 0, n > 2 ? units[n - 3] : 0, n > 1 ? units[n - 2] : 0, n > 0 ? units[n - 1] : 0);
        return;
    }

    pthread_mutex_lock(&clipboard_lock);
    if (set)
    {
        size_t n = 0;

        copy = malloc(strlen(text) + 1);
        for (; *text; text++)
        {
            if (text[0] == '\\' && text[1] == 'n') { copy[n++] = '\n'; text++; }
            else copy[n++] = *text;
        }
        copy[n] = 0;
        free(clipboard_text);
        clipboard_text = copy;
    }
    else if (clipboard_text != NULL)
    {
        const unsigned char *p = (const unsigned char *) clipboard_text;
        char hex[1200];
        size_t n = 0;

        for (; *p && n + 3 < sizeof(hex); p++) n += (size_t) snprintf(hex + n, sizeof(hex) - n, "%02x", *p);
        logprintf("Clipboard: %d bytes: %s", (int) strlen(clipboard_text), hex);
    }
    else logprintf("Clipboard: empty");
    pthread_mutex_unlock(&clipboard_lock);
}
#endif

uint32_t CCALL GetClipboardData_c(uint32_t uFormat)
{
    void *result = NULL;

    SRW_LOG(SRW_LOG_INPUT, "GetClipboardData(%u), the clipboard holds %d bytes", (unsigned int) uFormat, clipboard_text != NULL ? (int) strlen(clipboard_text) : -1);
    if (uFormat != 1 && uFormat != 13 && uFormat != 7) return 0;   /* CF_TEXT, CF_UNICODETEXT, CF_OEMTEXT */
    pthread_mutex_lock(&clipboard_lock);
    if (clipboard_text != NULL)
    {
        const unsigned char *p = (const unsigned char *) clipboard_text;

        if (uFormat == 13)
        {
            size_t units = utf16_units(p, NULL), n = 0;
            uint16_t *out = malloc((units + 1) * 2);

            while (out != NULL && *p)
            {
                uint32_t c;

                p += utf8_next(p, &c);
                if (c == '\n' && n > 0 && out[n - 1] != '\r') out[n++] = '\r';
                if (c > 0xffff)
                {
                    out[n++] = (uint16_t) (0xd800 | ((c - 0x10000) >> 10));
                    out[n++] = (uint16_t) (0xdc00 | ((c - 0x10000) & 0x3ff));
                }
                else out[n++] = (uint16_t) c;
            }
            if (out != NULL) out[n] = 0;
            result = out;
        }
        else
        {
            /* UTF-8 as it is, with a carriage return before every line feed as Windows has it */
            size_t length = strlen(clipboard_text), n = 0, i;
            unsigned char *out = malloc(length * 2 + 1);

            for (i = 0; out != NULL && i < length; i++)
            {
                if (p[i] == '\n' && (i == 0 || p[i - 1] != '\r')) out[n++] = '\r';
                out[n++] = p[i];
            }
            if (out != NULL) out[n] = 0;
            result = out;
        }
    }
    free(clipboard_handle);
    clipboard_handle = result;
    pthread_mutex_unlock(&clipboard_lock);
    return (uint32_t) (uintptr_t) result;
}
