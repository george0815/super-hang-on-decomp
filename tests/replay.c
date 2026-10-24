/*
 * Whole-game replay against a reference recording.
 *
 * Runs the recompiled game with interrupts injected at the exact instruction
 * counts recorded by gpgx_fncap (-e) and hardware reads served from the same
 * log, and at every VBlank compares registers, work RAM and VDP state with
 * the reference snapshot (-v). Stops at the first difference unless
 * --resync is given, in which case RAM and registers are copied from the
 * reference and the run continues.
 *
 * usage: replay ROM EVENTS VTRACE [--verify] [--resync] [--shot FRAME DIR]...
 *
 * The replay always runs the recompiled originals (exact instruction counts
 * are what place the recorded interrupts). --verify additionally runs each
 * handwritten function side by side with its original (src/runtime/verify.c).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "m68k_rt.h"
#include "genesis.h"
#include "vdp.h"
#include "sound.h"
#include "verify.h"

typedef struct { uint8_t kind; uint64_t icount; uint32_t pc; } Irq;
typedef struct { uint8_t width; uint32_t addr, value; } Read;

static Irq *irqs;
static size_t nirq, irq_pos;
static Read *reads;
static size_t nread, read_pos;
static gzFile vtrace;
static long vcount, mismatches;
static int resync;
static int lines_rendered;
static FILE *pclog, *sndlog;
int z80_trace;
static gzFile zsnap;
static long zcount;
static int z80_trace_frame = -1;

static void log_sound(int chip, int port, uint8_t v)
{
    if (chip == 0) fprintf(sndlog, "F %u %02X\n", port, v);
    else fprintf(sndlog, "P %02X\n", v);
}
static uint64_t pclog_limit;
static struct { int frame; const char *dir; } shots[64];
static int nshots;

#define VREC (12 + 72 + 32 + 4 + 128 + 128 + 8 + 0x10000)

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void load_events(const char *path)
{
    gzFile f = gzopen(path, "rb");
    size_t cap_i = 1 << 16, cap_r = 1 << 16;
    uint8_t b[13];
    if (!f) { perror(path); exit(1); }
    irqs = malloc(cap_i * sizeof *irqs);
    reads = malloc(cap_r * sizeof *reads);
    for (;;) {
        int k = gzgetc(f);
        if (k < 0) break;
        if (k == 'V' || k == 'H' || k == 'Z' || k == 'T' || k == 'K') {
            gzread(f, b, 12);
            if (nirq == cap_i) irqs = realloc(irqs, (cap_i *= 2) * sizeof *irqs);
            irqs[nirq].kind = (uint8_t)k;
            irqs[nirq].icount = (uint64_t)be32(b) << 32 | be32(b + 4);
            irqs[nirq].pc = be32(b + 8);
            nirq++;
        } else if (k == 'R') {
            gzread(f, b, 9);
            if (nread == cap_r) reads = realloc(reads, (cap_r *= 2) * sizeof *reads);
            reads[nread].width = b[0];
            reads[nread].addr = be32(b + 1);
            reads[nread].value = be32(b + 5);
            nread++;
        } else {
            fprintf(stderr, "bad event log\n");
            exit(1);
        }
    }
    gzclose(f);
    fprintf(stderr, "%zu interrupts, %zu hardware reads\n", nirq, nread);
}

static void finish(void)
{
    printf("%ld VBlanks compared, %ld mismatching\n", vcount, mismatches);
    verify_report();
    exit(mismatches != 0 || verify_failures != 0);
}

static void save_shot(int frame)
{
    int i;
    for (i = 0; i < nshots; i++) {
        char path[512];
        FILE *f;
        int p;
        if (shots[i].frame != frame) continue;
        snprintf(path, sizeof path, "%s/native_%05d.ppm", shots[i].dir, frame);
        f = fopen(path, "wb");
        if (!f) { perror(path); continue; }
        fprintf(f, "P6\n320 224\n255\n");
        for (p = 0; p < 320 * 224; p++) {
            uint32_t c = vdp_color(gen_frame[p]);
            uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
}

/* ---------------------------------------------------------------- hooks */

static void next_event(void)
{
    /* past the last recorded interrupt there is nothing left to compare:
       stop shortly after it */
    m68k_icount_event = irq_pos < nirq ? irqs[irq_pos].icount : m68k_icount + 1;
    if (pclog && m68k_icount < pclog_limit && m68k_icount + 1 < m68k_icount_event)
        m68k_icount_event = m68k_icount + 1;
}

