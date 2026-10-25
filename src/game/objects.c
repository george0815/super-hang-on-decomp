/* Game objects. */

#include "game.h"

#define OBJ_LAYER       0x0C    /* word: draw layer of the object (even) */
#define LAYER_LIST_SIZE 0x20    /* count word + up to 15 object pointers */
#define LAYER_LIST_MAX  0x1E    /* count is kept in bytes */

/*
 * Queue an object on its layer's list ($709A). Lists sit below
 * OBJ_LAYER_LISTS, one per layer: a count (in bytes) followed by the low
 * words of the object addresses. Full lists drop the object.
 */
void obj_add_to_layer(uint32_t obj)
{
    uint32_t list = OBJ_LAYER_LISTS - (uint32_t)(rd16(obj + OBJ_LAYER) & 0xfffe) * 16;
    uint16_t count = rd16(list);
    if ((int16_t)count >= LAYER_LIST_MAX)
        return;
    wr16(list, count + 2);
    wr16(list + 2 + count, obj);
}

void obj_add_to_layer_68k(void)
{
    uint32_t obj = cpu.a[0];
    uint16_t layer = rd16(obj + OBJ_LAYER) & 0xfffe;
    uint32_t list = 0xFFFFE000u - (uint32_t)(uint16_t)(layer << 4);
    uint16_t count = rd16(list);
    obj_add_to_layer(obj);
    set_dn(0, 2, (uint16_t)(layer << 4));
    set_dn(1, 2, count);
    cpu.a[1] = list;
    f_cmp(2, LAYER_LIST_MAX, count);
}

/* ------------------------------------------------------- object table */

#define OBJECTS         0xFFC900
#define OBJ_SIZE        0x40
#define OBJ_COUNT       29

/* object record fields */
#define OBJ_HANDLER     0x00    /* word: handler index x4 (0 = free) */
#define OBJ_FLAGS       0x01    /* byte, bit 0: initialised */
#define OBJ_TILE_BASE   0x02    /* word added to every sprite piece's tile */
#define OBJ_X           0x04    /* long, 16.16: lateral position */
#define OBJ_Y           0x08    /* long, 16.16 */
#define OBJ_Z           0x0C    /* long; its high word doubles as draw layer */
#define OBJ_SCREEN_X    0x10
#define OBJ_SCREEN_Y    0x14
#define OBJ_VX          0x18    /* long, 16.16 velocities */
#define OBJ_VY          0x1C
#define OBJ_VZ          0x20
#define OBJ_SPRITE_MAP  0x24    /* word: address of the sprite map (0 = hidden) */
#define OBJ_DISTANCE    0x26    /* word: distance ahead, x16 */
#define OBJ_ROAD_LINE   0x28    /* word: scanline of that distance */
#define OBJ_LANE        0x2A    /* word: lateral offset on the road */

#define HUD_HIDDEN      0xFFC82C
#define SOUND_LOCKED_BYTE 0xFFC754
#define ROAD_Y_LINES    0xFF0F00  /* per scanline: road height x16 */
#define ROAD_X_LINES    0xFF0D00  /* per scanline: road centre x64 */
#define BIKE_SPEED      0xFF064A
#define FRAME_COUNTER   0xFFC726
#define OBJECTS_FROZEN  0xFFC756

#define ROM_DISTANCE_TO_LINE 0x00CBB4
#define ROM_APPROACH_SPEED   0x00C8F8

#define SPRITE_TABLE    0xFFD100
#define LAYER_LISTS     0xFFD400  /* 97 lists of LAYER_LIST_SIZE bytes */
#define MAX_SPRITES     80

/* X += VX; screen x = X + $120 ($70BA) */
static void obj_move_x(uint32_t obj)
{
    wr32(obj + OBJ_X, rd32(obj + OBJ_X) + rd32(obj + OBJ_VX));
    wr16(obj + OBJ_SCREEN_X, rd16(obj + OBJ_X) + 0x120);
}

/* Clear the whole record ($70F0). */
static void obj_free(uint32_t obj)
{
    int i;
    for (i = 0; i < OBJ_SIZE; i += 4)
        wr32(obj + i, 0);
}

/* Screen y of a road scanline: the road table holds heights x16. */
static uint16_t road_screen_y(uint16_t line, uint16_t base)
{
    int16_t h = (int16_t)rd16(ROAD_Y_LINES + 2 * line) >> 4;
    return (uint16_t)(base - (h - 0x60));
}

/* Come closer at a rate that depends on the bike's speed ($C816). */
static void obj_approach(uint32_t obj)
{
    uint16_t step = rd16(ROM_APPROACH_SPEED + 2 * rd16(BIKE_SPEED));
    wr16(obj + OBJ_DISTANCE, rd16(obj + OBJ_DISTANCE) + 2 * step);
}

/* Distance -> scanline, layer and screen y ($C830). */
static void obj_project_distance(uint32_t obj)
{
    uint16_t line = rd16(ROM_DISTANCE_TO_LINE + 2 * (uint16_t)((int16_t)rd16(obj + OBJ_DISTANCE) >> 4)) >> 4;
    wr16(obj + OBJ_ROAD_LINE, line);
    wr16(obj + OBJ_Z, line);
    wr16(obj + OBJ_SCREEN_Y, road_screen_y(line, 0x100));
}

/* Lane offset scaled by the scanline's perspective, rounded away from zero
   by 4 ($C86A divides by $BF, $C89C by $E0). */
static int16_t lane_offset(uint32_t obj, int16_t divisor)
{
    int32_t scaled = (int32_t)(int16_t)rd16(obj + OBJ_LANE) * (int16_t)rd16(obj + OBJ_ROAD_LINE);
    int16_t offset = (int16_t)(scaled / divisor);
    return (int16_t)(offset + (offset >= 0 ? 4 : -4));
}

static int16_t road_centre(uint32_t obj)
{
    return (int16_t)rd16(ROAD_X_LINES + 2 * rd16(obj + OBJ_ROAD_LINE)) >> 6;
}

/* Road centre plus lane offset -> screen x ($C86A). */
static void obj_project_lane(uint32_t obj)
{
    wr16(obj + OBJ_SCREEN_X, (uint16_t)(road_centre(obj) + lane_offset(obj, 0xBF) + 0x120));
}

/* Y += VY; screen y = Y + $F0 ($70D0) */
static void obj_move_y(uint32_t obj)
{
    wr32(obj + OBJ_Y, rd32(obj + OBJ_Y) + rd32(obj + OBJ_VY));
    wr16(obj + OBJ_SCREEN_Y, rd16(obj + OBJ_Y) + 0xF0);
}

/* Z += VZ ($70E6) */
static void obj_move_z(uint32_t obj)
{
    wr32(obj + OBJ_Z, rd32(obj + OBJ_Z) + rd32(obj + OBJ_VZ));
}

/*
 * Pick the sprite map for the object's depth ($7060, $704C with another
 * depth table): the sprite set (byte $2D) selects a 20-byte row of map
 * addresses in ROM_SPRITE_SETS; a per-depth table gives the offset in it.
 */
#define OBJ_SPRITE_SET   0x2D
#define ROM_SPRITE_SETS  0x000204
static int obj_pick_sprite(uint32_t obj, uint32_t depth_table)
{
    int16_t depth = (int16_t)rd16(obj + OBJ_Z);
    uint32_t row;
    int8_t offset;
    if (depth == 0 || depth > 0xC0)
        return -1;
    row = ROM_SPRITE_SETS + rd8(obj + OBJ_SPRITE_SET) * 20u;
    offset = (int8_t)rd8(depth_table + depth);
    wr16(obj + OBJ_SPRITE_MAP, rd16(row + offset));
    return offset;
}

#define ROM_DEPTH_SPRITES_NEAR 0x006EC8
#define ROM_DEPTH_SPRITES_FAR  0x006F8A

/* ----------------------------------------------------------- handlers */

/*
 * Thrown/falling piece ($710A): moves on all three axes with gravity on X,
 * stops at depth $B8, and at depth $1C signals the race code.
 */
static void obj_debris(uint32_t obj)
{
    uint16_t depth;
    obj_pick_sprite(obj, ROM_DEPTH_SPRITES_NEAR);
    obj_add_to_layer(obj);
    depth = rd16(obj + OBJ_Z);
    if (depth == 0x1C) {
        obj_move_z(obj);
        wr16(0xFFC712, 0);
        wr16(0xFFC714, 0x18);
        wr8(0xFFC72C, rd8(0xFFC72C) + 4);
        return;
    }
    if (depth == 0xB8) {
        obj_move_z(obj);
        wr32(obj + OBJ_VX, 0);
        wr32(obj + OBJ_VY, 0);
        wr32(obj + OBJ_VZ, 0);
        return;
    }
    obj_move_x(obj);
    obj_move_y(obj);
    obj_move_z(obj);
    if (rd16(obj + OBJ_Z) != 0xBC)
        wr32(obj + OBJ_VX, rd32(obj + OBJ_VX) + 0xFFFF5000u);
}

#define PLAYER          0xFFC940  /* object 1: the player's bike */
#define OBJ_ANIM        0x3E

/* No overlays while any of these race states is set. */
static int overlays_suppressed(void)
{
    return (rd16(0xFFC7FE) | rd16(0xFF0666) | rd16(0xFF06BC)) != 0;
}

/*
 * Overlay that follows the bike's frame ($7112): picks a sprite and the
 * tile source for its DMA slot from the bike's lean frame.
 */
static void obj_bike_overlay(uint32_t obj)
{
    uint16_t frame;
    if (rd16(0xFFC794) == 0x10 || overlays_suppressed())
        return;
    if ((int16_t)rd16(0xFF0642) < (int16_t)rd16(0x009FFA))
        return;
    if (rd16(HUD_HIDDEN)) {
        int16_t c = (int16_t)rd16(0xFFB64C);
        if (c < 0)
            return;
        if (c == 0)
            wr16(0xFFB64C, 0xFFFF);
    }
    wr16(obj + OBJ_SCREEN_X, rd16(PLAYER + OBJ_SCREEN_X));
    wr16(obj + OBJ_SCREEN_Y, rd16(PLAYER + OBJ_SCREEN_Y));
    frame = (uint16_t)(rd8(PLAYER + OBJ_SPRITE_SET) << 3);
    if (frame != rd16(obj + OBJ_ANIM)) {
        wr16(obj + OBJ_ANIM, frame);
        wr32(0xFFC7A6, rd32(0x00A6DC + frame));
        wr32(0xFFC7AA, rd32(0x00A6E0 + frame));
    }
    wr16(obj + OBJ_SPRITE_MAP, rd16(0x00A6E2 + frame));
    obj_add_to_layer(obj);
}

