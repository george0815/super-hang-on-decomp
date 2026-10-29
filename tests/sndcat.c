/*
 * FM voice catalogue for the SNES port's sound: plays every sound ID the
 * game uses through the C driver and lists the distinct FM voices keyed
 * on (a channel's operator registers and algorithm, the carriers' total
 * levels left out: they are the volume), with how often and over which
 * pitches each is played.
 *
 * usage: sndcat ROM
 * Prints one line per voice: index, key-ons, lowest and highest pitch (Hz),
 * then the registers ($30-$9C for the four operators, $B0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "z80_rt.h"
#include "genesis.h"
#include "sound.h"

/* no video or pads here */
void host_frame(const uint8_t *pixels) { (void)pixels; }
uint16_t host_pad(int port) { (void)port; return 0; }

static const uint8_t sounds[] = {
    0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A,
    0x90, 0x92, 0x93, 0x94, 0x95, 0x97, 0x9B, 0x9C, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAD, 0xB3,
};
#define NSOUNDS (int)(sizeof sounds / sizeof sounds[0])

#define VOICE_REGS 29   /* 7 registers x 4 operators, then $B0 */

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
    for (op = 0; op < 4; op++)            /* the carriers' levels: volume */
        if (carriers[regs[28] & 7] >> op & 1)
            regs[1 * 4 + op] = 0;
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
    {
        int reg = ym_addr[port >> 1] | (port & 2) << 7;
        ym[reg] = v;
        if (reg == 0x28 && (v & 0xF0))
            key_on(v & 7);
    }
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

int main(int argc, char **argv)
{
    static uint8_t rom[0x80000];
    FILE *f;
    int k, t, r;
    if (argc < 2) {
        fprintf(stderr, "usage: sndcat ROM\n");
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
    sound_write_hook = hook;
    for (k = 0; k < NSOUNDS; k++) {
        int ticks = sounds[k] < 0x90 ? 6000 : 600;
        driver_reset();
        z80_ram[0x1C03] = sounds[k];
        for (t = 0; t < ticks; t++) {
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
    for (k = 0; k < nvoices; k++) {
        printf("%3d %6ld %7.1f %7.1f ", k, voices[k].keyons, voices[k].lo, voices[k].hi);
        for (r = 0; r < VOICE_REGS; r++)
            printf("%02X", voices[k].regs[r]);
        printf("\n");
    }
    fprintf(stderr, "%d voices\n", nvoices);
    return 0;
}
