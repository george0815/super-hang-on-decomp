/* Original mode's command screen (parts, mechanic, race, end). */

#include "game.h"

#define FRAME_COUNTER_BYTE 0xFFC726
#define COMMAND_ROW      0xFFB604

/* The cursor column: a 7-entry window into a strip with the arrow in it,
   shifted by the selected row ($5358). */
static void command_cursor(void)
{
    vdp_copy_rect(0x51840003, 0x0053B4 - 4 * rd16(COMMAND_ROW), 1, 7);
}

/* Blinking: shown every other 16 frames, else the blank column ($5350). */
static void command_cursor_blink(void)
{
    if (rd8(FRAME_COUNTER_BYTE) & 0x10)
        command_cursor();
    else
        vdp_copy_rect(0x51840003, 0x0053A6, 1, 7);
}

/* The originals mask interrupts around the transfer and then unmask
   them all. */
static void command_regs(void)
{
    set_sr(get_sr() & 0xF8FF);
    set_dn(5, 2, 0xFFFF);
}

void command_cursor_68k(void) { command_cursor(); command_regs(); }
void command_cursor_blink_68k(void) { command_cursor_blink(); command_regs(); }

/* ---------------------------------------------------------- mechanic */

#define PAD1_PRESSED     0xFFC707
#define SUBMENU          0xFFB602   /* submenu x4 (0 = command list); byte
                                       $FFB603 bit 0: submenu set up */
#define MECHANIC_CHOICE  0xFFB60E   /* 0-6 */
#define MECHANIC_BEST    0xFFB60A
#define MECHANICS_HIRED  0xFFB630   /* a byte per mechanic */
#define MONEY            0xFFB638   /* BCD long */
#define ROM_MECHANIC_PRICES 0x015602

static int command_unmasked;   /* a transfer ran: the original unmasks */

static void rect(uint32_t cmd, uint32_t src, int w, int h)
{
    vdp_copy_rect(cmd, src, w, h);
    command_unmasked = 1;
}

/* A rectangle described by its record: command, width-1, height-1,
   entries ($5622). */
static void rect_record(uint32_t rec)
{
    rect(rd32(rec), rec + 8, rd16(rec + 4) + 1, rd16(rec + 6) + 1);
}

/* A window: left edge, then the 2-cell body stepped a cell at a time
   ($6058). */
static void window_columns(uint32_t cmd, int steps)
{
    int k;
    vdp_copy_rect(cmd, 0x006088, 1, 6);
    for (k = 0; k < steps; k++) {
        cmd += 0x20000;
        vdp_copy_rect(cmd, 0x006094, 2, 6);
    }
}

static uint32_t mechanic_text_table(uint32_t normal, uint32_t alt)
{
    return rd16(0xFFC89C) ? alt : normal;
}

/* The selected mechanic's price line ($55D4). */
static void mechanic_price(void)
{
    rect(0x56920003, 0x015566, 20, 2);
    rect_record(rd32(mechanic_text_table(0x0157F4, 0x015C24) + 0x1C + 4 * rd16(MECHANIC_CHOICE)));
}

/* The mechanic panel: price, window, his words (whether hired or not)
   and the money ($5536). */
static void mechanic_panel(void)
{
    uint16_t m = rd16(MECHANIC_CHOICE);
    mechanic_price();
    window_columns(0x67100003, 0x12);
    rect(0x57920003, 0x015566, 20, 4);
    rect_record(rd32(mechanic_text_table(0x011504, 0x011CC4) + 4 * m +
                     32 * rd8(MECHANICS_HIRED + m)));
    wr32(VDP_CTRL, 0x55A00003);
    hex_tiles(0xFF0582, rd32(MONEY), 8, 0xE0, 0);
    for (m = 0; m < 8; m++)
        wr16(VDP_DATA, rd16(0xFF0582 + 2 * m));
}

/* Open the mechanic submenu ($5512): the first mechanic is always
   hired. */
static void mechanic_open(void)
{
    wr8(MECHANICS_HIRED, 1);
    vdp_copy(0x56820003, 0x0142AE, 8);
    command_unmasked = 1;
    mechanic_panel();
}

/* Slide the portraits a step (objects at $FFCC00 on). */
static void mechanic_slide(uint32_t first, uint16_t m, uint16_t dir)
{
    wr16(first + 0x40 * (m + 1) + 0x18, dir);
    mechanic_panel();
    mechanic_price();
    sound_command(0xA2);
}

/*
 * The mechanic submenu, one frame ($53CE): left/right pick one of seven
 * mechanics, B leaves, C/Start hires the one shown if the money is
 * enough (paid in BCD).
 */
static void mechanic_menu(void)
{
    uint8_t p = rd8(PAD1_PRESSED);
    uint16_t m = rd16(MECHANIC_CHOICE);
    command_unmasked = 0;
    if (!(rd8(SUBMENU + 1) & 1)) {
        wr8(SUBMENU + 1, rd8(SUBMENU + 1) | 1);
        mechanic_open();
        return;
    }
    if (p & 8) {
        if (++m == 7) {
            wr16(MECHANIC_CHOICE, 6);
            return;
        }
        wr16(MECHANIC_CHOICE, m);
        mechanic_slide(0xFFCBC0, m, 1);
    } else if (p & 4) {
        if (m-- == 0) {
            wr16(MECHANIC_CHOICE, 0);
            return;
        }
        wr16(MECHANIC_CHOICE, m);
        mechanic_slide(0xFFCC00, m, 0xFFFF);
    } else if (p & 0x10) {
        wr16(SUBMENU, 0);
        vdp_copy(0x56820003, 0x01429E, 8);
        command_unmasked = 1;
    } else if (p & 0xA0) {
        if (rd32(ROM_MECHANIC_PRICES + 4 * m) > rd32(MONEY)) {
            sound_command(0xA3);
            return;
        }
        if (!(rd8(MECHANICS_HIRED + m) & 1)) {
            int x = 0, k;
            wr8(MECHANICS_HIRED + m, rd8(MECHANICS_HIRED + m) | 1);
            for (k = 3; k >= 0; k--)
                wr8(MONEY + k, bcd_subx(rd8(MONEY + k), rd8(ROM_MECHANIC_PRICES + 4 * m + k), &x));
            mechanic_open();
        }
        if ((int16_t)m > (int16_t)rd16(MECHANIC_BEST)) {
            wr16(MECHANIC_BEST, m);
            sound_command(0xA4);
        }
    }
}

void mechanic_menu_68k(void)
{
    mechanic_menu();
    if (command_unmasked)
        set_sr(get_sr() & 0xF8FF);
}
