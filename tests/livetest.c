/*
 * Headless run of the native port in live mode (cycle-paced interrupts,
 * live hardware model, scripted pad input), saving screenshots.
 *
 * usage: livetest ROM FRAMES INPUT.txt OUTDIR [FRAME]...
 * SHO_MIX=master,music,engine,effects (percent) overrides the sound mix.
 * SHO_PADLOG=FILE lists the pad as the game reads it, one line per read:
 * the game frame (the main loop's count, $FFC726, carried past 255) and
 * the pad (hex, S A C B R L D U), for driving the SNES build (snesrun -P).
 * Audio is written to OUTDIR/live.wav, the timed YM2612 register writes to
 * OUTDIR/live.ymlog.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k_rt.h"
#include "genesis.h"
#include "vdp.h"
#include "sound.h"

static int frame, frames, nshots, shots[4096];
static const char *outdir;

static struct { int first, last; uint16_t pad; } script[1024];
static int nscript;
static FILE *wav, *ymlog;

static void log_ym(int chip, int port, uint8_t v)
{
    if (chip == 0)
        fprintf(ymlog, "%llu %d %02X\n", (unsigned long long)ym_samples, port, v);
}
static long wav_frames;

static void load_script(const char *path)
{
    char line[256], btn[64];
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f) && nscript < 1024) {
        int a, b;
        uint16_t p = 0;
        char *c;
        if (line[0] == '#' || sscanf(line, "%d %d %63s", &a, &b, btn) != 3)
            continue;
        for (c = btn; *c; c++)
            switch (*c) {
            case 'U': p |= PAD_UP; break;
            case 'D': p |= PAD_DOWN; break;
            case 'L': p |= PAD_LEFT; break;
            case 'R': p |= PAD_RIGHT; break;
            case 'A': p |= PAD_A; break;
            case 'B': p |= PAD_B; break;
            case 'C': p |= PAD_C; break;
            case 'S': p |= PAD_START; break;
            }
        script[nscript].first = a;
        script[nscript].last = b;
        script[nscript].pad = p;
        nscript++;
    }
    fclose(f);
}

static FILE *padlog;

uint16_t host_pad(int port)
{
    static int calls;
    int i;
    uint16_t p = 0;
    if (port) return 0;
    for (i = 0; i < nscript; i++)
        if (frame >= script[i].first && frame <= script[i].last)
            p |= script[i].pad;
    if (padlog && calls++ & 1) {    /* (each read takes the port twice) */
        static unsigned frames, last;
        unsigned now = m68k_ram[0xC726];
        frames += (now - last) & 0xFF;
        last = now;
        fprintf(padlog, "%u %02X\n", frames, p);
    }
    return p;
}

static void wav_header(FILE *f, long frames)
{
    uint32_t data = (uint32_t)frames * 4, rate = YM_RATE;
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
                      16,0,0,0, 1,0, 2,0, 0,0,0,0, 0,0,0,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    uint32_t riff = data + 36, bps = rate * 4;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &bps, 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void close_wav(void)
{
    if (!wav) return;
    wav_header(wav, wav_frames);
    fclose(wav);
}

void host_frame(const uint8_t *pixels)
{
    int i;
    if (wav) {
        fwrite(sound_buffer, 4, sound_buffered, wav);
        wav_frames += sound_buffered;
    }
    sound_buffered = 0;
    for (i = 0; i < nshots; i++) {
        char path[512];
        FILE *f;
        int k;
        if (shots[i] != frame) continue;
        snprintf(path, sizeof path, "%s/live_%05d.ppm", outdir, frame);
        f = fopen(path, "wb");
        if (!f) { perror(path); continue; }
        fprintf(f, "P6\n%d %d\n255\n", VDP_WIDTH, VDP_LINES);
        for (k = 0; k < VDP_WIDTH * VDP_LINES; k++) {
            uint32_t c = vdp_color(pixels[k]);
            uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        /* the VDP's memories and registers, for the SNES screen converter */
        snprintf(path, sizeof path, "%s/live_%05d.vdp", outdir, frame);
        f = fopen(path, "wb");
        if (!f) { perror(path); continue; }
        fwrite(vdp.vram, 1, sizeof vdp.vram, f);
        fwrite(vdp.cram, 2, 64, f);
        fwrite(vdp.vsram, 2, 64, f);
        fwrite(vdp.reg, 1, sizeof vdp.reg, f);
        fwrite(vdp.line_vsram, 2, VDP_LINES * 2, f);
        fclose(f);
        /* and the work RAM */
        snprintf(path, sizeof path, "%s/live_%05d.ram", outdir, frame);
        f = fopen(path, "wb");
        if (!f) { perror(path); continue; }
        fwrite(m68k_ram, 1, sizeof m68k_ram, f);
        fclose(f);
    }
    if (++frame >= frames)
        exit(0);
}

int main(int argc, char **argv)
{
    FILE *f;
    long len;
    uint8_t *rom;
    int i;

    if (argc < 5) {
        fprintf(stderr, "usage: %s ROM FRAMES INPUT.txt OUTDIR [FRAME]...\n", argv[0]);
        return 1;
    }
    frames = atoi(argv[2]);
    load_script(argv[3]);
    outdir = argv[4];
    for (i = 5; i < argc && nshots < 4096; i++)
        shots[nshots++] = atoi(argv[i]);
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    rom = malloc(len);
    if (fread(rom, 1, len, f) != (size_t)len) { perror(argv[1]); return 1; }
    fclose(f);
    {
        char path[512];
        snprintf(path, sizeof path, "%s/live.wav", outdir);
        wav = fopen(path, "wb");
        if (wav) { wav_header(wav, 0); atexit(close_wav); }
        snprintf(path, sizeof path, "%s/live.ymlog", outdir);
        ymlog = fopen(path, "w");
        if (ymlog) sound_write_hook = log_ym;
    }
    if (getenv("SHO_PADLOG"))
        padlog = fopen(getenv("SHO_PADLOG"), "w");
    if (getenv("SHO_MIX"))
        sscanf(getenv("SHO_MIX"), "%d,%d,%d,%d", &sound_settings.master,
               &sound_settings.group[SND_MUSIC], &sound_settings.group[SND_ENGINE],
               &sound_settings.group[SND_EFFECTS]);
    gen_init(rom, (uint32_t)len);
    gen_run();
    return 0;
}
