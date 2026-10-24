/*
 * FM synthesizer with the YM2612's voice architecture: 6 channels of 4
 * operators, 8 algorithms, ADSR envelopes, LFO, stereo panning and the
 * channel 6 sample DAC. Parameters are set through the chip's register
 * layout so the music driver can program it directly.
 *
 * Waveforms are produced the way the chip does it: a quarter-wave log-sine
 * table plus attenuation, converted back to linear with an exponent table.
 */
#include <math.h>
#include <string.h>

#include "sound.h"

int16_t sound_buffer[2 * 2048];
int sound_buffered;
uint64_t ym_samples;   /* total samples rendered (for timestamps in tests) */

/* The console's analog output stage rounds off the highs; a one-pole
   low-pass gives the same softened sound (console: ~4.3 kHz at 53 kHz). */
static const float lowpass_keep[FILTER_MODES] = { 0.0f, 0.6f, 0.75f };
static float lp_left, lp_right;

/* per-channel volume from the mix settings, ramped to avoid clicks */
static float gain_target[6] = { 1, 1, 1, 1, 1, 1 }, gain_now[6] = { 1, 1, 1, 1, 1, 1 };

void ym_set_gain(int ch, float gain)
{
    if (ch >= 0 && ch < 6)
        gain_target[ch] = gain;
}

void psg_render(int32_t *out, int samples);

/* --------------------------------------------------------------- tables */

static uint16_t logsin[256];   /* -log2(sin) in 1/256 units, quarter wave */
static uint16_t exptab[256];   /* 2^(-x/256), 10-bit fraction */
static int tables_ready;

/* frequency offset by detune setting (1-3) and key code (0-31), per the
   datasheet; detune 4-7 subtract the same amounts */
static const uint8_t detune[4][32] = {
    { 0 },
    { 0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7 },
    { 1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16 },
    { 2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22 },
};

/* envelope step patterns: 8 steps per rate group */
static const uint8_t eg_steps[19][8] = {
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1}, {1,1,1,2,1,1,1,2}, {1,2,1,2,1,2,1,2}, {1,2,2,2,1,2,2,2},
    {2,2,2,2,2,2,2,2}, {2,2,2,4,2,2,2,4}, {2,4,2,4,2,4,2,4}, {2,4,4,4,2,4,4,4},
    {4,4,4,4,4,4,4,4}, {4,4,4,8,4,4,4,8}, {4,8,4,8,4,8,4,8}, {4,8,8,8,4,8,8,8},
    {8,8,8,8,8,8,8,8}, {0,0,0,0,0,0,0,0}, {16,16,16,16,16,16,16,16},
};

/* samples per LFO step, by frequency setting */
static const uint8_t lfo_period[8] = { 108, 77, 71, 67, 62, 44, 8, 5 };
/* vibrato depth in cents by PMS */
static const float pm_cents[8] = { 0, 3.4f, 6.7f, 10, 14, 20, 40, 80 };
/* tremolo shift by AMS (attenuation = lfo >> shift) */
static const uint8_t am_shift[4] = { 8, 3, 1, 0 };

static void init_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        double s = sin((i + 0.5) * M_PI / 512.0);
        logsin[i] = (uint16_t)lround(-log2(s) * 256.0);
        exptab[i] = (uint16_t)lround((pow(2.0, i / 256.0) - 1.0) * 1024.0);
    }
    tables_ready = 1;
}

/* ---------------------------------------------------------------- state */

enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

typedef struct {
    /* registers */
    uint8_t dt, mul, tl, ks, ar, am, dr, sr, sl, rr;
    /* runtime */
    uint32_t phase;
    uint32_t inc;
    int eg_state;
    int att;          /* 0 (loud) .. 1023 (silent) */
    int key;
    int32_t out;
} Op;

typedef struct {
    Op op[4];
    uint16_t fnum;
    uint8_t block, fnum_hi;
    uint8_t algo, fb, ams, pms, pan;
    int32_t fb_hist[2];
    uint16_t op_fnum[3];    /* channel 3 special mode */
    uint8_t op_block[3], op_hi;
} Channel;

