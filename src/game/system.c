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
#ifdef SNES
    frame_sync_c();
#else
    if (frame_end_hook)
        frame_end_hook();
#endif
#ifdef SNES
    /* The SNES's frames often take longer than the ticks: then the next
       frame starts at once (the original's would be waiting for this
       VBlank), with the frame's VDP changes handed over. */
    if ((int16_t)rd16(FRAME_TICKS) >= 8) {
        vdp_flush_c();
        wr16(FRAME_TICKS, 0);
        frame_start_pads();
        return;
    }
#endif
    do
        wait_vblank();
    while ((int16_t)rd16(FRAME_TICKS) < 8);
    wr16(FRAME_TICKS, 0);
    frame_start_pads();
}

void wait_frame_ticks_68k(void) { wait_frame_ticks(); }

/* --------------------------------------------------------------- reset */


#define RAM_SIGNATURE    0xFF0000   /* "init" once RAM has been set up */

/* Keep the password codes, region flags and option settings over a RAM
   clear from $FF0500 up. */
static void ram_clear_keep(void)
{
    uint16_t lang = rd16(0xFFC89C);
    uint32_t region = rd32(0xFFC890), options = rd32(0xFF053A);
    int k;
    for (k = 0; k < 28; k += 4)
        wr32(0xFF0008 + k, rd32(0xFFB658 + k));
    for (k = 0; k < 0xFB00; k += 4)
        wr32(0xFF0500 + k, 0);
    for (k = 0; k < 28; k += 4)
        wr32(0xFFB658 + k, rd32(0xFF0008 + k));
    wr16(0xFFC89C, lang);
    wr32(0xFFC890, region);
    wr32(0xFF053A, options);
}

/* ROM checksum from $200 to the end, against the header ($6BF4). */
static int rom_checksum_ok(void)
{
#ifdef SNES
    return 1;                      /* the SNES build carries the ROM as is */
#else
    uint32_t a, end = rd32(0x0001A4) + 1;
    uint16_t sum = 0;
    for (a = 0x200; a < end; a += 2)
        sum = (uint16_t)(sum + rd16(a));
    return sum == rd16(0x00018E);
#endif
}

/* The game mode table ($6CF2) */
static void (*const modes[])(void) = {
    mode_logo, mode_title, mode_course_select, mode_music_select,
    mode_race_start, mode_demo, mode_race, mode_goal, mode_ranking,
    mode_command, mode_shop, mode_password, mode_options, mode_ranking_insert,
};

/*
 * Power on and reset ($6BAC): TMSS, region flags; on a cold start the ROM
 * checksum (a red screen if it fails), all RAM cleared and the ranking
 * and record tables set up; on a reset a fade out. Then the sound
 * driver, VDP, pads and HBlank routine, and the main loop: one game mode
 * frame after another.
 */
void reset(void)
{
    uint8_t version = rd8(0xA10001);
    int k;
    if (version & 0x0F)
        wr32(0xA14000, 0x53454741);       /* "SEGA" for the TMSS */
    wr16(0xFFC890, version & 0x80);
    wr16(0xFFC89C, version & 0x80);
    wr16(0xFFC892, version & 0x40);
    if ((rd8(0xA1000D) & 0x40) && rd32(RAM_SIGNATURE) == 0x696E6974) {
        if ((int16_t)rd16(0xFFC722) > 8)
            wr16(0xFFC722, 0);
        set_sr(get_sr() & 0xF8FF);
        fade_out();                /* fade out */
        set_sr(get_sr() | 0x700);
    } else {
        uint16_t lang;
        uint32_t region;
        if (!rom_checksum_ok()) {
            vdp_init_registers();
            wr32(VDP_CTRL, 0xC0000000);
            for (k = 0; k < 64; k++)
                wr16(VDP_DATA, 0x000E);
            for (;;)
                m68k_idle(0x006D8C);
        }
        lang = rd16(0xFFC89C);
        region = rd32(0xFFC890);
        for (k = 0; k < 0x10000; k += 4)
            wr32(0xFF0000 + k, 0);
        wr16(0xFFC89C, lang);
        wr32(0xFFC890, region);
        vdp_init_registers();
        ranking_tables_init();
        records_init();
        wr32(RAM_SIGNATURE, 0x696E6974);
    }
    ram_clear_keep();
    sound_driver_load();
    vdp_init_registers();
    pads_init();
    hblank_install();
    set_sr(get_sr() & 0xF8FF);
    for (;;) {
        uint16_t mode;
        wr8(0xFFC726, rd8(0xFFC726) + 1);
        mode = rd16(0xFFC704) & 0x7FFC;
        if (mode / 4 >= (int)(sizeof modes / sizeof modes[0]))
            m68k_fault("unknown game mode", mode);
        modes[mode / 4]();
    }
}

void reset_68k(void) { reset(); }

void mode_setup_done(uint16_t vblank_handler)
{
    set_sr(get_sr() & 0xF8FF);
    wr16(0xFFC722, vblank_handler);
    wait_vblank();
    display_on();
}

/* --------------------------------------------------------------- fade */

void palette_dma_68k(void);

/* One colour channel step down ($6E1C): even values, floor 0. */
static uint16_t channel_down(uint16_t c)
{
    c &= 0xE;
    return c >= 2 ? (uint16_t)(c - 2) : 0;
}

/*
 * Fade the whole palette to black ($6DD4): seven steps, each darkening
 * every channel of the 64 colours by one level, shown every 3 VBlanks.
 */
static uint16_t fade_last;
void fade_out(void)
{
    int step, k;
    wait_vblank();
    for (step = 0; step < 7; step++) {
        for (k = 0; k < 64; k++) {
            uint16_t c = rd16(0xFFD380 + 2 * k);
            fade_last = (uint16_t)(channel_down(c >> 8) << 8 | channel_down(c >> 4) << 4 | channel_down(c));
            wr16(0xFFD380 + 2 * k, fade_last);
        }
        wait_vblank();
        wait_vblank();
        palette_dma_68k();
    }
}

void fade_out_68k(void)
{
    fade_out();
    set_dn(0, 2, 0xFFFF);
    set_dn(1, 2, 0xFFFF);
    set_dn(5, 2, fade_last);
}
