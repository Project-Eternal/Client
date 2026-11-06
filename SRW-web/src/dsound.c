/* DirectSound (dsound.dll) for a program whose sound library plays through DirectSound and gives up all sound when there is
   none. The objects are guest COM objects (a block of memory whose first dword is the vtable, the methods are the procedures of
   SRW/dsound.def), the device is the page's audio (waveout.c has the ring buffer of stereo floats the page reads).

   The sound library makes a primary buffer (the format of the device), one looping secondary buffer of a fraction of a second and polls its
   play and write cursors to keep it filled. The secondary buffer is played into the ring from a hook of the output thread of waveout.c, a
   little ahead of what the page has read; the play cursor reported is what the page has played, the write cursor a tenth of a
   second ahead of it (more than the output thread is ahead, so what the program writes there is still in time). */
#include <emscripten.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "srw_debuglog.h"
#include "srw.h"
#include "imports.h"

#define DS_OK 0
#define DSERR_UNSUPPORTED 0x80004001u
#define E_NOINTERFACE_ 0x80004002u
#define DSERR_GENERIC 0x80004005u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_NODRIVER 0x88780078u
#define DSERR_ALLOCATED 0x8878000au

#define DSBCAPS_PRIMARYBUFFER 0x00000001u
#define DSBPLAY_LOOPING 1
#define DSBSTATUS_PLAYING 1
#define DSBSTATUS_LOOPING 4
#define DSBLOCK_FROMWRITECURSOR 1
#define DSBLOCK_ENTIREBUFFER 2

typedef struct {
    uint32_t vtable;             /* must be first: the program's view of the object */
    int references;
    int primary;
    /* the format */
    int channels, rate, bits, block, is_float;
    /* a secondary buffer */
    uint8_t *data;
    uint32_t size, flags;
    int playing, looping;
    double position;             /* the frame of the buffer the ring was fed up to */
    double volume;               /* gain */
    uint32_t lock_offset, lock_bytes;      /* the span the sound library has locked last */
    double valid_abs;            /* absolute frame (as fed) up to which the sound library has written */
    double fed_abs;              /* frames fed to the ring since the start */
    double last_unlock_ms;       /* when the sound library wrote last */
    double max_gap_ms;           /* the longest time between two writes since the last report */
    uint64_t underrun_frames, unlocks;
    double fmod_blocked[3];      /* what the thread that writes has spent sleeping / waiting for objects / waiting for the big lock */
    pthread_mutex_t lock;
} Buffer;

typedef struct {
    uint32_t vtable;
    int references;
} Device;

static uint32_t device_vtable[12], buffer_vtable[24];
static int vtables_ready;
static Buffer *playing_buffer;   /* the one the output hook feeds (the sound library has one) */
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
static int hook_added;

static const uint8_t IID_IUnknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
static const uint8_t IID_IDirectSound[16] = {0x83, 0xfa, 0x9a, 0x27, 0x81, 0x49, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60};
static const uint8_t IID_IDirectSound8[16] = {0x93, 0x7e, 0x0a, 0xc5, 0x95, 0xf3, 0x34, 0x48, 0x9e, 0xf6, 0x7f, 0xa9, 0x9d, 0xe5, 0x09, 0x66};
static const uint8_t IID_IDirectSoundBuffer[16] = {0x85, 0xfa, 0x9a, 0x27, 0x81, 0x49, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60};
static const uint8_t IID_IDirectSoundBuffer8[16] = {0x49, 0xa4, 0x25, 0x68, 0x24, 0x75, 0x82, 0x4d, 0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e};