static struct {
    Channel ch[6];
    uint8_t ch3_mode;
    uint8_t lfo_on, lfo_freq;
    int lfo_count, lfo_step;
    uint32_t eg_timer, eg_counter;
    uint8_t dac_on, dac;
} ym;

/* register slot order inside a channel: S1, S3, S2, S4 */
static const int slot_map[4] = { 0, 2, 1, 3 };

void ym_reset(void)
{
    int c, o;
    if (!tables_ready) init_tables();
    memset(&ym, 0, sizeof ym);
    for (c = 0; c < 6; c++) {
        ym.ch[c].pan = 3;
        for (o = 0; o < 4; o++) {
            ym.ch[c].op[o].att = 1023;
            ym.ch[c].op[o].eg_state = EG_RELEASE;
        }
    }
}

static int keycode(uint16_t fnum, uint8_t block)
{
    int f11 = (fnum >> 10) & 1, f10 = (fnum >> 9) & 1, f9 = (fnum >> 8) & 1, f8 = (fnum >> 7) & 1;
    int n3 = (f11 && (f10 || f9 || f8)) || (!f11 && f10 && f9 && f8);
    return block << 2 | f11 << 1 | n3;
}

static void update_freq(Channel *c, int ci)
{
    int o;
    for (o = 0; o < 4; o++) {
        Op *op = &c->op[o];
        uint16_t fnum = c->fnum;
        uint8_t block = c->block;
        int kc, d;
        uint32_t inc;
        /* channel 3 special mode: operators 1-3 have their own frequency */
        if (ci == 2 && (ym.ch3_mode & 0x40) && o < 3) {
            static const int idx[3] = { 1, 2, 0 };   /* S1: A9/AD, S3: A8/AC, S2: AA/AE */
            fnum = c->op_fnum[idx[o]];
            block = c->op_block[idx[o]];
        }
        kc = keycode(fnum, block);
        inc = ((uint32_t)fnum << block) >> 1;
        d = detune[op->dt & 3][kc];
        if (op->dt & 4) inc -= d; else inc += d;
        inc &= 0x1ffff;
        op->inc = op->mul ? inc * op->mul : inc >> 1;
    }
}

static int eg_rate(Op *op, Channel *c, int r)
{
    int rate, kc;
    if (r == 0) return 0;
    kc = keycode(c->fnum, c->block);
    rate = 2 * r + (kc >> (3 - op->ks));
    return rate > 63 ? 63 : rate;
}

static void key_on(Channel *c, int o)
{
    Op *op = &c->op[o];
    if (op->key) return;
    op->key = 1;
    op->phase = 0;
    op->eg_state = EG_ATTACK;
    if (eg_rate(op, c, op->ar) >= 62)
        op->att = 0;
}

static void key_off(Op *op)
{
    if (!op->key) return;
    op->key = 0;
    op->eg_state = EG_RELEASE;
}

