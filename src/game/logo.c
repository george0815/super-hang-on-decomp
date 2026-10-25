/* The SEGA logo screen (game mode 0). */

#include "game.h"

/*
 * The logo appears pixel by pixel: up to 32 walkers (8-byte records at
 * LOGO_WALKERS: x and y in the tile, byte offset of the tile, direction;
 * x negative = free) trace the logo's colour-1 pixels in LOGO_BITMAP
 * (4bpp tiles, 12 tiles = $180 bytes per row) and set them in
 * LOGO_SHOWN, which is uploaded to VRAM each frame.
 */
#define LOGO_BITMAP      0xFF3000
#define LOGO_SHOWN       0xFF4000
#define LOGO_WALKERS     0xFF5000
#define LOGO_FOUND       0xFF5100   /* first step found this turn: x, y, offset */
#define LOGO_WALKER_COUNT 0xFF5108
#define LOGO_FLAGS       0xFF5112   /* bit 7: a step was found */

static uint32_t logo_d1;
static uint16_t logo_d4, logo_d5;

/* One pixel in direction dir (0 = right, then clockwise in steps of 2,
   8 = left) from x, y in the tile at offset ($80D8). */
static void logo_step(uint16_t dir, uint16_t *x, uint16_t *y, uint16_t *offset)
{
    int dx = 0, dy = 0;
    switch (dir & 0xE) {
    case 0x0: dx = 1; break;
    case 0x2: dx = 1; dy = 1; break;
    case 0x4: dy = 1; break;
    case 0x6: dx = -1; dy = 1; break;
    case 0x8: dx = -1; break;
    case 0xA: dx = -1; dy = -1; break;
    case 0xC: dy = -1; break;
    case 0xE: dx = 1; dy = -1; break;
    }
    if (dx > 0 && ++*x >= 8) { *x = 0; *offset += 0x20; }
    if (dx < 0 && (*x)-- == 0) { *x = 7; *offset -= 0x20; }
    if (dy > 0 && ++*y >= 8) { *y = 0; *offset += 0x180; }
    if (dy < 0 && (*y)-- == 0) { *y = 7; *offset -= 0x180; }
}

/*
 * Move one walker ($8002): look at the five neighbours from 90 degrees
 * right of its direction to 90 degrees left. Every neighbour that is a
 * logo pixel not shown yet is shown; the walker moves to the first one,
 * and each further one starts a new walker (waiting a turn: its y has
 * the high byte set). A walker with nowhere to go is freed.
 */
static void logo_walk(uint32_t w)
{
    uint16_t dir = (uint16_t)((rd16(w + 6) + 4) & 0xE);
    int tries;
    wr8(LOGO_FLAGS, 0);
    for (tries = 0; tries < 5; tries++) {
        uint16_t x = rd16(w), y = rd16(w + 2), offset = rd16(w + 4);
        int16_t i;   /* signed: off the top or left edge reads below the bitmap */
        uint8_t bit, mask;
        if (tries)
            dir = (uint16_t)(dir < 2 ? 0xE : dir - 2);
        logo_step(dir, &x, &y, &offset);
        logo_d4 = offset;
        logo_d5 = dir;
        if (x & 1) {
            bit = 0x01; mask = 0x0F; logo_d1 = 0x0F;
        } else {
            bit = 0x10; mask = 0xF0; logo_d1 = 0xFFFFFFF0u;
        }
        i = (int16_t)((x >> 1) + y * 4 + offset);
        if ((rd8(LOGO_BITMAP + i) & mask) != bit || (rd8(LOGO_SHOWN + i) & mask) == bit)
            continue;
        wr8(LOGO_SHOWN + i, rd8(LOGO_SHOWN + i) | bit);
        if (!(rd8(LOGO_FLAGS) & 0x80)) {
            wr8(LOGO_FLAGS, rd8(LOGO_FLAGS) | 0x80);
            wr16(w + 6, dir);
            wr16(LOGO_FOUND, x);
            wr16(LOGO_FOUND + 2, y);
            wr16(LOGO_FOUND + 4, offset);
        } else {
            uint32_t n = LOGO_WALKERS;
            wr16(LOGO_WALKER_COUNT, rd16(LOGO_WALKER_COUNT) + 1);
            while ((int16_t)rd16(n) >= 0)
                n += 8;
            wr16(n, x);
            wr16(n + 2, y | 0xFF00);
            wr16(n + 4, offset);
            wr16(n + 6, dir);
        }
    }
    if (rd8(LOGO_FLAGS) & 0x80) {
        wr8(LOGO_FLAGS, rd8(LOGO_FLAGS) & 0x7F);
        wr32(w, rd32(LOGO_FOUND));
        wr16(w + 4, rd16(LOGO_FOUND + 4));
    } else {
        wr16(LOGO_WALKER_COUNT, rd16(LOGO_WALKER_COUNT) - 1);
        wr16(w, 0xFFFF);
    }
}

void logo_walk_68k(void)
{
    logo_walk(cpu.a[6]);
    cpu.d[1] = logo_d1;
    set_dn(4, 2, logo_d4);
    set_dn(5, 2, logo_d5);
}
