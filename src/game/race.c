/* The race: course scripts, roadside placement, checkpoints. */

#include <string.h>

#include "game.h"
#include "sho_funcs.h"

#define GAME_MODE        0xFFC704
#define MODE_RACE        0x14
#define HUD_HIDDEN       0xFFC82C
#define OBJECTS_FROZEN   0xFFC756
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

/* -------------------------------------------------------------- stages */

#define COURSE           0xFF060C
#define STAGE_DATA_INDEX 0x00FB86   /* per course: 9 words */
#define STAGE_DATA       0x00FB26   /* pointers to stage records */
#define ROM_STAGES       0x00FA66   /* stages per course (bytes) */
#define ROM_STAGES_HIDDEN 0x00FA56
#define EVENT_SCRIPT     0xFF0602
#define FADE_TARGET      0xFF08F0   /* 16 colours being faded towards */
#define FADE_CURRENT     0xFF0910
#define FADE_ACTIVE      0xFF08D0
#define FADE_TIMER       0xFFC74E
#define VRAM_CLEAR_STEP  0xFF057C
#define PALETTE_BUFFER   0xFFD380
#define PALETTE_DIRTY    0xFFC708

/* Stage record: palette pointers and the three course scripts ($ED50). */
static uint32_t stage_record(uint16_t stage)
{
    uint16_t i = (uint16_t)(rd16(COURSE) * 18 + stage * 2);
    return rd32(STAGE_DATA + 4 * rd16(STAGE_DATA_INDEX + i));
}

static void load_turn_script(void)  { wr32(TURN_SCRIPT, rd32(stage_record(rd16(STAGE)) + 8)); }
static void load_event_script(void) { wr32(EVENT_SCRIPT, rd32(stage_record(rd16(STAGE)) + 0x0C)); }
static void load_scenery_script(void) { wr32(SCENERY_SCRIPT, rd32(stage_record(rd16(STAGE)) + 0x10)); }

/* Move a colour two steps per channel towards a target ($E57A/$E594). */
static uint16_t colour_step(uint16_t target, uint16_t cur)
{
    uint16_t out = 0;
    int sh;
    for (sh = 8; sh >= 0; sh -= 4) {
        uint16_t t = (target >> sh) & 0xE, c = (cur >> sh) & 0xE;
        if (t != c)
            c = (uint16_t)(c + (t > c ? 2 : -2));
        out |= (uint16_t)((c & 0xE) << sh);
    }
    return out;
}

static int fade_stepped;   /* for the adapter */

/* Every 4 frames move the fading colours a step and show them ($E524). */
static void palette_fade_step(void)
{
    int k;
    uint32_t pal = PALETTE_BUFFER + 0x60;   /* $FFD3E0 */
    fade_stepped = 0;
    if (!rd16(FADE_ACTIVE))
        return;
    wr16(FADE_TIMER, rd16(FADE_TIMER) - 1);
    if (rd16(FADE_TIMER))
        return;
    wr16(FADE_TIMER, 4);
    fade_stepped = 1;
    for (k = 0; k < 11; k++)
        wr16(FADE_CURRENT + 2 * k, colour_step(rd16(FADE_TARGET + 2 * k), rd16(FADE_CURRENT + 2 * k)));
    /* the faded colours land in two palette lines */
    wr16(pal, rd16(FADE_CURRENT));
    for (k = 0; k < 5; k++) {
        wr16(pal - 0x0A + 2 * k, rd16(FADE_CURRENT + 2 + 4 * k));
        wr16(pal + 0x16 + 2 * k, rd16(FADE_CURRENT + 4 + 4 * k));
    }
    wr8(PALETTE_DIRTY, rd8(PALETTE_DIRTY) | 1);
}

static void copy_words(uint32_t dst, uint32_t src, int words)
{
    int k;
    for (k = 0; k < words; k++)
        wr16(dst + 2 * k, rd16(src + 2 * k));
}

/* Start fading the scenery colours to the next stage's ($E4BA). */
static void palette_fade_setup(void)
{
    uint32_t next = stage_record(rd16(STAGE) + 1), now, a, b;
    wr16(FADE_ACTIVE, 8);
    a = rd32(next);
    wr16(FADE_TARGET, rd16(a));
    copy_words(0xFFD396, a + 2, 5);
    copy_words(0xFFD3E8, a + 12, 7);
    b = rd32(next + 4);
    copy_words(FADE_TARGET + 2, b, 10);
    now = stage_record(rd16(STAGE));
    wr16(FADE_CURRENT, rd16(rd32(now)));
    copy_words(FADE_CURRENT + 2, rd32(now + 4), 10);
    wr16(FADE_TIMER, 4);
}

/* Course scripts for a new stage, or the goal after the last one. */
static void next_stage(void)
{
    uint16_t stages;
    if (rd16(HUD_HIDDEN)) {
        wr16(STAGE, rd16(STAGE) + 1);
        stages = (uint16_t)((rd16(0xFFC85A) & 0xFF00) | rd8(ROM_STAGES_HIDDEN + rd16(0xFFC85A)));
        if (stages != rd16(STAGE)) {
            wr32(TURN_SCRIPT, rd32(0xFFC84E));
            wr32(EVENT_SCRIPT, rd32(0xFFC852));
            wr32(SCENERY_SCRIPT, rd32(0xFFC856));
            goto reset;
        }
    } else {
        wr16(STAGE, rd16(STAGE) + 1);
        stages = (uint16_t)((rd16(COURSE) & 0xFF00) | rd8(ROM_STAGES + rd16(COURSE)));
        if (stages != rd16(STAGE)) {
            wr16(0xFFC748, 0);
            load_turn_script();
            load_event_script();
            load_scenery_script();
            wr16(0xFF0610, 0xFFFF);
            goto reset;
        }
    }
    /* goal: back to stage 0 with the course's finishing scripts */
    wr16(STAGE, 0);
    wr16(TIME_UP, 0x1ED0);
    wr8(0xFF06C5, 0xFF);
    wr32(TURN_SCRIPT, rd32(0x0576F0));
    wr32(EVENT_SCRIPT, rd32(0x0576F4));
    wr32(SCENERY_SCRIPT, rd32(0x0576F8 + 4 * rd16(COURSE)));
    wr16(0xFF0610, 0xFFFF);
reset:
    wr32(COURSE_PROGRESS, 0);
    wr32(ROAD_HEADING, 0);
    wr32(0xFF0578, 0);
    wr16(0xFF0606, 0);
    wr16(0xFFC74C, rd16(0xFFC74A) & 0xFF00);
}

/* Stage changes for one race frame ($E348). */
void race_stage(void)
{
    palette_fade_step();
    if ((int16_t)rd16(VRAM_CLEAR_STEP) < 0) {
        if (!(rd16(0xFFC748) | rd16(HUD_HIDDEN)) && (int16_t)rd16(COURSE_PROGRESS) >= 0x78A) {
            uint16_t stage = rd16(STAGE);
            wr16(0xFFC748, 0xFF);
            wr16(0xFF057E, stage);
            if ((uint8_t)(stage + 1) == rd8(ROM_STAGES + rd16(COURSE))) {
                wr16(VRAM_CLEAR_STEP, 0xFF80);    /* last stage: just clear */
            } else {
                wr16(VRAM_CLEAR_STEP, 0);         /* reload the next stage's scenery */
                palette_fade_setup();
                wr16(0xFFC806, rd16(0xFFC806) + 1);
                if ((int16_t)rd16(VRAM_CLEAR_STEP) >= 6)
                    wr16(VRAM_CLEAR_STEP, 0xFFFF);
            }
        }
    } else if ((int16_t)rd16(VRAM_CLEAR_STEP) >= 6) {
        wr16(VRAM_CLEAR_STEP, 0xFFFF);
    }
    if ((int16_t)rd16(COURSE_PROGRESS) > 0x800)
        next_stage();
    if (!rd16(STOPPED) && (int16_t)rd16(0xFF06B4) < 0)
        wr16(0xFF06B4, 0);
}

void race_stage_68k(void)
{
    race_stage();
    if (fade_stepped)
        cpu.d[4] &= 0xffff;       /* moveq #0,d4 in the colour step */
}

void load_turn_script_68k(void)    { load_turn_script(); }
void load_event_script_68k(void)   { load_event_script(); }
void load_scenery_script_68k(void) { load_scenery_script(); }
void stage_record_68k(void)        { cpu.a[0] = stage_record(rd16(STAGE)); }

/* ------------------------------------------------- per-frame helpers */

#define FRAME_COUNTER_BYTE 0xFFC726

/* Flash two palette entries grey every other frame pair ($8D0C). */
static void flash_colours(void)
{
    uint32_t c = 0x444;
    if (rd8(FRAME_COUNTER_BYTE) & 2)
        c = c << 16 | c >> 16;
    wr32(0xFFD3B0, c);
    wr32(0xFFD3D0, c);
}

