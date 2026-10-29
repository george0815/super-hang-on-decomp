/*
 * The SNES port's sound host, compiled for the SA-1 like the game
 * (tools/snes/c816.py). It runs the Genesis sound driver
 * (src/sound/driver.c) the way src/runtime/sound_host.c does: started by
 * the game's Z80 reset, then the VBlank interrupt and the YM2612's timer B
 * ticks. The driver writes the YM2612 and PSG; those registers are kept
 * here, and after each VBlank's driver work they become DSP writes for the
 * SPC700 program (snes/spc/driver.spc), queued for the S-CPU (dsp_queue,
 * snes/rt.s).
 *
 * DSP voices: 0-5 the FM channels, 6 and 7 PSG tones 1 and 2. Each FM key
 * on picks the channel's voice sample (sound_voice, from the build's
 * voice table) and envelope; its pitch follows the channel's frequency,
 * its volume the carriers' total levels and the pan.
 */
#include <stdint.h>
#include "z80_rt.h"
#include "genesis.h"
#include "driver.h"
#include "m68k_rt.h"
#include "voices.h"            /* (tools/snes/sound.py) */

void dsp_queue(uint16_t reg, uint16_t value);   /* snes/rt.s */

#define YM_RATE   53267
#define Z80_IMAGE 0x07D7F4      /* the driver in ROM (as the game loads it) */
#define Z80_SIZE  0x1A24

/* ------------------------------------------------------------ the host */

static uint16_t phase;          /* 0: held in reset */
uint8_t sound_poked;            /* the game wrote the Z80 RAM (snes/rt.s) */
static uint16_t idle;           /* the driver is stopped and waiting */
static uint16_t tick_acc;       /* timer B: YM samples since its last tick */

void sound_reset_line(uint16_t running)
{
    if (!running) {
        phase = 0;
        return;
    }
    if (phase)
        return;
    z80_ram[0x1C18] = 3;        /* the driver's entry: startup delay */
    z80_bank = 0;
    phase = 1;
    psg_vol[0] = psg_vol[1] = psg_vol[2] = psg_vol[3] = 15;
}

/* ------------------------------------------------------------ the chips */

/* The registers the driver wrote (snes/sound_io.s, with z80_read and
   z80_write): each FM channel's $30-$9C (7 kinds x 4 operators), then
   $A0, $A4, $B0, $B4. */
#define R_A0 28
#define R_A4 29
#define R_B0 30
#define R_B4 31
extern uint8_t ym_fm[6][32];
extern uint8_t ym_keys[6];      /* operators keyed on */
extern uint8_t ym_event[6];     /* 1 key on, 2 key off, since the last update */
extern uint8_t ym_dirty[6];     /* 1 frequency, 2 levels or pan written since */
extern uint16_t psg_tone[4];
extern uint8_t psg_vol[4];      /* attenuation (15: off) */
extern uint16_t z80_bank;

void z80_fault_at(uint16_t a)
{
    (void)a;
}

/* ------------------------------------------------------------ the DSP */

/* the carriers of each algorithm, as operator bits (slot order 1, 3, 2, 4) */
static const uint8_t carriers[8] = { 8, 8, 8, 8, 10, 14, 14, 15 };

