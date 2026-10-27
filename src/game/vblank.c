/*
 * Vertical blank work: DMA transfers to video memory, controller reading
 * and the HUD.
 *
 * The VBlank interrupt calls one of two handlers, chosen by the game mode:
 * menus run vblank_menu(); races run vblank_race(), which spreads its work
 * over the three VBlanks that each 20 fps race frame lasts.
 */
#include "game.h"

/* ------------------------------------------------------------ RAM map */

#define VDP_DMA_COMMAND   0xFFC70A  /* last DMA command; the trigger word is
                                       written from RAM, as the hardware needs */
#define PAD1_HELD         0xFFC706  /* buttons held (S A C B R L D U, 1 = down) */
#define PAD2_HELD         0xFFC7DE  /* and the next byte: pressed this frame */
#define PALETTE_DIRTY     0xFFC708  /* bit 0: upload the palette */
#define PALETTE_DELAY     0xFFC892  /* nonzero: wait a little before the upload */
#define GAME_MODE         0xFFC704
#define VBLANK_PHASE      0xFFC724  /* race VBlanks since the last race frame, x4 */
#define HUD_HIDDEN        0xFFC82C
#define FRAME_COUNTER_BYTE 0xFFC726
#define HUD_TASK          0xFFC8A6  /* step of the HUD setup sequence */

#define DMA_SLOT_1        0xFFC7E0  /* queued transfers: command, source, */
#define DMA_SLOT_2        0xFFC7EC  /* length, pending flag */
#define DMA_FIXED_SLOTS   0xFFC796  /* five transfers with fixed destinations */
#define DMA_STREAM_DELAY  0xFFC7CC  /* streamed upload list (see dma_stream) */
#define DMA_STREAM_MORE   0xFFC7CE
#define DMA_STREAM_LIST   0xFFC7D0
#define DMA_STREAM_LEN    0xFFC7D4
#define DMA_STREAM_VRAM   0xFFC7D6
#define DMA_STREAM_SRC    0xFFC7D8
#define VRAM_CLEAR_STEP   0xFF057C  /* see vram_clear_steps */
#define ANIM_ROW          0xFF060A
#define ANIM_SET          0xFF060C

#define SPRITE_BUFFER     0xFFD100
#define PALETTE_BUFFER    0xFFD380
#define SCROLL_BUFFER_SEL 0xFF0580  /* 0 or $700: which half of the double buffers */
#define SCROLL_FLIP       0xFF06C4  /* set by the race frame: flip buffers */
#define VSCROLL_LINES     0xFF1500  /* per-line vertical scroll, read by HBlank */
#define HBLANK_SOURCE     0xFF05F0  /* operand of the HBlank routine in RAM */
#define TILE_UPDATES      0xFF094A  /* 12 records of $18 bytes, see tile_pair_updates */

#define ROM_ANIM_INDEX    0x00FB86
#define ROM_ANIM_FRAMES   0x00FBCE  /* 28-byte records */
#define ROM_DIGIT_TILES   0x00C438
#define ROM_HUD_TEXT_1    0x00E0B4
#define ROM_HUD_TEXT_2    0x00E142
#define ROM_HUD_TEXT_PTRS 0x00DD5E

#define PAD1_PORT         0xA10003
#define PAD2_PORT         0xA10005


/* ---------------------------------------------------- bus and DMA */

static void z80_bus_request(void) { wr16(Z80_BUSREQ, 0x100); }   /* $6EB4 */
static void z80_bus_release(void) { wr16(Z80_BUSREQ, 0); }       /* $6EBE */

static uint16_t vdp_mode2(void) { return rd16(VDP_MODE_SHADOW + 2); }

static void dma_enable(void) { wr16(VDP_CTRL, vdp_mode2() | 0x10); }
static void dma_disable(void) { wr16(VDP_CTRL, vdp_mode2() & ~0x10); }

/* The command word that starts a DMA has to come from RAM. */
static void dma_start(uint32_t command)
{
    wr32(VDP_DMA_COMMAND, command);
    wr16(VDP_CTRL, rd16(VDP_DMA_COMMAND));
    wr16(VDP_CTRL, rd16(VDP_DMA_COMMAND + 2));
}

/* DMA with prepared length/source register writes; the command must
   already be in VDP_DMA_COMMAND ($827C). */
static void dma_prepared(uint32_t length_regs, uint32_t source_regs, uint16_t source_hi)
{
    z80_bus_request();
    dma_enable();
    wr32(VDP_CTRL, length_regs);
    wr32(VDP_CTRL, source_regs);
    wr16(VDP_CTRL, source_hi);
    wr16(VDP_CTRL, rd16(VDP_DMA_COMMAND));
    wr16(VDP_CTRL, rd16(VDP_DMA_COMMAND + 2));
    dma_disable();
    z80_bus_release();
}

/* Copy `words` words from 68000 memory to VDP memory ($82AC). */
static void dma_copy(uint32_t command, uint32_t src, uint16_t words)
{
    uint32_t s = (src >> 1) & 0x7fffff;
    z80_bus_request();
    dma_enable();
    wr32(VDP_CTRL, (0x9400u | words >> 8) << 16 | (0x9300u | (words & 0xff)));
    wr32(VDP_CTRL, (0x9600u | ((s >> 8) & 0xff)) << 16 | (0x9500u | (s & 0xff)));
    wr16(VDP_CTRL, 0x9700 | ((s >> 16) & 0xff));
    dma_start(command | 0x80);
    dma_disable();
    z80_bus_release();
}