void flash_colours_68k(void) { flash_colours(); }

/*
 * Original mode: count the parts worn out (six wear words at $FFB648,
 * negative = broken) and flag a change; the fourth one breaking first
 * also starts a warning ($8DE6).
 */
#define PART_WEAR        0xFFB648
static void parts_check(void)
{
    uint16_t broken = 0;
    int k;
    if (!rd16(HUD_HIDDEN))
        return;
    if ((int16_t)rd16(PART_WEAR + 6) < 0 && !rd16(0xFFC8A0)) {
        wr16(0xFFC8A2, 4);
        wr16(0xFFC8A4, 0x31);
        wr16(0xFFC8A0, 1);
    }
    for (k = 0; k < 6; k++)
        if ((int16_t)rd16(PART_WEAR + 2 * k) < 0)
            broken++;
    if (broken != rd16(0xFFC896)) {
        wr16(0xFFC896, broken);
        wr16(0xFFC89A, rd16(0xFFC89A) + 1);
    }
}

void parts_check_68k(void)
{
    parts_check();
    if (rd16(HUD_HIDDEN))
        cpu.d[1] = 0xFFFF;        /* moveq #5,d1 run out by dbf */
}

/*
 * After a stage change, point the road at the new stage's geometry, or at
 * the goal straight ($744F0) when the course is finished ($E2EC).
 */
#define ROM_STAGE_GEOMETRY 0x00FA6E
static uint16_t geometry_d0, geometry_d1;
void stage_geometry(void)
{
    if (!rd16(0xFF0610))
        return;
    wr32(TRACK_GEOMETRY, 0x0744F0);
    if (!rd8(0xFF06C5)) {
        geometry_d1 = (uint16_t)(rd16(STAGE) * 2);
        geometry_d0 = (uint16_t)(rd16(STAGE_DATA_INDEX + rd16(COURSE) * 18 + geometry_d1) * 4);
        wr32(TRACK_GEOMETRY, rd32(ROM_STAGE_GEOMETRY + geometry_d0));
        set_dn(0, 2, geometry_d0);
        set_dn(1, 2, geometry_d1);
    }
    wr16(0xFF0610, 0);
    wr8(0xFF06C5, 0);
}

void stage_geometry_68k(void) { stage_geometry(); }

/*
 * Background hill columns ($EA46): twelve layers take a height from the
 * road state (stride 16 at $FF0DC8, >> 9). When one changes, a 20-byte
 * column of tile words is picked from a per-layer table at $FFE100
 * (stride $AA) by the height and direction of the change, or a blank
 * column from ROM, and staged with its size and VRAM offset for the
 * VBlank upload (records of $18 bytes from $FF094A).
 */
#define HILL_HEIGHTS     0xFF0930
#define HILL_SOURCE      0xFF0DC8
#define HILL_UPLOADS     0xFF094E
#define HILL_TABLES      0xFFE100
#define ROM_HILL_BLANK   0x00EB02
static int hill_loaded;
static uint32_t hill_d1, hill_d4;   /* registers left by the last copy */
static uint16_t hill_last;
static int hill_changed;            /* the last layer changed */
static void hill_columns(void)
{
    uint32_t heights = HILL_HEIGHTS, src = HILL_SOURCE, up = HILL_UPLOADS, table = HILL_TABLES;
    uint16_t vram = 0xEE00;
    int k, i;
    hill_loaded = 0;
    for (k = 0; k < 12; k++, src += 0x10, up += 0x18, table += 0xAA, vram += 0x80) {
        uint16_t now = (uint16_t)((int16_t)rd16(src) >> 9);
        uint16_t old = rd16(heights + 2 * k), diff, d1;
        uint32_t col;
        int blank;
        wr16(heights + 2 * k, now);
        hill_last = now;
        hill_changed = old != now;
        if (!hill_changed)
            continue;
        diff = (uint16_t)(old - now);
        if ((int16_t)diff < 0) diff = (uint16_t)-diff;
        wr16(up - 4, diff);
        /* rising (old above now, same sign) or falling */
        if (((int16_t)old < 0) == ((int16_t)now < 0) ? old >= now : (int16_t)now < 0) {
            col = table + 0x94;
            d1 = (uint16_t)(-2 * now - 2 * diff);
            blank = (int16_t)d1 >= 0 && d1 >= 4;
        } else {
            col = table + 0x14;
            d1 = (uint16_t)(-2 * now);
            blank = (int16_t)d1 < 0 && d1 < 0xFFEC;
        }
        col = blank ? ROM_HILL_BLANK : col + (int16_t)d1;
        wr16(up - 2, (uint16_t)((d1 & 0x7E) | vram));
        for (i = 0; i < 20; i += 4)
            wr32(up + i, rd32(col + i));
        hill_loaded = 1;
        hill_d1 = rd32(col + 4);
        hill_d4 = rd32(col + 16);
    }
}

void hill_columns_68k(void)
{
    hill_columns();
    if (hill_loaded)
        cpu.d[1] = hill_d1, cpu.d[4] = hill_d4;
    if (!hill_changed)
        set_dn(1, 2, hill_last);  /* the last layer's height stays in d1 */
}

/*
 * Road line tables for the scroll code ($EFA8):
 *  - $FF0F80..$FF107E: each road line's height (>> 4) plus 8 per line,
 *    filled from the bottom;
 *  - $FF1300: for each screen row, the odd road line index (2 * line + 1)
 *    drawn there, found by walking the road heights from the near end;
 *    rows the road climbs back over (hills) are filled backwards, and the
 *    last such row is noted at ROAD_HIDDEN_FROM for the line marker.
 */
#define ROAD_Y_LINES     0xFF0F00
#define ROAD_ROW_LINES   0xFF1300
#define ROAD_ROWS_BLANK  0xFF0754   /* rows left unfilled at the top */
#define ROAD_HIDDEN_FROM 0xFF0758   /* word: line, word: row */
static int road_rows_done;
static void road_rows(void)
{
    uint32_t row;
    int16_t prev, h;
    int k, n;
    road_rows_done = 0;
    for (k = 0; k < 0xC0; k++)
        wr16(0xFF107E - 2 * k, (uint16_t)(((int16_t)rd16(0xFF127E - 2 * k) >> 4) + 8 * k));
    n = 0x80 - (int16_t)rd16(ROAD_ROWS_BLANK);
    for (k = 0; k < (uint16_t)(n - 1) + 1; k++)
        wr16(ROAD_ROW_LINES + 2 * k, 0);
    wr16(ROAD_HIDDEN_FROM, 1);
    prev = (int16_t)rd16(ROAD_Y_LINES) >> 4;
    if (prev >= 0 && 0xDF - prev >= 0) {
        row = ROAD_ROW_LINES + 2 * (0xDF - prev);
        for (k = 0; k < 0xBF; k++) {
            uint16_t line = (uint16_t)(2 * k + 1);
            int16_t d;
            h = (int16_t)rd16(ROAD_Y_LINES + 2 + 2 * k) >> 4;
            d = (int16_t)(prev - h);
            if (d == 0) {
                wr16(row, line);
            } else if (d > 0) {
                while (d--) { row += 2; wr16(row, line); }
            } else {
                while (d++) { row -= 2; wr16(row, line); }
                wr16(ROAD_HIDDEN_FROM, line);
                wr16(ROAD_HIDDEN_FROM + 2, (uint16_t)row);
            }
            prev = h;
        }
        road_rows_done = 1;
    }
    wr16(ROAD_HIDDEN_FROM, (uint16_t)((int16_t)rd16(ROAD_HIDDEN_FROM) >> 1));
    wr16(ROAD_HIDDEN_FROM + 2, (uint16_t)((rd16(ROAD_HIDDEN_FROM + 2) & 0x1FF) >> 1));
}

void road_rows_68k(void)
{
    road_rows();
    set_dn(5, 2, 8);
    if (road_rows_done)
        set_dn(1, 2, 0);
}

/* ---------------------------------------------------------- the clock */

/*
 * FF0554 holds the clock: time left (BCD), frames until the next second,
 * frames per second, frames into the current lap second, lap seconds and
 * hundredths (BCD).
 */
#define CLOCK            0xFF0554
#define ROM_HUNDREDTHS   0x0096BC   /* frame countdown -> hundredths (BCD) */
#define TOTAL_TIME       0xFFC82E   /* original mode: elapsed time record */
#define SPLIT_TIMES      0xFFC860   /* original mode: 8 bytes per segment */
#define SPLIT_INDEX      0xFFC88E

static uint32_t clock_d1;   /* for the adapter */
static int clock_d1_set;

