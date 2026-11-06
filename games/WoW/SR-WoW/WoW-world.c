/* The game's answers about its world, for the embedded server (mod-wowcontroller's MapDataProvider: terrain height, area, liquid, line of
   sight and hits with the models). The 3.3.5 client's own functions answer them; they walk the
   client's loaded world (the ADT tiles around the camera, their buildings and doodads), which the game thread changes, and they change some
   of it themselves (trace counters, the last hit, collision data prepared on first use): see "how the questions get to run" below.
   The server's coordinates and the client's are the same (yards, x, y, z).
   A query is answered only for the map the client is on and where it has the world loaded (the terrain tiles, or the map has none there,
   and the buildings' groups there: see tile_known): otherwise it is "unknown" (known = 0) and the server takes it as if it had no map data
   there. */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

#include "srw_debuglog.h"
#include "WoW-world.h"
#include "WoW.h"

#define CLIENT_MAP_ID 0xbd088cu       /* the map the client is on (Map.dbc id) */
#define CLIENT_ADT_TABLE 0xce48d0u    /* CMapArea* [64 * 64]: the loaded ADT tiles, index tileX * 64 + tileY; +0x70 != 0: still loading */
#define CLIENT_WDT_TILES 0xce88d0u    /* the map's WDT tile table (MAIN, 8 bytes per tile, same index): & 1 = the map has an ADT there */
/* every placed building (CMapObjDef) the client has: a list, first [0xd25440], the next one at [def + [0xd25438] + 4] (ends at 0 or an odd
   value; as the client's 0x7a09d0 walks it); def: +0xc flags (0x100: left out, as there), +0x48 / +0x54 its world box (min, max), +0xb0 the
   matrix into the building, +0xf4 the building (CMapObj): +0x1e0 != 0 loaded, +0x16c its group count, [+0x130] the groups' table (32 bytes
   each: flags, box in the building's coordinates), +0x1f8 the groups (CMapObjGroup*; +0x198 & 1 loaded) */
#define CLIENT_BUILDINGS_FIRST 0xd25440u
#define CLIENT_BUILDINGS_LINK 0xd25438u
#define CLIENT_TRACE_LINE 0x77f310u   /* bool (C3Vector *start, C3Vector *end, C3Vector *hit, float *t (in 1), uint32 flags, 0) */
#define CLIENT_QUERY_LIQUID 0x77f360u /* bool (C3Vector *pos, uint32 *liquid type, float *level) */
#define CLIENT_CHUNK_AREA 0x7a0490u   /* uint32 (C3Vector *pos): the area of the ADT chunk, 0 when not loaded */
/* the building part of the client's area lookup (0x77fa00): a trace down through the buildings, then the hit group */
#define CLIENT_TRACE_BUILDINGS 0x7a30d0u /* bool (C3Vector *start, C3Vector *end, uint32 flags 0x10, uint32 mode 0x500, float *t (in 1), 0, 0,
                                            CMapObjDef **def, CMapObjDefGroup **group) */
#define CLIENT_MODEL_SCENE 0xcd754cu  /* the world's model scene: TraceLine clears its +0x12c before each of its two parts */
#define CLIENT_OBJ_GROUP 0x7aea80u    /* thiscall CMapObj::Group(index [group+0x50], 0): the group: +0x30 its flags (MOGP), +0x180 its id */
/* the liquid of a group: thiscall bool CMapObjGroup::GetLiquid(C3Vector *local pos, uint32 *liquid type (LiquidType.dbc), float *level (local)),
   true only when the level is above the position's z (the group's own liquid grid at x, y). (The client's 0x7a09d0 = the same for the
   groups of the buildings around a position, but skips interior groups (flag 0x2000): the outdoor water of a building only.) */
#define CLIENT_GROUP_LIQUID 0x7c8360u
#define BUILDING_TO_LOCAL 0xb0u       /* CMapObjDef: the matrix from the world into the building (row vectors, translation at +0x30) */
#define BUILDING_TO_WORLD 0x70u       /* CMapObjDef: the matrix back */
#define CLIENT_SHOW_BUILDINGS 0xcd774cu /* & 0x100: the buildings are there (the client's area lookup asks this first) */
#define CLIENT_AREA_UP 0xa14accu      /* float: how far above the position the client's area lookup starts */
#define CLIENT_AREA_DOWN 0xa3e6e8u    /* float: how far below it goes */

#define TILE_SIZE (1600.0f / 3.0f)

static volatile uint64_t queries_answered, queries_unknown;
/* why questions were unknown, and when the counts were last logged (once a minute, in WoW_WorldQueries) */
static volatile uint64_t unknown_late, unknown_map, unknown_tile, unknown_building, batches;
static double counts_logged;
volatile int WoW_WorldLogNow;   /* the next call logs the counts (autoinput world) */

