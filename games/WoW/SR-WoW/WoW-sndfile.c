/* The files of the 3.3.5 client's sounds, read whole at their first read and served from memory: the sound system reads streams in
   pieces of 2 KB from its own threads, and every piece through Storm decodes its sector again.

   The client opens a sound file with Storm itself (a Storm handle) and gives FMOD Ex four callbacks (open 0x87b300, close 0x878170,
   read 0x878190, seek 0x878200: the fields of the CREATESOUNDEXINFO built at 0x87efaa) that only call Storm with that handle. Those
   four procedures are replaced by stubs (native_procs.llasm) that call the functions here; the original bodies stay at addresses in
   their padding. Open calls the original (the handle is the Storm one, so the original close works for it) and remembers the handle;
   the file is read whole at its first read or seek, and later reads and seeks are served from memory.
   How it is read (/Port/soundreader.txt, setting soundReader):
     client (the default)  the client's own read callback once for the whole file (Storm: SFileSetFilePointer 0x421bb0, SFileReadFile
                           0x422530): the files as the game sees them, loose files of patch/ and the archives in the game's order
     native                the port's MPQ reader (WoW-mpq.cpp, the big lock released meanwhile): only files the archives can give,
                           the rest goes to the original code
   What is not handled (0xffffffff) goes on in the original code, which is also where everything goes for a handle that is not in the
   registry. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

#include "srw_debuglog.h"
#include "WoW.h"

extern int WoW_MpqFind(const char *name, int64_t *size);
extern uint8_t *WoW_MpqReadFile(const char *name, int64_t *size);

#define ORIGINAL_OPEN 0x87b388u   /* the original callbacks, in the padding after the replaced procedures (native_procs.llasm) */
#define ORIGINAL_READ 0x8781f0u
#define ORIGINAL_SEEK 0x878218u
#define FMOD_OK 0u
#define FMOD_ERR_FILE_EOF 0x16u   /* what the original read answers for the end of the file */
#define NOT_HANDLED 0xffffffffu

typedef struct SoundFile {
    struct SoundFile *next;
    uint32_t handle;          /* the Storm handle the client and FMOD know the file by */
    char name[260];
    uint32_t expected_size;   /* what Storm said */
    uint8_t *data;
    int64_t size;
    int64_t position;
    int state;                /* 0 not read yet, 1 in memory, 2 the archives cannot give it (the original code serves it) */
    int loading, closed;      /* being read with the big lock released; closed meanwhile (freed when the read is over) */
    uint64_t reads;
} SoundFile;

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static int native_reader = -1;   /* /Port/soundreader.txt "native": WoW-mpq.cpp; else the client's Storm */

static int use_native_reader(void)
{
    if (native_reader < 0)
    {
        FILE *f = fopen("/Port/soundreader.txt", "r");
        char text[16] = "";

        if (f != NULL)
        {
            if (fgets(text, sizeof(text), f) == NULL) text[0] = 0;
            fclose(f);
        }
        native_reader = strncmp(text, "native", 6) == 0;
        logprintf("Sound files are read by %s", native_reader ? "the port's MPQ reader (native)" : "the client's Storm");
    }
    return native_reader;
}
static SoundFile *registry;
static uint64_t native_reads, native_bytes, loaded_files;

static SoundFile *find_file(uint32_t handle)
{
    SoundFile *f;

    pthread_mutex_lock(&registry_lock);
    for (f = registry; f != NULL && f->handle != handle; f = f->next) {}
    pthread_mutex_unlock(&registry_lock);
    return f;   /* (only the thread that uses a file, FMOD's, touches it; the entry stays until its close) */
}

/* the open callback: FMOD_RESULT (const char *name, int unicode, unsigned *filesize, void **handle, void **userdata) */
uint32_t CCALL WoW_SndOpen_c(uint32_t name, uint32_t filesize, uint32_t handle, uint32_t userdata)
{
    uint32_t args[5], result;

    args[0] = name; args[1] = 0; args[2] = filesize; args[3] = handle; args[4] = userdata;
    result = SRW_CallGuest(ORIGINAL_OPEN, 5, args);
    if (result == FMOD_OK && name != 0 && handle != 0)
    {
        int64_t size = 0;
        const char *text = (const char *) (uintptr_t) name;

        /* FMOD is given "<number><spaces><path>" (the number tells the sound object, the client formats it at 0x87f057) */
        while (*text >= '0' && *text <= '9') text++;
        while (*text == ' ') text++;

        if (!use_native_reader() || WoW_MpqFind(text, &size))
        {
            SoundFile *f = calloc(1, sizeof(*f));

            if (f != NULL)
            {
                f->handle = *(uint32_t *) (uintptr_t) handle;
                strncpy(f->name, text, sizeof(f->name) - 1);
                f->expected_size = filesize != 0 ? *(uint32_t *) (uintptr_t) filesize : (uint32_t) size;
                pthread_mutex_lock(&registry_lock);
                f->next = registry;
                registry = f;
                pthread_mutex_unlock(&registry_lock);
                SRW_LOG(SRW_LOG_SOUND, "Sound: %s opened (%u bytes)", f->name, (unsigned int) f->expected_size);
            }
        }
        else SRW_LOG(SRW_LOG_SOUND, "Sound: %s opened, the archives cannot give it: the client reads it", text);
    }
    return result;
}

