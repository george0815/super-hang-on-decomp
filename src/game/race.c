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

/* --------------------------------------------------------------- input */

#define PAD1_HELD        0xFFC706   /* S A C B R L D U */
#define THROTTLE_RAMP    0xFFC738
#define BRAKE_RAMP       0xFFC73A
#define STEER_RAMP       0xFFC73C
#define BIKE_THROTTLE    0xFF063E   /* 0..$7F */
#define BIKE_BRAKE       0xFF0642   /* 0..$FF */
#define BIKE_STEER       0xFF063A   /* -$7F..$7F */
#define BIKE_SPEED       0xFF064A
#define ROM_BRAKE_RATES  0x00A03C
#define ROM_STEER_RATES  0x00A004   /* per profile: return, outward, counter */

static uint16_t input_d1;   /* for the adapter */

/* B ramps the throttle up, releasing lets it fall. */
static void read_throttle(void)
{
    uint16_t t = rd16(THROTTLE_RAMP);
    if (!(rd8(PAD1_HELD) & 0x10)) {
        t = t < 0x0F ? 1 : (uint16_t)(t - 0x0F);
        if (t < 1)
            t = 1;
    } else {
        t += 0x14;
        if (t >= 0xA8)
            t = 0xA8;
    }
    wr16(THROTTLE_RAMP, t);
    {
        int16_t v = (int16_t)((int16_t)((t - 1) * 160) / 0xA8);
        wr16(BIKE_THROTTLE, (uint16_t)(v > 0x7F ? 0x7F : v));
    }
}

/* A ramps the brake up at a rate from the handling table. */
static void read_brake(void)
{
    uint16_t profile = (rd16(0xFFC8B6) || rd16(HUD_HIDDEN)) ? rd16(0xFFB61C) : 5;
    uint16_t rate = rd16(ROM_BRAKE_RATES + 2 * profile), b;
    if (!(rd8(PAD1_HELD) & 0x40)) {
        b = 1;
    } else {
        b = (uint16_t)(rd16(BRAKE_RAMP) + rate);
        if (b >= 0xC5)
            b = 0xC5;
    }
    wr16(BRAKE_RAMP, b);
    wr16(BIKE_BRAKE, (uint16_t)(((int32_t)(int16_t)(b - 1) << 8) / 0xC5));
}

/*
 * Left/Right move a steering value away from centre ($80) at one rate,
 * steer back from the far side at another; with neither it returns to
 * centre. A dead zone around centre, then scaled to +-$7F.
 */
static void read_steering(void)
{
    uint16_t profile = (rd16(0xFFC8B6) || rd16(HUD_HIDDEN)) ? rd16(0xFFB618) : 6;
    uint32_t e = ROM_STEER_RATES + 8 * profile;
    uint16_t back = rd16(e), out = rd16(e + 2), counter = rd16(e + 4), v;
    int16_t s, steer;
    if (profile >= 2) {
        uint16_t extra = rd16(BIKE_SPEED) >> 7;
        back += extra;
        out += extra;
        counter += extra;
    }
    input_d1 = back;
    v = rd16(STEER_RAMP);
    if (rd8(PAD1_HELD) & 0x04) {                 /* left */
        if (v >= 0x80) {
            v += out;
            if (v >= 0xC7)
                v = 0xC6;
        } else {
            v += counter;
        }
    } else if (rd8(PAD1_HELD) & 0x08) {          /* right */
        if (v < 0x80) {
            v -= out;
            if (v < 0x3A)
                v = 0x3A;
        } else {
            v -= counter;
        }
    } else if (v != 0x80) {
        v = v > 0x80 ? (uint16_t)(v - back) : (uint16_t)(v + back);
    }
    wr16(STEER_RAMP, v);
    s = (int16_t)v;
    if (s > 0xC6) s = 0xC6;
    else if (s < 0x3A) s = 0x3A;
    if (s >= 0x6C && s <= 0x94)
        s = 0x80;
    steer = (int16_t)(((int32_t)(s - 0x80) << 8) / 0x8C);
    if (steer > 0x7F) steer = 0x7F;
    else if (steer <= -0x7F) steer = -0x7F;
    wr16(BIKE_STEER, (uint16_t)-steer);
}

/* Player controls for one race frame ($975C). */
void race_input(void)
{
    read_throttle();
    read_brake();
    read_steering();
}