static uint32_t guest_u32(uint32_t address) { return *(volatile uint32_t *) (uintptr_t) address; }

static int tile_of(float x, float y, int *tx, int *ty)
{
    float fx = 32.0f - x / TILE_SIZE, fy = 32.0f - y / TILE_SIZE;

    if (!(fx >= 0.0f && fx < 64.0f && fy >= 0.0f && fy < 64.0f)) return 0;
    *tx = (int) fx;
    *ty = (int) fy;
    return 1;
}

/* Whether the client has the world at a place (its own rule: the edges it shows as walls in the world, 0x7b4bc0, are the edges of the loaded
   tiles): a tile is known when it is loaded or the map has none there (its WDT says so: no terrain, nothing placed; a map of a single
   building has no tiles at all), and every building that reaches into the place has its groups there loaded. Unknown places get the
   server's "no data": a trace through what is not loaded yet would say "nothing there". (Doodads and game objects whose models are still
   loading, shown as boxes in the world, are not looked at: they are missing from the answers for those moments.) */
static int tile_known(int tx, int ty)
{
    uint32_t index = (uint32_t) (tx * 64 + ty), area = guest_u32(CLIENT_ADT_TABLE + 4u * index);

    if (area != 0) return guest_u32(area + 0x70) == 0;
    return !(guest_u32(CLIENT_WDT_TILES + 8u * index) & 1);
}

/* the tiles of the box around a and b */
static int tiles_known(const float *a, const float *b)
{
    int ax, ay, bx, by, x, y;

    if (!tile_of(a[0], a[1], &ax, &ay) || !tile_of(b[0], b[1], &bx, &by)) return 0;
    for (x = ax < bx ? ax : bx; x <= (ax > bx ? ax : bx); x++)
        for (y = ay < by ? ay : by; y <= (ay > by ? ay : by); y++)
            if (!tile_known(x, y)) return 0;
    return 1;
}

static void transform(uint32_t matrix, const float *in, float *out);

static int boxes_meet(const volatile float *box, const float *low, const float *high)
{
    return box[0] <= high[0] && box[3] >= low[0] && box[1] <= high[1] && box[4] >= low[1] && box[2] <= high[2] && box[5] >= low[2];
}

/* every building reaching into the box around a and b has its groups there loaded */
static int buildings_known(const float *a, const float *b)
{
    float low[3], high[3];
    uint32_t link = guest_u32(CLIENT_BUILDINGS_LINK), def = guest_u32(CLIENT_BUILDINGS_FIRST);
    int i, guard = 0;

    for (i = 0; i < 3; i++) { low[i] = a[i] < b[i] ? a[i] : b[i]; high[i] = a[i] > b[i] ? a[i] : b[i]; }
    while (def != 0 && !(def & 1) && guard++ < 100000)
    {
        if (!(guest_u32(def + 0xc) & 0x100) && boxes_meet((const volatile float *) (uintptr_t) (def + 0x48), low, high))
        {
            uint32_t root = guest_u32(def + 0xf4), groups, table;
            float la[3], lb[3], llow[3], lhigh[3];
            uint32_t g;

            if (root == 0 || guest_u32(root + 0x1e0) == 0) return 0;
            transform(def + BUILDING_TO_LOCAL, a, la);
            transform(def + BUILDING_TO_LOCAL, b, lb);
            for (i = 0; i < 3; i++) { llow[i] = la[i] < lb[i] ? la[i] : lb[i]; lhigh[i] = la[i] > lb[i] ? la[i] : lb[i]; }
            groups = guest_u32(root + 0x16c);
            table = guest_u32(root + 0x130);
            for (g = 0; g < groups && g < 4096; g++)
                if (boxes_meet((const volatile float *) (uintptr_t) (table + 32u * g + 4u), llow, lhigh))
                {
                    uint32_t group = guest_u32(root + 0x1f8 + 4u * g);

                    if (group == 0 || !(guest_u32(group + 0x198) & 1)) return 0;
                }
        }
        def = guest_u32(def + link + 4);
    }
    return 1;
}

static uint32_t trace(const float *start, const float *end, uint32_t flags, float *hit)
{
    float s[3] = {start[0], start[1], start[2]}, e[3] = {end[0], end[1], end[2]}, h[3] = {0, 0, 0}, t = 1.0f;
    uint32_t args[6] = {(uint32_t) (uintptr_t) s, (uint32_t) (uintptr_t) e, (uint32_t) (uintptr_t) h, (uint32_t) (uintptr_t) &t, flags, 0};
    uint32_t result = SRW_CallGuest(CLIENT_TRACE_LINE, 6, args) & 0xff;

    if (hit != NULL) memcpy(hit, h, sizeof(h));
    return result;
}

