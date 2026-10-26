/*
 * The Z80's view of memory, shared by the C sound driver (src/sound) and
 * the recompiled original it is verified against (build/recomp/z80_code.c,
 * which also uses the register state and flag helpers below).
 */
#ifndef Z80_RT_H
#define Z80_RT_H

#include <stdint.h>

typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a2, f2, b2, c2, d2, e2, h2, l2;   /* shadow set */
    uint16_t ix, iy, sp;
    uint16_t ret;   /* return address popped by the last ret */
} Z80State;

extern Z80State z80;

uint8_t z80_read(uint16_t addr);
void z80_write(uint16_t addr, uint8_t v);
void z80_dispatch(uint16_t addr);
void z80_fault(const char *what, uint16_t addr);

enum { ZF_C = 0x01, ZF_N = 0x02, ZF_PV = 0x04, ZF_X = 0x08, ZF_H = 0x10, ZF_Y = 0x20, ZF_Z = 0x40, ZF_S = 0x80 };

#define Z_BC ((uint16_t)(z80.b << 8 | z80.c))
#define Z_DE ((uint16_t)(z80.d << 8 | z80.e))
#define Z_HL ((uint16_t)(z80.h << 8 | z80.l))
#define Z_AF ((uint16_t)(z80.a << 8 | z80.f))

static inline void z_set_bc(uint16_t v) { z80.b = (uint8_t)(v >> 8); z80.c = (uint8_t)v; }
static inline void z_set_de(uint16_t v) { z80.d = (uint8_t)(v >> 8); z80.e = (uint8_t)v; }
static inline void z_set_hl(uint16_t v) { z80.h = (uint8_t)(v >> 8); z80.l = (uint8_t)v; }
static inline void z_set_af(uint16_t v) { z80.a = (uint8_t)(v >> 8); z80.f = (uint8_t)v; }

static inline void z_push(uint16_t v)
{
    z80.sp -= 2;
    z80_write(z80.sp, (uint8_t)v);
    z80_write((uint16_t)(z80.sp + 1), (uint8_t)(v >> 8));
}

static inline uint16_t z_pop(void)
{
    uint16_t v = (uint16_t)(z80_read(z80.sp) | z80_read((uint16_t)(z80.sp + 1)) << 8);
    z80.sp += 2;
    return v;
}

static inline uint16_t z_rd16(uint16_t a)
{
    return (uint16_t)(z80_read(a) | z80_read((uint16_t)(a + 1)) << 8);
}

static inline void z_wr16(uint16_t a, uint16_t v)
{
    z80_write(a, (uint8_t)v);
    z80_write((uint16_t)(a + 1), (uint8_t)(v >> 8));
}

static inline uint8_t z_parity(uint8_t v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) ? 0 : ZF_PV;
}

static inline uint8_t z_szxy(uint8_t r)
{
    return (uint8_t)((r & (ZF_S | ZF_X | ZF_Y)) | (r ? 0 : ZF_Z));
}

/* 8-bit arithmetic; results land in A except for cp */
static inline uint8_t z_add8(uint8_t a, uint8_t b, int carry)
{
    unsigned r = a + b + carry;
    z80.f = (uint8_t)(z_szxy((uint8_t)r) | (r > 0xff ? ZF_C : 0) |
                      ((a ^ b ^ r) & ZF_H) | ((~(a ^ b) & (a ^ r) & 0x80) ? ZF_PV : 0));
    return (uint8_t)r;
}

static inline uint8_t z_sub8(uint8_t a, uint8_t b, int carry)
{
    unsigned r = a - b - carry;
    z80.f = (uint8_t)(z_szxy((uint8_t)r) | ZF_N | (r > 0xff ? ZF_C : 0) |
                      ((a ^ b ^ r) & ZF_H) | (((a ^ b) & (a ^ r) & 0x80) ? ZF_PV : 0));
    return (uint8_t)r;
}

static inline void z_cp(uint8_t a, uint8_t b)
{
    z_sub8(a, b, 0);
    z80.f = (uint8_t)((z80.f & ~(ZF_X | ZF_Y)) | (b & (ZF_X | ZF_Y)));
}

