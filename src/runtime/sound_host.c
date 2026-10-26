/*
 * Runs the sound driver (src/sound/driver.c; the verifying replay runs the
 * original Z80 program next to it, src/runtime/sound_verify.c) and models
 * the Z80's view of the bus: its RAM, the YM2612, the PSG and the bank
 * window into 68000 memory.
 *
 * The driver's own main loop busy-waits, so its three phases are run from
 * here instead:
 *   1. after reset, wait for three VBlank interrupts, then initialise;
 *   2. tick from the VBlank interrupt until the 68000 stores a timer value
 *      at $1C3C;
 *   3. tick on every YM2612 timer B overflow.
 */
#include <stdio.h>
#include <stdlib.h>

#include "m68k_rt.h"
#include "z80_rt.h"
#include "genesis.h"
#include "sound.h"

Z80State z80;

static int phase;          /* 0 = held in reset */
static uint16_t bank;      /* 68000 address bits 15-23 for the $8000 window */
static int timer_b_count;  /* in YM samples */

void (*sound_write_hook)(int chip, int port, uint8_t v);
void (*sound_port_log)(uint16_t addr, uint8_t v);
int sound_ports_muted;      /* log the chip writes but leave the chips alone */

uint16_t sound_bank_get(void) { return bank; }
void sound_bank_set(uint16_t b) { bank = b; }
int sound_external_ticks;   /* replay: timer ticks come from the recording */

void z80_fault(const char *what, uint16_t addr)
{
    fprintf(stderr, "fatal (sound driver): %s ($%04X)\n", what, addr);
    exit(2);
}

long sound_stack_reads;     /* data reads from the stack area ($1F00 on) */

uint8_t z80_read(uint16_t a)
{
    if (a < 0x4000) {
        if ((a & 0x1fff) >= 0x1f00)
            sound_stack_reads++;
        return z80_ram[a & 0x1fff];
    }
    if (a < 0x6000)
        return 0;   /* YM2612 status: never busy, timers handled by the host */
    if (a >= 0x8000)
        return rd8((uint32_t)bank << 15 | (a & 0x7fff));
    return 0xff;
}

static uint8_t ym_addr[2];

void z80_write(uint16_t a, uint8_t v)
{
    if (a < 0x4000) {
        z80_ram[a & 0x1fff] = v;
        return;
    }
    if (sound_port_log)
        sound_port_log(a, v);
    if (a == 0x6000) {
        bank = (uint16_t)((bank >> 1) | (v & 1) << 8);
        return;
    }
    if (sound_ports_muted)
        return;
    if (a < 0x6000) {
        int port = a & 3;
        if (sound_write_hook)
            sound_write_hook(0, port, v);
        if (port & 1)
            ym_write(ym_addr[port >> 1] | (port & 2) << 7, v);
        else
            ym_addr[port >> 1] = v;
    } else if (a == 0x7f11) {
        if (sound_write_hook)
            sound_write_hook(1, 0, v);
        psg_write(v);
    }
}

SoundSettings sound_settings = { 100, { 100, 100, 100 }, FILTER_CONSOLE };

/*
 * Mix groups. The driver keeps 14 tracks of $30 bytes at $1C60: 0-5 are the
 * music's FM parts, 6-7 the PSG tones the engine routine drives from the
 * RPM value, and 8-13 sound effects, which take over a channel from the
 * music while they play. Each output channel gets the volume of whoever
 * owns it right now.
 */
#define TRACKS 0x1c60
#define TRACK_SIZE 0x30

static int track_output(int t)
{
    uint8_t ch = z80_ram[TRACKS + t * TRACK_SIZE + 1];
    if (ch & 0x80)
        return 6 + ((ch >> 5) & 3);      /* PSG 0-3 -> outputs 6-9 */
    return (ch & 3) + ((ch & 4) ? 3 : 0); /* FM 0-5 */
}

static void update_mix(void)
{
    int group[10], i, t;
    for (i = 0; i < 10; i++)
        group[i] = SND_MUSIC;
    for (t = 6; t < 14; t++) {
        int o;
        if (!(z80_ram[TRACKS + t * TRACK_SIZE] & 0x80))
            continue;
        o = track_output(t);
        if (o > 9) continue;
        if (t >= 8)
            group[o] = SND_EFFECTS;
        else if (group[o] != SND_EFFECTS)
            group[o] = SND_ENGINE;
    }
    for (i = 0; i < 10; i++) {
        float g = sound_settings.master * sound_settings.group[group[i]] / 10000.0f;
        if (i < 6) ym_set_gain(i, g);
        else psg_set_gain(i - 6, g);
    }
}

static void call(int entry)
{
    sound_call(entry);
    update_mix();
}

void sound_reset_line(int running)
{
    if (!running) {
        phase = 0;
        return;
    }
    if (phase)
        return;
    /* driver entry: di; im 1; ld sp,$1FFD; ld ($1C18),3 */
    z80.sp = 0x1ffd;
    z80_ram[0x1c18] = 3;
    bank = 0;
    phase = 1;
}

void sound_vblank(void)
{
    switch (phase) {
    case 1:
        call(SND_IRQ);
        if (z80_ram[0x1c18] == 0) {
            call(SND_STOP_ALL);
            call(SND_FM_RESET);
            call(SND_BANK);
            phase = 2;
        }
        break;
    case 2:
        call(SND_REQUESTS);
        call(SND_IRQ);
        if (z80_ram[0x1c3c]) {
            call(SND_TIMER_START);
            timer_b_count = 0;
            phase = 3;
        }
        break;
    }
}

void sound_tick(void)
{
    if (phase != 3)
        return;
    call(SND_REQUESTS);
    call(SND_TIMER_RESET);
    call(SND_TICK);
}

/*
 * Replay against a recording splits the driver's work at the moments the
 * real Z80 reached each part: the interrupt itself (Z), the timer reset (T)
 * and the start of the tick routine (K), which in VBlank mode comes ~55
 * lines after the interrupt because of the delay loop at $0086.
 */
void sound_replay_irq(void)
{
    switch (phase) {
    case 1:
        if (z80_ram[0x1c18]) {
            z80_ram[0x1c18]--;
            if (z80_ram[0x1c18] == 0) {
                call(SND_STOP_ALL);
                call(SND_FM_RESET);
                call(SND_BANK);
                phase = 2;
            }
        }
        break;
    case 2:
        call(SND_REQUESTS);
        if (z80_ram[0x1c3c]) {
            call(SND_TIMER_START);
            phase = 3;
        }
        break;
    }
}

void sound_replay_timer(void)
{
    if (phase != 3)
        return;
    call(SND_REQUESTS);
    call(SND_TIMER_RESET);
}

void sound_replay_tick(void)
{
    if (phase == 2)
        call(SND_REQUESTS);
    call(SND_TICK);
}

void sound_frame(int samples)
{
    while (samples > 0) {
        int n = samples;
        if (phase == 3 && !sound_external_ticks) {
            int period = 16 * (256 - z80_ram[0x1c3c]);
            if (timer_b_count + n >= period) {
                n = period - timer_b_count;
                ym_render(n);
                samples -= n;
                timer_b_count = 0;
                sound_tick();
                continue;
            }
            timer_b_count += n;
        }
        ym_render(n);
        samples -= n;
    }
}
