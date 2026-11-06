/* Windows functions that the client imports and the runtime does not implement. Most are answered as a Windows without the feature would (no sound
   devices, no HID, no COM classes, no fibers); the few that do something are the ones the client needs to start. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "WoW.h"
#include "srw_debuglog.h"
#include "imports.h"
#include "srw_object.h"
#include "srw_remote.h"
#include "srw_win32.h"

#define INVALID_HANDLE ((uint32_t) -1)
#define MMSYSERR_NODRIVER 6
#define E_FAIL_ 0x80004005u
#define REGDB_E_CLASSNOTREG 0x80040154u
#define ERROR_NO_MORE_ITEMS 259

/* ------------------------------------------------------------------------------------------------------------------ */
/* kernel32 */

uint32_t CCALL GetConsoleOutputCP_c(void) { return 437; }
uint32_t CCALL GetConsoleCP_c(void) { return 437; }
uint32_t CCALL GetConsoleMode_c(uint32_t hConsole, void *lpMode) { (void) hConsole; (void) lpMode; return 0; }
uint32_t CCALL WriteConsoleA_c(uint32_t hConsole, void *lpBuffer, uint32_t n, void *lpWritten, void *lpReserved)
{
    (void) hConsole; (void) lpBuffer; (void) n; (void) lpWritten; (void) lpReserved;
    return 0;
}

uint32_t CCALL OpenFile_c(const char *lpFileName, void *lpReOpenBuff, uint32_t uStyle)
{
    (void) lpFileName; (void) lpReOpenBuff; (void) uStyle;
    return INVALID_HANDLE;
}

uint32_t CCALL DeviceIoControl_c(uint32_t hDevice, uint32_t code, void *in, uint32_t inSize, void *out, uint32_t outSize, void *returned, void *overlapped)
{
    (void) hDevice; (void) code; (void) in; (void) inSize; (void) out; (void) outSize; (void) returned; (void) overlapped;
    SetLastError_c(1);
    return 0;
}

uint32_t CCALL OpenFileMappingA_c(uint32_t access, uint32_t inherit, const char *name)
{
    (void) access; (void) inherit; (void) name;
    SetLastError_c(2);
    return 0;
}

uint32_t CCALL OpenEventA_c(uint32_t access, uint32_t inherit, const char *name)
{
    (void) access; (void) inherit; (void) name;
    SetLastError_c(2);
    return 0;
}

uint32_t CCALL WaitForSingleObjectEx_c(uint32_t hHandle, uint32_t dwMilliseconds, uint32_t bAlertable)
{
    (void) bAlertable;
    return WaitForSingleObject_c(hHandle, dwMilliseconds);
}

uint32_t CCALL WaitForMultipleObjectsEx_c(uint32_t nCount, void *lpHandles, uint32_t bWaitAll, uint32_t dwMilliseconds, uint32_t bAlertable)
{
    (void) bAlertable;
    return WaitForMultipleObjects_c(nCount, lpHandles, bWaitAll, dwMilliseconds);
}

uint32_t CCALL SignalObjectAndWait_c(uint32_t hSignal, uint32_t hWait, uint32_t dwMilliseconds, uint32_t bAlertable)
{
    (void) bAlertable;
    SetEvent_c(hSignal);
    return WaitForSingleObject_c(hWait, dwMilliseconds);
}

/* asynchronous (APC) file I/O is not used by the program in practice */
uint32_t CCALL WriteFileEx_c(uint32_t hFile, void *lpBuffer, uint32_t n, void *lpOverlapped, void *lpRoutine)
{
    (void) hFile; (void) lpBuffer; (void) n; (void) lpOverlapped; (void) lpRoutine;
    SetLastError_c(1);
    return 0;
}

uint32_t CCALL ReadFileEx_c(uint32_t hFile, void *lpBuffer, uint32_t n, void *lpOverlapped, void *lpRoutine)
{
    (void) hFile; (void) lpBuffer; (void) n; (void) lpOverlapped; (void) lpRoutine;
    SetLastError_c(1);
    return 0;
}