/*
 * Second bike overlay ($7116), alternating between two frames every 8
 * updates; also drives a countdown used when the HUD is hidden.
 */
static void obj_bike_overlay_2(uint32_t obj)
{
    uint16_t i;
    if (!rd8(0xFF0510)) {
        wr16(obj + 0x36, 0x0B);
        return;
    }
    if (overlays_suppressed() || rd16(0xFFC97A))
        return;
    if (!rd16(OBJECTS_FROZEN)) {
        if (rd16(HUD_HIDDEN) && !rd16(0xFFB64E)) {
            int16_t t = (int16_t)(rd16(obj + 0x36) - 1);
            wr16(obj + 0x36, (uint16_t)t);
            if (t < 0) {
                wr16(0xFFB64E, rd16(0xFFB64E) - 1);
                wr16(0xFFC8A2, 4);
                wr16(0xFFC8A4, 0x31);
                wr16(0xFFC8A0, 1);
            }
        }
        wr16(obj + OBJ_ANIM, rd16(obj + OBJ_ANIM) + 8);
    }
    i = (uint16_t)((rd16(obj + OBJ_ANIM) & 8) + (rd8(PLAYER + OBJ_SPRITE_SET) << 4));
    wr16(obj + OBJ_SPRITE_MAP, rd16(0x00A5F6 + i));
    wr32(0xFFC7AE, rd32(0x00A5F0 + i));
    wr32(0xFFC7B2, rd32(0x00A5F4 + i));
    wr16(obj + OBJ_SCREEN_X, rd16(PLAYER + OBJ_SCREEN_X));
    wr16(obj + OBJ_SCREEN_Y, rd16(PLAYER + OBJ_SCREEN_Y));
    obj_add_to_layer(obj);
}

/* Snaps its velocity to zero on four fixed columns ($7152). */
static void obj_column_stopper(uint32_t obj)
{
    uint16_t x;
    obj_add_to_layer(obj);
    obj_move_x(obj);
    x = rd16(obj + OBJ_SCREEN_X);
    if (x == 0x100 || x == 0x120 || x == 0xA8 || x == 0xC8)
        wr16(obj + OBJ_VX, 0);
}

/* Marker riding on a scanline chosen by the race code ($714A). */
static void obj_line_marker(uint32_t obj)
{
    uint16_t line = rd16(0xFF0758);
    if ((int16_t)line <= 1 || (int16_t)line > 0xBF)
        return;
    wr16(obj + OBJ_Z, line);
    wr16(obj + OBJ_SCREEN_Y, road_screen_y(line, 0x11E));
    wr16(obj + OBJ_SCREEN_X, 0x5F);
    wr16(obj + OBJ_SPRITE_MAP, 0x3CCA);
    obj_add_to_layer(obj);
}

/* Fixed sprite blinking every 16 frames ($711E). */
static void obj_blinker(uint32_t obj)
{
    uint8_t f = rd8(obj + OBJ_FLAGS);
    wr8(obj + OBJ_FLAGS, f | 1);
    if (!(f & 1)) {
        wr16(obj + OBJ_Z, 0xC0);
        wr16(obj + OBJ_SCREEN_X, 0xD8);
        wr16(obj + OBJ_SCREEN_Y, 0xF8);
        wr16(obj + OBJ_SPRITE_MAP, 0x3A1E);
    }
    if (rd8(FRAME_COUNTER) & 0x10)
        obj_add_to_layer(obj);
}

/* Come closer; 1 if the object has passed the camera. */
static int obj_passed(uint32_t obj)
{
    obj_approach(obj);
    return ((int16_t)rd16(obj + OBJ_DISTANCE) >> 4) >= 0x1FF;
}

/* Move closer and project; frees the object once passed ($C6EC). */
static int obj_roadside_step(uint32_t obj)
{
    if (obj_passed(obj)) {
        obj_free(obj);
        return 0;
    }
    obj_project_distance(obj);
    obj_project_lane(obj);
    return 1;
}

static void obj_roadside_draw(uint32_t obj)
{
    if ((int16_t)rd16(obj + OBJ_SCREEN_X) >= 0x70)
        obj_add_to_layer(obj);
}

/* Roadside object coming towards the camera; removed once passed ($7132). */
static void obj_approacher(uint32_t obj)
{
    uint8_t f = rd8(obj + OBJ_FLAGS);
    wr8(obj + OBJ_FLAGS, f | 1);
    if (!(f & 1)) {
        wr16(obj + OBJ_SPRITE_MAP, 0x39A4);
        wr16(obj + OBJ_DISTANCE, 0x1E90);
        wr16(obj + OBJ_LANE, 0xFEFE);
    }
    if (!rd16(OBJECTS_FROZEN) && !obj_roadside_step(obj))
        return;
    obj_roadside_draw(obj);
}

/* Starting lights: red, amber, green every 30 frames, with beeps ($7136). */
#define OBJ_TIMER   0x2E
static void obj_start_lights(uint32_t obj)
{
    uint8_t f = rd8(obj + OBJ_FLAGS);
    wr8(obj + OBJ_FLAGS, f | 1);
    if (!(f & 1)) {
        wr16(obj + OBJ_SPRITE_MAP, 0x39F6);
        wr16(obj + OBJ_DISTANCE, 0x1E90);
        wr16(obj + OBJ_LANE, 0xFEFE);
        wr8(obj + OBJ_TIMER, 0x1E);
        sound_command(0x88);
    }
    if (!rd16(OBJECTS_FROZEN)) {
        uint8_t t = (uint8_t)(rd8(obj + OBJ_TIMER) - 1);
        wr8(obj + OBJ_TIMER, t);
        if (t == 0) {
            uint8_t light = (uint8_t)(rd8(obj + OBJ_SPRITE_SET) + 1);
            wr8(obj + OBJ_TIMER, 0x1E);
            wr8(obj + OBJ_SPRITE_SET, light);
            if (light == 1) {
                wr16(obj + OBJ_SPRITE_MAP, 0x3A0A);
                sound_command(0x88);
            } else if (light == 2) {
                wr16(obj + OBJ_SPRITE_MAP, 0x39E2);
                sound_command(0x89);
            }
        }
        if (!obj_roadside_step(obj))
            return;
    }
    obj_roadside_draw(obj);
}

/*
 * When the sprite map changes, queue a DMA of its tiles into `slot`
 * ($C5AE). Tiles go to `tile` (or `tile + alt` on alternate updates of the
 * toggle counter, so the frame being shown is not overwritten); `toggle`
 * is added to that counter. The table holds 6-byte source/length entries,
 * picked by the sprite-row offset `index`.
 */
#define TILE_TOGGLE 0xFFC800
static void obj_queue_tiles(uint32_t obj, uint32_t table, uint32_t slot,
                            uint16_t tile, uint16_t alt, uint16_t toggle, uint16_t index)
{
    uint16_t map = rd16(obj + OBJ_SPRITE_MAP);
    uint32_t cmd, entry;
    if (map == rd16(obj + OBJ_ANIM))
        return;
    wr16(obj + OBJ_ANIM, map);
    wr16(TILE_TOGGLE, rd16(TILE_TOGGLE) + toggle);
    if (rd8(TILE_TOGGLE + 1) & 1)
        tile += alt;
    wr16(obj + OBJ_TILE_BASE, tile);
    /* VRAM write command for tile * 32 (built like the original) */
    cmd = (uint32_t)(int32_t)(int16_t)tile;
    cmd = (cmd & 0xffff0000u) | (uint16_t)(tile << 5);
    cmd <<= 2;
    cmd = (cmd & 0xffff0000u) | ((cmd & 0xffff) >> 2) | 0x4000;
    cmd = cmd << 16 | cmd >> 16;
    entry = table + (uint16_t)(index * 3);
    wr32(slot, cmd);
    wr32(slot + 4, rd32(entry));
    wr16(slot + 8, rd16(entry + 4));
    wr16(slot + 10, 0xFFFF);
}

/* Roadside hazard: counts a hit when passed close by ($712A). */
static void obj_hazard(uint32_t obj)
{
    int index;
    if (rd16(OBJECTS_FROZEN)) {
        obj_add_to_layer(obj);
        return;
    }
    if (!rd16(0xFF0666) && (int16_t)rd16(obj + OBJ_Z) >= 0x71) {
        int16_t side = (int16_t)rd16(obj + OBJ_LANE) < 0 ? -0x20 : 0x20;
        int16_t gap = (int16_t)(rd16(obj + OBJ_SCREEN_X) + side - 0x120);
        if (gap < 0) gap = (int16_t)-gap;
        if (gap <= 0x2A)
            wr16(0xFF0656, rd16(0xFF0656) + 1);
    }
    if (!obj_roadside_step(obj))
        return;
    index = obj_pick_sprite(obj, ROM_DEPTH_SPRITES_FAR);
    if (index < 0)
        index = rd16(obj + OBJ_SCREEN_X);   /* the original reuses that register */
    obj_queue_tiles(obj, 0x00C5FA, 0xFFC7E0, 0x408, 0x5A, 0, (uint16_t)index);
    obj_add_to_layer(obj);
}

/*
 * Roadside sign with three tile sets ($712E). Once passed, it starts the
 * streamed upload of the next piece of scenery.
 */
#define SIGN_KIND   0x3A
static void obj_sign(uint32_t obj)
{
    uint16_t kind = rd16(obj + SIGN_KIND);
    uint32_t table;
    int index;
    if (rd16(OBJECTS_FROZEN)) {
        obj_add_to_layer(obj);
        return;
    }
    if (obj_passed(obj)) {
        if (kind != 3)
            wr16(0xFF06B4, rd16(0xFF06B4) - 1);
        if (!rd16(0xFF06BE)) {
            uint16_t k = (uint16_t)(rd16(0xFF060C) * 72 + rd16(0xFFC7DC));
            wr32(0xFFC7D0, rd32(0x008ED4 + k));
            wr16(0xFFC7DC, rd16(0xFFC7DC) + 4);
            if (rd16(HUD_HIDDEN) && kind == 3)
                wr16(0xFFC7DC, rd16(0xFFC7DC) - 8);
            wr16(0xFFC7CE, 0);
            wr16(0xFFC7CC, 2);
        }
        obj_free(obj);
        return;
    }
    if (kind == 3)
        return;
    obj_project_distance(obj);
    wr16(obj + OBJ_SCREEN_X, (uint16_t)(((int16_t)rd16(ROAD_X_LINES + 2 * rd16(obj + OBJ_ROAD_LINE)) >> 6) + 0x120));
    index = obj_pick_sprite(obj, ROM_DEPTH_SPRITES_FAR);
    if (index < 0)
        index = rd16(obj + OBJ_SCREEN_X);
    table = kind == 0 ? 0x00C654 : kind == 1 ? 0x00C636 : 0x00C690;
    obj_queue_tiles(obj, table, 0xFFC7EC, 0x4BC, 0, 1, (uint16_t)index);
    obj_add_to_layer(obj);
}

