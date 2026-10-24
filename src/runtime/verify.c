#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k_rt.h"
#include "verify.h"

int verify_mode;
int verify_enabled;
long verify_failures;

#define MAX_IO 65536
#define MAX_FUNCS 512

typedef struct { uint8_t kind, width; uint32_t addr, value; } Access;

static Access io[MAX_IO];
static int nio, iopos, io_overflow, io_mismatch, tainted;
static char mismatch_msg[160];

static struct {
    uint32_t addr;
    const char *name;
    long calls, checked, skipped, failed;
} stats[MAX_FUNCS];
static int nstats;

static int stat_index(uint32_t addr, const char *name)
{
    int i;
    for (i = 0; i < nstats; i++)
        if (stats[i].addr == addr) return i;
    if (nstats == MAX_FUNCS) return -1;
    stats[nstats].addr = addr;
    stats[nstats].name = name;
    return nstats++;
}

/* ------------------------------------------------------------ hardware */

int verify_hw_read(int width, uint32_t addr, uint32_t *value)
{
    if (verify_mode != VERIFY_SHADOW)
        return 0;   /* performed normally (and logged by verify_hw_log_read) */
    if (iopos >= nio || io[iopos].kind != 'R' || io[iopos].width != width || io[iopos].addr != addr) {
        if (!io_mismatch)
            snprintf(mismatch_msg, sizeof mismatch_msg, "hardware read #%d %d@%06X not in original's sequence",
                     iopos, width, addr);
        io_mismatch = 1;
        *value = 0;
        return 1;
    }
    *value = io[iopos++].value;
    return 1;
}

void verify_hw_log_read(int width, uint32_t addr, uint32_t value)
{
    if (verify_mode != VERIFY_ORIGINAL) return;
    if (nio == MAX_IO) { io_overflow = 1; return; }
    io[nio++] = (Access){ 'R', (uint8_t)width, addr, value };
}

int verify_hw_write(int width, uint32_t addr, uint32_t value)
{
    if (verify_mode == VERIFY_ORIGINAL) {
        if (nio == MAX_IO) io_overflow = 1;
        else io[nio++] = (Access){ 'W', (uint8_t)width, addr, value };
        return 0;   /* and perform it */
    }
    if (verify_mode != VERIFY_SHADOW)
        return 0;
    if (iopos >= nio || io[iopos].kind != 'W' || io[iopos].width != width ||
        io[iopos].addr != addr || io[iopos].value != value) {
        if (!io_mismatch)
            snprintf(mismatch_msg, sizeof mismatch_msg,
                     "hardware write #%d %d@%06X=%X, original %c%d@%06X=%X", iopos, width, addr, value,
                     iopos < nio ? io[iopos].kind : '-', iopos < nio ? io[iopos].width : 0,
                     iopos < nio ? io[iopos].addr : 0, iopos < nio ? io[iopos].value : 0);
        io_mismatch = 1;
    } else {
        iopos++;
    }
    return 1;   /* captured, not performed */
}

void verify_interrupt(void)
{
    if (verify_mode == VERIFY_ORIGINAL)
        tainted = 1;
}

/* ---------------------------------------------------------- comparison */

static const char *reg_part_name(int bit)
{
    static char buf[16];
    static const char *flags = "XNZVC";
    if (bit < 24) snprintf(buf, sizeof buf, "d%d.%s", bit / 3, (const char *[]){ "b0", "b1", "hi" }[bit % 3]);
    else if (bit < 31) snprintf(buf, sizeof buf, "a%d", bit - 24);
    else snprintf(buf, sizeof buf, "%c", flags[bit - 32]);
    return buf;
}

static uint32_t reg_part(const M68kState *s, int bit)
{
    if (bit < 24) {
        uint32_t d = s->d[bit / 3];
        return bit % 3 == 0 ? (d & 0xff) : bit % 3 == 1 ? ((d >> 8) & 0xff) : d >> 16;
    }
    if (bit < 31) return s->a[bit - 24];
    switch (bit - 32) {
    case 0: return s->x;
    case 1: return s->n;
    case 2: return s->z;
    case 3: return s->v;
    default: return s->c;
    }
}

