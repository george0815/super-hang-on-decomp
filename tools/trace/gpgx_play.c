/*
 * Play the ROM in Genesis Plus GX and record the inputs as a script, so the
 * session can become a reference recording (tools/record.sh).
 *
 * usage: gpgx_play ROM OUT.txt [-i PREFIX.txt] [-n FRAMES]
 *
 * -i plays an existing script first (keys are added on top), so a session
 * can continue from e.g. the start of a race; -n stops after FRAMES frames.
 *
 * Keys: arrows, Z = A, X = B, C = C, Enter = Start, Esc = stop and save.
 * The first gamepad also works. The script lists frame ranges per button
 * combination, in the format tests/inputs/*.txt use.
 */
#include <SDL3/SDL.h>

#include "shared.h"
#include "md_ntsc.h"
#include "sms_ntsc.h"

int debug_on = 0;
int log_error = 0;
md_ntsc_t *md_ntsc;
sms_ntsc_t *sms_ntsc;

static int frame;
static uint16 cur_pad, last_pad = 0xFFFF;
static int since;
static FILE *out;
static SDL_Gamepad *gamepad;

#define MAX_INPUT 4096
static struct { int first, last; uint16 pad; } script[MAX_INPUT];
static int nscript;

static void load_script(const char *path)
{
  char line[256];
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); exit(1); }
  while (fgets(line, sizeof line, f) && nscript < MAX_INPUT)
  {
    int a, b, k;
    char btn[64] = "";
    uint16 pad = 0;
    if (line[0] == '#' || sscanf(line, "%d %d %63s", &a, &b, btn) < 2)
      continue;
    for (k = 0; btn[k]; k++)
      switch (btn[k])
      {
        case 'U': pad |= INPUT_UP; break;
        case 'D': pad |= INPUT_DOWN; break;
        case 'L': pad |= INPUT_LEFT; break;
        case 'R': pad |= INPUT_RIGHT; break;
        case 'A': pad |= INPUT_A; break;
        case 'B': pad |= INPUT_B; break;
        case 'C': pad |= INPUT_C; break;
        case 'S': pad |= INPUT_START; break;
      }
    script[nscript].first = a;
    script[nscript].last = b;
    script[nscript].pad = pad;
    nscript++;
  }
  fclose(f);
}

static void flush(int upto)
{
  static const struct { uint16 bit; char c; } names[] = {
    { INPUT_UP, 'U' }, { INPUT_DOWN, 'D' }, { INPUT_LEFT, 'L' }, { INPUT_RIGHT, 'R' },
    { INPUT_A, 'A' }, { INPUT_B, 'B' }, { INPUT_C, 'C' }, { INPUT_START, 'S' } };
  char btn[16];
  int k, n = 0;
  if (last_pad == 0xFFFF || !last_pad)
    return;
  for (k = 0; k < 8; k++)
    if (last_pad & names[k].bit) btn[n++] = names[k].c;
  btn[n] = 0;
  fprintf(out, "%d %d %s\n", since, upto, btn);
}

int sdl_input_update(void)
{
  const bool *k = SDL_GetKeyboardState(NULL);
  uint16 p = 0;
  int i;
  for (i = 0; i < nscript; i++)
    if (frame >= script[i].first && frame <= script[i].last)
      p |= script[i].pad;
  if (k[SDL_SCANCODE_UP])     p |= INPUT_UP;
  if (k[SDL_SCANCODE_DOWN])   p |= INPUT_DOWN;
  if (k[SDL_SCANCODE_LEFT])   p |= INPUT_LEFT;
  if (k[SDL_SCANCODE_RIGHT])  p |= INPUT_RIGHT;
  if (k[SDL_SCANCODE_Z])      p |= INPUT_A;
  if (k[SDL_SCANCODE_X])      p |= INPUT_B;
  if (k[SDL_SCANCODE_C])      p |= INPUT_C;
  if (k[SDL_SCANCODE_RETURN]) p |= INPUT_START;
  if (gamepad)
  {
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))    p |= INPUT_UP;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  p |= INPUT_DOWN;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  p |= INPUT_LEFT;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) p |= INPUT_RIGHT;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST))       p |= INPUT_A;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH))      p |= INPUT_B;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST))       p |= INPUT_C;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START))      p |= INPUT_START;
  }
  /* the pad is read once per frame; record changes by frame number */
  if (p != last_pad)
  {
    flush(frame - 1);
    last_pad = p;
    since = frame;
  }
  cur_pad = p;
  input.pad[0] = p;
  return 1;
}

