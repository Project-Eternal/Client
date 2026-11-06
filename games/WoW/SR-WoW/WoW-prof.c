/* The frame time pie chart (the chat command .prof): where the time of the last frames of the game thread went. Off by default, and
   then every hook is one test of SRW_ProfOn. The game thread (the one that presents the frames) is always in one category; the hooks
   switch it (SRW_ProfEnter / SRW_ProfLeave, nested: the time goes to the innermost one), the start of each Present closes a frame.
   The sums of the last 60 frames are in the shared memory (microseconds) for the page, which draws the pie (web/prerun.js,
   Module.wowProf; the address is passed when the chart is first switched on). */
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#include "srw_debuglog.h"
#include "WoW.h"

#define WINDOW 60
#define DEPTH 24


static const char *const names[WOW_PROF_COUNT] = { "Client code", "Draw calls", "Other Direct3D calls", "Shader compiles", "Uploads to WebGL", "Present",
                                               "Waiting for the big lock", "Sleep", "Waiting for objects", "Data reads", "Server's world questions" };

/* [0] on, [1] sequence, [2] frames, [3] total microseconds, [4] address of the procedure that sleeps most, [5...] microseconds per category */
static volatile int32_t shared[8 + WOW_PROF_COUNT];

static int current, depth, overflow, stack[DEPTH], fresh;   /* fresh: switched on, no baseline of the counters yet */
static double last, accumulated[WOW_PROF_COUNT];
static double ring[WINDOW][WOW_PROF_COUNT];
static int ring_count, ring_next;
static uint32_t sleep_callers[4], sleep_counts[4];   /* the callers of Sleep in this window, for the legend */

static void charge(double now)
{
    accumulated[current] += now - last;
    last = now;
}

void SRW_ProfSet(int on)
{
    static int told;

    if (on && !told)
    {
        told = 1;
        MAIN_THREAD_ASYNC_EM_ASM({ Module['wowProf'] = $0; }, shared);
    }
    if (on)
    {
        memset(accumulated, 0, sizeof(accumulated));
        memset(ring, 0, sizeof(ring));
        ring_count = ring_next = depth = overflow = 0;
        current = SRW_PROF_PROGRAM;
        last = emscripten_get_now();
        fresh = 1;
    }
    shared[0] = on;
    SRW_ProfOn = on;
}

void SRW_ProfEnter(int category)
{
    if (!SRW_GilIsGame()) return;
    charge(emscripten_get_now());
    if (depth < DEPTH) stack[depth++] = current; else overflow++;
    current = category;
}

void SRW_ProfLeave(void)
{
    if (!SRW_GilIsGame()) return;
    charge(emscripten_get_now());
    if (overflow > 0) overflow--;
    else if (depth > 0) current = stack[--depth];
}

/* the caller of a Sleep of the game thread (diagnostics builds know it): the legend names the one that sleeps most */
void SRW_ProfNoteSleep(uint32_t caller)
{
    int i;

    for (i = 0; i < 3 && sleep_counts[i] != 0 && sleep_callers[i] != caller; i++) {}
    sleep_callers[i] = caller;
    sleep_counts[i]++;
}

extern volatile uint32_t srw_remote_reads, srw_remote_ms, srw_remote_kb, srw_remote_cached_kb;
extern volatile uint64_t srw_remote_asked;
extern volatile uint64_t wow_world_batches;    /* WoW-world.c: batches of the server's world questions run at safe points, ever */
extern volatile double wow_world_waited_ms;    /* and how long they waited for it, in all */
void WoW_OpenCounts(uint32_t *out);
#ifdef SRW_DIAGNOSTICS
int SRW_TakeLongHold(double *milliseconds, double *ended, char *text, size_t size, uint32_t *count, double *total);
int SRW_SampleOtherThreads(const char **names, int *counts, int n, int max);
void SRW_TakeSliceStats(double *total, uint32_t *count, double *longest);
int SRW_TakeWhere(uint32_t *places, double *ms, int max);
int SRW_TakeHot(uint32_t *places, double *ms, int max);
int SRW_TakeThreadSlices(uint32_t *tags, uint32_t *slices, double *ms, int max, uint32_t *gaps, double *gap_ms);
#endif

/* counters of the whole program (all threads) at the start of the frame; the slow frame report takes the difference */
typedef struct { double start; uint32_t reads, ms, kb, cached_kb, opens[4]; uint64_t asked, world; double world_waited; } Baseline;

