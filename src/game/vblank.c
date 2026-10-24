/*
 * Vertical blank work: DMA transfers to video memory, controller reading
 * and the HUD.
 *
 * The VBlank interrupt calls one of two handlers, chosen by the game mode:
 * menus run vblank_menu(); races run vblank_race(), which spreads its work
 * over the three VBlanks that each 20 fps race frame lasts.
 */
#include "game.h"
#include "sho_funcs.h"

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

/* Call a recompiled function (it returns with rts). */
static void call68k(void (*fn)(void))
{
    push32(0);
    fn();
}

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

static void upload_palette(void)       /* $8464 */
{
    if (!(rd8(PALETTE_DIRTY) & 1))
        return;
    wr8(PALETTE_DIRTY, rd8(PALETTE_DIRTY) & ~1);
    /* $846C: an optional delay loop (timing only) precedes the transfer */
    z80_bus_request();
    dma_enable();
    wr32(VDP_CTRL, 0x94009340);                 /* $40 words */
    wr32(VDP_CTRL, 0x96E995C0);                 /* from PALETTE_BUFFER */
    wr16(VDP_CTRL, 0x977F);
    dma_start(0xC0000080);                      /* to CRAM 0 */
    dma_disable();
    z80_bus_release();
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
static void hud_big_digits(uint32_t command, uint32_t bcd_addr)
{
    const uint32_t buf = 0xFF0582;
    int k;
    bcd_digits(buf, rd8(bcd_addr), 0);
    for (k = 0; k < 2; k++) {
        uint16_t d = rd16(buf + 2 * k), i = 0;
        if (d)
            i = (uint16_t)((d - 0x613) << 2);
        wr16(buf + 2 * k, rd16(ROM_DIGIT_TILES + i));
        wr16(buf + 4 + 2 * k, rd16(ROM_DIGIT_TILES + i + 2));
    }
    vdp_copy_rect(command, buf, 2, 2);
}

static void hud_time(void)             /* $C142 */
{
    if (hud_blocked_b())
        return;
    hud_big_digits(0x52240003, rd8(SOUND_LOCKED) ? 0xFF054C : 0xFF0554);
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
    case 3: call68k(f_00DD50); break;
    case 4: call68k(f_00DD54); break;
    case 5: call68k(f_00DD58); break;
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
        call68k(f_00BB76);
        call68k(f_00BD0C);
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
