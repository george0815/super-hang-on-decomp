/*
 * Stand-in for src/runtime/z80_rt.h in the SNES compile of the sound
 * driver (src/sound/driver.c, tools/snes/c816.py): the Z80's memory
 * accessors, which the SNES implements (snes/sound.s).
 */
#ifndef Z80_RT_H
#define Z80_RT_H
#include <stdint.h>

uint8_t z80_read(uint16_t addr);
void z80_write(uint16_t addr, uint8_t v);
void z80_fault_at(uint16_t addr);
/* (no strings in the SNES compile: the message goes) */
#define z80_fault(what, addr) z80_fault_at(addr)
#endif
