/* the wide (UTF-16) forms of the functions the program calls when it runs on "NT": each converts to the narrow form and
   calls it, so there is one implementation of files, directories and the registry */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "srw.h"
#include "imports.h"
#include "srw_object.h"
#include "srw_win32.h"

/* UTF-16 to UTF-8 */
static char *narrow(const uint16_t *text, char *out, size_t size)
{
    size_t o = 0;

    if (text == NULL) return NULL;
    for (; *text != 0 && o + 4 < size; text++)
    {
        uint32_t c = *text;

        if (c >= 0xd800 && c < 0xdc00 && text[1] >= 0xdc00 && text[1] < 0xe000)
        {
            c = 0x10000 + ((c - 0xd800) << 10) + (text[1] - 0xdc00);
            text++;
        }
        if (c < 0x80)
        {
            out[o++] = (char) c;
        }
        else if (c < 0x800)
        {
            out[o++] = (char) (0xc0 | (c >> 6));
            out[o++] = (char) (0x80 | (c & 0x3f));
        }
        else if (c < 0x10000)
        {
            out[o++] = (char) (0xe0 | (c >> 12));
            out[o++] = (char) (0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char) (0x80 | (c & 0x3f));
        }
        else
        {
            out[o++] = (char) (0xf0 | (c >> 18));
            out[o++] = (char) (0x80 | ((c >> 12) & 0x3f));
            out[o++] = (char) (0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char) (0x80 | (c & 0x3f));
        }
    }
    out[o] = 0;
    return out;
}