/*
 * Rider overlay on the bike ($711A): frame and DMA tile source from the
 * bike's lean and race state, three palette entries into the palette
 * buffer; plays a sound when entering or leaving state $20.
 */
#define PALETTE_DIRTY    0xFFC708
static void obj_rider(uint32_t obj)
{
    uint32_t pal;
    uint16_t state, i;
    int count_down;

    if (!(rd16(0xFF066A) | rd16(0xFF06B8)))
        wr16(obj + 0x36, 0x0C);
    if (rd16(0xFFC794) == 0x10)
        return;
    wr16(0xFF0622, rd16(0xFF0614));
    if (rd16(0xFF06BC))
        return;

    if (rd16(0xFF0666))
        goto sounds;
    if (rd16(0xFF066A)) {
        uint16_t k = (uint16_t)(rd16(0xFF060C) * 9 + rd16(0xFFC806));
        pal = 0x00A914 + rd8(0x00A8F0 + k);
        state = 0;
        count_down = 1;
    } else if (rd16(0xFF06B8)) {
        pal = 0x00A92C;
        state = 0x20;
        count_down = 1;
    } else if (rd16(0xFF0626)) {
        wr16(0xFF0626, 0);
        pal = 0x00A92C;
        state = 0;
        count_down = 0;
    } else {
        wr16(obj + 0x3C, 0xFFFF);
        goto sounds;
    }

    if (count_down && rd16(HUD_HIDDEN) && !rd16(0xFFB652)) {
        int16_t t = (int16_t)(rd16(obj + 0x36) - 1);
        wr16(obj + 0x36, (uint16_t)t);
        if (t < 0) {
            wr16(0xFFB652, rd16(0xFFB652) - 1);
            wr16(0xFFC894, rd16(0xFFC894) + 1);
            wr16(0xFF0656, rd16(0xFF0656) + 1);
        }
    }
    wr16(obj + 0x3C, state);
    if (!rd16(OBJECTS_FROZEN)) {
        if (rd16(obj + 0x38)) {
            wr16(obj + 0x38, rd16(obj + 0x38) - 1);
        } else {
            wr16(obj + 0x38, 1);
            wr16(obj + 0x3A, rd16(obj + 0x3A) + 8);
        }
    }
    wr32(0xFFD3BA, rd32(pal));
    wr16(0xFFD3BE, rd16(pal + 4));
    wr8(PALETTE_DIRTY, rd8(PALETTE_DIRTY) | 1);

    i = rd16(obj + 0x3A) & 0x18;
    if (state) {
        i += state;
        if ((int8_t)rd8(PLAYER + OBJ_SPRITE_SET) > 4)
            i += 0x20;
    }
    wr16(obj + OBJ_SPRITE_MAP, rd16(0x00A896 + i));
    wr32(0xFFC7B6, rd32(0x00A890 + i));
    wr32(0xFFC7BA, rd32(0x00A894 + i));
    wr16(obj + OBJ_SCREEN_X, rd16(PLAYER + OBJ_SCREEN_X));
    wr16(obj + OBJ_SCREEN_Y, rd16(PLAYER + OBJ_SCREEN_Y));
    if (!state)
        wr16(obj + OBJ_SCREEN_X, rd16(obj + OBJ_SCREEN_X) +
             rd16(0x00A880 + 2 * rd8(PLAYER + OBJ_SPRITE_SET)));
    obj_add_to_layer(obj);

sounds:
    /* sound on entering / leaving state $20 */
    {
        uint16_t prev = rd16(obj + OBJ_ANIM), now = rd16(obj + 0x3C);
        if (prev != 0x20 && now == 0x20)
            sound_command(0x9C);
        else if (prev == 0x20 && now != 0x20)
            sound_command(0xB3);
        wr16(obj + OBJ_ANIM, now);
    }
}

/* Write 1 to a byte of the sound driver's mailbox (Z80 bus held). */
static void sound_flag(uint32_t addr)
{
    do
        wr16(Z80_BUSREQ, 0x100);
    while (rd8(Z80_BUSREQ) & 1);
    wr8(addr, 1);
    wr16(Z80_BUSREQ, 0);
}

#define PAUSED          0xFF08D4
#define PAD1_PRESSED    0xFFC707
#define SOUND_PAUSE     0xA01C08
#define SOUND_RESUME    0xA01C09

/* Start toggles pause: objects freeze, the music pauses, the icon blinks ($7122). */
static void obj_pause(uint32_t obj)
{
    uint8_t f = rd8(obj + OBJ_FLAGS);
    wr8(obj + OBJ_FLAGS, f | 1);
    if (!(f & 1)) {
        wr16(obj + OBJ_Z, 0xC0);
        wr16(obj + OBJ_SCREEN_X, 0xF8);
        wr16(obj + OBJ_SCREEN_Y, 0xF8);
        wr16(obj + OBJ_SPRITE_MAP, 0x3A38);
    }
    if (rd16(PAUSED)) {
        int8_t blink;
        if (rd8(PAD1_PRESSED) & 0x80) {
            wr16(PAUSED, 0);
            wr16(OBJECTS_FROZEN, 0);
            sound_flag(SOUND_RESUME);
            return;
        }
        blink = (int8_t)(rd8(obj + OBJ_SPRITE_SET) - 1);
        if (blink < 0)
            blink = 0x1F;
        wr8(obj + OBJ_SPRITE_SET, (uint8_t)blink);
        if (blink > 0x0F)
            obj_add_to_layer(obj);
        return;
    }
    if (rd8(PAD1_PRESSED) & 0x80) {
        wr8(obj + OBJ_SPRITE_SET, 0xFF);
        wr16(PAUSED, 0xFFFF);
        wr16(OBJECTS_FROZEN, 0xFFFF);
        sound_flag(SOUND_PAUSE);
    }
}

/* Ordinary roadside scenery ($713A); counts a hit when passed too close. */
#define ROM_SCENERY_WIDTHS 0x00D3BC
static void obj_scenery(uint32_t obj)
{
    if (!rd16(OBJECTS_FROZEN)) {
        int16_t x;
        if (!rd16(0xFF0666) && (int16_t)rd16(obj + OBJ_Z) >= 0x71) {
            int16_t width = (int16_t)rd16(ROM_SCENERY_WIDTHS + (int16_t)((rd8(obj + OBJ_SPRITE_SET) - 0xE) * 2));
            int16_t gap = (int16_t)(rd16(obj + OBJ_SCREEN_X) - 0x120);
            if (gap < 0) gap = (int16_t)-gap;
            if (gap <= width)
                wr16(0xFF0656, rd16(0xFF0656) + 1);
        }
        if (obj_passed(obj)) {
            obj_free(obj);
            return;
        }
        obj_project_distance(obj);
        x = (int16_t)(road_centre(obj) + lane_offset(obj, 0xE0) + 0x120);
        wr16(obj + OBJ_SCREEN_X, (uint16_t)x);
        obj_pick_sprite(obj, ROM_DEPTH_SPRITES_NEAR);
    }
    {
        int16_t x = (int16_t)rd16(obj + OBJ_SCREEN_X);
        if (x > 0x40 && x < 0x200)
            obj_add_to_layer(obj);
    }
}

/*
 * Continue countdown ($713E): sets up the screen, then counts down 12 frames
 * per step, blinking; a button or the end of the count leaves for mode $34.
 */
#define CD_STATE    0x3E
#define CD_HOLD     0x3C
#define CD_TICK     0x3A
static void obj_continue_countdown(uint32_t obj)
{
    uint16_t state = rd16(obj + CD_STATE);
    uint8_t pressed;
    if (state == 0) {
        wr16(0xFFC9C0, 0);              /* free objects 3-5 */
        wr16(0xFFCA00, 0);
        wr16(0xFFCA40, 0);
        wr16(0xFFC8A8, 0xFFFF);
        wr32(0xFFC7D0, 0x0000B2EA);     /* stream this screen's tiles */
        wr16(0xFFC7CC, 1);
        wr16(0xFFC7CE, 0);
        wr8(obj + OBJ_SPRITE_SET, 0x1E);
        wr16(obj + CD_STATE, 1);
        return;
    }
    if (state == 1) {
        uint8_t t = (uint8_t)(rd8(obj + OBJ_SPRITE_SET) - 1);
        wr8(obj + OBJ_SPRITE_SET, t);
        if (t)
            return;
        wr16(obj + OBJ_SCREEN_X, 0xE0);
        wr16(obj + OBJ_SCREEN_Y, 0xE0);
        wr16(obj + OBJ_SPRITE_MAP, 0x3A58);
        wr8(obj + OBJ_SPRITE_SET, 0);
        wr16(obj + CD_STATE, 2);
        return;
    }
    /* The original reads the command from ROM address $B1 (move.b ($B1).w
       instead of an immediate), which sends command 0. Kept as is. */
    sound_command(rd8(0x0000B1));
    pressed = rd8(PAD1_PRESSED);
    if ((pressed & 0x80) || (pressed & 0x70)) {
        wr16(0xFFC720, 0x34);
        return;
    }
    if (rd16(obj + CD_HOLD)) {
        uint16_t h = rd16(obj + CD_HOLD) - 1;
        wr16(obj + CD_HOLD, h);
        if (h) {
            obj_add_to_layer(obj);
        } else {
            wr16(0xFFC720, 0x34);
        }
        return;
    }
    {
        int16_t tick = (int16_t)(rd16(obj + CD_TICK) - 1);
        wr16(obj + CD_TICK, (uint16_t)tick);
        if (tick < 0) {
            uint8_t step = (uint8_t)(rd8(obj + OBJ_SPRITE_SET) + 1);
            wr16(obj + CD_TICK, 0x0C);
            wr8(obj + OBJ_SPRITE_SET, step);
            if (step >= 0x0D)
                wr16(obj + CD_HOLD, 0x1E);
        }
    }
    if (rd8(obj + OBJ_SPRITE_SET) & 1)
        obj_add_to_layer(obj);
}

/* ------------------------------------------------------------ crashes */

