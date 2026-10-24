/*
 * Per-function differential test for the recompiled code.
 *
 * Replays samples captured by tools/trace/gpgx_fncap: loads the entry
 * registers and RAM, runs the recompiled function with hardware reads served
 * from the recorded log, then checks the result registers, RAM and the exact
 * sequence of hardware accesses against the reference.
 *
 * usage: fntest ROM SAMPLES [-v] [-f ENTRY]
 */
#define _POSIX_C_SOURCE 200809L
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "m68k_rt.h"
#include "sho_funcs.h"

M68kState cpu;
uint8_t m68k_ram[0x10000];
const uint8_t *m68k_rom;
uint32_t m68k_rom_size;
uint64_t m68k_icount, m68k_icount_event = UINT64_MAX;
int64_t m68k_cycles, m68k_cycles_event = INT64_MAX;

void m68k_event(uint32_t pc) { (void)pc; }

typedef struct { uint8_t kind, width; uint32_t addr, value; } IoEntry;

static IoEntry *io;
static uint32_t nio, iopos;
static sigjmp_buf abort_jmp;
static char failmsg[256];

#define FAIL(...) do { snprintf(failmsg, sizeof failmsg, __VA_ARGS__); siglongjmp(abort_jmp, 1); } while (0)

static uint32_t io_next(char kind, int width, uint32_t addr)
{
    IoEntry *e;
    if (iopos >= nio)
        FAIL("extra %c%d access at %06X (log has %u)", kind, width, addr, nio);
    e = &io[iopos++];
    if (e->kind != kind || e->width != width || e->addr != addr)
        FAIL("io #%u: got %c%d %06X, expected %c%d %06X", iopos - 1, kind, width, addr,
             e->kind, e->width, e->addr);
    return e->value;
}

uint8_t hw_read8(uint32_t a) { return (uint8_t)io_next('R', 1, a); }
uint16_t hw_read16(uint32_t a) { return (uint16_t)io_next('R', 2, a); }

void hw_write8(uint32_t a, uint8_t v)
{
    uint32_t want = io_next('W', 1, a);
    if ((want & 0xff) != v) FAIL("io #%u: write8 %06X = %02X, expected %02X", iopos - 1, a, v, want & 0xff);
}

void hw_write16(uint32_t a, uint16_t v)
{
    uint32_t want = io_next('W', 2, a);
    if ((want & 0xffff) != v) FAIL("io #%u: write16 %06X = %04X, expected %04X", iopos - 1, a, v, want & 0xffff);
}

void m68k_fault(const char *what, uint32_t addr) { FAIL("fault: %s (%06X)", what, addr); }
void m68k_stop(void) { FAIL("stop instruction"); }

static void on_alarm(int sig) { (void)sig; FAIL("timeout (infinite loop?)"); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void load_regs(const uint8_t *p)
{
    int i;
    for (i = 0; i < 8; i++) cpu.d[i] = be32(p + 4 * i);
    for (i = 0; i < 8; i++) cpu.a[i] = be32(p + 32 + 4 * i);
    cpu.sr_hi = (uint8_t)(be32(p + 64) >> 8);
    set_ccr(be32(p + 64));
    cpu.usp = be32(p + 68);
}

static const char *regname(int i)
{
    static const char *n[] = { "d0","d1","d2","d3","d4","d5","d6","d7",
                               "a0","a1","a2","a3","a4","a5","a6","a7","sr","usp" };
    return n[i];
}

static uint8_t *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(*len);
    if (fread(buf, 1, *len, f) != (size_t)*len) { perror(path); exit(1); }
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    long romlen, slen, pos;
    uint8_t *samples;
    int verbose = 0, i;
    long only = -1;
    long pass = 0, fail = 0;
    static uint8_t status[0x10000];   /* 1 = some pass, 2 = some fail */
    struct sigaction sa;

    if (argc < 3) {
        fprintf(stderr, "usage: %s ROM SAMPLES [-v] [-f ENTRY]\n", argv[0]);
        return 1;
    }
    for (i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) only = strtol(argv[++i], NULL, 16);
    }
    m68k_rom = read_file(argv[1], &romlen);
    m68k_rom_size = (uint32_t)romlen;
    samples = read_file(argv[2], &slen);
    if (slen < 12 || memcmp(samples, "SHOFNCAP", 8)) { fprintf(stderr, "bad sample file\n"); return 1; }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    for (pos = 12; pos + 16 <= slen;) {
        const uint8_t *h = samples + pos;
        uint32_t entry = be32(h), ret = be32(h + 4), frame = be32(h + 8);
        const uint8_t *regs_in = h + 16, *ram_in = regs_in + 72;
        const uint8_t *regs_out = ram_in + 0x10000, *ram_out = regs_out + 72;
        const uint8_t *iop = ram_out + 0x10000;
        uint32_t k;

        nio = be32(h + 12);
        pos = (iop - samples) + 10L * nio;
        if (only >= 0 && entry != (uint32_t)only) continue;

        io = realloc(io, sizeof(IoEntry) * (nio + 1));
        for (k = 0; k < nio; k++) {
            io[k].kind = iop[10 * k];
            io[k].width = iop[10 * k + 1];
            io[k].addr = be32(iop + 10 * k + 2);
            io[k].value = be32(iop + 10 * k + 6);
        }
        iopos = 0;
        memcpy(m68k_ram, ram_in, sizeof m68k_ram);
        memset(&cpu, 0, sizeof cpu);
        load_regs(regs_in);
        failmsg[0] = 0;

        if (sigsetjmp(abort_jmp, 1) == 0) {
            alarm(2);
            m68k_dispatch(entry);
            alarm(0);
            if (cpu.ret != ret)
                snprintf(failmsg, sizeof failmsg, "returned to %06X, expected %06X", cpu.ret, ret);
            else if (iopos != nio)
                snprintf(failmsg, sizeof failmsg, "only %u of %u io accesses made", iopos, nio);
            else {
                uint32_t got[18];
                for (i = 0; i < 8; i++) got[i] = cpu.d[i];
                for (i = 0; i < 8; i++) got[8 + i] = cpu.a[i];
                got[16] = get_sr();
                got[17] = cpu.usp;
                for (i = 0; i < 18 && !failmsg[0]; i++)
                    if (got[i] != be32(regs_out + 4 * i))
                        snprintf(failmsg, sizeof failmsg, "%s = %08X, expected %08X",
                                 regname(i), got[i], be32(regs_out + 4 * i));
                for (k = 0; k < 0x10000 && !failmsg[0]; k++)
                    if (m68k_ram[k] != ram_out[k])
                        snprintf(failmsg, sizeof failmsg, "ram[FF%04X] = %02X, expected %02X (was %02X)",
                                 k, m68k_ram[k], ram_out[k], ram_in[k]);
            }
        } else {
            alarm(0);
        }

        if (failmsg[0]) {
            fail++;
            status[entry & 0xffff] |= 2;
            printf("FAIL %06X (frame %u, %u io): %s\n", entry, frame, nio, failmsg);
        } else {
            pass++;
            status[entry & 0xffff] |= 1;
            if (verbose) printf("ok   %06X (frame %u, %u io)\n", entry, frame, nio);
        }
    }
    {
        int fpass = 0, ffail = 0;
        for (i = 0; i < 0x10000; i++) {
            if (status[i] & 2) ffail++;
            else if (status[i] & 1) fpass++;
        }
        printf("\n%ld/%ld samples passed; functions: %d pass, %d fail\n",
               pass, pass + fail, fpass, ffail);
    }
    return fail != 0;
}
