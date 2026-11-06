/* The wave output of Windows (WinMM) for a program with a sound library of its own: it opens a wave device, queues
   buffers (WAVEHDR) and waits for each to be played before it mixes the next one into it. The sound goes to the page the way a sound
   library that is replaced by the runtime does (Module.srwAudio in web/runtime.js reads stereo floats from a ring buffer of the shared memory at the sample
   rate of the page's audio context).

   An output thread converts the queued buffers (format, channels, resampling) into the ring as far as it has room (a little, to keep
   the delay short), and reports a buffer played (WHDR_DONE, the callback or event) when the page has read everything up to its
   end: that is what keeps the program's mixer in step with the real time. Without a page (headless) the time goes by as if the
   sound was played. timeSetEvent (the program's timers) runs its callbacks from a thread of its own. */
#include <emscripten.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "srw_debuglog.h"
#include "srw.h"
#include "imports.h"

uint32_t SRW_LastCaller(void);

#define RING_FRAMES 16384        /* power of two */
#define FILL_LIMIT 4096          /* frames the ring is filled to: the delay added to what the program queues */
#define MAX_QUEUE 64
#define MAX_TIMERS 16

#define WHDR_DONE 0x01
#define WHDR_PREPARED 0x02
#define WHDR_INQUEUE 0x10
#define CALLBACK_TYPEMASK 0x70000
#define CALLBACK_WINDOW 0x10000
#define CALLBACK_FUNCTION 0x30000
#define CALLBACK_EVENT 0x50000
#define WAVE_FORMAT_QUERY 1
#define WOM_OPEN 0x3bb
#define WOM_CLOSE 0x3bc
#define WOM_DONE 0x3bd
#define MMSYSERR_NOERROR 0
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_ALLOCATED 4
#define MMSYSERR_INVALHANDLE 5
#define MMSYSERR_NOMEM 7
#define MMSYSERR_INVALPARAM 11
#define WAVERR_BADFORMAT 32
#define WAVERR_STILLPLAYING 33
#define WAVERR_UNPREPARED 34
#define DEVICE_HANDLE 0x7e01u

typedef struct {
    uint32_t header;             /* the WAVEHDR in guest memory */
    double position;             /* source frames consumed so far */
    int written;                 /* all of it is in the ring */
    uint32_t end_frame;          /* the ring's write counter after its last frame */
} Item;

typedef struct {
    uint32_t callback, instance, flags;
    int channels, rate, bits, block, is_float;
    Item queue[MAX_QUEUE];
    int count;
    int fill;                    /* the index of the first item not fully in the ring */
    double pushed;               /* source frames pushed into the ring, all in all */
} Device;

typedef struct {
    uint32_t type, handle, instance, callback, flags, header;
} Notice;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Device device;
static int opened;
static float ring[RING_FRAMES * 2];
static _Atomic uint32_t ring_read, ring_write;
static int context_rate = 48000, have_output;
static pthread_t output_thread;
static int output_started;

static uint16_t u16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* what the program wants to be told: called without the lock */
static void notify(const Notice *n)
{
    switch (n->flags & CALLBACK_TYPEMASK)
    {
        case CALLBACK_FUNCTION:
            if (n->callback != 0)
            {
                uint32_t arguments[5] = {n->handle, n->type, n->instance, n->header, 0};

                SRW_GilAcquire();
                SRW_CallGuest(n->callback, 5, arguments);
                SRW_GilRelease();
            }
            break;
        case CALLBACK_EVENT:
            SetEvent_c(n->callback);
            break;
        case CALLBACK_WINDOW:
            PostMessageA_c(n->callback, n->type, n->handle, n->header);
            break;
        default:
            break;
    }
}

/* the sample i of the frame of a buffer as a float */
static float sample_at(int bits, int is_float, const uint8_t *frame, int channel)
{
    if (is_float) return bits == 64 ? (float) *(const double *) (frame + channel * 8) : *(const float *) (frame + channel * 4);
    switch (bits)
    {
        case 8: return ((int) frame[channel] - 128) / 128.0f;
        case 24: return (float) (((int32_t) ((frame[channel * 3] << 8) | (frame[channel * 3 + 1] << 16) | ((uint32_t) frame[channel * 3 + 2] << 24))) >> 8) / 8388608.0f;
        case 32: return (float) ((const int32_t *) frame)[channel] / 2147483648.0f;
        default: return (float) ((const int16_t *) (const void *) frame)[channel] / 32768.0f;
    }
}