/* Fill `bytes`+1 bytes of VRAM with zero by DMA ($8380). */
static void dma_clear(uint32_t command, uint16_t bytes)
{
    z80_bus_request();
    wr16(VDP_CTRL, 0x8F01);                     /* auto-increment 1 */
    dma_enable();
    wr32(VDP_CTRL, (0x9400u | bytes >> 8) << 16 | (0x9300u | (bytes & 0xff)));
    wr16(VDP_CTRL, 0x9780);                     /* fill mode */
    wr32(VDP_CTRL, command | 0x40000080u);
    wr8(VDP_DATA, 0);
    while (rd16(VDP_CTRL) & 2)                  /* wait for the fill */
        ;
    dma_disable();
    wr16(VDP_CTRL, 0x8F02);
    z80_bus_release();
}

/* Fill the plane A name table with one entry, 28 rows of 40 ($8344,
   $833E with entry $E6FE). */
void plane_a_fill(uint16_t entry)
{
    uint32_t cmd = 0x50000003;
    int row;
    wr16(VDP_CTRL, 0x921C);
    for (row = 0; row < 28; row++, cmd += 0x800000)
        vdp_fill(cmd, entry, 0x28);
}

/*
 * Blank the screen and clear VRAM ($8210): display off, window off,
 * clear the name tables, scroll tables, sprite table (and its RAM copy)
 * and the pattern area at $8000 to zero; plane A gets entry $E000.
 * Interrupts stay masked for the mode's setup.
 */
void screen_reset(void)
{
    int k;
    set_sr(get_sr() | 0x700);
    wr8(VDP_MODE_SHADOW + 3, rd8(VDP_MODE_SHADOW + 3) & ~0x40);
    wr16(VDP_CTRL, rd16(VDP_MODE_SHADOW + 2));
    wr16(VDP_CTRL, 0x8AFF);
    dma_clear(0x4C000003, 0x37F);
    vdp_fill(0x40000010, 0, 0x29);
    for (k = 0; k < 0x140; k++)
        wr16(0xFFD100 + 2 * k, 0);
#ifdef SNES
    sprites_hide_c();
#endif
    dma_clear(0x48000003, 0x13F);
    dma_clear(0x40000003, 0x27F);
    dma_clear(0x60000003, 0x1FFF);
    plane_a_fill(0xE000);
}

void screen_reset_68k(void)
{
    screen_reset();
    set_dn(5, 2, 0xE000);
    cpu.d[4] = 0xFFFF;
    cpu.x = 0;
}

void plane_a_fill_68k(void)
{
    plane_a_fill(0xE6FE);
    set_dn(5, 2, 0xE6FE);
    cpu.d[4] = 0xFFFF;
    cpu.x = 0;
}

static uint32_t vram_write_command(uint16_t addr)
{
    return (0x4000u | (addr & 0x3fff)) << 16 | addr >> 14;
}

static void write_words(uint32_t src, int count)
{
    int i;
    for (i = 0; i < count; i++)
        wr16(VDP_DATA, rd16(src + 2 * i));
}

/* ---------------------------------------------------------- uploads */

static void upload_sprites(void)       /* $8238 */
{
    z80_bus_request();
    dma_enable();
    wr32(VDP_CTRL, 0x94019340);                 /* $140 words */
    wr32(VDP_CTRL, 0x96E89580);                 /* from SPRITE_BUFFER */
    wr16(VDP_CTRL, 0x977F);
    dma_start(0x48000083);                      /* to the sprite table, $C800 */
    dma_disable();
    z80_bus_release();
}

/* The palette buffer to CRAM by DMA ($846C; an optional delay loop
   before it only affects timing). */
static void palette_dma(void)
{
    z80_bus_request();
    dma_enable();
    wr32(VDP_CTRL, 0x94009340);                 /* $40 words */
    wr32(VDP_CTRL, 0x96E995C0);                 /* from PALETTE_BUFFER */
    wr16(VDP_CTRL, 0x977F);
    dma_start(0xC0000080);                      /* to CRAM 0 */
    dma_disable();
    z80_bus_release();
}

void palette_dma_68k(void)
{
    palette_dma();
    set_dn(4, 2, vdp_mode2() & ~0x10);
    f_logic(2, 0);
}

static void upload_palette(void)       /* $8464 */
{
    if (!(rd8(PALETTE_DIRTY) & 1))
        return;
    wr8(PALETTE_DIRTY, rd8(PALETTE_DIRTY) & ~1);
    palette_dma();
}

/* A queued transfer: command, source, length, pending flag ($F86E). */
static int dma_slot(uint32_t slot)
{
    if (!rd16(slot + 10))
        return 0;
    wr16(slot + 10, 0);
    dma_copy(rd32(slot), rd32(slot + 4), rd16(slot + 8));
    return 1;
}

/*
 * Streamed uploads ($F884): after a countdown, work through a list of
 * (length, VRAM address, source) entries, at most $5A0 words per VBlank;
 * longer entries continue on the next VBlank.
 */
static void dma_stream(void)
{
    uint16_t delay = rd16(DMA_STREAM_DELAY), len;
    if (delay == 0)
        return;
    if (delay != 1) {
        wr16(DMA_STREAM_DELAY, delay - 1);
        return;
    }
    if (!rd16(DMA_STREAM_MORE)) {
        uint32_t p = rd32(DMA_STREAM_LIST);
        uint16_t n = rd16(p);
        if (n == 0) {
            wr16(DMA_STREAM_DELAY, 0);
            return;
        }
        wr16(DMA_STREAM_LEN, n);
        wr16(DMA_STREAM_VRAM, rd16(p + 2));
        wr32(DMA_STREAM_SRC, rd32(p + 4));
        wr32(DMA_STREAM_LIST, p + 8);
    }
    len = rd16(DMA_STREAM_LEN);
    if (len >= 0x5A1) {
        uint32_t cmd = vram_write_command(rd16(DMA_STREAM_VRAM));
        uint32_t src = rd32(DMA_STREAM_SRC);
        wr16(DMA_STREAM_MORE, 0xFFFF);
        wr16(DMA_STREAM_LEN, len - 0x5A0);
        wr16(DMA_STREAM_VRAM, rd16(DMA_STREAM_VRAM) + 0xB40);
        wr32(DMA_STREAM_SRC, src + 0xB40);
        dma_copy(cmd, src, 0x5A0);
    } else {
        wr16(DMA_STREAM_MORE, 0);
        dma_copy(vram_write_command(rd16(DMA_STREAM_VRAM)), rd32(DMA_STREAM_SRC), len);
    }
}

