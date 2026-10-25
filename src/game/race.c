/* The race: course scripts, roadside placement, checkpoints. */

#include <string.h>

#include "game.h"

#define GAME_MODE        0xFFC704
#define MODE_RACE        0x14
#define HUD_HIDDEN       0xFFC82C
#define COURSE_PROGRESS  0xFF0616   /* distance along the course */
#define STAGE            0xFF060A
#define OBJECTS          0xFFC900
#define OBJ_SIZE         0x40

/* object fields used here */
#define OBJ_HANDLER      0x00
#define OBJ_TILE_BASE    0x02
#define OBJ_DISTANCE     0x26
#define OBJ_LANE         0x2A
#define OBJ_SPRITE_SET   0x2D
#define OBJ_KIND         0x3A

#define HANDLER_HAZARD   0x24
#define HANDLER_SIGN     0x28
#define HANDLER_LIGHTS   0x2C
#define HANDLER_SCENERY  0x34

/* ------------------------------------------------------------ scenery */

/*
 * Roadside scenery comes from a course script (pointer at SCENERY_SCRIPT):
 * 6-byte entries (start distance, count, pattern number), ended by a
 * negative distance. A pattern is a bit mask word plus 6-byte pieces (flag
 * byte, sprite set, lane word, tile word); the mask is rotated once per
 * unit of distance and a piece is placed whenever a set bit comes out.
 */
#define SCENERY_SCRIPT   0xFF066E
#define SCENERY          0xFF0592   /* spawner state: */
#define SC_NEXT          0x00       /*   distance of the next placement */
#define SC_LANE          0x02
#define SC_OFFSET        0x04       /*   offset of the current piece */
#define SC_MASK          0x06       /*   rotating placement mask */
#define SC_PATTERN       0x08       /*   long: first piece */
#define SC_COUNT         0x0C       /*   placements left (in 16s) */
#define SC_SUBCOUNT      0x0E
#define SC_SPRITE_SET    0x0F
#define SC_TILE          0x10
#define ROM_SCENERY_PATTERNS 0x05771C
#define SCENERY_SLOTS    0xFFCE00   /* nine object records */

static int scenery_search;          /* -1: no search, else slots left (for the adapter) */

static void scenery_place(void)
{
    uint32_t obj = SCENERY_SLOTS;
    int k;
    for (k = 0; k < 9; k++, obj += OBJ_SIZE) {
        if (rd16(obj + OBJ_HANDLER))
            continue;
        scenery_search = 8 - k;
        wr16(obj + OBJ_LANE, rd16(SCENERY + SC_LANE));
        wr8(obj + OBJ_SPRITE_SET, rd8(SCENERY + SC_SPRITE_SET));
        wr16(obj + OBJ_TILE_BASE, rd16(SCENERY + SC_TILE));
        wr16(obj + OBJ_HANDLER, HANDLER_SCENERY);
        return;
    }
    scenery_search = 0xFFFF;
}

static void scenery_spawn(void)            /* $4512 */
{
    uint32_t entry = rd32(SCENERY_SCRIPT);
    int16_t at = (int16_t)rd16(entry);
    uint16_t progress = rd16(COURSE_PROGRESS);

    scenery_search = -1;
    if (at >= 0 && at <= (int16_t)progress) {
        uint32_t pattern = rd32(ROM_SCENERY_PATTERNS + 4 * rd16(entry + 4));
        wr32(SCENERY_SCRIPT, entry + 6);
        wr16(SCENERY + SC_NEXT, (uint16_t)at);
        wr16(SCENERY + SC_COUNT, rd16(entry + 2));
        wr16(SCENERY + SC_MASK, rd16(pattern));
        wr32(SCENERY + SC_PATTERN, pattern + 2);
        wr16(SCENERY + SC_OFFSET, 0);
    }
    if (rd16(SCENERY + SC_COUNT) && (int16_t)rd16(SCENERY + SC_NEXT) <= (int16_t)rd16(COURSE_PROGRESS)) {
        uint32_t piece;
        uint16_t mask;
        int8_t sub;
        for (;;) {
            piece = rd32(SCENERY + SC_PATTERN) + rd16(SCENERY + SC_OFFSET);
            if (!(rd8(piece) & 0x80))
                break;
            wr16(SCENERY + SC_OFFSET, 0);     /* end marker: start over */
        }
        wr16(SCENERY + SC_NEXT, rd16(SCENERY + SC_NEXT) + 1);
        mask = rd16(SCENERY + SC_MASK);
        wr16(SCENERY + SC_MASK, (uint16_t)(mask << 1 | mask >> 15));
        sub = (int8_t)(rd8(SCENERY + SC_SUBCOUNT) - 1);
        wr8(SCENERY + SC_SUBCOUNT, (uint8_t)sub);
        if (sub < 0) {
            wr16(SCENERY + SC_COUNT, rd16(SCENERY + SC_COUNT) - 1);
            wr8(SCENERY + SC_SUBCOUNT, 0x0F);
        }
        if (mask & 0x8000) {
            wr8(SCENERY + SC_SPRITE_SET, rd8(piece + 1));
            wr16(SCENERY + SC_LANE, rd16(piece + 2));
            wr16(SCENERY + SC_TILE, rd16(piece + 4));
            wr16(SCENERY + SC_OFFSET, rd16(SCENERY + SC_OFFSET) + 6);
            scenery_place();
        }
    }
}

