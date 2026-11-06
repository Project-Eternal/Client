/* The port's own chat commands (not the game's): .zlib, .sched, .prof, and in the diagnostics build .gfx and .log. Whoever reads the player's
   commands calls WoW_PortCommand: the in-process sandbox server (WoW-sandbox.c, ".gfx flat") and the embedded AzerothCore server through
   mod-wowcontroller (".wow gfx flat", WoW-controller.c). The answers go to `reply`, one line per call. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef SANDBOX_NATIVE
/* the native test of the sandbox: no Direct3D, the log on stderr */
#define logprintf(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
int SRW_DebugLogCategory(int index, const char **name, const char **description, int *on);
int SRW_DebugLogSet(const char *name, int on);
static int gfx_mode;
static void SRW_D3DSetDebugMode(int mode) { gfx_mode = mode; }
static int SRW_D3DDebugMode(void) { return gfx_mode; }
static void SRW_D3DHideState(int state) { (void) state; }
static void SRW_D3DHideFvf(int fvf) { (void) fvf; }
static void SRW_D3DHalfPixel(int on) { (void) on; }
static int SRW_D3DDescribeGraphics(char *out, size_t size) { return snprintf(out, size, "no Direct3D"); }
static void WoW_WorldDescribe(char *text, size_t size) { snprintf(text, size, "World questions of the server: none (no game)"); }
#else
#include "srw_debuglog.h"
#include "WoW.h"
#include "WoW-world.h"
/* the Direct3D wrapper (d3d9-webgl/d3d9.cpp): the debug modes of its shaders */
void SRW_D3DSetDebugMode(int mode);
int SRW_D3DDebugMode(void);
void SRW_D3DHideState(int state);
void SRW_D3DHideFvf(int fvf);
void SRW_D3DHalfPixel(int on);
int SRW_D3DDescribeGraphics(char *out, size_t size);
#endif

extern volatile int SRW_ProfOn;
void SRW_ProfSet(int on);
void SRW_GilSetMode(int game_first, double budget_ms, double min_slice_ms);
int SRW_GilGetMode(void);
void WoW_ZlibSetMode(int mode);
int WoW_ZlibGetMode(void);