/* Five transfers with fixed destinations, each with a pending flag ($F7C4). */
static void dma_fixed_slots(void)
{
    uint32_t a = DMA_FIXED_SLOTS;
    if (rd8(a)) {
        wr8(a, 0);
        dma_copy(0x62C00001, rd32(a), rd16(0xFFC7FA));
    }
    a += 8;
    if (rd8(a)) {
        wr8(a, 0);
        dma_copy(0x5F600001, rd32(a), rd16(0xFFC7FC));
    }
    a += 8;
    if (rd16(a + 6)) {
        wr16(a + 6, 0);
        wr32(VDP_DMA_COMMAND, 0x5F600083);
        dma_prepared(0x93209400, rd32(a), rd16(a + 4));
    }
    a += 8;
    if (rd16(a + 6)) {
        wr16(a + 6, 0);
        wr32(VDP_DMA_COMMAND, 0x5EE00083);
        dma_prepared(0x93409400, rd32(a), rd16(a + 4));
    }
    a += 8;
    if (rd16(a + 6)) {
        wr16(a + 6, 0);
        wr32(VDP_DMA_COMMAND, 0x5DE00081);
        dma_prepared(0x93C09400, rd32(a), rd16(a + 4));
    }
}

/* Animation frame record for the current set/row ($F40A). */
static uint32_t anim_frame_record(void)
{
    uint16_t i = (uint16_t)(rd16(ANIM_SET) * 18 + (rd16(ANIM_ROW) + 1) * 2);
    uint16_t frame = rd16(ROM_ANIM_INDEX + i);
    return ROM_ANIM_FRAMES + (uint16_t)(frame * 28);
}

/*
 * One step per VBlank of a VRAM clear-and-reload sequence ($F754): steps
 * 1-2 clear areas, 3-6 upload the current animation frame's pieces. Setting
 * the step to $FF80 clears one area once and parks the sequence.
 */
static void vram_clear_steps(void)
{
    int16_t step = (int16_t)rd16(VRAM_CLEAR_STEP);
    uint32_t rec;
    if (step >= 0 && step + 1 < 7) {
        wr16(VRAM_CLEAR_STEP, (uint16_t)++step);
        switch (step) {
        case 1: dma_clear(0x62800003, 0x2FF); break;
        case 2: dma_clear(0x40800003, 0xFF); break;
        case 3: dma_copy(0x69800002, rd32(anim_frame_record()), 0x200); break;
        case 4: dma_copy(0x6D800002, rd32(anim_frame_record() + 4), 0x200); break;
        case 5:
            rec = anim_frame_record() + 8;
            dma_copy(rd32(rec), rd32(rec + 6), rd16(rec + 4));
            break;
        case 6:
            rec = anim_frame_record() + 0x12;
            dma_copy(rd32(rec), rd32(rec + 6), rd16(rec + 4));
            break;
        }
        return;
    }
    if ((uint16_t)step == 0xFF80) {
        wr16(VRAM_CLEAR_STEP, 0xFFFF);
        dma_clear(0x40800003, 0x1FF);
    }
}

/*
 * Load a stage's graphics ($F330): the previous animation frame's four
 * pieces by DMA, the road side tiles, and the background map, unpacked
 * and written twice (palette lines 0 and 1).
 */
static void dma_copy_regs(uint32_t src);
void nemesis_to_vdp_68k(void);

void stage_graphics_load(void)
{
    uint16_t stage = rd16(ANIM_ROW);
    uint32_t rec;
    int k;
    wr16(ANIM_ROW, stage - 1);
    dma_copy(0x69800002, rd32(anim_frame_record()), 0x200);
    rec = anim_frame_record() + 8;
    dma_copy(rd32(rec), rd32(rec + 6), rd16(rec + 4));
    dma_copy(0x6D800002, rd32(anim_frame_record() + 4), 0x200);
    rec = anim_frame_record() + 0x12;
    dma_copy(rd32(rec), rd32(rec + 6), rd16(rec + 4));
#ifndef SNES
    dma_copy_regs(rd32(rec + 6));
#endif
    wr16(VRAM_CLEAR_STEP, 0xFFFF);
    wr16(ANIM_ROW, stage);
    wr32(VDP_CTRL, 0x71800002);
#ifdef SNES
    nemesis_to_vdp(0x018000);
#else
    cpu.a[0] = 0x018000;
    nemesis_to_vdp_68k();
#endif
    map_unpack(0x018372);
    for (k = 0; k < 0x3FC; k++)
        wr16(0xFFE100 + 2 * k, rd16(0xFFF2F8 + 2 * k));
    wr32(VDP_CTRL, 0x68000003);
    vdp_map_rows(0);
    vdp_map_rows(0x2000);
}

void stage_graphics_load_68k(void)
{
    stage_graphics_load();
    set_dn(0, 2, rd16(0xFFEB14 + 24 * 0xAA - 0x2A - 2) + 0x2000);
    set_dn(1, 2, 2);
    set_dn(5, 2, 0xFFFF);
}