static uint16_t u16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void build_vtables(void)
{
    static const char *const device_names[] = {"DS8_QueryInterface", "DS8_AddRef", "DS8_Release", "DS8_CreateSoundBuffer", "DS8_GetCaps", "DS8_DuplicateSoundBuffer",
                                               "DS8_SetCooperativeLevel", "DS8_Compact", "DS8_GetSpeakerConfig", "DS8_SetSpeakerConfig", "DS8_Initialize", "DS8_VerifyCertification"};
    static const char *const buffer_names[] = {"DSB8_QueryInterface", "DSB8_AddRef", "DSB8_Release", "DSB8_GetCaps", "DSB8_GetCurrentPosition", "DSB8_GetFormat", "DSB8_GetVolume",
                                               "DSB8_GetPan", "DSB8_GetFrequency", "DSB8_GetStatus", "DSB8_Initialize", "DSB8_Lock", "DSB8_Play", "DSB8_SetCurrentPosition",
                                               "DSB8_SetFormat", "DSB8_SetVolume", "DSB8_SetPan", "DSB8_SetFrequency", "DSB8_Stop", "DSB8_Unlock", "DSB8_Restore", "DSB8_SetFX",
                                               "DSB8_AcquireResources", "DSB8_GetObjectInPath"};

    if (vtables_ready) return;
    for (int i = 0; i < 12; i++) device_vtable[i] = SRW_FindProc(device_names[i]);
    for (int i = 0; i < 24; i++) buffer_vtable[i] = SRW_FindProc(buffer_names[i]);
    vtables_ready = 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the feed: the playing secondary buffer into the ring, from the output thread of waveout.c */

static double play_position_frames(Buffer *b)
{
    double frames = b->block > 0 ? (double) (b->size / (uint32_t) b->block) : 1, step = (double) b->rate / SRW_AudioContextRate();
    double p = b->position - (double) SRW_AudioRingPending() * step;

    if (frames <= 0) return 0;
    while (p < 0) p += frames;
    while (p >= frames) p -= frames;
    return p;
}

static void feed_hook(void)
{
    Buffer *b;
    static double first_seen;
    static int reactivated;

    /* the buffer's lock is taken before the global one is given up: a Release that clears playing_buffer then waits for the buffer's
       lock before it frees the buffer */
    pthread_mutex_lock(&global_lock);
    b = playing_buffer;
    if (b != NULL) pthread_mutex_lock(&b->lock);
    pthread_mutex_unlock(&global_lock);
    if (b == NULL) return;
    /* the program takes itself for inactive (all sound muted) until it is told it is active after its sound system has set up its
       handler: after the sound library has been playing for a moment, and once more later in case that was early */
    {
        double now = emscripten_get_now();

        if (first_seen == 0) first_seen = now;
        if (reactivated == 0 && now - first_seen > 1500.0) { SRW_ReactivateWindow(); reactivated = 1; }
        else if (reactivated == 1 && now - first_seen > 5000.0) { SRW_ReactivateWindow(); reactivated = 2; }
    }
    if (b->playing && b->data != NULL && b->block > 0)
    {
        double frames = (double) (b->size / (uint32_t) b->block), step = (double) b->rate / SRW_AudioContextRate();
        uint32_t space = SRW_AudioRingSpace();

        while (space > 0 && b->playing)
        {
            float chunk[256 * 2];
            uint32_t n = space < 256 ? space : 256, produced = 0;

            while (produced < n)
            {
                uint32_t index = (uint32_t) b->position, next;
                float fraction, l0, r0, l1, r1;

                if (index >= (uint32_t) frames)
                {
                    if (!b->looping)
                    {
                        b->playing = 0;
                        break;
                    }
                    b->position -= frames;
                    index = (uint32_t) b->position;
                }
                next = index + 1 < (uint32_t) frames ? index + 1 : (b->looping ? 0 : index);
                fraction = (float) (b->position - index);
                SRW_AudioFrameStereo(b->channels, b->bits, b->is_float, b->data + (size_t) index * b->block, &l0, &r0);
                if (step == 1.0 || next == index)
                {
                    chunk[produced * 2] = l0;
                    chunk[produced * 2 + 1] = r0;
                }
                else
                {
                    SRW_AudioFrameStereo(b->channels, b->bits, b->is_float, b->data + (size_t) next * b->block, &l1, &r1);
                    chunk[produced * 2] = l0 + (l1 - l0) * fraction;
                    chunk[produced * 2 + 1] = r0 + (r1 - r0) * fraction;
                }
                chunk[produced * 2] *= (float) b->volume;
                chunk[produced * 2 + 1] *= (float) b->volume;
                b->position += step;
                produced++;
            }
            if (produced > 0)
            {
                double now_ms = emscripten_get_now();

                /* frames played past what the sound library has written: stale data (the thread that mixes did not get to run in time) */
                double fed_to = b->fed_abs + produced * step;

                if (b->unlocks > 0 && fed_to > b->valid_abs)
                    b->underrun_frames += (uint64_t) (fed_to - (b->valid_abs > b->fed_abs ? b->valid_abs : b->fed_abs));
                b->fed_abs = fed_to;
                if (b->unlocks > 0 && now_ms - b->last_unlock_ms > b->max_gap_ms) b->max_gap_ms = now_ms - b->last_unlock_ms;
            }
            if (produced > 0)
            {
                static float peak;
                static uint64_t pushed;
                static double reported;

                for (uint32_t k = 0; k < produced * 2; k++) if (__builtin_fabsf(chunk[k]) > peak) peak = __builtin_fabsf(chunk[k]);
                pushed += produced;
                if (SRW_LOG_ON(SRW_LOG_SOUND) && emscripten_get_now() - reported > 3000.0)
                {
                    logprintf("Sound: DirectSound feed: %llu frames so far, peak level %.3f in the last seconds; the sound library wrote %llu times, %llu frames played before the sound library had written them, longest wait for a write %.0f ms; the sound library's thread so far: slept %.0f ms, waited for objects %.0f ms, waited for the big lock %.0f ms",
                              (unsigned long long) pushed, peak, (unsigned long long) b->unlocks, (unsigned long long) b->underrun_frames, b->max_gap_ms,
                              b->fmod_blocked[0], b->fmod_blocked[1], b->fmod_blocked[2]);
                    b->max_gap_ms = 0;
                    peak = 0;
                    reported = emscripten_get_now();
                }
                SRW_AudioRingPush(chunk, produced);
            }
            if (produced < n) break;
            space -= produced;
        }
    }
    pthread_mutex_unlock(&b->lock);
}

/* ------------------------------------------------------------------------------------------------------------------ */

static int same(const void *a, const uint8_t *b)
{
    return memcmp(a, b, 16) == 0;
}

static void parse_format(Buffer *b, const uint8_t *format)
{
    int tag = u16(format);

    b->channels = u16(format + 2);
    b->rate = (int) u32(format + 4);
    b->block = u16(format + 12);
    b->bits = u16(format + 14);
    if (tag == 0xfffe && u16(format + 16) >= 22) tag = (int) u32(format + 24);
    b->is_float = tag == 3;
}

uint32_t CCALL DirectSoundCreate_c(void *lpcGuidDevice, void *ppDS, void *pUnkOuter)
{
    Device *d;

    (void) lpcGuidDevice;
    (void) pUnkOuter;
    if (ppDS == NULL) return DSERR_INVALIDPARAM;
    build_vtables();
    d = calloc(1, sizeof(*d));
    d->vtable = (uint32_t) (uintptr_t) device_vtable;
    d->references = 1;
    *(uint32_t *) ppDS = (uint32_t) (uintptr_t) d;
    SRW_LOG(SRW_LOG_SOUND, "Sound: DirectSoundCreate");
    return DS_OK;
}

uint32_t CCALL DirectSoundCreate8_c(void *lpcGuidDevice, void *ppDS8, void *pUnkOuter)
{
    return DirectSoundCreate_c(lpcGuidDevice, ppDS8, pUnkOuter);
}

/* the callback gets the default driver (no GUID) and the one device */
static uint32_t enumerate(uint32_t callback, uint32_t context, int wide)
{
    static const uint8_t guid[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01};
    static const char *const primary_name = "Primary Sound Driver", *device_name = "Web Audio", *module = "webaudio.drv";
    static uint16_t wide_text[3][32];

    if (callback == 0) return DSERR_INVALIDPARAM;
    if (wide)
    {
        const char *texts[3] = {primary_name, device_name, module};

        for (int t = 0; t < 3; t++)
        {
            int i = 0;

            for (; texts[t][i] != 0; i++) wide_text[t][i] = (uint16_t) texts[t][i];
            wide_text[t][i] = 0;
        }
    }
    for (int k = 0; k < 2; k++)
    {
        uint32_t arguments[4];

        arguments[0] = k == 0 ? 0 : (uint32_t) (uintptr_t) guid;
        arguments[1] = wide ? (uint32_t) (uintptr_t) wide_text[k == 0 ? 0 : 1] : (uint32_t) (uintptr_t) (k == 0 ? primary_name : device_name);
        arguments[2] = wide ? (uint32_t) (uintptr_t) wide_text[2] : (uint32_t) (uintptr_t) (k == 0 ? "" : module);
        arguments[3] = context;
        if (k == 0 && wide) wide_text[2][0] = 0;
        if (SRW_CallGuest(callback, 4, arguments) == 0) break;
    }
    return DS_OK;
}

uint32_t CCALL DirectSoundEnumerateA_c(uint32_t callback, uint32_t context) { return enumerate(callback, context, 0); }
uint32_t CCALL DirectSoundEnumerateW_c(uint32_t callback, uint32_t context) { return enumerate(callback, context, 1); }
uint32_t CCALL DirectSoundCaptureEnumerateA_c(uint32_t callback, uint32_t context) { (void) callback; (void) context; return DS_OK; }
uint32_t CCALL DirectSoundCaptureEnumerateW_c(uint32_t callback, uint32_t context) { (void) callback; (void) context; return DS_OK; }
uint32_t CCALL DirectSoundCaptureCreate_c(void *guid, void *pp, void *outer) { (void) guid; (void) outer; if (pp) *(uint32_t *) pp = 0; return DSERR_NODRIVER; }
uint32_t CCALL DirectSoundCaptureCreate8_c(void *guid, void *pp, void *outer) { (void) guid; (void) outer; if (pp) *(uint32_t *) pp = 0; return DSERR_NODRIVER; }

/* ------------------------------------------------------------------------------------------------------------------ */
/* the device */

uint32_t CCALL DS8_QueryInterface_c(uint32_t self, void *iid, void *out)
{
    Device *d = (Device *) (uintptr_t) self;

    if (iid == NULL || out == NULL) return DSERR_INVALIDPARAM;
    if (same(iid, IID_IUnknown) || same(iid, IID_IDirectSound) || same(iid, IID_IDirectSound8))
    {
        __atomic_add_fetch(&d->references, 1, __ATOMIC_SEQ_CST);
        *(uint32_t *) out = self;
        return DS_OK;
    }
    *(uint32_t *) out = 0;
    return E_NOINTERFACE_;
}

uint32_t CCALL DS8_AddRef_c(uint32_t self)
{
    return (uint32_t) __atomic_add_fetch(&((Device *) (uintptr_t) self)->references, 1, __ATOMIC_SEQ_CST);
}

uint32_t CCALL DS8_Release_c(uint32_t self)
{
    Device *d = (Device *) (uintptr_t) self;
    int references = __atomic_sub_fetch(&d->references, 1, __ATOMIC_SEQ_CST);

    if (references == 0) free(d);
    return (uint32_t) references;
}

uint32_t CCALL DS8_CreateSoundBuffer_c(uint32_t self, void *desc, void *out, void *outer)
{
    const uint8_t *description = desc;
    Buffer *b;
    uint32_t flags;

    (void) self;
    (void) outer;
    if (desc == NULL || out == NULL) return DSERR_INVALIDPARAM;
    build_vtables();
    flags = u32(description + 4);
    b = calloc(1, sizeof(*b));
    b->vtable = (uint32_t) (uintptr_t) buffer_vtable;
    b->references = 1;
    b->flags = flags;
    b->volume = 1.0;
    pthread_mutex_init(&b->lock, NULL);
    if (flags & DSBCAPS_PRIMARYBUFFER)
    {
        b->primary = 1;
        b->channels = 2;
        b->rate = 44100;
        b->bits = 16;
        b->block = 4;
    }
    else
    {
        uint32_t format = u32(description + 16);

        b->size = u32(description + 8);
        if (format == 0 || b->size == 0)
        {
            free(b);
            return DSERR_INVALIDPARAM;
        }
        parse_format(b, (const uint8_t *) (uintptr_t) format);
        if (b->block < 1 || b->channels < 1)
        {
            free(b);
            return DSERR_INVALIDPARAM;
        }
        b->size -= b->size % (uint32_t) b->block;
        b->data = calloc(1, b->size + 16);
        if (b->bits == 8 && !b->is_float) memset(b->data, 0x80, b->size);
        SRW_AudioStart();
        SRW_LOG(SRW_LOG_SOUND, "Sound: secondary buffer: %u bytes, %d channels, %d Hz, %d bits%s", (unsigned int) b->size, b->channels, b->rate, b->bits, b->is_float ? " (float)" : "");
    }
    *(uint32_t *) out = (uint32_t) (uintptr_t) b;
    return DS_OK;
}

uint32_t CCALL DS8_GetCaps_c(uint32_t self, void *caps)
{
    uint8_t *c = caps;
    uint32_t size;

    (void) self;
    if (caps == NULL) return DSERR_INVALIDPARAM;
    size = u32(c);
    if (size < 96) return DSERR_INVALIDPARAM;
    memset(c + 4, 0, size - 4);
    {
        uint32_t flags = 0xb5b;   /* mono and stereo, 8 and 16 bits, continuous rate, certified driver */
        uint32_t minimum = 100, maximum = 200000, primaries = 1;

        memcpy(c + 4, &flags, 4);
        memcpy(c + 8, &minimum, 4);
        memcpy(c + 12, &maximum, 4);
        memcpy(c + 16, &primaries, 4);
    }
    return DS_OK;
}

uint32_t CCALL DS8_DuplicateSoundBuffer_c(uint32_t self, uint32_t original, void *out)
{
    (void) self; (void) original;
    if (out) *(uint32_t *) out = 0;
    return DSERR_UNSUPPORTED;
}

uint32_t CCALL DS8_SetCooperativeLevel_c(uint32_t self, uint32_t hwnd, uint32_t level) { (void) self; (void) hwnd; (void) level; return DS_OK; }
uint32_t CCALL DS8_Compact_c(uint32_t self) { (void) self; return DS_OK; }
uint32_t CCALL DS8_GetSpeakerConfig_c(uint32_t self, void *config) { (void) self; if (config) *(uint32_t *) config = 4; return DS_OK; }   /* DSSPEAKER_STEREO */
uint32_t CCALL DS8_SetSpeakerConfig_c(uint32_t self, uint32_t config) { (void) self; (void) config; return DS_OK; }
uint32_t CCALL DS8_Initialize_c(uint32_t self, void *guid) { (void) self; (void) guid; return DS_OK; }
uint32_t CCALL DS8_VerifyCertification_c(uint32_t self, void *certification) { (void) self; if (certification) *(uint32_t *) certification = 0; return DS_OK; }

/* ------------------------------------------------------------------------------------------------------------------ */
/* the buffers */

static Buffer *buffer_of(uint32_t self)
{
    return (Buffer *) (uintptr_t) self;
}

uint32_t CCALL DSB8_QueryInterface_c(uint32_t self, void *iid, void *out)
{
    Buffer *b = buffer_of(self);

    if (iid == NULL || out == NULL) return DSERR_INVALIDPARAM;
    if (same(iid, IID_IUnknown) || same(iid, IID_IDirectSoundBuffer) || same(iid, IID_IDirectSoundBuffer8))
    {
        __atomic_add_fetch(&b->references, 1, __ATOMIC_SEQ_CST);
        *(uint32_t *) out = self;
        return DS_OK;
    }
    *(uint32_t *) out = 0;
    return E_NOINTERFACE_;
}

uint32_t CCALL DSB8_AddRef_c(uint32_t self)
{
    return (uint32_t) __atomic_add_fetch(&buffer_of(self)->references, 1, __ATOMIC_SEQ_CST);
}

uint32_t CCALL DSB8_Release_c(uint32_t self)
{
    Buffer *b = buffer_of(self);
    int references = __atomic_sub_fetch(&b->references, 1, __ATOMIC_SEQ_CST);

    if (references == 0)
    {
        pthread_mutex_lock(&global_lock);
        if (playing_buffer == b) playing_buffer = NULL;
        pthread_mutex_unlock(&global_lock);
        pthread_mutex_lock(&b->lock);   /* (the feed may be in it) */
        pthread_mutex_unlock(&b->lock);
        free(b->data);
        free(b);
    }
    return (uint32_t) references;
}

uint32_t CCALL DSB8_GetCaps_c(uint32_t self, void *caps)
{
    Buffer *b = buffer_of(self);
    uint8_t *c = caps;
    uint32_t values[3];

    if (caps == NULL || u32(c) < 20) return DSERR_INVALIDPARAM;
    values[0] = b->flags;
    values[1] = b->size;
    values[2] = 0;
    memcpy(c + 4, values, 12);
    memset(c + 16, 0, 4);
    return DS_OK;
}

uint32_t CCALL DSB8_GetCurrentPosition_c(uint32_t self, void *play, void *write)
{
    Buffer *b = buffer_of(self);
    uint32_t play_cursor = 0, write_cursor = 0;

    if (b->primary || b->data == NULL)
    {
        if (play) *(uint32_t *) play = 0;
        if (write) *(uint32_t *) write = 0;
        return DS_OK;
    }
    pthread_mutex_lock(&b->lock);
    {
        double frames = (double) (b->size / (uint32_t) b->block), p = b->playing ? play_position_frames(b) : b->position;
        double margin = b->rate / 10.0;   /* the write cursor is a tenth of a second ahead */

        if (margin > frames / 2) margin = frames / 2;
        while (p >= frames) p -= frames;
        play_cursor = (uint32_t) p * (uint32_t) b->block;
        write_cursor = (uint32_t) ((uint32_t) (p + margin) % (uint32_t) frames) * (uint32_t) b->block;
        if (!b->playing) write_cursor = play_cursor;
    }
    pthread_mutex_unlock(&b->lock);
    if (play) *(uint32_t *) play = play_cursor;
    if (write) *(uint32_t *) write = write_cursor;
    return DS_OK;
}

uint32_t CCALL DSB8_GetFormat_c(uint32_t self, void *format, uint32_t size, void *written)
{
    Buffer *b = buffer_of(self);
    uint8_t f[18];
    uint32_t value;

    memset(f, 0, sizeof(f));
    f[0] = b->is_float ? 3 : 1;
    f[2] = (uint8_t) b->channels;
    value = (uint32_t) b->rate;
    memcpy(f + 4, &value, 4);
    value = (uint32_t) (b->rate * b->block);
    memcpy(f + 8, &value, 4);
    f[12] = (uint8_t) b->block;
    f[14] = (uint8_t) b->bits;
    if (format != NULL) memcpy(format, f, size < sizeof(f) ? size : sizeof(f));
    if (written != NULL) *(uint32_t *) written = sizeof(f);
    return DS_OK;
}

uint32_t CCALL DSB8_GetVolume_c(uint32_t self, void *volume) { (void) self; if (volume) *(int32_t *) volume = 0; return DS_OK; }
uint32_t CCALL DSB8_GetPan_c(uint32_t self, void *pan) { (void) self; if (pan) *(int32_t *) pan = 0; return DS_OK; }
uint32_t CCALL DSB8_GetFrequency_c(uint32_t self, void *frequency) { if (frequency) *(uint32_t *) frequency = (uint32_t) buffer_of(self)->rate; return DS_OK; }

uint32_t CCALL DSB8_GetStatus_c(uint32_t self, void *status)
{
    Buffer *b = buffer_of(self);

    if (status == NULL) return DSERR_INVALIDPARAM;
    *(uint32_t *) status = (b->playing ? DSBSTATUS_PLAYING : 0) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0);
    return DS_OK;
}