void ym_write(int reg, uint8_t v)
{
    int part = reg >> 8, r = reg & 0xff;
    if (!tables_ready) ym_reset();

    if (part == 0 && r < 0x30) {
        switch (r) {
        case 0x22:
            ym.lfo_on = (v >> 3) & 1;
            ym.lfo_freq = v & 7;
            if (!ym.lfo_on) ym.lfo_step = 0;
            break;
        case 0x27:
            ym.ch3_mode = v & 0xc0;
            update_freq(&ym.ch[2], 2);
            break;
        case 0x28: {
            int ci = v & 3, o;
            if (ci == 3) break;
            if (v & 4) ci += 3;
            for (o = 0; o < 4; o++) {
                if (v & (0x10 << o)) key_on(&ym.ch[ci], o);
                else key_off(&ym.ch[ci].op[o]);
            }
            break;
        }
        case 0x2a: ym.dac = v; break;
        case 0x2b: ym.dac_on = v >> 7; break;
        }
        return;
    }
    if (r < 0x30 || (r & 3) == 3)
        return;
    {
        int ci = (r & 3) + part * 3;
        Channel *c = &ym.ch[ci];
        Op *op = &c->op[slot_map[(r >> 2) & 3]];
        switch (r & 0xf0) {
        case 0x30: op->dt = (v >> 4) & 7; op->mul = v & 15; update_freq(c, ci); break;
        case 0x40: op->tl = v & 0x7f; break;
        case 0x50: op->ks = v >> 6; op->ar = v & 31; break;
        case 0x60: op->am = v >> 7; op->dr = v & 31; break;
        case 0x70: op->sr = v & 31; break;
        case 0x80: op->sl = v >> 4; op->rr = v & 15; break;
        case 0x90: break;  /* SSG-EG: unused by this game */
        case 0xa0:
            switch (r & 0xfc) {
            case 0xa0: c->fnum = (uint16_t)((c->fnum_hi & 7) << 8 | v); c->block = (c->fnum_hi >> 3) & 7; update_freq(c, ci); break;
            case 0xa4: c->fnum_hi = v; break;
            case 0xa8:
                if (part == 0) {
                    int k = r & 3;
                    c = &ym.ch[2];
                    c->op_fnum[k] = (uint16_t)((c->op_hi & 7) << 8 | v);
                    c->op_block[k] = (c->op_hi >> 3) & 7;
                    update_freq(c, 2);
                }
                break;
            case 0xac: if (part == 0) ym.ch[2].op_hi = v; break;
            }
            break;
        case 0xb0:
            if ((r & 0xfc) == 0xb0) { c->algo = v & 7; c->fb = (v >> 3) & 7; }
            else if ((r & 0xfc) == 0xb4) { c->pan = v >> 6; c->ams = (v >> 4) & 3; c->pms = v & 7; }
            break;
        }
    }
}

/* ------------------------------------------------------------ envelopes */

static void eg_step(Channel *c, Op *op)
{
    int rate, shift, step;
    switch (op->eg_state) {
    case EG_ATTACK:  rate = eg_rate(op, c, op->ar); break;
    case EG_DECAY:   rate = eg_rate(op, c, op->dr); break;
    case EG_SUSTAIN: rate = eg_rate(op, c, op->sr); break;
    default:         rate = eg_rate(op, c, op->rr * 2 + 1); break;
    }
    if (rate < 2) return;
    shift = 11 - (rate >> 2);
    if (shift < 0) shift = 0;
    if (ym.eg_counter & ((1u << shift) - 1)) return;
    {
        int group = rate < 48 ? (rate & 3) : rate < 60 ? 4 + (rate - 48) : 16;
        step = eg_steps[group][(ym.eg_counter >> shift) & 7];
    }
    if (op->eg_state == EG_ATTACK) {
        if (rate >= 62) op->att = 0;
        else op->att += (~op->att * step) >> 4;
        if (op->att <= 0) {
            op->att = 0;
            op->eg_state = EG_DECAY;
        }
        return;
    }
    op->att += step;
    if (op->att > 1023) op->att = 1023;
    if (op->eg_state == EG_DECAY) {
        int sl = op->sl == 15 ? 1023 : op->sl << 5;
        if (op->att >= sl) op->eg_state = EG_SUSTAIN;
    }
}

/* --------------------------------------------------------------- output */

static int32_t op_out(Op *op, uint32_t phase_mod, int am_att)
{
    uint32_t p = ((op->phase >> 10) + phase_mod) & 0x3ff;
    int att = op->att + (op->tl << 3) + (op->am ? am_att : 0);
    int x, v;
    if (att > 1023) att = 1023;
    x = logsin[(p & 0x100) ? (~p & 0xff) : (p & 0xff)] + (att << 2);
    if (x >= 13 * 256)
        return 0;
    v = ((exptab[(x & 0xff) ^ 0xff] | 0x400) << 2) >> (x >> 8);
    return (p & 0x200) ? -v : v;
}

