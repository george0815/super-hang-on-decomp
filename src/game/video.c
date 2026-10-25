/* VDP setup and transfer helpers. */

#include "game.h"

/*
 * Fill `count` words of VDP memory with `value`, starting at the address
 * encoded in `command` ($83FA; $83F8 is the same with value 0).
 */
void vdp_fill(uint32_t command, uint16_t value, int count)
{
    int i;
    wr32(VDP_CTRL, command);
    for (i = 0; i < count; i++)
        wr16(VDP_DATA, value);
}

/* Copy `count` words from 68000 memory at `src` to VDP memory ($83DC). */
void vdp_copy(uint32_t command, uint32_t src, int count)
{
    int i;
    wr32(VDP_CTRL, command);
    for (i = 0; i < count; i++)
        wr16(VDP_DATA, rd16(src + 2 * i));
}

/*
 * Copy a `width` x `height` block of name table entries ($840E). Each row
 * starts 128 bytes (one 64-cell plane row) below the previous one.
 */
void vdp_copy_rect(uint32_t command, uint32_t src, int width, int height)
{
    int row, i;
    for (row = 0; row < height; row++) {
        wr32(VDP_CTRL, command + (uint32_t)row * 0x800000u);
        for (i = 0; i < width; i++) {
            wr16(VDP_DATA, rd16(src));
            src += 2;
        }
    }
}

/*
 * Load the default VDP register values, remember the two mode registers,
 * and clear the palette ($81E8).
 */
void vdp_init_registers(void)
{
    int i;
    for (i = 0; i < 19; i++)
        wr16(VDP_CTRL, rd16(ROM_VDP_REGISTER_DEFAULTS + 2 * i));
    wr32(VDP_MODE_SHADOW, rd32(ROM_VDP_REGISTER_DEFAULTS));
    vdp_fill(VDP_CMD_CRAM_WRITE(0), 0, 64);
}

/* ------------------------------------------------ 68000 interface */

void vdp_fill_68k(void)
{
    int count = (cpu.d[6] & 0xffff) + 1;
    vdp_fill(cpu.d[7], (uint16_t)cpu.d[5], count);
    cpu.a[5] = VDP_DATA;
    set_dn(6, 2, 0xffff);
    f_logic(2, cpu.d[5]);   /* flags from the last word written */
}

void vdp_fill_zero_68k(void)
{
    cpu.d[5] = 0;
    vdp_fill_68k();
}

void vdp_copy_68k(void)
{
    int count = (cpu.d[6] & 0xffff) + 1;
    uint32_t src = cpu.a[6];
    vdp_copy(cpu.d[7], src, count);
    cpu.a[5] = VDP_DATA;
    cpu.a[6] = src + 2 * count;
    set_dn(6, 2, 0xffff);
    f_logic(2, rd16(src + 2 * (count - 1)));
}

void vdp_copy_rect_68k(void)
{
    int width = (cpu.d[6] & 0xffff) + 1, height = (cpu.d[5] & 0xffff) + 1, row;
    uint32_t src = cpu.a[6], cmd = cpu.d[7];
    vdp_copy_rect(cmd, src, width, height);
    cpu.a[5] = VDP_DATA;
    cpu.a[6] = src + 2u * width * height;
    cpu.d[3] = 0x800000;
    set_dn(2, 2, 0xffff);
    set_dn(5, 2, 0xffff);
    /* d7 advanced past the last row, flags from that addition */
    for (row = 0; row < height; row++)
        cmd = f_add(4, 0x800000, cmd);
    cpu.d[7] = cmd;
}

void vdp_init_registers_68k(void)
{
    vdp_init_registers();
    cpu.a[0] = ROM_VDP_REGISTER_DEFAULTS + 2 * 19;
    set_dn(0, 2, 0xffff);
    cpu.d[7] = VDP_CMD_CRAM_WRITE(0);
    cpu.d[5] = 0;
    cpu.d[6] = (cpu.d[6] & 0xffff0000u) | 0xffff;
    cpu.a[5] = VDP_DATA;
    f_logic(2, 0);
}

/* ------------------------------------------------------------- more */

/*
 * The low n hex digits of a value as name table entries, base << 8 plus
 * the digit, plus $614 once shown: leading zeros stay blank unless shown
 * is set, the last digit always shows ($C16C/$C176/$C180/$C184).
 */
void hex_tiles(uint32_t dst, uint32_t v, int n, uint16_t base, int shown)
{
    int k;
    base = (uint16_t)(base << 8);
    for (k = 0; k < n; k++) {
        uint16_t digit = v >> (4 * (n - 1 - k)) & 0xF;
        if (digit || k == n - 1)
            shown = 1;
        wr16(dst + 2 * k, (uint16_t)(digit + base + (shown ? 0x614 : 0)));
    }
}


/* The first $310 words of VRAM (the logo's tiles) from a0 ($7FE8). */
void vdp_logo_tiles(uint32_t src)
{
    vdp_copy(0x40000000, src, 0x310);
}

void vdp_logo_tiles_68k(void)
{
    vdp_logo_tiles(cpu.a[0]);
    set_dn(1, 2, 0xFFFF);
}

/*
 * Write 24 rows of 64 name table entries from the unpacked map
 * ($FFEB14, rows $AA bytes apart) to the VDP data port, each plus `add`
 * (tile base and palette) ($F3A0). The VRAM address is already set.
 */
void vdp_map_rows(uint16_t add)
{
    uint32_t src = 0xFFEB14;
    int row, k;
    for (row = 0; row < 24; row++, src += 0x2A)
        for (k = 0; k < 64; k++, src += 2)
            wr16(VDP_DATA, (uint16_t)(rd16(src) + add));
}

void vdp_map_rows_68k(void) { vdp_map_rows((uint16_t)cpu.d[7]); }

/*
 * Palette record ($8448): byte offset into the palette buffer, byte count
 * minus one, then the colour words. Marks the palette for upload.
 */
uint32_t palette_load(uint32_t rec)
{
    uint32_t dst = 0xFFD380 + rd8(rec);
    int n = rd8(rec + 1) + 1, k;
    wr8(VDP_PALETTE_DIRTY, rd8(VDP_PALETTE_DIRTY) | 1);
    rec += 2;
    for (k = 0; k < n; k++, rec += 2)
        wr16(dst + 2 * k, rd16(rec));
    return rec;
}

void palette_load_68k(void)
{
    cpu.a[6] = palette_load(cpu.a[6]);
    cpu.d[0] = 0xFFFF;
    cpu.d[1] = 0;
}

/* Fill words in RAM ($E6D4). */
void fill_words_68k(void)
{
    int k, n = (uint16_t)cpu.d[1] + 1;
    for (k = 0; k < n; k++)
        wr16(cpu.a[0] + 2 * k, (uint16_t)cpu.d[0]);
    cpu.a[0] += 2 * n;
    set_dn(1, 2, 0xFFFF);
}
