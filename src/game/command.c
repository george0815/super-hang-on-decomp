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

static void window_columns(uint32_t cmd, int steps)
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

#include "sho_funcs.h"

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
static void password_check(uint8_t *hi, uint8_t *lo)
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
        call68k(f_006DD4);           /* fade out (waits for VBlanks) */
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
        call68k(f_006DD4);           /* fade out (waits for VBlanks) */
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
void window_tall_68k(void)
{
    uint32_t cmd = cpu.d[7];
    int steps = (uint16_t)cpu.d[4] + 1;
    window_frame(cmd, steps, 0x0051B2, 0x0051C0, 7);
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
