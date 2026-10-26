/*
 * Mega Drive VDP (315-5313), mode 5 only.
 *
 * Memories are kept the way the hardware addresses them: VRAM as 64 KB of
 * big-endian bytes, CRAM and VSRAM as 16-bit words. The renderer produces
 * one scanline of CRAM indices at a time so raster effects driven by the
 * HBlank interrupt (the road) land on the right lines.
 */
#ifndef VDP_H
#define VDP_H

#include <stdint.h>

#define VDP_WIDTH  320
#define VDP_LINES  224

typedef struct {
    uint8_t vram[0x10000];
    uint16_t cram[64];
    uint16_t vsram[64];
    uint8_t reg[32];

    uint16_t addr;
    uint8_t code;          /* CD5-CD0 */
    uint8_t pending;       /* first half of a command word received */
    uint8_t fill_pending;  /* DMA fill waiting for its data word */

    int line;              /* scanline being displayed (0..261) */
    uint16_t line_vsram[VDP_LINES][2];  /* VSRAM 0-1 as each line was drawn
                                           (raster effects change them) */
    int hint_counter;
    uint8_t vint_pending;
    uint8_t in_vblank;
} Vdp;

extern Vdp vdp;

void vdp_reset(void);
void vdp_write_ctrl(uint16_t v);
void vdp_write_data(uint16_t v);
uint16_t vdp_read_ctrl(void);   /* status register */
uint16_t vdp_read_data(void);
uint16_t vdp_read_hv(void);

/* Render one active scanline into 320 CRAM indices (0-63). */
void vdp_render_line(int line, uint8_t *out);

/* CRAM entry as 0x00RRGGBB */
uint32_t vdp_color(int index);

#endif
