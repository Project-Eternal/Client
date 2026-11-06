/* The game's side of mod-wowcontroller (server/acore/modules/mod-wowcontroller): the embedded AzerothCore server asks the game for things
   through the page (web/acore.js -> Module.wowGame in web/prerun.js -> here):
     kind 1  a file of the game, as the game itself sees it: Storm's SFileOpenFile (0x424f80: loose files first, then the archives in
             their order), SFileGetFileSize (0x4218c0), SFileReadFile (0x422530), SFileCloseFile (0x422910), all stdcall
     kind 2  one of the port's chat commands (WoW-portcmd.c: ".wow gfx flat" on the server becomes "gfx flat" here); the answer lines
     kind 3  questions about the game's world (WoW-world.c: an array of WoWWorldQuery, answered in place on the game thread)
   Kind 3 has a thread of its own (world_main: the questions must not wait behind a long file read). A thread serves the other requests one after the other (Storm is guest code: the thread is set up as a guest thread, as the
   client's own threads are); it waits until the client has opened its archives. The page posts a request (WoW_ControllerPost) and gets it
   back through Module.wowGame.done when it is answered; WoW_ControllerFree releases it. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <emscripten.h>

#include "srw_debuglog.h"
#include "WoW-world.h"
#include "WoW.h"

#define STORM_OPEN_FILE 0x424f80u
#define STORM_FILE_SIZE 0x4218c0u
#define STORM_READ_FILE 0x422530u
#define STORM_CLOSE_FILE 0x422910u

extern volatile int WoW_StormReady;   /* WoW-prefetch.c */
int WoW_PortCommand(const char *command, int count, char **arguments, void (*reply)(void *context, const char *text), void *context);

typedef struct ControllerRequest {
    volatile int32_t state;   /* 0 waiting, 1 answered, -1 failed (no such file, unknown command) */
    int32_t kind;
    char *text;               /* the file's name or the command line; kind 3: the queries (text_size bytes) */
    uint8_t *data;            /* the answer (malloc) */
    uint32_t size;
    struct ControllerRequest *next;
    uint32_t text_size;
} ControllerRequest;

typedef struct {
    ControllerRequest *head, *tail;
    int started;
} Queue;

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_changed = PTHREAD_COND_INITIALIZER;
static Queue service_queue, world_queue;
static uint64_t files_served, bytes_served;

static int read_game_file(ControllerRequest *r)
{
    uint32_t handle = 0, args[6], size, got = 0;

    args[0] = (uint32_t) (uintptr_t) r->text;
    args[1] = (uint32_t) (uintptr_t) &handle;
    if (!SRW_CallGuest(STORM_OPEN_FILE, 2, args) || handle == 0) return 0;
    args[0] = handle;
    args[1] = 0;
    size = SRW_CallGuest(STORM_FILE_SIZE, 2, args);
    r->data = malloc(size ? size : 1);
    if (r->data != NULL && size != 0xffffffffu)
    {
        args[0] = handle; args[1] = (uint32_t) (uintptr_t) r->data; args[2] = size; args[3] = (uint32_t) (uintptr_t) &got; args[4] = 0; args[5] = 0;
        if (!SRW_CallGuest(STORM_READ_FILE, 6, args) && got != size) got = 0xffffffffu;
    }
    args[0] = handle;
    SRW_CallGuest(STORM_CLOSE_FILE, 1, args);
    if (r->data == NULL || got != size)
    {
        free(r->data);
        r->data = NULL;
        return 0;
    }
    r->size = size;
    files_served++;
    bytes_served += size;
    return 1;
}

typedef struct { char *text; size_t length; } Answer;

static void collect_reply(void *context, const char *line)
{
    Answer *a = context;
    size_t n = strlen(line);
    char *grown = realloc(a->text, a->length + n + 2);

    if (grown == NULL) return;
    a->text = grown;
    memcpy(a->text + a->length, line, n);
    a->length += n;
    a->text[a->length++] = '\n';
    a->text[a->length] = 0;
}

static int run_command(ControllerRequest *r)
{
    char copy[512], *args[16], *token, *save;
    int count = 0, handled;
    Answer answer = {NULL, 0};

    snprintf(copy, sizeof(copy), "%s", r->text);
    for (token = strtok_r(copy, " ", &save); token != NULL && count < 16; token = strtok_r(NULL, " ", &save)) args[count++] = token;
    if (count == 0) return 0;
    handled = WoW_PortCommand(args[0], count - 1, args + 1, collect_reply, &answer);
    if (!handled)
    {
        free(answer.text);
        return 0;
    }
    r->data = (uint8_t *) answer.text;
    r->size = (uint32_t) answer.length;
    return 1;
}