/*
 * Pending tile writes ($EB16): 12 records of $18 bytes. Each has a count,
 * a VRAM address and up to 10 name table entries. Every entry is written
 * twice: at the address, and $C00 bytes further with the next palette line
 * (+$2000). The address steps along a 64-cell row, wrapping within it.
 */
static void tile_pair_updates(void)
{
    uint32_t rec = TILE_UPDATES;
    int r, k;
    for (r = 0; r < 12; r++, rec += 0x18) {
        uint16_t count = rd16(rec), addr, row_base;
        uint32_t cmd;
        if (!count)
            continue;
        if ((int16_t)count > 9)
            count = 9;
        wr16(rec, 0);
        addr = rd16(rec + 2);
        cmd = vram_write_command(addr);
        row_base = (uint16_t)((cmd >> 16) & 0xFF80);
        for (k = 0; k <= count; k++) {
            uint16_t tile = rd16(rec + 4 + 2 * k);
            wr32(VDP_CTRL, cmd);
            wr16(VDP_DATA, tile);
            wr32(VDP_CTRL, cmd + 0x0C000000);
            wr16(VDP_DATA, tile + 0x2000);
            cmd = ((uint32_t)((((cmd >> 16) + 2) & 0x7E) | row_base) << 16) | (cmd & 0xffff);
        }
    }
}

/* ------------------------------------------------------------ HUD */

/* the byte variant tests only the low byte of HUD_HIDDEN (or.b) */
static int hud_blocked_b(void) { return ((rd16(HUD_HIDDEN) & 0xff) | rd8(SOUND_LOCKED + 1)) != 0; }
static int hud_blocked_w(void) { return (rd16(HUD_HIDDEN) | rd16(SOUND_LOCKED)) != 0; }

static void hud_row(uint32_t command, uint32_t src, int words)
{
    wr32(VDP_CTRL, command);
    write_words(src, words);
}

static void hud_part_3(void)           /* $C078 */
{
    if (hud_blocked_w())
        return;
    hud_row(0x52140003, 0xFFC778, 8);
}

static void hud_part_12(void)          /* $C04C (continues into $C078) */
{
    if (hud_blocked_b())
        return;
    hud_row(0x510E0003, 0xFFC758, 8);
    hud_row(0x513C0003, 0xFFC768, 8);
    hud_part_3();
}

static void hud_part_4(void)           /* $C096 */
{
    if (hud_blocked_w())
        return;
    hud_row(0x52920003, 0xFFC788, 2);
}

static int hud_part_5(void)            /* $C0B4; returns 1 if it cleared */
{
    if ((rd16(GAME_MODE) & ~1) == 0x1C) {
        dma_clear(rd16(HUD_HIDDEN) ? 0x51320003 : 0x52320003, 0x15);
        return 1;
    }
    if (rd16(SOUND_LOCKED))
        return 0;
    hud_row(rd16(HUD_HIDDEN) ? 0x513C0003 : 0x523C0003, 0xFFC78C, 4);
    return 0;
}

/* Two BCD digits as name table entries ($C16C): leading zero blank, each
   digit tile = base + digit (+ $614 when shown). */
static void bcd_digits(uint32_t dst, uint8_t bcd, uint16_t base)
{
    int shown = 0, k;
    base = (uint16_t)(base << 8);
    for (k = 0; k < 2; k++) {
        uint16_t digit = k == 0 ? bcd >> 4 : bcd & 0x0f;
        if (digit || k == 1)
            shown = 1;
        wr16(dst + 2 * k, (uint16_t)(digit + base + (shown ? 0x614 : 0)));
    }
}

/* Draw two tall digits (2x2 entries each) for a BCD byte ($C1B0). */
static uint16_t big_digit_index;   /* for the adapter */
static void hud_big_digits_base(uint32_t command, uint32_t bcd_addr, uint16_t base)
{
    const uint32_t buf = 0xFF0582;
    int k;
    bcd_digits(buf, rd8(bcd_addr), base);
    for (k = 0; k < 2; k++) {
        uint16_t d = rd16(buf + 2 * k), i = 0;
        if (d)
            i = (uint16_t)((d - 0x613) << 2);
        big_digit_index = i;
        wr16(buf + 2 * k, rd16(ROM_DIGIT_TILES + i));
        wr16(buf + 4 + 2 * k, rd16(ROM_DIGIT_TILES + i + 2));
    }
    vdp_copy_rect(command, buf, 2, 2);
}

void hud_big_digits(uint32_t command, uint32_t bcd_addr)
{
    hud_big_digits_base(command, bcd_addr, 0);
}

void hud_big_digits_68k(void)          /* $C1B0 */
{
    hud_big_digits_base(cpu.d[7], cpu.a[1], (uint16_t)cpu.d[0]);
    set_dn(0, 2, big_digit_index);
    cpu.d[5] = 0xFFFF;
}

/* Eight digits, all shown ($C184). */
/* Eight digits, leading zeros blank ($C180), and four from a word
   ($C176). */
void hex_tiles_8_68k(void)
{
    uint32_t v = rd32(cpu.a[1]);
    hex_tiles(cpu.a[0], v, 8, (uint16_t)cpu.d[0], 0);
    cpu.a[0] += 16;
    cpu.d[1] = 0xFFFFFFFF;
    cpu.d[4] = v;
    cpu.x = 0;
}

void hex_tiles_4_68k(void)
{
    uint16_t v = rd16(cpu.a[1]);
    hex_tiles(cpu.a[0], v, 4, (uint16_t)cpu.d[0], 0);
    cpu.a[0] += 8;
    set_dn(0, 2, (uint16_t)(cpu.d[0] << 8));
    cpu.d[1] = 0xFFFFFFFF;
    cpu.d[4] = (cpu.d[4] & 0xFFFF0000u) | v;
    cpu.x = 0;
}

