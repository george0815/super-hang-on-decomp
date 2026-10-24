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