/* Elapsed time: one frame tick of a minutes/seconds/hundredths record
   (a byte at +1, +2, +3) with its own frame countdown ($9682). */
static void clock_tick(uint32_t countdown, uint32_t time, uint8_t fps)
{
    wr8(countdown, rd8(countdown) - 1);
    if (!rd8(countdown)) {
        uint8_t s;
        wr8(countdown, fps);
        s = bcd_add(rd8(time + 2), 1);
        wr8(time + 2, s);
        if (s >= 0x60) {
            wr8(time + 2, 0);
            wr8(time + 1, bcd_add(rd8(time + 1), 1));
        }
        clock_d1 = 1;
        clock_d1_set = 1;
    }
    wr8(time + 3, rd8(ROM_HUNDREDTHS + rd8(countdown)));
}

/* One frame of the race clock ($9626). */
static void race_clock(void)
{
    uint8_t fps = rd8(CLOCK + 2), frame;
    clock_d1_set = 0;
    if (rd16(HUD_HIDDEN)) {
        uint16_t seg;
        uint32_t split;
        if (rd16(OBJECTS_FROZEN))
            return;
        clock_tick(CLOCK + 1, TOTAL_TIME, fps);
        seg = rd16(SPLIT_INDEX);
        if (seg >= 6 || rd16(0xFFC8B4))   /* unsigned: negative is out too */
            return;
        split = SPLIT_TIMES + 8 * seg;
        clock_tick(0xFFC88C, split, fps);
        clock_d1 = rd32(split - 4);
        clock_d1_set = 1;
        if (rd32(split) >= clock_d1)       /* slower than the previous split */
            wr16(0xFFC8B4, rd16(0xFFC8B4) + 1);
        return;
    }
    /* arcade: count the time left down, and the lap time up */
    if ((int8_t)fps < 0)
        return;
    wr8(CLOCK + 1, rd8(CLOCK + 1) - 1);
    if (!rd8(CLOCK + 1)) {
        wr8(CLOCK + 1, fps);
        wr8(CLOCK, bcd_sub(rd8(CLOCK), 1));
    }
    frame = (uint8_t)(rd8(CLOCK + 3) + 1);
    wr8(CLOCK + 3, frame);
    if (frame >= fps) {
        wr8(CLOCK + 3, 0);
        wr8(CLOCK + 5, 0);
        wr8(CLOCK + 4, bcd_add(rd8(CLOCK + 4), 1));
        return;
    }
    {
        uint8_t hundredths = (uint8_t)(frame * 100u / fps);
        wr8(CLOCK + 5, (uint8_t)((hundredths / 10 & 0xF) << 4 | (hundredths % 10 & 0xF)));
        clock_d1 = (uint32_t)(hundredths / 10 & 0xF) << 4;
        clock_d1_set = 1;
    }
}

void race_clock_68k(void)
{
    race_clock();
    if (clock_d1_set)
        cpu.d[1] = clock_d1;
}

/* -------------------------------------------------------- checkpoints */

/*
 * Arcade segment times: per course, an offset into the record area at
 * $FF0400 and the index of the last segment (ROM_SEGMENT_RECORDS). The
 * area holds this run's segment times (BCD seconds/hundredths words) and,
 * $78 bytes on, the best time for each segment.
 */
#define CHECKPOINT       0xFF052C   /* checkpoints passed (binary) */
#define CHECKPOINT_BCD   0xFF052E
#define SEGMENT_TIME     0xFF0558   /* = CLOCK + 4 */
#define SEGMENT_RECORDS  0xFF0400
#define ROM_SEGMENT_RECORDS 0x00E7F8
#define RUN_TOTAL        0xFF04EC   /* long: 00 MM SS hh */
#define BEST_TOTALS      0xFF04D8   /* long per course */
#define DIGIT_VALUE      0xFF04E8   /* long being drawn; word: new record */
#define DIGIT_BUFFER     0xFF0582
#define ROM_CHECKPOINTS  0x00FA6A   /* checkpoints per course (bytes) */
#define ROM_CHECKPOINTS_HIDDEN 0x00FA5E
#define ROM_EXTEND_TIME  0x010A6E   /* per difficulty, 18 BCD seconds */

/* BCD -> binary for a 4-digit word ($E8DC). */
static uint16_t bcd_to_bin(uint16_t v)
{
    return (uint16_t)((v >> 12 & 0xF) * 1000 + (v >> 8 & 0xF) * 100 + (v >> 4 & 0xF) * 10 + (v & 0xF));
}

/* Binary -> BCD, five digits ($E8A6). */
static uint32_t bin_to_bcd(uint16_t v)
{
    uint32_t d3 = 0, d0 = v;
    static const uint16_t div[4] = { 10000, 1000, 100, 10 };
    int k;
    for (k = 0; k < 4; k++) {
        d3 = (d3 & ~0xFFu) | ((d3 | (d0 / div[k])) & 0xFF);
        d3 <<= 4;
        d0 %= div[k];
    }
    return d3 | d0;
}

/* Add a 3-byte minutes/seconds/hundredths BCD time to another, seconds
   wrapping at 60 and the sum capped at 99'59"99 ($E6EC). */
static void time_add(uint32_t dst, uint32_t src)
{
    int x = 0, mid_carry = 0, k;
    uint8_t s;
    for (k = 2; k >= 0; k--) {
        wr8(dst + k, bcd_addx(rd8(dst + k), rd8(src + k), &x));
        if (k == 1)
            mid_carry = x;
    }
    if (x)
        goto cap;
    if (mid_carry) {
        wr8(dst + 1, bcd_add(rd8(dst + 1), 0x40));   /* 100 -> 60 seconds */
        return;
    }
    s = rd8(dst + 1);
    if (s < 0x60)
        return;
    wr8(dst + 1, bcd_sub(s, 0x60));
    s = bcd_addx(rd8(dst), 1, &x);
    if (x)
        goto cap;
    wr8(dst, s);
    return;
cap:
    wr32(dst - 1, 0x00995999);
}

/*
 * Record the time of the segment just finished ($E6DC). In original mode
 * the clock is added to a running total; in arcade mode the segment time
 * is stored, its best kept, and after the last segment the run's total is
 * summed and compared with the course record.
 */
static void segment_record(uint16_t checkpoint)
{
    uint32_t course = ROM_SEGMENT_RECORDS + 4 * rd16(COURSE), times, k;
    uint16_t seg, t, last;
    uint32_t total, d0;
    if (rd16(HUD_HIDDEN)) {
        time_add(0xFFC833, 0xFFC82F);
        return;
    }
    times = SEGMENT_RECORDS + rd16(course);
    seg = (uint16_t)(checkpoint - 2);
    t = rd16(SEGMENT_TIME);
    wr16(times + 2 * seg, t);
    if (t < rd16(times + 0x78 + 2 * seg))
        wr16(times + 0x78 + 2 * seg, t);
    last = rd16(course + 2);
    if (seg < last)
        return;
    /* the run's total: BCD sum of every segment time */
    wr32(RUN_TOTAL, 0);
    for (k = 0; k <= last; k++) {
        uint32_t sum = 0, a = rd32(RUN_TOTAL), b = rd16(times + 2 * k);
        int x = 0, i;
        wr32(DIGIT_VALUE, b);
        for (i = 0; i < 32; i += 8)
            sum |= (uint32_t)bcd_addx((uint8_t)(a >> i), (uint8_t)(b >> i), &x) << i;
        wr32(RUN_TOTAL, sum);
    }
    /* seconds -> minutes (binary word) and seconds (BCD byte) */
    total = rd32(RUN_TOTAL) >> 8;
    d0 = (total & 0xFFFF0000u) | bcd_to_bin((uint16_t)total);
    if (d0 / 60 <= 0xFFFF)
        d0 = (d0 % 60) << 16 | (d0 / 60);
    wr16(RUN_TOTAL, (uint16_t)d0);
    wr8(RUN_TOTAL + 2, (uint8_t)bin_to_bcd((uint16_t)(d0 >> 16)));
    total = rd32(RUN_TOTAL);
    if (total < rd32(BEST_TOTALS + 4 * rd16(COURSE))) {
        wr32(BEST_TOTALS + 4 * rd16(COURSE), total);
        wr16(DIGIT_VALUE, 0xFFFF);
    }
}

/* Two BCD digits as tiles, the first blank when zero ($BCE2, $BCC8). */
static uint32_t digit_pair(uint32_t dst, uint8_t v, int blank)
{
    uint16_t hi = v >> 4 & 0xF;
    wr16(dst, blank && !hi ? 0 : hi + 0xA614);
    wr16(dst + 2, (uint16_t)((v & 0xF) + 0xA614));
    return dst + 4;
}