void race_input_68k(void)
{
    race_input();
    set_dn(1, 2, input_d1);
}

/* -------------------------------------------------------------- sounds */

#define SFX_STATE        0xFFC7BE   /* previous frame: */
#define SS_OFFROAD       0x00
#define SS_PUSH          0x02
#define SS_CURVE         0x04
#define SS_SKID          0x06
#define SS_SKID_PERIOD   0x08       /* byte */
#define SS_SKID_TIMER    0x09       /* byte */
#define BIKE_PUSH        0xFF065A
#define BIKE_OFFROAD     0xFF066A
#define BIKE_SKIDDING    0xFF06B6
#define SKID_SOUND_ON    0xFF06B8
#define ENGINE_FLAG      0xFFC80A   /* to the driver: off-road rumble */
#define SOUND_REQUEST    0xFFC80B   /* sound command for this frame */
#define ROM_DISTANCE_POINTS 0x009FD0

static int sounds_d1 = -1;

static void skid_sound(int on)
{
    wr8(SOUND_REQUEST, on ? 0x9C : 0xB3);
    wr16(SKID_SOUND_ON, on ? 0xFFFF : 0);
}

/* Sliding against the turn, hard over, makes the bike skid. */
static void detect_skid(void)
{
    const uint32_t s = SFX_STATE;
    int16_t curve = (int16_t)rd16(0xFF0614), prev = (int16_t)rd16(s + SS_CURVE);
    int16_t steer = (int16_t)rd16(BIKE_STEER);
    uint16_t dir = rd16(TURN_DIR);
    int skid = 0;
    if (dir != 1 && (steer < 0 ? -steer : steer) >= 0x70 && curve != prev) {
        if (curve > prev)
            skid = dir != 3 && steer >= 0;
        else
            skid = dir != 2 && steer < 0;
    }
    wr16(BIKE_SKIDDING, skid ? 0xFFFF : 0);
    wr16(0xFF06BA, (uint16_t)(curve - prev));
    wr16(s + SS_CURVE, (uint16_t)curve);
}

/* Engine RPM, flags and this frame's sound request to the driver. */
static void sound_mailbox(void)
{
    if (rd16(0xFF0666))
        wr16(ENGINE_FLAG, 0);                  /* also clears SOUND_REQUEST */
    do
        wr16(Z80_BUSREQ, 0x100);
    while (rd8(Z80_BUSREQ) & 1);
    wr8(0xA01C36, rd8(0xFF0510) ? 0x80 : 0);
    wr8(0xA01C31, rd8(BIKE_SPEED + 1));
    wr8(0xA01C32, rd8(BIKE_SPEED));
    wr8(0xA01C39, rd8(ENGINE_FLAG));
    if (rd8(SOUND_REQUEST)) {
        uint8_t cmd = rd8(SOUND_REQUEST);
        wr8(SOUND_REQUEST, 0);
        wr8(Z80_SOUND_COMMAND, cmd);
    }
    wr16(Z80_BUSREQ, 0);
}

/* Race sound state and distance points for one race frame ($98E4). */
void race_sounds(void)
{
    const uint32_t s = SFX_STATE;
    uint16_t mode = rd16(GAME_MODE) & ~1;

    sounds_d1 = -1;
    if (mode == MODE_RACE) {
        wr16(0xFF06BC, 0);
        wr16(0xFF06BE, 0);
    }
    if (rd16(0xFF06BC) && !rd16(0xFF06BE))
        sound_command((rd8(0xFFC726) & 1) ? 0xB4 : 0xAD);
    if (mode != 0x18 && mode != MODE_RACE)
        return;

    if (!rd16(BIKE_OFFROAD)) {
        uint16_t points = rd16(ROM_DISTANCE_POINTS + 2 * (uint16_t)((int16_t)rd16(BIKE_SPEED) >> 4));
        sounds_d1 = points;
        wr32(0xFF0548, points);
        score_add();
    }
    if (!rd16(s + SS_PUSH)) {
        if (rd16(BIKE_PUSH))
            skid_sound(1);
    } else if (!rd16(BIKE_PUSH)) {
        skid_sound(0);
    }
    wr16(s + SS_PUSH, rd16(BIKE_PUSH));

    detect_skid();
    if (!rd16(BIKE_PUSH)) {
        if (!rd16(s + SS_SKID)) {
            if (rd16(BIKE_SKIDDING)) {
                int8_t t = (int8_t)(rd8(s + SS_SKID_TIMER) - 2);
                wr8(s + SS_SKID_TIMER, (uint8_t)t);
                if (t >= 0)
                    goto offroad_done;         /* the original skips ahead here */
                wr8(s + SS_SKID_TIMER, rd8(s + SS_SKID_PERIOD));
                skid_sound(1);
            }
        } else if (!rd16(BIKE_SKIDDING)) {
            skid_sound(0);
        }
    }
    wr16(s + SS_SKID, rd16(BIKE_SKIDDING));

    if (!rd16(s + SS_OFFROAD)) {
        if (rd16(BIKE_OFFROAD)) {
            wr8(ENGINE_FLAG, 0x80);
            wr8(SOUND_REQUEST, 0xA8);
        }
    } else if (!rd16(BIKE_OFFROAD)) {
        wr8(ENGINE_FLAG, 0);
        wr8(SOUND_REQUEST, 0xB3);
    }
offroad_done:
    wr16(s + SS_OFFROAD, rd16(BIKE_OFFROAD));
    if (!rd8(0xFFC754))
        sound_mailbox();
}