/* the reads are done when ReadFile returns (see there): the structure says how much was transferred */
uint32_t CCALL GetOverlappedResult_c(uint32_t hFile, void *lpOverlapped, void *lpTransferred, uint32_t bWait)
{
    const uint32_t *overlapped = lpOverlapped;

    (void) hFile; (void) bWait;
    if (lpTransferred != NULL) *(uint32_t *) lpTransferred = overlapped[1];
    if (overlapped[0] != 0)
    {
        SetLastError_c(5);
        return 0;
    }
    return 1;
}

uint32_t CCALL CancelIo_c(uint32_t hFile) { (void) hFile; return 0; }

static uint32_t format_text(char *out, uint32_t size, const char *text)
{
    uint32_t length = (uint32_t) strlen(text) + 1;

    if (size == 0) return length;
    if (length > size)
    {
        SetLastError_c(122);
        return 0;
    }
    memcpy(out, text, length);
    return length;
}

/* the formats are those of the US locale whatever the flags and the format string say */
uint32_t CCALL GetDateFormatA_c(uint32_t locale, uint32_t flags, void *lpDate, const char *lpFormat, void *lpDateStr, uint32_t cchDate)
{
    uint16_t *st = lpDate;
    char text[64];
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);

    (void) locale; (void) flags; (void) lpFormat;
    if (st != NULL) snprintf(text, sizeof(text), "%u/%u/%u", st[1], st[3], st[0]);
    else snprintf(text, sizeof(text), "%d/%d/%d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
    return format_text(lpDateStr, cchDate, text);
}

uint32_t CCALL GetTimeFormatA_c(uint32_t locale, uint32_t flags, void *lpTime, const char *lpFormat, void *lpTimeStr, uint32_t cchTime)
{
    uint16_t *st = lpTime;
    char text[64];
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    unsigned hour = st ? st[4] : (unsigned) tm->tm_hour, minute = st ? st[5] : (unsigned) tm->tm_min, second = st ? st[6] : (unsigned) tm->tm_sec;

    (void) locale; (void) flags; (void) lpFormat;
    snprintf(text, sizeof(text), "%u:%02u:%02u %s", hour % 12 == 0 ? 12 : hour % 12, minute, second, hour < 12 ? "AM" : "PM");
    return format_text(lpTimeStr, cchTime, text);
}

static const uint16_t command_line_wide[] = {'"', 'C', ':', '\\', 'W', 'o', 'W', '\\', 'W', 'o', 'W', '.', 'e', 'x', 'e', '"', 0};

uint32_t CCALL GetCommandLineW_c(void)
{
    return PTR2REG(command_line_wide);
}

uint32_t CCALL GlobalMemoryStatusEx_c(void *lpBuffer)
{
    uint32_t *m = lpBuffer;   /* dwLength, dwMemoryLoad, ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual */

    if (m[0] < 64) return 0;
    memset(m + 1, 0, 60);
    m[1] = 25;
    m[2] = 0xc0000000u;  m[3] = 0;   /* 3 GB of RAM */
    m[4] = 0x80000000u;  m[5] = 0;   /* 2 GB free */
    m[6] = 0xc0000000u;  m[7] = 1;   /* 7 GB of page file */
    m[8] = 0x80000000u;  m[9] = 1;
    m[10] = 0x7ffe0000u; m[11] = 0;  /* the 2 GB of the user address space */
    m[12] = 0x70000000u; m[13] = 0;
    return 1;
}

uint32_t CCALL GetPriorityClass_c(uint32_t hProcess) { (void) hProcess; return 0x20; }
uint32_t CCALL SetPriorityClass_c(uint32_t hProcess, uint32_t cls) { (void) hProcess; (void) cls; return 1; }
uint32_t CCALL GetProcessAffinityMask_c(uint32_t hProcess, void *lpProcessMask, void *lpSystemMask)
{
    (void) hProcess;
    if (lpProcessMask) *(uint32_t *) lpProcessMask = 1;
    if (lpSystemMask) *(uint32_t *) lpSystemMask = 1;
    return 1;
}
uint32_t CCALL SetProcessAffinityMask_c(uint32_t hProcess, uint32_t mask) { (void) hProcess; (void) mask; return 1; }

