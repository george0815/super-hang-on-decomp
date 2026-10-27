/*
 * Runtime support for statically recompiled 68000 code.
 *
 * CPU state lives in the global `cpu`. Work RAM is kept big-endian, exactly
 * as the 68000 sees it, so RAM can be compared byte-for-byte against traces
 * from the reference emulator. Anything outside ROM and RAM goes to the
 * host's hw_read/hw_write (VDP, Z80 bus, I/O ports).
 */
#ifndef M68K_RT_H
#define M68K_RT_H

#include <stdint.h>

typedef struct {
    uint32_t d[8];
    uint32_t a[8];      /* a[7] is the active stack pointer */
    uint32_t usp;       /* inactive user stack pointer */
    uint8_t x, n, z, v, c;
    uint8_t sr_hi;      /* T, S, interrupt mask (upper byte of SR) */
    uint32_t ret;       /* return address popped by the last rts/rte */
} M68kState;

extern M68kState cpu;
extern uint8_t m68k_ram[0x10000];
extern const uint8_t *m68k_rom;
extern uint32_t m68k_rom_size;

/* Provided by the host. Addresses are 24-bit. */
uint8_t hw_read8(uint32_t addr);
uint16_t hw_read16(uint32_t addr);
void hw_write8(uint32_t addr, uint8_t val);
void hw_write16(uint32_t addr, uint16_t val);
/* Called for any address the recompiler cannot reach statically. */
void m68k_dispatch(uint32_t addr);
/* Fatal runtime error (unknown jump target, bad return, ...). */
void m68k_fault(const char *what, uint32_t addr);

/* ------------------------------------------------------------- timing */

/* Every recompiled instruction starts with HOOK(pc, cycles). The host gets
   control (m68k_event) before the instruction runs whenever the instruction
   count or the cycle count reaches its next event; that is where interrupts
   are taken. Live play uses the cycle count to pace video timing; replay
   against a reference trace uses exact instruction counts. */
extern uint64_t m68k_icount, m68k_icount_event;
extern int64_t m68k_cycles, m68k_cycles_event;
void m68k_event(uint32_t pc);
void m68k_idle(uint32_t pc);   /* let time pass to the next event */
/* called as each game frame ends (system.c wait_frame_ticks), for tests */
extern void (*frame_end_hook)(void);
/* tests: race frames read the pads as they start (vblank.c frame_start_pads) */
extern int test_frame_pads;

#define HOOK(pc, cyc) do { \
        if (m68k_icount == m68k_icount_event || m68k_cycles >= m68k_cycles_event) \
            m68k_event(pc); \
        m68k_icount++; \
        m68k_cycles += (cyc); \
    } while (0)

/* ------------------------------------------------------------- memory */

static inline uint8_t rd8(uint32_t a)
{
    a &= 0xffffff;
    if (a >= 0xe00000) return m68k_ram[a & 0xffff];
    if (a < m68k_rom_size) return m68k_rom[a];
    return hw_read8(a);
}

static inline uint16_t rd16(uint32_t a)
{
    a &= 0xffffff;
    if (a >= 0xe00000) {
        a &= 0xffff;
        return (uint16_t)(m68k_ram[a] << 8 | m68k_ram[(a + 1) & 0xffff]);
    }
    if (a < m68k_rom_size) return (uint16_t)(m68k_rom[a] << 8 | m68k_rom[a + 1]);
    return hw_read16(a);
}

static inline uint32_t rd32(uint32_t a)
{
    return (uint32_t)rd16(a) << 16 | rd16(a + 2);
}

static inline void wr8(uint32_t a, uint32_t v)
{
    a &= 0xffffff;
    if (a >= 0xe00000) { m68k_ram[a & 0xffff] = (uint8_t)v; return; }
    if (a < m68k_rom_size) return;  /* ROM writes are ignored */
    hw_write8(a, (uint8_t)v);
}

