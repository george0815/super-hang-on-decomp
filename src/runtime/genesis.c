#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k_rt.h"
#include "genesis.h"
#include "vdp.h"
#include "sound.h"
#include "verify.h"

M68kState cpu;
uint8_t m68k_ram[0x10000];
const uint8_t *m68k_rom;
uint32_t m68k_rom_size;
uint64_t m68k_icount, m68k_icount_event = UINT64_MAX;
int64_t m68k_cycles, m68k_cycles_event;

const GenReplay *gen_replay;
uint8_t gen_frame[320 * 224];
uint8_t z80_ram[0x2000];

/* NTSC: 3420 master clocks per line, 68000 runs at master / 7 */
#define LINE_CYCLES   489
#define TOTAL_LINES   262

static uint8_t z80_busreq, z80_reset;
static uint8_t pad_th[2] = { 0x40, 0x40 }, pad_ctrl[2];
static uint8_t hint_pending;

void m68k_fault(const char *what, uint32_t addr)
{
    fprintf(stderr, "fatal: %s ($%06X) at icount %llu\n", what, addr,
            (unsigned long long)m68k_icount);
    exit(2);
}

void m68k_stop(void) { m68k_fault("stop instruction", 0); }

/* ------------------------------------------------------------------ I/O */

static uint8_t pad_read(int port)
{
    uint16_t p = host_pad(port);
    uint8_t v;
    if (pad_th[port] & 0x40)  /* TH high: C B Right Left Down Up */
        v = 0x40 | (~((p & 0x0f) | (p & PAD_B ? 0x10 : 0) | (p & PAD_C ? 0x20 : 0)) & 0x3f);
    else                      /* TH low: Start A 0 0 Down Up */
        v = (~((p & 0x03) | (p & PAD_A ? 0x10 : 0) | (p & PAD_START ? 0x20 : 0)) & 0x33);
    /* output pins read back what was written */
    return (uint8_t)((v & ~pad_ctrl[port]) | (pad_th[port] & pad_ctrl[port] & 0x7f));
}

static uint32_t io_read(int width, uint32_t a)
{
    uint32_t v = 0;
    if (verify_mode && verify_hw_read(width, a, &v))
        return v;
    if (a >= 0xc00000 && a < 0xc00020) {
        switch (a & 0x1e) {
        case 0x00: case 0x02: v = vdp_read_data(); break;
        case 0x04: case 0x06: v = vdp_read_ctrl(); break;
        default:   v = vdp_read_hv(); break;
        }
        if (width == 1) v = (a & 1) ? (v & 0xff) : (v >> 8);
    } else if (a >= 0xa10000 && a < 0xa10020) {
        switch (a & 0x1e) {
        case 0x00: v = 0xa0; break;            /* overseas, NTSC, no expansion */
        case 0x02: v = pad_read(0); break;
        case 0x04: v = pad_read(1); break;
        case 0x08: v = pad_ctrl[0]; break;
        case 0x0a: v = pad_ctrl[1]; break;
        default:   v = 0; break;
        }
        if (width == 2) v |= v << 8;
    } else if (a >= 0xa11100 && a < 0xa11102) {
        /* bit 0 = 0 once the bus is granted; the stub grants instantly */
        v = z80_busreq ? 0 : 1;
        if (width == 2) v <<= 8;
    } else if (a >= 0xa00000 && a < 0xa10000) {
        v = (a & 0x7fff) < 0x4000 ? z80_ram[a & 0x1fff] : 0;
        if (width == 2) v |= v << 8;
    }
    if (gen_replay)
        v = gen_replay->hw_read(width, a, v);
    if (verify_mode)
        verify_hw_log_read(width, a, v);
    return v;
}

