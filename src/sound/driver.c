/*
 * The sound driver (the Z80 program at ROM $7D7F4), in C.
 *
 * An SMPS-style sequencer. Its state stays in the Z80 RAM image at the
 * original addresses, so it can be checked against the original byte for
 * byte (src/runtime/sound_verify.c) and the 68000 side keeps talking to it
 * through the same mailbox. Song data is read through the 32 KB window at
 * $8000 into 68000 ROM (bank $78000), the driver's own tables from its
 * RAM image.
 *
 * RAM ($1C00 on):
 *   $1C00        $80 once initialised
 *   $1C01/$1C02  tempo: frames to the next stretched frame, and reload
 *   $1C03        sound being started ($80 = none)
 *   $1C04-$1C06  requests from the 68000, bit 7 set
 *   $1C08/$1C09  pause / unpause
 *   $1C0A        priority of the effect playing
 *   $1C0B/$1C0C  fade out: steps left, frames to the next step
 *   $1C0D        $80 when the track just read a new note
 *   $1C0E        PSG noise mode
 *   $1C0F-$1C16  FM3 special mode: operator frequency offsets (two sets)
 *   $1C18        startup delay (VBlanks)
 *   $1C30-$1C42  engine sound (RPM from the 68000 at $1C31, turbo $1C36)
 *   $1C37-$1C39  values set by the 68000 that songs test
 *   $1C3A        last request
 *   $1C3C        YM2612 timer B value (0 = tick from VBlank)
 *   $1C60        14 tracks of $30 bytes: FM 1-6 and two PSG for the music,
 *                then the effects ($1DE0 on)
 */
#include "z80_rt.h"
#include "genesis.h"
#include "driver.h"

#define RAM(a)  z80_ram[(uint16_t)(a) & 0x1fff]

/* Track fields */
enum {
    T_FLAGS,     /* 7 playing, 6 PSG frequency held, 5 slide, 3 raw frequencies,
                    2 channel taken by an effect, 1 no key on, 0 FM3 special */
    T_CHANNEL,   /* FM: 0-2, 4-6 (bit 2 = second part); bit 3 on the FM3 track:
                    notes (clear: drums); PSG: $80/$A0/$C0/$E0 */
    T_TEMPO,     /* duration multiplier */
    T_PTR,       /* (2) data pointer */
    T_TRANSPOSE = 5,
    T_FREQ_ENV,  /* frequency envelope + 1 (0 = none) */
    T_VOICE,     /* FM voice; PSG volume envelope + 1 */
    T_VOLUME,    /* attenuation */
    T_STACK,     /* call stack offset (grows down from $30) */
    T_DURATION,
    T_TIMER,     /* frames into the note */
    T_VOL_POS,
    T_FREQ_POS,
    T_FREQ,      /* (2) */
    T_FREQ_HI = 16,
    T_SLIDE_FM,
    T_SLIDE_PSG,
    T_PSG_VOL,   /* attenuation written; $1F silent, $FF envelope ended */
    T_ENV_END,   /* bit 7: frequency envelope ended */
    T_LAST_DURATION,
    T_PAN,       /* register $B4 */
    T_ALGORITHM, /* register $B0 */
    T_TL1, T_TL3, T_TL2, T_TL4,   /* total levels: registers $40, $48, $44, $4C */
    T_LOOPS,     /* loop counters */
    T_SIZE = 0x30
};

#define TRACKS 0x1C60

uint16_t drv_ix;            /* the current track; persists as the Z80's IX did */
#define T(n) RAM(drv_ix + (n))

static uint8_t rd(uint16_t a) { return z80_read(a); }
static uint16_t rd16(uint16_t a) { return (uint16_t)(rd(a) | rd((uint16_t)(a + 1)) << 8); }

/* ------------------------------------------------------------ hardware */

static void ym1(uint8_t reg, uint8_t v)          /* $0D6A */
{
    z80_write(0x4000, reg);
    z80_write(0x4001, v);
}

static void ym2(uint8_t reg, uint8_t v)          /* $0D80 */
{
    z80_write(0x4002, reg);
    z80_write(0x4003, v);
}

/* Writes for the current track, skipped while an effect has its channel.
   (IX can be left pointing at a song header in the bank window.) */
static void ym1_track(uint8_t reg, uint8_t v)    /* $0D65 */
{
    if (!(rd(drv_ix) & 4))
        ym1(reg, v);
}

static void ym2_track(uint8_t reg, uint8_t v)    /* $0D7B */
{
    if (!(rd(drv_ix) & 4))
        ym2(reg, v);
}

static void ym_track(uint8_t reg, uint8_t v)     /* $0D5F: the track's part */
{
    if (T(T_CHANNEL) & 4)
        ym2_track(reg, v);
    else
        ym1_track(reg, v);
}

static void key_off(uint8_t ch)                  /* $0503 */
{
    ym1_track(0x28, ch);
}

static void psg(uint8_t v) { z80_write(0x7F11, v); }

/* The 68000 bank window, a bit at a time: $C00000 for the PSG writes that
   go through it ($1039), then back to the song data at $78000 ($1015). */
static void bank_io(void)
{
    int k;
    for (k = 0; k < 7; k++)
        z80_write(0x6000, 0);
    z80_write(0x6000, 1);
    z80_write(0x6000, 1);
}

void drv_bank_music(void)
{
    uint8_t v = 0x07;
    int k;
    z80_write(0x6000, 1);
    for (k = 0; k < 8; k++) {
        z80_write(0x6000, v);
        v = (uint8_t)(v >> 1 | v << 7);
    }
}

static void psg_banked(uint8_t v)                /* $100F */
{
    bank_io();
    psg(v);
    drv_bank_music();
}

/* Register/value pairs ended by $83, plus a channel offset ($0DFF, $0E0E). */
static void fm1_regs(uint16_t p, uint8_t ch)
{
    uint8_t r;
    for (; (r = rd(p)) != 0x83; p += 2)
        ym1_track((uint8_t)(r + ch), rd((uint16_t)(p + 1)));
}

static void fm2_regs(uint16_t p, uint8_t ch)
{
    uint8_t r;
    for (; (r = rd(p)) != 0x83; p += 2)
        ym2_track((uint8_t)(r + ch), rd((uint16_t)(p + 1)));
}