/*
 * After a crash the rider and the bike each follow a keyframe script: a
 * count word, then 8-byte keys (sprite map word, hold time, depth, height,
 * frame, lateral speed word). The road curve value (ROAD_CURVE) carries
 * them sideways until it is brought back through zero.
 */
#define ROAD_CURVE      0xFF0614
#define CRASH_TYPE      0xFF069A  /* 0 slow, 1 fast (speed above $96) */
#define CRASHING        0xFF0666
#define CRASH_KEY       0x32      /* byte: next key index */
#define CRASH_HOLD      0x33      /* byte */
#define CRASH_HEIGHT    0x34      /* word */
#define CRASH_LATERAL   0x30      /* word */
#define CRASH_KEYS      0x36      /* long: address of the first key */
#define CRASH_KEY_COUNT 0x3C

static void crash_load_script(uint32_t obj, uint32_t scripts)
{
    uint32_t script = rd32(scripts + 4 * rd16(CRASH_TYPE));
    wr16(obj + CRASH_KEY_COUNT, rd16(script));
    wr32(obj + CRASH_KEYS, script + 2);
    wr8(obj + CRASH_HOLD, 0);
    wr16(obj + OBJ_SCREEN_Y, 0x15F);
    wr16(obj + OBJ_SCREEN_X, 0x120);
}

/* Rider: freeze the race state and pick the script by speed ($ADCE). */
static void crash_start_rider(uint32_t obj)
{
    wr16(0xFF06A6, rd16(ROAD_CURVE));
    wr16(0xFF062E, 0);
    wr16(0xFF0630, 0);
    wr16(0xFFC802, 0);
    wr16(0xFFC828, 0);
    wr16(0xFFC7F8, 0xFFFF);
    wr16(CRASH_TYPE, (int16_t)rd16(BIKE_SPEED) > 0x96 ? 1 : 0);
    {
        uint32_t script = rd32(0x00AFCA + 4 * rd16(CRASH_TYPE));
        wr16(obj + CRASH_KEY_COUNT, rd16(script));
        wr32(obj + CRASH_KEYS, script + 2);
    }
    wr16(CRASHING, 0xFFFF);
    wr16(0xFFC7FE, 2);
    wr8(obj + CRASH_HOLD, 0);
    wr16(obj + OBJ_SCREEN_Y, 0x15F);
    wr16(obj + OBJ_SCREEN_X, 0x120);
}

/* A sound at two keys of each script ($AE3E). */
static void crash_key_sound(uint32_t obj)
{
    uint32_t t = 0x00AFC2 + 4 * rd16(CRASH_TYPE);
    int j;
    for (j = 0; j < 2; j++, t += 2)
        if (rd8(t) == rd8(obj + CRASH_KEY)) {
            wr16(0xFFC816, 0x0E);
            wr8(0xFFC80C, rd8(t + 1));
            return;
        }
}

/* Apply the current key, advance to the next one ($ACFA). */
static void crash_key_step(uint32_t obj)
{
    uint8_t i = rd8(obj + CRASH_KEY);
    uint32_t key = rd32(obj + CRASH_KEYS) + i * 8u;
    uint16_t lateral = rd16(key + 6);
    wr16(obj + OBJ_SPRITE_MAP, rd16(key));
    wr16(obj + OBJ_Z, rd8(key + 3));
    wr8(obj + CRASH_HEIGHT + 1, rd8(key + 4));
    if ((int16_t)rd16(ROAD_CURVE) >= 0)
        lateral = (uint16_t)-lateral;
    wr16(obj + CRASH_LATERAL, lateral);
    if ((int16_t)i < (int16_t)rd16(obj + CRASH_KEY_COUNT)) {
        wr8(obj + CRASH_HOLD, rd8(key + 2));
        wr8(obj + OBJ_SPRITE_SET, rd8(key + 5));
        wr8(obj + CRASH_KEY, i + 1);
        crash_key_sound(obj);
    }
}

/* Tile source and length of the current frame into a DMA slot ($AF2C). */
static void crash_frame_tiles(uint32_t obj, uint32_t table, uint32_t slot, uint32_t length)
{
    uint32_t e = table + (uint16_t)(rd8(obj + OBJ_SPRITE_SET) * 6);
    wr16(length, rd16(e));
    wr32(slot, rd32(e + 2));
}

/* Screen position from depth, lateral offset and height ($AD50). */
static void crash_project(uint32_t obj)
{
    int16_t depth = (int16_t)rd16(obj + OBJ_Z);
    int16_t x = (int16_t)((int32_t)(int16_t)rd16(obj + CRASH_LATERAL) * depth / 0xE0);
    int16_t lift = (int16_t)((int32_t)(int16_t)rd16(obj + CRASH_HEIGHT) * depth / 0xC0);
    wr16(obj + OBJ_SCREEN_X, (uint16_t)(x + 0x120));
    wr16(obj + OBJ_SCREEN_Y, (uint16_t)(road_screen_y((uint16_t)depth, 0x100) - lift));
}

/* Push the curve further out, up to a limit ($AD96, $ADAA). */
static void crash_slide_out(void)
{
    int16_t c = (int16_t)rd16(ROAD_CURVE), limit = (int16_t)rd16(0x009F8E);
    c = (int16_t)(c + (c >= 0 ? 0x10 : -0x10));
    wr16(ROAD_CURVE, (uint16_t)c);
    if (c >= 0 ? c >= limit : -c >= limit)
        wr16(ROAD_CURVE, (uint16_t)(c >= 0 ? limit : -limit));
}

/* Bring the curve back towards zero, moving the piece with it; returns 1
   once it has crossed zero. */
static int crash_slide_back(uint32_t obj, int move_curve)
{
    uint16_t step = (uint16_t)((uint32_t)0x10 * rd16(obj + OBJ_Z) / 0xBF);
    int16_t c = (int16_t)rd16(ROAD_CURVE);
    if (c < 0) {
        if (move_curve)
            wr16(ROAD_CURVE, (uint16_t)(c + 0x10));
        wr16(obj + OBJ_SCREEN_X, rd16(obj + OBJ_SCREEN_X) + step);
        return (int16_t)rd16(ROAD_CURVE) >= 0;
    }
    if (move_curve)
        wr16(ROAD_CURVE, (uint16_t)(c - 0x10));
    wr16(obj + OBJ_SCREEN_X, rd16(obj + OBJ_SCREEN_X) - step);
    return (int16_t)rd16(ROAD_CURVE) < 0;
}

/* Next key when the hold runs out, then reproject. */
static void crash_animate(uint32_t obj, uint32_t tiles, uint32_t slot, uint32_t length)
{
    int8_t hold = (int8_t)(rd8(obj + CRASH_HOLD) - 2);
    wr8(obj + CRASH_HOLD, (uint8_t)hold);
    if (hold < 0) {
        crash_key_step(obj);
        if (rd16(obj + OBJ_SPRITE_MAP) != rd16(obj + OBJ_ANIM)) {
            wr16(obj + OBJ_ANIM, rd16(obj + OBJ_SPRITE_MAP));
            crash_frame_tiles(obj, tiles, slot, length);
        }
    }
    crash_project(obj);
}

static int crash_script_running(uint32_t obj)
{
    return (int8_t)rd8(obj + CRASH_KEY_COUNT + 1) > (int8_t)rd8(obj + CRASH_KEY);
}

/* The player's bike, standing on the road ($8CD0). */
static void player_bike_place(void)
{
    wr16(PLAYER + OBJ_HANDLER, 8);
    wr16(PLAYER + OBJ_Z, 0xBF);
    wr16(PLAYER + OBJ_SCREEN_X, 0x120);
    wr16(PLAYER + OBJ_SCREEN_Y, 0x15F);
    wr16(PLAYER + OBJ_VX, 8);
    wr16(PLAYER + OBJ_VX + 2, 8);
    wr16(PLAYER + OBJ_SPRITE_MAP, 0x9AC);
    wr8(PLAYER + 0x34, 4);
    wr16(0xFFC73C, 0x80);
}

/* Put the bike back on the road after a crash ($ACC2). */
static void crash_recover(uint32_t obj)
{
    int i;
    wr8(obj + CRASH_KEY, 0);
    wr16(0xFF0658, rd16(0xFF0656));
    wr16(CRASHING, 0);
    wr16(0xFF065A, 0);
    wr16(0xFFC7F8, 0);
    wr16(0xFFC894, 0);
    wr16(0xFFC7BE, 0xFFFF);
    for (i = 0; i < 0x80; i += 4)           /* objects 1 and 2 */
        wr32(PLAYER + i, 0);
    player_bike_place();
}

/*
 * Objects for the start of a race ($8C58): everything cleared, five
 * rivals (alternating tile banks, start distances and lanes from a ROM
 * table per course, or per original-mode race; parked during the demo),
 * the rider overlays, the column stopper and the player's bike.
 */
#define RIVALS           0xFFCC40
#define ROM_RIVAL_STARTS 0x00F966   /* 5 x (distance, lane) per course */
static uint16_t setup_d1;
static void race_objects_setup(void)
{
    uint32_t obj, start;
    int k;
    for (k = 0, obj = OBJECTS; k < OBJ_COUNT; k++, obj += OBJ_SIZE)
        obj_free(obj);
    for (k = 0, obj = RIVALS; k < 5; k++, obj += OBJ_SIZE) {
        wr16(obj + OBJ_HANDLER, rd8(SOUND_LOCKED_BYTE) ? 0x48 : 0x49);
        wr16(obj + OBJ_TILE_BASE, (4 - k) & 1 ? 0x4000 : 0x2000);
        wr16(obj + 0x32, (uint16_t)(2 + 2 * k));
        wr16(obj + OBJ_Y + 2, 0x48);
    }
    start = ROM_RIVAL_STARTS + 20 * (rd16(HUD_HIDDEN) ? rd16(0xFFC85A) : rd16(0xFF060C));
    for (k = 0, obj = RIVALS; k < 5; k++, obj += OBJ_SIZE, start += 4) {
        setup_d1 = rd16(start + 2);
        wr16(obj + OBJ_DISTANCE, rd16(start));
        wr16(obj + OBJ_LANE, setup_d1);
        wr16(obj + OBJ_Y, setup_d1);
        if (rd8(SOUND_LOCKED_BYTE))
            wr16(obj + OBJ_DISTANCE, 0);
    }
    wr16(0xFFCA00, 0x0C);
    wr16(0xFFCA00 + OBJ_Z, 0xC1);
    wr16(0xFFCA40, 0x14);
    wr16(0xFFCA40 + OBJ_Z, 0xC1);
    wr16(0xFFC9C0, 0x10);
    wr16(0xFFC9C0 + OBJ_Z, 0xC1);
    wr16(0xFFCAC0, 0x44);
    player_bike_place();
}

