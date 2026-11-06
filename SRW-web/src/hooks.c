/* The defaults of the hooks declared in srw.h: a program defines the ones it needs, and its definition replaces the weak one here. */
#include <stddef.h>
#include <stdint.h>

#include "srw.h"

#define HOOK __attribute__((weak))

volatile int SRW_ProfOn;

HOOK void SRW_SafePoint(int source) { (void) source; }
HOOK void SRW_GameSleeps(int begins) { (void) begins; }
HOOK void SRW_ProgramStart(void) {}
HOOK int SRW_ProgramReady(void) { return 0; }
HOOK int SRW_AutoinputCommand(const char *line, const char *text) { (void) line; (void) text; return 0; }
HOOK const char *SRW_RegistryKey(void) { return NULL; }
HOOK int SRW_ThreadRole(uint32_t start) { (void) start; return SRW_THREAD_NORMAL; }
HOOK int SRW_RemoteSequentialFile(const char *path) { (void) path; return 0; }
HOOK int SRW_WindowAspect(float *aspect) { (void) aspect; return 0; }
HOOK int SRW_TextBox(int *left, int *top, int *right, int *bottom) { (void) left; (void) top; (void) right; (void) bottom; return -1; }
HOOK uint32_t SRW_IdleObject(void) { return 0; }
HOOK void SRW_DescribeIdle(uint32_t object, uint32_t timeout) { (void) object; (void) timeout; }

HOOK void SRW_ProfSet(int on) { (void) on; }
HOOK void SRW_ProfEnter(int category) { (void) category; }
HOOK void SRW_ProfLeave(void) {}
HOOK void SRW_ProfFrame(void) {}
HOOK void SRW_ProfNoteSleep(uint32_t caller) { (void) caller; }