/* 0.75 dB a step of total level, as a DSP volume (0-127) */
static const uint8_t tl_volume[128] = {
    127, 116, 107,  98,  90,  82,  76,  69,  64,  58,  54,  49,  45,  41,  38,  35,
     32,  29,  27,  24,  22,  21,  19,  17,  16,  15,  13,  12,  11,  10,   9,   9,
      8,   7,   7,   6,   6,   5,   5,   4,   4,   4,   3,   3,   3,   3,   2,   2,
      2,   2,   2,   2,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

/* 2 dB a step of PSG attenuation, as a DSP volume */
static const uint8_t psg_volume[16] = {
    80, 64, 50, 40, 32, 25, 20, 16, 13, 10, 8, 6, 5, 4, 3, 0
};

/* what each DSP voice was last given */
static uint16_t v_pitch[8];
static uint8_t v_left[8], v_right[8], v_src[8];
static uint8_t v_on[8];
static uint8_t v_voice[8];      /* the FM voice playing (0: none known) */

/* The FM voice channel ch keys on: its registers' hash as
   tools/snes/voices.c takes it (the carriers' levels left out: they are
   the volume), found in the build's table; 0 if it is not there. */
static uint16_t voice_of(uint16_t ch)
{
    uint16_t h = 0, k, alg = ym_fm[ch][R_B0] & 7;
    for (k = 0; k < 28; k++) {
        uint8_t b = ym_fm[ch][k];
        if (k >= 4 && k < 8 && (carriers[alg] >> (k - 4) & 1))
            b = 0;
        h = (uint16_t)(h << 3 | h >> 13) ^ b;
    }
    h = (uint16_t)(h << 3 | h >> 13) ^ (ym_fm[ch][R_B0] & 0x3F);
    for (k = 0; k < VOICES; k++)
        if (voice_hash[k] == h)
            return k + 1;
    return 0;
}

static void voice_pitch(uint16_t v, uint16_t p)
{
    if (p > 0x3FFF)
        p = 0x3FFF;
    if (p == v_pitch[v])
        return;
    v_pitch[v] = p;
    dsp_queue(v << 4 | 2, p & 0xFF);
    dsp_queue(v << 4 | 3, p >> 8);
}

static void voice_volume(uint16_t v, uint8_t left, uint8_t right)
{
    if (left != v_left[v]) {
        v_left[v] = left;
        dsp_queue(v << 4, left);
    }
    if (right != v_right[v]) {
        v_right[v] = right;
        dsp_queue(v << 4 | 1, right);
    }
}

/* An FM channel's pitch as the DSP's for its voice's sample, whose loop
   plays 500 x n Hz at $1000: fnum x 2^block x 53267 / 2^21 Hz, x $1000 /
   500 n. */
static uint16_t fm_pitch(uint16_t ch)
{
    uint32_t fnum = (uint32_t)(ym_fm[ch][R_A4] & 7) << 8 | ym_fm[ch][R_A0];
    uint16_t block = (ym_fm[ch][R_A4] >> 3) & 7;
    uint32_t p = (fnum * 213) << block >> 10;
    if (v_voice[ch] && voice_n[v_voice[ch] - 1] > 1)
        p /= voice_n[v_voice[ch] - 1];
    return p > 0x3FFF ? 0x3FFF : (uint16_t)p;
}

static uint8_t fm_volume(uint16_t ch)
{
    uint16_t alg = ym_fm[ch][R_B0] & 7, op, best = 127;
    for (op = 0; op < 4; op++)
        if (carriers[alg] >> op & 1) {
            uint16_t tl = ym_fm[ch][4 + op] & 0x7F;
            if (tl < best)
                best = tl;
        }
    return tl_volume[best];
}

static void dsp_update(void)
{
    uint16_t ch, kon = 0, koff = 0;
    for (ch = 0; ch < 6; ch++) {
        uint8_t dirty = ym_dirty[ch];
        ym_dirty[ch] = 0;
        if (ym_event[ch] == 1) {
            uint16_t voice = voice_of(ch);
            uint8_t src = (uint8_t)voice;       /* (sample 0: the square) */
            v_voice[ch] = (uint8_t)voice;
            if (src != v_src[ch]) {
                v_src[ch] = src;
                dsp_queue(ch << 4 | 4, src);
            }
            dsp_queue(ch << 4 | 5, voice ? voice_adsr1[voice - 1] : 0x8F);
            dsp_queue(ch << 4 | 6, voice ? voice_adsr2[voice - 1] : 0xE0);
            dirty |= 1;                         /* (the sample's pitch) */
            kon |= 1 << ch;
        } else if (ym_event[ch] == 2) {         /* released: GAIN decreasing */
            uint16_t voice = v_voice[ch];
            dsp_queue(ch << 4 | 5, 0);
            dsp_queue(ch << 4 | 7, 0xA0 | (voice ? voice_release[voice - 1] : 24));
        }
        ym_event[ch] = 0;
        if (dirty & 1)
            voice_pitch(ch, fm_pitch(ch));
        if (dirty & 2) {
            uint8_t vol = fm_volume(ch), pan = ym_fm[ch][R_B4];
            voice_volume(ch, (pan & 0x80) ? vol : 0, (pan & 0x40) ? vol : 0);
        }
    }
    for (ch = 0; ch < 2; ch++) {                /* PSG tones 1 and 2 */
        uint16_t v = 6 + ch, n = psg_tone[ch];
        uint8_t vol = psg_volume[psg_vol[ch]];
        if (vol && !v_on[v]) {
            dsp_queue(v << 4 | 4, 0);
            dsp_queue(v << 4 | 5, 0);           /* GAIN: direct, full */
            dsp_queue(v << 4 | 7, 0x7F);
            kon |= 1 << v;
            v_on[v] = 1;
        }
        /* 3579545 / 32n Hz for the square sample (1 kHz at $1000) */
        voice_pitch(v, n ? (uint16_t)(458182 / n > 0x3FFF ? 0x3FFF : 458182 / n) : 0);
        voice_volume(v, vol, vol);
    }
    if (koff) {
        dsp_queue(0x5C, koff);
        dsp_queue(0x5C, 0);
    }
    if (kon)
        dsp_queue(0x4C, kon);
}

/* ------------------------------------------------------------ the VBlank */

/* While nothing plays and nothing is asked for ($1C03 clear), each tick
   stops everything again ($0C37) and ends as the last did; the driver
   then sleeps until the game writes its RAM. */
static uint16_t driver_idle(void)
{
    return z80_ram[0x1C03] == 0 && !z80_ram[0x1C04] && !z80_ram[0x1C05] && !z80_ram[0x1C06];
}

/* The driver's work in a VBlank (sound_host.c's sound_vblank, then the
   timer B ticks its sound_frame would run in a 60th of a second). */
void sound_vblank_snes(void)
{
    if (phase >= 2) {
        if (idle && !sound_poked)
            return;
        sound_poked = 0;
    }
    switch (phase) {
    case 1:
        drv_irq();
        if (z80_ram[0x1C18] == 0) {
            drv_stop_all();
            drv_fm_reset();
            drv_bank_music();
            phase = 2;
        }
        break;
    case 2:
        drv_requests();
        drv_irq();
        if (z80_ram[0x1C3C]) {
            drv_timer_start();
            phase = 3;
        }
        break;
    case 3: {
        uint16_t period = 16 * (256 - z80_ram[0x1C3C]);     /* YM samples */
        tick_acc += YM_RATE / 60;
        while (tick_acc >= period) {
            tick_acc -= period;
            drv_requests();
            drv_timer_reset();
            drv_tick();
        }
        break;
    }
    }
    if (phase)
        dsp_update();
    idle = phase >= 2 && driver_idle();
}