/* Display on ($8430) and off ($8438). */
void display_on(void)
{
    wr8(VDP_MODE_SHADOW + 3, rd8(VDP_MODE_SHADOW + 3) | 0x40);
    wr16(VDP_CTRL, rd16(VDP_MODE_SHADOW + 2));
}

void display_off(void)
{
    wr8(VDP_MODE_SHADOW + 3, rd8(VDP_MODE_SHADOW + 3) & ~0x40);
    wr16(VDP_CTRL, rd16(VDP_MODE_SHADOW + 2));
}

void display_off_68k(void) { display_off(); }

void hex_tiles_shown_68k(void)
{
    hex_tiles(cpu.a[0], rd32(cpu.a[1]), 8, (uint16_t)cpu.d[0], 1);
    cpu.a[0] += 16;
    cpu.d[1] = 0xFFFFFFFF;
    cpu.d[4] = rd32(cpu.a[1]);
}

/*
 * A menu countdown ($7ADC): every 60 frames the BCD count at $FFC71A
 * goes down and is shown in big digits; at zero $FFC718 is set.
 */
void menu_countdown(void)
{
    wr8(0xFFC71B, rd8(0xFFC71B) - 1);
    if (rd8(0xFFC71B))
        return;
    wr8(0xFFC718, rd16(0xFFC71A) ? 0 : 0xFF);
    if (!rd16(0xFFC71A))
        return;
    wr8(0xFFC71B, 0x3C);
    wr8(0xFFC71A, bcd_sub(rd8(0xFFC71A), 1));
    hud_big_digits(0x5A260003, 0xFFC71A);
}

void menu_countdown_68k(void) { menu_countdown(); }

static void hud_time(void);

/*
 * Draw the whole race HUD ($BA9A): its frame pieces, then (unless in the
 * demo) every number, and the course progress bar, as long as the
 * course (or original mode race) has segments.
 */
void hud_draw_all(void)
{
    uint32_t rec;
    int n, k;
    uint16_t len;
    if (rd16(HUD_HIDDEN)) {
        rec = 0x00C224;
        n = 3;
    } else {
        rec = 0x00C248;
        n = rd8(SOUND_LOCKED) ? 3 : 6;
    }
    for (k = 0; k < n; k++, rec += 12)
        vdp_copy_rect(rd32(rec), rd32(rec + 8), rd16(rec + 4) + 1, rd16(rec + 6) + 1);
    if (rd8(SOUND_LOCKED))
        return;
    hud_numbers();
    hud_part_12();
    hud_part_3();
    hud_part_4();
    hud_part_5();
    hud_time();
    if (rd16(HUD_HIDDEN)) {
        wr32(VDP_CTRL, 0x52040003);
        wr16(VDP_DATA, 0xA630);
        len = rd16(0x00C388 + 2 * rd16(0xFFC85A));
        for (k = 0; k <= len; k++) {
            wr32(VDP_DATA, 0xC6BFC6C3);
            wr32(VDP_DATA, 0xC6C3C6C3);
        }
    } else {
        wr32(VDP_CTRL, 0x53020003);
        wr16(VDP_DATA, 0xA630);
        len = rd16(0x00C378 + 4 * rd16(0xFF060C) + 2);
        for (k = 0; k <= len; k++)
            wr32(VDP_DATA, 0xC6BFC6C3);
    }
    wr16(VDP_DATA, 0xA624);
    hud_map_marker();
}

/*
 * Open the HUD like a curtain ($88F4): every 4 frames one more line of a
 * striped pattern; when done, the window plane is reset and the HUD drawn.
 */
static int curtain_done;
void hud_curtain(void)
{
    uint32_t pattern = 0x008958;
    int16_t step;
    int n, k;
    curtain_done = 0;
    if (!rd8(SOUND_LOCKED + 1))
        return;
    wr16(0xFFC71A, rd16(0xFFC71A) - 1);
    if ((int16_t)rd16(0xFFC71A) >= 0)
        return;
    wr16(0xFFC71A, 3);
    step = (int16_t)(rd16(0xFFC71C) - 1);
    wr16(0xFFC71C, (uint16_t)step);
    if (step < 0) {
        wr16(VDP_CTRL, 0x9207);
        wr8(SOUND_LOCKED + 1, 0);
        hud_draw_all();
        curtain_done = 1;
        return;
    }
    n = 15 - step;
    if (n >= 8) {
        n -= 8;
        pattern += 8;
    }
    wr32(VDP_CTRL, 0x5FC00003);
    for (k = 0; k <= n; k++)
        wr32(VDP_DATA, rd32(pattern + 4 * (k & 1)));
}

void hud_curtain_68k(void)
{
    hud_curtain();
    if (curtain_done)
        set_dn(5, 2, 0xFFFF);
}

void hud_draw_all_68k(void)
{
    hud_draw_all();
    set_dn(5, 2, 0xFFFF);
}

/*
 * Graphics for the current course and stage ($8BFA): a list of DMA
 * transfers (length in words, VRAM address, source; ended by 0), then
 * the bike's tiles and the rider frames.
 */
static void dma_copy_regs(uint32_t src);

/* The bike's tiles and the rider frames ($8C36). */
void bike_graphics(void)
{
    wr32(VDP_CTRL, 0x40000000);
    nemesis_to_vdp(0x03DF62);
    nemesis_to_ram(0x040D3E, 0xFFB900);
}

static uint32_t course_dma_src;   /* last source, for the adapter */
void course_graphics(void)
{
    uint32_t list = rd32(0x008ED4 + (uint16_t)(rd16(0xFF060C) * 72 + rd16(0xFF060A) * 8));
    uint16_t words;
    course_dma_src = 0;
    while ((words = rd16(list)) != 0) {
        course_dma_src = rd32(list + 4);
        dma_copy(vram_write_command(rd16(list + 2)), course_dma_src, words);
        list += 8;
    }
    bike_graphics();
}