void race_sounds_68k(void)
{
    race_sounds();
    if (sounds_d1 >= 0)
        cpu.d[1] = (uint32_t)sounds_d1;
}

/* --------------------------------------------------------------- speed */

#define SPEED_STATE      0xFFC73E   /* +0 target speed, +4 scroll fraction */
#define BIKE_SPEED_FRAC  0xFF064A   /* long: speed 16.16 */
#define CRASHING         0xFF0666
#define TIME_UP          0xFF06BC
#define STOPPED          0xFF06BE
#define TURBO            0xFF0510
#define ROAD_CURVE       0xFF0614
#define COURSE_DISTANCE  0xFF0530   /* long, capped per course */
#define ROM_TOP_SPEED    0x009F7E   /* long */
#define ROM_BRAKE_LEVELS 0x009FFA   /* four brake thresholds */

static uint16_t speed_d1;

/* Ease the curve back to the centre line by `step` ($9EF2). */
static void curve_recentre(int16_t step)
{
    int16_t c = (int16_t)rd16(ROAD_CURVE);
    if ((c < 0 ? -c : c) < 0x0C || c == 0)
        return;
    wr16(ROAD_CURVE, (uint16_t)(c < 0 ? c + step : c - step));
}

static uint16_t divs_q(int32_t dividend, uint16_t divisor)
{
    return (uint16_t)f_divs(divisor, (uint32_t)dividend);
}

/* 0..3 extra quarters of the acceleration for upgrade levels. */
static int32_t upgrade_bonus(uint16_t level, uint32_t quarter)
{
    int n = level == 0 ? 0 : level == 1 ? 1 : level == 2 ? 2 : 3;
    return (int32_t)(quarter * (uint32_t)n);
}

static int upgraded(void) { return rd16(0xFFC8B6) || rd16(HUD_HIDDEN); }