uint32_t CCALL DSB8_Initialize_c(uint32_t self, uint32_t device, void *desc) { (void) self; (void) device; (void) desc; return DS_OK; }

/* the two spans of the buffer from the offset on (the second one when the span wraps around) */
uint32_t CCALL DSB8_Lock_c(uint32_t self, uint32_t offset, uint32_t bytes, void *ptr1, void *bytes1, void *ptr2, void *bytes2, uint32_t flags)
{
    Buffer *b = buffer_of(self);
    uint32_t first, second = 0;

    if (b->data == NULL || ptr1 == NULL || bytes1 == NULL) return DSERR_INVALIDPARAM;
    if (flags & DSBLOCK_FROMWRITECURSOR)
    {
        uint32_t write = 0;

        DSB8_GetCurrentPosition_c(self, NULL, &write);
        offset = write;
    }
    if (flags & DSBLOCK_ENTIREBUFFER)
    {
        offset = 0;
        bytes = b->size;
    }
    if (offset >= b->size || bytes > b->size) return DSERR_INVALIDPARAM;
    first = bytes < b->size - offset ? bytes : b->size - offset;
    second = bytes - first;
    b->lock_offset = offset;
    b->lock_bytes = bytes;
    *(uint32_t *) ptr1 = (uint32_t) (uintptr_t) (b->data + offset);
    *(uint32_t *) bytes1 = first;
    if (ptr2 != NULL) *(uint32_t *) ptr2 = second ? (uint32_t) (uintptr_t) b->data : 0;
    if (bytes2 != NULL) *(uint32_t *) bytes2 = second;
    return DS_OK;
}

