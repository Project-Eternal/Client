#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "srw.h"
#include "srw_debuglog.h"
#include "WoW.h"

/* Native versions of two loops of the program (SRW/native_procs.llasm): the first match of a dword in [start, end) (the end when
   there is none), and the index of the first element equal to the key from the given index on (the size when there is none) */
uint32_t CCALL WoW_FindDword_c(uint32_t start, uint32_t end, uint32_t value)
{
    const uint32_t *p = (const uint32_t *) (uintptr_t) start, *e = (const uint32_t *) (uintptr_t) end;

    while (p < e && *p != value) p++;
    return (uint32_t) (uintptr_t) p;
}

/* the answer of the scan module for a clean machine (see native_procs.llasm, 0x6bb240) */
uint32_t CCALL WoW_ScanStub_c(uint32_t byte_a, uint32_t byte_b, uint32_t message, uint32_t buffer)
{
    *(uint8_t *) (uintptr_t) byte_a = 0;
    *(uint8_t *) (uintptr_t) byte_b = 0;
    *(uint32_t *) (uintptr_t) message = 0x9e930c;   /* "OK" in the image */
    if (buffer != 0) *(uint8_t *) (uintptr_t) buffer = 0;
    return 1;
}

uint32_t CCALL WoW_FindIndex_c(uint32_t begin, uint32_t end, uint32_t key, uint32_t index)
{
    const uint32_t *v = (const uint32_t *) (uintptr_t) begin;
    uint32_t count = begin == 0 || end < begin ? 0 : (end - begin) / 4;

    while (index < count && v[index] != key) index++;
    return index;
}

/* the services of the game that start with the program: the built-in server, the sound self test */
void SRW_ProgramStart(void)
{
    WoW_SandboxStart();
#ifdef WOW_AUDIO_SELFTEST
    WoW_AudioSelfTest();
#endif
}

const char *SRW_RegistryKey(void)
{
    return "SOFTWARE\\Blizzard Entertainment\\World of Warcraft";
}

/* the start procedures of the threads of the sound library (its mixer and stream threads), and of a worker whose time the profile follows */
int SRW_ThreadRole(uint32_t start)
{
    return start == 0x8e5190u ? SRW_THREAD_AUDIO : start == 0x76ff30u ? SRW_THREAD_WATCHED : SRW_THREAD_NORMAL;
}

/* the archives of the game are read sequentially */
int SRW_RemoteSequentialFile(const char *path)
{
    size_t n = strlen(path);

    return n > 4 && strcasecmp(path + n - 4, ".mpq") == 0;
}
