/*
 * Sound driver entry points for the verifying replay: each call runs the
 * original Z80 program (recompiled, build/recomp/z80_code.c) and then the
 * C driver from the same state, and compares the Z80 RAM, every write to
 * the sound chips and the bank register, the bank and IX. The replay goes
 * on with the original's results. SOUND_C=1 runs the C driver alone (the
 * replay's --zram check then follows it end to end).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "z80_rt.h"
#include "z80_funcs.h"
#include "genesis.h"
#include "sound.h"
#include "driver.h"

static void (*const original[SND_ENTRIES])(void) = {
    z_0038, z_0091, z_11E8, z_0C37, z_1070, z_1015, z_001F, z_0027,
};
static void (*const handwritten[SND_ENTRIES])(void) = {
    drv_irq, drv_tick, drv_requests, drv_stop_all, drv_fm_reset, drv_bank_music,
    drv_timer_start, drv_timer_reset,
};
static const char *const names[SND_ENTRIES] = {
    "irq $0038", "tick $0091", "requests $11E8", "stop_all $0C37", "fm_reset $1070",
    "bank $1015", "timer_start $001F", "timer_reset $0027",
};

#define MAX_LOG 65536
typedef struct { uint16_t addr; uint8_t v; } PortWrite;
static PortWrite logs[2][MAX_LOG];
static int nlog[2], cur;
static long calls[SND_ENTRIES], failed[SND_ENTRIES], total_failed;
volatile int sound_verify_running;   /* 1 original, 2 C (for a watchdog) */
int sound_verify_quiet;              /* no table at exit */
long sound_verify_failed(void) { return total_failed; }

static void port_log(uint16_t addr, uint8_t v)
{
    if (nlog[cur] < MAX_LOG) {
        logs[cur][nlog[cur]].addr = addr;
        logs[cur][nlog[cur]].v = v;
    }
    nlog[cur]++;
}

static void report(void)
{
    int e;
    printf("sound driver entry          calls   FAILED\n");
    for (e = 0; e < SND_ENTRIES; e++)
        if (calls[e])
            printf("%-24s %8ld %8ld\n", names[e], calls[e], failed[e]);
}

static void mismatch(int e, const char *what)
{
    failed[e]++;
    if (total_failed++ < 20)
        printf("sound %s call %ld: %s\n", names[e], calls[e], what);
}

void sound_call(int e)
{
    static uint8_t before[0x2000], after[0x2000];
    static int c_only = -1;
    uint16_t bank0, bank1, ix1;
    char msg[96] = "";
    int k;

    if (c_only < 0) {
        c_only = getenv("SOUND_C") != NULL;
        if (!c_only && !sound_verify_quiet)
            atexit(report);
    }
    if (c_only) {
        handwritten[e]();
        return;
    }
    calls[e]++;
    memcpy(before, z80_ram, sizeof before);
    bank0 = sound_bank_get();
    drv_ix = z80.ix;

    cur = 0;
    nlog[0] = 0;
    sound_port_log = port_log;
    z_push(0xffff);
    sound_verify_running = 1;
    original[e]();
    memcpy(after, z80_ram, sizeof after);
    bank1 = sound_bank_get();
    ix1 = z80.ix;

    memcpy(z80_ram, before, sizeof before);
    sound_bank_set(bank0);
    cur = 1;
    nlog[1] = 0;
    sound_ports_muted = 1;
    sound_verify_running = 2;
    handwritten[e]();
    sound_verify_running = 0;
    sound_ports_muted = 0;
    sound_port_log = NULL;

    for (k = 0; k < nlog[0] && k < nlog[1] && k < MAX_LOG; k++)
        if (logs[0][k].addr != logs[1][k].addr || logs[0][k].v != logs[1][k].v)
            break;
    if (nlog[0] != nlog[1] || (k < nlog[0] && k < MAX_LOG)) {
        if (k < nlog[0] && k < nlog[1] && k < MAX_LOG)
            snprintf(msg, sizeof msg, "write %d: $%04X = %02X, original $%04X = %02X", k,
                     logs[1][k].addr, logs[1][k].v, logs[0][k].addr, logs[0][k].v);
        else
            snprintf(msg, sizeof msg, "%d writes, original %d", nlog[1], nlog[0]);
        mismatch(e, msg);
    } else {
        for (k = 0; k < 0x1F00 && z80_ram[k] == after[k]; k++)
            ;
        if (k < 0x1F00)
            snprintf(msg, sizeof msg, "RAM $%04X = %02X, original %02X", k, z80_ram[k], after[k]);
        else if (sound_bank_get() != bank1)
            snprintf(msg, sizeof msg, "bank %03X, original %03X", sound_bank_get(), bank1);
        else if (drv_ix != ix1)
            snprintf(msg, sizeof msg, "IX %04X, original %04X", drv_ix, ix1);
        if (msg[0])
            mismatch(e, msg);
    }
    memcpy(z80_ram, after, sizeof after);
    sound_bank_set(bank1);
}
