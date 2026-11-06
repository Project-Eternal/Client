#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "srw_object.h"
#include "srw_win32.h"

#define MAX_HANDLES 4096

static SRW_Object *table[MAX_HANDLES];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

/* handles look like 0x104, 0x108...: multiples of four, as the program expects from a Windows handle */
#define INDEX_TO_HANDLE(i) (((uint32_t)(i) + 64u) * 4u)
#define HANDLE_TO_INDEX(h) ((int)(((h) / 4u) - 64u))

uint32_t SRW_ObjectCreate(SRW_Object *object)
{
    int i;
    uint32_t handle = 0;

    object->references = 1;

    pthread_mutex_lock(&table_lock);
    for (i = 0; i < MAX_HANDLES; i++)
    {
        if (table[i] == NULL)
        {
            table[i] = object;
            handle = INDEX_TO_HANDLE(i);
            break;
        }
    }
    pthread_mutex_unlock(&table_lock);

    return handle;
}

/* the object behind a handle with a reference for the caller (SRW_ObjectUnref when done): a CloseHandle by another thread meanwhile
   only takes the handle away, the object stays until the last reference goes, as Windows keeps an object that a call uses */
SRW_Object *SRW_ObjectRef(uint32_t handle, uint32_t type)
{
    SRW_Object *object = NULL;
    int i;

    if (handle % 4u != 0 || handle < 64u * 4u) return NULL;
    i = HANDLE_TO_INDEX(handle);
    if (i < 0 || i >= MAX_HANDLES) return NULL;

    pthread_mutex_lock(&table_lock);
    object = table[i];
    if (object != NULL && type != 0 && object->type != type) object = NULL;
    if (object != NULL) __atomic_add_fetch(&object->references, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&table_lock);
    return object;
}

static void release(SRW_Object *object)
{
    if (__atomic_sub_fetch(&object->references, 1, __ATOMIC_SEQ_CST) == 0)
    {
        if (object->destroy != NULL) object->destroy(object);
        free(object);
    }
}

void SRW_ObjectUnref(SRW_Object *object)
{
    if (object != NULL) release(object);
}

/* whether a handle is an object of the type (0 = any) now */
int SRW_ObjectIs(uint32_t handle, uint32_t type)
{
    SRW_Object *object = SRW_ObjectRef(handle, type);

    SRW_ObjectUnref(object);
    return object != NULL;
}

int SRW_ObjectClose(uint32_t handle)
{
    SRW_Object *object;
    int i;

    if (handle % 4u != 0 || handle < 64u * 4u) return 0;
    i = HANDLE_TO_INDEX(handle);
    if (i < 0 || i >= MAX_HANDLES) return 0;

    pthread_mutex_lock(&table_lock);
    object = table[i];
    table[i] = NULL;
    pthread_mutex_unlock(&table_lock);

    if (object == NULL) return 0;
    release(object);
    return 1;
}

