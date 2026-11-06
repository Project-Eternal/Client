#ifndef SRW_WIN32_H
#define SRW_WIN32_H

#include <stdint.h>

/* The program's name comes from the build (-DSRW_PROGRAM_NAME='"name"'): it believes it is installed in C:\name and runs from name.exe;
   file names below that folder map to the root of the virtual file system */
#ifndef SRW_PROGRAM_NAME
#error "SRW_PROGRAM_NAME is not defined"
#endif
#define SRW_INSTALL_DIR "C:\\" SRW_PROGRAM_NAME
#define SRW_PROGRAM_EXE SRW_INSTALL_DIR "\\" SRW_PROGRAM_NAME ".exe"

#pragma pack(push, 4)

typedef struct {
    uint32_t dwOSVersionInfoSize;
    uint32_t dwMajorVersion;
    uint32_t dwMinorVersion;
    uint32_t dwBuildNumber;
    uint32_t dwPlatformId;
    char szCSDVersion[128];
} SRW_OSVERSIONINFOA;

typedef struct {
    uint32_t dwLength;
    uint32_t dwMemoryLoad;
    uint32_t dwTotalPhys;
    uint32_t dwAvailPhys;
    uint32_t dwTotalPageFile;
    uint32_t dwAvailPageFile;
    uint32_t dwTotalVirtual;
    uint32_t dwAvailVirtual;
} SRW_MEMORYSTATUS;

typedef struct {
    uint32_t MaxCharSize;
    uint8_t DefaultChar[2];
    uint8_t LeadByte[12];
} SRW_CPINFO;

/* RTL_CRITICAL_SECTION as the program lays it out */
typedef struct {
    uint32_t DebugInfo; /* used to hold the mutex */
    int32_t LockCount;
    int32_t RecursionCount;
    uint32_t OwningThread;
    uint32_t LockSemaphore;
    uint32_t SpinCount;
} SRW_CRITICAL_SECTION;

#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

void SRW_SetLastError(uint32_t error);

#ifdef __cplusplus
}
#endif

#endif