void SRW_AudioFrameStereo(int channels, int bits, int is_float, const uint8_t *frame, float *left, float *right)
{
    if (channels == 1)
    {
        *left = *right = sample_at(bits, is_float, frame, 0);
    }
    else if (channels == 2)
    {
        *left = sample_at(bits, is_float, frame, 0);
        *right = sample_at(bits, is_float, frame, 1);
    }
    else
    {
        /* surround: front pair, the centre and the rear pair folded in */
        float l = sample_at(bits, is_float, frame, 0), r = sample_at(bits, is_float, frame, 1), c = channels > 2 ? sample_at(bits, is_float, frame, 2) : 0;

        *left = l + 0.7f * c;
        *right = r + 0.7f * c;
        if (channels >= 7)
        {
            *left += 0.5f * sample_at(bits, is_float, frame, 4) + 0.5f * sample_at(bits, is_float, frame, 6);
            *right += 0.5f * sample_at(bits, is_float, frame, 5) + 0.5f * (channels > 7 ? sample_at(bits, is_float, frame, 7) : sample_at(bits, is_float, frame, 6));
        }
        else if (channels >= 5)
        {
            *left += 0.7f * sample_at(bits, is_float, frame, 4);
            *right += 0.7f * sample_at(bits, is_float, frame, 5 < channels ? 5 : 4);
        }
    }
}

/* pushes what fits of the first item that is not in the ring yet; returns whether it did something */
static int push_items(Device *d)
{
    int did = 0;

    while (d->fill < d->count)
    {
        Item *item = &d->queue[d->fill];
        const uint8_t *header = (const uint8_t *) (uintptr_t) item->header;
        const uint8_t *data = (const uint8_t *) (uintptr_t) u32(header);
        uint32_t length = u32(header + 4);
        double frames = d->block > 0 ? (double) (length / (uint32_t) d->block) : 0;
        double step = (double) d->rate / context_rate;
        uint32_t read = atomic_load(&ring_read), write = atomic_load(&ring_write);

        while (item->position < frames && write - read < FILL_LIMIT)
        {
            uint32_t index = (uint32_t) item->position, next = index + 1 < frames ? index + 1 : index;
            float fraction = (float) (item->position - index), l0, r0, l1, r1;
            float *out = ring + (write & (RING_FRAMES - 1)) * 2;

            SRW_AudioFrameStereo(d->channels, d->bits, d->is_float, data + (size_t) index * d->block, &l0, &r0);
            if (step == 1.0 || next == index)
            {
                out[0] = l0;
                out[1] = r0;
            }
            else
            {
                SRW_AudioFrameStereo(d->channels, d->bits, d->is_float, data + (size_t) next * d->block, &l1, &r1);
                out[0] = l0 + (l1 - l0) * fraction;
                out[1] = r0 + (r1 - r0) * fraction;
            }
            item->position += step;
            d->pushed += step;
            write++;
            did = 1;
        }
        atomic_store_explicit(&ring_write, write, memory_order_release);
        if (item->position < frames) break;   /* the ring is full */
        item->written = 1;
        item->end_frame = write;
        d->fill++;
    }
    return did;
}

#define MAX_HOOKS 4
static void (*hooks[MAX_HOOKS])(void);
static int hook_count;

static void *output_main(void *argument)
{
    double last = emscripten_get_now();

    (void) argument;
    for (;;)
    {
        Notice notices[MAX_QUEUE];
        int n = 0;
        double now;

        usleep(3000);
        now = emscripten_get_now();
        if (!have_output)
        {
            /* nowhere to play: what is in the ring is gone as fast as it would have been played */
            uint32_t read = atomic_load(&ring_read), write = atomic_load(&ring_write);
            uint32_t gone = (uint32_t) ((now - last) * context_rate / 1000.0);

            if (gone > write - read) gone = write - read;
            atomic_store(&ring_read, read + gone);
        }
        last = now;
        pthread_mutex_lock(&lock);
        if (opened)
        {
            Device *d = &device;

            push_items(d);
            while (d->count > 0 && d->queue[0].written && (int32_t) (atomic_load(&ring_read) - d->queue[0].end_frame) >= 0)
            {
                uint8_t *header = (uint8_t *) (uintptr_t) d->queue[0].header;
                uint32_t flags = u32(header + 16);

                flags = (flags | WHDR_DONE) & ~(uint32_t) WHDR_INQUEUE;
                memcpy(header + 16, &flags, 4);
                notices[n++] = (Notice) {WOM_DONE, DEVICE_HANDLE, d->instance, d->callback, d->flags, d->queue[0].header};
                memmove(&d->queue[0], &d->queue[1], (size_t) (d->count - 1) * sizeof(Item));
                d->count--;
                if (d->fill > 0) d->fill--;
            }
        }
        pthread_mutex_unlock(&lock);
        for (int i = 0; i < n; i++) notify(&notices[i]);
        for (int i = 0; i < hook_count; i++) hooks[i]();
    }
    return NULL;
}

