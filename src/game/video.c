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
