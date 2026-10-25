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
