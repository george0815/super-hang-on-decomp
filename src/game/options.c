/* The options screen (game mode $30): difficulty, time, language, sound
   test. */

#include "game.h"
#include "sho_funcs.h"

#define PAD1_PRESSED     0xFFC707
#define OPTION_ROW       0xFFC70E   /* 0-3 */
#define SOUND_TEST       0xFFC710   /* 0-$18; BCD-ish byte at $FFC711 */
#define DIFFICULTY       0xFF053C   /* 0-3 */
#define TIME_ADJUST      0xFF053A   /* 0-3 */
#define LANGUAGE         0xFFC89C   /* 0 English, $80 Japanese */

static int pad_lr(uint8_t p)
{
    return p & 8 ? 1 : p & 4 ? -1 : 0;
}

/*
 * One options frame after the VBlank ($6826): up/down pick a row
 * (leaving the sound test stops the sound), left/right change it, A/B/C
 * play the sound test's sound; every row is redrawn with the selected
 * label lit; Start fades out to the logo.
 */
static void options_frame(void)
{
    static const struct { uint32_t cmd, normal, lit; int words; } labels[4] = {
        { 0x53180003, 0x006AEE, 0x006B40, 15 },
        { 0x55A00003, 0x006B0C, 0x006B5E, 9 },
        { 0x58200003, 0x006B32, 0x006B84, 7 },
        { 0x5A9E0003, 0x006B1E, 0x006B70, 10 },
    };
    uint8_t p = rd8(PAD1_PRESSED);
    uint16_t row = rd16(OPTION_ROW);
    int d, k;
    if (p & 3) {
        uint8_t sound = row == 3 ? 0xB1 : 0xA2;
        row = (uint16_t)((row + (p & 2 ? 1 : -1)) & 3);
        wr16(OPTION_ROW, row);
        sound_command(sound);
    }
    if (row == 3) {
        if ((d = pad_lr(p)) != 0) {
            int16_t n = (int16_t)(rd16(SOUND_TEST) + d);
            if (n < 0)
                n = 0x18;
            else if (n >= 0x19)
                n = 0;
            wr16(SOUND_TEST, (uint16_t)n);
        }
        if (p & 0x70) {
            sound_command(0xB1);
            call68k(f_006D4E);           /* one frame for the stop to land */
            sound_command(rd8(0x006B92 + rd16(SOUND_TEST)));
        }
    } else if (row == 2) {
        if (p & 0x0C) {
            wr16(LANGUAGE, rd16(LANGUAGE) ? 0 : 0x80);
            sound_command(0xA2);
        }
    } else if ((d = pad_lr(p)) != 0) {
        uint32_t opt = row ? TIME_ADJUST : DIFFICULTY;
        wr16(opt, (uint16_t)((rd16(opt) + d) & 3));
        sound_command(0xA2);
    }
    for (k = 0; k < 4; k++)
        vdp_copy(labels[k].cmd, labels[k].normal, labels[k].words);
    vdp_copy(labels[row].cmd, labels[row].lit, labels[row].words);
    wr32(VDP_CTRL, 0x5BA60003);
    hex_tiles(0xFF0582, rd8(SOUND_TEST + 1), 2, 0x20, 0);
    wr16(VDP_DATA, rd16(0xFF0582));
    wr16(VDP_DATA, rd16(0xFF0584));
    vdp_copy(0x54200003, rd32(0x006A7E + 4 * rd16(DIFFICULTY)), 7);
    vdp_copy(0x56A00003, rd32(0x006A7E + 4 * rd16(TIME_ADJUST)), 7);
    vdp_copy(0x59200003, rd32(0x006AC6 + (rd16(LANGUAGE) ? 4 : 0)), 8);
    if (p & 0x80) {
        sound_command(0xB1);
        call68k(f_006DD4);
        wr16(0xFFC722, 0);
        wr16(0xFFC704, 0);
    }
}

void options_frame_68k(void) { options_frame(); }

/* The options screen's setup ($6A3E, with interrupts masked). */
static void options_setup(void)
{
    screen_reset();
    vdp_fill(0x40000000, 0, 0x20);
    palette_load(0x008E56);
    wr16(0xFFD3E0, 0x0220);
    wr16(0xFFC70E, 0);
    wr16(0xFFC710, 0);
}

void options_setup_68k(void) { options_setup(); }

/* Game mode $30 ($6D22): the setup, or one frame. */
void mode_options_68k(void)
{
    if (mode_first_call()) {
        options_setup();
        mode_setup_done(4);
        return;
    }
    wait_vblank();
    options_frame();
}