void course_graphics_68k(void)
{
    course_graphics();
    if (course_dma_src)
        dma_copy_regs(course_dma_src);
}

static void hud_time(void)             /* $C142 */
{
    if (hud_blocked_b())
        return;
    hud_big_digits(0x52240003, rd8(SOUND_LOCKED) ? 0xFF054C : 0xFF0554);
}

/* Every fourth VBlank (every one while the sound is locked). */
static int hud_type_turn(void)
{
    if (rd8(SOUND_LOCKED))
        return 1;
    wr16(0xFFC71E, (rd16(0xFFC71E) + 1) & 3);
    return rd16(0xFFC71E) == 0;
}

/* Type a line of text one tile per turn from ROM ($DDD0). */
static void hud_type_line(void)
{
    uint16_t i;
    if (!hud_type_turn())
        return;
    i = rd16(0xFFC710);
    wr32(VDP_CTRL, vram_write_command((uint16_t)(0xD386 + i)));
    wr16(VDP_DATA, rd16(0x00DE28 + i));
    wr16(0xFFC710, i + 2);
    if (rd16(0xFFC710) == 0x42) {
        wr16(HUD_TASK, rd16(HUD_TASK) + 1);
        wr16(0xFFC710, 0);
        wr16(0xFFC712, 0);
        wr16(0xFFC714, 0);
        wr16(0xFFC71E, 0);
    }
}

/*
 * Type the ranking table one tile per turn ($DE74): seven rows of fields
 * (widths 4, 8, 4, 4 tiles from ROM columns, then an 8-tile column block
 * written down when present), from the entries at the pointer $FFC716.
 * Blank tiles (no pattern) are skipped.
 */
static void hud_type_table(void)
{
    uint32_t src = rd32(0xFFC716), cmd;
    uint16_t field;
    if (!hud_type_turn())
        goto done;
    field = rd16(0xFFC714);
    cmd = vram_write_command((uint16_t)(0xD486 + (rd16(0xFFC712) << 8) +
                                        rd16(0x00DE6A + 2 * field) + 2 * rd16(0xFFC710)));
    if (field == 4) {
        if (rd32(src + 4)) {
            int k;
            for (k = 0; k < 8; k++, cmd += 0x20000, src += 2) {
                uint16_t t = rd16(src);
                wr32(VDP_CTRL, cmd);
                if (t & 0x7FF)
                    wr16(VDP_DATA, t);
            }
        } else {
            src += 0x10;
        }
        wr16(0xFFC714, 0);
        wr16(0xFFC712, rd16(0xFFC712) + 1);
        if (rd16(0xFFC712) >= 7 && rd16(0xFFC8AA) != 1)
            wr16(HUD_TASK, rd16(HUD_TASK) + 1);
    } else {
        uint16_t t = rd16(src);
        src += 2;
        wr32(VDP_CTRL, cmd);
        if (t & 0x7FF)
            wr16(VDP_DATA, t);
        wr16(0xFFC710, rd16(0xFFC710) + 1);
        if (rd16(0x00DF56 + 2 * field) == rd16(0xFFC710)) {
            wr16(0xFFC710, 0);
            wr16(0xFFC714, field + 1);
            if (rd16(0xFFC712) == 7 && field + 1 == 2)
                wr16(HUD_TASK, rd16(HUD_TASK) + 1);
        }
    }
done:
    wr32(0xFFC716, src);
}

/* Give `count` name table entries, 2 bytes apart from `cmd`, priority
   (read back each one, keep it if it has a pattern) ($E016). */
static void hud_highlight(uint32_t cmd, int count)
{
    int k;
    for (k = 0; k < count; k++, cmd += 0x20000) {
        uint16_t t;
        cmd &= ~0x40000000u;
        wr32(VDP_CTRL, cmd);
        t = rd16(VDP_DATA) & 0x7FF;
        if (t) {
            cmd |= 0x40000000u;
            wr32(VDP_CTRL, cmd);
            wr16(VDP_DATA, t | 0x8000);
        }
    }
}

/*
 * Ranking: highlight the new entry's row, or keep drawing the name entry
 * line: the letter row with the cursor letter highlighted, the score and
 * the four letters entered so far ($DF5E).
 */
static void hud_name_entry(void)
{
    uint16_t mode = rd16(0xFFC8AA), cursor;
    uint32_t name;
    int k;
    if (mode < 2) {
        if (mode == 1)
            hud_highlight(0x5B860003, 0x0E);
        wr16(HUD_TASK, rd16(HUD_TASK) + 1);
        return;
    }
    vdp_copy(0x518C0003, 0x00E086, 0x17);
    vdp_copy(0x5B880003, 0x00E04C, 0x1D);
    wr32(VDP_CTRL, 0x51BE0003);
    bcd_digits(0xFF0582, rd8(0xFF03CC), 0xC0);
    write_words(0xFF0582, 2);
    name = rd32(0xFFC8B0);
    wr32(VDP_CTRL, rd32(0xFFC8AC));
    for (k = 0; k < 4; k++)
        wr16(VDP_DATA, char_tile(rd8(name + k)));
    cursor = (uint16_t)(rd16(0xFF03C2) * 2);
    wr32(VDP_CTRL, 0x5B880003 + ((uint32_t)cursor << 16));
    wr16(VDP_DATA, (rd16(0x00E04C + cursor) & 0x7FF) | 0xC000);
    hud_highlight(rd32(0xFFC8AC) - 0x2A0000, 0x23);
}