void SRW_AudioStart(void)
{
    int rate;

    if (output_started) return;
    rate = MAIN_THREAD_EM_ASM_INT({ return Module['srwAudio'] ? Module['srwAudio'].start($0, $1, $2, $3) : 0; }, ring, &ring_read, &ring_write, RING_FRAMES);
    have_output = rate > 0;
    if (have_output) context_rate = rate;
    output_started = 1;
    pthread_create(&output_thread, NULL, output_main, NULL);
    logprintf("Sound: wave output %s, %d Hz", have_output ? "to the page" : "without a page (the time goes by)", context_rate);
}


/* ------------------------------------------------------------------------------------------------------------------ */
/* what the other users of the page's sound (the DirectSound of dsound.c) take from here */

int SRW_AudioContextRate(void)
{
    return context_rate;
}

uint32_t SRW_AudioRingPending(void)
{
    return atomic_load(&ring_write) - atomic_load(&ring_read);
}

/* how many frames may be pushed now (the ring is filled only so far, to keep the delay short) */
uint32_t SRW_AudioRingSpace(void)
{
    uint32_t pending = SRW_AudioRingPending();

    return pending >= FILL_LIMIT ? 0 : FILL_LIMIT - pending;
}

void SRW_AudioRingPush(const float *stereo, uint32_t frames)
{
    uint32_t write = atomic_load(&ring_write);

    for (uint32_t i = 0; i < frames; i++)
    {
        float *out = ring + ((write + i) & (RING_FRAMES - 1)) * 2;

        out[0] = stereo[i * 2];
        out[1] = stereo[i * 2 + 1];
    }
    atomic_store_explicit(&ring_write, write + frames, memory_order_release);
}

void SRW_AudioAddHook(void (*hook)(void))
{
    if (hook_count < MAX_HOOKS) hooks[hook_count++] = hook;
}

/* ------------------------------------------------------------------------------------------------------------------ */

uint32_t CCALL waveOutGetNumDevs_c(void)
{
    { static int logged; if (SRW_LOG_ON(SRW_LOG_SOUND) && logged++ < 6) logprintf("Sound: waveOutGetNumDevs from %x", (unsigned int) SRW_LastCaller()); }
    return 1;
}

uint32_t CCALL waveOutGetDevCapsA_c(uint32_t uDeviceID, void *pwoc, uint32_t cbwoc)
{
    uint8_t caps[52];

    SRW_LOG(SRW_LOG_SOUND, "Sound: waveOutGetDevCaps(%u)", (unsigned int) uDeviceID);

    if (uDeviceID != 0 && uDeviceID != 0xffffffffu) return MMSYSERR_BADDEVICEID;
    memset(caps, 0, sizeof(caps));
    caps[0] = 1;                       /* wMid */
    caps[2] = 1;                       /* wPid */
    caps[4] = 0x00; caps[5] = 0x05;    /* vDriverVersion 5.0 */
    memcpy(caps + 8, "Web Audio", 10);
    caps[40] = 0xff; caps[41] = 0x0f;  /* dwFormats: the 11, 22, 44 and 48 kHz formats */
    caps[44] = 2;                      /* wChannels */
    caps[48] = 0x26;                   /* dwSupport: volume, left/right volume, sample accurate */
    memcpy(pwoc, caps, cbwoc < sizeof(caps) ? cbwoc : sizeof(caps));
    return MMSYSERR_NOERROR;
}