/* Speed change for one race frame. */
static void speed_update(void)
{
    const uint32_t s = SPEED_STATE;
    uint16_t throttle, target, sp;
    int32_t accel, bonus = 0, brake_unit;

    if (rd16(0xFFC8A0) && !rd16(STOPPED)) {
        if (rd16(0xFFC8A2)) {
            wr16(0xFFC8A2, rd16(0xFFC8A2) - 1);
            wr16(0xFFC8A4, 0x31);
            wr16(BIKE_THROTTLE, 0);
            goto throttle_read;
        }
        wr16(0xFFC8A4, rd16(0xFFC8A4) - 1);
        if (!rd16(0xFFC8A4))
            wr16(0xFFC8A2, 4);
    }
    if (rd16(STOPPED) || !rd8(0xFF0554))
        wr16(BIKE_THROTTLE, 0);
throttle_read:
    throttle = rd16(BIKE_THROTTLE);
    if (rd16(BIKE_PUSH))
        throttle = 0;
    if (rd16(BIKE_OFFROAD))
        throttle = (uint16_t)f_divu(rd16(0x009F90), throttle);
    target = (uint16_t)(((uint32_t)rd16(ROM_TOP_SPEED) * throttle) / 0x7F);
    wr16(s, target);

    accel = (int16_t)divs_q((int32_t)(rd32(ROM_TOP_SPEED) - rd32(BIKE_SPEED_FRAC)), 0x400);
    if (upgraded()) {
        uint32_t quarter = (uint32_t)accel >> 2;
        uint16_t level;
        bonus += upgrade_bonus(rd16(0xFFB620), quarter);
        if (rd16(0xFFC8B6) || (int16_t)rd16(0xFFB64E) >= 0)
            bonus += upgrade_bonus(rd16(0xFFB61E), quarter);
        level = rd16(0xFFB61A);
        accel = level == 0 ? accel * 3 : level >= 4 ? accel * 8 : level == 1 ? accel * 4 : accel * 6;
    } else {
        accel *= 8;
    }
    accel += bonus;

    sp = rd16(BIKE_SPEED);
    if ((int16_t)sp >= (int16_t)rd16(0x009F82)) {
        if ((int16_t)sp > (int16_t)target) {
            int32_t d = (int32_t)rd32(0x009F8A);
            int mul = 4;
            if (upgraded()) {
                uint16_t level = rd16(0xFFB61A);
                mul = level < 2 ? 1 : level < 4 ? 2 : 4;
            }
            accel = -d * mul;
        } else if ((int16_t)sp == (int16_t)target) {
            accel = 0;
        }
        brake_unit = -2 * (int32_t)rd32(0x009F8A);
        if (rd16(BIKE_PUSH)) {
            accel += 3 * brake_unit;
        } else if (!(rd16(HUD_HIDDEN) && (int16_t)rd16(0xFFB64C) < 0)) {
            int16_t b = (int16_t)rd16(BIKE_BRAKE);
            int n = 0;
            while (n < 4 && b >= (int16_t)rd16(ROM_BRAKE_LEVELS + 2 * n))
                n++;
            if (n == 4)
                wr16(0xFF0626, 1);
            accel += n * brake_unit;
        }
    }
    accel += (int32_t)rd32(BIKE_SPEED_FRAC);
    if (accel < 0)
        accel = 0;
    wr32(BIKE_SPEED_FRAC, (uint32_t)accel);

    /* turbo with C above $F0 (mode $18), otherwise drop back to $F1 */
    {
        uint16_t mode = rd16(GAME_MODE) & ~1, step = (uint16_t)accel;   /* the original reuses d1 */
        int turbo_ok = 0;
        if (mode == 0x18) {
            step = 2;
            turbo_ok = 1;
            if (upgraded()) {
                uint16_t level = rd16(0xFFB61A);
                if (level < 3)
                    turbo_ok = 0;
                else
                    step = level == 5 ? 5 : level == 4 ? 3 : 1;
            }
        }
        if (turbo_ok && !(rd16(BIKE_OFFROAD) | rd16(CRASHING) | rd16(TIME_UP)) &&
            (rd8(PAD1_HELD) & 0x20) && (int16_t)rd16(BIKE_SPEED) > 0xF0) {
            wr8(TURBO, 0xFF);
            wr16(BIKE_SPEED, rd16(BIKE_SPEED) + step);
            if ((int16_t)rd16(BIKE_SPEED) > 0x117)
                wr16(BIKE_SPEED, 0x117);
            return;
        }
        wr8(TURBO, 0);
        if ((int16_t)rd16(BIKE_SPEED) > 0xF1) {
            wr16(BIKE_SPEED, rd16(BIKE_SPEED) - step);
            if ((int16_t)rd16(BIKE_SPEED) <= 0xF0)
                wr16(BIKE_SPEED, 0xF1);
        }
    }
}

