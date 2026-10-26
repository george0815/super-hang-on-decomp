/* The title screen (game mode 4): intro sequence and menu effects. */

#include "game.h"

#define TITLE_STEP       0xFFC72C   /* byte: intro step x4 */
#define TITLE_COUNT      0xFFC712
#define TITLE_TIMER      0xFFC714
#define TITLE_PHASE      0xFFC716
#define MENU_STATE       0xFFB600   /* bit 1: second menu; bits 0/2: cursor */
#define FRAME_COUNTER_BYTE 0xFFC726

/* Copy colours into the palette buffer and mark it ($738A). */
static void palette_words(uint32_t dst, uint32_t src, int count)
{
    int k;
    for (k = 0; k < count; k++)
        wr16(dst + 2 * k, rd16(src + 2 * k));
    wr8(VDP_PALETTE_DIRTY, rd8(VDP_PALETTE_DIRTY) | 1);
}

/*
 * Step 0: launch the title bike as a thrown object ($75EA): handler 4
 * with a fixed fall, its x from a ROM table.
 */
static void title_launch(void)
{
    uint8_t step = rd8(TITLE_STEP);
    uint32_t obj = 0xFFCE40 + 16u * step;
    if (rd16(obj))
        return;
    wr16(obj, 4);
    wr8(obj + 0x2D, (uint8_t)((int8_t)step >> 2));
    wr32(obj + 0x18, 0x100000);
    wr32(obj + 0x1C, 0x17000);
    wr16(obj + 0x20, 4);
    wr16(obj + 4, rd16(0x00763C + (uint8_t)((int8_t)step >> 1)));
    wr16(obj + 8, 0xFFB4);
}

void title_launch_68k(void) { title_launch(); }

/*
 * Step 4: wipe the first VRAM tile in a growing checker of colour 1, then
 * clear it again ($76B4), one stage every 4 counts.
 */
static int wipe_d1;
static void title_wipe(void)
{
    uint16_t n;
    int k;
    wipe_d1 = 0;
    wr16(TITLE_TIMER, rd16(TITLE_TIMER) - 1);
    if ((int16_t)rd16(TITLE_TIMER) >= 0)
        return;
    n = (uint16_t)(rd16(TITLE_COUNT) + 1);
    wr16(TITLE_COUNT, n);
    if (n > 0x20) {
        wr32(VDP_CTRL, 0x40000000);
        for (k = 0; k <= (n >> 2) - 8; k++)
            wr32(VDP_DATA, 0);
    } else {
        if (n & 3)
            return;
        wr32(VDP_CTRL, 0x40000000);
        for (k = 0; k <= (n >> 2); k++)
            wr32(VDP_DATA, k & 1 ? 0x00110011 : 0x11001100);
        wipe_d1 = 1;
    }
    if (rd16(TITLE_COUNT) == 0x40) {
        wr8(TITLE_STEP, rd8(TITLE_STEP) + 4);
        wr32(TITLE_COUNT, 0);
        wr32(TITLE_PHASE, 0);
    }
}

void title_wipe_68k(void)
{
    title_wipe();
    if (wipe_d1)
        cpu.d[1] = 0x11001100;
}

/*
 * Step 8: flash two colour ramps, alternating every count, ten counts
 * ($7740); then wait for the demo.
 */
static int flash_d1;
static void title_flash(void)
{
    uint16_t phase;
    int odd;
    flash_d1 = 0;
    if (rd8(TITLE_COUNT + 1) >= 0x0A) {
        wr8(TITLE_STEP, rd8(TITLE_STEP) + 4);
        wr16(TITLE_COUNT, 0xB4);
        wr16(0xFFC71C, 0);
        return;
    }
    odd = rd8(TITLE_COUNT + 1) & 1;
    wr16(TITLE_TIMER, rd16(TITLE_TIMER) - 1);
    if ((int16_t)rd16(TITLE_TIMER) >= 0)
        return;
    wr16(TITLE_TIMER, 4);
    phase = (uint16_t)(rd16(TITLE_PHASE) + 2);
    if (phase >= (odd ? 0x0C : 0x0E)) {
        phase = 0;
        wr16(TITLE_COUNT, rd16(TITLE_COUNT) + 1);
    }
    wr16(TITLE_PHASE, phase);
    if (odd)
        palette_words(0xFFD3A4, 0x007928 + phase, 6);
    else
        palette_words(0xFFD3B0, 0x00790C + phase, 7);
    flash_d1 = 1;
}

