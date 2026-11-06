/* kernel32: time, system information, locale and string functions, global memory, resources and the like */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <wctype.h>

#include "srw_debuglog.h"
#include "srw.h"
#include "imports.h"
#include "srw_win32.h"

/* ------------------------------------------------------------------------------------------------------------------ */
/* time */

#pragma pack(push, 1)
typedef struct {
    uint16_t wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SystemTime;
#pragma pack(pop)

static uint64_t filetime_get(const void *p)
{
    const uint32_t *w = p;

    return ((uint64_t) w[1] << 32) | w[0];
}

static void filetime_put(void *p, uint64_t value)
{
    uint32_t *w = p;

    w[0] = (uint32_t) value;
    w[1] = (uint32_t) (value >> 32);
}

#define UNIX_EPOCH_TICKS 116444736000000000ull

static void fill_system_time(SystemTime *out, time_t seconds, uint32_t milliseconds)
{
    struct tm tm;

    gmtime_r(&seconds, &tm);
    out->wYear = (uint16_t) (tm.tm_year + 1900);
    out->wMonth = (uint16_t) (tm.tm_mon + 1);
    out->wDayOfWeek = (uint16_t) tm.tm_wday;
    out->wDay = (uint16_t) tm.tm_mday;
    out->wHour = (uint16_t) tm.tm_hour;
    out->wMinute = (uint16_t) tm.tm_min;
    out->wSecond = (uint16_t) tm.tm_sec;
    out->wMilliseconds = (uint16_t) milliseconds;
}

/* the time zone of the program is always UTC: the browser does not tell the recompiled program otherwise */
uint32_t CCALL GetSystemTime_c(void *lpSystemTime)
{
    struct timespec now;

    clock_gettime(CLOCK_REALTIME, &now);
    fill_system_time(lpSystemTime, now.tv_sec, (uint32_t) (now.tv_nsec / 1000000));
    return 0;
}

uint32_t CCALL GetLocalTime_c(void *lpSystemTime)
{
    return GetSystemTime_c(lpSystemTime);
}

uint32_t CCALL GetTimeZoneInformation_c(void *lpTimeZoneInformation)
{
    memset(lpTimeZoneInformation, 0, 172);
    return 0; /* TIME_ZONE_ID_UNKNOWN */
}

uint32_t CCALL FileTimeToSystemTime_c(void *lpFileTime, void *lpSystemTime)
{
    uint64_t ticks = filetime_get(lpFileTime);

    if (ticks < UNIX_EPOCH_TICKS) return 0;
    ticks -= UNIX_EPOCH_TICKS;
    fill_system_time(lpSystemTime, (time_t) (ticks / 10000000ull), (uint32_t) ((ticks / 10000ull) % 1000ull));
    return 1;
}

uint32_t CCALL SystemTimeToFileTime_c(void *lpSystemTime, void *lpFileTime)
{
    const SystemTime *st = lpSystemTime;
    struct tm tm;
    time_t seconds;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min = st->wMinute;
    tm.tm_sec = st->wSecond;
    seconds = timegm(&tm);
    filetime_put(lpFileTime, UNIX_EPOCH_TICKS + (uint64_t) seconds * 10000000ull + (uint64_t) st->wMilliseconds * 10000ull);
    return 1;
}

uint32_t CCALL FileTimeToLocalFileTime_c(void *lpFileTime, void *lpLocalFileTime)
{
    filetime_put(lpLocalFileTime, filetime_get(lpFileTime));
    return 1;
}

uint32_t CCALL CompareFileTime_c(void *lpFileTime1, void *lpFileTime2)
{
    uint64_t a = filetime_get(lpFileTime1), b = filetime_get(lpFileTime2);

    return (a < b) ? 0xffffffffu : (a > b) ? 1u : 0u;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* system information and memory */

#pragma pack(push, 1)
typedef struct {
    uint16_t wProcessorArchitecture;
    uint16_t wReserved;
    uint32_t dwPageSize;
    uint32_t lpMinimumApplicationAddress;
    uint32_t lpMaximumApplicationAddress;
    uint32_t dwActiveProcessorMask;
    uint32_t dwNumberOfProcessors;
    uint32_t dwProcessorType;
    uint32_t dwAllocationGranularity;
    uint16_t wProcessorLevel;
    uint16_t wProcessorRevision;
} SystemInfo;

typedef struct {
    uint32_t BaseAddress;
    uint32_t AllocationBase;
    uint32_t AllocationProtect;
    uint32_t RegionSize;
    uint32_t State;
    uint32_t Protect;
    uint32_t Type;
} MemoryBasicInformation;
#pragma pack(pop)

uint32_t CCALL GetSystemInfo_c(void *lpSystemInfo)
{
    SystemInfo *info = lpSystemInfo;

    memset(info, 0, sizeof(*info));
    info->dwPageSize = 4096;
    info->lpMinimumApplicationAddress = 0x10000;
    info->lpMaximumApplicationAddress = 0x7ffeffff;
    info->dwActiveProcessorMask = 1;
    info->dwNumberOfProcessors = 1;
    info->dwProcessorType = 586;
    info->dwAllocationGranularity = 0x10000;
    info->wProcessorLevel = 6;
    return 0;
}

uint32_t CCALL VirtualQuery_c(void *lpAddress, void *lpBuffer, uint32_t dwLength)
{
    MemoryBasicInformation *info = lpBuffer;
    uint32_t address = (uint32_t) (uintptr_t) lpAddress;

    if (dwLength < sizeof(*info)) return 0;
    /* everything is committed and accessible; the program image is reported as one allocation */
    info->BaseAddress = address & ~0xfffu;
    info->AllocationBase = (address >= 0x400000 && address < 0x1000000) ? 0x400000 : info->BaseAddress;
    info->AllocationProtect = 0x40;
    info->RegionSize = 0x1000;
    info->State = 0x1000;
    info->Protect = 0x40;
    info->Type = (info->AllocationBase == 0x400000) ? 0x1000000 : 0x20000;
    return sizeof(*info);
}

uint32_t CCALL IsBadReadPtr_c(void *lp, uint32_t ucb)
{
    return lp == NULL && ucb != 0;
}

uint32_t CCALL IsBadWritePtr_c(void *lp, uint32_t ucb)
{
    return lp == NULL && ucb != 0;
}

uint32_t CCALL IsBadCodePtr_c(void *lpfn)
{
    return lpfn == NULL;
}

uint32_t CCALL IsProcessorFeaturePresent_c(uint32_t ProcessorFeature)
{
    extern uint32_t x86_cpu_features;   /* (llasm_sse.c, SRW_CpuConfigure) */

    /* PF_MMX_INSTRUCTIONS_AVAILABLE 3, PF_XMMI_INSTRUCTIONS_AVAILABLE 6, PF_XMMI64_INSTRUCTIONS_AVAILABLE 10: as cpuid says */
    if (ProcessorFeature == 3 || ProcessorFeature == 6 || ProcessorFeature == 10) return x86_cpu_features != 0;
    return 0;
}

uint32_t CCALL FlushInstructionCache_c(uint32_t hProcess, uint32_t lpBaseAddress, uint32_t dwSize)
{
    (void) hProcess;
    (void) lpBaseAddress;
    (void) dwSize;
    return 1;
}

/* a global or local handle is the pointer itself, as for GMEM_FIXED */
uint32_t CCALL GlobalAlloc_c(uint32_t uFlags, uint32_t dwBytes)
{
    void *memory = ((uFlags & 0x40) != 0) ? calloc(1, dwBytes ? dwBytes : 1) : malloc(dwBytes ? dwBytes : 1);

    if (memory == NULL) SRW_SetLastError(8);
    return (uint32_t) (uintptr_t) memory;
}

uint32_t CCALL GlobalFree_c(uint32_t hMem)
{
    free((void *) (uintptr_t) hMem);
    return 0;
}

uint32_t CCALL GlobalLock_c(uint32_t hMem)
{
    return hMem;
}

uint32_t CCALL GlobalUnlock_c(uint32_t hMem)
{
    (void) hMem;
    return 0;
}

uint32_t CCALL LocalFree_c(uint32_t hMem)
{
    free((void *) (uintptr_t) hMem);
    return 0;
}

uint32_t CCALL MulDiv_c(uint32_t nNumber, uint32_t nNumerator, uint32_t nDenominator)
{
    int64_t product, half;
    int32_t denominator = (int32_t) nDenominator;

    if (denominator == 0) return 0xffffffffu;
    product = (int64_t) (int32_t) nNumber * (int32_t) nNumerator;
    half = (denominator < 0 ? -(int64_t) denominator : denominator) / 2;
    product += (product < 0) != (denominator < 0) ? -half : half;
    return (uint32_t) (int32_t) (product / denominator);
}

uint32_t CCALL lstrcpynA_c(void *lpString1, const char *lpString2, uint32_t iMaxLength)
{
    char *destination = lpString1;

    if (iMaxLength > 0)
    {
        strncpy(destination, lpString2, iMaxLength - 1);
        destination[iMaxLength - 1] = 0;
    }
    return (uint32_t) (uintptr_t) lpString1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* diagnostics and things that do not apply */

uint32_t CCALL OutputDebugStringA_c(const char *lpOutputString)
{
    SRW_LOG(SRW_LOG_CLIENT, "debug: %s", lpOutputString);
    return 0;
}

uint32_t CCALL FormatMessageA_c(uint32_t dwFlags, void *lpSource, uint32_t dwMessageId, uint32_t dwLanguageId, void *lpBuffer, uint32_t nSize, void *Arguments)
{
    char text[64];
    uint32_t length;

    (void) lpSource;
    (void) dwLanguageId;
    (void) Arguments;

    length = (uint32_t) snprintf(text, sizeof(text), "Windows error %u", (unsigned int) dwMessageId);
    if ((dwFlags & 0x100) != 0) /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
    {
        char *memory = malloc(length + 1);

        if (memory == NULL) return 0;
        memcpy(memory, text, length + 1);
        *(uint32_t *) lpBuffer = (uint32_t) (uintptr_t) memory;
        return length;
    }
    if (nSize <= length) return 0;
    memcpy(lpBuffer, text, length + 1);
    return length;
}

uint32_t CCALL RaiseException_c(uint32_t dwExceptionCode, uint32_t dwExceptionFlags, uint32_t nNumberOfArguments, void *lpArguments)
{
    (void) dwExceptionFlags;

    if (dwExceptionCode == 0x406d1388u)
    {
        /* MSVC's way to name a thread for the debugger (arguments: 0x1000, the name, the thread id, flags): the program's own handler
           ignores it, the call returns */
        const uint32_t *info = lpArguments;

        if (nNumberOfArguments >= 2 && info != NULL && info[1] != 0) SRW_LOG(SRW_LOG_THREADS, "Thread name \"%s\"", (const char *) (uintptr_t) info[1]);
        return 0;
    }

    /* there is no structured exception handling: an exception the program raises itself ends the thread */
    warnprintf("The program raised exception %08x", (unsigned int) dwExceptionCode);
    SRW_DumpProcTrace();
    SRW_ThreadExit();
    return 0;
}

uint32_t CCALL RtlUnwind_c(void *TargetFrame, void *TargetIp, void *ExceptionRecord, uint32_t ReturnValue)
{
    (void) TargetFrame;
    (void) TargetIp;
    (void) ExceptionRecord;
    (void) ReturnValue;
    return 0;
}

uint32_t CCALL FatalAppExitA_c(uint32_t uAction, const char *lpMessageText)
{
    (void) uAction;
    warnprintf("Fatal error: %s", lpMessageText);
    SRW_ThreadExit();
    return 0;
}

uint32_t CCALL SetConsoleCtrlHandler_c(void *HandlerRoutine, uint32_t Add)
{
    (void) HandlerRoutine;
    (void) Add;
    return 1;
}

uint32_t CCALL SetStdHandle_c(uint32_t nStdHandle, uint32_t hHandle)
{
    (void) nStdHandle;
    (void) hHandle;
    return 1;
}

uint32_t CCALL Beep_c(uint32_t dwFreq, uint32_t dwDuration)
{
    (void) dwFreq;
    (void) dwDuration;
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* resources: the resource section of the program is not part of the recompiled image */

uint32_t CCALL FindResourceA_c(uint32_t hModule, const char *lpName, const char *lpType)
{
    (void) hModule;
    (void) lpName;
    (void) lpType;
    SRW_SetLastError(1813); /* ERROR_RESOURCE_DATA_NOT_FOUND */
    return 0;
}

uint32_t CCALL LoadResource_c(uint32_t hModule, uint32_t hResInfo)
{
    (void) hModule;
    (void) hResInfo;
    return 0;
}

uint32_t CCALL LockResource_c(uint32_t hResData)
{
    (void) hResData;
    return 0;
}

uint32_t CCALL SizeofResource_c(uint32_t hModule, uint32_t hResInfo)
{
    (void) hModule;
    (void) hResInfo;
    return 0;
}

uint32_t CCALL FreeResource_c(uint32_t hResData)
{
    (void) hResData;
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* locale: the program always runs as English (United States) */

#define LOCALE_ENGLISH_US 0x409u

uint32_t CCALL GetUserDefaultLCID_c(void)
{
    return LOCALE_ENGLISH_US;
}

uint32_t CCALL IsValidLocale_c(uint32_t Locale, uint32_t dwFlags)
{
    (void) dwFlags;
    return Locale == LOCALE_ENGLISH_US || Locale == 0x800 || Locale == 0x400;
}

uint32_t CCALL EnumSystemLocalesA_c(void *lpLocaleEnumProc, uint32_t dwFlags)
{
    static char name[] = "00000409";
    uint32_t argument = (uint32_t) (uintptr_t) name;

    (void) dwFlags;
    SRW_CallGuest((uint32_t) (uintptr_t) lpLocaleEnumProc, 1, &argument);
    return 1;
}

static const char *locale_text(uint32_t type)
{
    switch (type & 0xffff)
    {
    case 0x01: return "0409";
    case 0x02: return "English (United States)";
    case 0x03: return "ENU";
    case 0x04: return "English";
    case 0x05: return "1";
    case 0x06: return "United States";
    case 0x07: return "USA";
    case 0x08: return "United States";
    case 0x09: return "0409";
    case 0x0a: return "1";
    case 0x0b: return "437";
    case 0x0c: return ",";
    case 0x0d: return "1";
    case 0x0e: return ".";
    case 0x0f: return ",";
    case 0x10: return "3;0";
    case 0x11: return "2";
    case 0x12: return "1";
    case 0x14: return "$";
    case 0x15: return "USD";
    case 0x16: return ".";
    case 0x17: return ",";
    case 0x18: return "3;0";
    case 0x19: return "2";
    case 0x1a: return "0";
    case 0x1b: return "0";
    case 0x1d: return "/";
    case 0x1e: return ":";
    case 0x1f: return "M/d/yyyy";
    case 0x20: return "dddd, MMMM dd, yyyy";
    case 0x21: return "0";
    case 0x22: return "0";
    case 0x23: return "0";
    case 0x24: return "1";
    case 0x25: return "0";
    case 0x26: return "0";
    case 0x27: return "0";
    case 0x28: return "AM";
    case 0x29: return "PM";
    case 0x1001: return "English";
    case 0x1002: return "United States";
    case 0x1004: return "65001";
    default: return "";
    }
}

uint32_t CCALL GetLocaleInfoA_c(uint32_t Locale, uint32_t LCType, void *lpLCData, uint32_t cchData)
{
    const char *text = locale_text(LCType);
    uint32_t length = (uint32_t) strlen(text) + 1;

    (void) Locale;
    if (cchData == 0) return length;
    if (cchData < length)
    {
        SRW_SetLastError(122);
        return 0;
    }
    memcpy(lpLCData, text, length);
    return length;
}

uint32_t CCALL GetLocaleInfoW_c(uint32_t Locale, uint32_t LCType, void *lpLCData, uint32_t cchData)
{
    const char *text = locale_text(LCType);
    uint32_t length = (uint32_t) strlen(text) + 1, i;
    uint16_t *out = lpLCData;

    (void) Locale;
    if (cchData == 0) return length;
    if (cchData < length)
    {
        SRW_SetLastError(122);
        return 0;
    }
    for (i = 0; i < length; i++) out[i] = (uint8_t) text[i];
    return length;
}

uint32_t CCALL IsValidCodePage_c(uint32_t CodePage)
{
    return CodePage == 437 || CodePage == 1252 || CodePage == 65001 || CodePage == 850 || CodePage == 0;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* strings */

static int fold(int c, uint32_t flags)
{
    return ((flags & 1) != 0) ? towlower((wint_t) c) : c; /* NORM_IGNORECASE */
}

uint32_t CCALL CompareStringA_c(uint32_t Locale, uint32_t dwCmpFlags, const char *lpString1, uint32_t cchCount1, const char *lpString2, uint32_t cchCount2)
{
    uint32_t i, n1 = cchCount1, n2 = cchCount2;

    (void) Locale;
    if ((int32_t) n1 < 0) n1 = (uint32_t) strlen(lpString1);
    if ((int32_t) n2 < 0) n2 = (uint32_t) strlen(lpString2);
    for (i = 0; i < n1 && i < n2; i++)
    {
        int a = fold((uint8_t) lpString1[i], dwCmpFlags), b = fold((uint8_t) lpString2[i], dwCmpFlags);

        if (a != b) return (a < b) ? 1 : 3;
    }
    return (n1 == n2) ? 2 : (n1 < n2) ? 1 : 3;
}

uint32_t CCALL CompareStringW_c(uint32_t Locale, uint32_t dwCmpFlags, const uint16_t *lpString1, uint32_t cchCount1, const uint16_t *lpString2, uint32_t cchCount2)
{
    uint32_t i, n1 = cchCount1, n2 = cchCount2;

    (void) Locale;
    if ((int32_t) n1 < 0) for (n1 = 0; lpString1[n1] != 0; n1++) {}
    if ((int32_t) n2 < 0) for (n2 = 0; lpString2[n2] != 0; n2++) {}
    for (i = 0; i < n1 && i < n2; i++)
    {
        int a = fold(lpString1[i], dwCmpFlags), b = fold(lpString2[i], dwCmpFlags);

        if (a != b) return (a < b) ? 1 : 3;
    }
    return (n1 == n2) ? 2 : (n1 < n2) ? 1 : 3;
}

static int map_char(int c, uint32_t flags)
{
    if ((flags & 0x100) != 0) return towlower((wint_t) c); /* LCMAP_LOWERCASE */
    if ((flags & 0x200) != 0) return towupper((wint_t) c); /* LCMAP_UPPERCASE */
    return c;
}

uint32_t CCALL LCMapStringA_c(uint32_t Locale, uint32_t dwMapFlags, const char *lpSrcStr, uint32_t cchSrc, void *lpDestStr, uint32_t cchDest)
{
    uint32_t n = cchSrc, i;
    char *out = lpDestStr;

    (void) Locale;
    if ((int32_t) n < 0) n = (uint32_t) strlen(lpSrcStr) + 1;
    if (cchDest == 0) return n;
    if (cchDest < n)
    {
        SRW_SetLastError(122);
        return 0;
    }
    for (i = 0; i < n; i++) out[i] = (char) map_char((uint8_t) lpSrcStr[i], dwMapFlags);
    return n;
}

uint32_t CCALL LCMapStringW_c(uint32_t Locale, uint32_t dwMapFlags, const uint16_t *lpSrcStr, uint32_t cchSrc, void *lpDestStr, uint32_t cchDest)
{
    uint32_t n = cchSrc, i;
    uint16_t *out = lpDestStr;

    (void) Locale;
    if ((int32_t) n < 0)
    {
        for (n = 0; lpSrcStr[n] != 0; n++) {}
        n++;
    }
    if (cchDest == 0) return n;
    if (cchDest < n)
    {
        SRW_SetLastError(122);
        return 0;
    }
    for (i = 0; i < n; i++) out[i] = (uint16_t) map_char(lpSrcStr[i], dwMapFlags);
    return n;
}

static uint16_t char_type(int c)
{
    uint16_t type = 0;

    if (iswupper((wint_t) c)) type |= 0x1;
    if (iswlower((wint_t) c)) type |= 0x2;
    if (iswdigit((wint_t) c)) type |= 0x4;
    if (c == ' ' || (c >= 9 && c <= 13)) type |= 0x8;
    if (iswpunct((wint_t) c)) type |= 0x10;
    if (c < 32 || c == 127) type |= 0x20;
    if (c == ' ' || c == 9) type |= 0x40;
    if (iswxdigit((wint_t) c)) type |= 0x80;
    if (iswalpha((wint_t) c)) type |= 0x100;
    return type;
}

uint32_t CCALL GetStringTypeA_c(uint32_t Locale, uint32_t dwInfoType, const char *lpSrcStr, uint32_t cchSrc, void *lpCharType)
{
    uint32_t n = cchSrc, i;
    uint16_t *out = lpCharType;

    (void) Locale;
    if (dwInfoType != 1) return 0;
    if ((int32_t) n < 0) n = (uint32_t) strlen(lpSrcStr) + 1;
    for (i = 0; i < n; i++) out[i] = char_type((uint8_t) lpSrcStr[i]);
    return 1;
}

uint32_t CCALL GetStringTypeW_c(uint32_t dwInfoType, const uint16_t *lpSrcStr, uint32_t cchSrc, void *lpCharType)
{
    uint32_t n = cchSrc, i;
    uint16_t *out = lpCharType;

    if (dwInfoType != 1) return 0;
    if ((int32_t) n < 0)
    {
        for (n = 0; lpSrcStr[n] != 0; n++) {}
        n++;
    }
    for (i = 0; i < n; i++) out[i] = char_type(lpSrcStr[i]);
    return 1;
}

/* code page conversion: UTF-8 (65001, and CP_ACP = 0, which is UTF-8 here, see GetACP) is converted, every other code page is taken as Latin-1.
   A character beyond the Basic Multilingual Plane is a surrogate pair in UTF-16, as on Windows; an invalid sequence gives U+FFFD. */
static int is_utf8_page(uint32_t CodePage)
{
    return CodePage == 65001 || CodePage == 0;
}

uint32_t CCALL MultiByteToWideChar_c(uint32_t CodePage, uint32_t dwFlags, const char *lpMultiByteStr, uint32_t cbMultiByte, void *lpWideCharStr, uint32_t cchWideChar)
{
    const uint8_t *in = (const uint8_t *) lpMultiByteStr;
    uint16_t *out = lpWideCharStr;
    uint32_t n = cbMultiByte, i = 0, o = 0;

    (void) dwFlags;
    if ((int32_t) n < 0) n = (uint32_t) strlen(lpMultiByteStr) + 1;

    while (i < n)
    {
        uint32_t c = in[i++], units = 1, unit[2];

        if (is_utf8_page(CodePage) && c >= 0x80)
        {
            uint32_t extra = (c >= 0xf0 && c < 0xf8) ? 3 : (c >= 0xe0) ? 2 : 1, min = (extra == 3) ? 0x10000 : (extra == 2) ? 0x800 : 0x80, k;
            int valid = c >= 0xc2 && c < 0xf8;

            c &= (0x3f >> extra);
            for (k = 0; k < extra && valid; k++)
            {
                if (i >= n || (in[i] & 0xc0) != 0x80) valid = 0;
                else c = (c << 6) | (in[i++] & 0x3f);
            }
            if (!valid || c < min || c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) c = 0xfffd;
        }
        if (c > 0xffff)
        {
            c -= 0x10000;
            unit[0] = 0xd800 | (c >> 10);
            unit[1] = 0xdc00 | (c & 0x3ff);
            units = 2;
        }
        else unit[0] = c;
        if (cchWideChar != 0)
        {
            if (o + units > cchWideChar)
            {
                SRW_SetLastError(122);
                return 0;
            }
            out[o] = (uint16_t) unit[0];
            if (units == 2) out[o + 1] = (uint16_t) unit[1];
        }
        o += units;
    }
    return o;
}

uint32_t CCALL WideCharToMultiByte_c(uint32_t CodePage, uint32_t dwFlags, const uint16_t *lpWideCharStr, uint32_t cchWideChar, void *lpMultiByteStr,
                                     uint32_t cbMultiByte, void *lpDefaultChar, void *lpUsedDefaultChar)
{
    uint8_t *out = lpMultiByteStr;
    uint32_t n = cchWideChar, i, o = 0;

    (void) dwFlags;
    (void) lpDefaultChar;
    if (lpUsedDefaultChar != NULL) *(uint32_t *) lpUsedDefaultChar = 0;
    if ((int32_t) n < 0)
    {
        for (n = 0; lpWideCharStr[n] != 0; n++) {}
        n++;
    }
    for (i = 0; i < n; i++)
    {
        uint32_t c = lpWideCharStr[i];
        uint8_t bytes[4];
        uint32_t count;

        if (is_utf8_page(CodePage) && c >= 0x80)
        {
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && lpWideCharStr[i + 1] >= 0xdc00 && lpWideCharStr[i + 1] < 0xe000)
                c = 0x10000 + ((c - 0xd800) << 10) + (lpWideCharStr[++i] - 0xdc00);
            else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;   /* a lone surrogate */
            if (c < 0x800)
            {
                bytes[0] = 0xc0 | (c >> 6);
                bytes[1] = 0x80 | (c & 0x3f);
                count = 2;
            }
            else if (c < 0x10000)
            {
                bytes[0] = 0xe0 | (c >> 12);
                bytes[1] = 0x80 | ((c >> 6) & 0x3f);
                bytes[2] = 0x80 | (c & 0x3f);
                count = 3;
            }
            else
            {
                bytes[0] = 0xf0 | (c >> 18);
                bytes[1] = 0x80 | ((c >> 12) & 0x3f);
                bytes[2] = 0x80 | ((c >> 6) & 0x3f);
                bytes[3] = 0x80 | (c & 0x3f);
                count = 4;
            }
        }
        else
        {
            bytes[0] = (c > 0xff) ? '?' : (uint8_t) c;
            count = 1;
        }
        if (cbMultiByte != 0)
        {
            uint32_t j;

            if (o + count > cbMultiByte)
            {
                SRW_SetLastError(122);
                return 0;
            }
            for (j = 0; j < count; j++) out[o + j] = bytes[j];
        }
        o += count;
    }
    return o;
}


/* the program asks (through its own GetProcAddress) whether it is being debugged */
uint32_t CCALL IsDebuggerPresent_c(void)
{
    return 0;
}