static inline void wr16(uint32_t a, uint32_t v)
{
    a &= 0xffffff;
    if (a >= 0xe00000) {
        a &= 0xffff;
        m68k_ram[a] = (uint8_t)(v >> 8);
        m68k_ram[(a + 1) & 0xffff] = (uint8_t)v;
        return;
    }
    if (a < m68k_rom_size) return;
    hw_write16(a, (uint16_t)v);
}

static inline void wr32(uint32_t a, uint32_t v)
{
    /* the 68000 writes the high word first */
    wr16(a, v >> 16);
    wr16(a + 2, v);
}

static inline void push32(uint32_t v) { cpu.a[7] -= 4; wr32(cpu.a[7], v); }
static inline void push16(uint32_t v) { cpu.a[7] -= 2; wr16(cpu.a[7], v); }
static inline uint32_t pop32(void) { uint32_t v = rd32(cpu.a[7]); cpu.a[7] += 4; return v; }
static inline uint16_t pop16(void) { uint16_t v = rd16(cpu.a[7]); cpu.a[7] += 2; return v; }

/* --------------------------------------------------------- status reg */

static inline uint16_t get_sr(void)
{
    return (uint16_t)(cpu.sr_hi << 8 | cpu.x << 4 | cpu.n << 3 | cpu.z << 2 | cpu.v << 1 | cpu.c);
}

static inline uint8_t get_ccr(void) { return (uint8_t)get_sr(); }

static inline void set_ccr(uint32_t v)
{
    cpu.x = (v >> 4) & 1;
    cpu.n = (v >> 3) & 1;
    cpu.z = (v >> 2) & 1;
    cpu.v = (v >> 1) & 1;
    cpu.c = v & 1;
}

static inline void set_sr(uint32_t v)
{
    uint8_t hi = (uint8_t)((v >> 8) & 0xa7);
    if ((hi ^ cpu.sr_hi) & 0x20) {  /* supervisor bit changed: swap stacks */
        uint32_t t = cpu.a[7];
        cpu.a[7] = cpu.usp;
        cpu.usp = t;
    }
    cpu.sr_hi = hi;
    set_ccr(v);
}

/* ------------------------------------------------------------ helpers */

#define MSB(sz)  ((sz) == 1 ? 0x80u : (sz) == 2 ? 0x8000u : 0x80000000u)
#define MASK(sz) ((sz) == 1 ? 0xffu : (sz) == 2 ? 0xffffu : 0xffffffffu)

static inline uint32_t sx8(uint32_t v) { return (uint32_t)(int32_t)(int8_t)v; }
static inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }

/* store a sized result into a data register, keeping the upper bits */
static inline void set_dn(int n, int sz, uint32_t v)
{
    uint32_t m = MASK(sz);
    cpu.d[n] = (cpu.d[n] & ~m) | (v & m);
}

static inline void nz(int sz, uint32_t r)
{
    cpu.n = (r & MSB(sz)) != 0;
    cpu.z = (r & MASK(sz)) == 0;
}

static inline uint32_t f_logic(int sz, uint32_t r)
{
    r &= MASK(sz);
    nz(sz, r);
    cpu.v = cpu.c = 0;
    return r;
}

static inline uint32_t f_add(int sz, uint32_t s, uint32_t d)
{
    uint32_t m = MASK(sz), msb = MSB(sz);
    uint32_t r = (d + s) & m;
    s &= m; d &= m;
    nz(sz, r);
    cpu.v = (((s ^ r) & (d ^ r)) & msb) != 0;
    cpu.c = cpu.x = (((s & d) | (~r & (s | d))) & msb) != 0;
    return r;
}

static inline uint32_t f_sub(int sz, uint32_t s, uint32_t d)
{
    uint32_t m = MASK(sz), msb = MSB(sz);
    uint32_t r = (d - s) & m;
    s &= m; d &= m;
    nz(sz, r);
    cpu.v = (((s ^ d) & (r ^ d)) & msb) != 0;
    cpu.c = cpu.x = (((s & r) | (~d & (s | r))) & msb) != 0;
    return r;
}