void race_objects_setup_68k(void)
{
    race_objects_setup();
    cpu.d[1] = setup_d1;
}

/* The fallen rider ($7142). */
static void obj_crash_rider(uint32_t obj)
{
    if ((int8_t)rd8(obj + CRASH_KEY) <= 0)
        crash_start_rider(obj);
    if (rd16(OBJECTS_FROZEN) | rd16(0xFFC89E))
        goto draw;
    if (crash_script_running(obj)) {
        crash_slide_out();
    } else {
        if (rd16(HUD_HIDDEN)) {
            if (!rd16(0xFFC894)) {
                wr16(0xFFB648, rd16(0xFFB648) - 3);
                wr16(0xFFB64A, rd16(0xFFB64A) - 1);
                wr16(0xFFB64C, rd16(0xFFB64C) - 2);
                wr16(0xFFB64E, rd16(0xFFB64E) - 2);
                wr16(0xFFC894, rd16(0xFFC894) + 1);
            }
            if (((int16_t)rd16(0xFFB648) < 0 || (int16_t)rd16(0xFFB64A) < 0 ||
                 (int16_t)rd16(0xFFB652) < 0) && !rd16(0xFFC898)) {
                wr16(0xFFC898, rd16(0xFFC898) + 1);
                wr16(OBJECTS + OBJ_HANDLER, 0x50);
            }
        }
        if ((int8_t)rd8(obj + CRASH_HOLD) < 0) {
            if (crash_slide_back(obj, 1)) {
                crash_recover(obj);
                return;
            }
            goto draw;
        }
    }
    crash_animate(obj, 0x00AF44, 0xFFC796, 0xFFC7FA);
draw:
    obj_add_to_layer(obj);
}

/* The tumbling bike ($7146). */
static void obj_crash_bike(uint32_t obj)
{
    if (!rd16(CRASHING))
        return;
    if ((int8_t)rd8(obj + CRASH_KEY) <= 0)
        crash_load_script(obj, 0x00AFD2);
    if (!(rd16(OBJECTS_FROZEN) | rd16(0xFFC89E))) {
        if (!crash_script_running(obj) && (int8_t)rd8(obj + CRASH_HOLD) < 0)
            crash_slide_back(obj, 0);
        else
            crash_animate(obj, 0x00AF86, 0xFFC79E, 0xFFC7FC);
    }
    obj_add_to_layer(obj);
}

/* ------------------------------------------------------- player bike */

#define GAME_MODE       0xFFC704
#define RACE_MODE_COPY  0xFFC794  /* game mode with bit 0 clear */
#define MODE_RACE       0x14
#define MODE_FINISHED   0x10
#define BIKE_PUSH       0xFF065A  /* sideways push left from a bump */
#define BIKE_STEER      0xFF063A  /* steering target for the lean */
#define BIKE_OFFROAD    0xFF066A
#define BIKE_SPEED_CAP  0xFF0642

#define BIKE_TARGET     0x30      /* curve the auto-steer aims for */
#define BIKE_LAST_CURVE 0x32
#define BIKE_STEER_VEL  0x36
#define BIKE_LEAN       0x38
#define BIKE_SKID       0x3A
#define BIKE_FRAME_WAIT 0x3C
#define BIKE_FLAGS      0x2E      /* bit 0: leaning left */
#define BIKE_SHAKE      0x1C
#define BIKE_SHAKE_STEP 0x1E

#define ROM_CURVE_LIMIT 0x009F8E

static void clamp_curve(void)                 /* $A4F0 */
{
    int16_t c = (int16_t)rd16(ROAD_CURVE), lim = (int16_t)rd16(ROM_CURVE_LIMIT);
    if (c > lim)
        wr16(ROAD_CURVE, (uint16_t)lim);
    else if (c < -lim)
        wr16(ROAD_CURVE, (uint16_t)-lim);
}

static void add_curve(int16_t d)
{
    wr16(ROAD_CURVE, (uint16_t)((int16_t)rd16(ROAD_CURVE) + d));
}

/* Tail light flashing faster with speed ($A3AC). */
static void bike_tail_light(uint32_t obj)
{
    uint8_t t = (uint8_t)(rd8(obj + 0x1B) - 1);
    wr8(obj + 0x1B, t);
    if (t == 0) {
        wr8(obj + 0x1B, rd8(obj + 0x19));
        wr8(obj + 0x18, (uint8_t)~rd8(obj + 0x18));
    }
    wr32(0xFFD390, rd8(obj + 0x18) ? 0x00000444 : 0x04440000);
    wr8(PALETTE_DIRTY, rd8(PALETTE_DIRTY) | 1);
}

/* A bump's sideways push, decaying by 2 per frame ($A3D6). */
static void bike_push(void)
{
    int16_t push = (int16_t)rd16(BIKE_PUSH), step = (int16_t)(rd16(0x009F94) * 2);
    if (push == 0)
        return;
    if (push < 0) {
        add_curve((int16_t)-step);
        push += 2;
        wr16(BIKE_PUSH, push < 0 ? (uint16_t)push : 0);
    } else {
        add_curve(step);
        push -= 2;
        wr16(BIKE_PUSH, push >= 0 ? (uint16_t)push : 0);
    }
    clamp_curve();
}

/*
 * Steering ($A418): the lean follows BIKE_STEER by up to 12 per frame;
 * the lean (scaled down below speed $80, unless off-road) divided by the
 * grip value pushes the bike across the road.
 */
static void bike_steer(uint32_t obj)
{
    int16_t d = (int16_t)(rd16(BIKE_STEER) - rd16(obj + BIKE_LEAN));
    int32_t force;
    int16_t grip, q;
    if (d > 12) d = 12;
    if (d < -12) d = -12;
    wr16(obj + BIKE_LEAN, (uint16_t)((int16_t)rd16(obj + BIKE_LEAN) + d));
    force = (int16_t)rd16(obj + BIKE_LEAN);
    if (!rd16(BIKE_OFFROAD) && (int16_t)rd16(BIKE_SPEED) <= 0x7F)
        force = (force * (int16_t)rd16(BIKE_SPEED)) >> 7;
    force *= 16;
    grip = (int16_t)rd16(0x009F96);
    if (rd16(0xFFC8B6) || rd16(HUD_HIDDEN))
        grip = (int16_t)(grip + rd16(0x00A48C + 2 * rd16(0xFFB622)));
    q = (int16_t)f_divs((uint16_t)grip, (uint32_t)force);
    add_curve((int16_t)(-q * 2));
    clamp_curve();
}

static void bike_offroad_check(uint32_t obj)  /* $A496 */
{
    int16_t c = (int16_t)rd16(ROAD_CURVE);
    wr16(BIKE_OFFROAD, 0);
    if ((c < 0 ? -c : c) > 0x12C) {
        wr16(BIKE_OFFROAD, 0xFFFF);
        wr16(obj + BIKE_SHAKE, rd16(obj + BIKE_SHAKE) + 1);
    }
}

/* Shake on rough ground, otherwise drift back to the centre ($A512). */
static void bike_shake(uint32_t obj)
{
    if (rd16(0xFF06BE))
        return;
    if (rd16(obj + BIKE_SHAKE)) {
        uint16_t r = rng_next(), step;
        int16_t dx;
        wr16(obj + BIKE_SHAKE_STEP, rd16(obj + BIKE_SHAKE_STEP) + 1);
        step = rd16(obj + BIKE_SHAKE_STEP);
        dx = (int16_t)(r & 1);
        if (!(step & 2))
            dx = (int16_t)-dx;
        wr16(obj + OBJ_SCREEN_X, (uint16_t)(rd16(obj + OBJ_SCREEN_X) + dx));
        if (step & 1)
            wr16(obj + OBJ_SCREEN_Y, (uint16_t)(rd16(obj + OBJ_SCREEN_Y) + ((step & 2) ? -1 : 1)));
        wr16(obj + BIKE_SHAKE, 0);
        return;
    }
    wr16(obj + OBJ_SCREEN_Y, 0x15F);
    {
        int16_t x = (int16_t)rd16(obj + OBJ_SCREEN_X);
        if (x != 0x120)
            wr16(obj + OBJ_SCREEN_X, (uint16_t)(x + (x < 0x120 ? 1 : -1)));
    }
}

/* Skid frames push the bike sideways ($A4B8). */
static void bike_skid(uint32_t obj)
{
    int16_t skid = (int16_t)rd16(obj + BIKE_SKID), d;
    if (skid <= 0x0E)
        return;
    d = (int16_t)rd16(0x009F92);
    if (skid == 0x1E || skid == 0x1A || skid == 0x16)
        d += 0x0C;
    if (rd8(obj + BIKE_FLAGS) & 1)
        d = (int16_t)-d;
    add_curve((int16_t)(d * 2));
    clamp_curve();
}

/* Course-driven steering target and steering velocity (race mode). */
static void bike_auto_steer(uint32_t obj)
{
    int16_t target, diff, mag, delta, dir = 0;
    uint16_t turn = rd16(0xFF0676), next = rd16(0xFF067E);

    if ((int16_t)turn > 1)
        target = turn == 3 ? 0xB4 : -0xB4;
    else if (turn == 1)
        target = next == 2 ? 0xB4 : -0xB4;
    else
        target = next == 3 ? 0xB4 : next == 2 ? -0xB4 : 0;
    wr16(obj + BIKE_TARGET, (uint16_t)target);

    diff = (int16_t)(target - rd16(ROAD_CURVE));
    mag = diff < 0 ? (int16_t)-diff : diff;
    if (mag > 6)
        mag = 6;
    if (diff >= 0) {
        delta = (int16_t)(rd16(ROAD_CURVE) - rd16(obj + BIKE_LAST_CURVE));
        if (!(delta == 0 && mag == 0) && !rd16(BIKE_OFFROAD))
            dir = delta < 1 ? -1 : delta > 1 ? 1 : 0;
    } else {
        delta = (int16_t)(rd16(obj + BIKE_LAST_CURVE) - rd16(ROAD_CURVE));
        if (!(delta == 0 && mag == 0))
            dir = delta < 1 ? 1 : delta > 1 ? -1 : 0;
    }
    if (dir) {
        int16_t v = (int16_t)(dir * (mag + 1) * 2 + (int16_t)rd16(obj + BIKE_STEER_VEL));
        if (v > 0x7F) v = 0x7F;
        if (v < -0x7F) v = -0x7F;
        wr16(obj + BIKE_STEER_VEL, (uint16_t)v);
    }
    wr16(obj + BIKE_LAST_CURVE, rd16(ROAD_CURVE));
    wr16(BIKE_STEER, rd16(obj + BIKE_STEER_VEL));

    wr16(BIKE_SPEED_CAP, 0);
    if (rd8(0xFF068E)) {
        wr8(0xFF068E, 0);
        wr16(BIKE_SPEED_CAP, rd16(0x009FFE));
    }
    if (rd16(BIKE_OFFROAD))
        wr16(BIKE_SPEED_CAP, rd16(0x009FFE));

    if (rd16(0xFF067A)) {
        uint16_t n = rd16(obj + 0x20) + 1;
        wr16(obj + 0x20, n);
        if (n == 1) {
            int16_t t = (int16_t)(0x96 - rd16(0xFF067A));
            if (t >= 0)
                wr16(obj + 0x22, (uint16_t)t);
        } else if ((int16_t)rd16(obj + 0x22) > 0) {
            if ((int16_t)rd16(obj + 0x22) <= 0x0A || (rd8(obj + 0x23) & 8))
                wr16(BIKE_SPEED_CAP, rd16(0x009FFC));
            wr16(obj + 0x22, rd16(obj + 0x22) - 2);
        }
    } else {
        wr16(obj + 0x20, 0);
    }
    wr16(0xFF063E, 0x7F);
}

