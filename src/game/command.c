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
static void window_frame(uint32_t cmd, int steps, uint32_t left, uint32_t body, int height)
{
    int k;
    vdp_copy_rect(cmd, left, 1, height);
    for (k = 0; k < steps; k++) {
        cmd += 0x20000;
        vdp_copy_rect(cmd, body, 2, height);
    }
}

void window_columns(uint32_t cmd, int steps)
{
    window_frame(cmd, steps, 0x006088, 0x006094, 6);
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

/* --------------------------------------------------------------- end */


#define PASSWORD         0xFFB658   /* 28 character codes */

/* A row of 14 password characters, each followed by a blank ($5872). */
static void password_row(uint32_t cmd, uint32_t codes)
{
    int k;
    wr32(VDP_CTRL, cmd);
    for (k = 0; k < 14; k++) {
        uint8_t c = rd8(codes + k);
        wr16(VDP_DATA, (uint16_t)(c + 0x614 + rd16(0x005224 + 2 * c)));
        wr16(VDP_DATA, 0);
    }
    command_unmasked = 1;
}

/* Two check bytes over the 24 codes: XOR of the even and odd codes,
   rotated together as a 16-bit pair with feedback ($51DC). */
void password_check(uint8_t *hi, uint8_t *lo)
{
    uint8_t d1 = 0, d2 = 0;
    int k;
    for (k = 0; k < 12; k++) {
        int x, c;
        d2 ^= rd8(PASSWORD + 2 * k);
        d1 ^= rd8(PASSWORD + 2 * k + 1);
        x = d1 & 1;
        d1 >>= 1;
        c = d2 & 1;
        d2 = (uint8_t)(x << 7 | d2 >> 1);
        if (c) {
            d1 ^= 0x88;
            d2 ^= 0x10;
        }
    }
    *hi = d1;
    *lo = d2;
}

/*
 * The password ($57D0): 16 game state bytes (codes from $B0 up count as
 * $23), four more split into nibbles with position offsets, and the
 * check bytes as four more codes; drawn in two rows.
 */
static void password_show(void)
{
    uint32_t ptr = 0x0050E6, out = PASSWORD;
    uint8_t hi, lo;
    int k;
    for (k = 0; k < 16; k++, ptr += 4) {
        uint8_t c = rd8(rd32(ptr));
        wr8(out++, c >= 0xB0 ? 0x23 : c);
    }
    for (k = 3; k >= 0; k--, ptr += 4) {
        uint8_t c = rd8(rd32(ptr));
        wr8(out++, (uint8_t)((c >> 4) + 6 + 3 * k));
        wr8(out++, (uint8_t)((c & 0xF) + 10 + 3 * k));
    }
    password_check(&hi, &lo);
    wr8(PASSWORD + 0x18, (uint8_t)((hi >> 4) + 0x0B));
    wr8(PASSWORD + 0x19, (uint8_t)((hi & 0xF) + 0x0D));
    wr8(PASSWORD + 0x1A, (uint8_t)((lo >> 4) + 0x0F));
    wr8(PASSWORD + 0x1B, (uint8_t)((lo & 0xF) + 0x11));
    password_row(0x5B940003, PASSWORD);
    password_row(0x5C140003, PASSWORD + 0x0E);
}

/* Open the end submenu: the sponsor's message ($5A68). */
static void end_open(void)
{
    rect(0x5B120003, 0x015566, 30, 4);
    vdp_copy(0x5A020003, 0x0142CC, 7);
    rect_record(rd32(mechanic_text_table(0x0124A0, 0x01325C) + 4 * rd16(0xFFB610) +
                     32 * rd16(0xFFB612)));
}

/* The end submenu, one frame ($5A38): shows the password; C/Start fades
   out and restarts the game. */
static void end_menu(void)
{
    command_unmasked = 0;
    if (!(rd8(SUBMENU + 1) & 1)) {
        wr8(SUBMENU + 1, rd8(SUBMENU + 1) | 1);
        end_open();
        return;
    }
    if (rd8(PAD1_PRESSED) & 0xA0) {
        fade_out();           /* fade out (waits for VBlanks) */
        wr16(0xFFC722, 0);
        wr16(0xFFC704, 0);
        return;
    }
    vdp_copy(0x5B120003, 0x00613E, 8);
    password_show();
}

void end_menu_68k(void)
{
    end_menu();
    if (command_unmasked)
        set_sr(get_sr() & 0xF8FF);
}

/* Original mode's parts screen is its own game mode ($53C2). */
static void parts_menu(void)
{
    wr16(0xFFC722, 0);
    wr16(0xFFC704, 0x28);
}

void parts_menu_68k(void) { parts_menu(); }

/* -------------------------------------------------------------- race */

#define RIVAL            0xFFB606   /* 0-6: the rival raced against */
#define RIVAL_TEXT       0xFFB608   /* 3: beaten, ready to move up */
#define SPONSOR          0xFFB610
#define RACES            0xFFB644
#define WINS             0xFFB646

/* Rival and sponsor names ($60AC). */
static void race_names(void)
{
    uint32_t names = mechanic_text_table(0x0157F4, 0x015C24);
    rect(0x50A80003, 0x015566, 20, 2);
    rect_record(rd32(names + 4 * rd16(RIVAL)));
    rect(0x5A120003, 0x015566, 20, 2);
    rect_record(rd32(names + 0x38 + 4 * rd16(SPONSOR)));
}

/* The rival's panel and words ($567C). */
static void rival_panel(void)
{
    vdp_copy(0x509A0003, 0x014294, 5);
    command_unmasked = 1;
    window_columns(0x61260003, 0x12);
    rect(0x51A80003, 0x015566, 20, 4);
    rect_record(rd32(mechanic_text_table(0x010AB6, 0x010FA6) + 4 * rd16(RIVAL) +
                     32 * rd16(RIVAL_TEXT)));
}

static void race_start(void)
{
    wr16(0xFFC82C, 0xFFFF);
    wr16(0xFFC8B6, 0);
    wr16(0xFFC85A, rd16(RIVAL));
    wr16(0xFFC722, 0);
    wr16(0xFFC704, 0x0C);          /* music select, then the race */
}

/* Portraits sliding in or out: rival at $FFCE00 on, sponsor at $FFCEC0
   on (handler $4C). */
static void portrait(uint32_t obj, int16_t x, uint16_t y, uint16_t map)
{
    wr16(obj, 0x4C);
    wr16(obj + 4, (uint16_t)x);
    wr16(obj + 0x14, y);
    wr16(obj + 0x24, map);
}

static void results_clear(void)
{
    wr16(WINS, 0);
    wr16(RACES, 0);
}

/* Beaten 5 times: the next rival and sponsor slide in ($58B2); after the
   last one the game is over. */
static void rival_next(void)
{
    rect(0x54B80003, 0x015566, 11, 11);
    if (rd16(RIVAL) == 6) {
        fade_out();           /* fade out (waits for VBlanks) */
        wr16(0xFFC722, 0);
        wr16(0xFFC704, 0);
        return;
    }
    wr16(RIVAL, rd16(RIVAL) + 1);
    wr16(SPONSOR, rd16(SPONSOR) + 1);
    results_clear();
    portrait(0xFFCE80, -0x20, 0xB8, rd16(0x005FF2 + 2 * rd16(RIVAL)));
    wr16(0xFFCE40 + 0x18, 1);
    portrait(0xFFCF40, -0x78, 0x150, rd16(0x00601C + 2 * rd16(SPONSOR)));
    wr16(0xFFCF00 + 0x18, 1);
    wr16(0xFFB612, 0);
    end_open();
    wr16(RIVAL_TEXT, 0);
    rival_panel();
    race_names();
}

/* Lost 5 times: back to the previous rival ($5976). */
static void rival_previous(void)
{
    if (rd16(RIVAL)) {
        wr16(RIVAL, rd16(RIVAL) - 1);
        wr16(SPONSOR, rd16(SPONSOR) - 1);
        portrait(0xFFCE00, 0, 0xB8, rd16(0x005FF2 + 2 * rd16(RIVAL)));
        wr16(0xFFCE00 + 0x18, 0xFFFF);
        portrait(0xFFCEC0, -0x58, 0x150, rd16(0x00601C + 2 * rd16(SPONSOR)));
        wr16(0xFFCEC0 + 0x18, 0xFFFF);
        race_names();
    }
    results_clear();
    wr16(0xFFB612, 0);
    end_open();
    wr16(RIVAL_TEXT, 0);
    rect(0x54B80003, 0x015566, 11, 11);
}

/*
 * Entering the race submenu ($5700): after 5 wins the rival is beaten
 * (and beating the last one gives everything: best mechanic, all
 * upgrades, new parts, $99999999, and the password); after 5 losses
 * the previous rival comes back; otherwise straight to the race.
 */
static void race_open(void)
{
    static const uint16_t levels[6] = { 5, 5, 4, 3, 3, 4 };
    int k;
    if (rd16(WINS) == 5) {
        wr16(0xFFB612, 4);
        end_open();
        wr16(RIVAL_TEXT, 3);
        if (rd16(RIVAL) != 6 || rd16(WINS) != 5)
            return;
        wr16(MECHANIC_BEST, 6);
        for (k = 0; k < 6; k++)
            wr16(0xFFB618 + 2 * k, levels[k]);
        for (k = 0; k < 6; k++)
            wr16(0xFFB648 + 2 * k, 0x0F);
        wr32(MONEY, 0x99999999);
        vdp_copy(0x5B120003, 0x00613E, 8);
        password_show();
        return;
    }
    if ((uint16_t)(rd16(RACES) - rd16(WINS)) == 5) {
        rival_previous();
        return;
    }
    race_start();
}

/* The race submenu, one frame ($5636). */
static void race_menu(void)
{
    command_unmasked = 0;
    if (!(rd8(SUBMENU + 1) & 1)) {
        wr8(SUBMENU + 1, rd8(SUBMENU + 1) | 1);
        race_open();
        rival_panel();
        return;
    }
    if (!(rd8(PAD1_PRESSED) & 0xA0))
        return;
    if (rd16(RIVAL_TEXT) == 3)
        rival_next();
    else
        race_start();
}

void race_menu_68k(void)
{
    race_menu();
    if (command_unmasked)
        set_sr(get_sr() & 0xF8FF);
}

/* ------------------------------------------------ 68000 interfaces */

void window_columns_68k(void)
{
    uint32_t cmd = cpu.d[7];
    int steps = (uint16_t)cpu.d[4] + 1;
    window_columns(cmd, steps);
    cmd += 0x20000u * steps + 0x3000000;
    cpu.d[7] = cmd;
    f_add(4, 0x800000, cmd - 0x800000);
    set_dn(4, 2, 0xFFFF);
}

void race_names_68k(void)
{
    command_unmasked = 0;
    race_names();
    set_sr(get_sr() & 0xF8FF);
}

void password_check_68k(void)
{
    uint8_t hi, lo;
    password_check(&hi, &lo);
    set_dn(1, 1, hi);
    set_dn(2, 1, lo);
}

/* A taller window ($5182). */
void window_tall(uint32_t cmd, int steps)
{
    window_frame(cmd, steps, 0x0051B2, 0x0051C0, 7);
}

void window_tall_68k(void)
{
    uint32_t cmd = cpu.d[7];
    int steps = (uint16_t)cpu.d[4] + 1;
    window_tall(cmd, steps);
    cpu.d[7] = cmd + 0x20000u * steps + 0x3800000;
    set_dn(4, 2, 0xFFFF);
}

/* A 6x6 picture frame ($5FD2). */
void picture_frame_68k(void)
{
    vdp_copy_rect(cpu.d[7], 0x0141C0, 6, 6);
}

void rect_record_68k(void)
{
    rect_record(cpu.a[6]);
    set_dn(5, 2, 0xFFFF);
    set_sr(get_sr() & 0xF8FF);
}

/* ------------------------------------------------------------ frame */

/*
 * One command screen frame after the VBlank ($52AE): objects, then the
 * open submenu, or the command list: up/down move the cursor (4
 * commands), C/Start opens one.
 */
static void command_frame(void)
{
    uint16_t sub, row;
    uint8_t p;
    objects_update();
    sub = rd16(SUBMENU);
    if (sub) {
        switch (sub & 0xFC) {
        case 4: parts_menu(); break;
        case 8: mechanic_menu(); break;
        case 12: race_menu(); break;
        case 16: end_menu(); break;
        }
        return;
    }
    command_unmasked = 0;
    command_cursor_blink();
    p = rd8(PAD1_PRESSED);
    row = rd16(COMMAND_ROW);
    if (p & 1) {
        wr16(COMMAND_ROW, row == 0 ? 3 : row - 1);
        sound_command(0xA2);
    } else if (p & 2) {
        wr16(COMMAND_ROW, row == 3 ? 0 : row + 1);
        sound_command(0xA2);
    } else if (p & 0xA0) {
        wr16(SUBMENU, (uint16_t)((row + 1) * 4));
        command_cursor();
        sound_command(0xA2);
    }
}

void command_frame_68k(void)
{
    command_frame();
    set_sr(get_sr() & 0xF8FF);
}

/* ------------------------------------------------------------ setup */

#define MECHANIC_RANK    0xFFB60A
#define RACE_TIME        0xFFB63C   /* BCD long: the last race's time, 0 = none */
#define RIVAL_TIME       0xFFB640   /* BCD long */
#define COMMAND_FLAGS    0xFFB655   /* bit 0: results already counted,
                                       bit 1: a race was run (parts wear) */
#define PARTS_WEAR       0xFFB648   /* six words, negative = not fitted */

/* Money += the sponsor's prize in column `col` (BCD longs, 16 bytes per
   sponsor at $15784), unless the results were counted already; returns
   the carry out. */
static int prize_add(int col)
{
    uint32_t src = 0x015784 + 16 * rd16(SPONSOR) + 4 * col;
    int k, x = 0;
    if (rd8(COMMAND_FLAGS) & 1)
        return 0;
    for (k = 3; k >= 0; k--)
        wr8(MONEY + k, bcd_addx(rd8(src + k), rd8(MONEY + k), &x));
    return x;
}

/*
 * The command screen's setup, first part ($5AD6, with interrupts
 * masked): the frame, the result of the race just run (beating the
 * rival's time by a margin, narrowly, or losing; each pays its prize and
 * picks the rival's and sponsor's words), the time and the margin, wins
 * and losses, graphics, and the rival's panel. Returns the rival's words
 * (a rectangle record), drawn next.
 */
static uint32_t command_setup(void)
{
    uint32_t rec;
    int k;
    screen_reset();
    objects_clear();
    vdp_fill(0x40000000, 0, 0x20);
    vdp_fill(0x60000003, 0xA358, 0x700);
    vdp_copy_rect(0x64B80003, 0x01403E, 11, 11);
    for (rec = 0x0156E2, k = 0; k < 4; k++, rec += 12)
        vdp_copy(rd32(rec), rd32(rec + 8), (uint16_t)rd32(rec + 4) + 1);
    if (!rd32(RACE_TIME)) {
        vdp_copy(0x57BA0003, 0x015566, 9);
    } else {
        uint32_t margin;
        if (!(rd8(COMMAND_FLAGS) & 1))
            wr16(RACES, rd16(RACES) + 1);
        margin = rd32(RACE_TIME) - rd32(RIVAL_TIME);
        if (rd32(RACE_TIME) < rd32(RIVAL_TIME)) {
            wr16(RIVAL_TEXT, 1);
            wr16(0xFFB612, 3);
            if (prize_add(0))
                wr32(MONEY, 0x99999999);
            if (!(rd8(COMMAND_FLAGS) & 1))
                wr16(WINS, rd16(WINS) + 1);
            vdp_copy(0x57BA0003, 0x015758, 9);
        } else if (margin < 0x1000) {
            wr16(RIVAL_TEXT, 2);
            wr16(0xFFB612, 2);
            prize_add(1);
        } else {
            wr16(RIVAL_TEXT, 2);
            wr16(0xFFB612, 1);
            prize_add(2);
        }
        time_tiles(RIVAL_TIME + 1, 0xFFC83E);
        vdp_copy(0x56BC0003, 0xFFC83E, 8);
        if (!rd8(RACE_TIME)) {
            time_tiles(RACE_TIME + 1, 0xFFC83E);
            vdp_copy(0x55BC0003, 0xFFC83E, 8);
        } else {
            vdp_copy(0x55BE0003, 0x01576A, 6);
        }
    }
    wr32(VDP_CTRL, 0x593C0003);
    wr16(VDP_DATA, rd16(WINS) + 0xC614);
    wr32(VDP_CTRL, 0x59480003);
    wr16(VDP_DATA, rd16(RACES) - rd16(WINS) + 0x8614);
    vdp_copy_rect(0x60820003, 0x01403E, 11, 11);
    vdp_copy_rect(0x50860003, 0x014130, 8, 9);
    wr32(VDP_CTRL, 0x40400000);
    nemesis_to_vdp(0x04A3B0);
    wr32(VDP_CTRL, 0x62400001);
    nemesis_to_vdp(0x04BC00);
    palette_load(0x014208);
    vdp_copy(0x509A0003, 0x01428A, 5);
    vdp_copy_rect(rd32(0x0154FA), 0x0141C0, 6, 6);
    portrait(0xFFCE40, -0x20, 0xB8, rd16(0x005FF2 + 2 * rd16(RIVAL)));
    window_columns(0x61260003, 0x12);
    vdp_copy_rect(0x51A80003, 0x015566, 20, 4);
    return rd32(mechanic_text_table(0x010AB6, 0x010FA6) + 4 * rd16(RIVAL) + 32 * rd16(RIVAL_TEXT));
}

void command_setup_68k(void) { cpu.a[6] = command_setup(); }

/*
 * The second part ($5DB8): the mechanics' row (the hired ones raised),
 * the names, the sponsor's panel and words, the password after the last
 * rival's fifth win, the command list and the money.
 */
static void command_setup_2(void)
{
    int k;
    vdp_copy(0x56820003, 0x01429E, 8);
    vdp_copy_rect(rd32(0x0154FE), 0x0141C0, 6, 6);
    for (k = 6; k >= 0; k--)
        portrait(0xFFCDC0 - 0x40 * (6 - k), k < rd16(MECHANIC_RANK) ? -0x58 : -0x78, 0x118,
                 rd16(0x00600E + 2 * k));
    race_names();
    vdp_copy(0x5A020003, 0x0142BE, 7);
    vdp_copy_rect(rd32(0x015502), 0x0141C0, 6, 6);
    portrait(0xFFCF00, -0x78, 0x150, rd16(0x00601C + 2 * rd16(SPONSOR)));
    window_columns(0x6A900003, 0x1D);
    if (rd16(RIVAL) == 6 && rd16(WINS) == 5)
        password_show();
    rect_record(rd32(mechanic_text_table(0x0124A0, 0x01325C) + 4 * rd16(SPONSOR) +
                     32 * rd16(0xFFB612)));
    vdp_copy_rect(0x64980003, 0x013F86, 16, 5);
    vdp_copy(0x559C0003, 0x014026, 12);
    wr32(VDP_CTRL, 0x55A00003);
    hex_tiles(0xFF0582, rd32(MONEY), 8, 0xE0, 0);
    hud_eight_tiles();
    sound_command(0x81);
}

void command_setup_2_68k(void) { command_setup_2(); }

/* Parts wear after a race ($5F3E): each fitted part loses its rate from
   $5FE6 (rates count fitted parts only), floor 0. Returns a random word. */
static uint16_t parts_wear(void)
{
    uint32_t rate = 0x005FE6;
    int k;
    for (k = 0; k < 6; k++) {
        int16_t v = (int16_t)rd16(PARTS_WEAR + 2 * k);
        if (v >= 0) {
            v = (int16_t)(v - rd16(rate));
            rate += 2;
            if (v < 0)
                v = 0;
        }
        wr16(PARTS_WEAR + 2 * k, (uint16_t)v);
    }
    return rng_next();
}

void parts_wear_68k(void)
{
    uint32_t seed = rd32(RNG_SEED);
    uint16_t r;
    if ((seed & 0xFFFF) == 0)
        seed = 0x2A6D365B;
    r = parts_wear();
    cpu.d[0] = (seed & 0xFFFF0000u) | r;
    set_dn(1, 2, (uint16_t)(rd16(MECHANIC_RANK) << 2));
}

/* Then, from the random bits the mechanic's rank lets through ($5F8C):
   any set, every fitted part gains a point (up to 15), else loses one. */
static void parts_wear_adjust(uint16_t r)
{
    int k;
    wr16(0xFFB656, r);
    for (k = 0; k < 6; k++) {
        uint16_t v = rd16(PARTS_WEAR + 2 * k);
        if (!(v & 0x8000)) {
            if (r && v != 0x0F)
                v++;
            else if (!r && v)
                v--;
        }
        wr16(PARTS_WEAR + 2 * k, v);
    }
    wr8(COMMAND_FLAGS, rd8(COMMAND_FLAGS) & ~2);
}

void parts_wear_adjust_68k(void) { parts_wear_adjust((uint16_t)cpu.d[0]); }

/* Game mode $24 ($6D16): the setup, or one frame. */
void mode_command(void)
{
    if (mode_first_call()) {
        rect_record(command_setup());
        set_sr(get_sr() & 0xF8FF);       /* the rectangle code unmasks */
        command_setup_2();
        wr16(SUBMENU, 0);
        wr16(MECHANIC_CHOICE, rd16(MECHANIC_RANK));
        wr16(COMMAND_ROW, 0);
        wr16(0xFFC70E, 0);
        wr16(0xFFC710, 1);
        wr16(0xFFC722, 4);
        wr8(COMMAND_FLAGS, rd8(COMMAND_FLAGS) & ~1);
        if (rd8(COMMAND_FLAGS) & 2) {
            uint16_t r = parts_wear();
            parts_wear_adjust(r & (0xFF00 >> (rd16(MECHANIC_RANK) + 1)) & 0xFF);
        }
        wait_vblank();
        display_on();
        return;
    }
    wait_vblank();
    command_frame();
    set_sr(get_sr() & 0xF8FF);
}