/* reads the file whole (the native reader with the big lock released meanwhile); false when that is not possible */
static int load(SoundFile *f)
{
    double start = emscripten_get_now();
    int64_t size = 0;
    uint8_t *data;

    if (f->state == 1) return 1;
    if (f->state == 2) return 0;
    if (!use_native_reader())
    {
        /* the client's own callbacks: to the start, then the whole file in one read */
        uint32_t got = 0, args[5];

        data = malloc(f->expected_size ? f->expected_size : 1);
        args[0] = f->handle; args[1] = 0; args[2] = 0;
        if (data != NULL && SRW_CallGuest(ORIGINAL_SEEK, 3, args) == FMOD_OK)
        {
            uint32_t result;

            args[0] = f->handle; args[1] = (uint32_t) (uintptr_t) data; args[2] = f->expected_size; args[3] = (uint32_t) (uintptr_t) &got; args[4] = 0;
            result = SRW_CallGuest(ORIGINAL_READ, 5, args);
            if (result != FMOD_OK && result != FMOD_ERR_FILE_EOF) got = 0;
        }
        size = got;
        if (got == 0 && f->expected_size != 0)
        {
            free(data);
            data = NULL;
        }
        if (data == NULL || got != f->expected_size)
        {
            args[0] = f->handle; args[1] = 0; args[2] = 0;
            SRW_CallGuest(ORIGINAL_SEEK, 3, args);   /* (the original code goes on from the start of the file) */
        }
    }
    else
    {
        f->loading = 1;
        SRW_GilRelease();
        data = WoW_MpqReadFile(f->name, &size);
        SRW_GilAcquire();
        f->loading = 0;
    }
    if (f->closed)
    {
        free(data);
        free(f);
        return -1;   /* the file was closed while it was read: the caller has nothing to serve */
    }
    if (data == NULL || (uint32_t) size != f->expected_size)
    {
        SRW_LOG(SRW_LOG_SOUND, "Sound: %s could not be read whole (%s): the client reads it in pieces", f->name, data == NULL ? "no data" : "another size");
        free(data);
        f->state = 2;
        return 0;
    }
    f->data = data;
    f->size = size;
    f->state = 1;
    loaded_files++;
    SRW_LOG(SRW_LOG_SOUND, "Sound: %s read whole: %u bytes in %.0f ms", f->name, (unsigned int) size, emscripten_get_now() - start);
    return 1;
}

/* the read callback: FMOD_RESULT (void *handle, void *buffer, unsigned sizebytes, unsigned *bytesread, void *userdata) */
uint32_t CCALL WoW_SndRead_c(uint32_t handle, uint32_t buffer, uint32_t bytes, uint32_t bytes_read)
{
    SoundFile *f = find_file(handle);
    int64_t available;
    uint32_t count;

    if (f == NULL || load(f) != 1) return NOT_HANDLED;
    available = f->size - f->position;
    if (available < 0) available = 0;
    count = (uint64_t) available < bytes ? (uint32_t) available : bytes;
    memcpy((void *) (uintptr_t) buffer, f->data + f->position, count);
    f->position += count;
    if (bytes_read != 0) *(uint32_t *) (uintptr_t) bytes_read = count;
    native_reads++;
    native_bytes += count;
    f->reads++;
    {
        static double reported;
        double now = emscripten_get_now();

        if (SRW_LOG_ON(SRW_LOG_SOUND) && now - reported > 5000.0)
        {
            reported = now;
            logprintf("Sound: files read whole: %llu files read, %llu reads of %llu bytes served from memory", (unsigned long long) loaded_files,
                      (unsigned long long) native_reads, (unsigned long long) native_bytes);
        }
    }
    return count < bytes ? FMOD_ERR_FILE_EOF : FMOD_OK;
}

/* the seek callback: FMOD_RESULT (void *handle, unsigned pos, void *userdata) */
uint32_t CCALL WoW_SndSeek_c(uint32_t handle, uint32_t position)
{
    SoundFile *f = find_file(handle);

    if (f == NULL || load(f) != 1) return NOT_HANDLED;
    f->position = position;
    return FMOD_OK;
}

/* the close callback: FMOD_RESULT (void *handle, void *userdata); the original closes the Storm handle */
uint32_t CCALL WoW_SndClose_c(uint32_t handle)
{
    SoundFile **link, *f = NULL;

    pthread_mutex_lock(&registry_lock);
    for (link = &registry; *link != NULL; link = &(*link)->next)
    {
        if ((*link)->handle == handle)
        {
            f = *link;
            *link = f->next;
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    if (f != NULL)
    {
        if (f->loading) f->closed = 1;   /* (load frees it) */
        else
        {
            free(f->data);
            free(f);
        }
    }
    return NOT_HANDLED;
}
