/* Original mode's parts screen (game mode $28): drawing helpers. */

#include "game.h"

#define FRAME_COUNTER_BYTE 0xFFC726
#define PART             0xFFB616   /* selected part, 0-5 */
#define PART_LEVELS      0xFFB624   /* level shown per part */
#define PART_OWNED       0xFFB618   /* level fitted per part */
#define PART_WEAR        0xFFB648   /* negative: broken */
#define MECHANIC_BEST    0xFFB60A
#define MECHANIC_TEXT    0xFFB60C

static uint32_t text_table(uint32_t normal, uint32_t alt)
{
    return rd16(0xFFC89C) ? alt : normal;
}

static void rect_record(uint32_t rec)
{
    vdp_copy_rect(rd32(rec), rec + 8, rd16(rec + 4) + 1, rd16(rec + 6) + 1);
}

/* A part's box, normal or highlighted ($679A with $678C/$6794's art). */
static void part_box(uint16_t part, uint32_t art)
{
    vdp_copy_rect(rd32(0x01549A + 4 * part), art, 10, 6);
}

static void part_box_normal(uint16_t part) { part_box(part, 0x0143BA); }   /* $678C */
static void part_box_lit(uint16_t part) { part_box(part, 0x014432); }      /* $6794 */

/* The selected part's box: lit unless broken ($63DE). */
static void part_box_selected(void)
{
    uint16_t part = rd16(PART);
    if ((int16_t)rd16(PART_WEAR + 2 * part) >= 0)
        part_box_lit(part);
    else
        part_box_normal(part);
}

/* The part's name for a level (4 rows of 8) ($67B8). */
static uint32_t part_name_cmd;
static void part_name(uint16_t part, uint16_t level)
{
    part_name_cmd = rd32(0x0154B2 + 4 * part);
    vdp_copy_rect(part_name_cmd, rd32(text_table(0x01456A, 0x014D02) + 4 * part) + level * 64u, 8, 4);
}

/* The part's label, normal or ($60 on) highlighted ($67F0). */
static uint32_t part_label_last;
static void part_label(uint16_t part, uint16_t variant)
{
    uint32_t src = 0x0144AA + 16u * part + variant;
    vdp_copy(rd32(0x0154CA + 4 * part), src, 7);
    part_label_last = src + 12;
}

/* The mechanic's words for this part ($64D4). */
static void mechanic_words(void)
{
    rect_record(rd32(text_table(0x011544, 0x011D04) + 4 * rd16(MECHANIC_BEST) +
                     32 * rd16(MECHANIC_TEXT)));
}

/*
 * What the mechanic says about the selected part ($6404): by its wear
 * (broken or below 4, below 6, below 9) and whether the bike was
 * damaged in the last race ($FFB656), with a second line for some.
 */
static void mechanic_comment(void)
{
    uint16_t part = rd16(PART);
    int16_t wear = (int16_t)rd16(PART_WEAR + 2 * part);
    int damaged = rd16(0xFFB656) != 0;
    uint32_t cmd;
    vdp_copy_rect(0x51940003, 0x015566, 26, 3);
    wr16(MECHANIC_TEXT, 0);
    if (wear < 4)
        wr16(MECHANIC_TEXT, damaged ? 3 : 2);
    else if (wear < 6)
        wr16(MECHANIC_TEXT, damaged ? 2 : 1);
    else if (wear < 9 && damaged)
        wr16(MECHANIC_TEXT, 1);
    else {
        mechanic_words();
        return;
    }
    mechanic_words();
    cmd = rd32(rd32(text_table(0x016032, 0x0160BE) + 4 * rd16(MECHANIC_BEST)) + 4 * rd16(MECHANIC_TEXT));
    if (!cmd) {
        mechanic_words();
        return;
    }
    vdp_copy(cmd, 0x01614A + 16u * part, 7);
}

/* The bike picture: the selected part's sprite piece ($CDC0 objects)
   blinks in the colour of its level ($650A, $6556). */
static uint32_t part_object(uint16_t *index)
{
    uint16_t i = rd16(PART);
    if (i == 4)
        i = 1;
    *index = i;
    return 0xFFCDC0 + 0x40u * (i + 1);
}

static void part_piece_hide(void)
{
    uint16_t i;
    wr16(part_object(&i) + 0x24, 0);
}

static int piece_shown;
static void part_piece_blink(void)
{
    uint16_t i, level;
    uint32_t obj;
    piece_shown = 0;
    if (!(rd8(FRAME_COUNTER_BYTE) & 2)) {
        part_piece_hide();
        return;
    }
    obj = part_object(&i);
    level = rd16((rd16(PART) == 4 ? PART_OWNED : PART_LEVELS) + 2 * i);
    wr16(obj + 0x24, rd16(0x015506 + 16 * i + 2 * level));
    piece_shown = 1;
}

/* The selected part's label blinks every 8 frames ($657A, $6596). */
static void part_label_normal(void) { part_label(rd16(PART), 0); }

