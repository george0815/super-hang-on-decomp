/*
 * The sound driver (src/sound/driver.c): entry points the host calls at
 * the times the Z80 program ran them (see src/runtime/sound_host.c).
 */
#ifndef DRIVER_H
#define DRIVER_H

#include <stdint.h>

void drv_irq(void);           /* VBlank interrupt ($0038) */
void drv_tick(void);          /* one tick ($0091) */
void drv_requests(void);      /* read the 68000's requests ($11E8) */
void drv_stop_all(void);      /* $0C37 */
void drv_fm_reset(void);      /* $1070 */
void drv_bank_music(void);    /* $1015 */
void drv_timer_start(void);   /* $001F */
void drv_timer_reset(void);   /* $0027 */

extern uint16_t drv_ix;       /* the track the last update left (the Z80's IX) */

#endif