static inline void f_cmp(int sz, uint32_t s, uint32_t d)
{
    uint8_t x = cpu.x;
    f_sub(sz, s, d);
    cpu.x = x;
}

static inline uint32_t f_addx(int sz, uint32_t s, uint32_t d)
{
    uint32_t m = MASK(sz), msb = MSB(sz);
    uint32_t r = (d + s + cpu.x) & m;
    uint8_t z = cpu.z;
    s &= m; d &= m;
    nz(sz, r);
    cpu.z = z && r == 0;
    cpu.v = (((s ^ r) & (d ^ r)) & msb) != 0;
    cpu.c = cpu.x = (((s & d) | (~r & (s | d))) & msb) != 0;
    return r;
}

static inline uint32_t f_subx(int sz, uint32_t s, uint32_t d)
{
    uint32_t m = MASK(sz), msb = MSB(sz);
    uint32_t r = (d - s - cpu.x) & m;
    uint8_t z = cpu.z;
    s &= m; d &= m;
    nz(sz, r);
    cpu.z = z && r == 0;
    cpu.v = (((s ^ d) & (r ^ d)) & msb) != 0;
    cpu.c = cpu.x = (((s & r) | (~d & (s | r))) & msb) != 0;
    return r;
}

/* Shifts and rotates. count is 0..63; bit-at-a-time keeps the flag rules
   (last bit out, V on any sign change, count==0 cases) obviously right. */
enum { SH_ASL, SH_ASR, SH_LSL, SH_LSR, SH_ROL, SH_ROR, SH_ROXL, SH_ROXR };

static inline uint32_t f_shift(int kind, int sz, uint32_t v, int count)
{
    uint32_t m = MASK(sz), msb = MSB(sz);
    uint32_t r = v & m;
    int i;
    uint8_t c = 0, vf = 0;

    if (count == 0) {
        cpu.c = (kind == SH_ROXL || kind == SH_ROXR) ? cpu.x : 0;
        cpu.v = 0;
        nz(sz, r);
        return r;
    }
    for (i = 0; i < count; i++) {
        uint32_t old = r;
        switch (kind) {
        case SH_ASL:  c = (r & msb) != 0; r = (r << 1) & m; if ((old ^ r) & msb) vf = 1; break;
        case SH_ASR:  c = r & 1; r = (r >> 1) | (r & msb); break;
        case SH_LSL:  c = (r & msb) != 0; r = (r << 1) & m; break;
        case SH_LSR:  c = r & 1; r >>= 1; break;
        case SH_ROL:  c = (r & msb) != 0; r = ((r << 1) | c) & m; break;
        case SH_ROR:  c = r & 1; r = (r >> 1) | (c ? msb : 0); break;
        case SH_ROXL: c = (r & msb) != 0; r = ((r << 1) | cpu.x) & m; cpu.x = c; break;
        case SH_ROXR: c = r & 1; r = (r >> 1) | (cpu.x ? msb : 0); cpu.x = c; break;
        }
    }
    cpu.c = c;
    if (kind <= SH_LSR) cpu.x = c;
    cpu.v = vf;
    nz(sz, r);
    return r;
}

static inline uint32_t f_mulu(uint32_t s, uint32_t d)
{
    uint32_t r = (s & 0xffff) * (d & 0xffff);
    nz(4, r);
    cpu.v = cpu.c = 0;
    return r;
}

static inline uint32_t f_muls(uint32_t s, uint32_t d)
{
    uint32_t r = (uint32_t)((int32_t)(int16_t)s * (int32_t)(int16_t)d);
    nz(4, r);
    cpu.v = cpu.c = 0;
    return r;
}

/* Division by zero raises an exception on hardware; the game never does it,
   so it is treated as fatal. On overflow the destination is unchanged and
   N is set (matches real hardware and the reference emulator). */