/* -------------------------------------------------- course start signs */

/*
 * At the start of a stage, or at a checkpoint (progress $400-$402), put a
 * sign and two hazards into object slots 8-10 ($45E2); the first stage of
 * a fresh game also gets the starting lights.
 */
#define START_OBJECTS    (OBJECTS + 8 * OBJ_SIZE)

static void start_objects_place(uint16_t kind)
{
    uint32_t o = START_OBJECTS;
    wr16(o + OBJ_KIND, kind);
    wr16(o + OBJ_HANDLER, HANDLER_SIGN);
    wr16(o + OBJ_SIZE + OBJ_HANDLER, HANDLER_HAZARD);
    wr16(o + 2 * OBJ_SIZE + OBJ_HANDLER, HANDLER_HAZARD);
    wr8(o + OBJ_SPRITE_SET, 0x12);
    wr8(o + OBJ_SIZE + OBJ_SPRITE_SET, 0x11);
    wr8(o + 2 * OBJ_SIZE + OBJ_SPRITE_SET, 0x10);
    wr16(o + OBJ_SIZE + OBJ_LANE, 0x100);
    wr16(o + 2 * OBJ_SIZE + OBJ_LANE, 0xFF00);
}

static void start_objects(void)
{
    const uint32_t o = START_OBJECTS;
    int16_t progress = (int16_t)rd16(COURSE_PROGRESS);

    if (progress == 0) {
        if (!rd16(STAGE) && rd16(0xFF052C) == 1) {
            if (rd16(o + OBJ_HANDLER))
                return;
            wr16(o + 4 * OBJ_SIZE + OBJ_HANDLER, HANDLER_LIGHTS);
            wr16(o + OBJ_DISTANCE, 0x1E80);
            wr16(o + OBJ_SIZE + OBJ_DISTANCE, 0x1E80);
            wr16(o + 2 * OBJ_SIZE + OBJ_DISTANCE, 0x1E80);
            start_objects_place(0);
            return;
        }
        if (rd16(o + OBJ_HANDLER))
            return;
        start_objects_place(rd16(STAGE) ? 1 : 2);
        return;
    }
    if (progress < 0x400 || progress > 0x402)
        return;
    if (rd16(HUD_HIDDEN)) {
        if (!rd16(o + OBJ_HANDLER)) {
            wr16(o + OBJ_HANDLER, HANDLER_SIGN);
            wr16(o + OBJ_KIND, 3);
        }
        return;
    }
    if (rd16(o + OBJ_HANDLER))
        return;
    start_objects_place(1);
}

/* Roadside placement for one race frame ($4512, then $45E2 outside races). */
void race_place_objects(void)
{
    scenery_spawn();
    if ((rd16(GAME_MODE) & ~1) != MODE_RACE)
        start_objects();
}

void race_place_objects_68k(void)
{
    race_place_objects();
    if (scenery_search >= 0)
        cpu.d[1] = (uint32_t)scenery_search;   /* moveq #8 then dbf in the slot search */
}

void start_objects_68k(void) { start_objects(); }

/* --------------------------------------------------------------- turns */

/*
 * The course's turns ($9F1C): a script of 6-byte entries (distance,
 * strength, direction), ended by a negative distance. $28 before an entry
 * the turn is announced (rivals speed up); at the entry it becomes the
 * current turn.
 */
#define TURN_SCRIPT      0xFF0672
#define TURN_NEXT_POWER  0xFF067A
#define TURN_NEXT_DIR    0xFF0676
#define TURN_POWER       0xFF0682
#define TURN_DIR         0xFF067E
#define RIVALS_HURRY     0xFF0652

static int turn_announce_at = -1;   /* for the adapter */

