/* Sound for the client: the part of FMOD 3.75 (fmod.dll, the FSOUND functions) that the client uses, made of two decoders (dr_wav and dr_mp3,
   third_party/dr_libs), a mixer and the Web Audio output of the page (Module.srwAudio).

   The client plays everything as streams: FSOUND_Stream_Open with a name that its own file functions understand (FSOUND_File_SetCallbacks),
   FSOUND_Stream_Play(Ex) on a channel, then volume, frequency, 3D position, an end callback and time.
   Three threads work for the streams: the loader reads the files and calls the end callbacks (it needs the big lock), the decoder keeps
   decoded sound ahead for each stream, and the mixer makes the output without waiting for anything and writes it into a ring buffer of
   the shared memory that the page reads from a script processor node. */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <emscripten.h>

#include "WoW.h"
#include "srw_debuglog.h"

/* WoW-mpq.cpp: the files of the archives, read without the client */
int WoW_MpqFind(const char *name, int64_t *size);
uint8_t *WoW_MpqReadFile(const char *name, int64_t *size);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include "third_party/dr_libs/dr_wav.h"
#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "third_party/dr_libs/dr_mp3.h"
#pragma GCC diagnostic pop

/* constants of fmod.h */
#define FSOUND_LOOP_NORMAL 0x00000002u
#define FSOUND_2D 0x00002000u
#define FSOUND_LOADMEMORY 0x00008000u
#define FSOUND_FREE (-1)
#define FSOUND_ALL (-3)

enum {
    FMOD_ERR_NONE, FMOD_ERR_BUSY, FMOD_ERR_UNINITIALIZED, FMOD_ERR_INIT, FMOD_ERR_ALLOCATED, FMOD_ERR_PLAY, FMOD_ERR_OUTPUT_FORMAT,
    FMOD_ERR_COOPERATIVELEVEL, FMOD_ERR_CREATEBUFFER, FMOD_ERR_FILE_NOTFOUND, FMOD_ERR_FILE_FORMAT, FMOD_ERR_FILE_BAD, FMOD_ERR_MEMORY,
    FMOD_ERR_VERSION, FMOD_ERR_INVALID_PARAM, FMOD_ERR_NO_EAX, FMOD_ERR_CHANNEL_ALLOC
};

#define MAX_CHANNELS 64
#define OUT_RING_FRAMES 16384      /* power of two: the counters wrap around with the index */
#define MIX_CHUNK 512
#define TARGET_FILL 6144           /* frames that the mixer keeps ready ahead of the page */

typedef struct Stream Stream;

struct Stream {
    Stream *next;
    unsigned int mode;
    /* the source: memory or the file functions of the client */
    int from_memory;
    struct RawFile *raw;        /* the file in the cache of read files that the memory belongs to (NULL: the client's own memory) */
    int loading;                /* a large file that the loader thread reads into memory (the guest handle is open until then) */
    int native;                 /* ... from the archives itself (WoW-mpq.cpp), without the client: no guest handle */
    const uint8_t *memory;
    int64_t memory_size, position, size;
    uint32_t handle;   /* what the open function of the client returned */
    /* the decoder */
    int format;        /* 1 wav, 2 mp3 */
    drwav wav;
    drmp3 mp3;
    uint32_t rate;
    uint32_t channels;
    uint32_t length_ms;
    int ready;                  /* the decoder is set up (the decoder thread does that for a stream from the client's files) */
    int failed;                 /* it could not be: it ends as soon as it is played */
    /* the decoded sound (stereo, at the rate of the stream) */
    float *ring;
    uint32_t ring_frames;
    _Atomic uint64_t ring_write, ring_read;
    _Atomic int decoder_done;   /* everything is decoded (and no loop is left) */
    int loops_left;             /* -1 forever */
    /* playback and what the threads tell each other (audio_lock) */
    int channel;                /* -1 when it does not play */
    uint64_t consumed;          /* frames of the source that were played, since the start of the pass */
    double phase;               /* fraction of a frame to the next one, at the play position */
    int busy;                   /* the decoder thread works on it */
    int closing;
    int seek_ms;                /* -1: none */
    int end_pending;
    uint32_t end_callback, end_data;
    float min_distance, max_distance;
    uint32_t sample_tag;        /* FSOUND_Stream_GetSample answers the address of this */
    char name[96];              /* for the log */
    double opened_ms;           /* when it was opened (the log shows how long it took until it was ready) */
};

typedef struct {
    uint32_t generation;
    Stream *stream;
    int paused, muted, reserved;
    int volume;                 /* 0 to 255 */
    float frequency;
    float position[3], velocity[3];
    float gain_left, gain_right;   /* what the last chunk ended with */
} Channel;

static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int initialized, running, have_output, out_rate = 44100, last_error, max_channels = 32;
static uint32_t file_open, file_close, file_read, file_seek, file_tell;
static Channel channels[MAX_CHANNELS];
static Stream *streams;
static float listener_position[3], listener_velocity[3], listener_front[3] = { 0, 0, 1 }, listener_top[3] = { 0, 1, 0 };
static float rolloff_factor = 1.0f;
static int master_volume = 255;
static float out_ring[OUT_RING_FRAMES * 2];
static _Atomic uint32_t out_write, out_read;
static pthread_t mixer_thread, decoder_thread, loader_thread;
static int underruns;

static float bits_to_float(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, 4);
    return f;
}

