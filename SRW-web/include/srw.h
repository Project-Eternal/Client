#ifndef SRW_H
#define SRW_H

#include <stdint.h>
#include "llasm_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a dword of the program's memory */
#define GUEST32(address) (*(const volatile uint32_t *) (uintptr_t) (address))
int SRW_GuestReadable(uint32_t address, uint32_t length);

/* Hooks. The program's own code (outside this runtime) may define any of these; the runtime has a default that does nothing (hooks.c). */
/* a place where the program is between two units of work (source: 0 a poll, 1 the message queue, 2 a frame) */
void SRW_SafePoint(int source);
/* the thread that presents the frames goes to sleep (begins 1) or wakes (0) */
void SRW_GameSleeps(int begins);
/* the program starts, before its first thread: the program's own services start here */
void SRW_ProgramStart(void);
/* the program is in the state a script waits for (the scripted input's waitready) */
int SRW_ProgramReady(void);
/* a command of the scripted input the runtime does not know (the line, and the text after its first word); 0 when it is not known either */
int SRW_AutoinputCommand(const char *line, const char *text);
/* the registry key (under HKEY_LOCAL_MACHINE and HKEY_CURRENT_USER) in which the program looks for its install folder, NULL: none */
const char *SRW_RegistryKey(void);
/* what a thread is for, from its start procedure: the sound threads get the big lock ahead of the others, and a thread can be marked to be watched in the profile */
enum { SRW_THREAD_NORMAL, SRW_THREAD_AUDIO, SRW_THREAD_WATCHED };
int SRW_ThreadRole(uint32_t start);
/* a file the program reads sequentially, so that a read that does not continue the last one is not taken for a part of the same stream */
int SRW_RemoteSequentialFile(const char *path);
/* the aspect ratio a windowed program wants kept when the canvas changes (0: any) */
int SRW_WindowAspect(float *aspect);
/* the program's own text box that has the focus (asked at each PeekMessage): 1 with its rectangle in the window's pixels, 0 when no box has
   the focus, -1 when the program has no source of its own and the input method switch (ImmAssociateContext) decides */
int SRW_TextBox(int *left, int *top, int *right, int *bottom);
/* the program enabled (1) or disabled (0) the input method of its window */
void SRW_NoteInputMethod(int enabled);
/* what the thread of the frames waits in: the object at the top of the program's queue (0: none), and its description for the freeze report */
uint32_t SRW_IdleObject(void);
void SRW_DescribeIdle(uint32_t object, uint32_t timeout);
/* describes what the frame thread is waiting for (the scripted input's sched) */
void SRW_DescribeGameThread(void);

/* player-facing diagnostics: logprint is quiet, warnprint also raises the Log pane of the page */
void logprint(const char *text);
void logprintf(const char *format, ...);
void warnprint(const char *text);
void warnprintf(const char *format, ...);

/* recompiled program */
_cpu *x86_initialize_cpu(void);
void x86_deinitialize_cpu(void);

/* C-callable entry generated for the program's entry point (glue.llasm) */
extern void CCALL c_SRW_Entry(CPU);

extern void CCALL c_SRW_CallGuest(CPU);

/* functions looked up at run time (dynamic.def): the guest address of the procedure of each, and its library */
extern const uint32_t SRW_DynamicExportAddresses[];
extern const char *const SRW_DynamicExportNames[];
extern const char *const SRW_DynamicExportModules[];
extern const uint32_t SRW_DynamicExportCount;

/* calls a stdcall procedure of the program (a thread start, a window procedure...) from C and returns its eax */
uint32_t SRW_CallGuest(uint32_t address, unsigned int argument_count, const uint32_t *arguments);
void SRW_GuestStackBounds(uint32_t *low, uint32_t *high);   /* the guest stack the calling thread runs on now (its fiber's or its own) */
uint32_t SRW_CallGuestFastcall(uint32_t address, uint32_t first, uint32_t second);   /* ecx, edx */
uint32_t SRW_CallGuestFastcallArgs(uint32_t address, uint32_t first, uint32_t second, unsigned int argument_count, const uint32_t *arguments);

/* fills the map from the addresses of the program's code to its procedures (vamap.c) */
void SRW_VaInit(void);
extern const uint32_t SRW_VaBase, SRW_VaSize;   /* the range of the addresses of the program's code */
extern const uint32_t SRW_ImageSize;            /* the size of the executable's image, which starts at 0x400000 */

/* the pointer (cursor.c) */
void SRW_CursorImage(const uint32_t *rgba, int width, int height, int hot_x, int hot_y);
int SRW_CursorD3DShow(int show);
int SRW_CursorWin32Show(int show);
void SRW_CursorWin32Set(uint32_t system_id);
void SRW_CursorWarped(void);
void SRW_CursorButtons(int down);
int SRW_CursorLocked(void);
int SRW_CursorRelative(void);

void SRW_CanvasSize(int *width, int *height);
void SRW_ResizeToCanvas(void);

/* the big lock: only the thread that holds it runs guest code (when it is on: SRW_GilConfigure reads /biglock.txt at the start) */
/* runs a function on the game thread at its next look at the message queue and waits for it (user32.c; with the big lock: here, holding it) */
void SRW_RunOnGameThread(void (*function)(void *), void *argument);
/* the same with a limit: 0 (not run) when the game thread has not started it within timeout_ms or has not looked at its queue for that long */
int SRW_RunOnGameThreadWithin(void (*function)(void *), void *argument, double timeout_ms);

void SRW_GilConfigure(void);
void SRW_CpuConfigure(void);   /* /sse.txt: what cpuid reports */
int SRW_GilEnabled(void);
int SRW_GilHeld(void);
void SRW_GilAcquire(void);
void SRW_GilRelease(void);
void SRW_GilYield(void);
void SRW_GilPoll(void);
void SRW_ThreadExit(void);

