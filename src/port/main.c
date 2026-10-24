/*
 * Native Super Hang-On: SDL3 frontend.
 *
 * The game code runs recompiled; this file only provides a window, input,
 * audio output and frame pacing.
 *
 * usage: shangon [ROM]   (default: sho.md)
 *
 * Keys: arrows = steer / lean, Z = A (brake), X = B (accelerate), C = C,
 *       Enter = Start, Esc = quit. The first connected gamepad also works.
 */
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "genesis.h"
#include "vdp.h"
#include "sound.h"

#define FRAME_NS 16683350ull  /* NTSC: 59.94 Hz */

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_Gamepad *gamepad;
static SDL_AudioStream *audio;
static uint64_t next_frame;

static void poll_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            exit(0);
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE)
                exit(0);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!gamepad)
                gamepad = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad && SDL_GetGamepadID(gamepad) == e.gdevice.which) {
                SDL_CloseGamepad(gamepad);
                gamepad = NULL;
            }
            break;
        }
    }
}

void host_frame(const uint8_t *pixels)
{
    uint32_t palette[64];
    void *dst;
    int pitch, i, y;
    uint64_t now;

    poll_events();
    if (audio) {
        /* keep latency bounded if video and audio clocks drift apart */
        if (SDL_GetAudioStreamQueued(audio) < YM_RATE / 4 * 4)
            SDL_PutAudioStreamData(audio, sound_buffer, sound_buffered * 4);
    }
    sound_buffered = 0;
    for (i = 0; i < 64; i++)
        palette[i] = vdp_color(i);
    if (SDL_LockTexture(texture, NULL, &dst, &pitch)) {
        for (y = 0; y < VDP_LINES; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)dst + y * pitch);
            for (i = 0; i < VDP_WIDTH; i++)
                row[i] = palette[pixels[y * VDP_WIDTH + i]];
        }
        SDL_UnlockTexture(texture);
    }
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    /* pace to the console's frame rate */
    now = SDL_GetTicksNS();
    if (next_frame == 0 || now > next_frame + 4 * FRAME_NS)
        next_frame = now;
    next_frame += FRAME_NS;
    if (next_frame > now)
        SDL_DelayNS(next_frame - now);
}

uint16_t host_pad(int port)
{
    const bool *k;
    uint16_t p = 0;
    if (port != 0)
        return 0;
    k = SDL_GetKeyboardState(NULL);
    if (k[SDL_SCANCODE_UP])     p |= PAD_UP;
    if (k[SDL_SCANCODE_DOWN])   p |= PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT])   p |= PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT])  p |= PAD_RIGHT;
    if (k[SDL_SCANCODE_Z])      p |= PAD_A;
    if (k[SDL_SCANCODE_X])      p |= PAD_B;
    if (k[SDL_SCANCODE_C])      p |= PAD_C;
    if (k[SDL_SCANCODE_RETURN]) p |= PAD_START;
    if (gamepad) {
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))    p |= PAD_UP;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  p |= PAD_DOWN;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  p |= PAD_LEFT;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) p |= PAD_RIGHT;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST))       p |= PAD_A;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH))      p |= PAD_B;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST))       p |= PAD_C;
        if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START))      p |= PAD_START;
        {
            int16_t x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
            int16_t y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
            if (x < -16000) p |= PAD_LEFT;
            if (x > 16000)  p |= PAD_RIGHT;
            if (y < -16000) p |= PAD_UP;
            if (y > 16000)  p |= PAD_DOWN;
        }
    }
    return p;
}

static uint8_t *load_rom(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *buf;
    if (!f) {
        fprintf(stderr, "cannot open ROM %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len);
    if (!buf || fread(buf, 1, len, f) != (size_t)len) {
        fprintf(stderr, "cannot read ROM %s\n", path);
        exit(1);
    }
    fclose(f);
    *size = (uint32_t)len;
    return buf;
}

int main(int argc, char **argv)
{
    uint32_t size;
    uint8_t *rom = load_rom(argc > 1 ? argv[1] : "sho.md", &size);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!SDL_CreateWindowAndRenderer("Super Hang-On", VDP_WIDTH * 3, VDP_LINES * 3,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 0);
    SDL_SetRenderLogicalPresentation(renderer, VDP_WIDTH, VDP_LINES,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888,
                                SDL_TEXTUREACCESS_STREAMING, VDP_WIDTH, VDP_LINES);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    {
        SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, YM_RATE };
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (audio)
            SDL_ResumeAudioStreamDevice(audio);
        else
            fprintf(stderr, "no audio: %s\n", SDL_GetError());
    }
    atexit(SDL_Quit);

    gen_init(rom, size);
    gen_run();
    return 0;
}
