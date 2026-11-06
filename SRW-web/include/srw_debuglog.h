#ifndef SRW_DEBUGLOG_H
#define SRW_DEBUGLOG_H

/* Debug logging. The trace and statistics lines exist only in the diagnostics build (scons diagnostics=1) and every category is off until
   the chat command .log (also only there) turns it on. Warnings, errors and the few lines about the start are not
   categories: they are always logged. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void logprint(const char *text);
void logprintf(const char *format, ...);

enum {
    SRW_LOG_PERF     = 1u << 0,    /* frame rate and where the time of a frame goes, the Direct3D call statistics */
    SRW_LOG_D3D      = 1u << 1,    /* Direct3D resources and state: textures, render targets, shaders, lights, clip planes, the first draws */
    SRW_LOG_WATCHDOG = 1u << 2,    /* every 5 s: what each thread does, how much data was streamed */
    SRW_LOG_MEMORY   = 1u << 3,    /* every 5 s: heap of the module, guest allocations, data sent to WebGL */
    SRW_LOG_FILES    = 1u << 4,    /* file system calls of the program (CreateFile, FindFirstFile, GetFileAttributes...) and streamed reads */
    SRW_LOG_THREADS  = 1u << 5,    /* CreateThread, long waits, delays that were corrected */
    SRW_LOG_MODULES  = 1u << 6,    /* LoadLibrary and GetProcAddress failures, the synthetic modules */
    SRW_LOG_NET      = 1u << 7,    /* sockets and the connections through the proxy */
    SRW_LOG_INPUT    = 1u << 8,    /* the pointer and the focus of the page */
    SRW_LOG_CLIENT   = 1u << 9,    /* what the program says through OutputDebugString */
    SRW_LOG_IMPORTS  = 1u << 10,   /* unimplemented or first-called imports */
    SRW_LOG_SOUND    = 1u << 11    /* sound streams: open, play, stop, close, the decoder, underruns */
};

#ifdef SRW_DIAGNOSTICS
extern volatile uint32_t SRW_DebugLogMask;

/* the categories for the .log command: name, description, whether on; index from 0, returns 0 after the last one */
int SRW_DebugLogCategory(int index, const char **name, const char **description, int *on);
/* "all" or the name of a category; returns 0 for an unknown name */
int SRW_DebugLogSet(const char *name, int on);
/* reads /debuglog.txt: the categories that are on from the start */
void SRW_DebugLogInit(void);

#define SRW_LOG_ON(category) ((SRW_DebugLogMask & (category)) != 0)
#define SRW_LOG(category, ...) do { if (SRW_DebugLogMask & (category)) logprintf(__VA_ARGS__); } while (0)
#else
#define SRW_LOG_ON(category) 0
#define SRW_LOG(category, ...) do { } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif
