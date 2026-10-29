/*
 * Stand-in for src/runtime/genesis.h in the SNES compile of the sound
 * driver: the Z80 RAM image (snes/sound.s; the driver uses $1C00 on).
 */
#ifndef GENESIS_H
#define GENESIS_H
#include <stdint.h>

extern uint8_t z80_ram[0x2000];
#endif
