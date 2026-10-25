/* General utilities. */

#include "game.h"

/*
 * Pseudo-random numbers ($84C2). The 32-bit state is multiplied by 41; the
 * new value is the sum of the product's two halves, and the next state is
 * (value << 16 | low half of the product). A state whose low word is zero
 * is replaced by a fixed seed.
 */
static uint32_t rng_product, rng_state;

uint16_t rng_next(void)
{
    uint32_t seed = rd32(RNG_SEED);
    uint16_t value;
    if ((seed & 0xffff) == 0)
        seed = 0x2A6D365B;
    rng_product = seed * 41u;
    value = (uint16_t)(rng_product + (rng_product >> 16));
    rng_state = (uint32_t)value << 16 | (rng_product & 0xffff);
    wr32(RNG_SEED, rng_state);
    return value;
}

void rng_next_68k(void)
{
    uint32_t seed = rd32(RNG_SEED);
    uint16_t value;
    if ((seed & 0xffff) == 0)
        seed = 0x2A6D365B;
    value = rng_next();
    /* d0 keeps the old seed's upper word; d1 holds the new state */
    cpu.d[0] = (seed & 0xffff0000u) | value;
    cpu.d[1] = rng_state;
    f_logic(4, rng_state);
}

/* Packed BCD byte arithmetic as abcd/sbcd do it with X clear (digits
   outside 0-9 adjust the way the 68000 does). */
uint8_t bcd_add(uint8_t a, uint8_t b)
{
    unsigned lo = (a & 0x0f) + (b & 0x0f);
    unsigned r = lo + (a & 0xf0) + (b & 0xf0) + (lo > 9 ? 6 : 0);
    if (r > 0x9f)
        r -= 0xa0;
    return (uint8_t)r;
}

uint8_t bcd_sub(uint8_t a, uint8_t b)
{
    unsigned lo = (a & 0x0f) - (b & 0x0f);
    unsigned adj = lo > 0x0f ? 6 : 0;
    unsigned r = lo + (a & 0xf0) - (b & 0xf0);
    if (r > 0xff)
        r += 0xa0;
    return (uint8_t)(r - adj);
}

/* abcd with X in and out. */
uint8_t bcd_addx(uint8_t a, uint8_t b, int *x)
{
    unsigned lo = (a & 0x0f) + (b & 0x0f) + (unsigned)*x;
    unsigned r = lo + (a & 0xf0) + (b & 0xf0) + (lo > 9 ? 6 : 0);
    *x = r > 0x9f;
    if (*x)
        r -= 0xa0;
    return (uint8_t)r;
}

/* sbcd with X in and out: a - b - X. */
uint8_t bcd_subx(uint8_t a, uint8_t b, int *x)
{
    unsigned lo = (a & 0x0f) - (b & 0x0f) - (unsigned)*x;
    unsigned adj = lo > 0x0f ? 6 : 0;
    unsigned r = lo + (a & 0xf0) - (b & 0xf0);
    if (r > 0xff) {
        r += 0xa0;
        *x = 1;
    } else {
        *x = r < adj;
    }
    return (uint8_t)(r - adj);
}

/* Copy the HBlank routine into RAM, where it rewrites its own operand
   ($6D8E). The native runtime runs its own version of it. */
void hblank_install(void)
{
    int k;
    for (k = 0; k < 0x17; k++)
        wr16(0xFF05D2 + 2 * k, rd16(0x006DA6 + 2 * k));
}

void hblank_install_68k(void)
{
    hblank_install();
    cpu.a[0] = 0x006DA6 + 2 * 0x17;
    cpu.a[1] = 0xFF05D2 + 2 * 0x17;
    set_dn(0, 2, 0xFFFF);
    f_logic(2, rd16(0x006DA6 + 2 * 0x16));
}

/* --------------------------------------------------------------- waits */

#define VBLANK_COUNT     0xFF0500   /* incremented by every VBlank */
#define FRAME_TICKS      0xFFC724   /* counted up by the VBlank handler */

/* Wait for the next VBlank ($6D4E). */
void wait_vblank(void)
{
    wr16(VBLANK_COUNT, 0);
    while (!rd16(VBLANK_COUNT))
        m68k_idle(0x006D54);
}

void wait_vblank_68k(void)
{
    wait_vblank();
    f_logic(2, rd16(VBLANK_COUNT));
}

/* Wait until the VBlank handler has counted the frame tick counter up to
   8, and restart it ($6D5E). */
void wait_frame_ticks(void)
{
    do
        wait_vblank();
    while ((int16_t)rd16(FRAME_TICKS) < 8);
    wr16(FRAME_TICKS, 0);
}

void wait_frame_ticks_68k(void) { wait_frame_ticks(); }