/* Distance travelled, course progress and road scroll ($9DE4). */
static void travel(void)
{
    const uint32_t s = SPEED_STATE;
    uint32_t step = rd32(0x009F86), total;
    uint16_t q, rem, d34 = 0x22;

    if ((int16_t)rd16(TURN_DIR) > 1) {
        /* on the inside of a turn the bike covers less ground */
        int32_t c = (int16_t)rd16(ROAD_CURVE);
        int16_t f;
        if (rd16(TURN_DIR) != 3)
            c = -c;
        f = (int16_t)(divs_q(c, 0x28) + rd16(TURN_POWER));
        step = f_divs(rd16(TURN_POWER), (uint32_t)((int32_t)(int16_t)step * f));
    }
    step = (uint32_t)(uint16_t)step * rd16(BIKE_SPEED);
    wr32(0xFF0646, step);
    wr32(COURSE_PROGRESS, rd32(COURSE_PROGRESS) + step);
    wr8(0xFF0744, (int16_t)rd16(COURSE_PROGRESS) >= 0x7D0 ? 0xFF : 0);

    total = step + rd32(COURSE_DISTANCE);
    {
        uint32_t cap = rd16(HUD_HIDDEN) ? rd32(0x009FB0 + 4 * rd16(0xFFC85A))
                                        : rd32(0x009FA0 + 4 * rd16(0xFF060C));
        if ((int32_t)total > (int32_t)cap)
            total = cap;
    }
    wr32(COURSE_DISTANCE, total);

    if (!rd8(0xFF0744) && !rd8(0xFF0734)) {
        uint32_t ev = rd32(0xFF0602) + (int16_t)rd16(0xFF0606);
        if ((int16_t)rd16(ev) <= (int16_t)rd16(COURSE_PROGRESS)) {
            wr16(0xFF061E, rd16(ev + 2));
            wr16(0xFF0606, rd16(0xFF0606) + 4);
        }
    }

    q = (uint16_t)(rd16(BIKE_SPEED) / d34);
    rem = (uint16_t)(rd16(BIKE_SPEED) % d34);
    if ((int16_t)rd16(BIKE_SPEED) < 0) {
        /* divu of the sign-extended value, as the original */
        uint32_t r = f_divu(d34, (uint32_t)(int32_t)(int16_t)rd16(BIKE_SPEED));
        q = (uint16_t)r;
        rem = (uint16_t)(r >> 16);
    }
    wr16(s + 4, rd16(s + 4) + rem);
    if ((int16_t)rd16(s + 4) > (int16_t)d34) {
        wr16(s + 4, rd16(s + 4) - d34);
        q++;
    }
    speed_d1 = (uint16_t)(q * 2);
    wr16(0xFF065E, speed_d1);
    wr16(0xFF0662, rd16(0xFF0662) + speed_d1);
}

/* The bike's speed and travel for one race frame ($9B0C). */
void race_speed(void)
{
    if ((rd16(GAME_MODE) & ~1) == 0x10)
        return;
    speed_d1 = 0;
    if (rd16(TIME_UP)) {
        curve_recentre(8);
        if ((int16_t)rd16(COURSE_PROGRESS) > 0x1F) {
            if (!rd16(STOPPED))
                wr32(BIKE_SPEED_FRAC, 0);
            wr16(STOPPED, 0xFFFF);
            wr16(0xFF06B4, 0xFFFF);
            wr16(OBJECTS + OBJ_HANDLER, 0);
            travel();
            return;
        }
    }
    if (rd16(0xFFC89E)) {
        wr16(BIKE_SPEED, 0);
    } else if (rd16(CRASHING)) {
        int16_t sp = (int16_t)rd16(BIKE_SPEED);
        wr16(BIKE_SPEED, (uint16_t)((int16_t)(sp * 30) >> 5));
    } else {
        speed_update();
    }
    travel();
}

void race_speed_68k(void)
{
    race_speed();
    if ((rd16(GAME_MODE) & ~1) != 0x10)
        set_dn(1, 2, speed_d1);
}

/* ------------------------------------------------------------- drift */

#define ROAD_BEND_LINES  0xFF0B00   /* per scanline: accumulated road bend */

static uint16_t drift_d1;

/*
 * Bends push the bike outwards ($E9E8): the difference in road bend between
 * a line ahead (closer during turbo) and the bottom of the screen, scaled
 * by speed, moves the curve value, unless that would leave the road.
 */
void race_drift(void)
{
    uint16_t ahead = rd8(TURBO) ? 0xA6 - 0x1A : 0xA6;
    int16_t bend, push, step = 1, curve;
    drift_d1 = ahead;
    if (rd16(0xFFC7F8))
        return;
    bend = (int16_t)divs_q((int16_t)(rd16(ROAD_BEND_LINES + ahead) - rd16(ROAD_BEND_LINES + 0x1B8)), 0x40);
    if (bend == 0)
        return;
    push = (int16_t)divs_q((int32_t)bend * (int16_t)rd16(BIKE_SPEED), 0x6E);
    if (push < 0)
        step = -1;
    curve = (int16_t)((push + step) * 2 + (int16_t)rd16(ROAD_CURVE));
    drift_d1 = (uint16_t)(curve < 0 ? -curve : curve);
    if ((int16_t)drift_d1 > (int16_t)rd16(0x009F8E))
        return;
    wr16(ROAD_CURVE, (uint16_t)curve);
}