static void io_write(int width, uint32_t a, uint32_t v)
{
    if (verify_mode && verify_hw_write(width, a, v))
        return;
    if (a >= 0xc00000 && a < 0xc00020) {
        if (width == 1) {
            if ((a & 0x1f) == 0x11) return;    /* PSG: sound not modelled yet */
            v |= v << 8;                       /* byte writes repeat on the bus */
        }
        switch (a & 0x1e) {
        case 0x00: case 0x02: vdp_write_data((uint16_t)v); break;
        case 0x04: case 0x06: vdp_write_ctrl((uint16_t)v); break;
        }
    } else if (a >= 0xa10000 && a < 0xa10020) {
        int port = ((a & 0x1e) - 2) >> 1;
        switch (a & 0x1e) {
        case 0x02: case 0x04: pad_th[port] = (uint8_t)v; break;
        case 0x08: case 0x0a: pad_ctrl[(a & 0x1e) == 0x0a] = (uint8_t)v; break;
        }
    } else if (a >= 0xa11100 && a < 0xa11102) {
        z80_busreq = ((width == 2 ? v >> 8 : v) & 1);
    } else if (a >= 0xa11200 && a < 0xa11202) {
        z80_reset = ((width == 2 ? v >> 8 : v) & 1);
        sound_reset_line(z80_reset);
    } else if (a >= 0xa00000 && a < 0xa10000) {
        if ((a & 0x7fff) < 0x4000)             /* word writes store the high byte */
            z80_ram[a & 0x1fff] = (uint8_t)(width == 2 ? v >> 8 : v);
        /* YM2612 and bank register: sound not modelled yet */
    }
}

uint8_t hw_read8(uint32_t a) { return (uint8_t)io_read(1, a); }
uint16_t hw_read16(uint32_t a) { return (uint16_t)io_read(2, a); }
void hw_write8(uint32_t a, uint8_t v) { io_write(1, a, v); }
void hw_write16(uint32_t a, uint16_t v) { io_write(2, a, v); }

/* ----------------------------------------------------------- interrupts */

/*
 * The HBlank handler is copied to RAM ($FF05D2, from $6DA6) and patches its
 * own source operand: each run copies one longword from a table to VSRAM and
 * then adds 4 to the operand. It is executed here exactly as the 68000 would
 * run the RAM copy, operand fetches included.
 */
#define HB 0xff05d2u
#define STEP(cyc) do { m68k_icount++; m68k_cycles += (cyc); } while (0)

static void hblank_handler(void)
{
    const uint8_t *pristine = m68k_rom + 0x6da6;
    uint32_t k;

    /* everything but the patched source operand must still match ROM */
    for (k = 0; k < 0x2e; k++)
        if ((k < 0x1e || k > 0x21) && m68k_ram[(HB + k) & 0xffff] != pristine[k])
            m68k_fault("HBlank RAM routine modified", HB + k);

    STEP(20);                                           /* cmpi.b #$DF,($C00008).l */
    f_cmp(1, rd8(HB + 3), rd8(rd32(HB + 4)));
    STEP(10);                                           /* bne.s */
    if (cpu.z) {
        STEP(20);                                       /* move.w #$8004,($C00004).l */
        wr16(rd32(HB + 0x0e), f_logic(2, rd16(HB + 0x0c)));
    }
    STEP(28);                                           /* move.l #$40000010,($C00004).l */
    wr32(rd32(HB + 0x18), f_logic(4, rd32(HB + 0x14)));
    STEP(36);                                           /* move.l (src).l,($C00000).l */
    {
        uint32_t v = rd32(rd32(HB + 0x1e));
        wr32(rd32(HB + 0x22), f_logic(4, v));
    }
    STEP(20);                                           /* addq.w #4,($FF05F2).l */
    {
        uint32_t ea = rd32(HB + 0x28);
        wr16(ea, f_add(2, 4, rd16(ea)));
    }
    STEP(20);                                           /* rte */
    {
        uint16_t sr = pop16();
        cpu.ret = pop32();
        set_sr(sr);
    }
}