uint32_t CCALL DSB8_Unlock_c(uint32_t self, void *ptr1, uint32_t bytes1, void *ptr2, uint32_t bytes2)
{
    Buffer *b = buffer_of(self);

    (void) ptr1; (void) bytes1; (void) ptr2; (void) bytes2;
    if (!b->primary && b->data != NULL && b->block > 0 && b->size >= (uint32_t) b->block)
    {
        pthread_mutex_lock(&b->lock);
        {
            double frames = (double) (b->size / (uint32_t) b->block), end = (double) ((b->lock_offset + b->lock_bytes) / (uint32_t) b->block);
            double ahead = end - b->position;

            while (ahead < 0) ahead += frames;
            while (ahead > frames) ahead -= frames;
            b->valid_abs = b->fed_abs + ahead;
            b->last_unlock_ms = emscripten_get_now();
            SRW_BlockedPeek(b->fmod_blocked);
            b->unlocks++;
        }
        pthread_mutex_unlock(&b->lock);
    }
    return DS_OK;
}

uint32_t CCALL DSB8_Play_c(uint32_t self, uint32_t reserved1, uint32_t priority, uint32_t flags)
{
    Buffer *b = buffer_of(self);

    (void) reserved1;
    (void) priority;
    if (b->primary) return DS_OK;
    pthread_mutex_lock(&global_lock);
    playing_buffer = b;
    if (!hook_added)
    {
        hook_added = 1;
        SRW_AudioAddHook(feed_hook);
    }
    pthread_mutex_unlock(&global_lock);
    pthread_mutex_lock(&b->lock);
    b->looping = (flags & DSBPLAY_LOOPING) != 0;
    b->playing = 1;
    pthread_mutex_unlock(&b->lock);
    SRW_LOG(SRW_LOG_SOUND, "Sound: DirectSound buffer plays%s", b->looping ? " (looping)" : "");
    return DS_OK;
}

