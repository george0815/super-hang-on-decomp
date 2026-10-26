/*
 * Entry points for the native build, which runs only the handwritten C:
 * the reset handler, and the VBlank handler as an interrupt (it returns
 * with rte: sr, then the pc gen_take_irq pushed).
 */
#include "m68k_rt.h"
#include "genesis.h"
#include "game.h"

void vblank_entry_68k(void);

void gen_entry_reset(void) { reset(); }

void gen_entry_vblank(void)
{
    vblank_entry_68k();
    cpu.ret = pop32();
}
