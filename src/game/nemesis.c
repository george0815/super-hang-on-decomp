/*
 * Nemesis graphics decompression ($855E to VDP, $8570 to RAM).
 *
 * Stream layout:
 *   word   bit 15: XOR mode (each output row is XORed into the previous
 *          one); bits 0-14: number of 8x8 tiles (8 rows of 32 bits each)
 *   code table: for each pixel value, a byte $80|value followed by one
 *          entry per code: byte (run-1)<<4 | code length, then the code;
 *          $FF ends the table
 *   bit stream of codes; each code stands for a run of one 4-bit pixel
 *   value. The escape code (8 bits from $FC up) is followed by 7 inline
 *   bits: 3 bits of run length - 1 and the 4-bit value.
 *
 * The decoding table is kept in work RAM at $FFE900 like the original
 * (256 words: code length << 8 | (run-1) << 4 | value).
 */
#include "game.h"

#define NEM_TABLE 0xFFE900

typedef void (*RowOut)(uint32_t row, void *ctx);

/* Fill the decoding table from the stream's code definitions. */
static uint32_t nemesis_build_table(uint32_t src)
{
    uint8_t b = rd8(src++);
    while (b != 0xff) {
        uint8_t value = b & 0x0f;   /* b = $80 | pixel value */
        for (;;) {
            uint8_t len, run, shift;
            uint16_t entry;
            uint32_t code;
            b = rd8(src++);
            if (b >= 0x80)
                break;              /* next pixel value (or the end) */
            run = b & 0x70;
            len = b & 0x0f;
            entry = (uint16_t)(len << 8 | run | value);
            shift = 8 - len;
            code = (uint32_t)rd8(src++) << shift;
            /* every 8-bit lookup starting with this code maps to it */
            for (uint32_t k = 0; k < (1u << shift); k++)
                wr16(NEM_TABLE + 2 * (code + k), entry);
        }
    }
    return src;
}

static void nemesis_decode(uint32_t src, RowOut out, void *ctx)
{
    uint16_t header = rd16(src);
    int xor_mode = header >> 15;
    uint16_t rows = (uint16_t)(header << 3);     /* 8 rows per tile, 16-bit count */
    uint32_t row = 0, prev = 0;
    int pixels = 0;
    uint16_t bits;
    int avail;

    src = nemesis_build_table(src + 2);
    bits = (uint16_t)(rd8(src) << 8 | rd8(src + 1));
    src += 2;
    avail = 16;

    for (;;) {
        unsigned peek = (bits >> (avail - 8)) & 0xff, value, count;
        if (peek < 0xfc) {
            uint16_t entry = rd16(NEM_TABLE + 2 * peek);
            avail -= (int8_t)(entry >> 8);
            if (avail < 9) {
                avail += 8;
                bits = (uint16_t)(bits << 8 | rd8(src++));
            }
            value = entry & 0x0f;
            count = (entry >> 4) & 0x0f;
        } else {
            /* escape: 6-bit marker, then 3 bits of run and 4 of value */
            unsigned inline_bits;
            avail -= 6;
            if (avail < 9) {
                avail += 8;
                bits = (uint16_t)(bits << 8 | rd8(src++));
            }
            avail -= 7;
            inline_bits = bits >> avail;
            value = inline_bits & 0x0f;
            count = (inline_bits & 0x70) >> 4;
            if (avail < 9) {
                avail += 8;
                bits = (uint16_t)(bits << 8 | rd8(src++));
            }
        }
        do {
            row = row << 4 | value;
            if (++pixels == 8) {
                if (xor_mode) {
                    prev ^= row;
                    out(prev, ctx);
                } else {
                    out(row, ctx);
                }
                if (--rows == 0)
                    return;
                row = 0;
                pixels = 0;
            }
        } while (count--);
    }
}

static void to_vdp(uint32_t row, void *ctx)
{
    (void)ctx;
    wr32(VDP_DATA, row);
}

static void to_ram(uint32_t row, void *ctx)
{
    uint32_t *dst = ctx;
    wr32(*dst, row);
    *dst += 4;
}

/* Decompress to the VDP data port (VRAM address already set). */
void nemesis_to_vdp(uint32_t src)
{
    nemesis_decode(src, to_vdp, 0);
}

/* Decompress to 68000 memory at dst. */
void nemesis_to_ram(uint32_t src, uint32_t dst)
{
    nemesis_decode(src, to_ram, &dst);
}

/* -------------------------------------------- 68000 interface
   The originals save and restore every register they use. */

void nemesis_to_vdp_68k(void)
{
    nemesis_to_vdp(cpu.a[0]);
}

void nemesis_to_ram_68k(void)
{
    nemesis_to_ram(cpu.a[0], cpu.a[4]);
}
