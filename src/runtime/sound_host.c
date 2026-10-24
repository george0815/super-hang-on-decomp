/*
 * Drives the recompiled Z80 sound driver (temporary: replaced by the
 * handwritten sequencer in src/sound).
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
#include "z80_funcs.h"
#include "genesis.h"
#include "sound.h"

Z80State z80;

static int phase;          /* 0 = held in reset */
static uint16_t bank;      /* 68000 address bits 15-23 for the $8000 window */
static int timer_b_count;  /* in YM samples */

void (*sound_write_hook)(int chip, int port, uint8_t v);
int sound_external_ticks;   /* replay: timer ticks come from the recording */

void z80_fault(const char *what, uint16_t addr)
{
    fprintf(stderr, "fatal (sound driver): %s ($%04X)\n", what, addr);
    exit(2);
}

uint8_t z80_read(uint16_t a)
{
    if (a < 0x4000)
        return z80_ram[a & 0x1fff];
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
    } else if (a < 0x6000) {
        int port = a & 3;
        if (sound_write_hook)
            sound_write_hook(0, port, v);
        if (port & 1)
            ym_write(ym_addr[port >> 1] | (port & 2) << 7, v);
        else
            ym_addr[port >> 1] = v;
    } else if (a == 0x6000) {
        bank = (uint16_t)((bank >> 1) | (v & 1) << 8);
    } else if (a == 0x7f11) {
        if (sound_write_hook)
            sound_write_hook(1, 0, v);
        psg_write(v);
    }
}

static void call(void (*fn)(void))
{
    z_push(0xffff);
    fn();
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
        call(z_0038);
        if (z80_ram[0x1c18] == 0) {
            call(z_0C37);
            call(z_1070);
            call(z_1015);
            phase = 2;
        }
        break;
    case 2:
        call(z_11E8);
        call(z_0038);
        if (z80_ram[0x1c3c]) {
            call(z_001F);
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
    call(z_11E8);
    call(z_0027);
    call(z_0091);
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
                call(z_0C37);
                call(z_1070);
                call(z_1015);
                phase = 2;
            }
        }
        break;
    case 2:
        call(z_11E8);
        if (z80_ram[0x1c3c]) {
            call(z_001F);
            phase = 3;
        }
        break;
    }
}

void sound_replay_timer(void)
{
    if (phase != 3)
        return;
    call(z_11E8);
    call(z_0027);
}

void sound_replay_tick(void)
{
    if (phase == 2)
        call(z_11E8);
    call(z_0091);
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
