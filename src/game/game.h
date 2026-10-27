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

/* A game mode's first call ($C705 bit 0 clear): sets the bit, so the
   mode's setup runs once. */
static inline int mode_first_call(void)
{
    uint8_t v = rd8(0xFFC705);
    wr8(0xFFC705, v | 1);
    return !(v & 1);
}

/* The end of most game modes' setup: interrupts on with the given VBlank
   handler, then after the next VBlank the display on. */
void mode_setup_done(uint16_t vblank_handler);

/* The game modes ($6CF2 on, by mode / 4) */
void mode_logo(void);
void mode_title(void);
void mode_course_select(void);
void mode_music_select(void);
void mode_race_start(void);
void mode_demo(void);
void mode_race(void);
void mode_goal(void);
void mode_ranking(void);
void mode_command(void);
void mode_shop(void);
void mode_password(void);
void mode_options(void);
void mode_ranking_insert(void);

/* ------------------------------------------------------- work RAM map */

#define VDP_MODE_SHADOW   0xFFC700  /* copies of the VDP mode register commands
                                       $80xx/$81xx (reg 1 byte at $FFC703) */
#define VDP_PALETTE_DIRTY 0xFFC708  /* bit 0: upload the palette buffer */
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
void vdp_logo_tiles(uint32_t src);
void vdp_map_rows(uint16_t add);
uint32_t palette_load(uint32_t rec);
void hex_tiles(uint32_t dst, uint32_t v, int n, uint16_t base, int shown);

/* ------------------------------------------------------------ unpack.c */
uint32_t rle_unpack(uint32_t src, uint32_t dst, uint32_t *src_end);
void map_unpack(uint32_t src);

/* ---------------------------------------------------------- nemesis.c */
void nemesis_to_vdp(uint32_t src);
void nemesis_to_ram(uint32_t src, uint32_t dst);

/* ----------------------------------------------------------- system.c */
uint16_t rng_next(void);
uint8_t bcd_add(uint8_t a, uint8_t b);
uint8_t bcd_sub(uint8_t a, uint8_t b);
uint8_t bcd_addx(uint8_t a, uint8_t b, int *x);
uint8_t bcd_subx(uint8_t a, uint8_t b, int *x);
void wait_vblank(void);
void wait_frame_ticks(void);
void fade_out(void);
void ranking_tables_init(void);
void records_init(void);
void pads_init(void);
void sound_driver_load(void);
void hblank_install(void);
void reset(void);

/* ------------------------------------------------------------ audio.c */
void sound_command(uint8_t command);

/* ----------------------------------------------------------- vblank.c */
void vblank_menu(void);
void vblank_race(void);
uint16_t char_tile(uint8_t c);
void screen_reset(void);
void display_on(void);
void display_off(void);
void plane_a_fill(uint16_t entry);
void stage_graphics_load(void);
void hud_draw_all(void);
void course_graphics(void);
void bike_graphics(void);
void dma_slots_run(void);
void hud_eight_tiles(void);
void hud_big_digits(uint32_t command, uint32_t bcd_addr);

/* ------------------------------------------------------------- race.c */
void race_place_objects(void);
void race_turns(void);
void race_rivals(void);
void race_input(void);
void race_sounds(void);
void race_speed(void);
void race_drift(void);
void race_build_road(void);
void race_stage(void);
void hud_map_marker(void);
void hud_numbers(void);
void menu_countdown(void);
void push_start_blink(void);
void window_columns(uint32_t cmd, int steps);
void window_tall(uint32_t cmd, int steps);
void music_graphics(void);
void password_check(uint8_t *hi, uint8_t *lo);
void hud_curtain(void);
void stage_geometry(void);
void time_tiles(uint32_t time, uint32_t dst);

/* ---------------------------------------------------------- objects.c */
void obj_add_to_layer(uint32_t obj);
void objects_update(void);
void score_add(void);
void objects_clear(void);

#ifdef SNES
/* the SNES port's sprite renderer (snes/sprites.s, through snes/rt.s) */
void sprites_begin_c(void);
void sprite_object_c(uint32_t obj);
void sprites_end_c(void);
void sprites_hide_c(void);
#endif
void race_objects_setup(void);

#endif