/* the building group below a position, as the client's area lookup (0x77fa00) finds it: from a little above the position down through the
   buildings (the hit's z in *floor); 0 when none (or the buildings are not shown); *def the building's placement */
/* a building placement of a game object (a destructible building, a ship): the client places those itself (0x783500 -> 0x7bf120, flags
   0x10000 from the start; the map's are read from its files); its object's GUID is at +0x148 (the client's TraceLine reports it as the hit) */
static void clear_scene_hit(void)
{
    uint32_t scene = guest_u32(CLIENT_MODEL_SCENE);

    if (scene != 0) *(volatile uint32_t *) (uintptr_t) (scene + 0x12c) = 0;
}

static int game_object_building(uint32_t def)
{
    return (guest_u32(def + 0xc) & 0x10000) != 0;
}

/* The client's trace of its objects from start to end, keeping only the hits of one set when set is given: the map's own models
   (WOW_SET_MAP) or the game objects' (WOW_SET_GAME_OBJECTS). The models are a TraceLine of their own bits (doodads 0x1 are the map's,
   game objects' models 0x100000 theirs); the buildings (0x10) hit both sets, so they are traced with the client's object trace 0x7a30d0,
   which says the placement it hit, and a hit on a building of the other set is passed through: the trace goes on from just behind it.
   1 and the nearest hit's fraction of the whole segment, its placement and group (0 when it is a model). */
static int trace_objects(const float *start, const float *end, uint32_t flags, uint32_t mode, uint32_t set, float *t_out, uint32_t *def_out,
                         uint32_t *group_out)
{
    float from[3] = {start[0], start[1], start[2]}, to[3] = {end[0], end[1], end[2]}, done = 0.0f, length, model_t = 2.0f;
    int pass, i;

    length = sqrtf((end[0] - start[0]) * (end[0] - start[0]) + (end[1] - start[1]) * (end[1] - start[1]) + (end[2] - start[2]) * (end[2] - start[2]));
    if (!(length > 0.0f)) return 0;
    if (flags & (WOW_TRACE_M2 | WOW_TRACE_GAME_OBJECTS))
    {
        /* the models first, in a TraceLine of their own: 0x7a30d0 looks for models only nearer than its building hit */
        float hit[3];

        if (trace(start, end, flags & (WOW_TRACE_M2 | WOW_TRACE_GAME_OBJECTS), hit))
        {
            float d = sqrtf((hit[0] - start[0]) * (hit[0] - start[0]) + (hit[1] - start[1]) * (hit[1] - start[1]) + (hit[2] - start[2]) * (hit[2] - start[2]));

            model_t = d / length;
        }
    }
    flags &= ~(WOW_TRACE_M2 | WOW_TRACE_GAME_OBJECTS);
    for (pass = 0; (flags & WOW_TRACE_WMO) && pass < 16; pass++)
    {
        float t = 1.0f, total;
        uint32_t def = 0, group = 0, other = 0, args[9];

        args[0] = (uint32_t) (uintptr_t) from; args[1] = (uint32_t) (uintptr_t) to; args[2] = flags; args[3] = mode;
        args[4] = (uint32_t) (uintptr_t) &t; args[5] = 0; args[6] = (uint32_t) (uintptr_t) &other; args[7] = (uint32_t) (uintptr_t) &def;
        args[8] = (uint32_t) (uintptr_t) &group;
        clear_scene_hit();
        if (!(SRW_CallGuest(CLIENT_TRACE_BUILDINGS, 9, args) & 0xff)) break;
        total = done + (1.0f - done) * t;
        if (total > model_t) break;   /* (a model is nearer) */
        /* (a hit with a group is a building, def its placement; without one def is some other object: kept) */
        if (!set || group == 0 || def == 0 || game_object_building(def) == (set == WOW_SET_GAME_OBJECTS))
        {
            *t_out = total;
            *def_out = def;
            *group_out = group;
            return 1;
        }
        done = total + 0.02f / length;   /* (on from 2 cm behind the hit of the other set) */
        if (done >= 1.0f) break;
        for (i = 0; i < 3; i++) from[i] = start[i] + (end[i] - start[i]) * done;
    }
    if (model_t > 1.0f) return 0;
    *t_out = model_t;
    *def_out = 0;
    *group_out = 0;
    return 1;
}

/* the building group below a position, as the client's area lookup (0x77fa00) finds it: from a little above the position down through the
   buildings (the hit's z in *floor), of one set if given (see trace_objects); 0 when none (or the buildings are not shown); *def the
   building's placement */