/* The player's bike ($710E). */
static void obj_player_bike(uint32_t obj)
{
    uint16_t mode;
    int16_t lean;
    int frame;

    if (rd16(OBJECTS_FROZEN) | rd16(0xFFC89E)) {
        obj_add_to_layer(obj);
        return;
    }
    mode = rd16(GAME_MODE) & ~1;
    wr16(RACE_MODE_COPY, mode);
    if (mode == MODE_FINISHED) {
        /* blink a few times, then stay */
        uint8_t t = (uint8_t)(rd8(obj + 0x34) - 1);
        wr8(obj + 0x34, t);
        if (t == 0) {
            wr8(obj + 0x34, 6);
            wr8(obj + 0x35, rd8(obj + 0x35) + 1);
        }
        if ((int8_t)rd8(obj + 0x35) > 0x0A || !(rd8(obj + 0x35) & 1))
            obj_add_to_layer(obj);
        return;
    }
    if (rd16(0xFFC7FE))
        wr16(0xFFC7FE, rd16(0xFFC7FE) - 1);
    if (rd16(0xFF06BE)) {
        wr32(BIKE_SPEED, 0);
        wr16(obj + OBJ_HANDLER, 0x54);
        return;
    }
    if (rd16(HUD_HIDDEN) && (int16_t)rd16(BIKE_SPEED) >= 0xB4) {
        if (!rd16(0xFFB64A)) {
            wr16(0xFFB64A, 0xFFFF);
            wr16(0xFFC894, rd16(0xFFC894) + 1);
            wr16(0xFF0656, rd16(0xFF0656) + 1);
        }
        if (!rd16(0xFFB650)) {
            wr16(0xFFB64A, 0xFFFF);
            wr16(0xFFB650, rd16(0xFFB650) - 1);
            wr16(0xFFC894, rd16(0xFFC894) + 1);
            wr16(0xFF0656, rd16(0xFF0656) + 1);
        }
    }
    if (mode == MODE_RACE)
        bike_auto_steer(obj);

    /* at low course progress the steering is damped */
    if ((!rd16(0xFF060A) || mode == MODE_RACE) && (int16_t)rd16(0xFF0616) < 0x20) {
        uint32_t v = (uint32_t)(int32_t)(int16_t)rd16(BIKE_STEER);
        v >>= (uint16_t)(0x20 - rd16(0xFF0616)) >> 2;
        wr16(BIKE_STEER, (uint16_t)v);
    }

    /* lean -> frame */
    lean = (int16_t)rd16(obj + BIKE_LEAN);
    if (lean < 0) {
        wr8(obj + BIKE_FLAGS, rd8(obj + BIKE_FLAGS) | 1);
        lean = (int16_t)-lean;
    } else {
        wr8(obj + BIKE_FLAGS, rd8(obj + BIKE_FLAGS) & ~1);
    }
    for (frame = 0; lean > (int16_t)rd16(0x00A932 + 2 * frame); frame++)
        ;
    if (rd16(0xFF0626) && rd8(0xFF0628) == 3 &&
        (int16_t)(rd16(0x009F7E) - 0x14) <= (int16_t)rd16(BIKE_SPEED))
        wr16(obj + BIKE_SKID, 0x20);
    if (rd16(obj + BIKE_SKID)) {
        wr16(obj + BIKE_SKID, rd16(obj + BIKE_SKID) - 2);
        frame = 3;
    } else if (rd16(BIKE_PUSH)) {
        wr16(obj + BIKE_SHAKE, rd16(obj + BIKE_SHAKE) + 1);
        wr8(obj + BIKE_FLAGS, rd8(obj + BIKE_FLAGS) & ~1);
        frame = 2;
        if ((int16_t)rd16(BIKE_PUSH) < 0)
            wr8(obj + BIKE_FLAGS, rd8(obj + BIKE_FLAGS) | 1);
    } else {
        wr8(0xFF0628, (uint8_t)frame);
        if (rd16(0xFF06BE))
            frame = 0;
    }
    frame *= 4;
    if (rd8(obj + BIKE_FLAGS) & 1)
        frame = -frame;
    {
        int update = 1;
        if (mode == MODE_RACE) {
            int16_t w = (int16_t)(rd16(obj + BIKE_FRAME_WAIT) - 1);
            wr16(obj + BIKE_FRAME_WAIT, (uint16_t)w);
            if (w >= 0)
                update = 0;
            else
                wr16(obj + BIKE_FRAME_WAIT, 4);
        }
        if (update) {
            wr16(obj + OBJ_SPRITE_MAP, rd16(0x00A946 + frame));
            wr8(obj + OBJ_SPRITE_SET, rd8(0x00A948 + frame));
        }
    }
    wr8(obj + 0x19, (uint8_t)-(((int16_t)rd16(BIKE_SPEED) >> 7) - 3));

    bike_tail_light(obj);
    bike_push();
    bike_steer(obj);
    bike_offroad_check(obj);
    bike_shake(obj);
    bike_skid(obj);
    obj_add_to_layer(obj);
}

/* ------------------------------------------------------------ rivals */

#define RIVAL_LATERAL   0x08      /* word: lane position */
#define RIVAL_SMOOTH_X  0x04      /* word: smoothing state for screen x */
#define RIVAL_ACCEL     0x0A      /* word: speed adjustment */
#define RIVAL_BOOST     0x30
#define RIVAL_TARGET    0x32
#define RIVAL_LIFE      0x36      /* long: frames alive */
#define RIVAL_BLOCKED   0x3A      /* byte: bit 0 player on the left, 1 right, 2 push */
#define RIVAL_TOUCHING  0x3B
#define RIVAL_BEND      0x3C      /* byte: how sharply the road bends there */
#define RIVAL_WIPEOUT   0x3D
#define RIVAL_WIPE_STEP 0x3E
#define RIVAL_WIPE_X    0x34

#define ROM_RIVAL_WIDTHS   0x00D3B4
#define ROM_RIVAL_LINES    0x00CFB4
#define ROM_RIVAL_SPEEDS   0x00D3EE
#define ROM_BEND_STEPS     0x00D43E
#define ROM_BEND_FRAMES    0x00D436
#define ROAD_BEND_LINES    0xFF0B00

#define SCORE_ADD       0xFF0548
#define SCORE           0xFF054C
#define TOP_SCORE       0xFF0550

/* Add SCORE_ADD to the BCD score, capped at 99999999; new top score ($95E8). */
void score_add(void)
{
    int i;
    uint8_t carry = 0;
    if (rd16(HUD_HIDDEN))
        return;
    for (i = 3; i >= 0; i--) {
        uint8_t a = rd8(SCORE_ADD + i), b = rd8(SCORE + i);
        unsigned lo = (a & 0x0f) + (b & 0x0f) + carry, sum;
        if (lo > 9) lo += 6;
        sum = (a & 0xf0) + (b & 0xf0) + lo;
        carry = sum > 0x9f;
        if (carry) sum += 0x60;
        wr8(SCORE + i, (uint8_t)sum);
    }
    if (carry)
        wr32(SCORE, 0x99999999);
    if (rd32(SCORE) >= rd32(TOP_SCORE))
        wr32(TOP_SCORE, rd32(SCORE));
}

static void rival_reset(uint32_t obj)            /* $D90A */
{
    wr16(obj + OBJ_DISTANCE, 0);
    wr16(obj + OBJ_Z, 0);
    wr16(obj + OBJ_SPRITE_MAP, 0);
    wr32(obj + RIVAL_LIFE, 0);
    wr8(obj + RIVAL_WIPE_STEP, 0);
    wr8(obj + RIVAL_WIPEOUT, 0);
    wr8(obj + OBJ_FLAGS, rd8(obj + OBJ_FLAGS) & ~1);
}

/* Touching the player nudges the bike; a new touch plays a sound and may
   make the rival wipe out. */
static void rival_contact(uint32_t obj)
{
    uint8_t touch = 0, prev;
    if ((int16_t)rd16(obj + OBJ_Z) >= 0xA0) {
        int16_t reach = (int16_t)(rd16(ROM_RIVAL_WIDTHS + 2 * (int8_t)rd8(obj + RIVAL_BEND)) + 0x0A);
        int16_t gap = (int16_t)(rd16(obj + OBJ_SCREEN_X) - 0x120);
        if ((gap < 0 ? -gap : gap) <= reach) {
            int16_t lim = (int16_t)rd16(0x009F9A);
            int16_t push = (int16_t)rd16(0x009F98);
            if ((int16_t)(rd16(obj + OBJ_SCREEN_X) - 0x120) < 0)
                push = (int16_t)-push;
            push = (int16_t)(push + rd16(BIKE_PUSH));
            if (push <= lim && push >= -lim)
                wr16(BIKE_PUSH, (uint16_t)push);
            touch = 0x92;
        }
    }
    prev = rd8(obj + RIVAL_TOUCHING);
    wr8(obj + RIVAL_TOUCHING, touch);
    if (prev || !touch)
        return;
    wr8(0xFFC80D, touch);
    if (rd16(HUD_HIDDEN) && !rd16(0xFF06BC)) {
        wr16(0xFFB64E, rd16(0xFFB64E) - 1);
        wr16(0xFFB648, rd16(0xFFB648) - 1);
        if ((int16_t)rd16(0xFFB648) < 0) {
            wr16(0xFFC894, rd16(0xFFC894) + 1);
            wr16(0xFF0656, rd16(0xFF0656) + 1);
        }
    }
    if (rng_next() & 1)
        wr8(obj + RIVAL_WIPEOUT, 0xFF);
}