/*
 * Original mode's HUD extras ($BD0C): the elapsed time, a window over the
 * screen top that grows with the number of broken parts (a warning blinks
 * while it is negative), and when the count changes, a line per broken
 * part.
 */
static void hud_parts(void)
{
    static const struct { uint32_t wear, text; } parts[6] = {
        { 0xFFB650, 0x00BE8A }, { 0xFFB648, 0x00BEC0 }, { 0xFFB652, 0x00BEF6 },
        { 0xFFB64E, 0x00BF2C }, { 0xFFB64C, 0x00BF62 }, { 0, 0 },
    };
    int16_t broken;
    uint32_t cmd;
    int k;
    if (!rd16(HUD_HIDDEN) || rd16(SOUND_LOCKED))
        return;
    vdp_copy(0x511A0003, 0xFFC83E, 8);
    broken = (int16_t)rd16(0xFFC896);
    if (broken >= 0) {
        wr16(VDP_CTRL, 0x9200 | rd8(0x00BD04 + broken));
        if (!broken)
            return;
        vdp_copy(0x52A00003, rd8(FRAME_COUNTER_BYTE) & 2 ? 0x00BE30 : 0x00BE42, 9);
    } else {
        vdp_copy(0x52A00003, rd8(FRAME_COUNTER_BYTE) & 8 ? 0x00BE1E : 0x00BE42, 9);
    }
    if (!rd16(0xFFC89A))
        return;
    wr16(0xFFC89A, 0);
    cmd = 0x530E0003;
    if ((int16_t)rd16(0xFFB64A) < 0 && (int16_t)rd16(0xFFB650) >= 0) {
        vdp_copy(cmd, 0x00BE54, 0x1B);
        cmd += 0x800000;
    }
    for (k = 0; parts[k].wear; k++)
        if ((int16_t)rd16(parts[k].wear) < 0) {
            vdp_copy(cmd, parts[k].text, 0x1B);
            cmd += 0x800000;
        }
}

/* HUD setup sequence, one step per VBlank ($DD38). */
static void hud_tasks(void)
{
    switch (rd16(HUD_TASK)) {
    case 0:
        break;
    case 1:
        wr16(HUD_TASK, 2);
        dma_clear(0x50000003, 0xC7F);
        break;
    case 2:
        wr16(HUD_TASK, 3);
        wr16(0xFFC710, 0);
        wr16(0xFFC71E, 0);
        vdp_copy(0x5CB60003, ROM_HUD_TEXT_1, 11);
        vdp_copy(0x52960003, rd32(ROM_HUD_TEXT_PTRS + 4 * rd16(0xFFC70E)), 15);
        if (rd8(SOUND_LOCKED))
            vdp_copy(0x519A0003, ROM_HUD_TEXT_2, 13);
        break;
    case 3: hud_type_line(); break;
    case 4: hud_type_table(); break;
    case 5: hud_name_entry(); break;
    default: break;
    }
}

/* --------------------------------------------------------- controllers */

/* 3-button pad: TH low gives Start/A, TH high gives C/B/directions. */
static void read_pad(uint32_t state, uint32_t port)      /* $81BA */
{
    uint8_t lo, hi, now, changed;
    wr8(port, 0);
    lo = rd8(port);
    wr8(port, 0x40);
    hi = rd8(port);
    now = (uint8_t)~((hi & 0x3f) | ((lo << 2) & 0xc0));
    changed = rd8(state) ^ now;
    wr8(state, now);
    wr8(state + 1, now & changed);   /* newly pressed */
}

/* Pad ports as inputs with TH as output ($817A). */
void pads_init(void)
{
    z80_bus_request();
    wr8(0xA10009, 0x40);
    wr8(0xA1000B, 0x40);
    wr8(0xA1000D, 0x40);
    z80_bus_release();
}

void pads_init_68k(void) { pads_init(); f_logic(2, 0); }

static void read_pads(void)            /* $8196 */
{
    z80_bus_request();
    read_pad(PAD1_HELD, PAD1_PORT);
    read_pad(PAD2_HELD, PAD2_PORT);
    z80_bus_release();
}

/* ----------------------------------------------------------- handlers */

void vblank_menu(void)                 /* $7486 */
{
    upload_sprites();
    upload_palette();
    read_pads();
}

static void vblank_race_end(void)      /* $E9CC */
{
    wr32(HBLANK_SOURCE, VSCROLL_LINES + (int16_t)rd16(SCROLL_BUFFER_SEL));
    wr16(VDP_CTRL, 0x8014);            /* line interrupts on */
}

void vblank_race(void)                 /* $E906 */
{
    uint16_t phase;
    wr32(VDP_CTRL, 0x40000010);        /* vertical scroll 0 */
    wr32(VDP_DATA, 0);
    phase = rd16(VBLANK_PHASE) + 4;
    if (phase >= 0x10)
        phase = 0x0C;
    wr16(VBLANK_PHASE, phase);

    switch (phase) {
    case 4:   /* first VBlank of the race frame */
        if (rd8(SCROLL_FLIP)) {
            wr16(SCROLL_BUFFER_SEL, rd16(SCROLL_BUFFER_SEL) ^ 0x700);
            wr8(SCROLL_FLIP, 0);
        }
        wr32(VDP_DMA_COMMAND, 0x4C000083);   /* per-line horizontal scroll */
        dma_prepared(0x93C09401, rd16(SCROLL_BUFFER_SEL) ? 0x95C0968F : 0x9540968C, 0x977F);
        upload_sprites();
        upload_palette();
        dma_slot(DMA_SLOT_2);
        dma_fixed_slots();
        read_pads();
        hud_tasks();
        break;
    case 8:
        dma_slot(DMA_SLOT_1);
        dma_stream();
        vram_clear_steps();
        tile_pair_updates();
        hud_part_12();
        hud_part_3();      /* again: $C04C already ran it */
        hud_part_4();
        hud_part_5();
        hud_time();
        hud_map_marker();
        hud_parts();
        hud_tasks();
        break;
    default:
        dma_slot(DMA_SLOT_1);
        break;
    }
    vblank_race_end();
}