static uint32_t building_group(const float *p, uint32_t flags, uint32_t set, uint32_t *def_out, float *floor)
{
    float up = *(const volatile float *) (uintptr_t) CLIENT_AREA_UP, down = *(const volatile float *) (uintptr_t) CLIENT_AREA_DOWN;
    float start[3] = {p[0], p[1], p[2] + up}, end[3] = {p[0], p[1], p[2] - down}, t = 1.0f;
    uint32_t def = 0, group = 0, object, group_args[2];

    if (!(guest_u32(CLIENT_SHOW_BUILDINGS) & 0x100)) return 0;
    if (!trace_objects(start, end, flags, 0x500, set, &t, &def, &group) || def == 0 || group == 0) return 0;
    object = guest_u32(def + 0xf4);
    if (object == 0) return 0;
    group_args[0] = guest_u32(group + 0x50);
    group_args[1] = 0;
    *def_out = def;
    *floor = start[2] + (end[2] - start[2]) * t;
    return SRW_CallGuestFastcallArgs(CLIENT_OBJ_GROUP, object, 0, 2, group_args);
}

/* a point through one of a building's matrices (as the client's 0x4c21b0: row vector times the 3x3 part, plus the translation) */
static void transform(uint32_t matrix, const float *in, float *out)
{
    const volatile float *m = (const volatile float *) (uintptr_t) matrix;
    int i;

    for (i = 0; i < 3; i++) out[i] = in[0] * m[i] + in[1] * m[4 + i] + in[2] * m[8 + i] + m[12 + i];
}

