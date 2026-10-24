/*
 * Mega Drive machine model around the recompiled game code: memory map,
 * I/O ports, Z80 mailbox, interrupts and video timing.
 */
#ifndef GENESIS_H
#define GENESIS_H

#include <stdint.h>

/* pad bits, active high (converted to the hardware's active-low format) */
enum {
    PAD_UP = 0x01, PAD_DOWN = 0x02, PAD_LEFT = 0x04, PAD_RIGHT = 0x08,
    PAD_B = 0x10, PAD_C = 0x20, PAD_A = 0x40, PAD_START = 0x80
};

/* Host callbacks */
void host_frame(const uint8_t *pixels);   /* 320x224 CRAM indices, once per frame */
uint16_t host_pad(int port);              /* PAD_* bits */

/* Live mode: reset the machine and run the game. Never returns; the host
   regains control through host_frame. */
void gen_init(const uint8_t *rom, uint32_t size);
void gen_run(void);

/* Replay mode hooks (set by the replay driver before gen_run). */
typedef struct {
    int (*irq)(uint32_t pc);                 /* returns level to take, 0 = none */
    uint32_t (*hw_read)(int width, uint32_t addr, uint32_t live_value);
    void (*vblank)(void);                    /* after the exception frame is pushed */
} GenReplay;
extern const GenReplay *gen_replay;

/* Interrupt entry, shared with the replay driver */
void gen_take_irq(int level, uint32_t pc);
void gen_render_line(void);   /* render vdp.line and advance */
void gen_sound_frame(void);   /* Z80 VBlank interrupt + one frame of audio */

extern uint8_t gen_frame[320 * 224];
extern uint8_t z80_ram[0x2000];

#endif