/* A minutes/seconds/hundredths time as tiles with ' and " ($BC66). */
void time_tiles(uint32_t time, uint32_t dst)
{
    dst = digit_pair(dst, rd8(time), 1);
    wr16(dst, 0xC63C);
    dst = digit_pair(dst + 2, rd8(time + 1), 0);
    wr16(dst, 0xC63D);
    digit_pair(dst + 2, rd8(time + 2), 0);
}

/* Six of eight tile words into sprite pieces 6 bytes apart. */
static void tiles_to_sprites(uint32_t src, uint32_t sprite, const int *pick, int n)
{
    int k;
    for (k = 0; k < n; k++)
        wr16(sprite + 6 * k, rd16(src + 2 * pick[k]));
}

/* An arcade segment time (BCD seconds, hundredths) as digits ($BFEE):
   seconds past 59 show as a leading 1 minute. */
static void segment_digits(uint32_t time)
{
    uint32_t v = rd8(time);
    wr32(DIGIT_VALUE, 0);
    if (v >= 0x60)
        v = (v - 0x60) | 0x1000;
    v = (v << 12 & 0xFFFFFF00u) | rd8(time + 1);
    wr32(DIGIT_VALUE, v);
    hex_tiles(DIGIT_BUFFER, rd32(DIGIT_VALUE), 8, 0xA0, 1);
}

/* Show the time of the checkpoint just passed ($E808). */
static int checkpoint_d4_set;
static void checkpoint_times(uint16_t checkpoint)
{
    static const int time6[6] = { 0, 1, 3, 4, 6, 7 }, seg5[5] = { 1, 3, 4, 6, 7 };
    uint32_t course;
    if (rd16(HUD_HIDDEN)) {
        time_tiles(0xFFC82F, 0xFFC83E);
        tiles_to_sprites(0xFFC83E, 0xFFB804, time6, 6);
        return;
    }
    segment_digits(SEGMENT_TIME);
    tiles_to_sprites(DIGIT_BUFFER, 0xFFB804, seg5, 5);
    course = ROM_SEGMENT_RECORDS + 4 * rd16(COURSE);
    segment_digits(SEGMENT_RECORDS + rd16(course) + 0x78 + 2 * (uint16_t)(checkpoint - 2));
    tiles_to_sprites(DIGIT_BUFFER, 0xFFB822, seg5, 5);
    checkpoint_d4_set = 1;
}

/*
 * Passing a checkpoint ($E5BA): when the race code has counted the sign
 * (FF06B4 negative), record and show the segment time, add the extended
 * play time, restart the segment clock and start the "extended play"
 * object; past the course's last checkpoint the goal mode is selected.
 */
static void checkpoint_pass(void)
{
    uint16_t cp;
    uint8_t count, t;
    checkpoint_d4_set = 0;
    if (rd16(0xFFC7CA) || (int16_t)rd16(0xFF06B4) >= 0)
        return;
    cp = (uint16_t)(rd16(CHECKPOINT) + 1);
    segment_record(cp);
    count = rd16(HUD_HIDDEN) ? rd8(ROM_CHECKPOINTS_HIDDEN + rd16(0xFFC85A))
                             : rd8(ROM_CHECKPOINTS + rd16(COURSE));
    if ((uint8_t)cp >= count) {
        wr16(0xFFC720, 0x1C);
        return;
    }
    wr16(CHECKPOINT, cp);
    wr8(CHECKPOINT_BCD + 1, bcd_add(rd8(CHECKPOINT_BCD + 1), 1));
    checkpoint_times(cp);
    wr16(0xFFC7CA, 0xFFFF);
    wr16(0xFFCA80, 0x20);             /* object 6: extended play */
    {
        uint8_t bonus = rd8(ROM_EXTEND_TIME + 18 * rd16(0xFF053A) + (uint16_t)(rd16(CHECKPOINT) - 1));
        int x = 0;
        t = bcd_addx(rd8(CLOCK), bonus, &x);
        wr8(CLOCK, x ? 0x99 : t);
    }
    wr16(SEGMENT_TIME, 0);
    wr32(TOTAL_TIME, 0);
    wr8(CLOCK + 1, rd8(CLOCK + 2));
}

void checkpoint_pass_68k(void)
{
    checkpoint_pass();
    if (checkpoint_d4_set)
        cpu.d[4] = rd32(DIGIT_VALUE);
}

/* -------------------------------------------------------------- hills */

/*
 * The road's vertical profile ($F066). A hill comes from a script picked
 * by HILL_START (ROM_HILL_SCRIPTS: 2D vectors like the track geometry, 4
 * words per step). The state byte at +4 of the road scratch goes idle ->
 * start -> running; while running, the script is walked from the current
 * position exactly like the road builder walks the track, giving a height
 * per scanline at $FF1100 (96 lines, doubled to 192), and a horizon limit
 * for the line marker at +$24. A zero step ends the hill.
 */
#define HILL_START       0xFF061E   /* hill number to start (0 = none) */
#define ROM_HILL_SCRIPTS 0x00FAD2
#define ROAD_HEIGHTS     0xFF1100
static uint16_t hills_d1;
static int hills_ran;

static void hills_end(void)
{
    uint16_t hill = rd16(HILL_START);
    wr16(HILL_START, 0);
    if (hill == rd16(0xFF060E))
        wr16(0xFF0606, 0);
    wr8(ROAD_SCRATCH + 4, 0);
    hills_d1 = 0;
}

static void hills_run(void)
{
    const uint32_t sc = ROAD_SCRATCH;
    uint32_t script, p;
    int16_t vx, vy, dirx, diry, prev_h = 0, prev_line = 0, d0, d1, d4;
    uint16_t len;
    int32_t px = 0, py = 0;
    int seg, out = 0xBE;

    wr16(sc + 6, (uint16_t)(((int16_t)rd16(sc + 8) >> 1) * 4));
    script = rd32(sc + 0x0C);
    p = script + (int16_t)rd16(sc + 6);
    vx = (int16_t)(rd16(p) + rd16(p + 4));
    vy = (int16_t)(rd16(p + 2) + rd16(p + 6));
    wr16(sc + 0x38, (uint16_t)vx);
    wr16(sc + 0x48, (uint16_t)vy);
    len = isqrt_hex((uint32_t)((int32_t)vx * vx + (int32_t)vy * vy));
    dirx = (int16_t)divs_q((int32_t)vx << 14, len);
    diry = (int16_t)divs_q((int32_t)vy << 14, len);
    wr16(sc + 0x58, (uint16_t)dirx);
    wr16(sc + 0x68, (uint16_t)diry);
    wr32(sc + 0x78, 0);
    wr32(sc + 0x88, 0);
    wr16(sc + 0x98, 0);
    wr16(sc + 0xA8, 0);

    for (seg = 0x20; seg >= 0; seg--, p += 4) {
        int16_t x, y, h, line, span;
        int32_t fwd;
        int16_t a = (int16_t)rd16(p), b = (int16_t)rd16(p + 2);
        px += a + (int16_t)rd16(p + 4);
        py += b + (int16_t)rd16(p + 6);
        wr32(sc + 0x78, (uint32_t)px);
        wr32(sc + 0x88, (uint32_t)py);
        if (!(a | b)) {
            hills_end();
            return;
        }
        x = (int16_t)(px >> 5);
        y = (int16_t)(py >> 5);
        fwd = ((int32_t)x * dirx + (int32_t)y * diry) >> 7;
        h = (int16_t)divs_q((int32_t)x * diry - (int32_t)y * dirx, (uint16_t)((fwd >> 7) + 0x300));
        line = (int16_t)rd16(ROM_SEGMENT_LINES + 2 * seg);
        wr16(sc + 0xB8, (uint16_t)h);
        wr16(sc + 0xC8, (uint16_t)line);
        span = (int16_t)(line - prev_line);
        if (span < 0)
            break;
        if (span == 0)
            continue;
        {
            int16_t step = (int16_t)divs_q((int16_t)(h - prev_h), (uint16_t)span);
            int16_t v = prev_h, l = prev_line;
            do {
                v = (int16_t)(v + step);
                l++;
                wr16(ROAD_HEIGHTS + out, (uint16_t)v);
                out -= 2;
                if (out < 0)
                    goto done;
            } while (l != line);
        }
        prev_h = h;
        prev_line = line;
        wr16(sc + 0x98, (uint16_t)h);
        wr16(sc + 0xA8, (uint16_t)line);
    }
done:
    /* horizon: the slope of the step under the bike, limited by the road */
    p = script + (int16_t)rd16(sc + 6);
    d0 = (int16_t)(2 * (rd16(p) + rd16(p + 4)));
    d1 = (int16_t)(2 * (rd16(p + 2) + rd16(p + 6)));
    d4 = 0;
    if ((d0 < 0 ? -d0 : d0) > (d1 < 0 ? -d1 : d1)) {
        int16_t t = d0;
        d4 = d0 < 0 ? -0x70 : 0x70;
        d0 = d1;
        d1 = t;
    }
    d0 = (int16_t)divs_q((int32_t)d0 * 56, (uint16_t)d1);
    if (d4)
        d0 = (int16_t)(d4 - d0);
    d0 = (int16_t)-d0;
    d1 = (int16_t)(((int16_t)rd16(ROAD_Y_LINES) >> 4) - 0x5F);
    if (d0 > d1)
        d0 = d1;
    wr16(sc + 0x24, (uint16_t)d0);
    wr32(sc + 8, rd32(sc + 8) + rd32(0xFF0646));
    /* 96 lines -> 192 by inserting midpoints, from the bottom up */
    {
        uint32_t src = ROAD_HEIGHTS + 0xBE, dst = 0xFF1280;
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
            hills_d1 = (uint16_t)d;
            wr16(src + 2, (uint16_t)(b + d));
        }
    }
}