/* the stress build (scons stress=1): a random pause, to vary the interleavings of the threads; nothing in the other builds */
#ifdef SRW_STRESS
void SRW_StressConfigure(void);
void SRW_StressPoint(void);
#define SRW_STRESS_POINT() SRW_StressPoint()
#else
#define SRW_STRESS_POINT() ((void) 0)
#endif

/* the address of a procedure of the dynamic exports (SRW/dynamic.def, d3d9.def) by name */
uint32_t SRW_FindProc(const char *name);

/* a Direct3D method without an implementation (logged once) answers D3DERR_NOTAVAILABLE */
uint32_t CCALL SRW_D3DStub_c(uint32_t id);

/* names of the imported functions, indexed by the id the generated stubs pass (imports.c) */
extern const char *const SRW_ImportNames[];
extern const uint32_t SRW_ImportCount;

/* the last procedures entered (build with traceprocs=1); SRW_Proc_c is called at the start of each */
void CCALL SRW_Proc_c(uint32_t address);
/* an instruction the recompiler could not translate (SSE...): the program stops there with its address */
void CCALL SRW_Unsupported_c(uint32_t address);

/* the sound of the page (waveout.c): the wave output of the program and DirectSound feed one ring of stereo floats */
void SRW_AudioStart(void);
int SRW_AudioContextRate(void);
uint32_t SRW_AudioRingPending(void);
uint32_t SRW_AudioRingSpace(void);
void SRW_AudioRingPush(const float *stereo, uint32_t frames);
void SRW_AudioAddHook(void (*hook)(void));
void SRW_AudioFrameStereo(int channels, int bits, int is_float, const uint8_t *frame, float *left, float *right);

/* the dword that stands for fs:[0] (prepare_exe.py: the last dword of .zdata, 0x400000 + 0x9d1000 + 0x1000 - 4) */
#define SRW_SEH_HEAD 0xdd1ffcu
void SRW_DumpProcTrace(void);
/* what the calling thread is doing (for the watchdog): NULL when it runs */
void SRW_RecentProcs(uint32_t *out, int count);
uint32_t SRW_SaneTimeout(uint32_t milliseconds, const char *what); /* a wrapped negative delay becomes 1 ms (logged) */
uint64_t SRW_NowNs(void); /* the clock of the program: nanoseconds, never goes back, the same for all threads */
void SRW_Note(const char *what, uint32_t arg);
void SRW_DisplaySizes(int32_t *canvas_width, int32_t *canvas_height, int32_t *screen_width, int32_t *screen_height);   /* CSS pixels */
void SRW_CanvasResized(int width, int height);
void SRW_NoteDevice(int windowed, int width, int height);   /* d3d9-bridge.cpp: a device was created */
void SRW_LogGuestStack(const char *what);   /* diagnostics: the return addresses on the calling thread's guest stack */
void SRW_NoteTimeout(uint32_t milliseconds);   /* the timeout of the wait just noted */
void SRW_BlockedAdd(int which, double ms); /* 0 sleeping, 1 waiting for objects, 2 waiting for the big lock */
void SRW_BlockedTake(double *out);
void SRW_BlockedPeek(double *out);
void SRW_GilNoteGameThread(void);   /* the calling thread presents the frames (it gets the big lock ahead of the worker threads in the game first mode) */
void SRW_GilSetMode(int game_first, double budget_ms, double min_slice_ms);   /* the scheduling of the big lock: 0 first come first served, 1 the game thread first (the chat command .sched) */
int SRW_GilGetMode(void);
int SRW_GilIsGame(void);      /* the calling thread presents the frames */
int SRW_GilIsAudio(void);     /* the calling thread is one of the sound system's */
void SRW_GilSetTag(uint32_t tag);   /* names the thread in the statistics of the lock (its start procedure) */
void SRW_GilSetAudio(void);   /* the calling thread gets the big lock ahead of the others (the sound system's threads) */
/* statistics of the streaming of data files */
extern volatile uint32_t srw_remote_reads, srw_remote_ms, srw_remote_kb, srw_remote_cached_kb;
void SRW_BadJump(_cpu *cpu, uint32_t target);

/* called by the glue of every import when the build has trace=1 */
void CCALL SRW_Trace_c(uint32_t id, uint32_t caller);
void SRW_NotePresent(void);
void SRW_ReactivateWindow(void);

/* called by the generated stub of an import that has no implementation yet; does not return */
void CCALL SRW_Unimplemented_c(uint32_t id);

/* the frame time pie chart (the program supplies it): where the game thread's time goes; off until .prof */
enum { SRW_PROF_PROGRAM, SRW_PROF_DRAW, SRW_PROF_D3D, SRW_PROF_SHADER, SRW_PROF_UPLOAD, SRW_PROF_PRESENT, SRW_PROF_LOCK, SRW_PROF_SLEEP, SRW_PROF_WAIT, SRW_PROF_DATA, SRW_PROF_COUNT };
extern volatile int SRW_ProfOn;
void SRW_ProfSet(int on);
void SRW_ProfEnter(int category);
void SRW_ProfLeave(void);
void SRW_ProfFrame(void);
void SRW_ProfNoteSleep(uint32_t caller);
#define SRW_PROF_ENTER(category) do { if (SRW_ProfOn) SRW_ProfEnter(category); } while (0)
#define SRW_PROF_LEAVE() do { if (SRW_ProfOn) SRW_ProfLeave(); } while (0)


#ifdef __cplusplus
}
#endif

void SRW_SetEventNative(uint32_t handle);
void SRW_ResetEventNative(uint32_t handle);


#endif
