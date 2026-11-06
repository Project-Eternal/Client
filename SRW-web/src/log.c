#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <emscripten.h>
#include <emscripten/threading.h>

#include "srw.h"
#include "srw_debuglog.h"

/* both work from any thread: the page code runs on the main thread */
EM_JS(void, SRW_JsPrint, (const char *text, int raise), {
    out(UTF8ToString(text));
    if (raise && typeof showPanel === 'function') showPanel(true);
});

/* every line of the log begins with the part that wrote it: the game and its glue write [Game] here, the others (the Direct3D wrapper, the shared world) their own */
static void SRW_Print(const char *line, int raise)
{
    char text[1100];

    snprintf(text, sizeof(text), "%s%s", line[0] == '[' ? "" : "[Game] ", line);
    if (emscripten_is_main_browser_thread())
    {
        SRW_JsPrint(text, raise);
    }
    else if (EM_ASM_INT({ return typeof process === "object" && typeof require === "function" ? 1 : 0; }))
    {
        /* Node (headless runs): a worker writes to stderr itself, so the log goes on even when the main thread is blocked */
        EM_ASM({ require("fs").writeSync(2, UTF8ToString($0) + "\n"); }, text);
    }
    else
    {
        char *copy = strdup(text);
        MAIN_THREAD_ASYNC_EM_ASM({
            var text = UTF8ToString($0);
            _free($0);
            out(text);
            if ($1 && typeof showPanel === 'function') showPanel(true);
        }, copy, raise);
    }
}

void logprint(const char *text)
{
    SRW_Print(text, 0);
}

void warnprint(const char *text)
{
    SRW_Print(text, 1);
}

void logprintf(const char *format, ...)
{
    char buffer[1024];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    logprint(buffer);
}

void warnprintf(const char *format, ...)
{
    char buffer[1024];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    warnprint(buffer);
}

#ifdef SRW_DIAGNOSTICS
/* the debug logging categories (srw_debuglog.h): all off at the start */
volatile uint32_t SRW_DebugLogMask = 0;

static const struct { const char *name; uint32_t bit; const char *description; } debug_log_categories[] = {
    { "perf", SRW_LOG_PERF, "frame rate, where the time of a frame goes, Direct3D call statistics" },
    { "d3d", SRW_LOG_D3D, "Direct3D resources and state: textures, render targets, shaders, lights, the first draws" },
    { "watchdog", SRW_LOG_WATCHDOG, "every 5 s: what each thread does, streaming" },
    { "memory", SRW_LOG_MEMORY, "every 5 s: heap, allocations, data sent to WebGL" },
    { "files", SRW_LOG_FILES, "file system calls of the program, streamed reads" },
    { "threads", SRW_LOG_THREADS, "CreateThread, long waits, corrected delays" },
    { "modules", SRW_LOG_MODULES, "LoadLibrary/GetProcAddress failures, synthetic modules" },
    { "net", SRW_LOG_NET, "sockets and proxy connections" },
    { "input", SRW_LOG_INPUT, "pointer and focus of the page" },
    { "client", SRW_LOG_CLIENT, "OutputDebugString of the program" },
    { "imports", SRW_LOG_IMPORTS, "unimplemented imports" },
    { "sound", SRW_LOG_SOUND, "sound streams: open, play, stop, decoder, underruns" },
};

int SRW_DebugLogCategory(int index, const char **name, const char **description, int *on)
{
    if (index < 0 || index >= (int) (sizeof(debug_log_categories) / sizeof(debug_log_categories[0]))) return 0;
    *name = debug_log_categories[index].name;
    *description = debug_log_categories[index].description;
    *on = (SRW_DebugLogMask & debug_log_categories[index].bit) != 0;
    return 1;
}

/* the categories that are on from the start: /debuglog.txt (names separated by spaces, commas or lines; written by the page from the
   setting debugLog, or by SRW_NODE_FILES), for what happens before anyone can type a command */
void SRW_DebugLogInit(void)
{
    FILE *f = fopen("/Port/debuglog.txt", "r");
    char word[64];

    if (f == NULL) return;
    while (fscanf(f, " %63[^ ,\r\n]%*[ ,\r\n]", word) == 1)
    {
        if (!SRW_DebugLogSet(word, 1)) logprintf("/debuglog.txt: unknown category %s", word);
    }
    fclose(f);
}

int SRW_DebugLogSet(const char *name, int on)
{
    size_t i;
    uint32_t all = 0;

    for (i = 0; i < sizeof(debug_log_categories) / sizeof(debug_log_categories[0]); i++) all |= debug_log_categories[i].bit;
    if (strcmp(name, "all") == 0)
    {
        if (on) SRW_DebugLogMask |= all; else SRW_DebugLogMask &= ~all;
        return 1;
    }
    for (i = 0; i < sizeof(debug_log_categories) / sizeof(debug_log_categories[0]); i++)
    {
        if (strcmp(name, debug_log_categories[i].name) != 0) continue;
        if (on) SRW_DebugLogMask |= debug_log_categories[i].bit; else SRW_DebugLogMask &= ~debug_log_categories[i].bit;
        return 1;
    }
    return 0;
}
#endif
