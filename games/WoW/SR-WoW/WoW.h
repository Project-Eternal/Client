#ifndef WOW_H
#define WOW_H

#include <stdint.h>

#include "srw.h"

#ifdef __cplusplus
extern "C" {
#endif

void WoW_AudioSelfTest(void);
void WoW_SandboxStart(void);
extern volatile int WoW_SandboxInWorld;   /* a character of the sandbox server has entered the world (for the scripted input) */
void WoW_DescribeGameScheduler(void);   /* diagnostics build */
void WoW_ZlibSetMode(int mode);   /* the archives' zlib decompression: 0 the client's code, 1 native, 2 both and compared (the chat command .zlib) */
int WoW_ZlibGetMode(void);
void WoW_OpenCounts(uint32_t *out);

/* the game's own category of the frame time chart, after those of the runtime */
enum { WOW_PROF_WORLD = SRW_PROF_COUNT, WOW_PROF_COUNT };

#ifdef __cplusplus
}
#endif

#endif