static void take_baseline(Baseline *b, double now)
{
    b->start = now;
    b->reads = srw_remote_reads;
    b->ms = srw_remote_ms;
    b->kb = srw_remote_kb;
    b->cached_kb = srw_remote_cached_kb;
    b->asked = srw_remote_asked;
    b->world = wow_world_batches;
    b->world_waited = wow_world_waited_ms;
    WoW_OpenCounts(b->opens);
}

/* a frame that took long: where its time went (the three or four largest) and what the program did meanwhile */
static void report_slow_frame(const Baseline *was, const Baseline *is, const double *categories, double total)
{
    static int lines;
    char line[1500];
    size_t n;
    int i, order[WOW_PROF_COUNT];

    if (lines >= 400 || (lines >= 100 && total < 300.0)) return;   /* (after 100 lines only the long ones: the log stays readable) */
    lines++;
    for (i = 0; i < WOW_PROF_COUNT; i++) order[i] = i;
    for (i = 0; i < WOW_PROF_COUNT; i++)
    {
        int k, best = i;

        for (k = i + 1; k < WOW_PROF_COUNT; k++) if (categories[order[k]] > categories[order[best]]) best = k;
        k = order[i]; order[i] = order[best]; order[best] = k;
    }
    n = (size_t) snprintf(line, sizeof(line), "Slow frame %.0f ms:", total);
    for (i = 0; i < 4 && categories[order[i]] > 0.5; i++)
        n += (size_t) snprintf(line + n, sizeof(line) - n, " %s %.0f ms (%.0f%%),", names[order[i]], categories[order[i]], 100.0 * categories[order[i]] / total);
    n += (size_t) snprintf(line + n, sizeof(line) - n,
                           " | in that time: the program asked for %u KB of data files; %u data downloads (%u ms waited in all, %u KB from the network, %u KB from the browser's cache), opened %u terrain tiles, %u textures, %u models, %u other files",
                           (unsigned int) ((is->asked - was->asked) / 1024), (unsigned int) (is->reads - was->reads), (unsigned int) (is->ms - was->ms),
                           (unsigned int) ((is->kb - was->kb) - (is->cached_kb - was->cached_kb)), (unsigned int) (is->cached_kb - was->cached_kb),
                           (unsigned int) (is->opens[0] - was->opens[0]), (unsigned int) (is->opens[1] - was->opens[1]),
                           (unsigned int) (is->opens[2] - was->opens[2]), (unsigned int) (is->opens[3] - was->opens[3]));
    if (is->world != was->world)
        n += (size_t) snprintf(line + n, sizeof(line) - n, "; answered %u batches of the server's world questions (they had waited %.1f ms each)",
                               (unsigned int) (is->world - was->world), (is->world_waited - was->world_waited) / (double) (is->world - was->world));
#ifdef SRW_DIAGNOSTICS
    {
        double hold, ended;
        char text[120];
        const char *states[8];
        int counts[8], count;

        uint32_t holds;
        double holds_ms;

        double slices_ms, slice_longest;
        uint32_t slices;

        uint32_t tags[3], per_thread[3], gaps;
        double per_ms[3], gap_ms;
        int busiest;

        SRW_TakeSliceStats(&slices_ms, &slices, &slice_longest);
        busiest = SRW_TakeThreadSlices(tags, per_thread, per_ms, 3, &gaps, &gap_ms);
        n += (size_t) snprintf(line + n, sizeof(line) - n, " | other threads held the big lock for %.0f ms in %u slices (the longest %.0f ms)", slices_ms, (unsigned int) slices, slice_longest);
        for (i = 0; i < busiest; i++)
            n += (size_t) snprintf(line + n, sizeof(line) - n, "%s thread %x: %u slices, %.0f ms", i == 0 ? " [" : ";", (unsigned int) tags[i], (unsigned int) per_thread[i], per_ms[i]);
        if (busiest > 0) n += (size_t) snprintf(line + n, sizeof(line) - n, "]");
        {
            uint32_t hot[8];
            double hot_ms[8];
            int hots = SRW_TakeHot(hot, hot_ms, 8);

            for (i = 0; i < hots; i++)
                n += (size_t) snprintf(line + n, sizeof(line) - n, "%s %x: %.0f ms", i == 0 ? " [worker profile, 16 byte buckets" : ",", (unsigned int) hot[i], hot_ms[i]);
            if (hots > 0) n += (size_t) snprintf(line + n, sizeof(line) - n, "]");
        }
        {
            uint32_t places[5];
            double place_ms[5];
            int shown = SRW_TakeWhere(places, place_ms, 5);

            for (i = 0; i < shown; i++)
                n += (size_t) snprintf(line + n, sizeof(line) - n, "%s %x: %.0f ms", i == 0 ? " [worker threads, by place in the code" : ",", (unsigned int) places[i], place_ms[i]);
            if (shown > 0) n += (size_t) snprintf(line + n, sizeof(line) - n, "]");
        }
        n += (size_t) snprintf(line + n, sizeof(line) - n, " | %u handoffs of the lock to a waiting thread took %.0f ms between the release and the next thread running (%.2f ms each)", (unsigned int) gaps, gap_ms, gaps ? gap_ms / gaps : 0.0);
        if (SRW_TakeLongHold(&hold, &ended, text, sizeof(text), &holds, &holds_ms) && ended >= was->start)
            n += (size_t) snprintf(line + n, sizeof(line) - n, " | %u of those slices were longer than 10 ms (%.0f ms in all), the longest %.0f ms (%s)", (unsigned int) holds, holds_ms, hold, text);
        count = SRW_SampleOtherThreads(states, counts, 0, 8);
        if (count > 0) n += (size_t) snprintf(line + n, sizeof(line) - n, " | other threads now:");
        for (i = 0; i < count && n < sizeof(line) - 60; i++)
            n += (size_t) snprintf(line + n, sizeof(line) - n, " %s %d,", states[i] != NULL ? states[i] : "running or waiting for the big lock", counts[i]);
    }
#endif
    logprint(line);
}

