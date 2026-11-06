/* The files the client opens (Storm's open, 0x424b50: the stub of native_procs.llasm reports every name before the original goes on).
   Phase 1: the names are logged (category files) to see what a new area asks for. */
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <emscripten.h>

#include "srw_debuglog.h"
#include "WoW.h"

/* how many files of each kind the client has opened (all threads; the slow frame report of WoW-prof.c takes the difference over a frame) */
static volatile uint32_t opened[4];   /* terrain tiles (.adt), textures (.blp), models (.m2 .skin .wmo), others */

void WoW_OpenCounts(uint32_t *out)
{
    int i;

    for (i = 0; i < 4; i++) out[i] = opened[i];
}

/* 1 once the client has opened its first file through Storm: its archives are open by then (they are opened first, at the start) */
volatile int WoW_StormReady;

uint32_t CCALL WoW_StormOpen_c(uint32_t name)
{
    static int logged;
    const char *text = (const char *) (uintptr_t) name;

    WoW_StormReady = 1;

    if (name != 0)
    {
        const char *dot = strrchr(text, '.');
        int kind = 3;

        if (dot != NULL)
        {
            if (strcasecmp(dot, ".adt") == 0) kind = 0;
            else if (strcasecmp(dot, ".blp") == 0) kind = 1;
            else if (strcasecmp(dot, ".m2") == 0 || strcasecmp(dot, ".skin") == 0 || strcasecmp(dot, ".wmo") == 0) kind = 2;
        }
        __sync_fetch_and_add(&opened[kind], 1);
    }

    if (SRW_LOG_ON(SRW_LOG_FILES) && name != 0 && logged++ < 3000) logprintf("Files: open %s", text);
    return 0;
}
