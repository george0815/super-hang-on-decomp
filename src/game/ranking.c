/* The ranking (best scores) table. */

#include "game.h"

/*
 * A time as tiles ($BF98): minutes, seconds, hundredths from bytes 1-3
 * of the record as eight digits, with ' and " marks; minutes past 9 show
 * as 1 and the units digit.
 */
static void time_record_tiles(uint32_t rec, uint32_t dst)
{
    uint32_t v = (uint32_t)rd8(rec + 1) << 24 | (uint32_t)rd8(rec + 2) << 12 | rd8(rec + 3);
    uint16_t t;
    wr32(0xFF04E8, v);
    hex_tiles(dst, v, 8, 0xA0, 1);
    wr16(dst + 4, 0xC63C);
    wr16(dst + 0x0A, 0xC63D);
    wr16(dst, 0);
    t = rd16(dst + 2) & 0x7FF;
    if (t >= 0x61E) {
        wr16(dst + 2, (uint16_t)(t - 0x61E + 0xA614));
        wr16(dst, 0xA615);
    }
}

void time_record_tiles_68k(void)
{
    time_record_tiles(cpu.a[3], cpu.a[4]);
    cpu.d[1] = 0xFFFFFFFF;
}

/* Four name characters as tiles, codes past 'Z'+1 cleared to spaces
   ($DBD0). */
static uint16_t name_tiles(uint32_t name, uint32_t dst)
{
    uint16_t t = 0;
    int k;
    for (k = 0; k < 4; k++) {
        if (rd8(name + k) >= 0x5C)
            wr8(name + k, 0x20);
        t = char_tile(rd8(name + k));
        wr16(dst + 2 * k, t);
    }
    return t;
}

/*
 * Seven rows of the ranking table as tiles ($DB5E), $38 bytes each after
 * `rows`: rank label, score, stage number, name and (when recorded) the
 * time, from 14-byte entries at `entries`.
 */
static int ranking_timed;
static uint16_t ranking_tile;
static uint32_t ranking_rows(uint32_t rows, uint32_t entries)
{
    int k;
    for (k = 0; k < 7; k++, entries += 0x0E) {
        rows += 0x38;
        wr32(rows, rd32(0x00E15C + 8 * k));
        wr32(rows + 4, rd32(0x00E160 + 8 * k));
        hex_tiles(rows + 8, rd32(entries), 8, 0xA0, 0);
        hex_tiles(rows + 0x18, rd16(entries + 4), 4, 0xA0, 0);
        ranking_tile = name_tiles(entries + 6, rows + 0x20);
        wr32(rows + 0x2C, 0);
        ranking_timed = rd32(entries + 0x0A) != 0;
        if (ranking_timed)
            time_record_tiles(entries + 0x0A, rows + 0x28);
    }
    return rows;
}

void ranking_rows_68k(void)
{
    uint32_t last = cpu.a[4] + 6 * 0x0E;
    cpu.a[3] = ranking_rows(cpu.a[3], cpu.a[4]);
    if (ranking_timed) {
        cpu.d[1] = 0xFFFFFFFF;
        cpu.d[4] = rd32(0xFF04E8);
    } else {
        cpu.d[1] = 0xFFFF0000u | ranking_tile;
        cpu.d[4] = (rd32(last) & 0xFFFF0000u) | rd16(last + 4);
    }
    set_dn(5, 2, 7);
}

/* Seven default entries ($DC3E): score, stage 1, name, no time. */
static uint32_t ranking_defaults(uint32_t dst, uint32_t src)
{
    int k;
    for (k = 0; k < 7; k++, dst += 0x0E, src += 8) {
        wr32(dst, rd32(src));
        wr16(dst + 4, 1);
        wr32(dst + 6, rd32(src + 4));
        wr32(dst + 0x0A, 0);
    }
    return src;
}

void ranking_defaults_68k(void)
{
    cpu.a[2] = ranking_defaults(cpu.a[0], cpu.a[2]);
    cpu.a[0] += 7 * 0x0E;
    set_dn(0, 2, 0xFFFF);
}
