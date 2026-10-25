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

/* ------------------------------------------------------------ frame */

#include "sho_funcs.h"

#define PAD1_HELD        0xFFC706
#define PAD1_PRESSED     0xFFC707
#define HUD_TASK         0xFFC8A6
#define RANKING_MODE     0xFFC8AA   /* 2: entering a name */
#define NAME_ENTRY       0xFF03C0   /* position, letter, repeat timer, ...,
                                       +$C seconds left (BCD), +$D frames */
#define ROM_ALPHABET     0x00DA84   /* A-Z . back end */
#define LETTER_BACK      0x1B
#define LETTER_END       0x1C

static void ranking_end(void)
{
    call68k(f_006DD4);               /* fade out (waits for VBlanks) */
    wr16(0xFFC722, 0);
    wr16(0xFFC704, 0);
}

/* Left/right with auto-repeat every 4 frames ($DA5A). */
static int name_step(void)
{
    uint8_t held = rd8(PAD1_HELD) & 0x0C;
    if (!held) {
        wr16(NAME_ENTRY + 4, 0);
        return 0;
    }
    wr16(NAME_ENTRY + 4, rd16(NAME_ENTRY + 4) - 1);
    if ((int16_t)rd16(NAME_ENTRY + 4) >= 0)
        return 0;
    wr16(NAME_ENTRY + 4, 3);
    return held & 8 ? 1 : -1;
}

/* Finish the name: the cursor slot blank if on back/end; one name is
   not allowed. */
static void name_done(uint32_t name)
{
    if (rd16(NAME_ENTRY + 2) >= LETTER_BACK)
        wr8(name + rd16(NAME_ENTRY), 0x20);
    if (rd32(name) == 0x53455820)
        wr32(name, 0x4E414F20);
    ranking_end();
}

/*
 * One name entry frame: left/right pick a letter (the fourth slot only
 * offers back and end), A/B/C enter it, Start or the 30 second limit
 * finish.
 */
static void name_entry_frame(void)
{
    uint32_t name = rd32(0xFFC8B0);
    uint16_t pos, letter;
    int d;
    if (rd8(PAD1_PRESSED) & 0x80) {
        name_done(name);
        return;
    }
    if (rd16(HUD_TASK) != 5)
        goto objects;
    wr8(NAME_ENTRY + 0x0D, rd8(NAME_ENTRY + 0x0D) - 1);
    if (!rd8(NAME_ENTRY + 0x0D)) {
        wr8(NAME_ENTRY + 0x0D, 0x1D);
        if (!rd8(NAME_ENTRY + 0x0C)) {
            name_done(name);
            return;
        }
        wr8(NAME_ENTRY + 0x0C, bcd_sub(rd8(NAME_ENTRY + 0x0C), 1));
    }
    pos = rd16(NAME_ENTRY);
    d = name_step();
    if (pos == 3) {
        uint16_t t = (uint16_t)(d + rd16(NAME_ENTRY + 2));
        letter = t < LETTER_BACK ? LETTER_END : t == LETTER_BACK ? LETTER_BACK :
                 t == LETTER_END ? LETTER_END : LETTER_BACK;
    } else {
        letter = (uint16_t)(rd16(NAME_ENTRY + 2) + d);
        if (letter >= 0x1D)
            letter = d < 0 ? LETTER_END : 0;
    }
    wr16(NAME_ENTRY + 2, letter);
    if (rd8(PAD1_PRESSED) & 0x70) {
        if (letter == LETTER_END) {
            wr8(name + pos, 0x20);
            if (rd32(name) == 0x53455820)
                wr32(name, 0x4E414F20);
            ranking_end();
            return;
        }
        if (letter == LETTER_BACK) {
            if (pos) {
                wr8(name + pos, 0x20);
                wr16(NAME_ENTRY, --pos);
            }
        } else {
            wr16(NAME_ENTRY, ++pos);
        }
    }
    wr8(name + rd16(NAME_ENTRY), rd8(ROM_ALPHABET + rd16(NAME_ENTRY + 2)));
objects:
    objects_update();
}

/*
 * One ranking frame after the VBlank ($D93A): name entry, or the table
 * shown for a while (C/Start leaves), then the next page.
 */
static void ranking_frame(void)
{
    if (rd16(RANKING_MODE) == 2) {
        if (rd16(0xFFC71C)) {
            wr16(0xFFC71C, rd16(0xFFC71C) - 1);
            wr16(VDP_CTRL, 0x921C);
            wr16(HUD_TASK, 1);
            objects_update();
            return;
        }
        name_entry_frame();
        return;
    }
    if (rd8(PAD1_PRESSED) & 0xA0) {
        ranking_end();
        return;
    }
    if (rd16(0xFFC71C)) {
        wr16(0xFFC71C, rd16(0xFFC71C) - 1);
        if (!rd16(0xFFC71C)) {
            if (rd16(0xFFC71A) == 4) {
                ranking_end();
                return;
            }
            wr16(VDP_CTRL, 0x921C);
            wr16(HUD_TASK, 1);
        }
    } else if (rd16(HUD_TASK) == 6) {
        wr16(HUD_TASK, 0);
        wr16(0xFFC71C, 0x3C);
        wr16(0xFFC70E, rd16(0xFFC70E) + 1);
        wr16(0xFFC71A, rd16(0xFFC71A) + 1);
        if (rd16(RANKING_MODE) == 1)
            wr16(0xFFC71C, 0x96);
    }
    objects_update();
}

void ranking_frame_68k(void) { ranking_frame(); }