uint32_t CCALL DSB8_SetCurrentPosition_c(uint32_t self, uint32_t position)
{
    Buffer *b = buffer_of(self);

    if (b->primary || b->block < 1) return DS_OK;
    pthread_mutex_lock(&b->lock);
    b->position = (double) (position / (uint32_t) b->block);
    pthread_mutex_unlock(&b->lock);
    return DS_OK;
}

uint32_t CCALL DSB8_SetFormat_c(uint32_t self, void *format)
{
    Buffer *b = buffer_of(self);

    if (format == NULL) return DSERR_INVALIDPARAM;
    if (!b->primary) return DSERR_INVALIDPARAM;
    parse_format(b, format);
    return DS_OK;
}

/* hundredths of a decibel: gain = 10^(v / 2000) */
uint32_t CCALL DSB8_SetVolume_c(uint32_t self, uint32_t volume)
{
    Buffer *b = buffer_of(self);
    int32_t v = (int32_t) volume;
    double gain = 1.0;

    if (v <= -10000) gain = 0.0;
    else if (v < 0) gain = __builtin_pow(10.0, v / 2000.0);
    b->volume = gain;
    return DS_OK;
}

uint32_t CCALL DSB8_SetPan_c(uint32_t self, uint32_t pan) { (void) self; (void) pan; return DS_OK; }