void title_flash_68k(void)
{
    title_flash();
    if (flash_d1)
        cpu.d[1] = 0xFFFF;
}

/* Step 12: after a while, start the demo race ($75B0). */
static void title_wait(void)
{
    wr16(0xFFC82A, rd16(0xFFC82A) - 1);
    if ((int16_t)rd16(0xFFC82A) >= 0)
        return;
    wr16(0xFFC722, 0);
    wr16(0xFFC704, 0x14);
}

void title_wait_68k(void) { title_wait(); }

/* Cycle 12 colours of the title every 8 frames ($77C6). */
static int cycle_d1;
static void title_cycle(void)
{
    cycle_d1 = 0;
    wr16(0xFFC70E, rd16(0xFFC70E) - 1);
    if ((int16_t)rd16(0xFFC70E) >= 0)
        return;
    wr16(0xFFC70E, 7);
    wr16(0xFFC710, rd16(0xFFC710) - 2);
    if ((int16_t)rd16(0xFFC710) < 0)
        wr16(0xFFC710, 0x16);
    palette_words(0xFFD3E8, 0x0078DE + rd16(0xFFC710), 12);
    cycle_d1 = 1;
}

void title_cycle_68k(void)
{
    title_cycle();
    if (cycle_d1)
        cpu.d[1] = 0xFFFF;
}

/* The blinking menu cursor: two positions per menu, the selected one
   shown every other 16 frames ($7806). */
static void title_cursor(void)
{
    uint8_t m = rd8(MENU_STATE);
    int second = (m & 2) != 0;
    uint32_t top = second ? 0x57960003 : 0x579C0003, bottom = second ? 0x58960003 : 0x589C0003;
    wr32(VDP_CTRL, top);
    wr16(VDP_DATA, 0);
    wr32(VDP_CTRL, bottom);
    wr16(VDP_DATA, 0);
    if (!(rd8(FRAME_COUNTER_BYTE) & 0x10))
        return;
    wr32(VDP_CTRL, (m & (second ? 4 : 1)) ? bottom : top);
    wr16(VDP_DATA, 0xC349);
}

void title_cursor_68k(void) { title_cursor(); }

/* Copy 16 words of VRAM through $FFE900 ($7FAA). */
static void vram_copy16(uint32_t read_cmd, uint32_t write_cmd)
{
    int k;
    wr32(VDP_CTRL, read_cmd);
    for (k = 0; k < 16; k++)
        wr16(0xFFE900 + 2 * k, rd16(VDP_DATA));
    wr32(VDP_CTRL, write_cmd);
    for (k = 0; k < 16; k++)
        wr16(VDP_DATA, rd16(0xFFE900 + 2 * k));
}

void vram_copy16_68k(void) { vram_copy16(cpu.d[7], cpu.d[6]); }

/* ------------------------------------------------------ course select */

#define COURSE           0xFF060C
#define PAD1_PRESSED     0xFFC707

/* Start or C: on to the next mode; left/up and right/down pick one of
   the four courses ($7B38). */
static void course_select_input(void)
{
    uint8_t p = rd8(PAD1_PRESSED);
    int d;
    if (p & 0xA0) {
        wr16(0xFFC722, 0);
        wr16(0xFFC704, 0x0C);
        return;
    }
    if (p & 0x05)
        d = -1;
    else if (p & 0x0A)
        d = 1;
    else
        return;
    wr16(COURSE, (uint16_t)((rd16(COURSE) + d) & 3));
}