static int32_t channel_out(Channel *c, int am)
{
    Op *o = c->op;
    int32_t m1, out = 0;
    int am_att = am >> am_shift[c->ams];
    int32_t fbm = c->fb ? (c->fb_hist[0] + c->fb_hist[1]) >> (10 - c->fb) : 0;

    m1 = op_out(&o[0], (uint32_t)fbm, am_att);
    c->fb_hist[1] = c->fb_hist[0];
    c->fb_hist[0] = m1;

#define OP(n, mod) op_out(&o[n], (uint32_t)((mod) >> 1), am_att)
    switch (c->algo) {
    case 0: out = OP(3, OP(2, OP(1, m1))); break;
    case 1: out = OP(3, OP(2, OP(1, 0) + m1)); break;
    case 2: out = OP(3, OP(2, OP(1, 0)) + m1); break;
    case 3: out = OP(3, OP(2, 0) + OP(1, m1)); break;
    case 4: out = OP(1, m1) + OP(3, OP(2, 0)); break;
    case 5: out = OP(1, m1) + OP(2, m1) + OP(3, m1); break;
    case 6: out = OP(1, m1) + OP(2, 0) + OP(3, 0); break;
    case 7: out = m1 + OP(1, 0) + OP(2, 0) + OP(3, 0); break;
    }
#undef OP
    if (out > 8191) out = 8191;
    if (out < -8192) out = -8192;
    return out;
}

void ym_render(int samples)
{
    static int32_t psg[2048];
    int s, ci, o;
    if (!tables_ready) ym_reset();
    if (sound_buffered + samples > 2048)
        samples = 2048 - sound_buffered;
    psg_render(psg, samples);

    for (s = 0; s < samples; s++) {
        int32_t left = 0, right = 0;
        int am;

        /* LFO: 7-bit triangle for tremolo, sine-ish for vibrato */
        if (ym.lfo_on && ++ym.lfo_count >= lfo_period[ym.lfo_freq]) {
            ym.lfo_count = 0;
            ym.lfo_step = (ym.lfo_step + 1) & 127;
        }
        am = ym.lfo_step < 64 ? ym.lfo_step * 2 : (127 - ym.lfo_step) * 2;

        /* envelope generator runs at 1/3 of the sample rate */
        if (++ym.eg_timer >= 3) {
            ym.eg_timer = 0;
            ym.eg_counter++;
            for (ci = 0; ci < 6; ci++)
                for (o = 0; o < 4; o++)
                    eg_step(&ym.ch[ci], &ym.ch[ci].op[o]);
        }

        for (ci = 0; ci < 6; ci++) {
            Channel *c = &ym.ch[ci];
            int32_t v;
            if (ci == 5 && ym.dac_on) {
                v = ((int)ym.dac - 128) << 6;
            } else {
                v = channel_out(c, am);
            }
            gain_now[ci] += (gain_target[ci] - gain_now[ci]) * 0.002f;
            v = (int32_t)(v * gain_now[ci]);
            if (c->pan & 2) left += v;
            if (c->pan & 1) right += v;
            for (o = 0; o < 4; o++) {
                uint32_t inc = c->op[o].inc;
                if (c->pms && ym.lfo_on) {
                    float tri = (float)(ym.lfo_step < 64 ? ym.lfo_step - 32 : 95 - ym.lfo_step) / 32.0f;
                    inc = (uint32_t)(inc * powf(2.0f, pm_cents[c->pms] * tri / 1200.0f));
                }
                c->op[o].phase += inc;
            }
        }
        /* FM:PSG balance matched to the reference mix (PSG full scale ~4000
           per channel, FM channel sum x1.2) */
        left = left * 6 / 5 + psg[s];
        right = right * 6 / 5 + psg[s];
        {
            float k = lowpass_keep[sound_settings.filter % FILTER_MODES];
            lp_left = lp_left * k + left * (1.0f - k);
            lp_right = lp_right * k + right * (1.0f - k);
        }
        left = (int32_t)lp_left;
        right = (int32_t)lp_right;
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;
        sound_buffer[2 * sound_buffered] = (int16_t)left;
        sound_buffer[2 * sound_buffered + 1] = (int16_t)right;
        sound_buffered++;
        ym_samples++;
    }
}
