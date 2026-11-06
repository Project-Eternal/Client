#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <emscripten.h>

#include "srw.h"
#include "srw_debuglog.h"
#include "WoW.h"
#include "WoW-world.h"

#ifdef SRW_DIAGNOSTICS
static int compare_doubles(const void *a, const void *b)
{
    double x = *(const double *) a, y = *(const double *) b;

    return x < y ? -1 : x > y;
}

static void *world_bench_thread(void *argument)
{
    unsigned n = *(unsigned *) argument, i, answered = 0, hits = 0;
    static double took[100000];
    double start, total;

    if (n > 100000) n = 100000;
    x86_initialize_cpu();
    start = emscripten_get_now();
    for (i = 0; i < n; i++)
    {
        WoWWorldQuery q;
        double t0 = emscripten_get_now();

        memset(&q, 0, sizeof(q));
        q.op = WOW_WORLD_TRACE;
        q.flags = WOW_TRACE_M2 | WOW_TRACE_WMO;
        q.p[0] = -8949.95f + (float) (i % 20); q.p[1] = -132.49f + (float) (i % 7); q.p[2] = 85.0f;
        q.p[3] = -8880.0f; q.p[4] = -132.49f - (float) (i % 11); q.p[5] = 85.0f;
        WoW_WorldQueries(&q, 1);
        took[i] = emscripten_get_now() - t0;
        if (q.answer.known) answered++;
        if (q.answer.known && q.answer.u[0]) hits++;
    }
    total = emscripten_get_now() - start;
    qsort(took, n, sizeof(took[0]), compare_doubles);
    logprintf("Worldbench: %u questions in %.0f ms (%.0f per second), %u answered (%u hits); each: median %.2f ms, 90%% %.2f ms, 99%% %.2f ms, "
              "max %.2f ms", n, total, n * 1000.0 / (total > 0 ? total : 1), answered, hits, n ? took[n / 2] : 0.0, n ? took[n * 9 / 10] : 0.0,
              n ? took[n * 99 / 100] : 0.0, n ? took[n - 1] : 0.0);
    WoW_WorldLogNow = 1;
    {
        WoWWorldQuery q;

        memset(&q, 0, sizeof(q));
        WoW_WorldQueries(&q, 1);   /* (logs the counts) */
    }
    return NULL;
}

static void *world_question_thread(void *argument)
{
    x86_initialize_cpu();
    WoW_WorldLogNow = 1;
    WoW_WorldQueries(argument, 1);
    return NULL;
}

typedef struct { const char *name; uint32_t cvar; } CvarCall;

static void cvar_call(void *argument)
{
    CvarCall *call = argument;
    uint32_t args[1] = {(uint32_t) (uintptr_t) call->name};

    call->cvar = SRW_CallGuest(0x767440u, 1, args);
}

/* a Lua chunk in the client's interface (3.3.5: FrameScript_Execute 0x819210, cdecl script, source name, taint 0): the glue screens too */
static void lua_call(void *argument)
{
    static char source[] = "autoinput";
    uint32_t args[3] = {(uint32_t) (uintptr_t) argument, (uint32_t) (uintptr_t) source, 0};

    SRW_CallGuest(0x819210u, 3, args);
}

typedef struct { uint32_t args[5], result; } DeflateCall;

static void deflate_call(void *argument)
{
    DeflateCall *call = argument;

    call->result = SRW_CallGuest(0x778040u, 5, call->args);
}