/* A voice ($0D91): register pairs, the track keeping its algorithm, pan
   and total levels. */
static void voice_write(uint16_t p, uint8_t ch)
{
    uint8_t r;
    for (; (r = rd(p)) != 0x83; p += 2) {
        uint8_t v = rd((uint16_t)(p + 1));
        switch (r) {
        case 0xB0: T(T_ALGORITHM) = v; break;
        case 0x40: T(T_TL1) = v; break;
        case 0x48: T(T_TL3) = v; break;
        case 0x44: T(T_TL2) = v; break;
        case 0x4C: T(T_TL4) = v; break;
        case 0xB4: T(T_PAN) = v; break;
        }
        /* read again: the track can overlap the table */
        ym_track((uint8_t)(r + ch), rd((uint16_t)(p + 1)));
    }
}

static uint16_t voice_ptr(uint8_t voice)         /* $0BE4 */
{
    return rd16((uint16_t)(rd16(0x8006) + (uint8_t)(2 * voice)));
}

/* ----------------------------------------------------------- PSG, init */

static void psg_silence(void)                    /* $0C78 */
{
    int k;
    bank_io();
    for (k = 0; k < 10; k++)
        psg(rd((uint16_t)(0x0D55 + k)));
    drv_bank_music();
}

/* FM registers to their defaults, all keys off, PSG silent ($0C4E). */
static void fm_init(void)
{
    uint8_t ch;
    drv_ix = TRACKS;
    fm1_regs(0x0E48, 0);
    fm2_regs(0x0E48, 0);
    RAM(0x1C0B) = 0;
    RAM(0x1C0C) = 0;
    for (ch = 0; ch < 7; ch++)
        ym1_track(0x28, ch);
    RAM(0x1C00) = 0x80;
    psg_silence();
}

/* Stop everything and clear the state, keeping the timer ($0C37). */
void drv_stop_all(void)
{
    uint8_t timer;
    int k;
    RAM(0x1C0A) = 0;
    timer = RAM(0x1C3C);
    for (k = 0; k < 0x2E0; k++)
        RAM(0x1C00 + k) = 0;
    RAM(0x1C3C) = timer;
    fm_init();
}

void drv_fm_reset(void)                          /* $1070 */
{
    ym1(0x2B, 0);
    fm_init();
    ym1(0x27, 0x40);
}

void drv_timer_reset(void) { ym1(0x27, 0x6F); }  /* $0027 */

void drv_timer_start(void)                       /* $001F */
{
    ym1(0x26, RAM(0x1C3C));
    drv_timer_reset();
}

/* ------------------------------------------------------- volume, levels */

/* Add to the total levels of the algorithm's carrier operators ($07F4,
   $0864); levels past $7E are kept but not written. */
static void tl_add(uint8_t ch, int field, uint8_t add)
{
    uint8_t v = (uint8_t)(add + T(field));
    T(field) = v;
    if (v < 0x7F)
        ym_track(ch, v);
}

static void volume_apply(void)                   /* $07F4 */
{
    uint8_t alg = T(T_ALGORITHM) & 7, ch = (uint8_t)((T(T_CHANNEL) & 3) + 0x40);
    if (alg == 7)
        tl_add(ch, T_TL1, T(T_VOLUME));
    if (alg >= 5)
        tl_add((uint8_t)(ch + 4), T_TL2, T(T_VOLUME));
    if (alg >= 4)
        tl_add((uint8_t)(ch + 8), T_TL3, T(T_VOLUME));
    tl_add((uint8_t)(ch + 12), T_TL4, T(T_VOLUME));
}

static void volume_change(uint8_t d)             /* $0864 */
{
    uint8_t alg = T(T_ALGORITHM) & 7, ch = (uint8_t)((T(T_CHANNEL) & 3) + 0x40);
    RAM(0x1C17) = d;
    if (alg == 7)
        tl_add(ch, T_TL1, d);
    if (alg >= 5)
        tl_add((uint8_t)(ch + 4), T_TL2, d);
    if (alg >= 4)
        tl_add((uint8_t)(ch + 8), T_TL3, d);
    T(T_VOLUME) += d;
    tl_add((uint8_t)(ch + 12), T_TL4, d);
}

/* Restore a channel's carriers from a music track's levels ($0A46 etc.). */
static void tl_restore(void (*w)(uint8_t, uint8_t), uint16_t track, uint8_t ch)
{
    uint8_t alg = RAM(track + T_ALGORITHM) & 7;
    if (alg == 7)
        w((uint8_t)(0x40 + ch), RAM(track + T_TL1));
    if (alg >= 5)
        w((uint8_t)(0x44 + ch), RAM(track + T_TL2));
    if (alg >= 4)
        w((uint8_t)(0x48 + ch), RAM(track + T_TL3));
    w((uint8_t)(0x4C + ch), RAM(track + T_TL4));
}

/* -------------------------------------------------------- frequencies */

/* Step a frequency or volume envelope. Returns 0 when it has ended ($81
   or other codes: the caller stops there). */
static int freq_env(uint16_t table, uint16_t *freq)    /* $0516 */
{
    for (;;) {
        uint16_t p = (uint16_t)(table + T(T_FREQ_POS));
        uint8_t v = rd(p);
        if (!(v & 0x80)) {
            T(T_FREQ_POS)++;
            *freq = (uint16_t)(*freq + v);
            T(T_FREQ_HI) = (uint8_t)(*freq >> 8);
            return 1;
        }
        if (v == 0x82)
            T(T_FREQ_POS) = rd((uint16_t)(p + 1));
        else if (v == 0x80)
            T(T_FREQ_POS) = 0;
        else {
            T(T_ENV_END) = 0xFF;
            return 0;
        }
    }
}

static uint16_t track_freq(void) { return (uint16_t)(T(T_FREQ) | T(T_FREQ + 1) << 8); }

static void set_freq(uint16_t f)
{
    T(T_FREQ) = (uint8_t)f;
    T(T_FREQ + 1) = (uint8_t)(f >> 8);
}

/* FM3 in special mode: each operator its own frequency, offset by $132,
   $18E or $1E4 as the table at $1C0F (or $1C13) says ($040C). */
