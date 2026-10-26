#include <string.h>

#include "m68k_rt.h"
#include "vdp.h"

Vdp vdp;
int vdp_trace;   /* debug: log DMA operations to stderr */
#include <stdio.h>

void vdp_reset(void)
{
    memset(&vdp, 0, sizeof vdp);
}

/* ---------------------------------------------------------------- ports */

static void vram_write_word(uint16_t a, uint16_t v)
{
    if (a & 1)  /* odd addresses store the bytes swapped */
        v = (uint16_t)(v << 8 | v >> 8);
    a &= 0xfffe;
    vdp.vram[a] = (uint8_t)(v >> 8);
    vdp.vram[a + 1] = (uint8_t)v;
}

static void write_target(uint16_t v)
{
    switch (vdp.code & 0x0f) {
    case 0x01: vram_write_word(vdp.addr, v); break;
    case 0x03: vdp.cram[(vdp.addr >> 1) & 0x3f] = v & 0x0eee; break;
    case 0x05: vdp.vsram[(vdp.addr >> 1) & 0x3f] = v; break;
    default: break;  /* writes with a read code are ignored */
    }
    vdp.addr += vdp.reg[15];
}

static uint32_t dma_length(void)
{
    uint32_t len = vdp.reg[19] | vdp.reg[20] << 8;
    return len ? len : 0x10000;
}

/* Every DMA type (fill included) leaves the source address registers
   advanced by the programmed length and the length registers at zero. */
static void dma_done(void)
{
    uint16_t end = (uint16_t)(vdp.reg[21] + (vdp.reg[22] << 8) + vdp.reg[19] + (vdp.reg[20] << 8));
    vdp.reg[21] = (uint8_t)end;
    vdp.reg[22] = (uint8_t)(end >> 8);
    vdp.reg[19] = vdp.reg[20] = 0;
}

static void dma_68k(void)
{
    uint32_t len = dma_length();
    uint32_t src = (vdp.reg[21] << 1 | vdp.reg[22] << 9 | (vdp.reg[23] & 0x7f) << 17);
    if (vdp_trace) fprintf(stderr, "dma68k code %02X addr %04X len %X inc %d src %06X\n", vdp.code, vdp.addr, len, vdp.reg[15], src);
    while (len--) {
        write_target(rd16(src));
        /* the source address wraps within a 128 KB window */
        src = (src & 0xfe0000) | ((src + 2) & 0x1ffff);
    }
    dma_done();
}

static void dma_fill(uint16_t data)
{
    uint32_t len = dma_length();
    if (vdp_trace) fprintf(stderr, "fill code %02X addr %04X len %X inc %d data %04X\n", vdp.code, vdp.addr, len, vdp.reg[15], data);
    /* the data word is written first (a normal write, so the address
       advances), then its high byte is repeated through VRAM */
    vram_write_word(vdp.addr, data);
    vdp.addr += vdp.reg[15];
    do {
        vdp.vram[vdp.addr ^ 1] = (uint8_t)(data >> 8);
        vdp.addr += vdp.reg[15];
    } while (--len);
    dma_done();
}

static void dma_copy(void)
{
    uint32_t len = dma_length();
    uint16_t src = (uint16_t)(vdp.reg[21] | vdp.reg[22] << 8);
    if (vdp_trace) fprintf(stderr, "copy addr %04X len %X inc %d src %04X\n", vdp.addr, len, vdp.reg[15], src);
    do {
        vdp.vram[vdp.addr ^ 1] = vdp.vram[src ^ 1];
        src++;
        vdp.addr += vdp.reg[15];
    } while (--len);
    dma_done();
}

void vdp_write_ctrl(uint16_t v)
{
    if (!vdp.pending) {
        if ((v & 0xc000) == 0x8000) {  /* register write */
            int r = (v >> 8) & 0x1f;
            if (r < 24)
                vdp.reg[r] = (uint8_t)v;
            vdp.code &= 0x3c;  /* a register write also clears CD1-CD0 */
            return;
        }
        vdp.addr = (uint16_t)((vdp.addr & 0xc000) | (v & 0x3fff));
        vdp.code = (uint8_t)((vdp.code & 0x3c) | (v >> 14));
        vdp.pending = 1;
        return;
    }
    vdp.pending = 0;
    vdp.addr = (uint16_t)((vdp.addr & 0x3fff) | (v & 3) << 14);
    vdp.code = (uint8_t)((vdp.code & 0x03) | ((v >> 2) & 0x3c));
    if ((vdp.code & 0x20) && (vdp.reg[1] & 0x10)) {
        switch (vdp.reg[23] >> 6) {
        case 0: case 1: dma_68k(); break;
        case 2: vdp.fill_pending = 1; break;
        case 3: dma_copy(); break;
        }
    }
}

