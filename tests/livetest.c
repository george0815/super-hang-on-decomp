/*
 * Headless run of the native port in live mode (cycle-paced interrupts,
 * live hardware model, scripted pad input), saving screenshots.
 *
 * usage: livetest ROM FRAMES INPUT.txt OUTDIR [FRAME]...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis.h"
#include "vdp.h"

static int frame, frames, nshots, shots[64];
static const char *outdir;
static struct { int first, last; uint16_t pad; } script[1024];
static int nscript;

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

uint16_t host_pad(int port)
{
    int i;
    uint16_t p = 0;
    if (port) return 0;
    for (i = 0; i < nscript; i++)
        if (frame >= script[i].first && frame <= script[i].last)
            p |= script[i].pad;
    return p;
}

void host_frame(const uint8_t *pixels)
{
    int i;
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
    for (i = 5; i < argc && nshots < 64; i++)
        shots[nshots++] = atoi(argv[i]);
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    rom = malloc(len);
    if (fread(rom, 1, len, f) != (size_t)len) { perror(argv[1]); return 1; }
    fclose(f);
    gen_init(rom, (uint32_t)len);
    gen_run();
    return 0;
}
