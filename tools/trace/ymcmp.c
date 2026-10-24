/*
 * Plays a timed YM2612 register log through both the reference core
 * (Genesis Plus GX) and the port's FM synthesizer, and compares the output.
 *
 * usage: ymcmp LOG OUTDIR      (LOG lines: sample port value)
 * writes OUTDIR/ym_ref.wav and OUTDIR/ym_ours.wav (mono, 53267 Hz)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

void YM2612Init(void);
void YM2612ResetChip(void);
void YM2612Write(unsigned int a, unsigned int v);
void YM2612Update(int *buffer, int length);
void YM2612Config(int type);

/* port side (src/sound/ym2612.c) */
void ym_reset(void);
void ym_write(int reg, uint8_t v);
void ym_render(int samples);
extern int16_t sound_buffer[];
extern int sound_buffered;

static void wav_open(FILE *f)
{
    fseek(f, 44, SEEK_SET);
}

static void wav_close(FILE *f, long n)
{
    uint32_t data = n * 2, riff = data + 36, rate = 53267, bps = rate * 2;
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
                      16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &bps, 4); memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    FILE *log, *wr, *wo;
    char path[512];
    unsigned long long t, now = 0;
    int port, val;
    uint8_t addr[2] = { 0, 0 };
    double sxy = 0, sxx = 0, syy = 0;
    long n = 0;
    static int ref[2 * 4096];

    if (argc < 3) { fprintf(stderr, "usage: %s LOG OUTDIR\n", argv[0]); return 1; }
    log = fopen(argv[1], "r");
    if (!log) { perror(argv[1]); return 1; }
    snprintf(path, sizeof path, "%s/ym_ref.wav", argv[2]); wr = fopen(path, "wb");
    snprintf(path, sizeof path, "%s/ym_ours.wav", argv[2]); wo = fopen(path, "wb");
    wav_open(wr); wav_open(wo);
    YM2612Init();
    YM2612Config(0);
    YM2612ResetChip();
    ym_reset();

    for (;;) {
        int more = fscanf(log, "%llu %d %x", &t, &port, &val) == 3;
        unsigned long long until = more ? t : now;
        while (now < until) {
            int k, len = (int)(until - now > 1024 ? 1024 : until - now);
            YM2612Update(ref, len);
            sound_buffered = 0;
            ym_render(len);
            for (k = 0; k < len; k++) {
                double x = (ref[2 * k] + ref[2 * k + 1]) / 2.0;
                double y = (sound_buffer[2 * k] + sound_buffer[2 * k + 1]) / 2.0;
                int16_t sx = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
                int16_t sy = (int16_t)y;
                fwrite(&sx, 2, 1, wr);
                fwrite(&sy, 2, 1, wo);
                sxy += x * y; sxx += x * x; syy += y * y;
                n++;
                if (n % (53267 * 2) == 0) {
                    printf("t=%3lds  corr %.3f  rms ref %6.0f ours %6.0f\n", n / 53267,
                           sxy / sqrt(sxx * syy + 1e-9), sqrt(sxx / (53267 * 2)), sqrt(syy / (53267 * 2)));
                    sxy = sxx = syy = 0;
                }
            }
            now += len;
        }
        if (!more) break;
        YM2612Write(port, val);
        if (port & 1) ym_write(addr[port >> 1] | (port & 2) << 7, (uint8_t)val);
        else addr[port >> 1] = (uint8_t)val;
    }
    wav_close(wr, n);
    wav_close(wo, n);
    return 0;
}