static void answer(WoWWorldQuery *q)
{
    WoWWorldAnswer *a = &q->answer;
    uint32_t map = guest_u32(CLIENT_MAP_ID);

    memset(a, 0, sizeof(*a));
    if (q->op == WOW_WORLD_STATE)
    {
        int i, loaded = 0;

        for (i = 0; i < 64 * 64; i++)
            if (guest_u32(CLIENT_ADT_TABLE + 4u * (uint32_t) i) != 0) loaded++;
        a->known = 1;
        a->u[0] = map;
        a->u[1] = (uint32_t) loaded;
        for (i = 0, loaded = 0; i < 64 * 64; i++)
            if (guest_u32(CLIENT_WDT_TILES + 8u * (uint32_t) i) & 1) loaded++;
        a->u[2] = (uint32_t) loaded;   /* (the map's tiles in its WDT) */
        return;
    }
    if (q->op == WOW_WORLD_TILES)
    {
        int tx = (int) q->p[0], ty;

        a->known = 1;
        a->u[2] = map;
        if (tx < 0 || tx >= 64) return;
        for (ty = 0; ty < 64; ty++)
        {
            uint32_t area = guest_u32(CLIENT_ADT_TABLE + 4u * (uint32_t) (tx * 64 + ty));

            if (area != 0 && guest_u32(area + 0x70) == 0) a->u[ty >> 5] |= 1u << (ty & 31);
        }
        return;
    }
    if (q->map != map) { unknown_map++; return; }
    {
        /* the place the question looks at: the trace's segment, the vertical span the building questions trace through, else the point */
        float from[3] = {q->p[0], q->p[1], q->p[2]}, to[3] = {q->p[0], q->p[1], q->p[2]};

        if (q->op == WOW_WORLD_TRACE) { to[0] = q->p[3]; to[1] = q->p[4]; to[2] = q->p[5]; }
        if (q->op == WOW_WORLD_BUILDING || q->op == WOW_WORLD_BUILDING_LIQUID || q->op == WOW_WORLD_BUILDING_DEF || q->op == WOW_WORLD_LIQUID)
        {
            from[2] += *(const volatile float *) (uintptr_t) CLIENT_AREA_UP;
            to[2] -= *(const volatile float *) (uintptr_t) CLIENT_AREA_DOWN;
        }
        if (!tiles_known(from, to)) { unknown_tile++; return; }
        /* (the terrain questions look at the terrain alone) */
        if (q->op != WOW_WORLD_TERRAIN_HEIGHT && q->op != WOW_WORLD_TERRAIN_AREA && !buildings_known(from, to)) { unknown_building++; return; }
    }
    switch (q->op)
    {
    case WOW_WORLD_TERRAIN_HEIGHT:   /* the terrain alone: a vertical trace through the whole height range */
    {
        float top[3] = {q->p[0], q->p[1], 5000.0f}, bottom[3] = {q->p[0], q->p[1], -5000.0f}, hit[3];

        a->known = 1;
        a->u[0] = trace(top, bottom, q->flags ? q->flags : WOW_TRACE_TERRAIN, hit);
        a->f[0] = hit[2];
        break;
    }
    case WOW_WORLD_TERRAIN_AREA:
    {
        float pos[3] = {q->p[0], q->p[1], q->p[2]};
        uint32_t args[1] = {(uint32_t) (uintptr_t) pos};

        a->known = 1;
        a->u[0] = SRW_CallGuest(CLIENT_CHUNK_AREA, 1, args);
        break;
    }
    case WOW_WORLD_LIQUID:
    {
        float pos[3] = {q->p[0], q->p[1], q->p[2]}, level = 0;
        uint32_t liquid = 0, args[3] = {(uint32_t) (uintptr_t) pos, (uint32_t) (uintptr_t) &liquid, (uint32_t) (uintptr_t) &level};

        a->known = 1;
        a->u[0] = SRW_CallGuest(CLIENT_QUERY_LIQUID, 3, args) & 0xff;
        a->u[1] = liquid;
        a->f[0] = level;
        break;
    }
    case WOW_WORLD_TRACE:
        a->known = 1;
        if (q->flags & WOW_SET_MASK)
        {
            /* the models of one set (the map's or the game objects'): the client's object trace, hits of the other set passed through */
            float t = 1.0f;
            uint32_t def = 0, group = 0, i;

            a->u[0] = (uint32_t) trace_objects(q->p, q->p + 3, q->flags & ~WOW_SET_MASK, 0x100, q->flags & WOW_SET_MASK, &t, &def, &group);
            if (a->u[0]) for (i = 0; i < 3; i++) a->f[i] = q->p[i] + (q->p[3 + i] - q->p[i]) * t;
        }
        else a->u[0] = trace(q->p, q->p + 3, q->flags, a->f);
        break;
    case WOW_WORLD_BUILDING:
    {
        uint32_t def = 0, info, id, flags;
        float floor = 0;

        a->known = 1;
        info = building_group(q->p, (q->flags & ~WOW_SET_MASK) ? (q->flags & ~WOW_SET_MASK) : WOW_TRACE_WMO, q->flags & WOW_SET_MASK, &def, &floor);
        if (info == 0) break;
        a->u[0] = 1;
        a->f[0] = floor;
        a->u[1] = guest_u32(guest_u32(guest_u32(def + 0xf4) + 0x120) + 0x20);   /* the root's id (WMOAreaTable WMOID) */
        a->u[2] = guest_u32(def + 0x104);                                         /* the name set (WMOAreaTable NameSetID) */
        id = guest_u32(info + 0x180);
        flags = guest_u32(info + 0x30);
        memcpy(&a->f[1], &id, 4);
        memcpy(&a->f[2], &flags, 4);
        break;
    }
    case WOW_WORLD_BUILDING_DEF:
    {
        /* the placement of the building below: u0 found, u1 its flags +0xc, u2 its address; f0 +0x104, f1 +0x8, f2 +0x10 as raw bits */
        uint32_t def = 0, info, v;
        float floor = 0;

        a->known = 1;
        info = building_group(q->p, (q->flags & ~WOW_SET_MASK) ? (q->flags & ~WOW_SET_MASK) : WOW_TRACE_WMO, q->flags & WOW_SET_MASK, &def, &floor);
        if (info == 0) break;
        a->u[0] = 1;
        a->u[1] = guest_u32(def + 0xc);
        a->u[2] = def;
        v = guest_u32(def + 0x104); memcpy(&a->f[0], &v, 4);
        v = guest_u32(def + 0x8); memcpy(&a->f[1], &v, 4);
        v = guest_u32(def + 0x10); memcpy(&a->f[2], &v, 4);
        break;
    }
    case WOW_WORLD_BUILDING_LIQUID:
    {
        /* the liquid of the group below (as vmaps: the level at x, y whatever the height): asked from far below, because the client
           answers only when the level is above the position */
        uint32_t def = 0, info, liquid = 0, args[3];
        float floor = 0, local[3], level = 0, back[3];

        a->known = 1;
        info = building_group(q->p, (q->flags & ~WOW_SET_MASK) ? (q->flags & ~WOW_SET_MASK) : WOW_TRACE_WMO, q->flags & WOW_SET_MASK, &def, &floor);
        if (info == 0) break;
        transform(def + BUILDING_TO_LOCAL, q->p, local);
        local[2] = -100000.0f;
        args[0] = (uint32_t) (uintptr_t) local; args[1] = (uint32_t) (uintptr_t) &liquid; args[2] = (uint32_t) (uintptr_t) &level;
        if (!(SRW_CallGuestFastcallArgs(CLIENT_GROUP_LIQUID, info, 0, 3, args) & 0xff)) break;
        local[2] = level;
        transform(def + BUILDING_TO_WORLD, local, back);
        a->u[0] = 1;
        a->u[1] = liquid;
        a->f[0] = back[2];
        break;
    }
    default:
        return;
    }
}

