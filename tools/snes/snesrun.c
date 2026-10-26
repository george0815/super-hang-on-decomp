/*
 * Headless SNES runner for testing the SNES build: loads a libretro core
 * (Snes9x), plays an input script, and writes screenshots and RAM dumps.
 *
 * usage: snesrun CORE.so ROM FRAMES [-i SCRIPT] [-s FRAME]... [-o PREFIX]
 *                [-w WRAM.bin] [-b BWRAM.bin]
 *
 * The script has the Genesis tools' format (tests/inputs/NAME.txt): lines of
 * "FIRST LAST BUTTONS" with buttons from UDLRABCS. The Genesis pad maps to
 * the SNES pad as the port does: A = Y, B = B, C = A, S = Start.
 * -s writes PREFIX_FRAME.png after that frame (or each of FIRST-LAST);
 * -w and -b dump work RAM and
 * the cartridge RAM (SA-1 BW-RAM) after the last frame.
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "libretro.h"

#define MAX_SCRIPT 8192
static struct { int first, last; uint16_t pad; } script[MAX_SCRIPT];
static int nscript, frame;

static uint16_t video[512 * 480];
static unsigned video_w, video_h;

static void load_script(const char *path)
{
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f) && nscript < MAX_SCRIPT) {
        int first, last, n;
        uint16_t pad = 0;
        if (line[0] == '#' || sscanf(line, "%d %d %n", &first, &last, &n) < 2)
            continue;
        for (const char *p = line + n; *p && *p != '\n'; p++)
            switch (*p) {
            case 'U': pad |= 1 << RETRO_DEVICE_ID_JOYPAD_UP; break;
            case 'D': pad |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN; break;
            case 'L': pad |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT; break;
            case 'R': pad |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT; break;
            case 'A': pad |= 1 << RETRO_DEVICE_ID_JOYPAD_Y; break;
            case 'B': pad |= 1 << RETRO_DEVICE_ID_JOYPAD_B; break;
            case 'C': pad |= 1 << RETRO_DEVICE_ID_JOYPAD_A; break;
            case 'S': pad |= 1 << RETRO_DEVICE_ID_JOYPAD_START; break;
            }
        script[nscript].first = first;
        script[nscript].last = last;
        script[nscript].pad = pad;
        nscript++;
    }
    fclose(f);
}

static uint16_t pad_now(void)
{
    uint16_t pad = 0;
    for (int k = 0; k < nscript; k++)
        if (frame >= script[k].first && frame <= script[k].last)
            pad |= script[k].pad;
    return pad;
}

/* ------------------------------------------------------------ PNG */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    uint32_t crc;
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    fwrite(data, 1, len, f);
    crc = (uint32_t)crc32(0, hdr + 4, 4);
    crc = (uint32_t)crc32(crc, data, len);
    put32(hdr, crc);
    fwrite(hdr, 1, 4, f);
}

static void write_png(const char *path)
{
    static uint8_t raw[480 * (1 + 512 * 3)], packed[sizeof raw + 1024];
    uLongf packed_len = sizeof packed;
    uint8_t ihdr[13] = { 0 };
    size_t n = 0;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    for (unsigned y = 0; y < video_h; y++) {
        raw[n++] = 0;
        for (unsigned x = 0; x < video_w; x++) {
            uint16_t c = video[y * video_w + x];
            raw[n++] = (uint8_t)((c >> 11) << 3 | (c >> 13));
            raw[n++] = (uint8_t)((c >> 5 & 0x3F) << 2 | (c >> 9 & 3));
            raw[n++] = (uint8_t)((c & 0x1F) << 3 | (c >> 2 & 7));
        }
    }
    compress(packed, &packed_len, raw, n);
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    put32(ihdr, video_w);
    put32(ihdr + 4, video_h);
    ihdr[8] = 8;   /* bit depth */
    ihdr[9] = 2;   /* RGB */
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", packed, (uint32_t)packed_len);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
}

/* ------------------------------------------------------- callbacks */

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    if (level < RETRO_LOG_WARN)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = ".";
        return true;
    default:
        return false;
    }
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data || w > 512 || h > 480)
        return;
    for (unsigned y = 0; y < h; y++)
        memcpy(video + y * w, (const uint8_t *)data + y * pitch, w * 2);
    video_w = w;
    video_h = h;
}

static void audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch(const int16_t *d, size_t frames) { (void)d; return frames; }
static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)pad_now();
    return (int16_t)(pad_now() >> id & 1);
}

/* ------------------------------------------------------------ main */

#define SYM(name) do { *(void **)&name = dlsym(core, #name); \
    if (!name) { fprintf(stderr, "core lacks %s\n", #name); return 1; } } while (0)

static void dump(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f || !data) { perror(path); return; }
    fwrite(data, 1, size, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    void (*retro_init)(void), (*retro_run)(void);
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    bool (*retro_load_game)(const struct retro_game_info *);
    void *(*retro_get_memory_data)(unsigned);
    size_t (*retro_get_memory_size)(unsigned);
    static char shot[1 << 20];
    int frames;
    const char *prefix = "shot", *wram = NULL, *bwram = NULL;
    struct retro_game_info game = { 0 };
    void *core;
    FILE *f;

    if (argc < 4) {
        fprintf(stderr, "usage: snesrun CORE.so ROM FRAMES [-i SCRIPT] [-s FRAME]... [-o PREFIX] [-w WRAM] [-b BWRAM]\n");
        return 1;
    }
    frames = atoi(argv[3]);
    for (int k = 4; k + 1 < argc; k += 2) {
        if (!strcmp(argv[k], "-i")) load_script(argv[k + 1]);
        else if (!strcmp(argv[k], "-s")) {
            int first, last;
            if (sscanf(argv[k + 1], "%d-%d", &first, &last) != 2)
                last = first = atoi(argv[k + 1]);
            for (int n = first; n <= last; n++)
                if (n >= 0 && n < (int)sizeof shot) shot[n] = 1;
        }
        else if (!strcmp(argv[k], "-o")) prefix = argv[k + 1];
        else if (!strcmp(argv[k], "-w")) wram = argv[k + 1];
        else if (!strcmp(argv[k], "-b")) bwram = argv[k + 1];
        else { fprintf(stderr, "unknown option %s\n", argv[k]); return 1; }
    }

    core = dlopen(argv[1], RTLD_NOW);
    if (!core) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    SYM(retro_init); SYM(retro_run); SYM(retro_set_environment); SYM(retro_set_video_refresh);
    SYM(retro_set_audio_sample); SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll);
    SYM(retro_set_input_state); SYM(retro_load_game); SYM(retro_get_memory_data);
    SYM(retro_get_memory_size);

    f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    fseek(f, 0, SEEK_END);
    game.size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    game.data = malloc(game.size);
    if (fread((void *)game.data, 1, game.size, f) != game.size) { perror(argv[2]); return 1; }
    fclose(f);
    game.path = argv[2];

    retro_set_environment(environment);
    retro_init();
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    if (!retro_load_game(&game)) { fprintf(stderr, "the core rejected %s\n", argv[2]); return 1; }

    for (frame = 0; frame < frames; frame++) {
        retro_run();
        if (frame < (int)sizeof shot && shot[frame]) {
            char path[512];
            snprintf(path, sizeof path, "%s_%d.png", prefix, frame);
            write_png(path);
        }
    }
    if (wram)
        dump(wram, retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM), retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM));
    if (bwram)
        dump(bwram, retro_get_memory_data(RETRO_MEMORY_SAVE_RAM), retro_get_memory_size(RETRO_MEMORY_SAVE_RAM));
    return 0;
}
