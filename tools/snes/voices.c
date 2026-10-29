/*
 * The SNES port's FM voices (tools/snes/sound.py packs them): plays every
 * sound ID the game uses through the C sound driver, collects the FM
 * voices keyed on (a channel's operator registers and algorithm, the
 * carriers' total levels left out), and renders each with the YM2612
 * emulation:
 *  - a sample: the voice with its carriers held at full level, at a
 *    reference pitch of 500 x n Hz (n so the voice's highest note is
 *    within the DSP's 4x), resampled so that 64 samples at 32 kHz hold n
 *    periods: an attack of ATTACK samples, then a 64-sample loop;
 *  - its envelope: the voice as played (a key on, a key off after a
 *    second) measured and fitted with the DSP's ADSR, and the release with
 *    a GAIN exponential decrease.
 *
 * usage: voices ROM OUT
 * OUT: per voice a line "V hash n adsr1 adsr2 release lo hi keyons", then
 * "S" and its 16-bit samples.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "z80_rt.h"
#include "genesis.h"
#include "sound.h"

void host_frame(const uint8_t *pixels) { (void)pixels; }
uint16_t host_pad(int port) { (void)port; return 0; }

static const uint8_t sounds[] = {
    0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A,
    0x90, 0x92, 0x93, 0x94, 0x95, 0x97, 0x9B, 0x9C, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAD, 0xB3,
};
#define NSOUNDS (int)(sizeof sounds / sizeof sounds[0])

#define VOICE_REGS 29   /* 7 registers x 4 operators, then $B0 & $3F */
#define ATTACK     512  /* samples before the loop */
#define LOOP       64
#define OUT_RATE   32000.0

typedef struct {
    uint8_t regs[VOICE_REGS];
    long keyons;
    double lo, hi;
} Voice;

static Voice voices[256];
static int nvoices;
static uint8_t ym[0x200];
static int ym_addr[2];

/* the carriers of each algorithm: operator slots (+0 op1, +4 op3, +8 op2,
   +12 op4) as bits */
static const uint8_t carriers[8] = { 8, 8, 8, 8, 10, 14, 14, 15 };

/* The voice's hash, as snes/sound.c computes it at a key on. */
static unsigned voice_hash(const uint8_t *regs)
{
    unsigned h = 0;
    for (int k = 0; k < VOICE_REGS; k++)
        h = ((h << 3 | h >> 13) & 0xFFFF) ^ regs[k];
    return h;
}

/* ------------------------------------------------------------ catalogue */

static double pitch(int ch)
{
    int base = ch < 3 ? ch : 0x100 + ch - 4;
    int fnum = (ym[base + 0xA4] & 7) << 8 | ym[base + 0xA0];
    int block = ym[base + 0xA4] >> 3 & 7;
    return fnum * (double)YM_RATE * (1 << block) / (1 << 21);
}

static void key_on(int ch)
{
    int base = ch < 3 ? ch : 0x100 + ch - 4, r, op, k;
    uint8_t regs[VOICE_REGS];
    double hz = pitch(ch);
    for (r = 0; r < 7; r++)
        for (op = 0; op < 4; op++)
            regs[r * 4 + op] = ym[base + 0x30 + r * 0x10 + op * 4];
    regs[28] = ym[base + 0xB0] & 0x3F;
    for (op = 0; op < 4; op++)
        if (carriers[regs[28] & 7] >> op & 1)
            regs[4 + op] = 0;
    for (k = 0; k < nvoices; k++)
        if (!memcmp(voices[k].regs, regs, VOICE_REGS))
            break;
    if (k == nvoices) {
        if (nvoices == 256)
            return;
        memcpy(voices[k].regs, regs, VOICE_REGS);
        voices[k].lo = voices[k].hi = hz;
        nvoices++;
    }
    voices[k].keyons++;
    if (hz < voices[k].lo) voices[k].lo = hz;
    if (hz > voices[k].hi) voices[k].hi = hz;
}

static void hook(int chip, int port, uint8_t v)
{
    if (chip)
        return;
    if (!(port & 1)) {
        ym_addr[port >> 1] = v;
        return;
    }
    int reg = ym_addr[port >> 1] | (port & 2) << 7;
    ym[reg] = v;
    if (reg == 0x28 && (v & 0xF0))
        key_on(v & 7);
}

static void driver_reset(void)
{
    memset(z80_ram + 0x1A24, 0, 0x2000 - 0x1A24);
    memset(&z80, 0, sizeof z80);
    z80.sp = 0x1FFD;
    sound_call(SND_STOP_ALL);
    sound_call(SND_FM_RESET);
    sound_call(SND_BANK);
}

