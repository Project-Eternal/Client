#ifndef SRW_OBJECT_H
#define SRW_OBJECT_H

#include <stdint.h>

/* Kernel objects the program refers to by handle: files, directory searches, events, threads... A handle is a small
   number that indexes a table; 0 and 0xffffffff are never valid. */

enum {
    SRW_OBJ_FILE = 1,
    SRW_OBJ_FIND,
    SRW_OBJ_EVENT,
    SRW_OBJ_MUTEX,
    SRW_OBJ_SEMAPHORE,
    SRW_OBJ_THREAD,
    SRW_OBJ_MAPPING,
    SRW_OBJ_REGKEY,
    SRW_OBJ_MODULE,
    SRW_OBJ_INTERNET
};

typedef struct SRW_Object {
    uint32_t type;
    uint32_t references;
    struct SRW_WaitNode *waiters;   /* the threads that wait for the object (kernel32-sync.c, under its lock) */
    /* frees whatever the object holds; the structure itself is freed afterwards */
    void (*destroy)(struct SRW_Object *object);
} SRW_Object;

#ifdef __cplusplus
extern "C" {
#endif

/* takes ownership of object (which must start with a SRW_Object) and returns its handle, or 0 if the table is full */
uint32_t SRW_ObjectCreate(SRW_Object *object);

/* the object behind a handle if it has the type asked for (0 = any), else NULL, with a reference that the caller gives back with
   SRW_ObjectUnref (NULL is accepted) */
SRW_Object *SRW_ObjectRef(uint32_t handle, uint32_t type);
void SRW_ObjectUnref(SRW_Object *object);
int SRW_ObjectIs(uint32_t handle, uint32_t type);

/* the handle stops being valid; the object is destroyed when its last reference goes */
int SRW_ObjectClose(uint32_t handle);

/* adds a second handle to the same object (DuplicateHandle) */
uint32_t SRW_ObjectDuplicate(uint32_t handle);

/* Windows path -> path in the virtual file system: '\' becomes '/', the install directory and drive letters are
   dropped, relative names are taken from the current directory */
void SRW_MapPath(const char *windows_path, char *out, unsigned int size);
/* the local root of the program in the module's file system (the install folder) */
#define SRW_LOCAL_ROOT "/" SRW_PROGRAM_NAME
/* a mapped path relative to the program's root ("/name/Data/x" -> "/Data/x"; other paths unchanged) */
const char *SRW_GameRelative(const char *mapped);

/* the current directory, in the form SRW_MapPath produces */
void SRW_GetCurrentDirectory(char *out, unsigned int size);
void SRW_SetCurrentDirectory(const char *mapped);

#ifdef __cplusplus
}
#endif

#endif