static void answer_all(void *argument)
{
    WoWWorldBatch *batch = argument;
    uint32_t i;

    for (i = 0; i < batch->count; i++)
    {
        answer(&batch->queries[i]);
        if (batch->queries[i].answer.known) queries_answered++;
        else queries_unknown++;
    }
}

/* ---- how the questions get to run (WoW_WorldMode: "safepoint" by default, /Port/worldqueries.txt or autoinput worldmode) ----
   safepoint: the asking thread queues the batch; the game thread runs it at its next safe point: a frequent import (SRW_GilPoll), its look
              at the message queue or Present, when no code of the world (0x77e000-0x7e0000: World, WorldScene, Map*, MapObj*...; 0x75e000.. holds
              the main loop's event code, 0x7709df is on every stack) or of
              the models (0x818000-0x83e000: M2Scene, M2Model, M2Shared) is on its guest stack (a scan for return addresses): the world is
              not halfway through a change then, and the questions run on the thread that owns it. The results the game reads after its own
              traces (the last hit object) are put back afterwards.
   sleep:     run on the asking thread while the game thread is in Sleep (there are no sleeps in the world: frames are slower than maxfps)
   frame:     the game thread runs the batch at its next look at the message queue (one batch per frame)
   A number after the mode is how long a question may wait (ms, WOW_WORLD_TIMEOUT_MS by default): tests in a slow browser (llvmpipe) raise it */
static int world_mode = -1;   /* 0 safepoint, 1 sleep, 2 frame */
static double world_timeout_ms = WOW_WORLD_TIMEOUT_MS;

void WoW_WorldSetMode(const char *mode)
{
    const char *number = strchr(mode, ' ');
    double timeout = number != NULL ? atof(number + 1) : 0;

    world_mode = strncmp(mode, "sleep", 5) == 0 ? 1 : strncmp(mode, "frame", 5) == 0 ? 2 : 0;
    world_timeout_ms = timeout > 0 ? timeout : WOW_WORLD_TIMEOUT_MS;
    logprintf("World questions run %s, waiting %.0f ms at most", world_mode == 1 ? "while the game thread sleeps" :
              world_mode == 2 ? "once per frame on the game thread" : "on the game thread at safe points", world_timeout_ms);
}

static int current_mode(void)
{
    if (world_mode < 0)
    {
        char text[32] = "";
        FILE *f = fopen("/Port/worldqueries.txt", "r");

        if (f != NULL) { if (fgets(text, sizeof(text), f) == NULL) text[0] = 0; fclose(f); }
        WoW_WorldSetMode(text);
    }
    return world_mode;
}

/* -- sleep -- */
static pthread_mutex_t world_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t world_changed = PTHREAD_COND_INITIALIZER;
static int game_asleep;
static volatile double game_slept_at;   /* when the game thread last went to sleep (emscripten_get_now) */

void SRW_GameSleeps(int asleep)
{
    double start = asleep ? 0 : emscripten_get_now();

    if (world_mode != 1) return;
    if (asleep) game_slept_at = emscripten_get_now();
    pthread_mutex_lock(&world_lock);   /* (after a sleep: waits for a batch that is running) */
    game_asleep = asleep;
    if (asleep) pthread_cond_broadcast(&world_changed);
    pthread_mutex_unlock(&world_lock);
    if (!asleep && emscripten_get_now() - start > 100.0)
        logprintf("World questions kept the game thread waiting for %.0f ms after a sleep", emscripten_get_now() - start);
}

static void deadline(struct timespec *until, double timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, until);
    until->tv_sec += (time_t) (timeout_ms / 1000);
    until->tv_nsec += (long) (fmod(timeout_ms, 1000.0) * 1e6);
    if (until->tv_nsec >= 1000000000L) { until->tv_sec++; until->tv_nsec -= 1000000000L; }
}

/* runs the batch while the game thread sleeps; 0 when it did not sleep within timeout_ms, or not at all for that long before */
static int run_while_game_sleeps(WoWWorldBatch *batch, double timeout_ms)
{
    struct timespec until;
    int ran = 0;

    if (emscripten_get_now() - game_slept_at > timeout_ms) return 0;
    deadline(&until, timeout_ms);
    pthread_mutex_lock(&world_lock);
    while (!game_asleep)
        if (pthread_cond_timedwait(&world_changed, &world_lock, &until) != 0) break;
    if (game_asleep)
    {
        answer_all(batch);
        ran = 1;
    }
    pthread_mutex_unlock(&world_lock);
    return ran;
}

/* -- safepoint -- */
#define WORLD_CODE_START 0x77e000u
#define WORLD_CODE_END 0x7e0000u
#define MODEL_CODE_START 0x818000u
#define MODEL_CODE_END 0x83e000u
#define LAST_HIT 0xcd7768u   /* the last trace's hit object (8 bytes: 0xcd7768, 0xcd776c), read by the game after its own traces */
#define SAFE_POINT_INTERVAL_MS 0.1

