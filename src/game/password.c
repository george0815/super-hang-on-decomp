/* The password screen (game mode $2C): cursors. */

#include "game.h"

#define PAD1_HELD        0xFFC706
#define PAD1_PRESSED     0xFFC707
#define FRAME_COUNTER_BYTE 0xFFC726
#define PW_POSITION      0xFFC70E   /* byte: digit being entered, 9 a row */
#define PW_LETTER        0xFFC710   /* letter grid: -1, then 2 rows of 14 */

static uint32_t pw_d0, pw_d1;

/* Two tiles (a cell's top and bottom halves 4 bytes apart). */
static void pw_cell(uint32_t cmd, uint16_t top, uint16_t bottom)
{
    wr32(VDP_CTRL, cmd);
    wr16(VDP_DATA, top);
    wr32(VDP_CTRL, cmd + 0x40000);
    wr16(VDP_DATA, bottom);
}

/* Under the digit being entered: 9 digits a row, 6 bytes apart. */
static uint32_t pw_digit_cell(void)
{
    uint8_t pos = rd8(PW_POSITION);
    uint32_t r = pos % 9;
    pw_d0 = 2 * r;
    pw_d1 = (6 * r) << 16;
    return 0x530C0003 + 0x1000000u * (pos / 9) + pw_d1;
}

/* On the letter grid: two rows of 14, 4 bytes apart; -1 is the first
   cell of the first row. */
static uint32_t pw_letter_cell(void)
{
    uint16_t i = (uint16_t)(rd16(PW_LETTER) + 1);
    uint32_t cmd = 0x580A0003;
    if (i >= 14) {
        cmd = 0x590A0003;
        i -= 14;
        if (i >= 14)
            i = 13;
    }
    return cmd + ((uint32_t)i << 18);
}

/* Erase the digit cursor ($4D2A). */
static void pw_digit_erase(void)
{
    pw_cell(pw_digit_cell(), 0, 0);
}

/* Erase the letter cursor, and the cells at the start of both rows
   ($4E20). */
static void pw_letter_erase(void)
{
    wr32(VDP_CTRL, 0x580A0003);
    wr16(VDP_DATA, 0);
    wr32(VDP_CTRL, 0x590A0003);
    wr16(VDP_DATA, 0);
    pw_cell(pw_letter_cell(), 0, 0);
}

static void pw_cursors_draw(void)            /* $4EA8 */
{
    pw_cell(pw_digit_cell(), 0x8349, 0x8B49);
    pw_cell(pw_letter_cell(), 0xC349, 0xCB49);
}

/*
 * One frame of the cursors ($4D16). While A is held the pad moves the
 * letter cursor (with a click), which then shows blinking; otherwise
 * both cursors blink every 16 frames.
 */
static void pw_cursors(void)
{
    uint8_t p = rd8(PAD1_PRESSED);
    uint16_t i;
    if (!(rd8(PAD1_HELD) & 0x40)) {
        if (rd8(FRAME_COUNTER_BYTE) & 0x10)
            pw_digit_erase();
        else
            pw_cursors_draw();
        return;
    }
    i = rd16(PW_LETTER);
    if (p & 1) {                                   /* up a row */
        pw_letter_erase();
        if (i >= 0x0D) {
            wr16(PW_LETTER, i - 14);
            sound_command(0xA5);
        }
    } else if (p & 2) {                            /* down a row */
        pw_letter_erase();
        if (i == 0xFFFF || i < 14) {
            wr16(PW_LETTER, i + 14);
            sound_command(0xA5);
        }
    } else if (p & 4) {                            /* left */
        pw_letter_erase();
        if (i != 0xFFFF) {
            wr16(PW_LETTER, i - 1);
            sound_command(0xA5);
        }
    } else {
        if (p & 8) {                               /* right */
            pw_letter_erase();
            if (i == 0x1B) {
                pw_letter_erase();
                return;
            }
            wr16(PW_LETTER, i + 1);
            sound_command(0xA5);
        }
        if (!(rd8(FRAME_COUNTER_BYTE) & 0x10)) {
            pw_cursors_draw();
            return;
        }
    }
    pw_letter_erase();
}

static void pw_regs(void)
{
    set_sr(get_sr() & 0xF8FF);
}

void pw_cursors_68k(void) { pw_cursors(); pw_regs(); }
void pw_digit_erase_68k(void) { pw_digit_erase(); pw_regs(); cpu.d[0] = pw_d0; cpu.d[1] = pw_d1; }
void pw_letter_erase_68k(void) { pw_letter_erase(); pw_regs(); cpu.x = 0; }