static void catalogue(void)
{
    sound_write_hook = hook;
    for (int k = 0; k < NSOUNDS; k++) {
        int ticks = sounds[k] < 0x90 ? 6000 : 600;
        driver_reset();
        z80_ram[0x1C03] = sounds[k];
        for (int t = 0; t < ticks; t++) {
            if (sounds[k] == 0xB3) {          /* the engine: sweep the RPM */
                uint16_t rpm = (uint16_t)(t * 0x1C0 / ticks);
                z80_ram[0x1C31] = (uint8_t)rpm;
                z80_ram[0x1C32] = (uint8_t)(rpm >> 8);
            }
            sound_call(SND_REQUESTS);
            if (t % 7 == 0)
                sound_call(SND_TIMER_RESET);
            sound_call(SND_TICK);
        }
    }
    sound_write_hook = NULL;
}

/* ------------------------------------------------------------ rendering */

static float *render_buf;
static int render_len;

/* The voice on FM channel 1 at hz, keyed on for on samples then off, n
   samples in all (YM rate), into render_buf (left channel). held: the
   carriers never decay. */
static void render(const Voice *v, double hz, int on, int n, int held)
{
    int block, fnum = 0, r, op, k;
    ym_reset();
    sound_settings.filter = FILTER_OFF;
    ym_set_gain(0, 1.0f);
    for (block = 0; block < 8; block++) {
        fnum = (int)lround(hz * (1 << 21) / (YM_RATE * (double)(1 << block)));
        if (fnum < 2048)
            break;
    }
    for (r = 0; r < 7; r++)
        for (op = 0; op < 4; op++) {
            uint8_t x = v->regs[r * 4 + op];
            int carrier = carriers[v->regs[28] & 7] >> op & 1;
            if (held && carrier) {
                if (r == 3) x &= 0xE0;          /* D1R 0 */
                if (r == 4) x = 0;              /* D2R 0 */
                if (r == 5) x &= 0x0F;          /* SL 0 */
            }
            ym_write(0x30 + r * 0x10 + op * 4, x);
        }
    ym_write(0xB0, v->regs[28]);
    ym_write(0xB4, 0xC0);
    ym_write(0xA4, (uint8_t)(block << 3 | fnum >> 8));
    ym_write(0xA0, (uint8_t)fnum);
    ym_write(0x28, 0xF0);
    free(render_buf);
    render_buf = malloc(sizeof *render_buf * (size_t)n);
    render_len = n;
    for (k = 0; k < n; ) {
        int m = n - k < 1024 ? n - k : 1024;
        if (k <= on && k + m > on) {
            if (on > k) m = on - k;
            else ym_write(0x28, 0x00);
        }
        sound_buffered = 0;
        ym_render(m);
        for (int s = 0; s < m; s++)
            render_buf[k + s] = sound_buffer[2 * s];
        k += m;
    }
}

/* the frequency render() actually plays for hz */
static double played(double hz)
{
    for (int block = 0; block < 8; block++) {
        int fnum = (int)lround(hz * (1 << 21) / (YM_RATE * (double)(1 << block)));
        if (fnum < 2048)
            return fnum * (double)YM_RATE * (1 << block) / (1 << 21);
    }
    return hz;
}

/* render_buf at position x (in samples), by cubic interpolation */
static double at(double x)
{
    int i = (int)floor(x);
    double t = x - i, p[4];
    for (int k = 0; k < 4; k++) {
        int j = i - 1 + k;
        p[k] = j >= 0 && j < render_len ? render_buf[j] : 0;
    }
    return p[1] + 0.5 * t * (p[2] - p[0] + t * (2 * p[0] - 5 * p[1] + 4 * p[2] - p[3]
                                               + t * (3 * (p[1] - p[2]) + p[3] - p[0])));
}

/* ------------------------------------------------------------ envelopes */

static const int dsp_rate[32] = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1,
};

#define ENV_STEP 64     /* envelope points: every 2 ms at 32 kHz */
#define ENV_ON   500    /* 1 s keyed on */
#define ENV_ALL  750    /* then half a second released */

/* The DSP's ADSR (key on for ENV_ON points) into env (0-1 a point). */
static void dsp_adsr(int ar, int dr, int sl, int sr, float *env)
{
    int e = 0, mode = 0, count = 0;
    for (int p = 0; p < ENV_ON; p++) {
        for (int k = 0; k < ENV_STEP; k++) {
            int rate = mode == 0 ? ar * 2 + 1 : mode == 1 ? dr * 2 + 16 : sr;
            if (rate == 0)
                continue;
            if (++count < dsp_rate[rate])
                continue;
            count = 0;
            if (mode == 0) {
                e += rate == 31 ? 1024 : 32;
                if (e >= 0x7E0) { e = 0x7FF; mode = 1; }
            } else {
                e -= ((e - 1) >> 8) + 1;
                if (e < 0) e = 0;
                if (mode == 1 && (e >> 8) == sl)
                    mode = 2;
            }
        }
        env[p] = e / 2047.0f;
    }
}

static double db(double x) { return x < 1e-3 ? -60 : 20 * log10(x); }

/* The voice's envelope as played at hz: its level every ENV_STEP output
   samples, key on, then off at ENV_ON. */