uint32_t CCALL OpenThread_c(uint32_t access, uint32_t inherit, uint32_t id) { (void) access; (void) inherit; (void) id; SetLastError_c(5); return 0; }
uint32_t CCALL GetThreadContext_c(uint32_t hThread, void *lpContext) { (void) hThread; (void) lpContext; return 0; }
uint32_t CCALL CreateToolhelp32Snapshot_c(uint32_t flags, uint32_t pid) { (void) flags; (void) pid; return INVALID_HANDLE; }
uint32_t CCALL Thread32First_c(uint32_t h, void *entry) { (void) h; (void) entry; return 0; }
uint32_t CCALL Thread32Next_c(uint32_t h, void *entry) { (void) h; (void) entry; return 0; }
uint32_t CCALL Module32First_c(uint32_t h, void *entry) { (void) h; (void) entry; return 0; }
uint32_t CCALL Module32Next_c(uint32_t h, void *entry) { (void) h; (void) entry; return 0; }

uint32_t CCALL FindResourceExA_c(uint32_t hModule, const char *type, const char *name, uint32_t language)
{
    (void) hModule; (void) type; (void) name; (void) language;
    SetLastError_c(1813);
    return 0;
}

uint32_t CCALL CreateProcessA_c(const uint32_t *args)
{
    (void) args;
    SetLastError_c(5);
    return 0;
}

uint32_t CCALL GetShortPathNameA_c(const char *lpszLongPath, void *lpszShortPath, uint32_t cchBuffer)
{
    uint32_t length = (uint32_t) strlen(lpszLongPath);

    if (length >= cchBuffer) return length + 1;
    memcpy(lpszShortPath, lpszLongPath, length + 1);
    return length;
}

uint32_t CCALL SetFileAttributesA_c(const char *lpFileName, uint32_t attributes) { (void) lpFileName; (void) attributes; return 1; }

uint32_t CCALL GetFileAttributesExA_c(const char *lpFileName, uint32_t level, void *lpInfo)
{
    uint32_t *info = lpInfo;   /* attributes, creation, access, write (FILETIME each), size high, size low */
    char path[1040];
    struct stat st;
    uint64_t ticks, size;
    uint32_t attributes;
    const SRW_RemoteFile *remote;

    (void) level;
    SRW_MapPath(lpFileName, path, sizeof(path));
    if (stat(path, &st) == 0)
    {
        attributes = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
        size = (uint64_t) st.st_size;
        ticks = ((uint64_t) st.st_mtime + 11644473600u) * 10000000u;
    }
    else if ((remote = SRW_RemoteFind(path)) != NULL)
    {
        /* a file of the game's data: it is not in the file system, only in the manifest */
        attributes = 0x20;
        size = SRW_RemoteSize(remote);
        ticks = 132000000000000000ull;
    }
    else if (SRW_RemoteIsDirectory(path))
    {
        attributes = 0x10;
        size = 0;
        ticks = 132000000000000000ull;
    }
    else
    {
        SetLastError_c(2);
        return 0;
    }
    info[0] = attributes;
    for (int i = 0; i < 3; i++)
    {
        info[1 + 2 * i] = (uint32_t) ticks;
        info[2 + 2 * i] = (uint32_t) (ticks >> 32);
    }
    info[7] = (uint32_t) (size >> 32);
    info[8] = (uint32_t) size;
    return 1;
}

uint32_t CCALL MoveFileA_c(const char *lpExistingFileName, const char *lpNewFileName)
{
    char from[1040], to[1040];

    SRW_MapPath(lpExistingFileName, from, sizeof(from));
    SRW_MapPath(lpNewFileName, to, sizeof(to));
    if (rename(from, to) != 0)
    {
        SetLastError_c(2);
        return 0;
    }
    return 1;
}

uint32_t CCALL RemoveDirectoryA_c(const char *lpPathName)
{
    char path[1040];

    SRW_MapPath(lpPathName, path, sizeof(path));
    if (rmdir(path) != 0)
    {
        SetLastError_c(2);
        return 0;
    }
    return 1;
}

