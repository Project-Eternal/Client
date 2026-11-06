/* The mouse pointer of the program in the page. The program asks for its pointer in three ways: the cursor of the Direct3D device (an image
   and a visible/hidden state: what a game uses in its world and in its menus), the Win32 calls (SetCursor, ShowCursor) and, while it moves the
   camera with the mouse, by hiding the pointer and putting it back to the middle of the window every frame (SetCursorPos). Here they become
   the CSS cursor of the canvas and, for the camera, the pointer lock of the browser (Module.srwCursor in web/runtime.js). */
#include <emscripten.h>
#include <emscripten/html5.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "srw_debuglog.h"
#include "srw.h"

extern volatile int SRW_HasPage;   /* user32.c */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int d3d_seen, d3d_shown, have_image;   /* the cursor of the Direct3D device */
static int win_count;                          /* ShowCursor: the pointer is shown when this is not negative */
static uint32_t win_cursor_id = 32512;         /* the system cursor of the last SetCursor (IDC_ARROW), 0 for none */
static int buttons;                            /* mouse buttons that are down (bit mask of the page's events) */
static uint64_t last_warp_ms;                  /* time of the last SetCursorPos */
static int applied_kind = -1, applied_id = -1, applied_lock = -1;
static int reports;

static uint64_t now_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000u + (uint64_t) now.tv_nsec / 1000000u;
}

static void note(const char *what, int a, int b)
{
    if (SRW_LOG_ON(SRW_LOG_INPUT) && reports < 80)
    {
        reports++;
        logprintf("Cursor: %s %d %d", what, a, b);
    }
}

/* works out what the page should show; called after every change of the state */
static void update(void)
{
    int kind, id = 0, want_lock, hidden;

    pthread_mutex_lock(&lock);
    hidden = win_count < 0 || (d3d_seen && !d3d_shown);
    if (d3d_shown && have_image) kind = 1;              /* the image of the game */
    else if (hidden || win_cursor_id == 0) kind = 2;    /* no pointer */
    else if (d3d_shown) kind = 2;                       /* the program has a pointer but no image (yet): none rather than the system's */
    else { kind = 3; id = (int) win_cursor_id; }        /* a system cursor */
    /* moving the camera with the mouse: a button is down and the program hides the pointer or keeps putting it back */
    want_lock = buttons != 0 && (hidden || now_ms() - last_warp_ms < 300);
    if (kind == applied_kind && id == applied_id && want_lock == applied_lock)
    {
        pthread_mutex_unlock(&lock);
        return;
    }
    if (want_lock != applied_lock) note(want_lock ? "the mouse is locked, hidden:" : "the mouse is released, hidden:", hidden, buttons);
    {
        int change_cursor = kind != applied_kind || id != applied_id, change_lock = want_lock != applied_lock;

        applied_kind = kind;
        applied_id = id;
        applied_lock = want_lock;
        pthread_mutex_unlock(&lock);
        if (!SRW_HasPage) return;   /* (Node: scripted input, no page) */
        if (change_cursor) MAIN_THREAD_EM_ASM({ Module['srwCursor'].apply($0, $1); }, kind, id);
        if (change_lock) MAIN_THREAD_EM_ASM({ Module['srwCursor'].lock($0); }, want_lock);
    }
}

/* the bitmap of the cursor of the Direct3D device: rgba, width * height pixels, straight alpha */
void SRW_CursorImage(const uint32_t *rgba, int width, int height, int hot_x, int hot_y)
{
    int i, uniform = 1, visible = 0;

    /* A texture the program has not filled yet (at the start: a square of one colour, nothing in it) is not a cursor. Once a real pointer is set
       only an image without a single visible pixel is dropped: an icon of one colour that is dragged is a pointer too. */
    for (i = 0; i < width * height; i++)
    {
        if ((rgba[i] >> 24) != 0) visible = 1;
        if (i > 0 && rgba[i] != rgba[0]) uniform = 0;
    }
    pthread_mutex_lock(&lock);
    if (uniform && have_image && visible) uniform = 0;
    pthread_mutex_unlock(&lock);
    if (uniform || !visible)
    {
        note("an empty image for the Direct3D cursor (ignored), size", width, height);
        return;
    }
    note("image of the Direct3D cursor, size", width, height);
    MAIN_THREAD_EM_ASM({ Module['srwCursor'].setImage($0, $1, $2, $3, $4); }, rgba, width, height, hot_x, hot_y);
    pthread_mutex_lock(&lock);
    have_image = 1;
    applied_kind = -1;   /* apply the new image */
    pthread_mutex_unlock(&lock);
    update();
}

int SRW_CursorD3DShow(int show)
{
    int before;

    pthread_mutex_lock(&lock);
    before = d3d_shown;
    d3d_seen = 1;
    d3d_shown = show != 0;
    pthread_mutex_unlock(&lock);
    if (before != (show != 0)) note("Direct3D ShowCursor", show, 0);
    update();
    return before;
}

/* ShowCursor of Windows: the count goes up and down, the pointer is shown when it is not negative */
int SRW_CursorWin32Show(int show)
{
    int count;

    pthread_mutex_lock(&lock);
    win_count += show ? 1 : -1;
    count = win_count;
    pthread_mutex_unlock(&lock);
    note("ShowCursor count", count, show);
    update();
    return count;
}

/* SetCursor of Windows with the resource number of a system cursor (IDC_*) or 0 for none */
void SRW_CursorWin32Set(uint32_t system_id)
{
    pthread_mutex_lock(&lock);
    win_cursor_id = system_id;
    pthread_mutex_unlock(&lock);
    note("SetCursor", (int) system_id, 0);
    update();
}

/* the program put the pointer somewhere: while a button is down that is the camera control */
void SRW_CursorWarped(void)
{
    pthread_mutex_lock(&lock);
    last_warp_ms = now_ms();
    pthread_mutex_unlock(&lock);
    update();
}

void SRW_CursorButtons(int down)
{
    pthread_mutex_lock(&lock);
    buttons = down;
    pthread_mutex_unlock(&lock);
    update();
}

/* the camera control is wanted (a button is down and the program hides or keeps warping the pointer): the mouse events must only move the
   program's cursor from where the program put it, whether the browser's pointer lock has come into effect yet or not */
int SRW_CursorRelative(void)
{
    int relative;

    pthread_mutex_lock(&lock);
    relative = applied_lock == 1;
    pthread_mutex_unlock(&lock);
    return relative;
}

/* on the main thread: is the pointer locked to the canvas (the mouse events then carry movements only) */
int SRW_CursorLocked(void)
{
    EmscriptenPointerlockChangeEvent status;

    if (!SRW_HasPage) return 0;
    if (emscripten_get_pointerlock_status(&status) != EMSCRIPTEN_RESULT_SUCCESS) return 0;
    return status.isActive;
}