static Baseline baseline;

/* the start of a Present: the frame that was running is over */
void SRW_ProfFrame(void)
{
    double now = emscripten_get_now(), sums[WOW_PROF_COUNT], total = 0, frame = 0;
    int i, f;
    static double dumped;
    Baseline is;

    charge(now);
    for (i = 0; i < WOW_PROF_COUNT; i++) frame += accumulated[i];
    take_baseline(&is, now);
    if (frame > 80.0 && !fresh) report_slow_frame(&baseline, &is, accumulated, frame);
#ifdef SRW_DIAGNOSTICS
    else { double h, e, t, st, sl; char x[8]; uint32_t c, sc; SRW_TakeLongHold(&h, &e, x, sizeof(x), &c, &t); SRW_TakeSliceStats(&st, &sc, &sl); uint32_t tg[3], ps[3], gp; double pm[3], gm; SRW_TakeThreadSlices(tg, ps, pm, 3, &gp, &gm); { uint32_t wp[8]; double wm[8]; SRW_TakeWhere(wp, wm, 5); SRW_TakeHot(wp, wm, 8); } }   /* (the holds of this frame are not for the next) */
#endif
    fresh = 0;
    baseline = is;
    memcpy(ring[ring_next], accumulated, sizeof(accumulated));
    memset(accumulated, 0, sizeof(accumulated));
    ring_next = (ring_next + 1) % WINDOW;
    if (ring_count < WINDOW) ring_count++;
    memset(sums, 0, sizeof(sums));
    for (f = 0; f < ring_count; f++)
        for (i = 0; i < WOW_PROF_COUNT; i++) sums[i] += ring[f][i];
    for (i = 0; i < WOW_PROF_COUNT; i++) total += sums[i];
    {
        uint32_t best = 0, most = 0;

        for (i = 0; i < 4; i++) if (sleep_counts[i] > most) { most = sleep_counts[i]; best = sleep_callers[i]; }
        shared[4] = (int32_t) best;
    }
    for (i = 0; i < WOW_PROF_COUNT; i++) shared[5 + i] = (int32_t) (sums[i] * 1000.0);
    shared[2] = ring_count;
    shared[3] = (int32_t) (total * 1000.0);
    shared[1]++;
    if (ring_next == 0)
    {
        memset(sleep_callers, 0, sizeof(sleep_callers));
        memset(sleep_counts, 0, sizeof(sleep_counts));
    }
    if (SRW_LOG_ON(SRW_LOG_PERF) && now - dumped > 5000.0 && ring_count == WINDOW)
    {
        char line[600];
        size_t n = (size_t) snprintf(line, sizeof(line), "Prof: %.1f ms per frame (%.1f fps) over %d frames:", total / ring_count, 1000.0 * ring_count / total, ring_count);

        dumped = now;
        for (i = 0; i < WOW_PROF_COUNT; i++) n += (size_t) snprintf(line + n, sizeof(line) - n, " %s %.1f%% (%.2f ms),", names[i], 100.0 * sums[i] / total, sums[i] / ring_count);
        logprint(line);
    }
}