typedef struct PendingBatch {
    WoWWorldBatch *batch;
    int state;   /* 0 queued, 1 running, 2 done, 3 taken back */
    double posted;   /* ms of SRW_NowNs */
    struct PendingBatch *next;
} PendingBatch;

static pthread_mutex_t safe_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t safe_done = PTHREAD_COND_INITIALIZER;
static PendingBatch *safe_head, *safe_tail;
static volatile int safe_waiting;
static _Thread_local int in_safe_batch;
static double safe_last_check;
/* statistics (logged with the counts) */
static volatile uint64_t safe_checks, safe_rejected, safe_runs, safe_runs_by[3];
static volatile double safe_wait_sum, safe_wait_max;
volatile uint64_t wow_world_batches;   /* ever (the profiler's slow frame report) */
volatile double wow_world_waited_ms;

/* no code of the world or of the models is on the calling (game) thread's guest stack */
static int stack_is_safe(void)
{
    _cpu *cpu = x86_initialize_cpu();
    uint32_t address = cpu->_esp, low, top;
    static unsigned shown;

    SRW_GuestStackBounds(&low, &top);   /* (the game code runs on a fiber: its own stack) */
    if (address < low || address >= top) return 0;   /* (not where we think it is: not safe) */

    if (WoW_WorldLogNow == 2 || (shown < 3 && (safe_checks % 500) == 1))
    {
        /* (diagnostics: what the scan sees) */
        char line[600];
        size_t n = (size_t) snprintf(line, sizeof(line), "World safe point check: stack %u bytes, code addresses on it:", top - address);
        uint32_t a;
        int codes = 0;

        for (a = address; a + 4 <= top && n < sizeof(line) - 12; a += 4)
        {
            uint32_t v = *(const volatile uint32_t *) (uintptr_t) a;

            if (v >= 0x401000 && v < 0x9df000) { n += (size_t) snprintf(line + n, sizeof(line) - n, " %x", (unsigned int) v); codes++; }
        }
        logprint(line);
        shown++;
        if (WoW_WorldLogNow == 2) WoW_WorldLogNow = 0;
    }

    for (; address + 4 <= top; address += 4)
    {
        uint32_t value = *(const volatile uint32_t *) (uintptr_t) address;

        if ((value >= WORLD_CODE_START && value < WORLD_CODE_END) || (value >= MODEL_CODE_START && value < MODEL_CODE_END)) return 0;
    }
    return 1;
}

/* a safe point of the game thread (source: 0 a frequent import, 1 its message queue, 2 Present): runs the queued batches when no code of the
   world is in progress on it */
void SRW_SafePoint(int source)
{
    uint32_t saved[2];
    double now;

    if (!safe_waiting || in_safe_batch || !SRW_GilIsGame()) return;
    now = emscripten_get_now();
    if (now - safe_last_check < SAFE_POINT_INTERVAL_MS) return;
    safe_last_check = now;
    safe_checks++;
    if (!stack_is_safe()) { safe_rejected++; return; }

    in_safe_batch = 1;
    SRW_PROF_ENTER(WOW_PROF_WORLD);
    saved[0] = guest_u32(LAST_HIT);
    saved[1] = guest_u32(LAST_HIT + 4);
    pthread_mutex_lock(&safe_lock);
    while (safe_head != NULL)
    {
        PendingBatch *p = safe_head;
        double waited;

        safe_head = p->next;
        if (safe_head == NULL) safe_tail = NULL;
        p->state = 1;
        pthread_mutex_unlock(&safe_lock);
        answer_all(p->batch);
        waited = (double) SRW_NowNs() / 1e6 - p->posted;   /* (one clock for all threads: performance.now() differs per worker) */
        safe_wait_sum += waited;
        if (waited > safe_wait_max) safe_wait_max = waited;
        safe_runs++;
        safe_runs_by[source]++;
        wow_world_batches++;
        wow_world_waited_ms += waited;
        pthread_mutex_lock(&safe_lock);
        p->state = 2;
        pthread_cond_broadcast(&safe_done);
    }
    safe_waiting = 0;
    pthread_mutex_unlock(&safe_lock);
    *(volatile uint32_t *) (uintptr_t) LAST_HIT = saved[0];
    *(volatile uint32_t *) (uintptr_t) (LAST_HIT + 4) = saved[1];
    SRW_PROF_LEAVE();
    in_safe_batch = 0;
}

