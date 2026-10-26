/*
 * Sound: the driver host (src/runtime/sound_host.c; the driver itself is
 * src/sound/driver.c) and the native FM / PSG synthesizers in src/sound.
 */
#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

#define YM_RATE 53267   /* YM2612 output rate: 7.67 MHz / 144 */

/* driver host */
enum { SND_IRQ, SND_TICK, SND_REQUESTS, SND_STOP_ALL, SND_FM_RESET, SND_BANK,
       SND_TIMER_START, SND_TIMER_RESET, SND_ENTRIES };
void sound_call(int entry);           /* per build: entry_native.c, sound_verify.c */
extern void (*sound_port_log)(uint16_t addr, uint8_t v);   /* Z80 port writes */
extern int sound_ports_muted;
uint16_t sound_bank_get(void);
void sound_bank_set(uint16_t bank);
void sound_reset_line(int running);
void sound_vblank(void);
void sound_frame(int samples);        /* advance by this many YM samples */
void sound_tick(void);                /* one driver timer tick */
extern int sound_external_ticks;      /* replay: ticks driven by the recording */
void sound_replay_irq(void);          /* replay: Z80 took its VBlank interrupt */
void sound_replay_timer(void);        /* replay: timer reset */
void sound_replay_tick(void);         /* replay: tick routine entered */
extern void (*sound_write_hook)(int chip, int port, uint8_t v);  /* tests */

/* synthesizers: reg is 0x000-0x0FF for part 1, 0x100-0x1FF for part 2 */
void ym_reset(void);
void ym_write(int reg, uint8_t v);
void psg_write(uint8_t v);
void ym_render(int samples);          /* mixes FM + PSG into the output buffer */

/* User mix settings (percent). Each output channel is assigned to a group
   from what the driver is playing on it. */
enum { SND_MUSIC, SND_ENGINE, SND_EFFECTS, SND_GROUPS };
enum { FILTER_OFF, FILTER_CONSOLE, FILTER_SOFT, FILTER_MODES };
typedef struct {
    int master;
    int group[SND_GROUPS];
    int filter;
} SoundSettings;
extern SoundSettings sound_settings;

void ym_set_gain(int ch, float gain);    /* FM channel 0-5 */
void psg_set_gain(int ch, float gain);   /* PSG channel 0-3 (3 = noise) */

/* mixed output, interleaved stereo at YM_RATE */
extern int16_t sound_buffer[2 * 2048];
extern int sound_buffered;            /* frames in sound_buffer */
extern uint64_t ym_samples;           /* total samples rendered */

#endif