static inline uint8_t z_logic(uint8_t r, int is_and)
{
    z80.f = (uint8_t)(z_szxy(r) | z_parity(r) | (is_and ? ZF_H : 0));
    return r;
}

static inline uint8_t z_inc8(uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1);
    z80.f = (uint8_t)((z80.f & ZF_C) | z_szxy(r) | ((r & 0x0f) == 0 ? ZF_H : 0) | (r == 0x80 ? ZF_PV : 0));
    return r;
}

static inline uint8_t z_dec8(uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1);
    z80.f = (uint8_t)((z80.f & ZF_C) | z_szxy(r) | ZF_N | ((v & 0x0f) == 0 ? ZF_H : 0) | (v == 0x80 ? ZF_PV : 0));
    return r;
}

static inline uint16_t z_add16(uint16_t a, uint16_t b)
{
    unsigned r = a + b;
    z80.f = (uint8_t)((z80.f & (ZF_S | ZF_Z | ZF_PV)) | ((r >> 8) & (ZF_X | ZF_Y)) |
                      (r > 0xffff ? ZF_C : 0) | (((a ^ b ^ r) >> 8) & ZF_H));
    return (uint16_t)r;
}

static inline uint16_t z_sbc16(uint16_t a, uint16_t b)
{
    unsigned r = a - b - (z80.f & ZF_C);
    z80.f = (uint8_t)(((r >> 8) & (ZF_S | ZF_X | ZF_Y)) | ((r & 0xffff) ? 0 : ZF_Z) | ZF_N |
                      (r > 0xffff ? ZF_C : 0) | (((a ^ b ^ r) >> 8) & ZF_H) |
                      ((((a ^ b) & (a ^ r)) & 0x8000) ? ZF_PV : 0));
    return (uint16_t)r;
}

/* rotates/shifts with full flag update (CB prefix forms) */
static inline uint8_t z_rr(uint8_t v)
{
    uint8_t r = (uint8_t)((v >> 1) | ((z80.f & ZF_C) ? 0x80 : 0));
    z80.f = (uint8_t)(z_szxy(r) | z_parity(r) | (v & 1));
    return r;
}

static inline uint8_t z_srl(uint8_t v)
{
    uint8_t r = (uint8_t)(v >> 1);
    z80.f = (uint8_t)(z_szxy(r) | z_parity(r) | (v & 1));
    return r;
}

static inline void z_rrca(void)
{
    uint8_t c = z80.a & 1;
    z80.a = (uint8_t)((z80.a >> 1) | (c << 7));
    z80.f = (uint8_t)((z80.f & (ZF_S | ZF_Z | ZF_PV)) | (z80.a & (ZF_X | ZF_Y)) | c);
}

static inline void z_bit(int n, uint8_t v)
{
    uint8_t r = v & (1 << n);
    z80.f = (uint8_t)((z80.f & ZF_C) | ZF_H | (r ? 0 : (ZF_Z | ZF_PV)) | (r & ZF_S) | (v & (ZF_X | ZF_Y)));
}

static inline void z_ldi(int dir)
{
    uint8_t v = z80_read(Z_HL);
    z80_write(Z_DE, v);
    z_set_hl((uint16_t)(Z_HL + dir));
    z_set_de((uint16_t)(Z_DE + dir));
    z_set_bc((uint16_t)(Z_BC - 1));
    z80.f = (uint8_t)((z80.f & (ZF_S | ZF_Z | ZF_C)) | (Z_BC ? ZF_PV : 0));
}

static inline int z_cond(int cc)
{
    switch (cc) {
    case 0: return !(z80.f & ZF_Z);
    case 1: return (z80.f & ZF_Z) != 0;
    case 2: return !(z80.f & ZF_C);
    case 3: return (z80.f & ZF_C) != 0;
    case 4: return !(z80.f & ZF_PV);
    case 5: return (z80.f & ZF_PV) != 0;
    case 6: return !(z80.f & ZF_S);
    default: return (z80.f & ZF_S) != 0;
    }
}

#endif