static int run_at_safe_point(WoWWorldBatch *batch, double timeout_ms)
{
    PendingBatch p = {batch, 0, (double) SRW_NowNs() / 1e6, NULL};
    struct timespec until;

    deadline(&until, timeout_ms);
    pthread_mutex_lock(&safe_lock);
    if (safe_tail != NULL) safe_tail->next = &p;
    else safe_head = &p;
    safe_tail = &p;
    safe_waiting = 1;
    while (p.state < 2)
    {
        if (p.state == 1) pthread_cond_wait(&safe_done, &safe_lock);
        else if (pthread_cond_timedwait(&safe_done, &safe_lock, &until) != 0 && p.state == 0)
        {
            /* no safe point in time: out of the queue */
            PendingBatch **link = &safe_head, *previous = NULL;

            while (*link != NULL && *link != &p) { previous = *link; link = &(*link)->next; }
            if (*link == &p)
            {
                *link = p.next;
                if (safe_tail == &p) safe_tail = previous;
            }
            if (safe_head == NULL) safe_waiting = 0;
            p.state = 3;
        }
    }
    pthread_mutex_unlock(&safe_lock);
    return p.state == 2;
}

int WoW_WorldQueries(WoWWorldQuery *queries, uint32_t count)
{
    WoWWorldBatch batch = {queries, count};
    uint32_t i;

    /* while the game thread sleeps; when it does not sleep for longer (a loading screen waits for the server, which may be waiting for this
       answer) the questions are unknown */
    int mode = current_mode();
    int answered = SRW_GilEnabled() || mode == 2 ? SRW_RunOnGameThreadWithin(answer_all, &batch, world_timeout_ms) :
                   mode == 1 ? run_while_game_sleeps(&batch, world_timeout_ms) : run_at_safe_point(&batch, world_timeout_ms);
    double now = emscripten_get_now();

    batches++;
    if (!answered)
    {
        for (i = 0; i < count; i++) memset(&queries[i].answer, 0, sizeof(queries[i].answer));
        queries_unknown += count;
        unknown_late += count;
    }
    if (now - counts_logged >= 60000.0 || WoW_WorldLogNow)
    {
        int asked_now = WoW_WorldLogNow;

        WoW_WorldLogNow = 0;
        if (counts_logged != 0 || asked_now)
            logprintf("World questions of the server in %.0f s: %llu requests, %llu answered, %llu unknown (%llu too late, "
                      "%llu another map, %llu terrain not loaded, %llu buildings not loaded)", (now - counts_logged) / 1000.0, (unsigned long long) batches,
                      (unsigned long long) queries_answered, (unsigned long long) queries_unknown, (unsigned long long) unknown_late,
                      (unsigned long long) unknown_map, (unsigned long long) unknown_tile, (unsigned long long) unknown_building);
        if ((counts_logged != 0 || asked_now) && safe_checks)
            logprintf("World safe points: %llu checks, %llu with world code on the stack, %llu batches run (%llu at imports, %llu at the message "
                      "queue, %llu at Present), waited %.2f ms each, %.1f ms at most", (unsigned long long) safe_checks, (unsigned long long) safe_rejected,
                      (unsigned long long) safe_runs, (unsigned long long) safe_runs_by[0], (unsigned long long) safe_runs_by[1],
                      (unsigned long long) safe_runs_by[2], safe_runs ? safe_wait_sum / safe_runs : 0.0, safe_wait_max);
        counts_logged = now;
        batches = queries_answered = queries_unknown = unknown_late = unknown_map = unknown_tile = unknown_building = 0;
        safe_checks = safe_rejected = safe_runs = safe_runs_by[0] = safe_runs_by[1] = safe_runs_by[2] = 0;
        safe_wait_sum = safe_wait_max = 0;
    }
    return answered;
}

/* the counts since the last minute's log line, for .prof */
void WoW_WorldDescribe(char *text, size_t size)
{
    double seconds = (emscripten_get_now() - counts_logged) / 1000.0;

    if (batches == 0 && safe_checks == 0)
    {
        snprintf(text, size, "World questions of the server (game side): none in the last %.0f s", counts_logged ? seconds : 0.0);
        return;
    }
    snprintf(text, size, "World questions of the server (game side, last %.0f s): %llu requests, %llu answered, %llu unknown (%llu too late); "
             "safe points: %llu checks, %llu with world code on the stack, waited %.2f ms each, %.1f ms at most", seconds, (unsigned long long) batches,
             (unsigned long long) queries_answered, (unsigned long long) queries_unknown, (unsigned long long) unknown_late, (unsigned long long) safe_checks,
             (unsigned long long) safe_rejected, safe_runs ? safe_wait_sum / safe_runs : 0.0, safe_wait_max);
}

void WoW_WorldQueryCounts(uint64_t *answered, uint64_t *unknown)
{
    *answered = queries_answered;
    *unknown = queries_unknown;
}