/* Note which side the player is on when close behind. */
static void rival_watch_player(uint32_t obj)
{
    int16_t gap;
    if ((int16_t)rd16(obj + OBJ_Z) <= 0x70)
        return;
    gap = (int16_t)(rd16(obj + OBJ_SCREEN_X) - 0x120);
    if ((gap < 0 ? -gap : gap) >= 0x38)
        return;
    wr8(obj + RIVAL_BLOCKED, rd8(obj + RIVAL_BLOCKED) | (gap < 0 ? 1 : 2));
    wr8(0xFF068E, rd8(0xFF068E) | rd8(obj + RIVAL_BLOCKED));
}

/* Move the lane by d, unless that side is blocked or it would leave the road. */
static void rival_shift(uint32_t obj, int16_t d)
{
    int16_t lane;
    if (rd8(obj + RIVAL_BLOCKED) & (d > 0 ? 1 : 2))
        return;
    lane = (int16_t)(d + rd16(obj + RIVAL_LATERAL));
    if ((lane < 0 ? -lane : lane) < 0xFA)
        wr16(obj + RIVAL_LATERAL, (uint16_t)lane);
}

/* Lane changes following the course's turns ($D55A-$D614). */
static void rival_steer(uint32_t obj)
{
    uint16_t turn = rd16(0xFF0676), next = rd16(0xFF067E);
    int toward;
    if ((int16_t)rd16(obj + OBJ_Z) >= 0xAF)
        return;
    if (turn != next) {
        if (turn == 2 || (turn == 1 && next == 2)) {
            rival_shift(obj, -4);
            return;
        }
        if (turn == 3 || (turn == 1 && next != 1)) {
            rival_shift(obj, 4);
            return;
        }
    }
    if (next == 2) {
        rival_shift(obj, 4);
    } else if (next == 1) {
        int16_t target = (int16_t)rd16(obj + OBJ_LANE), lane = (int16_t)rd16(obj + RIVAL_LATERAL);
        if (target == lane)
            return;
        toward = target > lane;
        if (rd8(obj + RIVAL_BLOCKED) & (toward ? 1 : 2))
            return;
        wr16(obj + RIVAL_LATERAL, (uint16_t)(lane + (toward ? 6 : -6)));
    } else {
        rival_shift(obj, -4);
    }
}

/* Speed relative to the player; returns 0 if the rival was reset. */
static int rival_speed(uint32_t obj)
{
    int16_t accel, adj, rel;
    uint16_t course, idx;

    if (rd16(0xFF0652))
        wr16(obj + RIVAL_BOOST, rd16(obj + RIVAL_BOOST) + 1);
    else if (rd16(obj + RIVAL_BOOST))
        wr16(obj + RIVAL_BOOST, rd16(obj + RIVAL_BOOST) - 1);

    if (rd8(obj + RIVAL_BLOCKED) & 4) {
        wr16(obj + RIVAL_ACCEL, rd16(obj + RIVAL_ACCEL) + 1);
    } else if (rd8(obj + 0x39) & 1) {
        int16_t target = (int16_t)rd16(obj + RIVAL_TARGET), a = (int16_t)rd16(obj + RIVAL_ACCEL);
        if (target != a)
            wr16(obj + RIVAL_ACCEL, (uint16_t)(a + (target > a ? 1 : -1)));
    }
    accel = (int16_t)rd16(obj + RIVAL_ACCEL);
    if (rd16(obj + RIVAL_BOOST)) {
        adj = (int16_t)f_divu(rd16(0x009F9C), (uint32_t)(int32_t)(int16_t)rd16(obj + RIVAL_BOOST));
        if (adj > 0x26)
            adj = 0x26;
        accel = (int16_t)(accel + adj);
    }
    course = (rd8(SOUND_LOCKED_BYTE) || rd16(HUD_HIDDEN)) ? 0 : rd16(0xFF053C);
    idx = (uint16_t)(course * 18 + rd16(0xFF060A) * 2);
    rel = (int16_t)(rd16(BIKE_SPEED) - rd16(ROM_RIVAL_SPEEDS + idx));
    accel = (int16_t)(accel + ((int16_t)(rel * 10) >> 4));
    if (rd8(obj + RIVAL_WIPEOUT))
        accel = (int16_t)rd16(0x009F9E);
    wr16(obj + OBJ_DISTANCE, (uint16_t)(rd16(obj + OBJ_DISTANCE) + accel * 2));

    if ((int16_t)rd16(obj + OBJ_DISTANCE) < 0) {
        int16_t c = (int16_t)rd16(ROAD_CURVE);
        /* dropped behind: only kept (and respawned ahead) while the
           player is off the road and slow */
        if ((c < 0 ? -c : c) <= 0x12C || (int16_t)rd16(BIKE_SPEED) > 0x78) {
            rival_reset(obj);
            return 0;
        }
        if (!rd32(obj + RIVAL_LIFE))
            wr16(obj + OBJ_DISTANCE, 0x1FC0);
    }
    wr32(obj + RIVAL_LIFE, rd32(obj + RIVAL_LIFE) + 1);
    return 1;
}

/* Passed by the player: points and a sound if close ($D8CE). */
static void rival_overtaken(uint32_t obj)
{
    int16_t gap = (int16_t)(rd16(obj + OBJ_SCREEN_X) - 0x120);
    uint8_t sfx = 0xAB;
    wr16(0xFF064E, rd16(0xFF064E) + 1);
    if (gap < 0) {
        sfx = 0xAA;
        gap = (int16_t)-gap;
    }
    if (gap <= 0x80) {
        wr8(0xFFC80D, sfx);
        wr16(0xFFC826, 0x0F);
        wr16(0xFFC828, 0);
    }
    wr32(SCORE_ADD, 0x00020000);
    score_add();
    rival_reset(obj);
}

/* Distance -> screen position, lean frame and wipe-out slide ($D708). */
static int rival_project(uint32_t obj)
{
    int16_t far = (int16_t)rd16(obj + OBJ_DISTANCE) >> 4, x, d1, smooth, half;
    uint16_t line, bend_line;
    int32_t lane;
    int up, k;

    if (far > 0x1FC) {
        rival_overtaken(obj);
        return 0;
    }
    if (far < 0) {
        rival_reset(obj);
        return 0;
    }
    line = rd16(ROM_RIVAL_LINES + 2 * far) >> 4;
    wr16(obj + OBJ_ROAD_LINE, line);
    wr16(obj + OBJ_Z, line > 0xBF ? 0xBF : line);
    wr16(obj + OBJ_SCREEN_Y, road_screen_y(line, 0x100));

    lane = ((int32_t)(int16_t)line * (int16_t)rd16(obj + RIVAL_LATERAL)) << 6;
    x = (int16_t)((int16_t)(rd16(ROAD_X_LINES + 2 * line) + (int16_t)(lane / 0xE0)) >> 6);
    d1 = (int16_t)(x - rd16(obj + RIVAL_SMOOTH_X));
    if ((uint16_t)(d1 < 0 ? -d1 : d1) >= 4)
        wr16(obj + RIVAL_SMOOTH_X, (uint16_t)x);
    smooth = (int16_t)(rd16(obj + RIVAL_SMOOTH_X) + x);
    half = (int16_t)(smooth >> 1);
    wr16(obj + RIVAL_SMOOTH_X, (uint16_t)(smooth - half));
    wr16(obj + OBJ_SCREEN_X, (uint16_t)(half + 0x120));

    /* how sharply the road bends at this depth picks the lean frame */
    bend_line = (uint16_t)(((uint32_t)line * 0x54) >> 8);
    {
        int16_t bend = (int16_t)(rd16(ROAD_BEND_LINES + 2 * line) - rd16(ROAD_BEND_LINES + 2 * bend_line));
        up = 1;
        if (bend < 0) {
            up = 0;
            bend = (int16_t)-bend;
        }
        for (k = 0; bend > (int16_t)rd16(ROM_BEND_STEPS + 2 * k); k++)
            ;
    }
    wr8(obj + RIVAL_BEND, (uint8_t)k);
    wr8(obj + OBJ_SPRITE_SET, rd8(ROM_BEND_FRAMES + up * 4 + k));

    if (rd8(obj + RIVAL_WIPEOUT)) {
        int16_t side = (int16_t)(rd16(obj + OBJ_SCREEN_X) - 0x120), dx;
        if (!rd8(obj + RIVAL_WIPE_STEP))
            wr16(obj + RIVAL_WIPE_X, (uint16_t)side);
        wr8(obj + OBJ_SPRITE_SET, side < 0 ? 7 : 0x0D);
        dx = (int16_t)(rd8(obj + RIVAL_WIPE_STEP) * 4);
        if ((int16_t)rd16(obj + RIVAL_WIPE_X) < 0)
            dx = (int16_t)-dx;
        x = (int16_t)(dx * 2 + rd16(obj + OBJ_SCREEN_X));
        if (x > 0x1E8) x = 0x1E8;
        else if (x <= 0x58) x = 0x58;
        wr16(obj + OBJ_SCREEN_X, (uint16_t)x);
        wr8(obj + RIVAL_WIPE_STEP, rd8(obj + RIVAL_WIPE_STEP) + 1);
    }
    return 1;
}

/* Per-frame record for the race code, and the gap to the player. */
static void rival_record(uint32_t obj)
{
    uint32_t rec = 0xFF0804 + (uint16_t)(rd16(0xFF068A) * 8);
    uint32_t gap_slot = sx16(rd16(0xFFC81A));
    uint16_t gap;
    wr16(0xFF068A, rd16(0xFF068A) + 1);
    wr32(rec, 0xFF000000u | obj);
    wr16(rec + 4, rd16(obj + RIVAL_LATERAL));
    wr16(rec + 6, rd16(obj + OBJ_Z));
    wr8(obj + RIVAL_BLOCKED, 0);
    wr16(0xFFC81A, rd16(0xFFC81A) + 2);
    if (!rd16(0xFF060A) && (int16_t)rd16(0xFF0616) < 0x20) {
        gap = 0xFFFF;
    } else {
        uint16_t dz = (uint16_t)(rd16(PLAYER + OBJ_Z) - rd16(obj + OBJ_Z));
        int16_t dx = (int16_t)(rd16(obj + OBJ_SCREEN_X) - 0x120);
        uint16_t ax = (uint16_t)(dx < 0 ? -dx : dx);
        gap = (uint16_t)((dz < 0x28 ? 0 : dz - 0x28) + (ax < 0x50 ? 0 : ax - 0x50));
    }
    wr16(gap_slot, gap);
    wr16(0xFF0632, rd16(0xFF0632) + rd16(obj + RIVAL_ACCEL));
}