uint32_t SRW_ObjectDuplicate(uint32_t handle)
{
    SRW_Object *object = SRW_ObjectRef(handle, 0);
    int i;
    uint32_t result = 0;

    if (object == NULL) return 0;

    pthread_mutex_lock(&table_lock);
    for (i = 0; i < MAX_HANDLES; i++)
    {
        if (table[i] == NULL)
        {
            table[i] = object;   /* (the reference taken above is the new handle's) */
            result = INDEX_TO_HANDLE(i);
            break;
        }
    }
    pthread_mutex_unlock(&table_lock);
    if (result == 0) SRW_ObjectUnref(object);
    return result;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* paths */

static char current_directory[260] = "/";
static pthread_mutex_t path_lock = PTHREAD_MUTEX_INITIALIZER;

void SRW_GetCurrentDirectory(char *out, unsigned int size)
{
    pthread_mutex_lock(&path_lock);
    strncpy(out, current_directory, size - 1);
    out[size - 1] = 0;
    pthread_mutex_unlock(&path_lock);
}

const char *SRW_GameRelative(const char *mapped)
{
    size_t n = strlen(SRW_LOCAL_ROOT);

    if (strncmp(mapped, SRW_LOCAL_ROOT, n) == 0 && (mapped[n] == '/' || mapped[n] == 0)) return mapped[n] == 0 ? "/" : mapped + n;
    return mapped;
}

void SRW_SetCurrentDirectory(const char *mapped)
{
    pthread_mutex_lock(&path_lock);
    strncpy(current_directory, SRW_GameRelative(mapped), sizeof(current_directory) - 1);
    current_directory[sizeof(current_directory) - 1] = 0;
    pthread_mutex_unlock(&path_lock);
}

static void map_game_path(const char *windows_path, char *out, unsigned int size);

/* the local path of a Windows path: the program's tree (its install folder = every drive) lives under /name in the file system of the module */
void SRW_MapPath(const char *windows_path, char *out, unsigned int size)
{
    char relative[520];
    int n;

    map_game_path(windows_path, relative, sizeof(relative));
    n = snprintf(out, size, "%s%s", SRW_LOCAL_ROOT, strcmp(relative, "/") == 0 ? "" : relative);
    if (n < 0 || (unsigned int) n >= size) out[size - 1] = 0;
}

/* a Windows path as a path relative to the program's root ("/", "/Data/x.bin"...) */
static void map_game_path(const char *windows_path, char *out, unsigned int size)
{
    char buffer[520];
    unsigned int i, o;
    const char *p;
    int absolute;

    /* normalize the separators */
    for (i = 0; windows_path[i] != 0 && i < sizeof(buffer) - 1; i++)
    {
        buffer[i] = (windows_path[i] == '\\') ? '/' : windows_path[i];
    }
    buffer[i] = 0;
    p = buffer;

    absolute = 0;
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':')
    {
        p += 2; /* a drive letter: every drive is the same tree */
        absolute = (p[0] == '/');
    }
    else if (p[0] == '/')
    {
        absolute = 1;
    }

    /* the install directory is the root of the virtual file system */
    if (absolute)
    {
        const char *install = SRW_INSTALL_DIR;
        const char *rest = install + 2; /* skip "C:" */
        char normalized[64];

        for (i = 0; rest[i] != 0 && i < sizeof(normalized) - 1; i++)
        {
            normalized[i] = (rest[i] == '\\') ? '/' : rest[i];
        }
        normalized[i] = 0;
        if (strncasecmp(p, normalized, i) == 0 && (p[i] == '/' || p[i] == 0))
        {
            p += i;
        }
    }

    o = 0;
    if (!absolute)
    {
        char cwd[260];

        SRW_GetCurrentDirectory(cwd, sizeof(cwd));
        for (i = 0; cwd[i] != 0 && o < size - 1; i++) out[o++] = cwd[i];
        if (o == 0 || out[o - 1] != '/')
        {
            if (o < size - 1) out[o++] = '/';
        }
    }
    else if (o < size - 1)
    {
        /* keep a single leading slash */
        out[o++] = '/';
        while (*p == '/') p++;
    }

    /* the rest, dropping "." components and collapsing repeated slashes */
    while (*p != 0 && o < size - 1)
    {
        if (p[0] == '/' && (o > 0 && out[o - 1] == '/'))
        {
            p++;
            continue;
        }
        if (p[0] == '.' && (p[1] == '/' || p[1] == 0) && (o == 0 || out[o - 1] == '/'))
        {
            p += (p[1] == '/') ? 2 : 1;
            continue;
        }
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0) && (o == 0 || out[o - 1] == '/'))
        {
            /* the parent directory; the root has none */
            while (o > 1 && out[o - 1] == '/') o--;
            while (o > 1 && out[o - 1] != '/') o--;
            if (o == 0 || out[o - 1] != '/') out[o++] = '/';
            p += (p[2] == '/') ? 3 : 2;
            continue;
        }
        out[o++] = *p++;
    }
    /* no trailing slash except for the root */
    while (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
}
