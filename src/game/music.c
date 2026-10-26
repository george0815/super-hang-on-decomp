/* The music select screen (game mode $C). */

#include "game.h"
#include "sho_funcs.h"

#define PAD1_PRESSED     0xFFC707
#define MUSIC_CHOICE     0xFF0516   /* 0-3 */
#define FRAME_COUNTER_BYTE 0xFFC726
#define ROM_MUSIC_SOUNDS 0x007E2A   /* sound command per choice */

/*
 * Start or C: confirm, fade out and go on to course select; otherwise
 * up/down change the tune, which starts playing at once ($7DCC).
 */
static void music_select_input(void)
{
    uint8_t p = rd8(PAD1_PRESSED);
    int d;
    if (p & 0xA0) {
        sound_command(0xB0);
        call68k(f_006DD4);           /* fade out (waits for VBlanks) */
        wr16(0xFFC722, 0);
        wr16(0xFFC704, 0x10);
        return;
    }
    if (p & 1)
        d = -1;
    else if (p & 2)
        d = 1;
    else
        return;
    wr16(MUSIC_CHOICE, (uint16_t)((rd16(MUSIC_CHOICE) + d) & 3));
    wr8(0xFF0518, rd8(ROM_MUSIC_SOUNDS + rd16(MUSIC_CHOICE)));
    sound_command(rd8(0xFF0518));
}

void music_select_input_68k(void)
{
    uint8_t p = rd8(PAD1_PRESSED);
    music_select_input();
    if (!(p & 0xA0))
        cpu.d[1] = (p & 1) ? 0xFFFFFFFF : 1;
}

/* The four tune names from their RAM copy ($7F00). */
static void music_names(void)
{
    vdp_copy_rect(0x53880003, 0xFFEFC8, 0x1F, 0x0C);
}

void music_names_68k(void) { music_names(); set_dn(5, 2, 0xFFFF); }

/* Blank the selected name every other 8 frames ($7E42-$7E4E). */
static int music_blanked;
static void music_blink(uint16_t choice)
{
    static const uint32_t rows[4] = { 0x53880003, 0x55080003, 0x56880003, 0x58080003 };
    uint32_t cmd = rows[choice & 3];
    int k;
    music_blanked = 0;
    if (rd8(FRAME_COUNTER_BYTE) & 8)
        return;
    for (k = 0; k < 3; k++, cmd += 0x800000)
        vdp_fill(cmd, 0, 0x1F);
    music_blanked = 1;
}

static void music_blink_regs(void)
{
    if (music_blanked) {
        cpu.d[5] = 0;
        cpu.d[4] = 0xFFFF;
    }
}

void music_blink_0_68k(void) { music_blink(0); music_blink_regs(); }
void music_blink_1_68k(void) { music_blink(1); music_blink_regs(); }
void music_blink_2_68k(void) { music_blink(2); music_blink_regs(); }
void music_blink_3_68k(void) { music_blink(3); music_blink_regs(); }

/* Redraw the names with the selected one blinking ($7E2E). */
static void music_draw(void)
{
    music_names();
    music_blink((uint16_t)(int8_t)rd8(MUSIC_CHOICE + 1));
}

void music_draw_68k(void)
{
    music_draw();
    if (!music_blanked)
        set_dn(5, 2, 0xFFFF);
    music_blink_regs();
}

/*
 * The screen's graphics ($7F2E): four Nemesis sets, 25 tiles copied
 * within VRAM, and the palette.
 */
void music_graphics(void)
{
    uint32_t src = 0x200000, dst = 0x50600003;
    int k;
    wr32(VDP_CTRL, 0x7D800002);
    nemesis_to_vdp(0x01D948);
    wr32(VDP_CTRL, 0x42800003);
    nemesis_to_vdp(0x01DA32);
    wr32(VDP_CTRL, 0x5E000003);
    nemesis_to_vdp(0x01DDA2);
    wr32(VDP_CTRL, 0x40200000);
    nemesis_to_vdp(0x01DE10);
    for (k = 0; k < 25; k++, src += 0x200000, dst += 0x800000) {
        int i;
        wr32(VDP_CTRL, src);
        for (i = 0; i < 16; i++)
            wr16(0xFFE900 + 2 * i, rd16(VDP_DATA));
        wr32(VDP_CTRL, dst);
        for (i = 0; i < 16; i++)
            wr16(VDP_DATA, rd16(0xFFE900 + 2 * i));
    }
    palette_load(0x008E56);
}

void music_graphics_68k(void)
{
    music_graphics();
    cpu.d[0] = 0xFFFF;
    cpu.d[1] = 0;
}

/* One music select frame after the VBlank ($7DBA): when the countdown
   runs out, the tune is confirmed as with Start. */
static void music_select_frame(void)
{
    menu_countdown();
    if (rd8(0xFFC718)) {
        sound_command(0xB0);
        call68k(f_006DD4);
        wr16(0xFFC722, 0);
        wr16(0xFFC704, 0x10);
        return;
    }
    music_select_input();
    music_draw();
    push_start_blink();
}

void music_select_frame_68k(void) { music_select_frame(); }

/* Step the tune choice by d and play it ($7E08). */
static void music_step(int d)
{
    wr16(MUSIC_CHOICE, (uint16_t)((rd16(MUSIC_CHOICE) + d) & 3));
    wr8(0xFF0518, rd8(ROM_MUSIC_SOUNDS + rd16(MUSIC_CHOICE)));
    sound_command(rd8(0xFF0518));
}

/* The music select screen's setup ($7E8E, with interrupts masked); the
   first tune starts playing. */
static void music_select_setup(void)
{
    screen_reset();
    music_names();
    map_unpack(0x03D718);
    wr32(VDP_CTRL, 0x40000000);
    nemesis_to_vdp(0x03DA6E);
    vdp_copy(0x521C0003, 0x007F16, 12);
    palette_load(0x008E56);
    palette_load(0x03D70E);
    wr16(0xFFD3E0, 0x00AE);
    wr16(MUSIC_CHOICE, 0xFFFF);
    wr32(0xFFC718, 0x1101);              /* countdown 11 */
    music_step(1);
}

void music_select_setup_68k(void) { music_select_setup(); }

/* Game mode $0C ($6CFE): the setup, or one frame. */
void mode_music_select_68k(void)
{
    if (mode_first_call()) {
        music_select_setup();
        mode_setup_done(4);
        return;
    }
    wait_vblank();
    music_select_frame();
}