static void fm3_freq(void)
{
    static const uint16_t offsets[4] = { 0, 0x132, 0x18E, 0x1E4 };
    uint16_t table = (T(T_CHANNEL) & 8) ? 0x1C13 : 0x1C0F;
    int k;
    if ((T(T_CHANNEL) & 7) != 2)
        z80_fault("FM3 special mode on another channel (the original hangs)", drv_ix);
    for (k = 0; k < 4; k++) {
        uint8_t mode = RAM(table + k), reg = rd((uint16_t)(0x0480 + k));
        uint16_t f;
        if (mode > 3)
            continue;
        f = (uint16_t)(track_freq() + offsets[mode]);
        ym1_track(reg, (uint8_t)(f >> 8));
        ym1_track((uint8_t)(reg - 4), (uint8_t)f);
    }
}

static void fm_freq(uint16_t f)                  /* $03F0 */
{
    uint8_t reg;
    if (T(T_FLAGS) & 1) {
        fm3_freq();
        return;
    }
    reg = (uint8_t)((T(T_CHANNEL) & 3) + 0xA4);
    ym_track(reg, (uint8_t)(f >> 8));
    ym_track((uint8_t)(reg - 4), (uint8_t)f);
}

/* The frequency through the envelope, to the chip ($03A3). */
static void fm_freq_update(void)
{
    uint8_t env = (uint8_t)(T(T_FREQ_ENV) - 1);
    uint16_t f = track_freq();
    if (!(env & 0x80) &&
        !freq_env(rd16((uint16_t)(rd16(0x8008) + 2 * env)), &f))
        return;
    fm_freq(f);
}

static void fm_freq_env(void)                    /* $0399 */
{
    if (!T(T_FREQ_ENV) || (T(T_ENV_END) & 0x80))
        return;
    fm_freq_update();
}

static void key_on(void)                         /* $03BE */
{
    if (!track_freq() || (T(T_FLAGS) & 2))
        return;
    ym1_track(0x28, (uint8_t)((T(T_CHANNEL) & 7) | 0xF0));
}

/* Key off a frame before the note ends, unless a tie ($E7) follows ($03D7). */
static void key_off_early(void)
{
    if ((uint8_t)(T(T_DURATION) - T(T_TIMER)) == 1 &&
        rd((uint16_t)(T(T_PTR) | T(T_PTR + 1) << 8)) != 0xE7)
        key_off(T(T_CHANNEL) & 7);
}

/* FM slide: the frequency moves by a signed step; the fraction wraps into
   the next block at its ends ($0484). */
static void fm_slide(void)
{
    uint8_t step = T(T_SLIDE_FM);
    unsigned s = (unsigned)step + T(T_FREQ);
    uint8_t lo = (uint8_t)s;
    if (step & 0x80) {
        if (s <= 0xFF)
            T(T_FREQ + 1)--;
        T(T_FREQ) = lo;
        if ((T(T_FREQ + 1) & 7) != 2)
            return;
        lo = T(T_FREQ);
        if (lo >= 0x46) {
            T(T_FREQ) = lo;
            return;
        }
        lo = (uint8_t)(lo - 0x46 + 0x8C);
        T(T_FREQ) = lo;
        T(T_FREQ + 1) -= 6;
    } else {
        if (s > 0xFF)
            T(T_FREQ + 1)++;
        if (!(T(T_FREQ + 1) & 4) || lo < 0xD2) {
            T(T_FREQ) = lo;
            return;
        }
        lo = (uint8_t)(lo - 0xD2 + 0x69);
        T(T_FREQ) = lo;
        T(T_FREQ + 1) += 6;
    }
    T(T_FLAGS) |= 2;
    T(T_FREQ) = lo;
}

/* ------------------------------------------------------------ parsing */

static uint16_t pos;          /* the track's data pointer while it is read */
static uint8_t redispatch;    /* byte to read again (handlers that jump back) */

/* What a command handler does next */
enum {
    H_NEXT,        /* go on with the next byte */
    H_NEXT_MAIN,   /* the same, in the main parser (from the drum parser too) */
    H_AGAIN,       /* read `redispatch` as if it were the next byte */
    H_DRUM,        /* go on in the drum parser */
    H_END,         /* the track has stopped: end its update */
    H_RETURN       /* stop reading, without storing the pointer */
};

static int track_end(void);

/* Effects starting take their channel; the music track is marked ($0F5F). */
static uint16_t track_load(uint16_t src, uint16_t dst)  /* $0FE2: returns src */
{
    int k;
    drv_ix = src;
    for (k = 0; k < 9; k++)
        RAM(dst + k) = rd((uint16_t)(src + k));
    RAM(dst + T_STACK) = 0x30;
    RAM(dst + T_DURATION) = 1;
    RAM(dst + T_TIMER) = 0;
    RAM(dst + T_VOL_POS) = 0;
    RAM(dst + T_FREQ_POS) = 0;
    return (uint16_t)(src + 9);
}

/* Load the tracks of a song or effect: a count, then 9-byte headers. */
static void tracks_load(uint16_t src, uint16_t dst)     /* $0FD5 */
{
    uint8_t n = rd(src++);
    do {
        src = track_load(src, dst);
        dst += T_SIZE;
    } while (--n);
}

static void engine_stop(void)                    /* $01D7 */
{
    static const uint8_t off[7] = { 0xDF, 0xFF, 0xBF, 0xA0, 0x00, 0xC0, 0x00 };
    int k;
    for (k = 0; k < 7; k++)
        psg(off[k]);
    RAM(0x1C30) = 0;
    RAM(0x1D80) = 0;
    RAM(0x1DB0) = 0;
}