static void road_hills(void)
{
    const uint32_t sc = ROAD_SCRATCH;
    hills_ran = 0;
    if (rd16(0xFFC8A8))
        return;
    switch (rd8(sc + 4) & 3) {
    case 0:
        if (rd16(HILL_START))
            wr8(sc + 4, 1);
        break;
    case 1:
        wr32(sc + 0x0C, rd32(ROM_HILL_SCRIPTS + (uint16_t)((rd16(HILL_START) - 1) * 4)));
        wr32(sc + 8, 0);
        wr16(sc + 6, 0);
        wr8(sc + 4, 2);
        break;
    case 2:
        hills_ran = 1;
        hills_run();
        break;
    }
}

void road_hills_68k(void)
{
    road_hills();
    if (hills_ran) {
        set_dn(1, 2, hills_d1);
        cpu.d[4] &= 0xffff;
    }
}

/* ---------------------------------------------------------------- HUD */

/*
 * HUD numbers for the VBlank code to draw ($BBFC): speed in km/h (BCD at
 * $FF055A; its digits flash past 240 unless the bike can't go that fast),
 * then either original mode's time or the arcade stage number, course
 * name tiles, score and top score.
 */
#define SPEED_KMH        0xFF055A
static uint32_t hud_d4_in;
void hud_numbers(void)
{
    uint16_t base = 0xE0;
    wr16(SPEED_KMH, (uint16_t)bin_to_bcd((uint16_t)(rd16(BIKE_SPEED) * 0x117u / 0xF0)));
    if (!((rd16(0xFFC8B6) || rd16(HUD_HIDDEN)) && rd16(0xFFB61A) < 3) &&
        (int16_t)rd16(BIKE_SPEED) >= 0xF0) {
        base = 0x80;
        if (!rd8(0xFF0510) && !(rd8(FRAME_COUNTER_BYTE) & 8))
            base = 0xE0;
    }
    hex_tiles(0xFFC78C, rd16(SPEED_KMH), 4, base, 0);
    if (rd16(HUD_HIDDEN)) {
        time_tiles(0xFFC82F, 0xFFC83E);
        return;
    }
    copy_words(0xFFC778, 0x00C338 + 16 * rd16(COURSE), 8);
    hex_tiles(0xFFC788, rd8(CHECKPOINT_BCD + 1), 2, 0xE0, 0);
    hex_tiles(0xFFC758, rd32(0xFF0550), 8, 0x80, 0);
    hex_tiles(0xFFC768, rd32(0xFF054C), 8, 0xC0, 0);
}

void hud_numbers_68k(void)
{
    hud_d4_in = cpu.d[4];
    hud_numbers();
    if (rd16(HUD_HIDDEN)) {
        cpu.d[1] = 0xFFFF0000u | (uint16_t)((rd8(0xFFC831) & 0xF) + 0xA614);
        cpu.d[4] = (hud_d4_in & 0xFFFF0000u) | rd16(SPEED_KMH);
    } else {
        cpu.d[1] = 0xFFFFFFFF;
        cpu.d[4] = rd32(0xFF054C);
    }
    cpu.x = 0;
}

/* ------------------------------------------------------------- scroll */

/*
 * Per-line scroll tables for the HBlank code ($EB86), written into the
 * half of the double buffers not being shown (SCROLL_BUFFER_SEL ^ $700):
 *  - background: the heading, followed smoothly (16.16 at $FF0578), gives
 *    the far (BG_SCROLL) and near (3/4 of it) background scroll;
 *  - for every screen row the road covers (ROAD_ROW_LINES), the vertical
 *    scroll (+ the stripe offset when the row's road line is in a light
 *    stripe of the ROM pattern for the current phase) and the horizontal
 *    scroll from the road centre; rows above the road get the background.
 * The H-scroll longs keep the upper word of the smoothing step in their
 * first half, as the original's register does.
 */
#define BG_SCROLL        0xFF0570   /* word far, word near */
#define BG_HEADING       0xFF0578   /* 16.16 heading being followed */
#define SCROLL_BUFFER_SEL 0xFF0580
#define SCROLL_FLIP      0xFF06C4
#define VSCROLL_LINES    0xFF1500
#define HSCROLL_LINES    0xFF1880
#define ROM_STRIPES      0x00FE6E   /* 32 patterns of 96 bytes */
static uint16_t scroll_d1, scroll_d4, scroll_d5;
static void scroll_tables(void)
{
    uint32_t delta, vs, hs, half;
    uint16_t bg, d4 = 0, d5 = 0, blank_rows, count;
    uint32_t stripes, vrow;
    int row;
    int16_t top;

    delta = (uint32_t)((int32_t)(((uint32_t)(uint16_t)(rd16(ROAD_HEADING) << 5) << 16) - (rd32(BG_HEADING) << 6)) >> 7);
    wr32(BG_HEADING, rd32(BG_HEADING) + delta);
    bg = (uint16_t)((rd16(0xFFC74C) - (uint16_t)(rd16(BG_HEADING) - 8)) & 0x7FF);
    wr16(BG_SCROLL, bg);
    wr16(0xFFC74A, bg);
    wr16(BG_SCROLL + 2, (uint16_t)((int16_t)(bg * 3) >> 2));

    stripes = ROM_STRIPES + (rd16(0xFF0662) & 0x1F) * 96;
    half = rd16(SCROLL_BUFFER_SEL) ^ 0x700;
    hs = HSCROLL_LINES + half;
    vs = VSCROLL_LINES + half;
    vrow = 0;                          /* upper word: minus the row */
    for (row = 0; row < 0xE0; row++, d5++, vrow -= 0x10000, hs += 4, vs += 4) {
        uint16_t line = rd16(ROAD_ROW_LINES + 2 * row), v;
        if (!line) {
            d4++;
            continue;
        }
        line >>= 1;
        v = (uint16_t)(0x140 - d5 + line);
        if (rd8(stripes + (line >> 1)))
            v = (uint16_t)(v - 0xC0);
        vrow = (vrow & 0xFFFF0000u) | v;
        wr32(vs, vrow);
        wr32(hs, (delta & 0xFFFF0000u) |
                 (uint16_t)(((int16_t)rd16(ROAD_CENTRE_LINES + 2 * line) >> 6) - 100));
    }
    /* rows above the road: background */
    blank_rows = d4;
    d4--;
    {
        uint32_t c;
        for (c = 0, hs = HSCROLL_LINES + half; c <= d4; c++, hs += 4)
            wr32(hs, (uint32_t)rd16(BG_SCROLL) << 16 | rd16(BG_SCROLL + 2));
    }
    vs = VSCROLL_LINES + half;
    top = (int16_t)rd16(ROAD_ROWS_BLANK);
    if (top < 0) {
        uint16_t lead = (uint8_t)-top, v = 0;
        d4 = (uint16_t)(d4 - lead - 1);
        for (count = 0; count <= lead; count++, v--)
            wr32(vs + 4 * count, v);
        vs += 4u * (lead + 1);
        d5 = 0xFFFF;
        if ((int16_t)d4 >= 0) {
            for (count = 0; count <= d4; count++)
                wr32(vs + 4 * count, (uint32_t)rd16(BG_SCROLL) << 16 | (uint16_t)(top - 1));
            d4 = 0xFFFF;
        }
    } else {
        uint32_t c;
        for (c = 0; c <= d4; c++)
            wr32(vs + 4 * c, (uint32_t)rd16(BG_SCROLL) << 16 | (uint16_t)(top - 1));
        d4 = 0xFFFF;
    }
    /* first halves of the top rows: minus the row, then held */
    {
        uint16_t lines = (uint16_t)(0x80 - rd16(ROAD_ROWS_BLANK) - 0x28), v = 0;
        int below = lines < blank_rows;
        uint32_t c, a = VSCROLL_LINES + half;
        if (!below)
            lines = (uint16_t)(blank_rows - 1);
        for (c = 0; c <= lines; c++, a += 4, v--)
            wr16(a, v);
        if (below) {
            int16_t rest = (int16_t)(blank_rows - lines - 2);
            for (; rest >= 0; rest--, a += 4)
                wr16(a, v);
        }
        scroll_d1 = v;
    }
    wr32(VSCROLL_LINES + (rd16(SCROLL_BUFFER_SEL) ^ 0x700), 0);
    wr8(SCROLL_FLIP, 0xFF);
    scroll_d4 = d4;
    scroll_d5 = d5;
}

