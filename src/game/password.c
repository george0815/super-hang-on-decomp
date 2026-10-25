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

/* A row of 14 characters from codes, each followed by a blank ($4C96;
   the same as the password row on the command screen). */
static void pw_row(uint32_t cmd, uint32_t codes)
{
    int k;
    wr32(VDP_CTRL, cmd);
    for (k = 0; k < 14; k++) {
        uint8_t c = rd8(codes + k);
        wr16(VDP_DATA, (uint16_t)(c + 0x614 + rd16(0x005224 + 2 * c)));
        wr16(VDP_DATA, 0);
    }
}

void pw_row_68k(void)
{
    pw_row(cpu.d[7], cpu.a[6]);
    cpu.a[6] += 14;
    pw_regs();
    cpu.x = 0;
}

/* ------------------------------------------------------------ frame */

#define PW_STATE         0xFFB676   /* bit 0: accepted, bit 1: error shown */
#define PW_COUNT         0xFFC710   /* letters entered - 1 */
#define PW_CODES         0xFFB658
#define PW_CHOICE        0xFFC712   /* after acceptance: 0 arcade, 1 original */

static void pw_rows(void)                    /* $4C78 */
{
    pw_row(0x580C0003, PW_CODES);
    pw_row(0x590C0003, PW_CODES + 0x0E);
}

/* The arcade/original choice after a good password ($4C02, $4C4A). */
static void pw_choice_erase(void)
{
    wr32(VDP_CTRL, 0x5BAE0003);
    wr16(VDP_DATA, 0);
    wr32(VDP_CTRL, 0x5CAE0003);
    wr16(VDP_DATA, 0);
}

static void pw_choice_blink(void)
{
    if (!(rd8(FRAME_COUNTER_BYTE) & 0x10)) {
        pw_choice_erase();
        return;
    }
    wr32(VDP_CTRL, rd16(PW_CHOICE) ? 0x5CAE0003 : 0x5BAE0003);
    wr16(VDP_DATA, 0xA349);
}

/* Check the 28 codes: check bytes, then each state byte against its
   maximum. */
static int pw_valid(void)
{
    uint8_t hi, lo;
    int k;
    password_check(&hi, &lo);
    if ((uint8_t)(rd8(PW_CODES + 0x18) - 0x0B) != hi >> 4 ||
        (uint8_t)(rd8(PW_CODES + 0x19) - 0x0D) != (hi & 0xF) ||
        (uint8_t)(rd8(PW_CODES + 0x1A) - 0x0F) != lo >> 4 ||
        (uint8_t)(rd8(PW_CODES + 0x1B) - 0x11) != (lo & 0xF))
        return 0;
    for (k = 0; k < 16; k++)
        if (rd8(PW_CODES + k) > rd8(0x005136 + k))
            return 0;
    return 1;
}

/* Restore the game state from the codes (the reverse of $57D0). */
static void pw_restore(void)
{
    uint32_t ptr = 0x0050E6;
    int k;
    for (k = 0; k < 16; k++, ptr += 4) {
        uint8_t c = rd8(PW_CODES + k);
        if (c == 0x23)
            wr16(rd32(ptr) - 1, 0xFFFF);
        else
            wr8(rd32(ptr), c);
    }
    for (k = 3; k >= 0; k--, ptr += 4) {
        uint32_t i = PW_CODES + 16 + 2 * (3 - k);
        uint8_t hi = (uint8_t)((rd8(i) - 6 - 3 * k) << 4);
        uint8_t lo = (uint8_t)(rd8(i + 1) - 10 - 3 * k);
        wr8(rd32(ptr), (uint8_t)(hi + lo));
    }
    wr16(0xFFB610, rd16(0xFFB606));
    wr8(0xFFB630 + rd16(0xFFB60A), 1);
    wr8(PW_STATE, rd8(PW_STATE) | 1);
}

static void pw_move(int to)
{
    if (to < 0) {
        sound_command(0xA3);
        return;
    }
    wr8(PW_POSITION, (uint8_t)to);
    sound_command(0xA5);
}

/*
 * One password screen frame after the VBlank ($4962). Entering: the pad
 * moves the digit cursor (A held: the letter cursor), C enters the
 * letter, Start checks the password. An error message waits for C/Start;
 * after a good password up/down pick arcade (with the upgrades) or
 * original mode.
 */
static int pw_frame_unmasked;
static void pw_frame(void)
{
    uint8_t p = rd8(PAD1_PRESSED), state = rd8(PW_STATE), pos;
    pw_frame_unmasked = 1;
    if (state & 1) {
        pw_choice_blink();
        if (p & 3) {
            pw_choice_erase();
            wr16(PW_CHOICE, p & 1 ? 0 : 1);
            sound_command(0xA2);
        } else if (p & 0xA0) {
            wr16(0xFFC722, 0);
            if (rd16(PW_CHOICE)) {
                wr16(0xFFC704, 0x24);
            } else {
                wr16(0xFFC82C, 0);
                wr16(0xFFC8B6, 0xFFFF);
                wr16(0xFFC704, 0x08);
            }
        }
        return;
    }
    if (state & 2) {
        if (!(p & 0xA0)) {
            pw_frame_unmasked = 0;
            return;
        }
        vdp_copy(0x57160003, 0x015566, 16);
        wr8(PW_STATE, rd8(PW_STATE) & ~2);
        sound_command(0xA5);
        return;
    }
    pw_cursors();
    if (rd8(PAD1_HELD) & 0x40)
        return;
    pos = rd8(PW_POSITION);
    if (p & 1) {
        pw_digit_erase();
        pw_move(pos < 9 ? -1 : pos - 9);
    } else if (p & 2) {
        pw_digit_erase();
        pw_move(pos + 9 >= 0x24 ? -1 : pos + 9);
    } else if (p & 4) {
        pw_digit_erase();
        pw_move((pos % 9 == 0 ? pos + 9 : pos) - 1);
    } else if (p & 8) {
        pw_digit_erase();
        pw_move((pos % 9 == 8 ? pos - 9 : pos) + 1);
    } else if (p & 0x20) {
        uint16_t n = (uint16_t)(rd16(PW_COUNT) + 1);
        if (n == 0x1C) {
            sound_command(0xA3);
            return;
        }
        wr16(PW_COUNT, n);
        wr8(PW_CODES + n, rd8(PW_POSITION));
        pw_rows();
        pw_letter_erase();
        sound_command(0x94);
    } else if (p & 0x80) {
        if (pw_valid()) {
            pw_restore();
        } else {
            vdp_copy(0x57160003, 0x0050C6, 16);
            wr8(PW_STATE, rd8(PW_STATE) | 2);
            sound_command(0xA3);
        }
    }
}

void pw_frame_68k(void)
{
    pw_frame();
    if (pw_frame_unmasked)
        pw_regs();
}
