/*
 * Entry points for the native build, which runs only the handwritten C:
 * the reset handler, and the VBlank handler as an interrupt (it returns
 * with rte: sr, then the pc gen_take_irq pushed).
 */
#include "m68k_rt.h"
#include "genesis.h"
#include "game.h"
#include "sound.h"
#include "driver.h"

void vblank_entry_68k(void);

void gen_entry_reset(void) { reset(); }

void gen_entry_vblank(void)
{
    vblank_entry_68k();
    cpu.ret = pop32();
}

/* The sound driver's entry points (see sound_host.c) */
void sound_call(int entry)
{
    static void (*const entries[SND_ENTRIES])(void) = {
        drv_irq, drv_tick, drv_requests, drv_stop_all, drv_fm_reset, drv_bank_music,
        drv_timer_start, drv_timer_reset,
    };
    entries[entry]();
}