void course_select_input_68k(void)
{
    uint8_t p = rd8(PAD1_PRESSED);
    course_select_input();
    if (!(p & 0xA0))
        cpu.d[1] = (p & 0x05) ? 0xFFFFFFFF : 1;
}

/* The four course names in their colours, the selected one flashing
   white every 8 frames ($7B82). */
static void course_select_colours(void)
{
    wr8(VDP_PALETTE_DIRTY, rd8(VDP_PALETTE_DIRTY) | 1);
    wr32(0xFFD398, rd32(0x007BB4));
    wr32(0xFFD39C, rd32(0x007BB8));
    if (rd8(FRAME_COUNTER_BYTE) & 8)
        wr16(0xFFD398 + 2 * rd16(COURSE), 0x0ECA);
}

void course_select_colours_68k(void) { course_select_colours(); }

/* ------------------------------------------------------------ frame */

#define PAD1_HELD        0xFFC706

/* Any menu activity keeps the demo away for at least 2 seconds ($77F4). */
static void demo_delay(void)
{
    if (rd16(0xFFC82A) < 0x78)
        wr16(0xFFC82A, 0x78);
}

static void next_mode(uint16_t mode)
{
    wr16(0xFFC722, 0);
    wr16(0xFFC704, mode);
}

static void (*const title_steps[10])(void) = {
    title_launch, title_launch, title_launch, title_launch, title_launch,
    title_launch, title_launch, title_wipe, title_flash, title_wait,
};

/*
 * One title screen frame after the VBlank ($74A0). First menu: new game
 * or password (A held with Start: mode $30); second menu: arcade or
 * original mode. Then the colour cycling, the cursor, and the intro
 * steps with the objects once the start delay is over.
 */
static void title_frame(void)
{
    uint8_t p = rd8(PAD1_PRESSED), m = rd8(MENU_STATE);
    if (!(m & 2)) {
        if (p & 3) {
            demo_delay();
            wr8(MENU_STATE, m ^ 1);
        } else if (p & 0xA0) {
            if (rd8(PAD1_HELD) & 0x40) {
                next_mode(0x30);
                return;
            }
            demo_delay();
            if (m & 1) {
                next_mode(0x2C);               /* password */
                return;
            }
            wr8(MENU_STATE, m | 2);
            vdp_copy(0x579A0003, 0x007A8C, 13);
            vdp_copy(0x589A0003, 0x007A72, 13);
            return;
        }
    } else {
        if (p & 3) {
            demo_delay();
            wr8(MENU_STATE, m ^ 4);
        } else if (p & 0xA0) {
            int k;
            if (!(m & 4)) {
                next_mode(0x08);               /* arcade */
                return;
            }
            for (k = 0; k < 0x200; k += 4)     /* original mode: new save */
                wr32(MENU_STATE + k, 0);
            for (k = 0; k < 6; k++)
                wr16(0xFFB648 + 2 * k, 0x0F);
            next_mode(0x24);
            return;
        }
    }
    title_cycle();
    title_cursor();
    wr16(0xFFC71C, rd16(0xFFC71C) - 1);
    if ((int16_t)rd16(0xFFC71C) >= 0)
        return;
    title_steps[rd8(TITLE_STEP) / 4]();
    objects_update();
}

void title_frame_68k(void) { title_frame(); }

/* "PUSH START BUTTON", shown every other 32 frames ($7BBC). */
void push_start_blink(void)
{
    if (rd8(FRAME_COUNTER_BYTE) & 0x20)
        vdp_copy(0x5B960003, 0x007D10, 0x11);
    else
        vdp_fill(0x5B960003, 0, 0x11);
}

/* One course select frame after the VBlank ($7B24): the countdown, then
   on to music select when it runs out. */
static void course_select_frame(void)
{
    menu_countdown();
    if (rd8(0xFFC718)) {
        next_mode(0x0C);
        return;
    }
    course_select_input();
    course_select_colours();
    push_start_blink();
}

void course_select_frame_68k(void) { course_select_frame(); }

/* ------------------------------------------------------------ setup */