static int command(uint8_t cmd)
{
    uint8_t v;
    switch (cmd) {
    case 0xE4:                                   /* stop the track ($09C4) */
        T(T_FLAGS) = 0;
        key_off(2);
        return H_END;
    case 0xE5: {                                 /* note by a 68000 value ($08F1) */
        uint8_t level = RAM(0x1C38);
        uint16_t e;
        if (level >= 0x65) {
            redispatch = 0x80;
            return H_AGAIN;
        }
        e = (uint16_t)(0x0977 + 2 * (level / 10 > 9 ? 9 : level / 10));
        T(T_VOLUME) = rd(e);
        volume_apply();
        redispatch = rd((uint16_t)(e + 1));
        return H_AGAIN;
    }
    case 0xE6:                                   /* volume change ($0864) */
        volume_change(rd(pos));
        return H_NEXT;
    case 0xE7:                                   /* tie ($06E5) */
        if (!(T(T_CHANNEL) & 0x80))
            T(T_FLAGS) |= 2;
        pos--;
        return H_NEXT;
    case 0xE8:                                   /* FM3 special, second set ($06F1) */
        T(T_CHANNEL) |= 8;
        T(T_FLAGS) |= 1;
        for (v = 0; v < 4; v++)
            RAM(0x1C13 + v) = rd((uint16_t)(pos + v));
        pos += 3;
        return H_NEXT_MAIN;
    case 0xE9:                                   /* engine sound off ($06E0) */
        engine_stop();
        pos--;
        return H_NEXT;
    case 0xEA:                                   /* PSG effects channels off ($07B3) */
        if (RAM(0x1D80) & 0x80) {
            psg(0xA0);
            psg(0xBF);
            RAM(0x1D80) |= 4;
        }
        if (RAM(0x1DB0) & 0x80) {
            psg(0xC0);
            psg(0xDF);
            RAM(0x1DB0) |= 4;
            psg(0xFF);
        }
        RAM(0x1C39) = 0;
        pos--;
        return H_NEXT;
    case 0xEB:                                   /* skip 2 unless $1C39 bit 7 ($06D4) */
        if (RAM(0x1C39) & 0x80)
            pos--;
        else
            pos += 2;
        return H_NEXT;
    case 0xEC:                                   /* loop counter 4 as the next byte, */
        v = T(0x20);                             /* or 5 bytes back when 0 ($0728) */
        if (!v) {
            pos -= 5;
            v = rd(pos);
        }
        redispatch = v;
        return H_AGAIN;
    case 0xED:                                   /* start effect $AA/$AB as $1C37 says ($06AF) */
        v = RAM(0x1C37);
        if (v == 0x81 || v == 0x82) {
            RAM(0x1C03) = v == 0x81 ? 0xAA : 0xAB;
            RAM(0x1C37) = 0;
        } else
            pos--;
        return H_NEXT;
    case 0xEE:                                   /* hold / release ($073A) */
        if (rd(pos) == 1) {
            if (!(T(T_CHANNEL) & 0x80))
                T(T_FLAGS) |= 2;
            else {
                T(T_ALGORITHM) = T(T_VOICE);
                T(T_VOICE) = 1;
            }
        } else {
            if (!(T(T_CHANNEL) & 0x80))
                T(T_FLAGS) &= ~2;
            else
                T(T_VOICE) = T(T_ALGORITHM);
        }
        return H_NEXT;
    case 0xEF:                                   /* voice ($0771) */
        if (T(T_CHANNEL) & 0x80)
            return H_NEXT;
        v = rd(pos);
        T(T_VOICE) = v;
        voice_write(voice_ptr(v), T(T_CHANNEL) & 3);
        if (T(T_VOLUME))
            volume_apply();
        return H_NEXT;
    case 0xF0:                                   /* volume ($07F0) */
        T(T_VOLUME) = rd(pos);
        volume_apply();
        return H_NEXT;
    case 0xF1:                                   /* drum mode ($07E7) */
        T(T_CHANNEL) &= ~8;
        pos--;
        return H_DRUM;
    case 0xF2:                                   /* end ($09D0) */
        return track_end();
    case 0xF4:                                   /* frequency envelope ($0997) */
        T(T_FREQ_ENV) = rd(pos);
        return H_NEXT;
    case 0xF5:                                   /* PSG volume envelope ($098B) */
        if (T(T_CHANNEL) & 0x80) {
            T(T_VOICE) = rd(pos);
            pos++;
        }
        return H_NEXT;
    case 0xF6:                                   /* jump ($099C) */
        pos = (uint16_t)(rd16(pos) - 1);
        return H_NEXT;
    case 0xF7: {                                 /* loop: counter, count, address ($0C1F) */
        uint16_t c = (uint16_t)(drv_ix + (uint8_t)(rd(pos) + T_LOOPS));
        pos++;
        if (!RAM(c))
            RAM(c) = rd(pos);
        pos++;
        if (--RAM(c))
            pos = (uint16_t)(rd16(pos) - 1);
        else
            pos++;
        return H_NEXT;
    }
    case 0xF8: {                                 /* call ($0BF1) */
        uint16_t target = rd16(pos);
        uint8_t sp;
        pos++;
        sp = --T(T_STACK);
        T(T_STACK)--;
        RAM(drv_ix + sp) = (uint8_t)(pos >> 8);
        RAM(drv_ix + sp - 1) = (uint8_t)pos;
        pos = (uint16_t)(target - 1);
        return H_NEXT;
    }
    case 0xF9: {                                 /* return ($0C0C) */
        uint16_t p = (uint16_t)(drv_ix + T(T_STACK));
        pos = rd16(p);
        T(T_STACK) += 2;
        return H_NEXT;
    }
    case 0xFA:                                   /* tempo ($08E1) */
        T(T_TEMPO) = rd(pos);
        return H_NEXT;
    case 0xFB:                                   /* transpose ($08D9) */
        T(T_TRANSPOSE) += rd(pos);
        return H_NEXT;
    case 0xFC:                                   /* slide on/off ($09A2) */
        if (rd(pos) == 1)
            T(T_FLAGS) |= 0x20;
        else
            T(T_FLAGS) &= ~0x22;
        return H_NEXT;
    case 0xFD:                                   /* raw frequencies on/off ($09B5) */
        if (rd(pos) == 1)
            T(T_FLAGS) |= 8;
        else
            T(T_FLAGS) &= ~8;
        return H_NEXT;
    case 0xFE:                                   /* FM3 special, first set ($0710) */
        T(T_FLAGS) |= 1;
        for (v = 0; v < 4; v++)
            RAM(0x1C0F + v) = rd((uint16_t)(pos + v));
        pos += 3;
        return H_NEXT;
    case 0xFF:                                   /* pan ($0794) */
        T(T_PAN) = (uint8_t)((T(T_PAN) & 0x3F) | rd(pos));
        ym_track((uint8_t)((T(T_CHANNEL) & 3) + 0xB4), T(T_PAN));
        return H_NEXT;
    default:                                     /* $E0-$E3, $F3: PSG noise ($08E6) */
        v = (uint8_t)(rd(pos) | 0xE0);
        psg_banked(v);
        RAM(0x1C0E) = v;
        return H_NEXT;
    }
}