static int irq(uint32_t pc)
{
    Irq *e;
    if (pclog && m68k_icount < pclog_limit) {
        static uint64_t last = UINT64_MAX;
        if (last != m68k_icount)
            fprintf(pclog, "%llu %06X\n", (unsigned long long)m68k_icount, pc);
        last = m68k_icount;
    }
    for (;;) {
        if (irq_pos >= nirq)
            finish();
        if (irqs[irq_pos].icount != m68k_icount) {
            next_event();
            return 0;
        }
        e = &irqs[irq_pos++];
        next_event();
        if (e->kind == 'T') {
            sound_replay_timer();
            continue;
        }
        if (e->kind == 'K') {
            sound_replay_tick();
            continue;
        }
        if (e->kind != 'Z')
            break;
        /* the Z80 took its VBlank interrupt; one frame of audio */
        if (sndlog) fprintf(sndlog, "V %u\n", e->pc);   /* pc field holds the frame */
        z80_trace = (int)e->pc == z80_trace_frame;
        if (zsnap) {
            static uint8_t ref[0x2000];
            int k;
            if (gzread(zsnap, ref, sizeof ref) == sizeof ref) {
                for (k = 0; k < 0x1f00; k++)   /* stack area excluded */
                    if (ref[k] != z80_ram[k]) {
                        printf("Z80 RAM differs at Z80 interrupt %ld (frame %u): $%04X = %02X, reference %02X\n",
                               zcount, e->pc, k, z80_ram[k], ref[k]);
                        gzclose(zsnap);
                        zsnap = NULL;
                        break;
                    }
            }
            zcount++;
        }
        sound_replay_irq();
        sound_frame(889);
        sound_buffered = 0;
    }
    if (e->pc != pc) {
        printf("interrupt #%zu at icount %llu: native pc %06X, reference %06X\n",
               irq_pos - 1, (unsigned long long)e->icount, pc, e->pc);
        mismatches++;
        finish();
    }
    if (e->kind == 'H') {
        if (lines_rendered < 224) {
            vdp.line = lines_rendered++;
            vdp_render_line(vdp.line, &gen_frame[vdp.line * 320]);
        }
        return 4;
    }
    return 6;
}

static uint32_t hw_read(int width, uint32_t addr, uint32_t live)
{
    Read *r;
    (void)live;
    if (read_pos >= nread) {
        printf("hardware read past end of log: %c%d %06X\n", 'R', width, addr);
        finish();
    }
    r = &reads[read_pos++];
    if (r->width != width || r->addr != addr) {
        printf("hardware read #%zu: native %d@%06X, reference %d@%06X (icount %llu)\n",
               read_pos - 1, width, addr, r->width, r->addr, (unsigned long long)m68k_icount);
        mismatches++;
        finish();
    }
    return r->value;
}

