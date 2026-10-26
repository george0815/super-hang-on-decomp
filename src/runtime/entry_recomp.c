/*
 * Entry points for builds that keep the recompiled originals (replay
 * with verification, fntest): reset and VBlank go through the generated
 * wrappers, which check the handwritten C against the original.
 */
#include "m68k_rt.h"
#include "genesis.h"
#include "sho_funcs.h"

void gen_entry_reset(void) { m68k_dispatch(rd32(4)); }
void gen_entry_vblank(void) { f_006D2A(); }