void race_drift_68k(void)
{
    race_drift();
    if (rd16(0xFFC7F8))
        return;
    set_dn(1, 2, drift_d1);
}

/* -------------------------------------------------------- road builder */

#define ROM_SQUARES      0x00F9F2   /* k*k for the hex square root */
#define ROM_SEGMENT_LINES 0x00FA14  /* screen line of each of 33 segments */
#define TRACK_GEOMETRY   0xFF0800   /* long: the course's direction vectors */
#define ROAD_CENTRE_LINES 0xFF0D00
#define ROAD_SCRATCH     0xFF0730
#define ROAD_HEADING     0xFF061A

/*
 * Integer square root, one hex digit at a time from the most significant
 * non-zero byte ($F2C4). Kept literal so rounding matches the original.
 */
static uint16_t isqrt_hex(uint32_t n)
{
    uint32_t d0 = n, d1, d3 = 0, d7;
    uint16_t d4 = 0, d5;
    int32_t d6;
    int d2 = 3;
    if (n == 0)
        return 0;
    for (;;) {
        d0 = d0 << 8 | d0 >> 24;
        if (d0 & 0xff)
            break;
        if (--d2 < 0)
            break;
    }
    d1 = d0 & 0xff;
    for (;;) {
        int32_t t;
        d5 = (uint16_t)-2;
        d6 = -(int32_t)d3;
        do {
            d5 += 2;
            d6 += (int32_t)d3;
            t = (int32_t)(rd16(ROM_SQUARES + d5) + d6 - d1);
        } while (t < 0);
        if (t != 0) {
            d5 -= 2;
            d6 -= (int32_t)d3;
            d7 = (uint32_t)(rd16(ROM_SQUARES + d5) + d6);
            d7 = d1 - d7;          /* remainder (sub then exg) */
        } else {
            d7 = 0;
        }
        d5 >>= 1;
        d4 = (uint16_t)((d4 << 4) + d5);
        if (d2 == 0)
            break;
        d3 = (d3 + 2u * d5) << 4;
        d7 <<= 8;
        d0 = d0 << 8 | d0 >> 24;
        d1 = (d0 & 0xff) + d7;
        d2--;
    }
    return d4;
}

static uint16_t road_d1;

/* Direction of the track at the bike: a binary angle ($200 per quadrant). */
static uint16_t track_heading(uint32_t p)
{
    int16_t x = (int16_t)(2 * (rd16(p) + rd16(p + 4)));
    int16_t y = (int16_t)(2 * (rd16(p + 2) + rd16(p + 6)));
    int16_t ax = x < 0 ? (int16_t)-x : x, ay = y < 0 ? (int16_t)-y : y;
    uint16_t a;
    int steep = (int16_t)(ax - ay) < 0;
    if (steep)
        a = divs_q((int32_t)x << 8, (uint16_t)y);
    else
        a = divs_q(-((int32_t)y << 8), (uint16_t)x);
    if (x > 0 || (x == 0 && y >= 0))
        a += 0x200;
    else
        a += 0x600;
    if (steep)
        a += ((int32_t)x * y < 0) ? 0x200 : 0xFE00;
    return a;
}

/*
 * Project the road ahead into the per-line bend table ($ED7C): walk 33
 * track segments from the bike, rotate them into the bike's heading,
 * divide the sideways offset by depth and fill the scanlines between
 * consecutive segments by linear interpolation; then double the 96 lines
 * to 192 and derive the road centre lines (plus the sideways sweep while
 * being pushed).
 */