static void vblank(void)
{
    static uint8_t rec[VREC];
    uint32_t got[18];
    const uint8_t *regs, *vreg, *cram, *vsram, *ram;
    char diff[512] = "";
    int i, frame;
    uint32_t crc;

    /* finish the frame: any lines not split by HBlank interrupts */
    while (lines_rendered < 224) {
        vdp_render_line(lines_rendered, &gen_frame[lines_rendered * 320]);
        lines_rendered++;
    }
    lines_rendered = 0;

    if (gzread(vtrace, rec, VREC) != VREC) {
        printf("reference trace ended\n");
        finish();
    }
    frame = (int)be32(rec);
    save_shot(frame);
    regs = rec + 12;
    vreg = regs + 72;
    cram = vreg + 32 + 4;
    vsram = cram + 128;
    ram = vsram + 128 + 8;
    vcount++;

    for (i = 0; i < 8; i++) got[i] = cpu.d[i];
    for (i = 0; i < 8; i++) got[8 + i] = cpu.a[i];
    got[16] = get_sr();
    got[17] = cpu.usp;
    for (i = 0; i < 18 && !diff[0]; i++)
        if (got[i] != be32(regs + 4 * i))
            snprintf(diff, sizeof diff, "reg %d = %08X, reference %08X", i, got[i], be32(regs + 4 * i));
    if (!diff[0] && memcmp(m68k_ram, ram, 0x10000)) {
        int n = 0, len = 0;
        len = snprintf(diff, sizeof diff, "RAM:");
        for (i = 0; i < 0x10000 && n < 8; i++)
            if (m68k_ram[i] != ram[i]) {
                len += snprintf(diff + len, sizeof diff - len, " FF%04X=%02X/%02X", i, m68k_ram[i], ram[i]);
                n++;
            }
    }
    for (i = 0; i < 24 && !diff[0]; i++)
        if (vdp.reg[i] != vreg[i])
            snprintf(diff, sizeof diff, "VDP reg %d = %02X, reference %02X", i, vdp.reg[i], vreg[i]);
    for (i = 0; i < 64 && !diff[0]; i++) {
        uint16_t c = vdp.cram[i];
        uint16_t packed = (uint16_t)(((c & 0xe00) >> 3) | ((c & 0x0e0) >> 2) | ((c & 0x00e) >> 1));
        if (packed != be16(cram + 2 * i))
            snprintf(diff, sizeof diff, "CRAM[%d] = %03X, reference %03X (packed)", i, packed, be16(cram + 2 * i));
    }
    for (i = 0; i < 64 && !diff[0]; i++)
        if (vdp.vsram[i] != be16(vsram + 2 * i))
            snprintf(diff, sizeof diff, "VSRAM[%d] = %04X, reference %04X", i, vdp.vsram[i], be16(vsram + 2 * i));
    crc = (uint32_t)crc32(0, vdp.vram, 0x10000);
    if (!diff[0] && crc != be32(vreg + 32))
        snprintf(diff, sizeof diff, "VRAM crc %08X, reference %08X", crc, be32(vreg + 32));

    if (diff[0]) {
        mismatches++;
        printf("frame %d (VBlank %ld): %s\n", frame, vcount - 1, diff);
        if (!resync) finish();
        memcpy(m68k_ram, ram, 0x10000);
        for (i = 0; i < 8; i++) cpu.d[i] = be32(regs + 4 * i);
        for (i = 0; i < 8; i++) cpu.a[i] = be32(regs + 32 + 4 * i);
        set_sr(be32(regs + 64));
        cpu.usp = be32(regs + 68);
    }
}

static void on_interrupt(int sig)
{
    (void)sig;
    fprintf(stderr, "interrupted: icount %llu, next irq %zu/%zu at %llu, read %zu/%zu, VBlank %ld\n",
            (unsigned long long)m68k_icount, irq_pos, nirq,
            (unsigned long long)m68k_icount_event, read_pos, nread, vcount);
    _exit(3);
}

static const GenReplay replay_hooks = { irq, hw_read, vblank };

/* live-mode host callbacks are unused during replay */
void host_frame(const uint8_t *pixels) { (void)pixels; }
uint16_t host_pad(int port) { (void)port; return 0; }

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
    long romlen;
    uint8_t *rom;
    int i;

    if (argc < 4) {
        fprintf(stderr, "usage: %s ROM EVENTS VTRACE [--resync] [--shot FRAME DIR]...\n", argv[0]);
        return 1;
    }
    for (i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--resync")) resync = 1;
        else if (!strcmp(argv[i], "--verify")) verify_enabled = 1;
        else if (!strcmp(argv[i], "--zram") && i + 1 < argc)
            zsnap = gzopen(argv[++i], "rb");
        else if (!strcmp(argv[i], "--z80trace") && i + 1 < argc)  /* debug */
            z80_trace_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sndlog") && i + 1 < argc) {
            sndlog = fopen(argv[++i], "w");
            sound_write_hook = log_sound;
        }
        else if (!strcmp(argv[i], "--pclog") && i + 2 < argc) {  /* debug: log first N pcs */
            pclog = fopen(argv[++i], "w");
            pclog_limit = strtoull(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc && nshots < 64) {
            shots[nshots].frame = atoi(argv[++i]);
            shots[nshots].dir = argv[++i];
            nshots++;
        }
    }
    rom = read_file(argv[1], &romlen);
    load_events(argv[2]);
    vtrace = gzopen(argv[3], "rb");
    if (!vtrace) { perror(argv[3]); return 1; }

    signal(SIGINT, on_interrupt);
    gen_init(rom, (uint32_t)romlen);
    gen_replay = &replay_hooks;
    sound_external_ticks = 1;
    m68k_cycles_event = INT64_MAX;
    next_event();
    if (pclog) m68k_icount_event = 0;
    gen_run();
    return 0;
}