static ControllerRequest *take(Queue *q)
{
    ControllerRequest *r;

    pthread_mutex_lock(&queue_lock);
    while (q->head == NULL) pthread_cond_wait(&queue_changed, &queue_lock);
    r = q->head;
    q->head = r->next;
    if (q->head == NULL) q->tail = NULL;
    pthread_mutex_unlock(&queue_lock);
    return r;
}

/* the world questions (kind 3): a guest thread of its own, the questions run on it while the game thread sleeps (WoW-world.c) */
static void *world_main(void *unused)
{
    (void) unused;
    x86_initialize_cpu();
    for (;;)
    {
        ControllerRequest *r = take(&world_queue);

        WoW_WorldQueries((WoWWorldQuery *) r->text, r->text_size / sizeof(WoWWorldQuery));
        r->data = (uint8_t *) r->text;
        r->size = r->text_size;
        r->text = NULL;
        __atomic_store_n(&r->state, 1, __ATOMIC_SEQ_CST);
        MAIN_THREAD_ASYNC_EM_ASM({ Module['wowGame'].done($0); }, r);
    }
    return NULL;
}

static void *service_main(void *unused)
{
    (void) unused;
    x86_initialize_cpu();
    while (!WoW_StormReady) usleep(50000);
    logprintf("Game controller: ready (the archives are open)");
    for (;;)
    {
        ControllerRequest *r = take(&service_queue);
        int ok;

        SRW_GilAcquire();
        ok = r->kind == 1 ? read_game_file(r) : r->kind == 2 ? run_command(r) : 0;
        SRW_GilRelease();
        SRW_LOG(SRW_LOG_FILES, "Game controller: %s %s: %s (%u bytes)", r->kind == 1 ? "file" : "command", r->text, ok ? "answered" : "failed", (unsigned int) r->size);
        __atomic_store_n(&r->state, ok ? 1 : -1, __ATOMIC_SEQ_CST);
        MAIN_THREAD_ASYNC_EM_ASM({ Module['wowGame'].done($0); }, r);
    }
    return NULL;
}

/* the page: a new request (kind, a copy of the text) */
EMSCRIPTEN_KEEPALIVE ControllerRequest *WoW_ControllerNew(int kind, const char *text)
{
    ControllerRequest *r = calloc(1, sizeof(*r));

    if (r == NULL) return NULL;
    r->kind = kind;
    r->text = strdup(text != NULL ? text : "");
    return r;
}

/* the page: a new request with binary content (kind 3: the world queries) */
EMSCRIPTEN_KEEPALIVE ControllerRequest *WoW_ControllerNewData(int kind, const uint8_t *content, uint32_t size)
{
    ControllerRequest *r = calloc(1, sizeof(*r));

    if (r == NULL) return NULL;
    r->kind = kind;
    r->text = malloc(size ? size : 1);
    if (r->text == NULL) { free(r); return NULL; }
    memcpy(r->text, content, size);
    r->text_size = size;
    return r;
}

EMSCRIPTEN_KEEPALIVE void WoW_ControllerPost(ControllerRequest *r)
{
    Queue *q = r->kind == 3 ? &world_queue : &service_queue;

    pthread_mutex_lock(&queue_lock);
    if (!q->started)
    {
        pthread_t thread;

        q->started = 1;
        if (pthread_create(&thread, NULL, r->kind == 3 ? world_main : service_main, NULL) == 0) pthread_detach(thread);
    }
    r->next = NULL;
    if (q->tail != NULL) q->tail->next = r;
    else q->head = r;
    q->tail = r;
    pthread_cond_broadcast(&queue_changed);
    pthread_mutex_unlock(&queue_lock);
}

EMSCRIPTEN_KEEPALIVE void WoW_ControllerFree(ControllerRequest *r)
{
    if (r == NULL) return;
    free(r->text);
    free(r->data);
    free(r);
}

/* the answer for the page: state, data and size (the layout of ControllerRequest: wasm32) */
EMSCRIPTEN_KEEPALIVE uint32_t WoW_ControllerServed(int bytes)
{
    return (uint32_t) (bytes ? bytes_served : files_served);
}