uint32_t CCALL DSB8_SetFrequency_c(uint32_t self, uint32_t frequency)
{
    Buffer *b = buffer_of(self);

    if (!b->primary && frequency >= 100 && frequency <= 200000) b->rate = (int) frequency;
    return DS_OK;
}

uint32_t CCALL DSB8_Stop_c(uint32_t self)
{
    Buffer *b = buffer_of(self);

    pthread_mutex_lock(&b->lock);
    b->playing = 0;
    pthread_mutex_unlock(&b->lock);
    return DS_OK;
}

uint32_t CCALL DSB8_Restore_c(uint32_t self) { (void) self; return DS_OK; }
uint32_t CCALL DSB8_SetFX_c(uint32_t self, uint32_t count, void *effects, void *results) { (void) self; (void) count; (void) effects; (void) results; return DSERR_UNSUPPORTED; }
uint32_t CCALL DSB8_AcquireResources_c(uint32_t self, uint32_t flags, uint32_t count, void *results) { (void) self; (void) flags; (void) count; (void) results; return DSERR_UNSUPPORTED; }
uint32_t CCALL DSB8_GetObjectInPath_c(uint32_t self, void *guid, uint32_t index, void *iid, void *out) { (void) self; (void) guid; (void) index; (void) iid; (void) out; return DSERR_UNSUPPORTED; }