static inline uint32_t f_divu(uint32_t s, uint32_t d)
{
    uint32_t q, rem;
    s &= 0xffff;
    if (s == 0) { cpu.c = 0; m68k_fault("divu by zero", d); return d; }
    q = d / s;
    rem = d % s;
    if (q > 0xffff) { cpu.v = 1; cpu.n = 1; cpu.c = 0; return d; }
    cpu.z = q == 0;
    cpu.n = (q & 0x8000) != 0;
    cpu.v = cpu.c = 0;
    return rem << 16 | q;
}

static inline uint32_t f_divs(uint32_t s, uint32_t d)
{
    int32_t sd = (int32_t)d, ss = (int16_t)s, q, rem;
    if (ss == 0) { cpu.c = 0; m68k_fault("divs by zero", d); return d; }
    if (d == 0x80000000u && ss == -1) {
        cpu.z = cpu.n = cpu.v = cpu.c = 0;
        return 0;
    }
    q = sd / ss;
    rem = sd % ss;
    if (q != (int16_t)q) { cpu.v = 1; cpu.n = 1; cpu.c = 0; return d; }
    cpu.z = (q & 0xffff) == 0;
    cpu.n = (q & 0x8000) != 0;
    cpu.v = cpu.c = 0;
    return (uint32_t)(rem & 0xffff) << 16 | (q & 0xffff);
}

/* Packed BCD. N and V are officially undefined; these follow what real
   hardware produces (and what the reference emulator models). */
static inline uint32_t f_abcd(uint32_t s, uint32_t d)
{
    uint32_t lo = (s & 0x0f) + (d & 0x0f) + cpu.x;
    uint32_t bin = lo + (s & 0xf0) + (d & 0xf0);   /* binary sum before decimal adjust */
    uint32_t r = bin + (lo > 9 ? 6 : 0);
    uint8_t carry = r > 0x9f;
    if (carry) r -= 0xa0;
    cpu.v = ((~bin & r) & 0x80) != 0;
    cpu.x = cpu.c = carry;
    r &= 0xff;
    cpu.n = (r & 0x80) != 0;
    if (r) cpu.z = 0;
    return r;
}

static inline uint32_t f_sbcd(uint32_t s, uint32_t d)
{
    uint32_t lo = (d & 0x0f) - (s & 0x0f) - cpu.x;
    uint32_t adj = lo > 0x0f ? 6 : 0;
    uint32_t bin = lo + (d & 0xf0) - (s & 0xf0);    /* binary difference */
    uint32_t r = bin;
    uint8_t borrow;
    if (r > 0xff) { r += 0xa0; borrow = 1; }
    else borrow = r < adj;
    r = (r - adj) & 0xff;
    cpu.v = ((bin & ~r) & 0x80) != 0;
    cpu.x = cpu.c = borrow;
    cpu.n = (r & 0x80) != 0;
    if (r) cpu.z = 0;
    return r;
}

static inline uint32_t f_nbcd(uint32_t d)
{
    return f_sbcd(d, 0);
}

/* condition codes, numbered as in the opcode */
static inline int cond(int cc)
{
    switch (cc) {
    case 0:  return 1;
    case 1:  return 0;
    case 2:  return !cpu.c && !cpu.z;
    case 3:  return cpu.c || cpu.z;
    case 4:  return !cpu.c;
    case 5:  return cpu.c;
    case 6:  return !cpu.z;
    case 7:  return cpu.z;
    case 8:  return !cpu.v;
    case 9:  return cpu.v;
    case 10: return !cpu.n;
    case 11: return cpu.n;
    case 12: return cpu.n == cpu.v;
    case 13: return cpu.n != cpu.v;
    case 14: return !cpu.z && cpu.n == cpu.v;
    default: return cpu.z || cpu.n != cpu.v;
    }
}

#endif