void scroll_tables_68k(void)
{
    scroll_tables();
    cpu.d[1] = scroll_d1;
    cpu.d[4] = scroll_d4;
    set_dn(5, 2, scroll_d5);
}

/* Move the bike marker along the HUD course map ($BB76): two tiles of
   the marker strip chosen by the progress at $FF0530. */
void hud_map_marker(void)
{
    uint16_t progress = rd16(0xFF0530);
    if (rd16(SOUND_LOCKED))
        return;
    if (rd16(HUD_HIDDEN)) {
        wr32(VDP_CTRL, 0x52060003 + ((uint32_t)(rd8(0xFF0530) & 0xF8) << 16));
        wr32(VDP_DATA, rd32(0x00C3B8 + ((progress & 0x780) >> 4)));
        wr32(VDP_DATA, rd32(0x00C3B8 + ((progress & 0x780) >> 4) + 4));
    } else {
        wr32(VDP_CTRL, 0x53040003 + ((uint32_t)(rd8(0xFF0530) & 0xFC) << 16));
        wr32(VDP_DATA, rd32(0x00C398 + ((progress & 0x380) >> 5)));
    }
}

void hud_map_marker_68k(void) { hud_map_marker(); }

/* A new crash (hit count changed): start the rider and bike crash
   objects ($8D26). */
static void crash_start(void)
{
    int k;
    if (rd16(0xFF0666) || rd16(0xFF0656) == rd16(0xFF0658))
        return;
    for (k = 0; k < 0x80; k += 4)
        wr32(0xFFC940 + k, 0);
    wr16(0xFFC940, 0x3C);
    wr16(0xFFC980, 0x40);
}

void crash_start_68k(void) { crash_start(); }

/* ---------------------------------------------------------- race start */

static void clear_words(const uint32_t *addr, int n)
{
    int k;
    for (k = 0; k < n; k++)
        wr16(addr[k], 0);
}

/*
 * Reset everything for a new race ($F448): counters and flags, the clock
 * (time from the difficulty's first entry, 30 frames a second), segment
 * records, checkpoints, the course's top score, the bike and course
 * state, the road tables (flat), and both scroll buffers.
 */
static void race_init(void)
{
    static const uint32_t flags[] = {
        0xFFC80A, 0xFFC80C, 0xFFC818, 0xFFC7CC, 0xFFC7DC, 0xFFC79C, 0xFFC7A4, 0xFFC7AC,
        0xFFC7B4, 0xFFC7BC, 0xFFC708, 0xFFC720, 0xFFC756, 0xFFC89E, 0xFFC7F8, 0xFF0658,
        0xFFC806, 0xFFC88E, 0xFFC894, 0xFFC896, 0xFFC898, 0xFFC89A, 0xFFC8A0, 0xFFC8A2,
        0xFFC8A4, 0xFFC8A6, 0xFFC8A8, 0xFFC8AA, 0xFFC8B4,
    };
    static const uint32_t state1[] = {
        0xFF068A, 0xFFC748,
    };
    static const uint32_t state2[] = {
        0xFF0622, 0xFF0614, 0xFF060A, 0xFF0612, 0xFF061E,
    };
    static const uint32_t state3[] = {
        0xFF0606, 0xFF061A, 0xFFC74A, 0xFFC74C, 0xFF0578, 0xFF0754,
    };
    static const uint32_t state4[] = {
        0xFF064E, 0xFF0652, 0xFF0656, 0xFF065A, 0xFF0666, 0xFF066A,
    };
    static const uint32_t state5[] = {
        0xFF0692, 0xFF06B4, 0xFF06B6, 0xFF06B8, 0xFF06BC, 0xFF06BE, 0xFF062E, 0xFF0630,
        0xFF06C0,
    };
    int k;

    wr32(0xFFC80E, 0x10000);
    wr16(0xFFC812, 0xB4);
    wr32(0xFFC7BE, 0xFFFF0000);
    wr32(0xFFC7C2, 0);
    wr8(0xFFC7C6, 0);
    wr8(0xFFC7C7, 3);
    wr32(0xFFC73E, 0);
    wr16(0xFFC742, 0);
    wr16(0xFFC7FE, 2);
    clear_words(flags, sizeof flags / sizeof flags[0]);
    for (k = 0; k < 12; k++)
        wr16(HILL_HEIGHTS + 2 * k, 0);
    for (k = 0; k < 0xF0; k++)
        wr16(0xFF094A + 2 * k, 0);
    wr8(0xFF06C5, 0);
    wr16(0xFF0610, 0xFFFF);
    wr8(0xFF05A0, 0x0F);
    /* the clock */
    wr8(CLOCK, rd8(ROM_EXTEND_TIME + 18 * rd16(0xFF053A)));
    wr8(CLOCK + 1, 0x1E);
    wr8(CLOCK + 2, 0x1E);
    wr8(0xFFC88C, 0x1E);
    wr16(SEGMENT_TIME, 0);
    wr32(RUN_TOTAL, 0);
    /* segment records ($E688) */
    for (k = 0; k < 0x3C; k++)
        wr16(SEGMENT_RECORDS + 2 * k, 0);
    wr16(SEGMENT_TIME, 0);
    wr8(0xFF0557, 0);
    wr32(0xFF054C, 0);
    wr16(CHECKPOINT, 1);
    wr16(CHECKPOINT_BCD, 1);
    wr32(0xFF0550, rd32(0xFF0200 + rd16(COURSE) * 0x70));
    /* bike and course */
    clear_words(state1, 2);
    wr32(BIKE_SPEED, 0);
    wr32(COURSE_PROGRESS, 0);
    wr32(0xFF0530, 0);
    clear_words(state2, 5);
    wr8(0xFF0734, 0);
    wr8(0xFF0744, 0);
    clear_words(state3, 6);
    if (!rd16(HUD_HIDDEN))
        wr32(RNG_SEED, 0);
    clear_words(state4, 6);
    wr8(0xFF0510, 0);
    clear_words(state5, 9);
    wr8(0xFF051A, 0);
    wr8(0xFF0519, 0);
    wr16(FADE_ACTIVE, 0);
    /* flat road */
    for (k = 0; k < 0x200; k += 4) {
        wr32(ROAD_CENTRE_LINES + k, 0);
        wr32(ROAD_HEIGHTS + k, 0);
        wr32(ROAD_BEND_LINES + k, 0);
    }
    for (k = 0; k < 0xC0; k++)
        wr16(ROAD_Y_LINES + 2 * k, (uint16_t)(0x5F8 - 8 * k));
    for (k = 0; k < 0x100; k += 4)
        wr32(ROAD_ROW_LINES + k, 0);
    for (k = 0; k < 0x60; k++)
        wr16(ROAD_ROW_LINES + 0x100 + 2 * k, (uint16_t)(1 + 4 * k));
    scroll_tables();
    wr16(SCROLL_BUFFER_SEL, rd16(SCROLL_BUFFER_SEL) ^ 0x700);
    scroll_tables();
}

void race_init_68k(void)
{
    race_init();
    set_dn(0, 2, 0xFFFF);
    cpu.d[1] = scroll_d1;
    cpu.d[4] = scroll_d4;
    set_dn(5, 2, scroll_d5);
}

/* ------------------------------------------------------- original mode */

/* All six parts as new ($7FD8). */
static void parts_reset(void)
{
    int k;
    for (k = 0; k < 6; k++)
        wr16(PART_WEAR + 2 * k, 0x0F);
}

void parts_reset_68k(void) { parts_reset(); }

/*
 * The rival's split times for this race ($8D7C): a base time per race
 * with random hundredths for each of six segments (8-byte records at
 * $FFC85C), and their running total at $FFC83B.
 */
#define RIVAL_SPLITS     0xFFC85C
static void rival_splits(void)
{
    uint32_t base = rd32(0x008D58 + 4 * rd16(0xFFC85A)), rec = RIVAL_SPLITS;
    int k, n;
    wr32(0xFFC83A, 0);
    for (k = 0; k < 6; k++, rec += 8) {
        uint16_t tens;
        wr32(rec, base);
        tens = (uint16_t)(((rng_next() & 3) + 3) << 4);
        wr8(rec + 3, (uint8_t)(rd8(0x008D74 + (rng_next() & 7)) | tens));
        wr32(rec + 4, 0);
    }
    n = rd16(0x00C388 + 2 * rd16(0xFFC85A));
    for (k = 0; k <= n; k++)
        time_add(0xFFC83B, 0xFFC85D + 8 * k);
}