int main(int argc, char **argv)
{
  SDL_Window *win;
  SDL_Renderer *ren;
  SDL_Texture *tex;
  SDL_AudioStream *audio;
  SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 44100 };
  uint64_t next = 0;
  int running = 1, limit = -1, i;

  if (argc < 3)
  {
    fprintf(stderr, "usage: %s ROM OUT.txt [-i PREFIX.txt] [-n FRAMES]\n", argv[0]);
    return 1;
  }
  for (i = 3; i < argc; i++)
  {
    if (!strcmp(argv[i], "-i") && i + 1 < argc) load_script(argv[++i]);
    else if (!strcmp(argv[i], "-n") && i + 1 < argc) limit = atoi(argv[++i]);
  }
  out = fopen(argv[2], "w");
  if (!out) { perror(argv[2]); return 1; }
  fprintf(out, "# recorded with gpgx_play\n");

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
  {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_CreateWindowAndRenderer("gpgx_play (Esc to stop)", 960, 672, SDL_WINDOW_RESIZABLE, &win, &ren);
  SDL_SetRenderLogicalPresentation(ren, 320, 224, SDL_LOGICAL_PRESENTATION_LETTERBOX);
  tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 320, 224);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (audio) SDL_ResumeAudioStreamDevice(audio);

  error_init();
  set_config_defaults();
  system_bios = 0;
  memset(boot_rom, 0xff, sizeof(boot_rom));
  memset(&bitmap, 0, sizeof(t_bitmap));
  bitmap.width = 720;
  bitmap.height = 576;
  bitmap.pitch = bitmap.width * 2;
  bitmap.data = calloc(bitmap.pitch, bitmap.height);
  bitmap.viewport.changed = 3;
  if (!load_rom(argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
  audio_init(44100, 0);
  system_init();
  system_reset();

  while (running && frame != limit)
  {
    SDL_Event e;
    int16 snd[4096 * 2];
    int n, y;
    void *px;
    int pitch;
    uint64_t now;

    while (SDL_PollEvent(&e))
    {
      if (e.type == SDL_EVENT_QUIT) running = 0;
      if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = 0;
      if (e.type == SDL_EVENT_GAMEPAD_ADDED && !gamepad) gamepad = SDL_OpenGamepad(e.gdevice.which);
    }
    system_frame_gen(0);
    n = audio_update(snd);
    if (audio && SDL_GetAudioStreamQueued(audio) < 44100)
      SDL_PutAudioStreamData(audio, snd, n * 4);
    if (SDL_LockTexture(tex, NULL, &px, &pitch))
    {
      for (y = 0; y < 224; y++)
        memcpy((uint8 *)px + y * pitch,
               bitmap.data + (y + bitmap.viewport.y) * bitmap.pitch + bitmap.viewport.x * 2, 640);
      SDL_UnlockTexture(tex);
    }
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
    frame++;

    now = SDL_GetTicksNS();
    if (!next || now > next + 100000000ull) next = now;
    next += 16683350ull;
    if (next > now) SDL_DelayNS(next - now);
  }
  flush(frame - 1);
  fprintf(out, "# %d frames\n", frame);
  fclose(out);
  printf("%d frames recorded to %s\nnext: tools/record.sh NAME %d   (with the script as tests/inputs/NAME.txt)\n",
         frame, argv[2], frame);
  SDL_Quit();
  return 0;
}
