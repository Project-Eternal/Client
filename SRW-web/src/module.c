/* modules: the program itself and the system libraries it asks for. The libraries are implemented by this runtime, so
   a handle is only a block of memory laid out like the header and export directory of a DLL, because the program
   resolves exports by walking that structure itself (its delay-load helper has its own GetProcAddress). */
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "srw_debuglog.h"
#include "srw.h"
#include "imports.h"
#include "srw_win32.h"

#define SRW_MAIN_MODULE 0x400000u

/* the libraries a program of this kind may open; anything else is reported as missing */
static const char *const known_libraries[] = {
    "kernel32", "user32", "gdi32", "advapi32", "shell32", "winmm", "version", "comctl32", "comdlg32", "imm32",
    "wsock32", "ws2_32", "wininet", "opengl32", "d3d9", "dsound", "ole32", "oleaut32", "msvcrt", "ntdll", "psapi", "shlwapi", NULL
};

typedef struct {
    char name[32];
    uint8_t *image;
} Module;

#define MAX_MODULES 32
static Module modules[MAX_MODULES];
static pthread_mutex_t module_lock = PTHREAD_MUTEX_INITIALIZER;

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xffff);
    put16(p + 2, v >> 16);
}

/* "KERNEL32.DLL", "C:\WINDOWS\system32\kernel32" -> "kernel32" */
static int normalize(const char *path, char *out, size_t size)
{
    const char *base = path, *p;
    size_t length;

    for (p = path; *p != 0; p++)
    {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    length = strlen(base);
    if (length >= 4 && strcasecmp(base + length - 4, ".dll") == 0) length -= 4;
    if (length == 0 || length >= size) return 0;
    memcpy(out, base, length);
    out[length] = 0;
    for (p = out; *p != 0; p++)
    {
        out[p - out] = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
    }
    return 1;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(SRW_DynamicExportNames[*(const uint32_t *)a], SRW_DynamicExportNames[*(const uint32_t *)b]);
}

static uint8_t *build_image(const char *name)
{
    uint32_t order[1024], count = 0, i;
    size_t strings = 0, size;
    uint8_t *image, *export, *functions, *names, *ordinals, *text;
    const uint32_t export_rva = 0x200;

    for (i = 0; i < SRW_DynamicExportCount && count < 1024; i++)
    {
        if (strcasecmp(SRW_DynamicExportModules[i], name) == 0)
        {
            order[count++] = i;
            strings += strlen(SRW_DynamicExportNames[i]) + 1;
        }
    }
    qsort(order, count, sizeof(order[0]), compare_names);

    size = export_rva + 40 + count * 10 + strings + strlen(name) + 8 + 64;
    image = calloc(1, size);
    if (image == NULL) return NULL;

    /* DOS header, PE signature, file header, optional header with the export directory entry */
    image[0] = 'M';
    image[1] = 'Z';
    put32(image + 0x3c, 0x80);
    memcpy(image + 0x80, "PE\0\0", 4);
    put16(image + 0x84, 0x14c);        /* i386 */
    put16(image + 0x94, 224);          /* size of the optional header */
    put16(image + 0x96, 0x2102);       /* a DLL */
    put16(image + 0x98, 0x10b);        /* PE32 */
    put32(image + 0x98 + 28, (uint32_t)(uintptr_t)image); /* image base */
    put32(image + 0x98 + 92, 16);      /* number of data directories */
    put32(image + 0x98 + 96, export_rva);
    put32(image + 0x98 + 100, 40);

    export = image + export_rva;
    functions = export + 40;
    names = functions + count * 4;
    ordinals = names + count * 4;
    text = ordinals + count * 2;

    put32(export + 12, (uint32_t)(text - image)); /* name of the library */
    strcpy((char *)text, name);
    text += strlen(name) + 1;
    put32(export + 16, 1);
    put32(export + 20, count);
    put32(export + 24, count);
    put32(export + 28, (uint32_t)(functions - image));
    put32(export + 32, (uint32_t)(names - image));
    put32(export + 36, (uint32_t)(ordinals - image));

    for (i = 0; i < count; i++)
    {
        const char *function = SRW_DynamicExportNames[order[i]];

        /* the address of the function is stored relative to the image; the program adds the image base back */
        put32(functions + i * 4, SRW_DynamicExportAddresses[order[i]] - (uint32_t)(uintptr_t)image);
        put32(names + i * 4, (uint32_t)(text - image));
        put16(ordinals + i * 2, i);
        strcpy((char *)text, function);
        text += strlen(function) + 1;
    }
    {
        /* the search the program performs, done here as a check of the layout */
        const uint8_t *pe = image + *(uint32_t *) (image + 0x3c);
        const uint8_t *dir = image + *(uint32_t *) (pe + 0x78);
        uint32_t n = *(uint32_t *) (dir + 0x18);
        const uint32_t *name_rvas = (const uint32_t *) (image + *(uint32_t *) (dir + 0x20));
        uint32_t k, bad = 0;

        for (k = 0; k < count; k++)
        {
            const char *want = SRW_DynamicExportNames[order[k]];
            int lo = 0, hi = (int) n - 1, found = 0;

            while (lo <= hi)
            {
                int mid = (lo + hi) / 2, c = strcmp(want, (const char *) image + name_rvas[mid]);

                if (c == 0) { found = 1; break; }
                if (c < 0) hi = mid - 1; else lo = mid + 1;
            }
            if (!found) { bad++; logprintf("Export check failed for %s", want); }
        }
        if (bad > 0 || SRW_LOG_ON(SRW_LOG_MODULES)) logprintf("%s: %u exports, %u failed the check, image at %x, directory at %x", name, (unsigned int) n, (unsigned int) bad, (unsigned int)(uintptr_t) image, (unsigned int)(uintptr_t) dir);
    }
    return image;
}

static Module *find_by_handle(uint32_t handle)
{
    int i;

    for (i = 0; i < MAX_MODULES; i++)
    {
        if (modules[i].image != NULL && (uint32_t)(uintptr_t)modules[i].image == handle) return &modules[i];
    }
    return NULL;
}

/* the handle of a library, creating it on first use; 0 if the library is unknown */
static uint32_t open_library(const char *path)
{
    char name[32];
    int i, known = 0;
    uint32_t handle = 0;

    if (!normalize(path, name, sizeof(name))) return 0;
    for (i = 0; known_libraries[i] != NULL; i++)
    {
        if (strcmp(known_libraries[i], name) == 0) known = 1;
    }
    if (!known) return 0;

    pthread_mutex_lock(&module_lock);
    for (i = 0; i < MAX_MODULES; i++)
    {
        if (modules[i].image != NULL && strcmp(modules[i].name, name) == 0)
        {
            handle = (uint32_t)(uintptr_t)modules[i].image;
            break;
        }
    }
    if (handle == 0)
    {
        for (i = 0; i < MAX_MODULES; i++)
        {
            if (modules[i].image == NULL)
            {
                modules[i].image = build_image(name);
                if (modules[i].image != NULL)
                {
                    strcpy(modules[i].name, name);
                    handle = (uint32_t)(uintptr_t)modules[i].image;
                }
                break;
            }
        }
    }
    pthread_mutex_unlock(&module_lock);
    return handle;
}

uint32_t CCALL GetModuleHandleA_c(const char *lpModuleName)
{
    uint32_t handle;

    if (lpModuleName == NULL) return SRW_MAIN_MODULE;
    handle = open_library(lpModuleName);
    if (handle == 0) SRW_SetLastError(126);
    return handle;
}

uint32_t CCALL LoadLibraryA_c(const char *lpLibFileName)
{
    uint32_t handle = open_library(lpLibFileName);

    if (handle == 0)
    {
        SRW_LOG(SRW_LOG_MODULES, "LoadLibrary(\"%s\") is not available", lpLibFileName);
        SRW_SetLastError(126);
    }
    return handle;
}

uint32_t CCALL FreeLibrary_c(uint32_t hLibModule)
{
    (void) hLibModule; /* the libraries stay: they are part of this runtime */
    return 1;
}

uint32_t CCALL GetProcAddress_c(uint32_t hModule, const char *lpProcName)
{
    Module *module;
    uint32_t i;

    if ((uintptr_t) lpProcName < 0x10000)
    {
        SRW_SetLastError(127);
        return 0;
    }
    pthread_mutex_lock(&module_lock);
    module = find_by_handle(hModule);
    pthread_mutex_unlock(&module_lock);
    if (module != NULL)
    {
        for (i = 0; i < SRW_DynamicExportCount; i++)
        {
            if (strcasecmp(SRW_DynamicExportModules[i], module->name) == 0 && strcmp(SRW_DynamicExportNames[i], lpProcName) == 0)
            {
                return SRW_DynamicExportAddresses[i];
            }
        }
    }
    SRW_LOG(SRW_LOG_MODULES, "GetProcAddress(\"%s\") is not available", lpProcName);
    SRW_SetLastError(127);
    return 0;
}

/* the guest address of a procedure of the dynamic exports, by name; 0 if there is none */
uint32_t SRW_FindProc(const char *name)
{
    uint32_t i;

    for (i = 0; i < SRW_DynamicExportCount; i++)
    {
        if (strcmp(SRW_DynamicExportNames[i], name) == 0) return SRW_DynamicExportAddresses[i];
    }
    return 0;
}