void race_turns(void)
{
    uint32_t entry = rd32(TURN_SCRIPT);
    int16_t at = (int16_t)rd16(entry), progress = (int16_t)rd16(COURSE_PROGRESS);

    turn_announce_at = -1;
    if (at < 0)
        return;
    turn_announce_at = (uint16_t)(at - 0x28);
    if ((int16_t)(at - 0x28) <= progress) {
        wr16(TURN_NEXT_POWER, rd16(entry + 2));
        wr16(TURN_NEXT_DIR, rd16(entry + 4));
        wr16(RIVALS_HURRY, 0xFF);
    }
    if (at <= progress) {
        wr16(TURN_POWER, rd16(entry + 2));
        wr16(TURN_DIR, rd16(entry + 4));
        wr32(TURN_SCRIPT, entry + 6);
        wr16(TURN_NEXT_DIR, 0);
        wr16(TURN_NEXT_POWER, 0);
        wr16(RIVALS_HURRY, 0);
    }
}

void race_turns_68k(void)
{
    race_turns();
    if (turn_announce_at >= 0)
        set_dn(1, 2, (uint16_t)turn_announce_at);
}

/* -------------------------------------------------------------- rivals */

#define RIVAL_RECORDS    0xFF0804   /* 8 bytes: object, lane, depth (see obj_rival) */
#define RIVAL_COUNT      0xFF068A
#define RIVAL_COUNT_LAST 0xFF0690
#define RIVAL_ACCEL_SUM  0xFF0632
#define RIVAL_ACCEL_AVG  0xFF0634
#define RIVAL_OBJECTS    0xFFCC40   /* five rival object records */
#define RIVAL_SPAWN      0xFFC80E   /* spawner: */
#define RS_DELAY         0x00       /*   frames until the next slide spawn */
#define RS_PENDING       0x02
#define RS_COUNTDOWN     0x04
#define RS_SPAWNED       0x06
#define SFX_COOLDOWN     0xFFC816
#define ROM_RIVAL_DENSITY 0x004930  /* per course x stage: max rivals */

/* register leftovers the recompiled callers still read (for the adapter) */
static struct { int d1_set, d4_set; uint16_t d1; uint32_t d4, d5; } rv;

static void rival_average(void)
{
    uint16_t n = rd16(RIVAL_COUNT);
    if (n) {
        int16_t avg = (int16_t)f_divs(n, (uint32_t)(int32_t)(int16_t)rd16(RIVAL_ACCEL_SUM));
        if (rd16(0xFF0666))
            avg = 8;
        wr16(RIVAL_ACCEL_AVG, (uint16_t)avg);
    }
    wr16(RIVAL_ACCEL_SUM, 0);
    wr16(RIVAL_COUNT_LAST, n);
}

/* Five passes of a bubble sort by depth over the five records. */
static void rival_sort(void)
{
    int pass, i;
    for (pass = 0; pass < 5; pass++)
        for (i = 0; i < 4; i++) {
            uint32_t a = RIVAL_RECORDS + 8 * i, b = a + 8;
            rv.d1 = rd16(b + 6);
            rv.d1_set = 1;
            if ((int16_t)rd16(b + 6) < (int16_t)rd16(a + 6)) {
                uint32_t a0 = rd32(a), a1 = rd32(a + 4);
                rv.d4 = a0;
                rv.d5 = a1;
                rv.d4_set = 1;
                wr32(a, rd32(b));
                wr32(a + 4, rd32(b + 4));
                wr32(b, a0);
                wr32(b + 4, a1);
            }
        }
}

/* Rivals close behind one another steer apart. */
static void rival_spacing(uint16_t count)
{
    int i;
    for (i = count - 1; i >= 1; i--) {
        uint32_t cur = RIVAL_RECORDS + 8 * i, prev = cur - 8;
        uint16_t depth = rd16(cur + 6);
        uint16_t reach = (uint16_t)(((uint32_t)(uint16_t)(depth << 5)) / 0xBF);
        int16_t dz = (int16_t)(depth - rd16(prev + 6)), dx;
        uint32_t me, other;
        if ((int16_t)(dz - (int16_t)reach) >= 0)
            continue;
        dx = (int16_t)(rd16(cur + 4) - rd16(prev + 4));
        if ((dx < 0 ? -dx : dx) >= 0x3C)
            continue;
        me = rd32(cur) & 0xffffff;
        other = rd32(prev) & 0xffffff;
        if (dx < 0) {
            wr8(other + 0x3A, rd8(other + 0x3A) | 2);
            wr8(me + 0x3A, rd8(me + 0x3A) | 1);
        } else {
            wr8(me + 0x3A, rd8(me + 0x3A) | 2);
            wr8(other + 0x3A, rd8(other + 0x3A) | 1);
        }
        wr8(me + 0x3A, rd8(me + 0x3A) | 4);
    }
}