static void part_label_blink(void)
{
    part_label(rd16(PART), rd8(FRAME_COUNTER_BYTE) & 8 ? 0x60 : 0);
}

static void part_blink(void)         /* $6504 */
{
    part_piece_blink();
    part_label_blink();
}

/* ------------------------------------------------ 68000 interfaces */

static void label_regs(void)
{
    cpu.d[1] = (uint32_t)rd16(PART) << 2;
    f_logic(2, rd16(part_label_last));
    cpu.x = 0;
}

static void unmask(void) { set_sr(get_sr() & 0xF8FF); }

void part_box_normal_68k(void) { part_box_normal((uint16_t)cpu.d[0]); }
void part_box_lit_68k(void) { part_box_lit((uint16_t)cpu.d[0]); }
void part_box_68k(void) { part_box((uint16_t)cpu.d[0], cpu.a[6]); }
void part_box_selected_68k(void) { part_box_selected(); }
void part_box_unlit_68k(void) { part_box_normal(rd16(PART)); }

void part_name_68k(void)
{
    part_name((uint16_t)cpu.d[0], (uint16_t)cpu.d[2]);
    f_add(4, 0x800000, part_name_cmd + 3 * 0x800000);
}

void part_label_68k(void)
{
    part_label((uint16_t)cpu.d[0], (uint16_t)cpu.d[2]);
    f_logic(2, rd16(part_label_last));
    cpu.x = 0;
}

void mechanic_words_68k(void) { mechanic_words(); }
void mechanic_comment_68k(void) { mechanic_comment(); }

void part_piece_hide_68k(void) { part_piece_hide(); f_logic(2, 0); }

void part_piece_blink_68k(void)
{
    part_piece_blink();
    if (piece_shown)
        cpu.x = 0;
    else
        f_logic(2, 0);
}

void part_label_normal_68k(void) { part_label_normal(); unmask(); label_regs(); }
void part_label_blink_68k(void) { part_label_blink(); unmask(); label_regs(); }
void part_blink_68k(void) { part_blink(); unmask(); label_regs(); }

/* ------------------------------------------------------------ frame */

#define PAD1_PRESSED     0xFFC707
#define MONEY            0xFFB638
#define ROM_PART_PRICES  0x015622   /* per part 8 BCD longs, by level */

static void part_select(int16_t d)
{
    int16_t part = (int16_t)(rd16(PART) + d);
    part_piece_hide();
    part_label_normal();
    if (part > 5) part = 0;
    if (part < 0) part = 5;
    wr16(PART, (uint16_t)part);
    mechanic_comment();
    sound_command(0xA2);
}

/* Show another level of the selected part. */
static void part_level(int16_t d)
{
    uint16_t part = rd16(PART), level;
    static const uint16_t levels[6] = { 6, 6, 5, 4, 4, 5 };
    int16_t l = (int16_t)(rd16(PART_LEVELS + 2 * part) + d);
    if (l < 0 || l == (int16_t)levels[part])
        return;
    level = (uint16_t)l;
    wr16(PART_LEVELS + 2 * part, level);
    if (level == rd16(PART_OWNED + 2 * part))
        part_box_selected();
    else
        part_box_normal(part);
    part_name(part, level);
    sound_command(0xA2);
}

/* Buy the level shown, if the money is enough: fitted new. */
static void part_buy(void)
{
    uint16_t part = rd16(PART), level = rd16(PART_LEVELS + 2 * part), k;
    uint32_t price = ROM_PART_PRICES + 32u * part + 4u * level;
    int x = 0;
    if (rd32(price) > rd32(MONEY)) {
        sound_command(0xA3);
        return;
    }
    for (k = 4; k-- > 0;)
        wr8(MONEY + k, bcd_subx(rd8(MONEY + k), rd8(price + k), &x));
    wr32(VDP_CTRL, 0x5CA00003);
    hex_tiles(0xFF0582, rd32(MONEY), 8, 0xE0, 0);
    for (k = 0; k < 8; k++)
        wr16(VDP_DATA, rd16(0xFF0582 + 2 * k));
    wr16(PART_WEAR + 2 * part, 0x0F);
    mechanic_comment();
    sound_command(0xA4);
    wr16(PART_OWNED + 2 * part, level);
    part_box_selected();
}

/*
 * One parts screen frame after the VBlank ($615C): objects, the blinking
 * selection; up/down pick a part, left/right its level, C/Start buys,
 * B goes back to the command screen.
 */
static void shop_frame(void)
{
    uint8_t p;
    objects_update();
    part_blink();
    p = rd8(PAD1_PRESSED);
    if (p & 1)
        part_select(-1);
    else if (p & 2)
        part_select(1);
    else if (p & 4)
        part_level(-1);
    else if (p & 8)
        part_level(1);
    else if (p & 0xA0)
        part_buy();
    else if (p & 0x10) {
        wr16(0xFFC722, 0);
        wr16(0xFFC704, 0x24);
    }
}

void shop_frame_68k(void)
{
    shop_frame();
    set_sr(get_sr() & 0xF8FF);
}