void verify_call(uint32_t addr, const char *name, void (*original)(void),
                 void (*handwritten)(void), uint64_t outputs)
{
    static uint8_t ram_before[0x10000], ram_after[0x10000];
    M68kState cpu_before, cpu_after;
    uint64_t icount, icount_event;
    int64_t cycles, cycles_event;
    int k = stat_index(addr, name);
    long n;
    char msg[256] = "";
    uint32_t sp, stack_lo;
    int bit;

    if (verify_mode == VERIFY_SHADOW) { handwritten(); return; }
    if (!verify_enabled || verify_mode == VERIFY_ORIGINAL || k < 0) { original(); return; }

    n = ++stats[k].calls;
    /* sample: every call at first, then a spread */
    if (n > 2000 && n % 97) { original(); return; }

    memcpy(ram_before, m68k_ram, sizeof ram_before);
    cpu_before = cpu;
    nio = iopos = io_overflow = io_mismatch = tainted = 0;

    verify_mode = VERIFY_ORIGINAL;
    original();
    verify_mode = VERIFY_OFF;

    if (tainted || io_overflow) {   /* interrupted: not a pure function of its inputs */
        stats[k].skipped++;
        return;
    }
    memcpy(ram_after, m68k_ram, sizeof ram_after);
    cpu_after = cpu;
    icount = m68k_icount; icount_event = m68k_icount_event;
    cycles = m68k_cycles; cycles_event = m68k_cycles_event;

    /* shadow run from the same starting point; no interrupts, no timing */
    memcpy(m68k_ram, ram_before, sizeof ram_before);
    cpu = cpu_before;
    m68k_icount_event = UINT64_MAX;
    m68k_cycles_event = INT64_MAX;
    verify_mode = VERIFY_SHADOW;
    handwritten();
    verify_mode = VERIFY_OFF;

    stats[k].checked++;
    sp = cpu_before.a[7];
    /* bytes below the entry stack pointer are the function's scratch */
    stack_lo = (sp >= 0xff0000 && sp < 0xff1000) ? (sp & 0xffff) : 0;

    if (io_mismatch)
        snprintf(msg, sizeof msg, "%s", mismatch_msg);
    else if (iopos != nio)
        snprintf(msg, sizeof msg, "made %d of the original's %d hardware accesses", iopos, nio);
    else if (cpu.a[7] != cpu_after.a[7])
        snprintf(msg, sizeof msg, "sp = %08X, original %08X", cpu.a[7], cpu_after.a[7]);
    else if (cpu.ret != cpu_after.ret)
        snprintf(msg, sizeof msg, "returned to %06X, original %06X", cpu.ret, cpu_after.ret);
    else {
        for (bit = 0; bit < 37 && !msg[0]; bit++)
            if ((outputs >> bit) & 1)
                if (reg_part(&cpu, bit) != reg_part(&cpu_after, bit))
                    snprintf(msg, sizeof msg, "%s = %X, original %X", reg_part_name(bit),
                             reg_part(&cpu, bit), reg_part(&cpu_after, bit));
        if (!msg[0]) {
            uint32_t i;
            for (i = 0; i < 0x10000; i++) {
                if (i < stack_lo) continue;
                if (m68k_ram[i] != ram_after[i]) {
                    snprintf(msg, sizeof msg, "ram[FF%04X] = %02X, original %02X (was %02X)",
                             i, m68k_ram[i], ram_after[i], ram_before[i]);
                    break;
                }
            }
        }
    }
    if (msg[0]) {
        stats[k].failed++;
        if (verify_failures++ < 50)
            fprintf(stderr, "verify %s ($%06X) call %ld: %s\n", name, addr, n, msg);
    }

    /* continue with the original's results */
    memcpy(m68k_ram, ram_after, sizeof ram_after);
    cpu = cpu_after;
    m68k_icount = icount; m68k_icount_event = icount_event;
    m68k_cycles = cycles; m68k_cycles_event = cycles_event;
}

void verify_report(void)
{
    int i;
    if (!nstats) return;
    printf("%-28s %8s %8s %8s %8s\n", "handwritten function", "calls", "checked", "skipped", "FAILED");
    for (i = 0; i < nstats; i++)
        printf("%-28s %8ld %8ld %8ld %8ld\n", stats[i].name, stats[i].calls, stats[i].checked,
               stats[i].skipped, stats[i].failed);
}
