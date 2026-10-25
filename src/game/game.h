/*
 * Handwritten Super Hang-On game code: shared declarations.
 *
 * During the transition the handwritten code works on the original 68000
 * work RAM image (m68k_ram, big-endian) through the rd/wr accessors, so it
 * can run alongside the recompiled code and be verified against it call by
 * call. Functions ending in _68k are adapters to the original register
 * interface for recompiled callers; they go away as callers are rewritten.
 */
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#include "m68k_rt.h"

/* ------------------------------------------------------- work RAM map */

#define VDP_MODE_SHADOW   0xFFC700  /* copies of the VDP mode register commands
                                       $80xx/$81xx (reg 1 byte at $FFC703) */
#define SOUND_LOCKED      0xFFC754  /* nonzero: don't send sound commands */
#define OBJ_LAYER_LISTS   0xFFE000  /* per-layer object lists, $20 bytes each,
                                       growing down from here (see obj_add_to_layer) */
#define RNG_SEED          0xFF0502  /* 32-bit random generator state */

/* --------------------------------------------------------- hardware */

#define VDP_DATA          0xC00000
#define VDP_CTRL          0xC00004
#define Z80_BUSREQ        0xA11100
#define Z80_SOUND_COMMAND 0xA01C03  /* sound driver mailbox: command byte */

/* VDP command longwords (address + access code) */
#define VDP_CMD_CRAM_WRITE(addr)  (0xC0000000u | ((uint32_t)(addr) & 0x3fff) << 16 | (uint32_t)(addr) >> 14)

/* ------------------------------------------------------------ ROM data
   Fixed-address ROM tables; the asset extractor will turn these into
   named data files. */
#define ROM_VDP_REGISTER_DEFAULTS 0x0086A0  /* 19 register-write words */

/* ------------------------------------------------------------ video.c */
void vdp_init_registers(void);
void vdp_fill(uint32_t command, uint16_t value, int count);
void vdp_copy(uint32_t command, uint32_t src, int count);
void vdp_copy_rect(uint32_t command, uint32_t src, int width, int height);

/* ---------------------------------------------------------- nemesis.c */
void nemesis_to_vdp(uint32_t src);
void nemesis_to_ram(uint32_t src, uint32_t dst);

/* ----------------------------------------------------------- system.c */
uint16_t rng_next(void);

/* ------------------------------------------------------------ audio.c */
void sound_command(uint8_t command);

/* ----------------------------------------------------------- vblank.c */
void vblank_menu(void);
void vblank_race(void);
uint16_t char_tile(uint8_t c);

/* ------------------------------------------------------------- race.c */
void race_place_objects(void);

/* ---------------------------------------------------------- objects.c */
void obj_add_to_layer(uint32_t obj);
void objects_update(void);

#endif