void vdp_write_data(uint16_t v)
{
    vdp.pending = 0;
    if (vdp.fill_pending) {
        vdp.fill_pending = 0;
        dma_fill(v);
        return;
    }
    write_target(v);
}

uint16_t vdp_read_data(void)
{
    uint16_t v = 0;
    vdp.pending = 0;
    switch (vdp.code & 0x0f) {
    case 0x00: v = (uint16_t)(vdp.vram[vdp.addr & 0xfffe] << 8 | vdp.vram[(vdp.addr & 0xfffe) + 1]); break;
    case 0x04: v = vdp.vsram[(vdp.addr >> 1) & 0x3f]; break;
    case 0x08: v = vdp.cram[(vdp.addr >> 1) & 0x3f]; break;
    }
    vdp.addr += vdp.reg[15];
    return v;
}

uint16_t vdp_read_ctrl(void)
{
    uint16_t s = 0x3600;  /* FIFO empty, upper bits read back as open bus */
    vdp.pending = 0;
    if (vdp.vint_pending) s |= 0x80;
    if (vdp.in_vblank || !(vdp.reg[1] & 0x40)) s |= 0x08;
    return s;
}

uint16_t vdp_read_hv(void)
{
    /* NTSC V counter: 0x00-0xEA, then jumps back to 0xE5 */
    int l = vdp.line;
    int v = l <= 0xea ? l : l - 6;
    return (uint16_t)((v & 0xff) << 8);
}

/* ------------------------------------------------------------- rendering */

typedef struct { uint8_t pix[VDP_WIDTH]; uint8_t pri[VDP_WIDTH]; } Layer;

static const int plane_cells[4] = { 32, 64, 32, 128 };

static inline uint8_t tile_pixel(uint16_t entry, int x, int y)
{
    uint32_t base = (entry & 0x7ff) << 5;
    uint8_t b;
    if (entry & 0x0800) x = 7 - x;
    if (entry & 0x1000) y = 7 - y;
    b = vdp.vram[(base + y * 4 + (x >> 1)) & 0xffff];
    return (x & 1) ? (b & 0x0f) : (b >> 4);
}

static void draw_plane(Layer *l, int line, int plane, uint32_t nt, int x0, int x1)
{
    int w = plane_cells[vdp.reg[16] & 3], h = plane_cells[(vdp.reg[16] >> 4) & 3];
    uint32_t hs_base = (vdp.reg[13] & 0x3f) << 10;
    int hmode = vdp.reg[11] & 3, vcol = vdp.reg[11] & 4;
    int hs_line = hmode == 0 ? 0 : hmode == 2 ? (line & ~7) : line;
    uint32_t ha = (hs_base + hs_line * 4 + plane * 2) & 0xffff;
    int hscroll = (vdp.vram[ha] << 8 | vdp.vram[ha + 1]) & 0x3ff;
    int x;

    for (x = x0; x < x1; x++) {
        int vs = vdp.vsram[vcol ? ((x >> 4) * 2 + plane) & 0x3f : plane] & 0x3ff;
        int px = (x - hscroll) & (w * 8 - 1);
        int py = (line + vs) & (h * 8 - 1);
        uint32_t ea = (nt + ((py >> 3) * w + (px >> 3)) * 2) & 0xffff;
        uint16_t e = (uint16_t)(vdp.vram[ea] << 8 | vdp.vram[ea + 1]);
        uint8_t c = tile_pixel(e, px & 7, py & 7);
        l->pix[x] = c ? (uint8_t)(((e >> 9) & 0x30) | c) : 0;
        l->pri[x] = (e >> 15) & 1;
    }
}

static void draw_window(Layer *l, int line, int x0, int x1)
{
    uint32_t nt = (vdp.reg[3] & 0x3c) << 10;  /* H40: bit 1 ignored */
    int x;
    for (x = x0; x < x1; x++) {
        uint32_t ea = (nt + ((line >> 3) * 64 + (x >> 3)) * 2) & 0xffff;
        uint16_t e = (uint16_t)(vdp.vram[ea] << 8 | vdp.vram[ea + 1]);
        uint8_t c = tile_pixel(e, x & 7, line & 7);
        l->pix[x] = c ? (uint8_t)(((e >> 9) & 0x30) | c) : 0;
        l->pri[x] = (e >> 15) & 1;
    }
}