uint32_t CCALL GetDiskFreeSpaceExA_c(const char *lpDirectoryName, void *lpFreeBytesAvailable, void *lpTotalNumberOfBytes, void *lpTotalNumberOfFreeBytes)
{
    uint32_t *available = lpFreeBytesAvailable, *total = lpTotalNumberOfBytes, *free_bytes = lpTotalNumberOfFreeBytes;

    (void) lpDirectoryName;
    /* 64 GB free of 128 GB */
    if (available) { available[0] = 0; available[1] = 16; }
    if (total) { total[0] = 0; total[1] = 32; }
    if (free_bytes) { free_bytes[0] = 0; free_bytes[1] = 16; }
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* version, wininet, ole32, setupapi, hid, dinput8, msacm32: the answers of a system that has none of it */

uint32_t CCALL GetFileVersionInfoSizeA_c(const char *name, void *lpHandle) { (void) name; if (lpHandle) *(uint32_t *) lpHandle = 0; SetLastError_c(1813); return 0; }
uint32_t CCALL GetFileVersionInfoA_c(const char *name, uint32_t handle, uint32_t length, void *data) { (void) name; (void) handle; (void) length; (void) data; return 0; }
uint32_t CCALL VerQueryValueA_c(void *block, const char *sub, void *buffer, void *length) { (void) block; (void) sub; (void) buffer; (void) length; return 0; }

uint32_t CCALL InternetSetCookieA_c(const char *url, const char *name, const char *data) { (void) url; (void) name; (void) data; return 0; }
uint32_t CCALL InternetSetStatusCallbackA_c(void *handle, void *callback) { (void) handle; (void) callback; return 0; }

uint32_t CCALL CoInitialize_c(void *reserved) { (void) reserved; return 0; }
uint32_t CCALL CoUninitialize_c(void) { return 0; }
uint32_t CCALL CoCreateInstance_c(void *clsid, void *outer, uint32_t context, void *iid, void *ppv)
{
    (void) clsid; (void) outer; (void) context; (void) iid;
    if (ppv) *(uint32_t *) ppv = 0;
    return REGDB_E_CLASSNOTREG;
}
uint32_t CCALL CoTaskMemFree_c(void *p) { (void) p; return 0; }
uint32_t CCALL CLSIDFromString_c(const uint16_t *text, void *clsid) { (void) text; (void) clsid; return E_FAIL_; }
uint32_t CCALL PropVariantClear_c(void *pv) { (void) pv; return 0; }

uint32_t CCALL SetupDiGetClassDevsA_c(void *guid, const char *enumerator, void *hwnd, uint32_t flags)
{
    (void) guid; (void) enumerator; (void) hwnd; (void) flags;
    return INVALID_HANDLE;
}
uint32_t CCALL SetupDiEnumDeviceInterfaces_c(void *h, void *info, void *guid, uint32_t index, void *data) { (void) h; (void) info; (void) guid; (void) index; (void) data; SetLastError_c(259); return 0; }
uint32_t CCALL SetupDiGetDeviceInterfaceDetailA_c(void *h, void *data, void *detail, uint32_t size, void *required, void *info) { (void) h; (void) data; (void) detail; (void) size; (void) required; (void) info; return 0; }
uint32_t CCALL SetupDiDestroyDeviceInfoList_c(void *h) { (void) h; return 1; }
uint32_t CCALL SetupDiGetDeviceRegistryPropertyA_c(void *h, void *info, uint32_t prop, void *type, void *buffer, uint32_t size, void *required) { (void) h; (void) info; (void) prop; (void) type; (void) buffer; (void) size; (void) required; return 0; }
uint32_t CCALL SetupDiEnumDeviceInfo_c(void *h, uint32_t index, void *info) { (void) h; (void) index; (void) info; SetLastError_c(259); return 0; }

uint32_t CCALL HidD_GetHidGuid_c(void *guid) { if (guid) memset(guid, 0, 16); return 0; }
uint32_t CCALL HidD_GetAttributes_c(void *h, void *attributes) { (void) h; (void) attributes; return 0; }
uint32_t CCALL HidD_GetPreparsedData_c(void *h, void *data) { (void) h; (void) data; return 0; }
uint32_t CCALL HidD_FreePreparsedData_c(void *data) { (void) data; return 0; }
uint32_t CCALL HidP_GetCaps_c(void *data, void *caps) { (void) data; (void) caps; return 0xc0110001u; }
uint32_t CCALL HidD_GetSerialNumberString_c(void *h, void *buffer, uint32_t size) { (void) h; (void) buffer; (void) size; return 0; }
uint32_t CCALL HidD_GetProductString_c(void *h, void *buffer, uint32_t size) { (void) h; (void) buffer; (void) size; return 0; }
uint32_t CCALL HidD_SetFeature_c(void *h, void *buffer, uint32_t size) { (void) h; (void) buffer; (void) size; return 0; }

uint32_t CCALL DirectInput8Create_c(void *hinst, uint32_t version, void *iid, void *ppv, void *outer)
{
    (void) hinst; (void) version; (void) iid; (void) outer;
    if (ppv) *(uint32_t *) ppv = 0;
    return E_FAIL_;
}

/* the audio compression manager: no codecs */
uint32_t CCALL acmStreamOpen_c(void *a, void *b, void *c, void *d, void *e, uint32_t f, uint32_t g, uint32_t h) { (void) a; (void) b; (void) c; (void) d; (void) e; (void) f; (void) g; (void) h; return MMSYSERR_NODRIVER; }
uint32_t CCALL acmStreamSize_c(void *a, uint32_t b, void *c, uint32_t d) { (void) a; (void) b; (void) c; (void) d; return MMSYSERR_NODRIVER; }
uint32_t CCALL acmStreamPrepareHeader_c(void *a, void *b, uint32_t c) { (void) a; (void) b; (void) c; return MMSYSERR_NODRIVER; }
uint32_t CCALL acmStreamConvert_c(void *a, void *b, uint32_t c) { (void) a; (void) b; (void) c; return MMSYSERR_NODRIVER; }
uint32_t CCALL acmStreamUnprepareHeader_c(void *a, void *b, uint32_t c) { (void) a; (void) b; (void) c; return MMSYSERR_NODRIVER; }
uint32_t CCALL acmFormatSuggest_c(void *a, void *b, void *c, uint32_t d, uint32_t e) { (void) a; (void) b; (void) c; (void) d; (void) e; return MMSYSERR_NODRIVER; }

/* ------------------------------------------------------------------------------------------------------------------ */
/* user32, gdi32, advapi32, winmm, ws2_32 */

uint32_t CCALL VkKeyScanA_c(uint32_t ch)
{
    ch &= 0xff;
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 'A';
    if (ch >= 'A' && ch <= 'Z') return (ch) | 0x100;
    if ((ch >= '0' && ch <= '9') || ch == ' ') return ch;
    return 0xffff;
}

uint32_t CCALL CharLowerBuffA_c(const char *lpsz, uint32_t length)
{
    char *text = (char *) lpsz;

    for (uint32_t i = 0; i < length; i++) text[i] = (char) tolower((unsigned char) text[i]);
    return length;
}

uint32_t CCALL DrawTextExA_c(uint32_t hdc, const char *text, uint32_t count, void *rect, uint32_t format, void *params)
{
    (void) hdc; (void) text; (void) count; (void) rect; (void) format; (void) params;
    return 0;
}

uint32_t CCALL MonitorFromPoint_c(uint32_t x, uint32_t y, uint32_t flags) { (void) x; (void) y; (void) flags; return 1; }

uint32_t CCALL CreateBitmap_c(uint32_t w, uint32_t h, uint32_t planes, uint32_t bits, void *data) { (void) w; (void) h; (void) planes; (void) bits; (void) data; return 0; }
uint32_t CCALL TranslateCharsetInfo_c(void *source, void *csi, uint32_t flags) { (void) source; (void) csi; (void) flags; return 0; }
uint32_t CCALL StretchBlt_c(const uint32_t *args)
{
    (void) args;
    return 0;
}
uint32_t CCALL Rectangle_c(uint32_t hdc, uint32_t l, uint32_t t, uint32_t r, uint32_t b) { (void) hdc; (void) l; (void) t; (void) r; (void) b; return 1; }

uint32_t CCALL CryptAcquireContextA_c(void *phProv, const char *container, const char *provider, uint32_t type, uint32_t flags)
{
    (void) container; (void) provider; (void) type; (void) flags;
    if (phProv) *(uint32_t *) phProv = 1;
    return 1;
}
uint32_t CCALL CryptReleaseContext_c(uint32_t hProv, uint32_t flags) { (void) hProv; (void) flags; return 1; }
uint32_t CCALL CryptGenRandom_c(uint32_t hProv, uint32_t length, void *buffer)
{
    (void) hProv;
    uint8_t *bytes = buffer;

    for (uint32_t i = 0; i < length; i++) bytes[i] = (uint8_t) (rand() >> 7);
    return 1;
}
uint32_t CCALL ConvertStringSecurityDescriptorToSecurityDescriptorW_c(const uint16_t *text, uint32_t revision, void *sd, void *size)
{
    (void) text; (void) revision; (void) sd; (void) size;
    SetLastError_c(5);
    return 0;
}
uint32_t CCALL RegEnumKeyA_c(uint32_t hKey, uint32_t index, void *name, uint32_t size) { (void) hKey; (void) index; (void) name; (void) size; return ERROR_NO_MORE_ITEMS; }

uint32_t CCALL timeGetTime_c(void)
{
    return (uint32_t) (SRW_NowNs() / 1000000u);
}
uint32_t CCALL mciSendCommandA_c(uint32_t device, uint32_t message, uint32_t flags, uint32_t param) { (void) device; (void) message; (void) flags; (void) param; return 0x115; }

/* the wave output and the multimedia timers are in waveout.c of the runtime; the recording devices: none */
uint32_t CCALL waveInGetNumDevs_c(void) { return 0; }
uint32_t CCALL waveInOpen_c(void *a, uint32_t b, void *c, uint32_t d, uint32_t e, uint32_t f) { (void) a; (void) b; (void) c; (void) d; (void) e; (void) f; return MMSYSERR_NODRIVER; }
uint32_t CCALL waveInClose_c(uint32_t h) { (void) h; return MMSYSERR_NODRIVER; }
uint32_t CCALL waveInReset_c(uint32_t h) { (void) h; return MMSYSERR_NODRIVER; }
uint32_t CCALL waveInStart_c(uint32_t h) { (void) h; return MMSYSERR_NODRIVER; }
uint32_t CCALL waveInGetDevCapsA_c(uint32_t id, void *caps, uint32_t size) { (void) id; (void) caps; (void) size; return MMSYSERR_NODRIVER; }
uint32_t CCALL waveInAddBuffer_c(uint32_t h, void *header, uint32_t size) { (void) h; (void) header; (void) size; return MMSYSERR_NODRIVER; }
uint32_t CCALL waveInPrepareHeader_c(uint32_t h, void *header, uint32_t size) { (void) h; (void) header; (void) size; return MMSYSERR_NODRIVER; }
uint32_t CCALL waveInUnprepareHeader_c(uint32_t h, void *header, uint32_t size) { (void) h; (void) header; (void) size; return MMSYSERR_NODRIVER; }

uint32_t CCALL htonl_c(uint32_t value)
{
    return __builtin_bswap32(value);
}
uint32_t CCALL WSACreateEvent_c(void) { SRW_LOG(SRW_LOG_NET, "WSACreateEvent()"); return CreateEventA_c(NULL, 1, 0, NULL); }
uint32_t CCALL WSACloseEvent_c(uint32_t hEvent) { return CloseHandle_c(hEvent); }

/* the name is resolved at once; the answer is a HOSTENT in the caller's buffer and a message to the window */
uint32_t CCALL WSAAsyncGetHostByName_c(uint32_t hWnd, uint32_t wMsg, const char *name, void *buf, uint32_t buflen)
{
    static uint32_t next_task = 1;
    uint32_t task = next_task++;
    uint32_t found;

    SRW_LOG(SRW_LOG_NET, "WSAAsyncGetHostByName(\"%s\", window %x, message %x)", name, (unsigned int) hWnd, (unsigned int) wMsg);
    found = gethostbyname_c(name);
    uint32_t *hostent = (uint32_t *) (uintptr_t) found;
    uint32_t error = 0, length = 0;

    if (found == 0 || buflen < 64)
    {
        error = 11001;   /* WSAHOST_NOT_FOUND */
    }
    else
    {
        /* HOSTENT (name, aliases, type + length, address list), then the name, the null alias list, the address list, one address */
        uint32_t *out = buf;
        char *text = (char *) buf + 16;
        const char *host_name = (const char *) (uintptr_t) hostent[0];
        uint32_t *addresses = (uint32_t *) (uintptr_t) hostent[3];
        const uint8_t *address = (const uint8_t *) (uintptr_t) addresses[0];
        size_t n = strlen(host_name) + 1;
        uint32_t *tail;

        if (n > 200) n = 200;
        memcpy(text, host_name, n);
        text[n - 1] = 0;
        tail = (uint32_t *) (((uintptr_t) text + n + 3) & ~(uintptr_t) 3);
        tail[0] = 0;                                   /* the alias list: empty */
        tail[1] = (uint32_t) (uintptr_t) (tail + 3);   /* the address list: the address, then the end */
        tail[2] = 0;
        memcpy(tail + 3, address, 4);
        out[0] = (uint32_t) (uintptr_t) text;
        out[1] = (uint32_t) (uintptr_t) tail;
        out[2] = 2 | (4u << 16);                       /* AF_INET, 4 bytes */
        out[3] = (uint32_t) (uintptr_t) (tail + 1);
        length = (uint32_t) (((uint8_t *) (tail + 4)) - (uint8_t *) buf);
    }
    PostMessageA_c(hWnd, wMsg, task, (error << 16) | (length & 0xffff));
    return task;
}

uint32_t CCALL WSACancelAsyncRequest_c(uint32_t task) { (void) task; return 0; }

/* The game UI (FrameXML) of the 3.3.5 client: up while [0xbd0880] is 1 (set at the end of its initialization 0x52a980, cleared by its
   shutdown 0x528f00), built for the aspect [0xbd07f8] (stored by the initialization from the aspect function). */
int SRW_WindowClientSize(uint32_t hwnd, int *width, int *height);   /* (user32.c) */

int SRW_WindowAspect(float *aspect)
{
    if (*(volatile const uint32_t *) (uintptr_t) 0xbd0880u == 0) return 0;
    *aspect = *(volatile const float *) (uintptr_t) 0xbd07f8u;
    return *aspect > 0.1f && *aspect < 10.0f;
}

/* The edit box that has the focus: [0xdce474] (zero: none). Its layout object is at box+0x20: flags at +0x40 of it (0x100: rectangle valid),
   the rectangle at +0x44 as bottom, left, top, right, normalized: the window's pixels are the value divided by the factors [0xac0cb4]
   (horizontal) and [0xac0cb8] (vertical) and multiplied by the window's width and height; the vertical axis points up. */
int SRW_TextBox(int *left, int *top, int *right, int *bottom)
{
    uint32_t box = *(volatile const uint32_t *) (uintptr_t) 0xdce474u;
    const uint32_t *layout;
    float rect[4], kx, ky;
    int width = 0, height = 0;

    *left = *top = *right = *bottom = 0;
    if (box == 0) return 0;
    layout = (const uint32_t *) (uintptr_t) (box + 0x60);
    if ((layout[0] & 0x100) == 0) return 1;
    memcpy(rect, layout + 1, sizeof(rect));
    memcpy(&kx, (const void *) (uintptr_t) 0xac0cb4u, 4);
    memcpy(&ky, (const void *) (uintptr_t) 0xac0cb8u, 4);
    if (!(kx > 0.0f) || !(ky > 0.0f) || !SRW_WindowClientSize(0, &width, &height) || width <= 0 || height <= 0) return 1;
    *left = (int) (rect[1] / kx * (float) width);
    *right = (int) (rect[3] / kx * (float) width);
    *top = height - (int) (rect[2] / ky * (float) height);
    *bottom = height - (int) (rect[0] / ky * (float) height);
    return 1;
}

/* The client's aspect function (0x50f7c0, replaced in native_procs.llasm; it read the CVars widescreen and gxResolution, then the
   window's size): the window's aspect while the game UI is up (the window keeps the UI's aspect: user32.c fit_to_ui); while it is
   down (the login screens, and the moment a /reload or a change of the resolution builds it again) the canvas' aspect, and the window
   is resized to the whole canvas: the UI is built for it, and the resize that follows changes no aspect (no second reload). The float
   bits are returned; the stub pushes them on the x87 stack. */
uint32_t CCALL WoW_Aspect_c(void)
{
    float aspect = 4.0f / 3.0f, ui;
    int width = 0, height = 0;
    uint32_t bits;

    if (SRW_WindowAspect(&ui))
    {
        if (SRW_WindowClientSize(0, &width, &height) && width > 0 && height > 0) aspect = (float) width / (float) height;
    }
    else
    {
        SRW_CanvasSize(&width, &height);
        if (width > 0 && height > 0)
        {
            aspect = (float) width / (float) height;
            SRW_ResizeToCanvas();
        }
        else if (SRW_WindowClientSize(0, &width, &height) && width > 0 && height > 0) aspect = (float) width / (float) height;
    }
    memcpy(&bits, &aspect, 4);
    return bits;
}
