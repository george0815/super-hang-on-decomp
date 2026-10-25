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

/* Lane offset scaled by perspective plus the road centre -> screen x ($C86A). */
static void obj_project_lane(uint32_t obj)
{
    uint16_t line = rd16(obj + OBJ_ROAD_LINE);
    int16_t centre = (int16_t)rd16(ROAD_X_LINES + 2 * line) >> 6;
    int32_t scaled = (int32_t)(int16_t)rd16(obj + OBJ_LANE) * (int16_t)line;
    int16_t offset = (int16_t)(scaled / 0xBF);
    offset += offset >= 0 ? 4 : -4;
    wr16(obj + OBJ_SCREEN_X, (uint16_t)(centre + offset + 0x120));
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

/* Handlers by index; NULL ones still run recompiled. */
static void (*const handlers[25])(uint32_t obj) = {
    [(0x710A - 0x710A) / 4] = obj_debris,
    [(0x7112 - 0x710A) / 4] = obj_bike_overlay,
    [(0x7116 - 0x710A) / 4] = obj_bike_overlay_2,
    [(0x711A - 0x710A) / 4] = obj_rider,
    [(0x7122 - 0x710A) / 4] = obj_pause,
    [(0x712A - 0x710A) / 4] = obj_hazard,
    [(0x712E - 0x710A) / 4] = obj_sign,
    [(0x7132 - 0x710A) / 4] = obj_approacher,
    [(0x7136 - 0x710A) / 4] = obj_start_lights,
    [(0x711E - 0x710A) / 4] = obj_blinker,
    [(0x714A - 0x710A) / 4] = obj_line_marker,
    [(0x7152 - 0x710A) / 4] = obj_column_stopper,
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