static void measure(const Voice *v, double hz, float *env)
{
    double k = YM_RATE / OUT_RATE, peak = 1e-9;
    int on = (int)(ENV_ON * ENV_STEP * k), n = (int)(ENV_ALL * ENV_STEP * k);
    render(v, hz, on, n, 0);
    for (int p = 0; p < ENV_ALL; p++) {
        int a = (int)(p * ENV_STEP * k), b = (int)((p + 1) * ENV_STEP * k);
        double m = 0;
        for (int i = a; i < b && i < render_len; i++)
            if (fabs(render_buf[i]) > m) m = fabs(render_buf[i]);
        env[p] = (float)m;
        if (m > peak) peak = m;
    }
    for (int p = 0; p < ENV_ALL; p++)
        env[p] = (float)(env[p] / peak);
}

/* ADSR registers fitting env's key-on part, and the GAIN exponential
   decrease rate fitting its release. */
static void fit(const float *env, int *adsr1, int *adsr2, int *release)
{
    static float sim[ENV_ON];
    double best = 1e30;
    int ar = 15, dr, sl, sr, bdr = 7, bsl = 7, bsr = 0;
    /* attack: the first point within 3 dB of the peak */
    {
        int p = 0;
        while (p < ENV_ON && env[p] < 0.7f) p++;
        double ms = p * 2.0;
        static const double ar_ms[16] = { 4100, 2600, 1500, 1000, 640, 380, 260, 160,
                                          96, 64, 40, 24, 16, 10, 6, 0 };
        for (ar = 15; ar > 0 && ar_ms[ar] < ms * 0.7; ar--) ;
    }
    for (dr = 0; dr < 8; dr++)
        for (sl = 0; sl < 8; sl++)
            for (sr = 0; sr < 32; sr++) {
                double err = 0;
                dsp_adsr(ar, dr, sl, sr, sim);
                for (int p = 0; p < ENV_ON; p += 2) {
                    double d = db(env[p]) - db(sim[p]);
                    err += d * d;
                }
                if (err < best) { best = err; bdr = dr; bsl = sl; bsr = sr; }
            }
    *adsr1 = 0x80 | bdr << 4 | ar;
    *adsr2 = bsl << 5 | bsr;
    /* release: from the level at the key off, GAIN mode 101 */
    {
        double best_r = 1e30;
        int r;
        *release = 31;
        for (r = 1; r < 32; r++) {
            double e = env[ENV_ON - 1] * 2047, err = 0;
            int count = 0;
            for (int p = ENV_ON; p < ENV_ALL; p++) {
                for (int k = 0; k < ENV_STEP; k++)
                    if (++count >= dsp_rate[r]) {
                        count = 0;
                        e -= (((int)e - 1) >> 8) + 1;
                        if (e < 0) e = 0;
                    }
                double d = db(env[p]) - db(e / 2047);
                err += d * d;
            }
            if (err < best_r) { best_r = err; *release = r; }
        }
    }
}

int main(int argc, char **argv)
{
    static uint8_t rom[0x80000];
    static float env[ENV_ALL];
    FILE *f, *out;
    if (argc < 3) {
        fprintf(stderr, "usage: voices ROM OUT\n");
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (!f || fread(rom, 1, sizeof rom, f) != sizeof rom) {
        perror(argv[1]);
        return 1;
    }
    fclose(f);
    gen_init(rom, sizeof rom);
    memcpy(z80_ram, rom + 0x7D7F4, 0x1A24);
    catalogue();
    out = fopen(argv[2], "w");
    if (!out) { perror(argv[2]); return 1; }
    for (int v = 0; v < nvoices; v++) {
        const Voice *vo = &voices[v];
        int n = 1, adsr1, adsr2, release;
        while (500.0 * n * 3.9 < vo->hi && n < 8)
            n++;
        double ref = 500.0 * n, hz = played(ref);
        /* the envelope, played at the middle of its range */
        measure(vo, sqrt(vo->lo * vo->hi), env);
        fit(env, &adsr1, &adsr2, &release);
        /* the sample: n periods of hz in LOOP output samples */
        double step = (YM_RATE / hz) * n / LOOP;
        render(vo, hz, (int)((ATTACK + LOOP + 8) * step) + 64, (int)((ATTACK + LOOP + 8) * step) + 64, 1);
        double peak = 1e-9;
        static double pcm[ATTACK + LOOP];
        for (int j = 0; j < ATTACK + LOOP; j++) {
            pcm[j] = at(j * step);
            if (fabs(pcm[j]) > peak) peak = fabs(pcm[j]);
        }
        fprintf(out, "V %04X %d %02X %02X %d %.1f %.1f %ld\nS", voice_hash(vo->regs), n,
                adsr1, adsr2, release, vo->lo, vo->hi, vo->keyons);
        for (int j = 0; j < ATTACK + LOOP; j++)
            fprintf(out, " %d", (int)lround(pcm[j] / peak * 30000));
        fprintf(out, "\n");
    }
    fclose(out);
    fprintf(stderr, "%d voices\n", nvoices);
    return 0;
}
