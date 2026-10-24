/*
 * Square-wave / noise synthesizer with the SN76489 register interface:
 * three tone channels and one noise channel, 4-bit attenuation in 2 dB steps.
 */
#include <math.h>
#include <stdint.h>

#include "sound.h"

#define PSG_CLOCK (3579545.0 / 16.0)

static struct {
    uint16_t period[4];     /* tone periods; [3] is the noise control */
    uint8_t vol[4];
    int latch;
    double count[4];
    int out[4];             /* current polarity (+1 / -1), noise: lfsr bit */
    uint16_t lfsr;
    int16_t level[16];
    int ready;
    float gain_target[4], gain_now[4];
} psg;

static void psg_init(void);

void psg_set_gain(int ch, float gain)
{
    if (!psg.ready) psg_init();
    if (ch >= 0 && ch < 4)
        psg.gain_target[ch] = gain;
}

static void psg_init(void)
{
    int i;
    for (i = 0; i < 15; i++)
        psg.level[i] = (int16_t)(4000.0 * pow(10.0, -0.1 * i));
    psg.level[15] = 0;
    for (i = 0; i < 4; i++) {
        psg.vol[i] = 15;
        psg.out[i] = 1;
        psg.gain_target[i] = psg.gain_now[i] = 1.0f;
    }
    psg.lfsr = 0x8000;
    psg.ready = 1;
}

void psg_write(uint8_t v)
{
    if (!psg.ready) psg_init();
    if (v & 0x80) {
        psg.latch = (v >> 4) & 7;
        if (psg.latch & 1)
            psg.vol[psg.latch >> 1] = v & 15;
        else if ((psg.latch >> 1) == 3) {
            psg.period[3] = v & 7;
            psg.lfsr = 0x8000;
        } else
            psg.period[psg.latch >> 1] = (uint16_t)((psg.period[psg.latch >> 1] & 0x3f0) | (v & 15));
        return;
    }
    if (psg.latch & 1)
        psg.vol[psg.latch >> 1] = v & 15;
    else if ((psg.latch >> 1) != 3)
        psg.period[psg.latch >> 1] = (uint16_t)((psg.period[psg.latch >> 1] & 15) | (v & 0x3f) << 4);
}

void psg_render(int32_t *out, int samples)
{
    const double clocks = PSG_CLOCK / YM_RATE;
    int s, c;
    if (!psg.ready) psg_init();
    for (s = 0; s < samples; s++) {
        int32_t mix = 0;
        for (c = 0; c < 4; c++) {
            int period;
            if (c < 3)
                period = psg.period[c] ? psg.period[c] : 1;
            else {
                int r = psg.period[3] & 3;
                period = r == 3 ? (psg.period[2] ? psg.period[2] : 1) : (0x10 << r);
            }
            psg.count[c] -= clocks;
            while (psg.count[c] <= 0) {
                psg.count[c] += period;
                if (c < 3) {
                    psg.out[c] = -psg.out[c];
                } else {
                    /* white noise taps bits 0 and 3; periodic noise only bit 0 */
                    int fb = (psg.period[3] & 4) ? ((psg.lfsr ^ (psg.lfsr >> 3)) & 1) : (psg.lfsr & 1);
                    psg.lfsr = (uint16_t)((psg.lfsr >> 1) | (fb << 15));
                    psg.out[3] = (psg.lfsr & 1) ? 1 : -1;
                }
            }
            psg.gain_now[c] += (psg.gain_target[c] - psg.gain_now[c]) * 0.002f;
            /* periods 0/1 hold the output high (used for sample playback) */
            if (c < 3 && psg.period[c] <= 1)
                mix += (int32_t)(psg.level[psg.vol[c]] * psg.gain_now[c]);
            else
                mix += (int32_t)(psg.out[c] * psg.level[psg.vol[c]] * psg.gain_now[c]);
        }
        out[s] = mix;
    }
}