void rival_splits_68k(void) { rival_splits(); }

/*
 * A course's first stage ($F6DC): number of stages, the three course
 * scripts and the scenery colours.
 */
static void stage_start(void)
{
    uint32_t rec, pal;
    int k;
    wr16(0xFF06C2, (uint16_t)(int8_t)rd8(ROM_STAGES + rd16(COURSE)));
    load_turn_script();
    load_event_script();
    load_scenery_script();
    rec = stage_record(rd16(STAGE));
    pal = rd32(rec);
    wr16(0xFFD3E0, rd16(pal));
    copy_words(0xFFD396, pal + 2, 5);
    copy_words(0xFFD3E8, pal + 12, 7);
    pal = rd32(rec + 4);
    for (k = 0; k < 5; k++) {
        wr16(0xFFD3D6 + 2 * k, rd16(pal + 4 * k));
        wr16(0xFFD3F6 + 2 * k, rd16(pal + 4 * k + 2));
    }
    wr8(PALETTE_DIRTY, rd8(PALETTE_DIRTY) | 1);
}

void stage_start_68k(void) { stage_start(); }

/* ----------------------------------------------------------- race start */

/*
 * One race start frame after the wait ($86D2): the HUD curtain, sound,
 * input, rivals and objects run; once the curtain is open the start
 * lights object runs, and when it has counted down the music starts and
 * the race begins (mode $18).
 */
static void race_start_frame(void)
{
    hud_curtain();
    race_sounds();
    race_input();
    stage_geometry();
    race_turns();
    race_rivals();
    objects_update();
    hud_numbers();
    if (rd8(0xFFC755))
        return;
    if (!rd16(0xFFCBC0))
        wr16(0xFFCBC0, 0x30);
    if (rd8(0xFFCBC0 + 0x2D) != 3)
        return;
    if (!rd16(OBJECTS))
        wr16(OBJECTS, 0x1C);
    sound_command(rd8(0xFF0518));
    wr16(0xFFC704, 0x18);
}

void race_start_frame_68k(void) { race_start_frame(); }

/* ------------------------------------------------------------ race frame */

/*
 * The second half of a race frame, after the race logic and the wait
 * ($8B58): objects and scroll tables, then the engine sound for the
 * driver: the nearest rival's distance (capped at $80) as the engine
 * volume, a horn sound ($A9) when one is close and nothing else is
 * playing, and any pending sound command.
 */
static void race_frame_end(void)
{
    uint16_t near;
    int close, k;
    uint8_t cmd;
    objects_update();
    scroll_tables();
    if (rd16(0xFFC826))
        wr16(0xFFC826, rd16(0xFFC826) - 1);
    near = rd16(0xFFC81C);
    for (k = 0; k < 4; k++)
        if (rd16(0xFFC81E + 2 * k) <= near)
            near = rd16(0xFFC81E + 2 * k);
    close = near < 0x80;
    if (!close) {
        near = 0x80;
        wr16(0xFFC828, 0);
    }
    wr8(0xFFC819, (uint8_t)near);
    if (close && !(rd16(0xFFC826) | rd16(0xFF0666) | rd16(0xFF06BC) | rd16(0xFFC828))) {
        wr16(0xFFC828, (near & 0xFF00) | 0xA9);
        wr8(0xFFC80D, 0xA9);
    }
    do
        wr16(Z80_BUSREQ, 0x100);
    while (rd8(Z80_BUSREQ) & 1);
    wr8(0xA01C38, rd8(0xFFC819));
    if ((cmd = rd8(0xFFC80C)) != 0)
        wr8(0xFFC80C, 0);
    else if ((cmd = rd8(0xFFC80D)) != 0)
        wr8(0xFFC80D, 0);
    if (cmd)
        wr8(Z80_SOUND_COMMAND, cmd);
    wr16(Z80_BUSREQ, 0);
}

void race_frame_end_68k(void) { race_frame_end(); }

/*
 * The second half of a demo race frame ($89B6): objects and scroll
 * tables; when the demo's time runs out the ranking follows, C/Start
 * fades back to the logo.
 */
static void demo_frame_end(void)
{
    objects_update();
    scroll_tables();
    if (!rd8(CLOCK)) {
        wr16(GAME_MODE, 0x20);
        return;
    }
    if (rd8(0xFFC707) & 0xA0) {
        call68k(f_006DD4);
        wr16(0xFFC722, 0);
        wr16(GAME_MODE, 0);
    }
}

void demo_frame_end_68k(void) { demo_frame_end(); }

/* Wait for the frame's fourth tick (the VBlank handler counts them). */
static void wait_ticks_4(uint32_t pc)
{
    while ((int16_t)rd16(0xFFC724) < 4)
        m68k_idle(pc);
}

/*
 * A race frame after the start of frame wait ($8ACC): a mode change
 * requested during the frame takes effect (fading out first when leaving
 * to the command screen); otherwise, unless paused, the race logic runs
 * (or the race stops when the time is up), then the second half after
 * the fourth tick.
 */
static void race_frame(void)
{
    uint16_t next = rd16(0xFFC720);
    if (next) {
        wr16(GAME_MODE, next);
        if (next == 0x24) {
            call68k(f_006DD4);
            wr16(0xFFC722, 0);
        }
        return;
    }
    if (!rd16(0xFF08D4)) {
        crash_start();
        if (!rd8(CLOCK)) {
            wr16(OBJECTS_FROZEN, 0xFFFF);
            wr16(OBJECTS, 0x38);             /* time up */
        } else {
            hill_columns();
            parts_check();
            flash_colours();
            race_clock();
            checkpoint_pass();
            race_stage();
            stage_geometry();
            race_rivals();
            race_place_objects();
            race_turns();
            race_sounds();
            race_input();
            race_speed();
            race_drift();
            race_build_road();
            road_hills();
            road_rows();
            hud_numbers();
        }
    }
    wait_ticks_4(0x008B50);
    race_frame_end();
}

void race_frame_68k(void) { race_frame(); }

/* The demo race: the same logic without input, checkpoints or the HUD
   ($8976). */
static void demo_frame(void)
{
    hill_columns();
    flash_colours();
    race_clock();
    race_sounds();
    race_speed();
    race_drift();
    stage_geometry();
    race_build_road();
    road_hills();
    road_rows();
    race_turns();
    race_place_objects();
    race_rivals();
    crash_start();
    wait_ticks_4(0x0089AE);
    demo_frame_end();
}

void demo_frame_68k(void) { demo_frame(); }

/* Best segment times and course totals as "none yet" ($E6A4). */
void records_init(void)
{
    int k;
    for (k = 0; k < 0x3A; k++)
        wr16(0xFF0478 + 2 * k, 0x9999);
    for (k = 0; k < 4; k++)
        wr32(BEST_TOTALS + 4 * k, 0x595999);
}

void records_init_68k(void) { records_init(); set_dn(1, 2, 0xFFFF); }

/* ------------------------------------------------------------ setup */

/*
 * The race's setup ($872E, with interrupts masked): road and HUD
 * graphics, the sky colour for the course, the first stage, the bike
 * and the rivals placed. Original mode adds the rival bikes' graphics,
 * its own course table entry and object list, and the frame-broken
 * warning when a part is worn out.
 */
