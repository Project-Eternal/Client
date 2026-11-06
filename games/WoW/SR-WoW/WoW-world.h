/* The game's answers about its world (WoW-world.c): the layout is shared with the server's module (mod-wowcontroller, PageProcess) through
   the page, little endian, 4-byte fields. */
#ifndef WOW_WORLD_H
#define WOW_WORLD_H

#include <stddef.h>
#include <stdint.h>

enum {
    WOW_WORLD_STATE = 0,            /* u0 the client's map, u1 the loaded ADT tiles (always known) */
    WOW_WORLD_TERRAIN_HEIGHT = 1,   /* p0 p1 (x, y): u0 hit, f0 the height (flags: the trace's, 0 = terrain) */
    WOW_WORLD_TERRAIN_AREA = 2,     /* p0 p1 p2: u0 the area of the terrain's chunk (AreaTable.dbc) */
    WOW_WORLD_LIQUID = 3,           /* p0 p1 p2: u0 found, u1 the liquid type, f0 its level (buildings' liquid first, then the terrain's) */
    WOW_WORLD_TRACE = 4,            /* p0..p2 to p3..p5 with the flags: u0 hit, f0..f2 the hit point, */
    WOW_WORLD_TILES = 5,            /* p0 a row of tiles (tileX 0..63): u0, u1 the loaded ones (bit tileY; u0 tileY 0..31), u2 the map (always known) */
    WOW_WORLD_BUILDING = 6,         /* p0 p1 p2 (flags: the trace's, 0 = the map's buildings 0x10): the building (WMO) group below: u0 found, f0 its floor z, u1 root id, u2 name set (adt id); the raw
                                       bits of f1 the group id, of f2 the group's flags (MOGP) */
    WOW_WORLD_BUILDING_DEF = 8,     /* p0 p1 p2 (research): the placement of the building below: u0 found, u1 its flags (+0xc), u2 its address */
    WOW_WORLD_BUILDING_LIQUID = 7   /* p0 p1 p2: the liquid of the building group below (that of op 6): u0 found, u1 the liquid type, f0 its level */
};

/* the client's TraceLine flags (inferred from its own calls) */
#define WOW_TRACE_M2 0x1u
#define WOW_TRACE_WMO 0x10u
#define WOW_TRACE_TERRAIN 0x100u
#define WOW_TRACE_LIQUID 0x20000u
/* (ours, not the client's: which of the server's two sets of models a trace (op 4) or a building question (ops 6-8) is about: the map's
   own buildings and doodads, or the game objects' models and buildings; the client's flags cannot tell the buildings of the two apart) */
#define WOW_SET_MAP 0x80000000u
#define WOW_SET_GAME_OBJECTS 0x08000000u
#define WOW_SET_MASK (WOW_SET_MAP | WOW_SET_GAME_OBJECTS)
#define WOW_TRACE_GAME_OBJECTS 0x100000u   /* doors and the like (an open door is not hit); the client's own line of sight and floor traces
                                              use it (0x100111); 0x200000, 0x400000, 0x800000 and 0x40000000 hit them too */

typedef struct {
    int32_t known;    /* 0: the client cannot tell (another map, not loaded there): the server's "no data" */
    uint32_t u[3];
    float f[3];
} WoWWorldAnswer;

typedef struct {
    uint32_t op, map, flags;
    float p[6];
    WoWWorldAnswer answer;
} WoWWorldQuery;

typedef struct {
    WoWWorldQuery *queries;
    uint32_t count;
} WoWWorldBatch;

/* how long a question waits for the game thread to sleep; when it has not slept for that long, the questions are unknown at once (the game
   thread does not sleep when its frames take longer than its frame limiter's period: CVar maxfps) */
#define WOW_WORLD_TIMEOUT_MS 250.0

/* answers the queries while the game thread sleeps (the calling thread must be a guest thread); 0: the game thread did not sleep within
   WOW_WORLD_TIMEOUT_MS, all unknown */
int WoW_WorldQueries(WoWWorldQuery *queries, uint32_t count);
/* the game thread around its sleeps (Sleep_c): 1 before, 0 after */
void SRW_GameSleeps(int asleep);
extern volatile int WoW_WorldLogNow;   /* the next WoW_WorldQueries logs its counts */
/* a safe point of the game thread (0 a frequent import, 1 its message queue, 2 Present): runs the waiting questions when no world code is in
   progress on it */
void SRW_SafePoint(int source);
/* "safepoint" (default), "sleep" or "frame" */
void WoW_WorldSetMode(const char *mode);
/* a line about the questions since the last minute's log line (.prof) */
void WoW_WorldDescribe(char *text, size_t size);
void WoW_WorldQueryCounts(uint64_t *answered, uint64_t *unknown);

#endif
