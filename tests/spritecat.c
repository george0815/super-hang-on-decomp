/*
 * Sprite frame catalogue: plays input scripts through the native port and
 * lists every distinct sprite frame the objects show, for the SNES port's
 * sprite conversion. A frame is an object's sprite map with its tile base
 * and the VRAM contents of the tiles it uses at the time.
 *
 * usage: spritecat ROM FRAMES INPUT.txt [FRAMES INPUT.txt]...
 * Prints one line per frame: map address, tile base, piece count, cells,
 * width x height, and a hash of the tiles' contents.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "m68k_rt.h"
#include "genesis.h"
#include "vdp.h"

#define OBJECTS   0xFFC900
#define OBJ_SIZE  0x40
#define OBJ_COUNT 29

static int frame, frames;
static jmp_buf done;
static struct { int first, last; uint16_t pad; } script[4096];
static int nscript;

static void load_script(const char *path)
{
    char line[256], btn[64];
    FILE *f = fopen(path, "r");
    nscript = 0;
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f) && nscript < 4096) {
        int a, b;
        uint16_t p = 0;
        if (line[0] == '#' || sscanf(line, "%d %d %63s", &a, &b, btn) != 3)
            continue;
        for (char *c = btn; *c; c++)
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
    uint16_t p = 0;
    if (port) return 0;
    for (int i = 0; i < nscript; i++)
        if (frame >= script[i].first && frame <= script[i].last)
            p |= script[i].pad;
    return p;
}

/* ------------------------------------------------------------ catalogue */

typedef struct {
    uint32_t map;
    uint16_t base;
    uint64_t hash;
    int pieces, cells, w, h;
    long seen;
} Entry;

static Entry *cat;
static int ncat, capcat;

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    while (n--) { h ^= *p++; h *= 0x100000001B3ull; }
    return h;
}

static void note(uint32_t map, uint16_t base)
{
    uint16_t pieces = rd16(map);
    uint64_t h = 0xCBF29CE484222325ull;
    int cells = 0, x0 = 9999, y0 = 9999, x1 = -9999, y1 = -9999;
    uint32_t m = map + 2;
    for (int p = 0; p <= pieces; p++, m += 6) {
        int y = rd8(m) - 256, size = rd8(m + 1);    /* from -256 */
        uint16_t tile = (uint16_t)(rd16(m + 2) + base);
        int x = (int16_t)rd16(m + 4);
        int w = (size >> 2 & 3) + 1, hh = (size & 3) + 1;
        uint8_t key[6] = { (uint8_t)y, (uint8_t)size, (uint8_t)(tile >> 8), (uint8_t)tile,
                           (uint8_t)(x >> 8), (uint8_t)x };
        h = fnv(h, key, sizeof key);
        for (int c = 0; c < w * hh; c++)
            h = fnv(h, &vdp.vram[(((tile & 0x7FF) + c) & 0x7FF) * 32], 32);
        cells += w * hh;
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + 8 * w > x1) x1 = x + 8 * w;
        if (y + 8 * hh > y1) y1 = y + 8 * hh;
    }
    for (int i = 0; i < ncat; i++)
        if (cat[i].map == map && cat[i].base == base && cat[i].hash == h) {
            cat[i].seen++;
            return;
        }
    if (ncat == capcat) {
        capcat = capcat ? 2 * capcat : 256;
        cat = realloc(cat, capcat * sizeof *cat);
    }
    cat[ncat++] = (Entry){ map, base, h, pieces + 1, cells, x1 - x0, y1 - y0, 1 };
}

void host_frame(const uint8_t *pixels)
{
    (void)pixels;
    for (int k = 0; k < OBJ_COUNT; k++) {
        uint32_t obj = OBJECTS + k * OBJ_SIZE;
        uint16_t map = rd16(obj + 0x24);
        if (rd16(obj) && map)
            note(map & 0x8000 ? 0xFF0000u | map : map, rd16(obj + 2));
    }
    if (++frame >= frames)
        longjmp(done, 1);
}

int main(int argc, char **argv)
{
    FILE *f;
    long len;
    uint8_t *rom;
    long total_cells = 0;

    if (argc < 4 || argc % 2) {
        fprintf(stderr, "usage: %s ROM FRAMES INPUT.txt [FRAMES INPUT.txt]...\n", argv[0]);
        return 1;
    }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    rom = malloc(len);
    if (fread(rom, 1, len, f) != (size_t)len) { perror(argv[1]); return 1; }
    fclose(f);

    for (int a = 2; a + 1 < argc; a += 2) {
        frames = atoi(argv[a]);
        load_script(argv[a + 1]);
        frame = 0;
        gen_init(rom, (uint32_t)len);
        if (!setjmp(done))
            gen_run();
    }
    for (int i = 0; i < ncat; i++) {
        printf("%06X %04X %2d %3d %3dx%-3d %016llX %ld\n", cat[i].map, cat[i].base, cat[i].pieces,
               cat[i].cells, cat[i].w, cat[i].h, (unsigned long long)cat[i].hash, cat[i].seen);
        total_cells += cat[i].cells;
    }
    fprintf(stderr, "%d frames, %ld cells\n", ncat, total_cells);
    return 0;
}