uint32_t CCALL waveOutOpen_c(void *phwo, uint32_t uDeviceID, void *pwfx, uint32_t dwCallback, uint32_t dwInstance, uint32_t fdwOpen)
{
    const uint8_t *format = pwfx;
    int tag, channels, rate, bits, block, is_float = 0;
    Notice notice;

    if (uDeviceID != 0 && uDeviceID != 0xffffffffu) return MMSYSERR_BADDEVICEID;
    if (format == NULL) return MMSYSERR_INVALPARAM;
    tag = u16(format);
    channels = u16(format + 2);
    rate = (int) u32(format + 4);
    block = u16(format + 12);
    bits = u16(format + 14);
    if (tag == 0xfffe && u16(format + 16) >= 22) tag = (int) u32(format + 24);   /* WAVEFORMATEXTENSIBLE: the sub format */
    if (tag == 3) is_float = 1;
    if ((tag != 1 && tag != 3) || channels < 1 || channels > 8 || rate < 4000 || rate > 192000 || block < 1 ||
        (!is_float && bits != 8 && bits != 16 && bits != 24 && bits != 32) || (is_float && bits != 32 && bits != 64))
    {
        SRW_LOG(SRW_LOG_SOUND, "Sound: waveOutOpen refuses the format: tag %d, %d channels, %d Hz, %d bits", tag, channels, rate, bits);
        return WAVERR_BADFORMAT;
    }
    if (fdwOpen & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
    pthread_mutex_lock(&lock);
    if (opened)
    {
        pthread_mutex_unlock(&lock);
        return MMSYSERR_ALLOCATED;
    }
    memset(&device, 0, sizeof(device));
    device.callback = dwCallback;
    device.instance = dwInstance;
    device.flags = fdwOpen;
    device.channels = channels;
    device.rate = rate;
    device.bits = bits;
    device.block = block;
    device.is_float = is_float;
    opened = 1;
    pthread_mutex_unlock(&lock);
    SRW_AudioStart();
    logprintf("Sound: waveOutOpen: %d channels, %d Hz, %d bits%s, callback type %x", channels, rate, bits, is_float ? " (float)" : "", (unsigned int) (fdwOpen & CALLBACK_TYPEMASK));
    if (phwo != NULL) *(uint32_t *) phwo = DEVICE_HANDLE;
    notice = (Notice) {WOM_OPEN, DEVICE_HANDLE, dwInstance, dwCallback, fdwOpen, 0};
    notify(&notice);
    return MMSYSERR_NOERROR;
}

uint32_t CCALL waveOutPrepareHeader_c(uint32_t hwo, void *pwh, uint32_t cbwh)
{
    uint8_t *header = pwh;
    uint32_t flags;

    (void) cbwh;
    if (hwo != DEVICE_HANDLE || !opened) return MMSYSERR_INVALHANDLE;
    flags = u32(header + 16) | WHDR_PREPARED;
    memcpy(header + 16, &flags, 4);
    return MMSYSERR_NOERROR;
}

uint32_t CCALL waveOutUnprepareHeader_c(uint32_t hwo, void *pwh, uint32_t cbwh)
{
    uint8_t *header = pwh;
    uint32_t flags = u32(header + 16);

    (void) cbwh;
    if (hwo != DEVICE_HANDLE) return MMSYSERR_INVALHANDLE;
    if (flags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    flags &= ~(uint32_t) WHDR_PREPARED;
    memcpy(header + 16, &flags, 4);
    return MMSYSERR_NOERROR;
}

uint32_t CCALL waveOutWrite_c(uint32_t hwo, void *pwh, uint32_t cbwh)
{
    uint8_t *header = pwh;
    uint32_t flags;

    (void) cbwh;
    if (hwo != DEVICE_HANDLE || !opened) return MMSYSERR_INVALHANDLE;
    flags = u32(header + 16);
    if (!(flags & WHDR_PREPARED)) return WAVERR_UNPREPARED;
    if (flags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    pthread_mutex_lock(&lock);
    if (device.count >= MAX_QUEUE)
    {
        pthread_mutex_unlock(&lock);
        return MMSYSERR_NOMEM;
    }
    flags = (flags | WHDR_INQUEUE) & ~(uint32_t) WHDR_DONE;
    memcpy(header + 16, &flags, 4);
    device.queue[device.count] = (Item) {(uint32_t) (uintptr_t) pwh, 0, 0, 0};
    device.count++;
    pthread_mutex_unlock(&lock);
    return MMSYSERR_NOERROR;
}

/* everything that is queued is "played" at once; what the ring holds is dropped */
static void reset_locked(Notice *notices, int *n)
{
    Device *d = &device;

    for (int i = 0; i < d->count; i++)
    {
        uint8_t *header = (uint8_t *) (uintptr_t) d->queue[i].header;
        uint32_t flags = (u32(header + 16) | WHDR_DONE) & ~(uint32_t) WHDR_INQUEUE;

        memcpy(header + 16, &flags, 4);
        notices[(*n)++] = (Notice) {WOM_DONE, DEVICE_HANDLE, d->instance, d->callback, d->flags, d->queue[i].header};
    }
    d->count = 0;
    d->fill = 0;
    atomic_store(&ring_write, atomic_load(&ring_read));
}

uint32_t CCALL waveOutReset_c(uint32_t hwo)
{
    Notice notices[MAX_QUEUE];
    int n = 0;

    if (hwo != DEVICE_HANDLE || !opened) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&lock);
    reset_locked(notices, &n);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) notify(&notices[i]);
    return MMSYSERR_NOERROR;
}

uint32_t CCALL waveOutClose_c(uint32_t hwo)
{
    Notice notices[MAX_QUEUE + 1];
    int n = 0;

    if (hwo != DEVICE_HANDLE || !opened) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&lock);
    reset_locked(notices, &n);
    notices[n++] = (Notice) {WOM_CLOSE, DEVICE_HANDLE, device.instance, device.callback, device.flags, 0};
    opened = 0;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) notify(&notices[i]);
    return MMSYSERR_NOERROR;
}