/* 1 when the command is one of the port's (answered), 0 when it is not */
int WoW_PortCommand(const char *command, int count, char **arguments, void (*reply)(void *context, const char *text), void *context)
{
    char text[480];

    if (strcasecmp(command, "zlib") == 0)
    {
        /* .zlib [client | native | check]: who decompresses the archives' zlib sectors (WoW-mpqcodec.c): the client's own (recompiled) code, the program's zlib without
           the big lock, or both with the results compared (mismatches are logged) */
        static const char *const names[] = { "client", "native", "check" };

        if (count > 0)
        {
            int m;

            for (m = 0; m < 3; m++) if (strcasecmp(arguments[0], names[m]) == 0) WoW_ZlibSetMode(m);
        }
        snprintf(text, sizeof(text), "Archive zlib decompression: %s", names[WoW_ZlibGetMode()]);
        reply(context, text);
    }
    else if (strcasecmp(command, "sched") == 0)
    {
        /* .sched [game | fair] [budget in ms] [minimum slice in ms]: who gets the big lock (cpu.c of the runtime): "game" lets the thread that draws the frames in before
           the worker threads (they still get it after waiting for the budget, 8 ms by default, and keep it for at least 2 ms), "fair" (the default)
           is first come first served;
           without an argument it says which is on */
        if (count > 0)
        {
            SRW_GilSetMode(strcasecmp(arguments[0], "game") == 0, count > 1 ? atof(arguments[1]) : 0, count > 2 ? atof(arguments[2]) : 0);
        }
        snprintf(text, sizeof(text), "Big lock: %s", SRW_GilGetMode() ? "the game thread first" : "first come first served");
        reply(context, text);
    }
    else if (strcasecmp(command, "prof") == 0)
    {
        /* .prof [on | off]: the pie chart of where the time of the last frames went (WoW-prof.c, drawn by the page); with no argument it toggles */
        int on = count == 0 ? !SRW_ProfOn : !(strcmp(arguments[0], "off") == 0 || strcmp(arguments[0], "0") == 0);

        SRW_ProfSet(on);
        reply(context, on ? "Frame time chart: on" : "Frame time chart: off");
        WoW_WorldDescribe(text, sizeof(text));   /* (the server's world questions: WoW-world.c) */
        reply(context, text);
    }
#ifdef SRW_DIAGNOSTICS
    else if (strcasecmp(command, "gfx") == 0)
    {
        /* the debug modes of the Direct3D shaders: 0 off, 2 flat color, 3 lighting, 4 textures, 9 census of the draw states (log),
           10 no draws, 11 no rasterization, 12 first triangle only, 14-17 alpha of the vertex, of stage 0, of stage 1, the final one, 18 red with the real alpha, 19 red with alpha one half */
        static const struct { const char *name; int mode; } names[] = {
            { "off", 0 }, { "flat", 2 }, { "lighting", 3 }, { "textures", 4 }, { "census", 9 }, { "nodraw", 10 }, { "novertexraster", 11 },
            { "triangle", 12 }, { "vertexalpha", 14 }, { "tex0alpha", 15 }, { "tex1alpha", 16 }, { "alpha", 17 }, { "redalpha", 18 }, { "halfred", 19 }
        };
        int mode = -1, n;

        if (count == 0)
        {
            snprintf(text, sizeof(text), "Graphics debug mode: %d", SRW_D3DDebugMode());
            reply(context, text);
            return 1;
        }
        if (strcmp(arguments[0], "report") == 0)
        {
            /* .gfx report: the back buffer, alpha to coverage, anisotropy and the specular/lit/generated-coordinate draws of the last frame
              ; one long text, sent in lines cut after a "; " */
            char big[1500], *line = big;

            SRW_D3DDescribeGraphics(big, sizeof(big));
            while (*line)
            {
                char *end = line + strlen(line), *cut;

                if (end - line > 200)
                {
                    for (cut = line + 200; cut > line && !(cut[0] == ' ' && cut[-1] == ';'); cut--) ;
                    if (cut > line) end = cut;
                    else end = line + 200;
                }
                snprintf(text, sizeof(text), "%.*s", (int) (end - line), line);
                reply(context, text);
                line = end;
                while (*line == ' ') line++;
            }
            return 1;
        }
        if (strcmp(arguments[0], "halfpixel") == 0)
        {
            /* .gfx halfpixel 0|1: the half pixel shift of Direct3D 9 (on by default) */
            int on = count > 1 ? atoi(arguments[1]) : 1;

            SRW_D3DHalfPixel(on != 0);
            snprintf(text, sizeof(text), "Half pixel shift %s", on ? "on" : "off");
            reply(context, text);
            return 1;
        }
        if (strcmp(arguments[0], "hidefvf") == 0)
        {
            /* .gfx hidefvf 212: the draws with the vertex format 0x212 (as the census prints it) are skipped; no argument: show all */
            int fvf = count > 1 ? (int) strtol(arguments[1], NULL, 16) : -1;

            SRW_D3DHideFvf(fvf);
            if (count > 1) snprintf(text, sizeof(text), "Hiding vertex format %x", (unsigned int) fvf);
            else snprintf(text, sizeof(text), "Hiding no vertex format");
            reply(context, text);
            return 1;
        }
        if (strcmp(arguments[0], "hide") == 0)
        {
            /* .gfx hide N: the draws of the census state N are skipped (the states are numbered in the log while the census runs) */
            int state = (count > 1 && arguments[1][0] >= '0' && arguments[1][0] <= '9') ? atoi(arguments[1]) : -1;

            SRW_D3DHideState(state);
            if (state >= 0) snprintf(text, sizeof(text), "Hiding draw state %d", state);
            else snprintf(text, sizeof(text), "Hiding nothing");
            reply(context, text);
            return 1;
        }
        for (n = 0; n < (int) (sizeof(names) / sizeof(names[0])); n++) if (strcmp(arguments[0], names[n].name) == 0) mode = names[n].mode;
        if (mode < 0 && arguments[0][0] >= '0' && arguments[0][0] <= '9') mode = atoi(arguments[0]);
        if (mode < 0)
        {
            reply(context, "Unknown mode");
            return 1;
        }
        SRW_D3DSetDebugMode(mode);
        snprintf(text, sizeof(text), "Graphics debug mode: %d", mode);
        reply(context, text);
    }
    else if (strcasecmp(command, "log") == 0)
    {
        /* .log: the debug log categories and whether they are on; .log {category | all} [on | off]; .log off: all categories off */
        if (count == 0)
        {
            const char *name, *description;
            int on, n;

            reply(context, "Debug log categories:");
            for (n = 0; SRW_DebugLogCategory(n, &name, &description, &on); n++)
            {
                snprintf(text, sizeof(text), "    %s: %s - %s", name, on ? "on" : "off", description);
                reply(context, text);
            }
            return 1;
        }
        else
        {
            int on = !(count > 1 && (strcmp(arguments[1], "off") == 0 || strcmp(arguments[1], "0") == 0));
            const char *name = strcmp(arguments[0], "off") == 0 ? "all" : arguments[0];

            if (strcmp(arguments[0], "off") == 0) on = 0;
            if (!SRW_DebugLogSet(name, on))
            {
                reply(context, "Unknown category");
                return 1;
            }
            snprintf(text, sizeof(text), "Log %s: %s", name, on ? "on" : "off");
            reply(context, text);
            logprintf("Debug log %s %s", name, on ? "on" : "off");
        }
    }
#endif
    else return 0;
    return 1;
}