/*
 * The title screen's setup ($793E, with interrupts masked): the
 * background, menu graphics, logo tiles and texts, the palette, and the
 * demo after 3 seconds.
 */
static void title_setup(void)
{
    int k;
    screen_reset();
    wr16(SOUND_LOCKED, 0);
    wr16(0xFFC82C, 0);
    wr16(0xFFC8B6, 0);
    wr32(VDP_CTRL, 0x40000000);
    nemesis_to_vdp(0x01E07C);
    map_unpack(0x01E03C);
    music_graphics();
    wr16(0xFFD3C4, 0x00EE);
    wr32(VDP_CTRL, 0x62400001);
    nemesis_to_vdp(0x04BC00);
    objects_clear();
    wr16(TITLE_STEP, 0);
    vdp_copy_rect(0x62160003, 0xFFEBB0, 16, 3);
    vdp_copy_rect(0x5A1E0003, 0xFFEB60, 10, 4);
    vdp_fill(0x40000000, 0x1111, 0x20);
    vdp_copy(0x57A00003, 0x007ABC, 8);
    vdp_copy(0x58A00003, 0x007ACC, 8);
    vdp_copy(0x5CB60003, 0x007AA6, 11);
    wr32(VDP_CTRL, 0x66460003);
    wr32(VDP_DATA, 0xE631E62A);
    palette_load(0x0078D6);
    for (k = 0; k < 7; k++) {
        wr16(0xFFD3B0 + 2 * k, rd16(0x00790C + 2 * k));
        wr16(0xFFD3A2 + 2 * k, rd16(0x007926 + 2 * k));
    }
    wr16(0xFFC82A, 0xB4);                /* demo delay */
    wr16(0xFFC70E, 0);
    wr16(0xFFC710, 0);
    wr16(0xFFC71C, 0x23);
    wr16(MENU_STATE, 0);
}

void title_setup_68k(void) { title_setup(); }

/* Game mode 4 ($6CF6): the setup, or one frame. */
void mode_title_68k(void)
{
    if (mode_first_call()) {
        title_setup();
        mode_setup_done(4);
        return;
    }
    wait_vblank();
    title_frame();
}

/* The course select labels ($7C5E, 16 bytes apart): VRAM command, word
   count, text; drawn last to first. */
static const struct { uint32_t cmd; uint16_t count; uint32_t src; } course_labels[9] = {
    { 0x55A20003, 9, 0x007D9A }, { 0x568C0003, 9, 0x007D88 }, { 0x54B20003, 9, 0x007D76 },
    { 0x58A20003, 8, 0x007D66 }, { 0x55240003, 6, 0x007D5A }, { 0x560E0003, 6, 0x007D4E },
    { 0x54340003, 6, 0x007D42 }, { 0x58220003, 8, 0x007D32 }, { 0x51980003, 17, 0x007CEE },
};

/* The course select screen's setup ($7BD8, with interrupts masked);
   Africa first, a countdown of 16 and its music. */
static void course_select_setup(void)
{
    int k;
    screen_reset();
    wr32(VDP_CTRL, 0x40000000);
    nemesis_to_vdp(0x03DA6E);
    map_unpack(0x03D718);
    vdp_copy_rect(0x63060003, 0xFFEB00, 0x22, 0x12);
    for (k = 8; k >= 0; k--)
        vdp_copy(course_labels[k].cmd, course_labels[k].src, course_labels[k].count);
    palette_load(0x008E56);
    palette_load(0x03D704);
    wr16(0xFFD3E0, 0x0ECA);
    wr16(COURSE, 0);
    wr32(0xFFC718, 0x1601);              /* countdown 16 */
    sound_command(0x81);
}

void course_select_setup_68k(void) { course_select_setup(); }

/* Game mode 8 ($6CFA): the setup, or one frame. */
void mode_course_select_68k(void)
{
    if (mode_first_call()) {
        course_select_setup();
        mode_setup_done(4);
        return;
    }
    wait_vblank();
    course_select_frame();
}