void race_build_road(void)
{
    const uint32_t sc = ROAD_SCRATCH;
    uint32_t base, p;
    uint16_t idx, len;
    int16_t vx, vy, dirx, diry, prev_x = 0, prev_line = 0;
    int32_t px = 0, py = 0;
    int seg, out = 0xBE;

    if (rd16(0xFFC8A8))
        return;
    idx = (uint16_t)(((int16_t)rd16(COURSE_PROGRESS) >> 1) * 4);
    wr16(sc, idx);
    base = rd32(TRACK_GEOMETRY);
    p = base + (int16_t)idx;
    vx = (int16_t)(rd16(p) + rd16(p + 4));
    vy = (int16_t)(rd16(p + 2) + rd16(p + 6));
    wr16(sc + 0x30, (uint16_t)vx);
    wr16(sc + 0x40, (uint16_t)vy);
    len = isqrt_hex((uint32_t)((int32_t)vx * vx + (int32_t)vy * vy));
    dirx = (int16_t)divs_q((int32_t)vx << 14, len);
    diry = (int16_t)divs_q((int32_t)vy << 14, len);
    wr16(sc + 0x50, (uint16_t)dirx);
    wr16(sc + 0x60, (uint16_t)diry);
    wr16(ROAD_HEADING, track_heading(p));
    wr32(sc + 0x70, 0);
    wr32(sc + 0x80, 0);
    wr16(sc + 0x90, 0);
    wr16(sc + 0xA0, 0);

    for (seg = 0x20; seg >= 0; seg--, p += 4) {
        int16_t x, y, side, line, span;
        int32_t fwd;
        px += (int16_t)rd16(p) + (int16_t)rd16(p + 4);
        py += (int16_t)rd16(p + 2) + (int16_t)rd16(p + 6);
        wr32(sc + 0x70, (uint32_t)px);
        wr32(sc + 0x80, (uint32_t)py);
        x = (int16_t)(px >> 5);
        y = (int16_t)(py >> 5);
        fwd = ((int32_t)x * dirx + (int32_t)y * diry) >> 7;
        side = (int16_t)divs_q((int32_t)x * diry - (int32_t)y * dirx, (uint16_t)((fwd >> 7) + 0x410));
        line = (int16_t)rd16(ROM_SEGMENT_LINES + 2 * seg);
        wr16(sc + 0xB0, (uint16_t)side);
        wr16(sc + 0xC0, (uint16_t)line);
        span = (int16_t)(line - prev_line);
        if (span < 0)
            break;
        if (span == 0)
            continue;
        {
            int16_t step = (int16_t)divs_q((int16_t)(side - prev_x), (uint16_t)span);
            int16_t v = prev_x, l = prev_line;
            do {
                v = (int16_t)(v + step);
                l++;
                if (v > 0x3200 || v < (int16_t)0xCE00)
                    goto expand;
                wr16(ROAD_BEND_LINES + out, (uint16_t)v);
                out -= 2;
                if (out < 0)
                    goto expand;
            } while (l != line);
        }
        prev_x = side;
        prev_line = line;
        wr16(sc + 0x90, (uint16_t)side);
        wr16(sc + 0xA0, (uint16_t)line);
    }
expand:
    /* 96 lines -> 192 by inserting midpoints, from the bottom up */
    {
        uint32_t src = ROAD_BEND_LINES + 0xBE, dst = ROAD_BEND_LINES + 0x180;
        int k;
        for (k = 0; k < 0x5F; k++) {
            int16_t a = (int16_t)rd16(src), b;
            dst -= 2;
            wr16(dst, (uint16_t)a);
            src -= 2;
            b = (int16_t)rd16(src);
            a = (int16_t)(a + ((int16_t)(b - a) >> 1));
            dst -= 2;
            wr16(dst, (uint16_t)a);
        }
        {
            int16_t b = (int16_t)rd16(src + 2), d = (int16_t)((int16_t)(rd16(src) - b) >> 1);
            road_d1 = (uint16_t)d;
            wr16(src + 2, (uint16_t)(b + d));
        }
    }
    /* road centre lines: the bend, plus a sweep while being pushed */
    {
        int16_t sweep = (int16_t)(rd16(0xFF0622) << 6);
        int k;
        if (sweep == 0) {
            for (k = 0; k < 0xE0; k++)
                wr16(ROAD_CENTRE_LINES + 2 * k, rd16(ROAD_BEND_LINES + 2 * k));
            return;
        }
        sweep = (int16_t)divs_q(sweep, 0xE0);
        {
            uint16_t acc = 0;
            for (k = 0; k < 0xE0; k++) {
                uint16_t b = rd16(ROAD_BEND_LINES + 2 * k);
                wr16(ROAD_CENTRE_LINES + 2 * k, (uint16_t)((b == 0x3210 ? 0 : acc) + b));
                acc = (uint16_t)(acc + sweep);
            }
            road_d1 = acc;
        }
    }
}

void race_build_road_68k(void)
{
    if (rd16(0xFFC8A8))
        return;
    race_build_road();
    set_dn(1, 2, road_d1);
    cpu.d[4] &= 0xffff;        /* moveq #0,d4 before the fill index */
}
