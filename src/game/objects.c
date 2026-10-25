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

/* ----------------------------------------------------------- handlers */

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
    if (!rd16(OBJECTS_FROZEN)) {
        obj_approach(obj);
        if (((int16_t)rd16(obj + OBJ_DISTANCE) >> 4) >= 0x1FF) {
            obj_free(obj);
            return;
        }
        obj_project_distance(obj);
        obj_project_lane(obj);
    }
    if ((int16_t)rd16(obj + OBJ_SCREEN_X) >= 0x70)
        obj_add_to_layer(obj);
}

/* Handlers by index; NULL ones still run recompiled. */
static void (*const handlers[25])(uint32_t obj) = {
    [(0x7132 - 0x710A) / 4] = obj_approacher,
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