static void set_error(int error)
{
    last_error = error;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* The files of the sounds are read whole and kept by name: the decoders then work from memory and never wait for the client's file
   functions (which need the big lock) or for the network, and a sound that is played again needs no reading at all. */

#define SMALL_FILE (1024 * 1024)          /* read when it is opened, on the caller's thread: ready when it is played */
#define RAW_CACHE_BYTES (64 * 1024 * 1024)

typedef struct RawFile {
    struct RawFile *next;
    char key[128];
    uint8_t *data;
    int64_t size;
    int refs;
    uint64_t used;
} RawFile;

static pthread_mutex_t raw_lock = PTHREAD_MUTEX_INITIALIZER;
static RawFile *raw_files;
static int64_t raw_bytes;
static uint64_t raw_clock;

static void raw_key(const char *name, char *key, size_t size)
{
    size_t i;

    for (i = 0; name[i] != 0 && i + 1 < size; i++) key[i] = (char) (name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : (name[i] == '/' ? '\\' : name[i]));
    key[i] = 0;
}

/* the cached file with a reference, or NULL */
static RawFile *raw_get(const char *key)
{
    RawFile *r;

    pthread_mutex_lock(&raw_lock);
    for (r = raw_files; r != NULL && strcmp(r->key, key) != 0; r = r->next) { }
    if (r != NULL)
    {
        r->refs++;
        r->used = ++raw_clock;
    }
    pthread_mutex_unlock(&raw_lock);
    return r;
}

/* takes the data (malloc'ed) into the cache and returns it with a reference; files nobody uses are dropped beyond the limit */
static RawFile *raw_put(const char *key, uint8_t *data, int64_t size)
{
    RawFile *r = calloc(1, sizeof(*r)), **link;

    if (r == NULL) return NULL;
    snprintf(r->key, sizeof(r->key), "%s", key);
    r->data = data;
    r->size = size;
    r->refs = 1;
    pthread_mutex_lock(&raw_lock);
    r->used = ++raw_clock;
    r->next = raw_files;
    raw_files = r;
    raw_bytes += size;
    while (raw_bytes > RAW_CACHE_BYTES)
    {
        RawFile **oldest = NULL;

        for (link = &raw_files; *link != NULL; link = &(*link)->next)
        {
            if ((*link)->refs == 0 && (oldest == NULL || (*link)->used < (*oldest)->used)) oldest = link;
        }
        if (oldest == NULL) break;
        {
            RawFile *victim = *oldest;

            *oldest = victim->next;
            raw_bytes -= victim->size;
            free(victim->data);
            free(victim);
        }
    }
    pthread_mutex_unlock(&raw_lock);
    return r;
}

static void raw_release(RawFile *r)
{
    if (r == NULL) return;
    pthread_mutex_lock(&raw_lock);
    r->refs--;
    pthread_mutex_unlock(&raw_lock);
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the client's functions, called with the big lock (the thread may hold it already) */

static uint32_t call_guest(uint32_t address, unsigned int count, const uint32_t *arguments)
{
    int held = SRW_GilHeld();
    uint32_t result;

    if (!held) SRW_GilAcquire();
    result = SRW_CallGuest(address, count, arguments);
    if (!held) SRW_GilRelease();
    return result;
}

/* the source of a stream: bytes for the decoders */

static size_t source_read(void *user, void *buffer, size_t count)
{
    Stream *s = user;

    if (s->from_memory)
    {
        int64_t left = s->memory_size - s->position;

        if (left < 0) left = 0;
        if ((int64_t) count > left) count = (size_t) left;
        memcpy(buffer, s->memory + s->position, count);
        s->position += (int64_t) count;
        return count;
    }
    else
    {
        uint32_t arguments[3] = { (uint32_t) (uintptr_t) buffer, (uint32_t) count, s->handle };
        int got = (int) call_guest(file_read, 3, arguments);

        if (got < 0) got = 0;
        s->position += got;
        return (size_t) got;
    }
}

static int source_seek_to(Stream *s, int64_t position)
{
    if (position < 0) position = 0;
    if (s->from_memory)
    {
        s->position = position > s->memory_size ? s->memory_size : position;
        return 1;
    }
    else
    {
        uint32_t arguments[3] = { s->handle, (uint32_t) position, 0 /* SEEK_SET */ };

        call_guest(file_seek, 3, arguments);
        {
            uint32_t tell[1] = { s->handle };

            s->position = file_tell != 0 ? (int32_t) call_guest(file_tell, 1, tell) : position;
        }
        return 1;
    }
}

static int source_seek(void *user, int offset, int origin)
{
    Stream *s = user;

    /* origin: 0 from the start, 1 from where it is, 2 from the end */
    return source_seek_to(s, origin == 0 ? offset : (origin == 1 ? s->position + offset : s->size + offset));
}

static int source_tell(void *user, int64_t *cursor)
{
    *cursor = ((Stream *) user)->position;
    return 1;
}

static size_t wav_read_proc(void *user, void *buffer, size_t count) { return source_read(user, buffer, count); }
static drwav_bool32 wav_seek_proc(void *user, int offset, drwav_seek_origin origin) { return source_seek(user, offset, origin == DRWAV_SEEK_SET ? 0 : (origin == DRWAV_SEEK_CUR ? 1 : 2)); }
static drwav_bool32 wav_tell_proc(void *user, drwav_int64 *cursor) { return source_tell(user, (int64_t *) cursor); }
static size_t mp3_read_proc(void *user, void *buffer, size_t count) { return source_read(user, buffer, count); }
static drmp3_bool32 mp3_seek_proc(void *user, int offset, drmp3_seek_origin origin) { return source_seek(user, offset, origin == DRMP3_SEEK_SET ? 0 : (origin == DRMP3_SEEK_CUR ? 1 : 2)); }
static drmp3_bool32 mp3_tell_proc(void *user, drmp3_int64 *cursor) { return source_tell(user, (int64_t *) cursor); }

/* ------------------------------------------------------------------------------------------------------------------ */
/* streams */

/* the length of an mp3 in milliseconds from its size and the bit rate of the first frame (what FMOD does without MPEGACCURATE) */
static uint32_t estimate_mp3_ms(Stream *s)
{
    uint8_t header[10], block[4096];
    int64_t start = 0, size = s->size;
    static const int rates1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int rates2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    size_t got, i;

    if (size <= 0) return 0;
    source_seek_to(s, 0);
    if (source_read(s, header, 10) == 10 && memcmp(header, "ID3", 3) == 0)
    {
        start = 10 + (((int64_t) (header[6] & 0x7f) << 21) | ((int64_t) (header[7] & 0x7f) << 14) | ((int64_t) (header[8] & 0x7f) << 7) | (header[9] & 0x7f));
    }
    source_seek_to(s, start);
    got = source_read(s, block, sizeof(block));
    for (i = 0; i + 4 <= got; i++)
    {
        if (block[i] == 0xFF && (block[i + 1] & 0xE0) == 0xE0)
        {
            int version = (block[i + 1] >> 3) & 3, layer = (block[i + 1] >> 1) & 3, index = block[i + 2] >> 4;
            int kbps = version == 3 ? rates1[index] : rates2[index];

            if (version != 1 && layer == 1 && kbps > 0)
            {
                source_seek_to(s, 0);
                return (uint32_t) ((size - start) * 8 / kbps);
            }
        }
    }
    source_seek_to(s, 0);
    return 0;
}

static void stream_free(Stream *s)
{
    if (s->format == 1) drwav_uninit(&s->wav);
    else if (s->format == 2) drmp3_uninit(&s->mp3);
    raw_release(s->raw);
    if (!s->from_memory && s->handle != 0 && file_close != 0)
    {
        uint32_t arguments[1] = { s->handle };

        call_guest(file_close, 1, arguments);
    }
    free(s->ring);
    free(s);
}

/* decodes up to `wanted` frames into the ring; returns how many. Called by one thread at a time for a stream. */
static uint32_t stream_decode(Stream *s, uint32_t wanted)
{
    /* on the heap: a large thread-local array would move the image of the program in memory */
    float *scratch = malloc(2048 * 8 * sizeof(float));
    uint32_t done = 0;

    while (done < wanted && !atomic_load(&s->decoder_done))
    {
        uint32_t want = wanted - done, got, i;
        uint64_t write = atomic_load_explicit(&s->ring_write, memory_order_relaxed);

        if (want > 2048) want = 2048;
        if (s->format == 1) got = (uint32_t) drwav_read_pcm_frames_f32(&s->wav, want, scratch);
        else got = (uint32_t) drmp3_read_pcm_frames_f32(&s->mp3, want, scratch);
        for (i = 0; i < got; i++)
        {
            float *frame = s->ring + ((write + i) % s->ring_frames) * 2;

            frame[0] = scratch[i * s->channels];
            frame[1] = s->channels > 1 ? scratch[i * s->channels + 1] : scratch[i * s->channels];
        }
        atomic_store_explicit(&s->ring_write, write + got, memory_order_release);
        done += got;
        if (got < want)
        {
            /* the end of the sound: again from the start, or the last of it */
            if (s->loops_left != 0)
            {
                int ok;

                if (s->format == 1) ok = drwav_seek_to_pcm_frame(&s->wav, 0);
                else ok = drmp3_seek_to_pcm_frame(&s->mp3, 0);
                if (ok)
                {
                    if (s->loops_left > 0) s->loops_left--;
                    if (got == 0) break;   /* a sound of no length must not make a loop of no end */
                    continue;
                }
            }
            atomic_store(&s->decoder_done, 1);
        }
    }
    free(scratch);
    return done;
}

/* reads the client's open file whole (from the start) into a new buffer, in pieces of `chunk` bytes; NULL when it fails */
static uint8_t *read_whole(uint32_t handle, int64_t size, uint32_t chunk)
{
    uint8_t *data = malloc(size > 0 ? (size_t) size : 1);
    int64_t done = 0;
    uint32_t seek[3] = { handle, 0, 0 /* SEEK_SET */ };

    if (data == NULL) return NULL;
    if (file_seek != 0) call_guest(file_seek, 3, seek);
    while (done < size)
    {
        uint32_t want = (uint32_t) (size - done > chunk ? chunk : size - done);
        uint32_t arguments[3] = { (uint32_t) (uintptr_t) (data + done), want, handle };
        int got = (int) call_guest(file_read, 3, arguments);

        if (got <= 0) break;
        done += got;
    }
    if (done != size)
    {
        free(data);
        return NULL;
    }
    return data;
}

static void close_guest_file(uint32_t handle)
{
    uint32_t arguments[1] = { handle };

    if (handle != 0 && file_close != 0) call_guest(file_close, 1, arguments);
}

static void verify_with_client(const char *name, const uint8_t *data, int64_t size);

/* the file read from the archives into the cache (with a reference), or NULL */
static RawFile *native_read(const char *key, const char *name)
{
    int64_t size = 0;
    uint8_t *data = WoW_MpqReadFile(name, &size);
    RawFile *raw;

    if (data == NULL) return NULL;
    verify_with_client(name, data, size);
    raw = raw_put(key, data, size);
    if (raw == NULL) free(data);
    return raw;
}

/* the stream reads from a file of the cache from now on */
static void stream_use_raw(Stream *s, RawFile *raw)
{
    s->raw = raw;
    s->from_memory = 1;
    s->memory = raw->data;
    s->memory_size = raw->size;
    s->size = raw->size;
    s->position = 0;
    s->handle = 0;
}

static uint32_t call_stream_open(const char *name);

/* the debug build with the sound log: the first native reads are compared with what the client's own functions read */
static void verify_with_client(const char *name, const uint8_t *data, int64_t size)
{
#ifdef SRW_DIAGNOSTICS
    static int verified;
    uint32_t handle;
    uint8_t *theirs;

    if (!SRW_LOG_ON(SRW_LOG_SOUND) || verified >= 20 || file_open == 0 || size > SMALL_FILE) return;   /* (a large one takes the client seconds) */
    verified++;
    handle = call_stream_open(name);
    if (handle == 0) return;
    theirs = read_whole(handle, size, (uint32_t) size);
    close_guest_file(handle);
    if (theirs == NULL) logprintf("Sound: native read of %s: the client could not read it for the comparison", name);
    else if (memcmp(theirs, data, (size_t) size) != 0) logprintf("Sound: native read of %s DIFFERS from the client's", name);
    else logprintf("Sound: native read of %s matches the client's (%lld bytes)", name, (long long) size);
    free(theirs);
#else
    (void) name; (void) data; (void) size;
#endif
}

static uint32_t call_stream_open(const char *name)
{
    uint32_t arguments[1] = { (uint32_t) (uintptr_t) name };

    return call_guest(file_open, 1, arguments);
}

/* finds the format and starts the decoder, makes the ring and fills the first moments; may wait for the client's files */
static void stream_init(Stream *s)
{
    uint8_t magic[12];
    int ok;

    if (source_read(s, magic, 12) != 12) memset(magic, 0, sizeof(magic));
    source_seek_to(s, 0);
    if (memcmp(magic, "RIFF", 4) == 0 && memcmp(magic + 8, "WAVE", 4) == 0)
    {
        ok = drwav_init(&s->wav, wav_read_proc, wav_seek_proc, wav_tell_proc, s, NULL);
        if (ok)
        {
            s->format = 1;
            s->rate = s->wav.sampleRate;
            s->channels = s->wav.channels;
            s->length_ms = s->wav.totalPCMFrameCount > 0 ? (uint32_t) (s->wav.totalPCMFrameCount * 1000 / s->rate) : 0;
        }
    }
    else
    {
        /* the length is estimated before the decoder reads: the estimate reads from the start and leaves the file at the start */
        uint32_t length_ms = estimate_mp3_ms(s);

        source_seek_to(s, 0);
        ok = drmp3_init(&s->mp3, mp3_read_proc, mp3_seek_proc, mp3_tell_proc, NULL, s, NULL);
        if (ok)
        {
            s->format = 2;
            s->rate = s->mp3.sampleRate;
            s->channels = s->mp3.channels;
            s->length_ms = length_ms;
        }
    }
    if (!ok || s->channels == 0 || s->channels > 8 || s->rate == 0)
    {
        set_error(FMOD_ERR_FILE_FORMAT);
        if (s->format == 1) drwav_uninit(&s->wav);
        else if (s->format == 2) drmp3_uninit(&s->mp3);
        s->format = 0;
        s->failed = 1;
        atomic_store(&s->decoder_done, 1);   /* it ends as soon as it is played */
        SRW_LOG(SRW_LOG_SOUND, "Sound: %s could not be decoded (magic %02x %02x %02x %02x)", s->name, magic[0], magic[1], magic[2], magic[3]);
        return;
    }
    /* a second of decoded sound; six for music (mp3 and anything long) */
    s->ring_frames = (s->format == 2 || s->length_ms > 15000) ? s->rate * 6 : s->rate;
    s->ring = calloc((size_t) s->ring_frames * 2, sizeof(float));
    stream_decode(s, s->rate / 4);   /* the first moments are ready when it is played */
    s->ready = 1;
    SRW_LOG(SRW_LOG_SOUND, "Sound: %s ready after %.0f ms: %s, %u Hz, %u channels, %u ms", s->name, emscripten_get_now() - s->opened_ms,
            s->format == 1 ? "wav" : "mp3", (unsigned int) s->rate, (unsigned int) s->channels, (unsigned int) s->length_ms);
}

uint32_t CCALL FSOUND_Stream_Open_c(uint32_t name_or_data, uint32_t mode, uint32_t offset, uint32_t length)
{
    Stream *s = calloc(1, sizeof(*s));

    if (!initialized)
    {
        set_error(FMOD_ERR_UNINITIALIZED);
        free(s);
        return 0;
    }
    s->mode = mode;
    s->opened_ms = emscripten_get_now();
    snprintf(s->name, sizeof(s->name), "%s", (mode & FSOUND_LOADMEMORY) ? "(memory)" : (const char *) (uintptr_t) name_or_data);
    s->channel = -1;
    s->seek_ms = -1;
    s->min_distance = 1.0f;
    s->max_distance = 1000000000.0f;
    s->loops_left = (mode & FSOUND_LOOP_NORMAL) ? -1 : 0;
    if (mode & FSOUND_LOADMEMORY)
    {
        s->from_memory = 1;
        s->memory = (const uint8_t *) (uintptr_t) name_or_data + offset;
        s->memory_size = length;
        s->size = length;
    }
    else
    {
        char key[128];
        RawFile *raw;

        if (file_open == 0 || file_read == 0)
        {
            set_error(FMOD_ERR_FILE_NOTFOUND);
            free(s);
            return 0;
        }
        int64_t native_size = 0;

        raw_key((const char *) (uintptr_t) name_or_data, key, sizeof(key));
        raw = raw_get(key);
        if (raw != NULL)
        {
            stream_use_raw(s, raw);   /* read before: nothing to wait for */
        }
        else if (WoW_MpqFind((const char *) (uintptr_t) name_or_data, &native_size) && native_size > SMALL_FILE)
        {
            s->size = native_size;   /* large (music, ambience): the loader thread reads it from the archives */
            s->native = 1;
            s->loading = 1;
        }
        else if (native_size > 0 && (raw = native_read(key, (const char *) (uintptr_t) name_or_data)) != NULL)
        {
            stream_use_raw(s, raw);   /* small (effects): read now from the archives, ready when it is played */
        }
        else
        {
            s->handle = call_stream_open((const char *) (uintptr_t) name_or_data);
            if (s->handle == 0)
            {
                set_error(FMOD_ERR_FILE_NOTFOUND);
                free(s);
                return 0;
            }
            if (file_seek != 0 && file_tell != 0)
            {
                uint32_t seek_end[3] = { s->handle, 0, 2 /* SEEK_END */ }, tell[1] = { s->handle };

                call_guest(file_seek, 3, seek_end);
                s->size = (int32_t) call_guest(file_tell, 1, tell);
                source_seek_to(s, 0);
            }
            if (s->size > 0 && s->size <= SMALL_FILE && file_seek != 0)
            {
                /* a small file (the effects): read now, it is ready when it is played */
                uint8_t *data = read_whole(s->handle, s->size, (uint32_t) s->size);

                if (data != NULL && (raw = raw_put(key, data, s->size)) != NULL)
                {
                    close_guest_file(s->handle);
                    stream_use_raw(s, raw);
                }
                else
                {
                    free(data);
                }
            }
            else if (s->size > 0 && file_seek != 0)
            {
                s->loading = 1;   /* a large one (music): the loader thread reads it */
            }
        }
    }
    if (s->from_memory)
    {
        stream_init(s);   /* memory: nothing to wait for */
        if (s->failed)
        {
            stream_free(s);
            return 0;
        }
    }
    pthread_mutex_lock(&audio_lock);
    s->next = streams;
    streams = s;
    pthread_mutex_unlock(&audio_lock);
    SRW_LOG(SRW_LOG_SOUND, "Sound: open %s, mode %x, %lld bytes -> stream %x", s->name, mode, (long long) s->size, (unsigned int) (uintptr_t) s);
    return (uint32_t) (uintptr_t) s;
}

static Stream *find_stream(uint32_t address)
{
    Stream *s;

    for (s = streams; s != NULL; s = s->next)
    {
        if ((uint32_t) (uintptr_t) s == address && !s->closing) return s;
    }
    return NULL;
}

/* channels: the handle is the index and, above, a counter that makes a handle of a sound that has ended invalid */

static int handle_of(int index)
{
    return (int) ((channels[index].generation << 12) | (uint32_t) index);
}

static Channel *channel_of(int handle)
{
    int index = handle & 0xfff;

    if (handle < 0 || index >= MAX_CHANNELS) return NULL;
    if (channels[index].stream == NULL || (uint32_t) handle >> 12 != channels[index].generation) return NULL;
    return &channels[index];
}

static void channel_release(int index)
{
    Channel *c = &channels[index];

    if (c->stream != NULL) c->stream->channel = -1;
    c->stream = NULL;
    c->generation = (c->generation + 1) & 0x7ffff;
}

static int stream_play(int channel, uint32_t stream_address, int start_paused)
{
    Stream *s;
    int index = -1, i;

    if (!initialized)
    {
        set_error(FMOD_ERR_UNINITIALIZED);
        return -1;
    }
    pthread_mutex_lock(&audio_lock);
    s = find_stream(stream_address);
    if (s == NULL)
    {
        pthread_mutex_unlock(&audio_lock);
        set_error(FMOD_ERR_INVALID_PARAM);
        return -1;
    }
    if (s->channel >= 0) channel_release(s->channel);
    if (channel == FSOUND_FREE)
    {
        for (i = 0; i < max_channels && index < 0; i++)
        {
            if (channels[i].stream == NULL && !channels[i].reserved) index = i;
        }
    }
    else if ((channel & 0xfff) < max_channels)
    {
        index = channel & 0xfff;
        if (channels[index].stream != NULL) channel_release(index);
    }
    if (index < 0)
    {
        pthread_mutex_unlock(&audio_lock);
        set_error(FMOD_ERR_CHANNEL_ALLOC);
        return -1;
    }
    if (atomic_load(&s->decoder_done) && atomic_load(&s->ring_read) >= atomic_load(&s->ring_write))
    {
        s->seek_ms = 0;   /* it ended before: from the start */
        atomic_store(&s->decoder_done, 0);   /* the mixer must not take it for finished until it has been rewound */
        s->loops_left = (s->mode & FSOUND_LOOP_NORMAL) ? -1 : s->loops_left;
    }
    channels[index].stream = s;
    channels[index].paused = start_paused != 0;
    channels[index].muted = 0;
    channels[index].volume = 255;
    channels[index].frequency = 0;   /* the rate of the stream, when it is known */
    memset(channels[index].position, 0, sizeof(channels[index].position));
    memset(channels[index].velocity, 0, sizeof(channels[index].velocity));
    channels[index].gain_left = channels[index].gain_right = -1.0f;   /* the first chunk starts at its level */
    s->channel = index;
    s->end_pending = 0;
    i = handle_of(index);
    pthread_mutex_unlock(&audio_lock);
    SRW_LOG(SRW_LOG_SOUND, "Sound: play %s on channel %d%s%s, %.0f ms after the open", s->name, index, start_paused ? " (paused)" : "",
            s->ready ? "" : " (not ready yet)", emscripten_get_now() - s->opened_ms);
    return i;
}

uint32_t CCALL FSOUND_Stream_Play_c(uint32_t channel, uint32_t stream)
{
    return (uint32_t) stream_play((int) channel, stream, 0);
}

uint32_t CCALL FSOUND_Stream_PlayEx_c(uint32_t channel, uint32_t stream, uint32_t dsp, uint32_t start_paused)
{
    (void) dsp;
    return (uint32_t) stream_play((int) channel, stream, (int) (start_paused & 0xff));
}

/* stops what plays on a channel and rewinds its stream */
static void stop_channel(int index)
{
    Stream *s = channels[index].stream;

    if (s != NULL)
    {
        s->seek_ms = 0;
        s->end_pending = 0;
    }
    channel_release(index);
}

uint32_t CCALL FSOUND_StopSound_c(uint32_t channel)
{
    int i;

    pthread_mutex_lock(&audio_lock);
    if ((int) channel == FSOUND_ALL)
    {
        for (i = 0; i < MAX_CHANNELS; i++)
        {
            if (channels[i].stream != NULL) stop_channel(i);
        }
    }
    else if (channel_of((int) channel) != NULL)
    {
        stop_channel((int) channel & 0xfff);
    }
    pthread_mutex_unlock(&audio_lock);
    return 1;
}

uint32_t CCALL FSOUND_Stream_Stop_c(uint32_t stream)
{
    Stream *s;

    pthread_mutex_lock(&audio_lock);
    s = find_stream(stream);
    if (s != NULL && s->channel >= 0) stop_channel(s->channel);
    pthread_mutex_unlock(&audio_lock);
    if (s != NULL) SRW_LOG(SRW_LOG_SOUND, "Sound: stop %s", s->name);
    return s != NULL;
}

uint32_t CCALL FSOUND_Stream_Close_c(uint32_t stream)
{
    Stream *s;

    pthread_mutex_lock(&audio_lock);
    s = find_stream(stream);
    if (s != NULL)
    {
        if (s->channel >= 0) channel_release(s->channel);
        s->closing = 1;   /* the decoder thread frees it when it does not work on it */
        s->end_pending = 0;
    }
    pthread_mutex_unlock(&audio_lock);
    if (s != NULL) SRW_LOG(SRW_LOG_SOUND, "Sound: close %s", s->name);
    return s != NULL;
}

uint32_t CCALL FSOUND_Stream_SetLoopCount_c(uint32_t stream, uint32_t count)
{
    Stream *s;

    pthread_mutex_lock(&audio_lock);
    s = find_stream(stream);
    if (s != NULL) s->loops_left = (int) count;
    pthread_mutex_unlock(&audio_lock);
    return s != NULL;
}

uint32_t CCALL FSOUND_Stream_SetTime_c(uint32_t stream, uint32_t ms)
{
    Stream *s;

    pthread_mutex_lock(&audio_lock);
    s = find_stream(stream);
    if (s != NULL) s->seek_ms = (int) ms;
    pthread_mutex_unlock(&audio_lock);
    return s != NULL;
}

uint32_t CCALL FSOUND_Stream_GetTime_c(uint32_t stream)
{
    Stream *s;
    uint32_t ms = 0;

    pthread_mutex_lock(&audio_lock);
    s = find_stream(stream);
    if (s != NULL && s->rate != 0) ms = (uint32_t) (s->consumed * 1000 / s->rate);
    pthread_mutex_unlock(&audio_lock);
    return ms;
}

uint32_t CCALL FSOUND_Stream_GetLengthMs_c(uint32_t stream)
{
    Stream *s;
    uint32_t ms = 0;

    pthread_mutex_lock(&audio_lock);
    s = find_stream(stream);
    if (s != NULL) ms = s->length_ms;
    pthread_mutex_unlock(&audio_lock);
    return ms;
}

uint32_t CCALL FSOUND_Stream_GetSample_c(uint32_t stream)
{
    Stream *s;
    uint32_t address = 0;

    pthread_mutex_lock(&audio_lock);
    s = find_stream(stream);
    if (s != NULL) address = (uint32_t) (uintptr_t) &s->sample_tag;
    pthread_mutex_unlock(&audio_lock);
    return address;
}

uint32_t CCALL FSOUND_Sample_SetMinMaxDistance_c(uint32_t sample, uint32_t min, uint32_t max)
{
    Stream *s;
    uint32_t done = 0;

    pthread_mutex_lock(&audio_lock);
    for (s = streams; s != NULL; s = s->next)
    {
        if ((uint32_t) (uintptr_t) &s->sample_tag == sample)
        {
            s->min_distance = bits_to_float(min);
            s->max_distance = bits_to_float(max);
            done = 1;
        }
    }
    pthread_mutex_unlock(&audio_lock);
    return done;
}

uint32_t CCALL FSOUND_Stream_SetEndCallback_c(uint32_t stream, uint32_t callback, uint32_t data)
{
    Stream *s;

    pthread_mutex_lock(&audio_lock);
    s = find_stream(stream);
    if (s != NULL)
    {
        s->end_callback = callback;
        s->end_data = data;
    }
    pthread_mutex_unlock(&audio_lock);
    return s != NULL;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* channels: volume, frequency, position */

static void action_volume(Channel *c, int value) { c->volume = value; }
static void action_frequency(Channel *c, int value) { c->frequency = (float) value; }
static void action_paused(Channel *c, int value) { c->paused = value; }
static void action_muted(Channel *c, int value) { c->muted = value; }
static void action_reserved(Channel *c, int value) { c->reserved = value; }

static void for_channels(int channel, void (*action)(Channel *, int), int value)
{
    int i;

    if (SRW_LOG_ON(SRW_LOG_SOUND))
    {
        static int logged;
        const char *what = action == action_volume ? "volume" : action == action_frequency ? "frequency" : action == action_paused ? "paused" :
                           action == action_muted ? "muted" : "reserved";

        if (logged++ < 200)
            logprintf("Sound: %s %d for channel %x (%s)", what, value, (unsigned int) channel,
                      channel == FSOUND_ALL ? "all" : channel_of(channel) != NULL ? channel_of(channel)->stream->name : "no such channel");
    }

    pthread_mutex_lock(&audio_lock);
    if (channel == FSOUND_ALL)
    {
        for (i = 0; i < MAX_CHANNELS; i++)
        {
            if (channels[i].stream != NULL) action(&channels[i], value);
        }
    }
    else if (channel_of(channel) != NULL)
    {
        action(channel_of(channel), value);
    }
    pthread_mutex_unlock(&audio_lock);
}

uint32_t CCALL FSOUND_SetVolume_c(uint32_t channel, uint32_t volume)
{
    for_channels((int) channel, action_volume, (int) (volume > 255 ? 255 : volume));
    return 1;
}

uint32_t CCALL FSOUND_SetFrequency_c(uint32_t channel, uint32_t frequency)
{
    if ((int) frequency > 0) for_channels((int) channel, action_frequency, (int) frequency);
    return 1;
}

uint32_t CCALL FSOUND_SetPaused_c(uint32_t channel, uint32_t paused)
{
    for_channels((int) channel, action_paused, (int) (paused & 0xff) != 0);
    return 1;
}

uint32_t CCALL FSOUND_SetMute_c(uint32_t channel, uint32_t mute)
{
    /* FSOUND_ALL sets the mute state of every channel (as FMOD 3 does: a channel that was muted alone is unmuted by it too) */
    if ((int) channel == FSOUND_ALL) SRW_LOG(SRW_LOG_SOUND, "Sound: all channels %s", (mute & 0xff) ? "muted" : "unmuted");
    for_channels((int) channel, action_muted, (int) (mute & 0xff) != 0);
    return 1;
}

uint32_t CCALL FSOUND_SetReserved_c(uint32_t channel, uint32_t reserved)
{
    for_channels((int) channel, action_reserved, (int) (reserved & 0xff) != 0);
    return 1;
}

uint32_t CCALL FSOUND_SetSFXMasterVolume_c(uint32_t volume)
{
    master_volume = volume > 255 ? 255 : (int) volume;
    return 1;
}

uint32_t CCALL FSOUND_IsPlaying_c(uint32_t channel)
{
    uint32_t playing;

    pthread_mutex_lock(&audio_lock);
    playing = channel_of((int) channel) != NULL;
    pthread_mutex_unlock(&audio_lock);
    return playing;
}

uint32_t CCALL FSOUND_3D_SetAttributes_c(uint32_t channel, uint32_t position, uint32_t velocity)
{
    Channel *c;

    pthread_mutex_lock(&audio_lock);
    c = channel_of((int) channel);
    if (c != NULL)
    {
        if (position != 0) memcpy(c->position, (const float *) (uintptr_t) position, sizeof(c->position));
        if (velocity != 0) memcpy(c->velocity, (const float *) (uintptr_t) velocity, sizeof(c->velocity));
    }
    pthread_mutex_unlock(&audio_lock);
    return c != NULL;
}

uint32_t CCALL FSOUND_3D_Listener_SetAttributes_c(uint32_t position, uint32_t velocity, uint32_t fx, uint32_t fy, uint32_t fz, uint32_t tx, uint32_t ty, uint32_t tz)
{
    pthread_mutex_lock(&audio_lock);
    if (position != 0) memcpy(listener_position, (const float *) (uintptr_t) position, sizeof(listener_position));
    if (velocity != 0) memcpy(listener_velocity, (const float *) (uintptr_t) velocity, sizeof(listener_velocity));
    listener_front[0] = bits_to_float(fx); listener_front[1] = bits_to_float(fy); listener_front[2] = bits_to_float(fz);
    listener_top[0] = bits_to_float(tx); listener_top[1] = bits_to_float(ty); listener_top[2] = bits_to_float(tz);
    pthread_mutex_unlock(&audio_lock);
    return 1;
}

uint32_t CCALL FSOUND_3D_Listener_GetAttributes_c(uint32_t position, uint32_t velocity, uint32_t fx, uint32_t fy, uint32_t fz, uint32_t tx, uint32_t ty, uint32_t tz)
{
    pthread_mutex_lock(&audio_lock);
    if (position != 0) memcpy((float *) (uintptr_t) position, listener_position, sizeof(listener_position));
    if (velocity != 0) memcpy((float *) (uintptr_t) velocity, listener_velocity, sizeof(listener_velocity));
    if (fx != 0) memcpy((float *) (uintptr_t) fx, &listener_front[0], 4);
    if (fy != 0) memcpy((float *) (uintptr_t) fy, &listener_front[1], 4);
    if (fz != 0) memcpy((float *) (uintptr_t) fz, &listener_front[2], 4);
    if (tx != 0) memcpy((float *) (uintptr_t) tx, &listener_top[0], 4);
    if (ty != 0) memcpy((float *) (uintptr_t) ty, &listener_top[1], 4);
    if (tz != 0) memcpy((float *) (uintptr_t) tz, &listener_top[2], 4);
    pthread_mutex_unlock(&audio_lock);
    return 1;
}

uint32_t CCALL FSOUND_3D_SetRolloffFactor_c(uint32_t scale) { rolloff_factor = bits_to_float(scale); return 1; }
uint32_t CCALL FSOUND_3D_SetDistanceFactor_c(uint32_t scale) { (void) scale; return 1; }
uint32_t CCALL FSOUND_3D_SetDopplerFactor_c(uint32_t scale) { (void) scale; return 1; }

/* ------------------------------------------------------------------------------------------------------------------ */
/* the mixer */

static float dot3(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* the gains of a channel for the left and the right output: volume, and for a 3D stream the distance and the direction */
static void channel_gains(const Channel *c, float *left, float *right)
{
    float base, gain_left = 1.0f, gain_right = 1.0f;
    const Stream *s = c->stream;

    /* the master volume of the effects does not reach the channels that the client has reserved (its music) */
    base = (float) c->volume / 255.0f * (c->reserved ? 1.0f : (float) master_volume / 255.0f);
    if (c->muted) base = 0;
    if (!(s->mode & FSOUND_2D))
    {
        float relative[3] = { c->position[0] - listener_position[0], c->position[1] - listener_position[1], c->position[2] - listener_position[2] };
        float distance = sqrtf(dot3(relative, relative)), attenuation = 1.0f, pan = 0;
        float minimum = s->min_distance, maximum = s->max_distance;

        if (distance > minimum && minimum > 0)
        {
            float d = distance < maximum ? distance : maximum;

            attenuation = minimum / (minimum + rolloff_factor * (d - minimum));
        }
        if (distance > 0.0001f)
        {
            /* right = top x front (Direct3D style axes: x right, y up, z ahead) */
            float right_axis[3] = { listener_top[1] * listener_front[2] - listener_top[2] * listener_front[1],
                                    listener_top[2] * listener_front[0] - listener_top[0] * listener_front[2],
                                    listener_top[0] * listener_front[1] - listener_top[1] * listener_front[0] };
            float length = sqrtf(dot3(right_axis, right_axis));

            if (length > 0.0001f) pan = dot3(relative, right_axis) / (distance * length);
        }
        gain_left = 1.0f - (pan > 0 ? pan : 0);
        gain_right = 1.0f + (pan < 0 ? pan : 0);
        base *= attenuation;
    }
    *left = base * gain_left;
    *right = base * gain_right;
}

static void mix_chunk(float *out, int frames)
{
    int i, f;

    memset(out, 0, (size_t) frames * 2 * sizeof(float));
    pthread_mutex_lock(&audio_lock);
    for (i = 0; i < MAX_CHANNELS; i++)
    {
        Channel *c = &channels[i];
        Stream *s = c->stream;
        float target_left, target_right, step, from_left, from_right;
        uint64_t read, write;

        if (s == NULL || c->paused || (!s->ready && !s->failed)) continue;
        channel_gains(c, &target_left, &target_right);
        from_left = c->gain_left < 0 ? target_left : c->gain_left;
        from_right = c->gain_right < 0 ? target_right : c->gain_right;
        step = (c->frequency > 0 ? c->frequency : (float) s->rate) / (float) out_rate;
        read = atomic_load_explicit(&s->ring_read, memory_order_relaxed);
        write = atomic_load_explicit(&s->ring_write, memory_order_acquire);
        for (f = 0; f < frames; f++)
        {
            const float *a, *b;
            float t = (float) s->phase, left, right, gl, gr;

            if (write <= read)
            {
                if (atomic_load(&s->decoder_done))
                {
                    /* it has been played to its end */
                    s->end_pending = s->end_callback != 0;
                    channel_release(i);
                    break;
                }
                underruns++;
                break;   /* the decoder is behind: nothing for the rest of the chunk */
            }
            a = s->ring + (read % s->ring_frames) * 2;
            b = write > read + 1 ? s->ring + ((read + 1) % s->ring_frames) * 2 : a;
            left = a[0] + (b[0] - a[0]) * t;
            right = a[1] + (b[1] - a[1]) * t;
            gl = from_left + (target_left - from_left) * ((float) f / (float) frames);
            gr = from_right + (target_right - from_right) * ((float) f / (float) frames);
            out[f * 2] += left * gl;
            out[f * 2 + 1] += right * gr;
            s->phase += step;
            while (s->phase >= 1.0 && write > read)
            {
                s->phase -= 1.0;
                read++;
                s->consumed++;
            }
            if (write <= read) s->phase = 0;
        }
        atomic_store_explicit(&s->ring_read, read, memory_order_release);
        if (c->stream == s)
        {
            c->gain_left = target_left;
            c->gain_right = target_right;
        }
    }
    pthread_mutex_unlock(&audio_lock);
    for (f = 0; f < frames * 2; f++)
    {
        if (out[f] > 1.0f) out[f] = 1.0f;
        else if (out[f] < -1.0f) out[f] = -1.0f;
    }
}

static void *mixer_main(void *argument)
{
    double last_report = emscripten_get_now();
    int reported_underruns = 0;

    (void) argument;
    while (running)
    {
        if (emscripten_get_now() - last_report > 5000.0)
        {
            int active = 0, i;

            for (i = 0; i < MAX_CHANNELS; i++) active += channels[i].stream != NULL;
            if (underruns != reported_underruns) SRW_LOG(SRW_LOG_SOUND, "Sound: %d underruns in 5 s (a stream had no decoded sound), %d channels play",
                                                         underruns - reported_underruns, active);
            reported_underruns = underruns;
            last_report = emscripten_get_now();
        }
        uint32_t write = atomic_load(&out_write), read = atomic_load(&out_read);

        if (write - read + MIX_CHUNK <= TARGET_FILL)
        {
            float chunk[MIX_CHUNK * 2];
            int i;

            mix_chunk(chunk, MIX_CHUNK);
            for (i = 0; i < MIX_CHUNK; i++)
            {
                float *frame = out_ring + ((write + (uint32_t) i) & (OUT_RING_FRAMES - 1)) * 2;

                frame[0] = chunk[i * 2];
                frame[1] = chunk[i * 2 + 1];
            }
            atomic_store_explicit(&out_write, write + MIX_CHUNK, memory_order_release);
            if (!have_output)
            {
                /* nowhere to play (no page): the time goes by as if it played */
                usleep((useconds_t) (1000000.0 * MIX_CHUNK / out_rate));
                atomic_store(&out_read, write + MIX_CHUNK);
            }
        }
        else
        {
            usleep(3000);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the decoder thread: fills the streams, seeks, calls the end callbacks and frees the streams that were closed */

static void stream_seek_ms(Stream *s, int ms)
{
    uint64_t frame = (uint64_t) ms * s->rate / 1000;
    int ok;

    if (s->format == 1) ok = drwav_seek_to_pcm_frame(&s->wav, frame);
    else ok = drmp3_seek_to_pcm_frame(&s->mp3, frame);
    pthread_mutex_lock(&audio_lock);
    atomic_store(&s->ring_read, 0);
    atomic_store(&s->ring_write, 0);
    s->phase = 0;
    s->consumed = ok ? frame : 0;
    atomic_store(&s->decoder_done, 0);
    pthread_mutex_unlock(&audio_lock);
}

static void *decoder_main(void *argument)
{
    (void) argument;
    while (running)
    {
        Stream *s, *work = NULL;
        int action = 0;   /* 1 free, 2 end callback, 3 seek, 4 decode */

        pthread_mutex_lock(&audio_lock);
        for (s = streams; s != NULL && work == NULL; s = s->next)
        {
            if (s->busy) continue;
            if (s->closing) action = 1;
            else if (s->loading) continue;   /* the loader thread reads it */
            else if (!s->ready && !s->failed) action = 5;
            else if (s->seek_ms >= 0 && s->ready) action = 3;
            else if (s->ready && !atomic_load(&s->decoder_done) &&
                     s->ring_frames - (atomic_load(&s->ring_write) - atomic_load(&s->ring_read)) >= s->ring_frames / 4) action = 4;
            if (action != 0)
            {
                work = s;
                s->busy = 1;
            }
        }
        if (work != NULL && action == 1)
        {
            Stream **link;

            for (link = &streams; *link != NULL && *link != work; link = &(*link)->next) { }
            if (*link == work) *link = work->next;
        }
        pthread_mutex_unlock(&audio_lock);
        if (work == NULL)
        {
            usleep(4000);
            continue;
        }
        if (action == 1)
        {
            stream_free(work);
            continue;
        }
        if (action == 5)
        {
            stream_init(work);
        }
        else if (action == 3)
        {
            int ms = work->seek_ms;

            work->seek_ms = -1;
            stream_seek_ms(work, ms);
        }
        else
        {
            uint64_t free_frames = work->ring_frames - (atomic_load(&work->ring_write) - atomic_load(&work->ring_read));

            stream_decode(work, (uint32_t) free_frames);
        }
        pthread_mutex_lock(&audio_lock);
        work->busy = 0;
        pthread_mutex_unlock(&audio_lock);
    }
    return NULL;
}

/* the loader thread: the work that needs the client (and so the big lock): reading the large files into memory, the end callbacks.
   The decoder thread then never waits for the client. */
static void *loader_main(void *argument)
{
    int64_t unused;

    (void) argument;
    WoW_MpqFind("(the archives are opened now, not at the first sound)", &unused);   /* their tables: a few MB from the network */
    while (running)
    {
        Stream *s, *work = NULL;
        int action = 0;   /* 1 load, 2 end callback */

        pthread_mutex_lock(&audio_lock);
        for (s = streams; s != NULL && work == NULL; s = s->next)
        {
            if (s->busy || s->closing) continue;
            if (s->loading) action = 1;
            else if (s->end_pending) action = 2;
            if (action != 0)
            {
                work = s;
                s->busy = 1;
            }
        }
        pthread_mutex_unlock(&audio_lock);
        if (work == NULL)
        {
            usleep(4000);
            continue;
        }
        if (action == 1)
        {
            char key[128];
            /* the whole file in one call: each call waits for the big lock */
            double read_start = emscripten_get_now();
            uint8_t *data = NULL;
            RawFile *raw = NULL;

            raw_key(work->name, key, sizeof(key));
            if (work->native)
            {
                /* from the archives, without the client */
                raw = native_read(key, work->name);
                if (raw == NULL)
                {
                    /* the client's own open */
                    work->native = 0;
                    work->handle = call_stream_open(work->name);
                }
            }
            if (raw == NULL && work->handle != 0) data = read_whole(work->handle, work->size, (uint32_t) work->size);
            if (data != NULL) raw = raw_put(key, data, work->size);
            if (raw == NULL) SRW_LOG(SRW_LOG_SOUND, "Sound: %s could not be read", work->name);
            if (raw != NULL)
            {
                close_guest_file(work->handle);
                stream_use_raw(work, raw);
            }
            else
            {
                free(data);   /* (the decoder reads it through the client then, as before) */
            }
            SRW_LOG(SRW_LOG_SOUND, "Sound: %s read: %lld bytes in %.0f ms, %.0f ms after the open", work->name, (long long) work->size,
                    emscripten_get_now() - read_start, emscripten_get_now() - work->opened_ms);
            pthread_mutex_lock(&audio_lock);
            work->loading = 0;
            pthread_mutex_unlock(&audio_lock);
        }
        else
        {
            /* the end callback is read with the big lock held: the client clears it before it frees the stream */
            uint32_t callback, arguments[4] = { (uint32_t) (uintptr_t) work, 0, 0, 0 };

            SRW_GilAcquire();
            pthread_mutex_lock(&audio_lock);
            callback = (work->end_pending && !work->closing) ? work->end_callback : 0;
            arguments[3] = work->end_data;
            work->end_pending = 0;
            pthread_mutex_unlock(&audio_lock);
            if (callback != 0) SRW_CallGuest(callback, 4, arguments);
            SRW_GilRelease();
        }
        pthread_mutex_lock(&audio_lock);
        work->busy = 0;
        pthread_mutex_unlock(&audio_lock);
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the sound system */

uint32_t CCALL FSOUND_Init_c(uint32_t mixrate, uint32_t maxsoftwarechannels, uint32_t flags)
{
    int rate;

    (void) flags;
    if (initialized)
    {
        set_error(FMOD_ERR_BUSY);
        return 0;
    }
    max_channels = maxsoftwarechannels < 1 ? 32 : (maxsoftwarechannels > MAX_CHANNELS ? MAX_CHANNELS : (int) maxsoftwarechannels);
    out_rate = mixrate > 0 ? (int) mixrate : 44100;
    rate = MAIN_THREAD_EM_ASM_INT({ return Module['srwAudio'] ? Module['srwAudio'].start($0, $1, $2, $3) : 0; }, out_ring, &out_read, &out_write, OUT_RING_FRAMES);
    have_output = rate > 0;
    if (have_output) out_rate = rate;
    atomic_store(&out_read, 0);
    atomic_store(&out_write, 0);
    initialized = 1;
    running = 1;
    pthread_create(&mixer_thread, NULL, mixer_main, NULL);
    pthread_create(&decoder_thread, NULL, decoder_main, NULL);
    pthread_create(&loader_thread, NULL, loader_main, NULL);
    logprintf("Sound: %s, %d Hz, %d channels", have_output ? "Web Audio" : "no output device", out_rate, max_channels);
    SRW_ReactivateWindow();   /* the client takes itself for inactive (all sound muted) until it is told it is active after this */
    return 1;
}

uint32_t CCALL FSOUND_Close_c(void)
{
    int i;
    Stream *s;

    if (!initialized) return 0;
    pthread_mutex_lock(&audio_lock);
    for (i = 0; i < MAX_CHANNELS; i++) channel_release(i);
    for (s = streams; s != NULL; s = s->next) s->closing = 1;
    pthread_mutex_unlock(&audio_lock);
    initialized = 0;   /* the threads go on with the streams that are left and then idle */
    return 1;
}

uint32_t CCALL FSOUND_File_SetCallbacks_c(uint32_t open, uint32_t close, uint32_t read, uint32_t seek, uint32_t tell)
{
    file_open = open;
    file_close = close;
    file_read = read;
    file_seek = seek;
    file_tell = tell;
    return 1;
}

uint32_t CCALL FSOUND_GetError_c(void)
{
    return (uint32_t) last_error;
}

/* things that only configure a sound card, and things the client asks about it */
#define FMOD_TRUE(name, ...) uint32_t CCALL name##_c(__VA_ARGS__) { return 1; }
#define FMOD_ZERO(name, ...) uint32_t CCALL name##_c(__VA_ARGS__) { return 0; }

FMOD_TRUE(FSOUND_SetOutput, uint32_t outputtype)
FMOD_TRUE(FSOUND_SetDriver, uint32_t driver)
FMOD_TRUE(FSOUND_SetMixer, uint32_t mixer)
FMOD_TRUE(FSOUND_SetBufferSize, uint32_t lenms)
FMOD_TRUE(FSOUND_SetHWND, uint32_t hwnd)
FMOD_TRUE(FSOUND_SetMaxHardwareChannels, uint32_t max)
FMOD_TRUE(FSOUND_SetMinHardwareChannels, uint32_t min)
FMOD_TRUE(FSOUND_SetMemorySystem, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
FMOD_TRUE(FSOUND_Update, void)
FMOD_TRUE(FSOUND_Reverb_SetProperties, uint32_t prop)
FMOD_TRUE(FSOUND_Reverb_GetProperties, uint32_t prop)
FMOD_TRUE(FSOUND_Reverb_SetChannelProperties, uint32_t channel, uint32_t prop)
FMOD_ZERO(FSOUND_GetDriver, void)
FMOD_ZERO(FSOUND_GetOutput, void)
FMOD_ZERO(FSOUND_GetMixer, void)

uint32_t CCALL FSOUND_GetDriverCaps_c(uint32_t id, uint32_t caps)
{
    (void) id;
    if (caps != 0) *(uint32_t *) (uintptr_t) caps = 0;
    return 1;
}

/* the client insists on version 3.75, which comes back as a float in st(0) (the bits are returned) */
uint32_t CCALL FSOUND_GetVersion_c(void)
{
    return 0x40700000;
}

uint32_t CCALL FSOUND_GetCPUUsage_c(void)
{
    return 0; /* 0.0f */
}

uint32_t CCALL FSOUND_GetNumDrivers_c(void)
{
    return 1;
}

uint32_t CCALL FSOUND_GetDriverName_c(uint32_t id)
{
    static const char name[] = "Web Audio";

    (void) id;
    return (uint32_t) (uintptr_t) name;
}

uint32_t CCALL FSOUND_GetMaxChannels_c(void)
{
    return (uint32_t) max_channels;
}

uint32_t CCALL FSOUND_GetOutputRate_c(void)
{
    return (uint32_t) out_rate;
}

uint32_t CCALL FSOUND_GetNumHWChannels_c(uint32_t a, uint32_t b, uint32_t c)
{
    if (a != 0) *(uint32_t *) (uintptr_t) a = 0;
    if (b != 0) *(uint32_t *) (uintptr_t) b = 0;
    if (c != 0) *(uint32_t *) (uintptr_t) c = 0;
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
#ifdef WOW_AUDIO_SELFTEST
/* scons audiotest=1: the sound system plays streams made of memory and reports what the mixer did (no page needed) */
#include WOW_AUDIO_SAMPLE_INC   /* the build makes it (tools/audio-test/make_sample.py) */

static uint8_t *make_wav(uint32_t rate, uint32_t frames, uint32_t *size)
{
    uint8_t *wav = malloc(44 + frames * 2);
    uint32_t i, data = frames * 2, fields[11] = { 0x46464952, 36 + data, 0x45564157, 0x20746d66, 16, 0, 0, 0, 0, 0, 0 };

    memcpy(wav, fields, 20);
    *(uint16_t *) (wav + 20) = 1;              /* PCM */
    *(uint16_t *) (wav + 22) = 1;              /* mono */
    *(uint32_t *) (wav + 24) = rate;
    *(uint32_t *) (wav + 28) = rate * 2;
    *(uint16_t *) (wav + 32) = 2;
    *(uint16_t *) (wav + 34) = 16;
    memcpy(wav + 36, "data", 4);
    *(uint32_t *) (wav + 40) = data;
    for (i = 0; i < frames; i++) ((int16_t *) (wav + 44))[i] = (int16_t) (16000.0 * sin(2.0 * 3.14159265 * 440.0 * i / rate));
    *size = 44 + data;
    return wav;
}

/* left and right energy of the last frames the mixer made */
static void recent_energy(double *left, double *right)
{
    uint32_t write = atomic_load(&out_write), i;

    *left = *right = 0;
    for (i = 1; i <= 2048; i++)
    {
        const float *frame = out_ring + ((write - i) & (OUT_RING_FRAMES - 1)) * 2;

        *left += (double) frame[0] * frame[0];
        *right += (double) frame[1] * frame[1];
    }
}

static int checks, passed;

static void check(int ok, const char *what, long value)
{
    checks++;
    passed += ok;
    if (!ok) logprintf("Audio self test FAILED: %s (%ld)", what, value);
}

static uint64_t test_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000u + (uint64_t) now.tv_nsec / 1000000u;
}

static void *audio_test_main(void *argument)
{
    uint32_t size, wav_size;
    uint8_t *wav = make_wav(22050, 11025, &wav_size);   /* half a second */
    uint32_t stream, h, mp3;
    uint64_t started;
    double left, right, quiet_left, quiet_right;
    float far_right[3] = { 10.0f, 0.0f, 0.0f };

    (void) argument;
    (void) size;
    sleep(1);
    check(FSOUND_Init_c(44100, 16, 0) == 1, "init", 0);

    /* plays once and ends */
    stream = FSOUND_Stream_Open_c((uint32_t) (uintptr_t) wav, 0x8000 | 0x2000, 0, wav_size);
    check(stream != 0, "open a wav from memory", 0);
    check(FSOUND_Stream_GetLengthMs_c(stream) >= 495 && FSOUND_Stream_GetLengthMs_c(stream) <= 505, "length of the wav", FSOUND_Stream_GetLengthMs_c(stream));
    started = test_ms();
    h = FSOUND_Stream_Play_c((uint32_t) -1, stream);
    check((int) h >= 0 && FSOUND_IsPlaying_c(h) == 1, "play", (long) h);
    usleep(200000);
    check(FSOUND_Stream_GetTime_c(stream) >= 20 && FSOUND_Stream_GetTime_c(stream) <= 350, "time of the stream after 200 ms", FSOUND_Stream_GetTime_c(stream));
    recent_energy(&left, &right);
    check(left > 1.0 && right > 1.0, "sound reaches the output", (long) left);
    while (FSOUND_IsPlaying_c(h) && test_ms() - started < 3000) usleep(10000);
    check(FSOUND_IsPlaying_c(h) == 0 && test_ms() - started >= 400 && test_ms() - started <= 1200, "it ends after about half a second", (long) (test_ms() - started));

    /* plays again from the start (a rewound stream), with the volume turned down */
    h = FSOUND_Stream_Play_c((uint32_t) -1, stream);
    FSOUND_SetVolume_c(h, 0);
    usleep(150000);
    recent_energy(&quiet_left, &quiet_right);
    check(quiet_left < 0.001 && quiet_right < 0.001, "volume 0 is silent", (long) (quiet_left * 1000));
    FSOUND_SetVolume_c(h, 255);
    usleep(120000);
    check(FSOUND_IsPlaying_c(h) == 1, "plays again after a rewind", 0);
    FSOUND_StopSound_c(h);
    check(FSOUND_IsPlaying_c(h) == 0, "stop", 0);
    FSOUND_Stream_Close_c(stream);

    /* loops */
    stream = FSOUND_Stream_Open_c((uint32_t) (uintptr_t) wav, 0x8000 | 0x2000 | 0x2, 0, wav_size);
    h = FSOUND_Stream_Play_c((uint32_t) -1, stream);
    usleep(1400000);
    check(FSOUND_IsPlaying_c(h) == 1, "a looping sound goes on after 1.4 s", 0);
    FSOUND_StopSound_c(h);
    FSOUND_Stream_Close_c(stream);

    /* 3D: a sound on the right is louder on the right */
    stream = FSOUND_Stream_Open_c((uint32_t) (uintptr_t) wav, 0x8000 | 0x2, 0, wav_size);
    h = FSOUND_Stream_Play_c((uint32_t) -1, stream);
    FSOUND_3D_SetAttributes_c(h, (uint32_t) (uintptr_t) far_right, 0);
    usleep(200000);
    recent_energy(&left, &right);
    check(right > left * 2.0, "3D: a sound on the right is louder in the right output", (long) (left * 1000));
    FSOUND_StopSound_c(h);
    FSOUND_Stream_Close_c(stream);

    /* an mp3 */
    mp3 = FSOUND_Stream_Open_c((uint32_t) (uintptr_t) sample_mp3, 0x8000 | 0x2000, 0, sizeof(sample_mp3));
    check(mp3 != 0, "open an mp3 from memory", 0);
    h = FSOUND_Stream_Play_c((uint32_t) -1, mp3);
    usleep(400000);
    recent_energy(&left, &right);
    check(FSOUND_Stream_GetTime_c(mp3) >= 200 && (left > 0.01 || right > 0.01), "an mp3 plays", (long) FSOUND_Stream_GetTime_c(mp3));
    FSOUND_Stream_SetTime_c(mp3, 500);
    usleep(300000);
    check(FSOUND_Stream_GetTime_c(mp3) >= 500 && FSOUND_Stream_GetTime_c(mp3) < 1500, "seeking in an mp3", FSOUND_Stream_GetTime_c(mp3));
    FSOUND_Stream_Close_c(mp3);
    usleep(100000);
    check(FSOUND_Close_c() == 1, "close", 0);
    logprintf("Audio self test: %d of %d checks passed, %d underruns", passed, checks, underruns);
    return NULL;
}

void WoW_AudioSelfTest(void)
{
    pthread_t thread;

    pthread_create(&thread, NULL, audio_test_main, NULL);
    pthread_detach(thread);
}
#endif