/* The drum track's notes: each loads a voice into effect track $1DE0's
   header and the FM3 channel ($1263). */
static void drum_voice(uint8_t note)
{
    uint16_t src;
    int k;
    note = (uint8_t)(note - 0x81);
    T(T_FREQ_HI) = note;
    src = rd16((uint16_t)(0x12C1 + (uint8_t)(2 * note)));
    for (k = 0; k < 9; k++)
        RAM(0x1DE0 + k) = rd((uint16_t)(src + k));
    RAM(0x1DE0 + T_STACK) = 0x30;
    RAM(0x1DE0 + T_DURATION) = 1;
    RAM(0x1DE0 + T_TIMER) = 0;
    RAM(0x1DE0 + T_VOL_POS) = 0;
    RAM(0x1DE0 + T_FREQ_POS) = 0;
    fm1_regs(rd16((uint16_t)(0x12D7 + (uint8_t)(2 * RAM(0x1DE7)))), 2);
}

/*
 * Read the track's next note ($02BB; the drum track $122D): commands, then
 * a note (or rest) with an optional slide and duration. Returns 1 when
 * the track stopped and its update must end there.
 */
static int track_read(int drum)
{
    uint8_t a, raw = 0;
    int from_drum = 0;
    if (drum) {
        if (!(T(T_FLAGS) & 4)) {                 /* $158D */
            fm1_regs(0x0E79, 2);
            key_off(2);
        }
        pos = (uint16_t)(T(T_PTR) | T(T_PTR + 1) << 8);
        goto drum_next;
    }
    pos = (uint16_t)(T(T_PTR) | T(T_PTR + 1) << 8);
next:                                            /* $02C1 */
    T(T_FLAGS) &= ~2;
    a = rd(pos++);
dispatch:                                        /* $02C7 */
    if (a >= 0xE0) {
        from_drum = 0;
        goto cmd;
    }
    if (T(T_FLAGS) & 8) {                        /* raw frequency ($0356) */
        uint16_t f = (uint16_t)(a << 8 | rd(pos++));
        if (f && T(T_TRANSPOSE))
            f = (uint16_t)(f + (int8_t)T(T_TRANSPOSE));
        set_freq(f);
        goto slide;
    }
    if (a < 0x80)
        goto duration;
    a = (uint8_t)(a - 0x80);
    if (!a) {                                    /* rest */
        if (T(T_FLAGS) & 0x20)
            pos += 2;
        T(T_FLAGS) |= 2;
        goto maybe_duration;
    }
    {
        uint16_t p = (uint16_t)(((T(T_CHANNEL) & 0x80) ? 0x159B : 0x162D) +
                                2 * (uint8_t)(a + T(T_TRANSPOSE)));
        T(T_FREQ) = rd(p);
        T(T_FREQ + 1) = rd((uint16_t)(p + 1));
    }
slide:                                           /* $02F6 */
    if (!(T(T_FLAGS) & 0x20))
        goto maybe_duration;
    a = rd(pos++);
    T(T_SLIDE_PSG) = a;
    T(T_SLIDE_FM) = a;
    a = rd(pos);
    if (!(T(T_FLAGS) & 8)) {
        T(T_SLIDE_FM) = a;
        pos++;
        a = rd(pos);
    }
    pos++;
    goto duration;
maybe_duration:                                  /* $0313 */
    a = rd(pos);
    if (!(a & 0x80)) {
        pos++;
        goto duration;
    }
    T(T_DURATION) = T(T_LAST_DURATION);
    goto start;
duration:                                        /* $0321 */
    a = (uint8_t)(a * T(T_TEMPO));
    T(T_DURATION) = a;
    T(T_LAST_DURATION) = a;
start:                                           /* $0331 */
    if (track_freq())
        T(T_PSG_VOL) = 0;
    T(T_VOL_POS) = 0;
    T(T_FREQ_POS) = 0;
    T(T_TIMER) = 0;
    T(T_ENV_END) = 0;
    T(T_PTR) = (uint8_t)pos;
    T(T_PTR + 1) = (uint8_t)(pos >> 8);
    RAM(0x1C0D) = 0x80;
    return 0;

drum_next:                                       /* $123A */
    raw = rd(pos);
    a = rd(pos++);
    if (a >= 0xE0) {
        from_drum = 1;
        goto cmd;
    }
    if (a < 0x7F) {
        a = T(T_FREQ_HI);
        if (T(T_FLAGS) & 4) {
            a = raw;
            goto duration;
        }
    }
    if ((T(T_FLAGS) & 4) || a == 0x80)
        goto maybe_duration;
    drum_voice(a);
    if (raw < 0x80) {
        a = raw;
        goto duration;
    }
    goto maybe_duration;

cmd:
    switch (command(a)) {
    case H_NEXT:
        pos++;
        if (from_drum)
            goto drum_next;
        goto next;
    case H_NEXT_MAIN:
        pos++;
        goto next;
    case H_AGAIN:
        a = redispatch;
        goto dispatch;
    case H_DRUM:
        pos++;
        goto drum_next;
    case H_END:
        return 1;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------- track end */

static int end_fm3(void)                         /* $0A0F */
{
    fm1_regs(0x0E79, 2);
    if (!(RAM(0x1CC1) & 8)) {
        ym1(0xB6, 0xC0);
        key_off(2);
        RAM(0x1CC0) &= ~4;
        return H_RETURN;
    }
    key_off(2);
    fm1_regs(voice_ptr(RAM(0x1CC0 + T_VOICE)), 2);
    tl_restore(ym1_track, 0x1CC0, 2);
    ym1_track(0xB6, RAM(0x1CC0 + T_PAN));
    fm1_regs(0x0E8A, 2);
    key_off(2);
    RAM(0x1CC0) &= ~4;
    return H_END;
}

/* An effect on FM5/FM6 ends: the other effect's voice, or the music's
   voice and levels back ($0A9F, $0B28). */
static int end_fm56(uint8_t ch, uint8_t key, uint16_t music, uint16_t other, int other_ok)
{
    fm2_regs(0x0E79, ch);
    key_off(key);
    if (other_ok && (RAM(other) & 0x80)) {
        fm2_regs(voice_ptr(RAM(other + T_VOICE)), ch);
        RAM(other) &= ~4;
    } else {
        fm2_regs(voice_ptr(RAM(music + T_VOICE)), ch);
        tl_restore(ym2, music, ch);
        ym2((uint8_t)(0xB4 + ch), RAM(music + T_PAN));
    }
    fm2_regs(0x0E96, ch);
    RAM(music) &= ~4;
    return H_END;
}

static int end_psg(void)                         /* $0BB9 */
{
    uint8_t v = (uint8_t)(T(T_CHANNEL) + 0x1F);
    psg_banked(v);
    if (v >= 0xDF) {
        bank_io();
        psg(0xDF);
        psg(0xFF);
        drv_bank_music();
        RAM(0x1E40) &= ~4;
        RAM(0x1EE3) = 0x1F;
    }
    return H_END;
}

static int track_end(void)                       /* $09D0 */
{
    uint8_t ch = T(T_CHANNEL);
    RAM(0x1C0A) = 0;
    T(T_FLAGS) = 0;
    if (ch & 0x80)
        return end_psg();
    switch (ch & 7) {
    case 2: return end_fm3();
    case 5: return end_fm56(1, 5, 0x1D20, 0x1E10, 1);
    case 6: return end_fm56(2, 6, 0x1D50, 0x1E40, RAM(0x1C3A) != 0x90);
    }
    if (ch & 4)
        fm2_regs(0x0E79, ch & 3);
    else
        fm1_regs(0x0E79, ch & 3);
    key_off(ch & 7);
    return H_END;
}

/* ------------------------------------------------------- track update */

static uint8_t count_down(void)                  /* $038F */
{
    T(T_TIMER)++;
    return (uint8_t)(T(T_DURATION) - T(T_TIMER));
}

static void psg_mute(void)                       /* $0599 */
{
    T(T_PSG_VOL) = 0x1F;
    psg((uint8_t)(0x1F + T(T_CHANNEL)));
}

/* PSG volume envelope ($0621): an attenuation step, or -1 to mute ($83),
   -2 when it has ended ($81). */
static int vol_env(uint16_t table)
{
    for (;;) {
        uint16_t p = (uint16_t)(table + T(T_VOL_POS));
        uint8_t v = rd(p);
        if (!(v & 0x80)) {
            T(T_VOL_POS)++;
            v = (uint8_t)(v + T(T_VOLUME));
            return (v & 0x10) ? 0x0F : v;
        }
        if (v == 0x83)
            return -1;
        if (v == 0x81) {
            T(T_PSG_VOL) = 0xFF;
            return -2;
        }
        T(T_VOL_POS) = v == 0x80 ? 0 : rd((uint16_t)(p + 1));
    }
}

static void psg_update(void)                     /* $054D */
{
    uint8_t a = count_down(), env;
    uint16_t f;
    if (!a) {
        if (track_read(0))
            return;
        a = 0x80;
    }
    RAM(0x1C0D) = a;
    if (a != 0x80 && (T(T_FLAGS) & 0x20)) {
        uint8_t step = T(T_SLIDE_PSG);
        unsigned s = (unsigned)step + T(T_FREQ);
        if (step & 0x80) {
            if (s <= 0xFF)
                T(T_FREQ + 1)--;
        } else if (s > 0xFF)
            T(T_FREQ + 1)++;
        T(T_FREQ) = (uint8_t)s;
    }
    if ((T(T_FLAGS) & 4) || T(T_PSG_VOL) == 0x1F)
        return;
    if (T(T_FREQ + 1) == T(T_FREQ)) {
        psg_mute();
        return;
    }
    if (T(T_PSG_VOL) != 0xFF) {
        uint8_t e = (uint8_t)(T(T_VOICE) - 1);
        if (!(e & 0x80)) {
            int v = vol_env(rd16((uint16_t)(rd16(0x800A) + 2 * e)));
            if (v == -1) {
                psg_mute();
                return;
            }
            if (v == -2)
                return;
            psg((uint8_t)(((uint8_t)v | T(T_CHANNEL)) + 0x10));
        }
    }
    env = (uint8_t)(T(T_FREQ_ENV) - 1);
    if (!(RAM(0x1C0D) & 0x80) && ((T(T_ENV_END) & 0x80) || (env & 0x80)))
        return;
    f = track_freq();
    if (!(env & 0x80)) {
        if (!f)
            T(T_ENV_END) = 0xFF;
        else if (!freq_env(rd16((uint16_t)(rd16(0x8008) + 2 * env)), &f))
            return;
    }
    if (T(T_FLAGS) & 0x40)
        return;
    {
        uint8_t ch = T(T_CHANNEL) == 0xE0 ? 0xC0 : T(T_CHANNEL);
        uint8_t hi = (uint8_t)((f & 0xF0) | (f >> 8));
        psg((uint8_t)((f & 0x0F) | ch));
        psg((uint8_t)(hi >> 4 | hi << 4));
    }
}

static void track_update(void)                   /* $0238 */
{
    if (T(T_CHANNEL) & 0x80) {
        psg_update();
        return;
    }
    if (!count_down()) {
        RAM(0x1C0D) = 0;
        if (track_read(0))
            return;
        fm_freq_update();
        key_on();
        return;
    }
    key_off_early();
    if (T(T_FLAGS) & 0x20) {
        fm_slide();
        fm_freq_update();
    } else
        fm_freq_env();
}

/* The FM3 music track: in drum mode, the drum parser ($022A). */
static void track_update_fm3(void)
{
    if (T(T_CHANNEL) & 8) {
        track_update();
        return;
    }
    if (!count_down())
        track_read(1);
}

static void tracks_update(void)                  /* $0205 */
{
    int n;
    drv_ix = TRACKS;
    for (n = 14; n; n--, drv_ix += T_SIZE) {
        if (!(T(T_FLAGS) & 0x80))
            continue;
        if (n == 12)
            track_update_fm3();
        else
            track_update();
    }
}

/* ----------------------------------------------------- engine sound */

static void engine_marks(uint8_t rpm)            /* $016D */
{
    if (RAM(0x1C34) || RAM(0x1C35))
        return;
    if (rpm == 0x64 || rpm == 0x96 || rpm == 0xC8 || rpm == 0xFA)
        RAM(0x1C33) = rpm;
}

/* RPM to a pitch value ($012A). */
static uint16_t engine_curve(uint16_t rpm)
{
    uint8_t a = (uint8_t)rpm;
    if (rpm >> 8) {
        if (a < 0x18)
            return (uint16_t)((rpm - 0x80) * 2);
        return (uint16_t)((rpm >> 1) + 0xA8);
    }
    engine_marks(a);
    if (a < 0x32)
        return (uint8_t)(a * 2);
    if (a < 0x96)
        return (uint8_t)((a >> 1) + 0x4B);
    if (a < 0xB9)
        return (uint8_t)((a - 0x4B) * 2);
    return (uint16_t)((rpm >> 1) + 0x80);
}

/* ($0198; the masked term is always 0) */
static uint16_t engine_offset(uint16_t v)
{
    uint16_t r;
    if (v > 0x15F)
        return 0x180;
    r = (uint16_t)((((v & 0xFF) >> 4) & ((v >> 8 & 0x0F) << 4)) + v);
    if (RAM(0x1C36))
        r = (uint16_t)(r + 0xA0);
    return r;
}

/* The engine: two PSG tones from the RPM ($00B0). */
static void engine(void)
{
    uint16_t v;
    if (!RAM(0x1C30))
        return;
    v = (uint16_t)(0x3FF - engine_offset(engine_curve((uint16_t)(RAM(0x1C31) | RAM(0x1C32) << 8))));
    RAM(0x1C3E) = (uint8_t)(v >> 8);
    RAM(0x1C3F) = (uint8_t)v;
    v = (uint16_t)(v - 0x150);
    RAM(0x1C41) = (uint8_t)(v >> 8);
    RAM(0x1C42) = (uint8_t)v;
    RAM(0x1D85) = 0xF0;
    RAM(0x1DB5) = 0x00;
    RAM(0x1C40) = 2;
    if (RAM(0x1C36)) {
        RAM(0x1C3F) = 1;
        RAM(0x1D85) = 0x90;
        RAM(0x1DB5) = 0x99;
    }
}

/* -------------------------------------------------- starting sounds */

static void sound_done(void) { RAM(0x1C03) = 0x80; }   /* $0FDC */

static void music_start(uint8_t id)              /* $0EEC */
{
    uint8_t song = (uint8_t)(id - 0x81);
    if (song & 0x80)
        return;
    drv_stop_all();
    RAM(0x1C01) = RAM(0x1C02) = rd((uint16_t)(rd16(0x8002) + song));
    tracks_load(rd16((uint16_t)(rd16(0x8004) + (uint8_t)(2 * song))), TRACKS);
    sound_done();
}

/* Effects $90-$A7: each track takes over its channel ($0F5F). */
static void sfx_start(uint8_t id)
{
    uint16_t p;
    uint8_t n;
    if (id == 0x9C)
        RAM(0x1C30) = 0;
    p = rd16((uint16_t)(0x16D9 + (uint8_t)(2 * (id - 0x90))));
    n = rd(p++);
    do {
        uint8_t ch = rd(++p), c;
        if (ch == 0xA0 || ch == 0xC0 || ch == 0xE0) {
            if (ch == 0xA0) {
                psg(0xDF);
                psg(0xFF);
                psg(0x80);
                psg(0x00);
            }
            tracks_load((uint16_t)(p - 2), ch == 0xA0 ? 0x1D80 : 0x1DB0);
            break;
        }
        c = ch & 7;
        key_off(c);
        RAM(rd16((uint16_t)(0x0EA9 + 2 * c))) |= 4;
        p = track_load((uint16_t)(p - 1), rd16((uint16_t)(0x0E9B + 2 * c)));
    } while (--n);
    sound_done();
}

/* Effects $A8-$AF ($0F10). */
static void sfx2_start(uint8_t id)
{
    uint16_t p, dst;
    uint8_t n, ch;
    if (id == 0xA9 && RAM(0x1C39)) {
        RAM(0x1C03) = RAM(0x1C39);
        return;
    }
    p = rd16((uint16_t)(0x1709 + (uint8_t)(2 * (RAM(0x1C03) - 0xA8))));
    n = rd(p);
    ch = rd((uint16_t)(p + 2));
    /* one track on FM3/FM5/FM6: that channel's effect track; otherwise
       (oddly) by the track count */
    if (n == 1 && (ch == 2 || ch == 5 || ch == 6))
        dst = ch == 2 ? 0x1DE0 : ch == 5 ? 0x1E10 : 0x1E40;
    else
        dst = (n == 1 ? ch : n) == 2 ? 0x1E10 : 0x1DE0;
    tracks_load(p, dst);
    sound_done();
}

static void engine_start(void)                   /* $01C3 */
{
    int k;
    RAM(0x1C30) = 0x99;
    sfx_start(0x99);
    for (k = 0; k < 12; k++)
        RAM(0x1C3B + k) = rd((uint16_t)(0x186C + k));
}

static void fade_start(void)                     /* $116C */
{
    static const uint16_t tracks[10] = {
        0x1CC0, 0x1DE0, 0x1E10, 0x1E40, 0x1E70, 0x1EA0, 0x1ED0, 0x1D50, 0x1D80, 0x1DB0,
    };
    int k;
    RAM(0x1C0B) = 0xE0;
    RAM(0x1C0C) = 2;
    for (k = 0; k < 10; k++)
        RAM(tracks[k]) = 0;
    psg_silence();
    RAM(0x1C00) = 0x80;
    sound_done();
}

/* The sound in $1C03 ($0EBE). */
static void sound_start(void)
{
    uint8_t id = RAM(0x1C03);
    if (!(id & 0x80) || id >= 0xB5)
        drv_stop_all();
    else if (id < 0x90)
        music_start(id);
    else if (id <= 0xA7)
        sfx_start(id);
    else if (id < 0xB0)
        sfx2_start(id);
    else switch (id) {
    case 0xB0: fade_start(); break;
    case 0xB1: drv_stop_all(); break;
    case 0xB2: psg_silence(); break;
    case 0xB3: engine_start(); break;
    case 0xB4: engine_stop(); break;
    }
}

/* The 68000's requests: a sound with bit 7 of its priority set starts at
   once and clears the queue; otherwise one at least as important as the
   effect playing ($11E8). */
void drv_requests(void)
{
    uint16_t slot;
    for (slot = 0x1C04; slot <= 0x1C06; slot++) {
        uint8_t id = RAM(slot), prio;
        if (!(id & 0x80))
            continue;
        RAM(0x1C3A) = id;
        prio = rd((uint16_t)(rd16(0x8000) + (uint8_t)((id & 0x7F) - 1)));
        if (prio & 0x80) {
            RAM(0x1C03) = id;
            RAM(0x1C04) = RAM(0x1C05) = RAM(0x1C06) = 0;
            continue;
        }
        if (RAM(0x1C0A) <= prio) {
            RAM(0x1C03) = id;
            RAM(0x1C0A) = prio;
        }
        RAM(slot) = 0;
    }
}

/* -------------------------------------------------------------- tick */

static void pause(void)                          /* $0C8F */
{
    uint8_t r;
    psg_silence();
    for (r = 0xB4; r < 0xB7; r++)
        ym1(r, 0);
    for (r = 0xB4; r < 0xB7; r++)
        ym2(r, 0);
    for (r = 0; r < 7; r++)
        ym1(0x28, r);
    sound_done();
}

/* A channel an effect has taken: its pan from the effect ($0D01 etc.). */
static void pan_restore(void (*w)(uint8_t, uint8_t), uint8_t reg, uint16_t music,
                        uint16_t effect2, uint16_t effect1)
{
    if (!(RAM(music) & 4))
        return;
    w(reg, RAM(((RAM(effect2) & 0x80) ? effect2 : effect1) + T_PAN));
}

static void unpause(void)                        /* $0CBA */
{
    int k;
    if (RAM(0x1C0B)) {
        RAM(0x1C0B) = 0;
        drv_stop_all();
        return;
    }
    for (k = 0; k < 3; k++)
        ym1((uint8_t)(0xB4 + k), RAM(TRACKS + T_SIZE * k + T_PAN));
    for (k = 0; k < 3; k++)
        ym2((uint8_t)(0xB4 + k), RAM(TRACKS + T_SIZE * (k + 3) + T_PAN));
    pan_restore(ym1, 0xB6, 0x1CC0, 0x1E70, 0x1DE0);
    pan_restore(ym2, 0xB5, 0x1D20, 0x1EA0, 0x1E10);
    pan_restore(ym2, 0xB6, 0x1D50, 0x1ED0, 0x1E40);
    sound_done();
}

/* Tempo: every so many frames the music's notes last a frame longer ($11A0). */
static void tempo(void)
{
    int k;
    if (!RAM(0x1C01) || --RAM(0x1C01))
        return;
    RAM(0x1C01) = RAM(0x1C02);
    for (k = 0; k < 6; k++)
        RAM(TRACKS + T_SIZE * k + T_DURATION)++;
}

/* Fade out: every other frame the music gets quieter, until a carrier is
   silent ($10A0). The writes check the track the last update left in IX. */
static void fade(void)
{
    uint16_t t;
    int k;
    if (!RAM(0x1C0B))
        return;
    if (--RAM(0x1C0C))
        return;
    RAM(0x1C0C) = 2;
    if (!--RAM(0x1C0B)) {
        drv_stop_all();
        return;
    }
    for (k = 0, t = TRACKS; k < 6; k++, t += T_SIZE)
        if ((uint8_t)(RAM(t + T_VOLUME) + 1) < 0x7F)
            RAM(t + T_VOLUME)++;
    for (k = 0, t = TRACKS; k < 6; k++, t += T_SIZE) {
        static const int fields[4] = { T_TL1, T_TL2, T_TL3, T_TL4 };
        uint8_t alg, ch;
        int op;
        if (RAM(t + T_CHANNEL) == 2)
            continue;
        alg = RAM(t + T_ALGORITHM) & 7;
        ch = (uint8_t)((RAM(t + T_CHANNEL) & 3) + 0x40);
        for (op = alg == 7 ? 0 : alg >= 5 ? 1 : alg == 4 ? 2 : 3; op < 4; op++) {
            uint8_t v = (uint8_t)(RAM(t + T_VOLUME) + RAM(t + fields[op]));
            if (v >= 0x7F) {
                RAM(0x1C0B) = 0;
                RAM(0x1C0C) = 0;
                drv_stop_all();
                return;
            }
            if (RAM(t + T_CHANNEL) & 4)
                ym2((uint8_t)(ch + 4 * op), v);
            else
                ym1_track((uint8_t)(ch + 4 * op), v);
        }
    }
}

/* While an effect plays on FM5/FM6 the music track stays off it ($11B9,
   $11CC). */
static void effects_hold(void)
{
    if (RAM(0x1E10) & 0x80) {
        if (RAM(0x1EA0) & 0x80)
            RAM(0x1E10) |= 4;
        else
            RAM(0x1D20) |= 4;
    }
    if (RAM(0x1E40) & 0x80) {
        if (RAM(0x1ED0) & 0x80)
            RAM(0x1E40) |= 4;
        else if (RAM(0x1C3A) != 0x90)
            RAM(0x1D50) |= 4;
    }
}

/* One driver tick ($0091). */
void drv_tick(void)
{
    if (RAM(0x1C08) && RAM(0x1C09)) {
        RAM(0x1C08) = 0;
        RAM(0x1C09) = 0;
        unpause();
    }
    if (RAM(0x1C08)) {
        pause();
        return;
    }
    tempo();
    drv_requests();
    fade();
    sound_start();
    effects_hold();
    engine();
    tracks_update();
}

/* The VBlank interrupt ($0038): the startup delay, then (until the
   68000 sets a timer value) a tick per frame. */
void drv_irq(void)
{
    if (RAM(0x1C18)) {
        RAM(0x1C18)--;
        return;
    }
    if (!RAM(0x1C3C))
        drv_tick();
}
