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

/* ------------------------------------------------------------ frame */

#define PAD1_PRESSED     0xFFC707
#define FRAME_COUNTER_BYTE 0xFFC726
#define LOGO_SWEEP       0xFF510E   /* two sweep positions */

static void logo_next_mode(void)
{
    wr16(0xFFC722, 0);
    wr16(0xFFC704, 4);                    /* title screen */
}

/*
 * One logo frame after the VBlank ($724E). C/Start skips to the title.
 * While walkers remain, they trace the logo (a new walker waits one
 * turn); then the logo brightens in 14 steps of $111, and two bands of
 * colour sweep across it every other frame; after a pause, the title.
 */
static void logo_frame(void)
{
    if (rd8(PAD1_PRESSED) & 0xA0) {
        logo_next_mode();
        return;
    }
    wr8(VDP_PALETTE_DIRTY, rd8(VDP_PALETTE_DIRTY) | 1);
    if (rd16(LOGO_WALKER_COUNT)) {
        uint32_t w;
        for (w = LOGO_WALKERS; w < LOGO_WALKERS + 32 * 8; w += 8) {
            if ((int16_t)rd16(w) < 0)
                continue;
            if ((int16_t)rd16(w + 2) < 0)
                wr16(w + 2, (uint16_t)(int8_t)rd8(w + 3));
            else
                logo_walk(w);
        }
        vdp_logo_tiles(LOGO_SHOWN);
        return;
    }
    vdp_logo_tiles(LOGO_BITMAP);
    if (rd16(0xFFC71C)) {
        wr16(0xFFC71C, rd16(0xFFC71C) - 1);
        if (!rd16(0xFFC71C))
            logo_next_mode();
        return;
    }
    if (!rd16(0xFFC70E)) {
        /* brighten */
        uint16_t c = (uint16_t)(rd16(0xFFC714) + 0x111);
        int16_t steps;
        int k;
        for (k = 0; k < 14; k++) {
            wr16(0xFFD3A4 + 2 * k, c);
            wr16(0xFFD384 + 2 * k, c);
        }
        wr16(0xFFC714, c);
        steps = (int16_t)(rd16(0xFFC710) - 1);
        wr16(0xFFC710, (uint16_t)steps);
        if (steps < 0) {
            wr16(0xFFC70E, rd16(0xFFC70E) + 1);
            wr16(LOGO_SWEEP, 7);
            wr16(LOGO_SWEEP + 2, 0x0A);
            wr16(0xFFC710, 0x0D);
        }
        return;
    }
    if (!(rd8(FRAME_COUNTER_BYTE) & 1))
        return;
    {
        /* sweep: four colours per band from a ramp of 8 */
        int16_t steps = (int16_t)rd16(0xFFC710);
        int band, k;
        if ((uint16_t)steps >= 6) {
            wr16(0xFFD384, rd16(0xFFD384) - 0x111);
            wr16(0xFFD3A4, rd16(0xFFD3A4) - 0x111);
        }
        for (band = 0; band < 2; band++) {
            int16_t i = (int16_t)(rd16(LOGO_SWEEP + 2 * band) - 1);
            uint32_t dst = 0xFFD388 + 0x20 * band;
            wr16(LOGO_SWEEP + 2 * band, (uint16_t)i);
            for (k = 0; k < 4; k++, dst += 6) {
                i++;
                if (i < 0 || i >= 8)
                    continue;
                wr32(dst, rd32(0x00814A + 6 * i));
                wr16(dst + 4, rd16(0x00814A + 6 * i + 4));
            }
        }
        steps--;
        wr16(0xFFC710, (uint16_t)steps);
        if (steps < 0)
            wr16(0xFFC71C, 0x3C);
    }
}

void logo_frame_68k(void) { logo_frame(); }

/* ------------------------------------------------------------ setup */

/*
 * The logo screen's setup ($739A, with interrupts masked): the
 * bitmap unpacked to RAM and an empty copy shown, a 12 x 4 tile block on
 * plane A (the lower half flipped vertically), palettes 0-1 black with
 * colour 1 white, one walker at the logo's first pixel, the rest free.
 */
static void logo_setup(void)
{
    uint16_t tile = 1;
    uint32_t cmd = 0x561C0003;
    int half, row, k;
    screen_reset();
    sound_command(0xB1);
    wr32(VDP_CTRL, 0x50000000);
    nemesis_to_vdp(0x01E84E);
    nemesis_to_ram(0x020A56, LOGO_BITMAP);
    for (k = 0; k < 0x188; k++)
        wr32(LOGO_SHOWN + 4 * k, 0);
    vdp_logo_tiles(LOGO_SHOWN);
    for (half = 0; half < 2; half++) {
        for (row = 0; row < 2; row++, cmd += 0x800000) {
            wr32(VDP_CTRL, cmd);
            for (k = 0; k < 12; k++)
                wr16(VDP_DATA, tile++);
        }
        tile |= 0x2000;
    }
    for (k = 0; k < 2; k++) {
        int j;
        wr32(0xFFD380 + 0x20 * k, 0x0EEE);
        for (j = 1; j < 8; j++)
            wr32(0xFFD380 + 0x20 * k + 4 * j, 0);
    }
    wr16(LOGO_WALKER_COUNT, 1);
    wr32(LOGO_WALKERS, rd32(0x008142));
    wr32(LOGO_WALKERS + 4, rd32(0x008146));
    for (k = 1; k < 32; k++)
        wr16(LOGO_WALKERS + 8 * k, 0xFFFF);
    wr16(0xFFC70E, 0);
    wr16(0xFFC710, 0x0D);
    wr16(0xFFC714, 0);
    wr16(0xFFC71C, 0);
    wr16(SOUND_LOCKED, 0);
    wr16(0xFFC82C, 0);
    wr16(0xFFC8B6, 0);
}

void logo_setup_68k(void) { logo_setup(); }

/* Game mode 0 ($6CF2): the setup, or one frame. */
void mode_logo(void)
{
    if (mode_first_call()) {
        logo_setup();
        mode_setup_done(4);
        return;
    }
    wait_vblank();
    logo_frame();
}
