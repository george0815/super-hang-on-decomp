/*
 * Stand-in for src/runtime/m68k_rt.h in the SNES compile
 * (tools/snes/c816.py): the memory accessors and the few runtime calls
 * the game code makes, which the compiler turns into code or runtime
 * calls (snes/rt.s). The register state is declared only so the adapters
 * (_68k), which are left out, still parse.
 */
#ifndef M68K_RT_H
#define M68K_RT_H
#include <stdint.h>

uint8_t rd8(uint32_t a);
uint16_t rd16(uint32_t a);
uint32_t rd32(uint32_t a);
void wr8(uint32_t a, uint32_t v);
void wr16(uint32_t a, uint32_t v);
void wr32(uint32_t a, uint32_t v);
uint32_t sx8(uint32_t v);
uint32_t sx16(uint32_t v);
uint16_t get_sr(void);
void set_sr(uint32_t v);
void m68k_idle(uint32_t pc);
void m68k_fault(const char *what, uint32_t v);

typedef struct {
    uint32_t d[8], a[8];
    uint8_t x, sr_hi;
    uint32_t ret;
} M68kState;
extern M68kState cpu;
void set_dn(int n, int sz, uint32_t v);
uint32_t f_logic(int sz, uint32_t r);
uint32_t f_add(int sz, uint32_t s, uint32_t d);
void f_cmp(int sz, uint32_t s, uint32_t d);
uint32_t f_divs(uint32_t s, uint32_t d);
uint32_t f_divu(uint32_t s, uint32_t d);
uint16_t pop16(void);
/* pad-table test builds read the race's pads as each frame starts */
#ifdef PADTABLE
#define test_frame_pads 1
#else
#define test_frame_pads 0
#endif
#endif