/* ---------------------------------------- 68000 interfaces
   Only for functions that recompiled code outside this file still calls.
   Results follow tools/flow.py's view of what those callers read. */

/*
 * The VBlank interrupt ($6D2A): the handler for the current mode
 * ($FFC722: 0 none, 4 menus, 8 race), then the VBlank count for waits.
 * The registers are saved on the 68000 stack like the original does, so
 * RAM matches; the adapter returns from the exception.
 */
static void vblank_entry(void)
{
    uint32_t sp = cpu.a[7];
    int n;
    for (n = 14; n >= 0; n--) {            /* movem.l d0-d7/a0-a6,-(sp) */
        sp -= 4;
        wr32(sp, n < 8 ? cpu.d[n] : cpu.a[n - 8]);
    }
    cpu.a[7] = sp;
    switch (rd16(0xFFC722)) {
    case 0: break;
    case 4: vblank_menu(); break;
    case 8: vblank_race(); break;
    default: m68k_fault("unknown VBlank handler", rd16(0xFFC722));
    }
    wr16(0xFF0500, rd16(0xFF0500) + 1);
    for (n = 0; n < 15; n++, sp += 4) {    /* movem.l (sp)+,d0-d7/a0-a6 */
        if (n < 8) cpu.d[n] = rd32(sp);
        else cpu.a[n - 8] = rd32(sp);
    }
    cpu.a[7] = sp;
}

void vblank_entry_68k(void)
{
    vblank_entry();
    set_sr(pop16());                        /* rte; the wrapper pops the pc */
}

void vblank_menu_68k(void) { vblank_menu(); }
void vblank_race_68k(void) { vblank_race(); }

void z80_bus_request_68k(void) { z80_bus_request(); }

void z80_bus_release_68k(void)
{
    z80_bus_release();
    f_logic(2, 0);
}

/* d4/d5 as the original leaves them after a transfer */
static void dma_copy_regs(uint32_t src)
{
    uint32_t s = (src >> 1) & 0x7fffff;
    cpu.d[5] = s << 16 | s >> 16;
    cpu.d[4] = (0x9600u | ((s >> 8) & 0xff)) << 16 | (vdp_mode2() & ~0x10);
    cpu.x = 0;
}

void dma_copy_68k(void)
{
    uint32_t src = cpu.a[6];
    dma_copy(cpu.d[7], src, (uint16_t)cpu.d[6]);
    dma_copy_regs(src);
}

static void dma_slot_68k(uint32_t slot)
{
    uint32_t src = rd32(slot + 4);
    if (dma_slot(slot))
        dma_copy_regs(src);
}

/* Both queued transfers, if pending ($F864, $F86A). */
void dma_slots_run(void)
{
    dma_slot(DMA_SLOT_1);
    dma_slot(DMA_SLOT_2);
}

void dma_slot_1_68k(void) { dma_slot_68k(DMA_SLOT_1); }
void dma_slot_2_68k(void) { dma_slot_68k(DMA_SLOT_2); }

void hud_part_12_68k(void) { hud_part_12(); }
void hud_part_3_68k(void) { hud_part_3(); }
void hud_part_4_68k(void) { hud_part_4(); }

void hud_part_5_68k(void)
{
    if (hud_part_5()) {
        cpu.d[5] = 0;
        cpu.d[4] = 0x94000000u | (vdp_mode2() & ~0x10);
    }
}

void hud_time_68k(void)
{
    if (hud_blocked_b())
        return;
    hud_time();
    cpu.d[5] = 0x0000ffff;   /* moveq #1,d5, then counted down by the copy */
}

void bcd_digits_68k(void)
{
    uint8_t b = rd8(cpu.a[1]);
    bcd_digits(cpu.a[0], b, (uint16_t)cpu.d[0]);
    cpu.a[0] += 4;
    cpu.d[1] = 0xffffffff;
    cpu.d[2] = 0x0000ffff;
    cpu.d[4] = (cpu.d[4] & ~0xffu) | b;
}

/* Eight tile words from $FF0582 to the VDP ($C1F6). */
void hud_eight_tiles(void)
{
    write_words(0xFF0582, 8);
}

void hud_eight_tiles_68k(void)
{
    hud_eight_tiles();
    cpu.d[0] = 0xFFFF;
    f_logic(2, rd16(0xFF0582 + 14));
}

void hud_two_tiles_68k(void)           /* $C1EE */
{
    write_words(0xFF0582, 2);
}

/* ASCII to font tile ($DBE6): space blank, '.' its own tile, others from $61E. */
uint16_t char_tile(uint8_t c)
{
    uint16_t t = (uint8_t)(c - 0x20);
    if (t == 0x0E)
        t = 0x638;
    else if (t)
        t = (uint16_t)(t - 0x21 + 0x61E);
    return t | 0xE000;
}

void char_tile_68k(void)
{
    set_dn(1, 2, char_tile(rd8(cpu.a[1])));
    cpu.a[1]++;
}

void anim_frame_record_68k(void)
{
    uint16_t i = (uint16_t)(rd16(ANIM_SET) * 18 + (rd16(ANIM_ROW) + 1) * 2);
    uint16_t frame = rd16(ROM_ANIM_INDEX + i);
    cpu.a[0] = anim_frame_record();
    set_dn(1, 2, (uint16_t)(frame * 12));
}