/* MMTIME: the type, then the milliseconds, samples or bytes played */
uint32_t CCALL waveOutGetPosition_c(uint32_t hwo, void *pmmt, uint32_t cbmmt)
{
    uint32_t *time = pmmt;
    double played;
    uint32_t type;

    (void) cbmmt;
    if (hwo != DEVICE_HANDLE || !opened) return MMSYSERR_INVALHANDLE;
    pthread_mutex_lock(&lock);
    played = device.pushed - (double) (atomic_load(&ring_write) - atomic_load(&ring_read)) * device.rate / context_rate;
    if (played < 0) played = 0;
    type = time[0];
    if (type == 1) time[1] = (uint32_t) (played * 1000.0 / device.rate);
    else if (type == 4) time[1] = (uint32_t) played * (uint32_t) device.block;
    else
    {
        time[0] = 2;   /* TIME_SAMPLES */
        time[1] = (uint32_t) played;
    }
    pthread_mutex_unlock(&lock);
    return MMSYSERR_NOERROR;
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* the program's multimedia timers: the callback runs in a thread of its own, as on Windows */

typedef struct {
    int active;
    uint32_t id, proc, user, flags;
    double period, due;
} Timer;

static Timer timers[MAX_TIMERS];
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t timer_thread;
static int timer_started;
static uint32_t next_timer_id = 1;

static void *timer_main(void *argument)
{
    (void) argument;
    for (;;)
    {
        uint32_t fire[MAX_TIMERS][4];
        int n = 0;
        double now;

        usleep(1000);
        now = emscripten_get_now();
        pthread_mutex_lock(&timer_lock);
        for (int i = 0; i < MAX_TIMERS; i++)
        {
            Timer *t = &timers[i];

            if (!t->active || now < t->due) continue;
            fire[n][0] = t->id;
            fire[n][1] = t->proc;
            fire[n][2] = t->user;
            fire[n][3] = t->flags;
            n++;
            if (t->flags & 1) t->due += t->period > 1 ? t->period : 1;   /* TIME_PERIODIC */
            else t->active = 0;
            if (t->due < now - 100) t->due = now;
        }
        pthread_mutex_unlock(&timer_lock);
        for (int i = 0; i < n; i++)
        {
            uint32_t arguments[5] = {fire[i][0], 0, fire[i][2], 0, 0};

            SRW_GilAcquire();
            SRW_CallGuest(fire[i][1], 5, arguments);
            SRW_GilRelease();
        }
    }
    return NULL;
}

uint32_t CCALL timeSetEvent_c(uint32_t uDelay, uint32_t uResolution, void *lpTimeProc, uint32_t dwUser, uint32_t fuEvent)
{
    uint32_t id = 0;

    (void) uResolution;
    if (lpTimeProc == NULL) return 0;
    pthread_mutex_lock(&timer_lock);
    for (int i = 0; i < MAX_TIMERS; i++)
    {
        if (timers[i].active) continue;
        id = next_timer_id++;
        timers[i] = (Timer) {1, id, (uint32_t) (uintptr_t) lpTimeProc, dwUser, fuEvent, (double) uDelay, emscripten_get_now() + uDelay};
        break;
    }
    if (id != 0 && !timer_started)
    {
        timer_started = 1;
        pthread_create(&timer_thread, NULL, timer_main, NULL);
    }
    pthread_mutex_unlock(&timer_lock);
    return id;
}

uint32_t CCALL timeKillEvent_c(uint32_t uTimerID)
{
    uint32_t result = 97;   /* MMSYSERR_INVALPARAM-ish: TIMERR_NOCANDO */

    pthread_mutex_lock(&timer_lock);
    for (int i = 0; i < MAX_TIMERS; i++)
    {
        if (timers[i].active && timers[i].id == uTimerID)
        {
            timers[i].active = 0;
            result = 0;
        }
    }
    pthread_mutex_unlock(&timer_lock);
    return result;
}