static void draw_sprites(Layer *l, int line)
{
    uint32_t sat = (vdp.reg[5] & 0x7e) << 9;  /* H40: bit 0 ignored */
    int n = 0, count = 0, dots = 0, seen_nonzero = 0;
    uint8_t filled[VDP_WIDTH];
    memset(filled, 0, sizeof filled);

    do {
        const uint8_t *s = &vdp.vram[(sat + n * 8) & 0xffff];
        int y = ((s[0] << 8 | s[1]) & 0x3ff) - 128;
        int hs = ((s[2] >> 2) & 3) + 1, vs = (s[2] & 3) + 1;
        int link = s[3] & 0x7f;
        uint16_t e = (uint16_t)(s[4] << 8 | s[5]);
        int xraw = (s[6] << 8 | s[7]) & 0x1ff;
        int x = xraw - 128;

        if (line >= y && line < y + vs * 8) {
            int row = line - y, cx;
            if (++count > 20) break;
            if (xraw == 0) {
                if (seen_nonzero) break;  /* sprite masking */
            } else {
                seen_nonzero = 1;
            }
            if (e & 0x1000) row = vs * 8 - 1 - row;
            for (cx = 0; cx < hs * 8; cx++) {
                int sx = x + cx, col = cx, tile;
                uint8_t c;
                if (dots++ >= VDP_WIDTH) break;
                if (sx < 0 || sx >= VDP_WIDTH || filled[sx]) continue;
                if (e & 0x0800) col = hs * 8 - 1 - col;
                tile = (e & 0x7ff) + (col >> 3) * vs + (row >> 3);
                c = tile_pixel((uint16_t)((e & 0xe000) | (tile & 0x7ff)), col & 7, row & 7);
                if (!c) continue;
                filled[sx] = 1;
                l->pix[sx] = (uint8_t)(((e >> 9) & 0x30) | c);
                l->pri[sx] = (e >> 15) & 1;
            }
            if (dots >= VDP_WIDTH) break;
        }
        n = link;
    } while (n && n < 80);
}

void vdp_render_line(int line, uint8_t *out)
{
    static Layer a, b, s;
    uint8_t backdrop = vdp.reg[7] & 0x3f;
    int x, wx0 = 0, wx1 = 0;

    vdp.line_vsram[line][0] = vdp.vsram[0];
    vdp.line_vsram[line][1] = vdp.vsram[1];
    if (!(vdp.reg[1] & 0x40)) {
        memset(out, backdrop, VDP_WIDTH);
        return;
    }
    memset(&s, 0, sizeof s);
    draw_plane(&b, line, 1, (vdp.reg[4] & 7) << 13, 0, VDP_WIDTH);

    /* window: rows above/below reg 18's split, or columns left/right of
       reg 17's split, replace plane A */
    {
        int wv = (vdp.reg[18] & 0x1f) * 8, down = vdp.reg[18] & 0x80;
        int wh = (vdp.reg[17] & 0x1f) * 16, right = vdp.reg[17] & 0x80;
        if (down ? line >= wv : line < wv) {
            wx0 = 0; wx1 = VDP_WIDTH;
        } else if (wh) {
            if (wh > VDP_WIDTH) wh = VDP_WIDTH;
            if (right) { wx0 = wh; wx1 = VDP_WIDTH; } else { wx0 = 0; wx1 = wh; }
        }
    }
    draw_plane(&a, line, 0, (vdp.reg[2] & 0x38) << 10, 0, VDP_WIDTH);
    if (wx1 > wx0)
        draw_window(&a, line, wx0, wx1);
    draw_sprites(&s, line);

    for (x = 0; x < VDP_WIDTH; x++) {
        uint8_t c;
        if (s.pix[x] && s.pri[x]) c = s.pix[x];
        else if (a.pix[x] && a.pri[x]) c = a.pix[x];
        else if (b.pix[x] && b.pri[x]) c = b.pix[x];
        else if (s.pix[x]) c = s.pix[x];
        else if (a.pix[x]) c = a.pix[x];
        else if (b.pix[x]) c = b.pix[x];
        else c = backdrop;
        out[x] = c;
    }
    if (vdp.reg[0] & 0x20)  /* blank the leftmost column */
        memset(out, backdrop, 8);
}

uint32_t vdp_color(int index)
{
    /* 3-bit DAC levels, roughly as measured on hardware */
    static const uint8_t level[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
    uint16_t c = vdp.cram[index & 0x3f];
    return (uint32_t)level[(c >> 1) & 7] << 16 | level[(c >> 5) & 7] << 8 | level[(c >> 9) & 7];
}