/* UTF-8 to UTF-16; returns the number of characters without the terminator; stops when size (in characters) is used up */
static size_t widen(const char *text, uint16_t *out, size_t size)
{
    size_t o = 0;
    const uint8_t *p = (const uint8_t *) text;

    while (*p != 0 && o + 2 < size)
    {
        uint32_t c = *p++;

        if (c >= 0xf0 && p[0] != 0)
        {
            c = ((c & 7) << 18) | ((p[0] & 0x3f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            p += 3;
        }
        else if (c >= 0xe0 && p[0] != 0)
        {
            c = ((c & 15) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f);
            p += 2;
        }
        else if (c >= 0xc0 && p[0] != 0)
        {
            c = ((c & 31) << 6) | (p[0] & 0x3f);
            p += 1;
        }
        if (c >= 0x10000)
        {
            c -= 0x10000;
            out[o++] = (uint16_t) (0xd800 + (c >> 10));
            out[o++] = (uint16_t) (0xdc00 + (c & 0x3ff));
        }
        else
        {
            out[o++] = (uint16_t) c;
        }
    }
    out[o] = 0;
    return o;
}

#define NARROW(name, w) char name[1040]; narrow(w, name, sizeof(name))

/* ------------------------------------------------------------------------------------------------------------------ */
/* files */

uint32_t CCALL CreateFileW_c(const uint16_t *lpFileName, uint32_t dwDesiredAccess, uint32_t dwShareMode, void *lpSecurityAttributes,
                             uint32_t dwCreationDisposition, uint32_t dwFlagsAndAttributes, uint32_t hTemplateFile)
{
    NARROW(name, lpFileName);
    return CreateFileA_c(name, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

uint32_t CCALL GetFileAttributesW_c(const uint16_t *lpFileName)
{
    NARROW(name, lpFileName);
    return GetFileAttributesA_c(name);
}

uint32_t CCALL SetFileAttributesW_c(const uint16_t *lpFileName, uint32_t dwFileAttributes)
{
    (void) lpFileName;
    (void) dwFileAttributes;
    return 1;
}

uint32_t CCALL DeleteFileW_c(const uint16_t *lpFileName)
{
    NARROW(name, lpFileName);
    return DeleteFileA_c(name);
}

uint32_t CCALL MoveFileW_c(const uint16_t *lpExistingFileName, const uint16_t *lpNewFileName)
{
    char from[520], to[520];
    NARROW(a, lpExistingFileName);
    NARROW(b, lpNewFileName);

    SRW_MapPath(a, from, sizeof(from));
    SRW_MapPath(b, to, sizeof(to));
    if (rename(from, to) != 0)
    {
        SRW_SetLastError(2);
        return 0;
    }
    return 1;
}

uint32_t CCALL CopyFileW_c(const uint16_t *lpExistingFileName, const uint16_t *lpNewFileName, uint32_t bFailIfExists)
{
    char from[520], to[520], buffer[8192];
    FILE *in, *out;
    size_t got;
    NARROW(a, lpExistingFileName);
    NARROW(b, lpNewFileName);

    SRW_MapPath(a, from, sizeof(from));
    SRW_MapPath(b, to, sizeof(to));
    if (bFailIfExists)
    {
        struct stat st;

        if (stat(to, &st) == 0)
        {
            SRW_SetLastError(80);
            return 0;
        }
    }
    in = fopen(from, "rb");
    if (in == NULL)
    {
        SRW_SetLastError(2);
        return 0;
    }
    out = fopen(to, "wb");
    if (out == NULL)
    {
        fclose(in);
        SRW_SetLastError(5);
        return 0;
    }
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) fwrite(buffer, 1, got, out);
    fclose(in);
    fclose(out);
    return 1;
}

uint32_t CCALL CreateDirectoryW_c(const uint16_t *lpPathName, void *lpSecurityAttributes)
{
    NARROW(name, lpPathName);
    return CreateDirectoryA_c(name, lpSecurityAttributes);
}

uint32_t CCALL RemoveDirectoryW_c(const uint16_t *lpPathName)
{
    char path[520];
    NARROW(name, lpPathName);

    SRW_MapPath(name, path, sizeof(path));
    if (rmdir(path) != 0)
    {
        SRW_SetLastError(2);
        return 0;
    }
    return 1;
}

uint32_t CCALL SetCurrentDirectoryW_c(const uint16_t *lpPathName)
{
    NARROW(name, lpPathName);
    return SetCurrentDirectoryA_c(name);
}

uint32_t CCALL GetCurrentDirectoryW_c(uint32_t nBufferLength, void *lpBuffer)
{
    char text[520];
    size_t length;
    uint16_t wide[520];

    (void) GetCurrentDirectoryA_c(sizeof(text), text);
    length = widen(text, wide, sizeof(wide) / sizeof(wide[0]));
    if (nBufferLength <= length) return (uint32_t) length + 1;
    memcpy(lpBuffer, wide, (length + 1) * 2);
    return (uint32_t) length;
}

uint32_t CCALL GetFullPathNameW_c(const uint16_t *lpFileName, uint32_t nBufferLength, void *lpBuffer, void *lpFilePart)
{
    char text[1040];
    uint16_t wide[1040];
    size_t length, i;
    NARROW(name, lpFileName);

    (void) GetFullPathNameA_c(name, sizeof(text), text, NULL);
    length = widen(text, wide, sizeof(wide) / sizeof(wide[0]));
    if (nBufferLength <= length) return (uint32_t) length + 1;
    memcpy(lpBuffer, wide, (length + 1) * 2);
    if (lpFilePart != NULL)
    {
        uint16_t *buffer = lpBuffer;

        *(uint32_t *) lpFilePart = 0;
        for (i = length; i > 0; i--)
        {
            if (buffer[i - 1] == '\\')
            {
                *(uint32_t *) lpFilePart = (uint32_t) (uintptr_t) (buffer + i);
                break;
            }
        }
    }
    return (uint32_t) length;
}

uint32_t CCALL GetModuleFileNameW_c(uint32_t hModule, void *lpFilename, uint32_t nSize)
{
    char text[520];
    uint16_t wide[520];
    size_t length;

    (void) GetModuleFileNameA_c(hModule, text, sizeof(text));
    length = widen(text, wide, sizeof(wide) / sizeof(wide[0]));
    if (nSize <= length) length = nSize > 0 ? nSize - 1 : 0;
    if (nSize > 0)
    {
        memcpy(lpFilename, wide, length * 2);
        ((uint16_t *) lpFilename)[length] = 0;
    }
    return (uint32_t) length;
}

uint32_t CCALL GetDriveTypeW_c(const uint16_t *lpRootPathName)
{
    (void) lpRootPathName;
    return 3;
}

uint32_t CCALL GetDiskFreeSpaceExW_c(const uint16_t *lpDirectoryName, void *lpFreeBytesAvailable, void *lpTotalNumberOfBytes, void *lpTotalNumberOfFreeBytes)
{
    uint64_t total = 64ull << 30, available = 32ull << 30;

    (void) lpDirectoryName;
    if (lpFreeBytesAvailable != NULL) memcpy(lpFreeBytesAvailable, &available, 8);
    if (lpTotalNumberOfBytes != NULL) memcpy(lpTotalNumberOfBytes, &total, 8);
    if (lpTotalNumberOfFreeBytes != NULL) memcpy(lpTotalNumberOfFreeBytes, &available, 8);
    return 1;
}

uint32_t CCALL CreateProcessW_c(const uint32_t *args)
{
    (void) args;
    SRW_SetLastError(5); /* other programs cannot be started from a page */
    return 0;
}

/* WIN32_FIND_DATAW: 44 bytes of numbers like the narrow form, then the names in UTF-16 */
static void widen_find_data(const uint8_t *narrow_data, uint8_t *out)
{
    memcpy(out, narrow_data, 44);
    widen((const char *) (narrow_data + 44), (uint16_t *) (out + 44), 260);
    memset(out + 44 + 520, 0, 28);
}

uint32_t CCALL FindFirstFileW_c(const uint16_t *lpFileName, void *lpFindFileData)
{
    uint8_t data[320];
    uint32_t handle;
    NARROW(name, lpFileName);

    handle = FindFirstFileA_c(name, data);
    if (handle != 0xffffffffu) widen_find_data(data, lpFindFileData);
    return handle;
}

uint32_t CCALL FindNextFileW_c(uint32_t hFindFile, void *lpFindFileData)
{
    uint8_t data[320];

    if (!FindNextFileA_c(hFindFile, data)) return 0;
    widen_find_data(data, lpFindFileData);
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the registry: strings are kept narrow */

uint32_t CCALL RegOpenKeyExW_c(uint32_t hKey, const uint16_t *lpSubKey, uint32_t ulOptions, uint32_t samDesired, void *phkResult)
{
    NARROW(name, lpSubKey);
    return RegOpenKeyExA_c(hKey, lpSubKey != NULL ? name : NULL, ulOptions, samDesired, phkResult);
}

uint32_t CCALL RegCreateKeyExW_c(const uint32_t *args)
{
    uint32_t copy[9];
    char name[1040];

    memcpy(copy, args, sizeof(copy));
    narrow((const uint16_t *) (uintptr_t) args[1], name, sizeof(name));
    copy[1] = (uint32_t) (uintptr_t) name;
    return RegCreateKeyExA_c(copy);
}

uint32_t CCALL RegQueryValueExW_c(uint32_t hKey, const uint16_t *lpValueName, void *lpReserved, void *lpType, void *lpData, void *lpcbData)
{
    uint8_t temp[4096];
    uint32_t size = sizeof(temp), type = 0, status;
    uint32_t *out_size = lpcbData;
    NARROW(name, lpValueName);

    status = RegQueryValueExA_c(hKey, lpValueName != NULL ? name : NULL, lpReserved, &type, temp, &size);
    if (status != 0) return status;
    if (lpType != NULL) *(uint32_t *) lpType = type;
    if (type == 1 || type == 2) /* strings are wide for the caller */
    {
        uint16_t wide[4096];
        size_t length = widen((const char *) temp, wide, sizeof(wide) / sizeof(wide[0]));
        uint32_t bytes = (uint32_t) (length + 1) * 2;

        if (lpData != NULL && out_size != NULL)
        {
            if (*out_size < bytes)
            {
                *out_size = bytes;
                return 234;
            }
            memcpy(lpData, wide, bytes);
        }
        if (out_size != NULL) *out_size = bytes;
        return 0;
    }
    if (lpData != NULL && out_size != NULL)
    {
        if (*out_size < size)
        {
            *out_size = size;
            return 234;
        }
        memcpy(lpData, temp, size);
    }
    if (out_size != NULL) *out_size = size;
    return 0;
}

uint32_t CCALL RegSetValueExW_c(uint32_t hKey, const uint16_t *lpValueName, uint32_t Reserved, uint32_t dwType, void *lpData, uint32_t cbData)
{
    char text[4096];
    NARROW(name, lpValueName);

    if ((dwType == 1 || dwType == 2) && lpData != NULL)
    {
        narrow((const uint16_t *) lpData, text, sizeof(text));
        return RegSetValueExA_c(hKey, lpValueName != NULL ? name : NULL, Reserved, dwType, text, (uint32_t) strlen(text) + 1);
    }
    return RegSetValueExA_c(hKey, lpValueName != NULL ? name : NULL, Reserved, dwType, lpData, cbData);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* user32 */

uint32_t CCALL MessageBoxW_c(uint32_t hWnd, const uint16_t *lpText, const uint16_t *lpCaption, uint32_t uType)
{
    char text[2048], caption[256];

    narrow(lpText, text, sizeof(text));
    narrow(lpCaption, caption, sizeof(caption));
    return MessageBoxA_c(hWnd, text, caption, uType);
}

uint32_t CCALL SetWindowTextA_c(uint32_t hWnd, const char *lpString);   // not in the generated header when the program does not import it

uint32_t CCALL SetWindowTextW_c(uint32_t hWnd, const uint16_t *lpString)
{
    NARROW(text, lpString);
    return SetWindowTextA_c(hWnd, text);
}

uint32_t CCALL DrawTextExW_c(uint32_t hdc, const uint16_t *lpchText, uint32_t cchText, void *lprc, uint32_t format, void *lpdtp)
{
    (void) hdc; (void) lpchText; (void) cchText; (void) lprc; (void) format; (void) lpdtp;
    return 0;
}