/* A rival rider ($714E). */
static void obj_rival(uint32_t obj)
{
    if (!(rd8(obj + OBJ_FLAGS) & 1))
        return;
    if (rd16(OBJECTS_FROZEN)) {
        obj_add_to_layer(obj);
        return;
    }
    if ((rd16(GAME_MODE) & ~1) != MODE_FINISHED) {
        rival_contact(obj);
        rival_watch_player(obj);
        rival_steer(obj);
        if (!rival_speed(obj))
            return;
    }
    if (!rival_project(obj))
        return;
    obj_pick_sprite(obj, ROM_DEPTH_SPRITES_NEAR);
    obj_add_to_layer(obj);
    rival_record(obj);
}

/*
 * "Extended play" message after a checkpoint ($7126): blinks by switching
 * the piece count of its sprite map in RAM ($FFB800), with a beep every 4
 * frames at first; in original mode it also shows the time of the
 * segment just finished. When it ends, the next segment's split starts.
 */
#define EXTEND_MAP       0xFFB800
#define EXTEND_TIMER     0x2D
static void obj_extended_play(uint32_t obj)
{
    static const int pick[6] = { 0, 1, 3, 4, 6, 7 };
    int hidden;
    if (!rd16(OBJECTS_FROZEN)) {
        uint8_t t;
        uint16_t pieces;
        hidden = rd16(HUD_HIDDEN) != 0;
        if (!(rd8(obj + OBJ_FLAGS) & 1)) {
            wr8(obj + OBJ_FLAGS, rd8(obj + OBJ_FLAGS) | 1);
            wr16(obj + OBJ_Z, 0xBF);
            wr16(obj + OBJ_SCREEN_Y, 0xE8);
            wr16(obj + OBJ_SPRITE_MAP, 0xB800);
            wr16(obj + OBJ_SCREEN_X, hidden ? 0xE8 : 0xC0);
        }
        t = (uint8_t)(rd8(obj + EXTEND_TIMER) + 2);
        wr8(obj + EXTEND_TIMER, t);
        if (t >= 0xF0) {
            uint16_t split;
            wr16(0xFFC7CA, 0);
            obj_free(obj);
            wr16(0xFFC8B4, 0);
            wr8(0xFFC88C, rd8(0xFF0555));
            split = (uint16_t)(rd16(0xFFC88E) + 1);
            wr16(0xFFC88E, split);
            wr32(0xFFC860 + (uint16_t)(split * 8), rd32(0xFFC82E));
            return;
        }
        if (t & 8) {
            pieces = hidden ? 0x17 : 0x12;
        } else {
            pieces = hidden ? 0x19 : 0x15;
            if (t < 0x78 && !(t & 3))
                sound_command(0x94);
        }
        wr16(EXTEND_MAP, pieces);
    }
    if (rd16(HUD_HIDDEN)) {
        int k;
        time_tiles(0xFFC861 + (uint16_t)((rd16(0xFF060A) - 1) * 8), 0xFFC8B8);
        for (k = 0; k < 6; k++)
            wr16(0xFFB828 + 6 * k, rd16(0xFFC8B8 + 2 * pick[k]));
    }
    obj_add_to_layer(obj);
}

/*
 * Retirement in original mode ($7156): stops the bike and freezes the
 * race; after a second the "retire" sound, then a button press (A, B or
 * C) or 10 seconds ends the race (best lap marked as none).
 */
#define RETIRE_DELAY     0x18
#define RETIRE_TIMEOUT   0x1C
static void obj_retire(uint32_t obj)
{
    if ((rd16(0xFFC704) & ~1) == 0x10)
        return;
    if (!(rd8(obj + OBJ_FLAGS) & 1)) {
        wr8(obj + OBJ_FLAGS, rd8(obj + OBJ_FLAGS) | 1);
        wr32(BIKE_SPEED, 0);
        wr16(0xFFC89E, 0xFFFF);
        wr16(obj + RETIRE_DELAY, 0x3C);
        wr16(obj + RETIRE_TIMEOUT, 0x258);
    }
    if (rd16(obj + RETIRE_DELAY)) {
        wr16(obj + RETIRE_DELAY, rd16(obj + RETIRE_DELAY) - 1);
        return;
    }
    sound_command(0xB1);
    wr16(OBJECTS_FROZEN, 0xFFFF);
    wr16(0xFFC896, 0xFFFF);
    if (!(rd8(0xFFC707) & 0x70)) {
        wr16(obj + RETIRE_TIMEOUT, rd16(obj + RETIRE_TIMEOUT) - 1);
        if (rd16(obj + RETIRE_TIMEOUT))
            return;
    }
    wr32(0xFFB63C, 0xFFFFFFFF);
    wr32(0xFFB640, rd32(0xFFC83A));
    wr16(0xFFC720, 0x24);
}

/* Handlers by index; NULL ones still run recompiled. */
static void (*const handlers[25])(uint32_t obj) = {
    [(0x710A - 0x710A) / 4] = obj_debris,
    [(0x710E - 0x710A) / 4] = obj_player_bike,
    [(0x7112 - 0x710A) / 4] = obj_bike_overlay,
    [(0x7116 - 0x710A) / 4] = obj_bike_overlay_2,
    [(0x711A - 0x710A) / 4] = obj_rider,
    [(0x7122 - 0x710A) / 4] = obj_pause,
    [(0x7126 - 0x710A) / 4] = obj_extended_play,
    [(0x712A - 0x710A) / 4] = obj_hazard,
    [(0x712E - 0x710A) / 4] = obj_sign,
    [(0x7132 - 0x710A) / 4] = obj_approacher,
    [(0x7136 - 0x710A) / 4] = obj_start_lights,
    [(0x713A - 0x710A) / 4] = obj_scenery,
    [(0x713E - 0x710A) / 4] = obj_continue_countdown,
    [(0x7142 - 0x710A) / 4] = obj_crash_rider,
    [(0x7146 - 0x710A) / 4] = obj_crash_bike,
    [(0x711E - 0x710A) / 4] = obj_blinker,
    [(0x714A - 0x710A) / 4] = obj_line_marker,
    [(0x714E - 0x710A) / 4] = obj_rival,
    [(0x7152 - 0x710A) / 4] = obj_column_stopper,
    [(0x7156 - 0x710A) / 4] = obj_retire,
};

/* ------------------------------------------------------ sprite table */

/*
 * Turn the layer lists into the hardware sprite table ($71A8). Each
 * object's sprite map is a piece count (minus one) and 6-byte pieces:
 * y offset (byte, from -256), size, tile (word), x offset (word). Pieces
 * off the left/right edges are dropped; at most 80 sprites.
 */
static int build_sprite_table(void)
{
    uint32_t sprite = SPRITE_TABLE, list = LAYER_LISTS;
    int left = MAX_SPRITES, l;

    for (l = 0; l < 97; l++, list += LAYER_LIST_SIZE) {
        int16_t count = (int16_t)rd16(list);
        int e;
        if (!count)
            continue;
        wr16(list, 0);
        for (e = 0; e < (count >> 1); e++) {
            uint32_t obj = sx16(rd16(list + 2 + 2 * e));
            uint32_t map;
            uint16_t pieces, y0, x0, tile0, p;
            if (!rd16(obj + OBJ_SPRITE_MAP))
                continue;
            map = sx16(rd16(obj + OBJ_SPRITE_MAP));
            pieces = rd16(map);
            map += 2;
            y0 = rd16(obj + OBJ_SCREEN_Y);
            x0 = rd16(obj + OBJ_SCREEN_X);
            tile0 = rd16(obj + OBJ_TILE_BASE);
            for (p = 0; p <= pieces; p++, map += 6) {
                int16_t x = (int16_t)(rd16(map + 4) + x0);
                wr16(sprite, (uint16_t)((0xFF00 | rd8(map)) + y0));
                wr8(sprite + 2, rd8(map + 1));
                wr16(sprite + 4, rd16(map + 2) + tile0);
                wr16(sprite + 6, (uint16_t)x);
                if (x >= 0x1BF || x < 0x5F)
                    continue;
                sprite += 8;
                if (--left == 0)
                    return MAX_SPRITES;
            }
        }
    }
    return MAX_SPRITES - left;
}

/* Link the sprites into one chain (hidden placeholder if none). */
static void link_sprites(int count)
{
    int i;
    if (count == 0) {
        wr16(SPRITE_TABLE, 0x168);
        wr8(SPRITE_TABLE + 3, 0);
        return;
    }
    for (i = 1; i < count; i++)
        wr8(SPRITE_TABLE + 3 + 8 * (i - 1), (uint8_t)i);
    wr8(SPRITE_TABLE + 3 + 8 * (count - 1), 0);
}

/* --------------------------------------------------------- main loop */

static int objects_sprite_count;

/* Run every object, then build the sprite table ($716E). */
void objects_update(void)
{
    uint32_t obj = OBJECTS;
    int k;

    wr16(0xFFC81A, 0xC81C);
    wr32(0xFFC81C, 0xFFFFFFFF);
    wr32(0xFFC820, 0xFFFFFFFF);
    wr16(0xFFC824, 0xFFFF);

    for (k = 0; k < OBJ_COUNT; k++, obj += OBJ_SIZE) {
        uint16_t index = rd16(obj + OBJ_HANDLER) & 0xFFFE;
        void (*h)(uint32_t);
        if (!index)
            continue;
        h = handlers[index / 4 - 1];
        if (h) {
            h(obj);
        } else {
            /* recompiled handler: registers as the original loop sets them */
            uint16_t counter = (uint16_t)(OBJ_COUNT - 1 - k);
            set_dn(7, 2, counter);
            push16(counter);
            cpu.a[0] = 0xFF000000u | obj;   /* as lea ($FFFFC900).w sign-extends it */
            set_dn(0, 2, index);
            cpu.a[1] = 0x7106;
            push32(0x71A0);
            m68k_dispatch(0x7106 + index);
            pop16();
        }
    }
    objects_sprite_count = build_sprite_table();
    link_sprites(objects_sprite_count);
}

void objects_update_68k(void)
{
    objects_update();
    cpu.x = objects_sprite_count == 1;   /* left by the linking code */
}