/* Activate a free rival ($48BE). */
static void rival_spawn(void)
{
    uint32_t obj = RIVAL_OBJECTS;
    int k;
    rv.d1 = 0x40;
    rv.d1_set = 1;
    for (k = 0; k < 5; k++, obj += OBJ_SIZE) {
        uint8_t f = rd8(obj + 1);
        wr8(obj + 1, f | 1);
        if (!(f & 1))
            break;
    }
    if (k == 5)
        return;
    wr16(obj + 0x0A, rd16(RIVAL_ACCEL_AVG));
    wr8(obj + 0x3D, 0);
    wr8(obj + 0x3E, 0);
    wr16(RIVAL_SPAWN + RS_SPAWNED, rd16(RIVAL_SPAWN + RS_SPAWNED) + 1);
    wr16(obj + OBJ_TILE_BASE, (rd8(RIVAL_SPAWN + RS_SPAWNED + 1) & 1) ? 0x4000 : 0x2000);
    if (rd16(0xFF0666) && !rd16(SFX_COOLDOWN)) {
        wr16(SFX_COOLDOWN, 0x0A);
        sound_command((int16_t)rd16(0xFF0622) < 0 ? 0x95 : 0x97);
    }
}

/* Spawn timing: new rivals appear while the player is fast, or while off
   the road / sliding wide. */
static void rival_spawn_timing(void)
{
    const uint32_t s = RIVAL_SPAWN;
    uint16_t rate = (uint16_t)(rd16(0xFF052C) * 2);
    int16_t left;
    if (rd16(HUD_HIDDEN))
        rate *= 2;
    if ((int16_t)rate > 0x0C)
        rate = 0x0C;
    left = (int16_t)(rd16(s + RS_COUNTDOWN) - rate);
    wr16(s + RS_COUNTDOWN, (uint16_t)left);
    if (left < 0) {
        uint16_t course, k;
        wr16(s + RS_COUNTDOWN, (uint16_t)((rng_next() & 3) * 60 + 0x64));
        course = (rd8(0xFFC754) || rd16(HUD_HIDDEN)) ? 0 : rd16(0xFF053C);
        k = (uint16_t)(course * 9 + rd16(STAGE));
        rv.d1 = k;
        rv.d1_set = 1;
        if ((int8_t)rd16(RIVAL_COUNT_LAST) < (int8_t)rd8(ROM_RIVAL_DENSITY + k))
            wr16(s + RS_PENDING, 1);
    }
    if ((int16_t)rd16(0xFF064A) > 0x64) {
        if (rd16(s + RS_PENDING)) {
            wr16(s + RS_PENDING, 0);
            rival_spawn();
        }
        return;
    }
    wr16(s + RS_PENDING, rd16(s + RS_PENDING) | rd16(0xFF066A));
    if (!rd16(s + RS_PENDING))
        return;
    {
        int16_t c = (int16_t)rd16(0xFF0614);
        if ((c < 0 ? -c : c) <= 0x180) {
            wr16(s + RS_PENDING, 0);
            return;
        }
    }
    {
        int16_t d = (int16_t)(rd16(s + RS_DELAY) - 2);
        uint16_t seed;
        wr16(s + RS_DELAY, (uint16_t)d);
        if (d >= 0)
            return;
        seed = rd16(0xFF0502);
        seed = (uint16_t)(seed >> 1 | seed << 15);
        wr16(0xFF0502, seed);
        wr16(s + RS_DELAY, (uint16_t)(((seed & 7) + 1) * 4));
        rival_spawn();
    }
}

/* Rival bookkeeping for one race frame ($468E). */
void race_rivals(void)
{
    uint16_t count;
    memset(&rv, 0, sizeof rv);
    if (rd16(SFX_COOLDOWN))
        wr16(SFX_COOLDOWN, rd16(SFX_COOLDOWN) - 1);
    rival_average();
    count = rd16(RIVAL_COUNT);
    if ((int16_t)count > 1) {
        rival_sort();
        wr16(RIVAL_COUNT, 0);
        rival_spacing(count);
    } else {
        /* the original clears the count only on the sorting path */
    }
    race_turns();
    if (turn_announce_at >= 0) {
        rv.d1 = (uint16_t)turn_announce_at;
        rv.d1_set = 1;
    }
    rival_spawn_timing();
}

void race_rivals_68k(void)
{
    race_rivals();
    if (rv.d1_set)
        set_dn(1, 2, rv.d1);
    if (rv.d4_set) {
        cpu.d[4] = rv.d4;
        cpu.d[5] = rv.d5;
    }
}