static void race_setup(void)
{
    int k, n;
    uint32_t list;
    screen_reset();
    palette_load(0x008E56);
    nemesis_to_ram(0x0412E0, 0xFF3000);
    nemesis_to_ram(0x0421E4, 0xFF6400);
    wr32(VDP_CTRL, 0x4A800003);
    nemesis_to_vdp(0x01DC80);
    vdp_copy(0x4F800003, 0x01DD22, 0x41);
    wr32(VDP_CTRL, 0x5F600001);
    nemesis_to_vdp(0x040B12);
    plane_a_fill(0xE6FE);
    if (rd16(HUD_HIDDEN))
        wr16(0xFFD3E6, rd16(0x008EC6 + 2 * rd16(0xFFC85A)));
    else
        wr16(0xFFD3E6, rd16(0x008EBE + 2 * rd16(0xFF060C)));
    vdp_fill(0x5FC00003, 0x3333, 16);
    wr8(0xFFC755, 0xFF);
    wr32(0xFFC71A, 0x0F);
    race_init();
    if (rd16(HUD_HIDDEN)) {
        uint32_t rec = 0x008E2C + 6 * rd16(0xFFC85A);
        wr32(VDP_CTRL, 0x7D800002);
        nemesis_to_vdp(0x04D20E);
        wr32(0xFFC82E, 0);
        wr32(0xFFC832, 0);
        wr16(0xFF060C, rd16(rec));
        wr16(0xFF060A, rd16(rec + 2));
        wr16(0xFFC806, rd16(rec + 2));
        wr16(0xFFC7DC, rd16(rec + 4));
        rival_splits();
        list = 0x00954A;
        n = 0x4F;
    } else {
        list = 0x0094C4;
        n = 0x43;
    }
    for (k = 0; k < n; k++)
        wr16(0xFFB800 + 2 * k, rd16(list + 2 * k));
    stage_start();
    stage_graphics_load();
    stage_geometry();
    wr32(0xFFC84E, rd32(0xFF0672));
    wr32(0xFFC852, rd32(0xFF0602));
    wr32(0xFFC856, rd32(0xFF066E));
    wr16(0xFF060A, 0);
    bike_graphics();
    race_objects_setup();
    start_objects();
    objects_update();
    dma_slots_run();
    wr16(0xFF06B4, 1);
    wr16(0xFFC7CA, 0);
    sound_command(0xB1);
    if (rd16(HUD_HIDDEN)) {
        if ((int16_t)rd16(0xFFB648) < 0 || (int16_t)rd16(0xFFB64A) < 0 || (int16_t)rd16(0xFFB652) < 0) {
            wr16(0xFFC900, 0x50);
        } else if ((int16_t)rd16(0xFFB650) < 0) {
            wr16(0xFFB64A, 0xFFFF);
            wr16(0xFFC900, 0x50);
        }
    }
}

void race_setup_68k(void) { race_setup(); }

/* Interrupts on, then the race VBlank handler from a clean phase ($88CA,
   $8AA6). */
static void race_setup_done(void)
{
    set_sr(get_sr() & 0xF8FF);
    wait_vblank();
    wr16(0xFFC724, 0);
    wr16(0xFFC722, 8);
    wr16(VDP_CTRL, 0x8A00);
    wait_frame_ticks();
}

/* Game mode $10 ($6D02): the setup and the start music, or one frame of
   the start sequence. */
void mode_race_start_68k(void)
{
    if (mode_first_call()) {
        race_setup();
        race_setup_done();
        sound_command(0x93);
        display_on();
        return;
    }
    wait_frame_ticks();
    race_start_frame();
}

/*
 * The demo race's setup ($89EE, with interrupts masked): sound locked,
 * the demo course and stage from the rotation at $C750/$C752 (each
 * course shows its stages in turn, the counts in the table at $FA66),
 * which then moves on for the next demo.
 */
static void demo_setup(void)
{
    uint16_t course, stage;
    screen_reset();
    palette_load(0x008E56);
    wr32(VDP_CTRL, 0x4A800003);
    nemesis_to_vdp(0x01DF1C);
    vdp_copy(0x4F800003, 0x01DFBC, 0x41);
    wr8(SOUND_LOCKED, 0xFF);
    wr16(0xFF060C, rd16(0xFFC750));
    race_init();
    wr16(0xFFC812, 0x1E);
    wr16(0xFF060A, rd16(0xFFC752));
    wr16(0xFFC806, rd16(0xFFC752));
    stage_start();
    wr8(0xFF0554, 0x20);
    stage_graphics_load();
    hud_draw_all();
    course_graphics();
    race_objects_setup();
    wr16(0xFFC900, 0x18);
    stage = (uint16_t)(rd16(0xFFC752) + 1);
    course = rd16(0xFFC750);
    if ((uint8_t)stage == rd8(0x00FA66 + course)) {
        wr16(0xFFC750, (course + 1) & 3);
        stage = 0;
    }
    wr16(0xFFC752, stage);
    hud_numbers();
    wr16(0xFF064A, 0x50);
    wr16(VDP_CTRL, 0x9207);
}

void demo_setup_68k(void) { demo_setup(); }

/* Game mode $14 ($6D06): the demo's setup, or one frame. */
void mode_demo_68k(void)
{
    if (mode_first_call()) {
        demo_setup();
        race_setup_done();
        display_on();
        return;
    }
    wait_frame_ticks();
    demo_frame();
}

/* Game mode $18 ($6D0A): one race frame. */
void mode_race_68k(void)
{
    wait_frame_ticks();
    race_frame();
}

/* ------------------------------------------------------------- goal */

/* Game mode $1C's setup ($B95C): original mode waits 30 frames after
   the goal; arcade mode moves the window plane. */
static void goal_setup(void)
{
    if (rd16(HUD_HIDDEN))
        wr16(0xFFC70E, 0x1E);
    else
        wr16(VDP_CTRL, 0x921C);
}

void goal_setup_68k(void) { goal_setup(); }

/* One bonus point: 1000 on the score ($B974). */
static void goal_bonus_tick(void)
{
    wr32(0xFF0548, 0x100000);
    score_add();
}

void goal_bonus_tick_68k(void) { goal_bonus_tick(); }

/*
 * The arcade goal bonus ($B984): three texts, then the bonus from the
 * time left (BCD seconds x 100, plus 10 minus the tenths' digit) shown
 * in big digits twice, and converted to binary to count down.
 */
#define GOAL_BONUS       0xFF04E8
static void goal_bonus_show(void)
{
    uint32_t t = 0x00BA3C;
    uint16_t v;
    uint8_t tenths, b;
    int k;
    for (k = 0; k < 3; k++, t += 10)
        vdp_copy(rd32(t), rd32(t + 6), rd16(t + 4) + 1);
    v = (uint16_t)(rd8(CLOCK) << 8);
    tenths = rd8(0xFF04EF) & 0xF0;
    if (tenths)
        v |= (uint16_t)(10 - (tenths >> 4));
    wr16(GOAL_BONUS, v);
    hud_big_digits(0x550E0003, GOAL_BONUS);
    hud_big_digits(0x55120003, GOAL_BONUS + 1);
    wr32(VDP_CTRL, 0x55920003);
    wr16(VDP_DATA, 0xA638);
    hud_big_digits(0x553A0003, GOAL_BONUS + 1);
    hud_big_digits(0x55380003, GOAL_BONUS);
    b = rd8(GOAL_BONUS);
    wr16(GOAL_BONUS, (uint16_t)((b & 0xF) * 10 + (b >> 4) * 100 + (rd8(GOAL_BONUS + 1) & 0xF)));
}

void goal_bonus_show_68k(void) { goal_bonus_show(); }

/*
 * A frame past the goal ($B874): the race goes on without the clock or
 * checkpoints. Original mode, once the goal sequence is over and a delay
 * has passed, takes the times to the command screen (the parts wear);
 * arcade mode counts the bonus onto the score, two points a frame, and
 * when the ceremony ends goes on to the ranking.
 */
static void goal_frame(void)
{
    if (rd16(HUD_HIDDEN)) {
        if (rd16(0xFF06C0)) {
            wr16(0xFFC70E, rd16(0xFFC70E) - 1);
            if (!rd16(0xFFC70E)) {
                wr32(0xFFB63C, rd32(0xFFC832));
                wr32(0xFFB640, rd32(0xFFC83A));
                wr8(0xFFB655, rd8(0xFFB655) | 2);
                fade_out();
                wr16(0xFFC722, 0);
                wr16(GAME_MODE, 0x24);
                return;
            }
        }
    } else if (rd16(0xFF06C0)) {
        int k;
        if (!rd8(0xFF0519)) {
            goal_bonus_show();
            wr8(0xFF0519, 0xFF);
        }
        for (k = 0; k < 2 && (int16_t)rd16(GOAL_BONUS) > 0; k++) {
            wr16(GOAL_BONUS, rd16(GOAL_BONUS) - 1);
            goal_bonus_tick();
        }
        if (rd8(0xFF051A)) {
            wr8(0xFF051A, 0);
            wr8(0xFF0519, 0);
            wr16(0xFF06C0, 0);
            wr16(GAME_MODE, 0x34);
            wr16(OBJECTS_FROZEN, 0xFFFF);
        }
    }
    race_stage();
    stage_geometry();
    race_rivals();
    race_place_objects();
    race_turns();
    race_sounds();
    race_input();
    race_speed();
    race_drift();
    race_build_road();
    road_hills();
    road_rows();
    hill_columns();
    hud_numbers();
    wait_ticks_4(0x00B94C);
    objects_update();
    scroll_tables();
}

void goal_frame_68k(void) { goal_frame(); }

/* Game mode $1C ($6D0E): the setup, or one frame past the goal. */
void mode_goal_68k(void)
{
    if (mode_first_call()) {
        goal_setup();
        return;
    }
    wait_frame_ticks();
    goal_frame();
}