void gen_take_irq(int level, uint32_t pc)
{
    if (level == 6 && getenv("VERIFY_TAINT_PC"))
        fprintf(stderr, "vblank at pc %06X\n", pc);
    verify_interrupt(level);
    push32(pc);
    push16(get_sr());
    cpu.sr_hi = (uint8_t)((cpu.sr_hi & 0xf8) | 0x20 | level);
    if (level == 6) {
        if (gen_replay && gen_replay->vblank)
            gen_replay->vblank();
        gen_entry_vblank();
    } else {
        hblank_handler();
    }
    verify_interrupt_end(level);
}

/* The Z80 gets its interrupt at VBlank; one frame of audio is produced here */
void gen_sound_frame(void)
{
    static uint32_t frac;
    /* YM_RATE / 59.9227 Hz = 888.93 samples per frame */
    int n = 888;
    frac += 93;
    if (frac >= 100) { frac -= 100; n++; }
    sound_vblank();
    sound_frame(n);
}

void gen_render_line(void)
{
    int l = vdp.line;
    if (l < 224)
        vdp_render_line(l, &gen_frame[l * 320]);
    /* the line interrupt counter runs on active lines and reloads in VBlank */
    if (l <= 224) {
        if (--vdp.hint_counter < 0) {
            vdp.hint_counter = vdp.reg[10];
            if (vdp.reg[0] & 0x10)
                hint_pending = 1;
        }
    } else {
        vdp.hint_counter = vdp.reg[10];
    }
}

static void next_line(uint32_t pc)
{
    int mask;
    gen_render_line();
    mask = cpu.sr_hi & 7;
    if (hint_pending && mask < 4) {   /* fires at the end of the line */
        hint_pending = 0;
        gen_take_irq(4, pc);
    }
    if (++vdp.line == 224) {
        vdp.in_vblank = 1;
        vdp.vint_pending = 1;
        gen_sound_frame();
        host_frame(gen_frame);
    } else if (vdp.line == TOTAL_LINES) {
        vdp.line = 0;
        vdp.in_vblank = 0;
    }
    mask = cpu.sr_hi & 7;
    if (vdp.vint_pending && (vdp.reg[1] & 0x20) && mask < 6) {
        vdp.vint_pending = 0;
        gen_take_irq(6, pc);
    }
}

void m68k_event(uint32_t pc)
{
    if (gen_replay) {
        int level;
        while ((level = gen_replay->irq(pc)) != 0)
            gen_take_irq(level, pc);
        return;
    }
    while (m68k_cycles >= m68k_cycles_event) {
        m68k_cycles_event += LINE_CYCLES;
        next_line(pc);
    }
}

/*
 * Let time pass until the next event for handwritten code that waits
 * (for VBlank): replay jumps the instruction count to the next recorded
 * interrupt, live play the cycle count to the next scanline. pc is the
 * original's address for the stacked return of an interrupt.
 */
void m68k_idle(uint32_t pc)
{
    if (gen_replay) {
        if (m68k_icount_event != UINT64_MAX && m68k_icount < m68k_icount_event)
            m68k_icount = m68k_icount_event;
    } else if (m68k_cycles < m68k_cycles_event) {
        m68k_cycles = m68k_cycles_event;
    }
    m68k_event(pc);
}

/* ---------------------------------------------------------------- reset */

void gen_init(const uint8_t *rom, uint32_t size)
{
    m68k_rom = rom;
    m68k_rom_size = size;
    memset(&cpu, 0, sizeof cpu);
    memset(m68k_ram, 0, sizeof m68k_ram);
    vdp_reset();
    ym_reset();
    cpu.a[7] = rd32(0);
    cpu.sr_hi = 0x27;
    m68k_cycles = 0;
    m68k_cycles_event = LINE_CYCLES;
    m68k_icount = 0;
}

void gen_run(void)
{
    gen_entry_reset();
    m68k_fault("reset handler returned", cpu.ret);
}