/* the commands of the scripted input (/Port/autoinput.txt) that are about the game; the line and the text after its first word */
int SRW_AutoinputCommand(const char *line, const char *text)
{
    if (0) {}
        else if (strncmp(line, "cvar ", 5) == 0)   /* a CVar's value (3.3.5: 0x767440 looks it up by name, +0x28 its string, +0x30 its number) */
        {
            static char name[64];
            CvarCall call;
            uint32_t cvar;

            snprintf(name, sizeof(name), "%s", text);
            call.name = name;
            SRW_RunOnGameThread(cvar_call, &call);
            cvar = call.cvar;
            logprintf("Cvar %s = \"%s\" (number %d)", name, cvar != 0 && *(const uint32_t *) (uintptr_t) (cvar + 0x28) != 0 ?
                      (const char *) (uintptr_t) *(const uint32_t *) (uintptr_t) (cvar + 0x28) : "?", cvar != 0 ? (int) *(const uint32_t *) (uintptr_t) (cvar + 0x30) : -1);
        }
        else if (strncmp(line, "deflate", 7) == 0)   /* deflate N: the client's compress (3.3.5: 0x778040, stdcall dest, &length, source, size, level) on N bytes, checked with the system's zlib */
        {
            static uint8_t source[65536], dest[70000], check[65536];
            unsigned long size = (unsigned long) strtoul(text, NULL, 10), n, produced;
            uint32_t length, result;
            static DeflateCall call;
            uLongf out_size = sizeof(check);
            int z;

            if (size == 0 || size > sizeof(source)) size = 670;
            for (n = 0; n < size; n++) source[n] = (uint8_t) ((n % 7 == 0) ? "Blizzard_BattlefieldMinimap"[(n / 7) % 27] : (n * 31 + (n >> 3)) & 0x7f);
            length = sizeof(dest);
            call.args[0] = (uint32_t) (uintptr_t) dest; call.args[1] = (uint32_t) (uintptr_t) &length; call.args[2] = (uint32_t) (uintptr_t) source; call.args[3] = (uint32_t) size;
            call.args[4] = 0xffffffffu;
            SRW_RunOnGameThread(deflate_call, &call);
            result = call.result;
            produced = length;
            z = uncompress(check, &out_size, dest, produced);
            logprintf("Deflate %lu bytes: the client's compress returned %d, %lu bytes; zlib uncompress %d, %lu bytes, equal %d", size, (int) result, produced, z, (unsigned long) out_size,
                      z == 0 && out_size == size && memcmp(check, source, size) == 0);
        }
        else if (strncmp(line, "zlib ", 5) == 0) WoW_ZlibSetMode(strstr(line, "check") != NULL ? 2 : strstr(line, "native") != NULL ? 1 : 0);   /* zlib client | native | check */
        else if (strncmp(line, "lua ", 4) == 0)   /* lua CHUNK: runs it in the client's interface (3.3.5 only), e.g. lua SetGlueScreen("charcreate") */
        {
            static char chunk[1024];

            snprintf(chunk, sizeof(chunk), "%s", text);
            SRW_RunOnGameThread(lua_call, chunk);
        }
        else if (strncmp(line, "worldmode ", 10) == 0) WoW_WorldSetMode(text);   /* worldmode safepoint | sleep | frame [MS]: how the world questions run (and how long one may wait) */
        else if (strncmp(line, "worldbench ", 11) == 0)   /* worldbench N: N line of sight questions one after another (Northshire), the time each took */
        {
            pthread_t asker;
            static unsigned count;

            count = (unsigned) strtoul(text, NULL, 10);
            if (pthread_create(&asker, NULL, world_bench_thread, &count) == 0) pthread_join(asker, NULL);
        }
        else if (strncmp(line, "world ", 6) == 0)   /* world OP MAP FLAGS X Y Z [X2 Y2 Z2]: a world query of the server's (WoW-world.h) */
        {
            WoWWorldQuery q;

            memset(&q, 0, sizeof(q));
            if (sscanf(line + 6, "%u %u %x %f %f %f %f %f %f", &q.op, &q.map, &q.flags, &q.p[0], &q.p[1], &q.p[2], &q.p[3], &q.p[4], &q.p[5]) >= 3)
            {
                pthread_t asker;

                /* (on a guest thread: the questions run on the asking thread) */
                if (pthread_create(&asker, NULL, world_question_thread, &q) == 0) pthread_join(asker, NULL);
                uint32_t bits[2];

                memcpy(bits, &q.answer.f[1], 8);
                logprintf("World %u map %u flags %x (%.2f %.2f %.2f -> %.2f %.2f %.2f): known %d, %u %u %u, %.3f %.3f %.3f (bits %x %x)", q.op, q.map, q.flags,
                          q.p[0], q.p[1], q.p[2], q.p[3], q.p[4], q.p[5], q.answer.known, q.answer.u[0], q.answer.u[1], q.answer.u[2], q.answer.f[0],
                          q.answer.f[1], q.answer.f[2], (unsigned int) bits[0], (unsigned int) bits[1]);
            }
        }
    else return 0;
    return 1;
}

int SRW_ProgramReady(void)
{
    return WoW_SandboxInWorld;
}
#endif
