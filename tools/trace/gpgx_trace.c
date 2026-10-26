/*
 * Headless reference tracer built on the Genesis Plus GX core.
 *
 * Runs a ROM for N frames with scripted pad input and records, at the end
 * of every frame, the 68000 registers and the 64 KB work RAM. The trace is
 * the ground truth the native port is compared against.
 *
 * usage: gpgx_trace ROM FRAMES OUT.trace [-i input.txt] [-s FRAME]... [-d DIR]
 *
 * input.txt lines:  FIRST LAST BUTTONS   (frames inclusive; buttons from
 *                   "UDLRABCS", e.g. "200 210 S" holds Start)
 * -w FILE.wav       also record the audio output
 * -a FRAME OUT.txt  autopilot from FRAME: hold B and steer towards the road
 *                   (env AP_TH, AP_AVOID, AP_TURBO, AP_SLOW tune it)
 *                   centre; the inputs used are written to OUT.txt as an
 *                   input script (so gpgx_fncap can replay the session)
 * -s FRAME          also write DIR/frame_NNNNN.ppm and DIR/frame_NNNNN.vdp
 *                   (VRAM, CRAM, VSRAM, registers) for that frame
 *
 * Trace file: "SHOTRACE" u32 version, u32 frames, then per frame
 * (all big-endian): u32 frame, u32 d0-d7, u32 a0-a7, u32 pc, u32 sr,
 * 65536 bytes work RAM ($FF0000-$FFFFFF).
 */

#include "shared.h"
#include "md_ntsc.h"
#include "sms_ntsc.h"

/* symbols the core expects from the frontend */
int debug_on = 0;
int log_error = 0;
md_ntsc_t *md_ntsc;
sms_ntsc_t *sms_ntsc;

#define MAX_INPUT 4096
static struct { int first, last; uint16 pad; } script[MAX_INPUT];
static int nscript;
static int cur_frame;
static int autopilot_from = -1, autopilot_dir = 1;
static FILE *autopilot_out;
static uint16 autopilot_last = 0xFFFF;
static int autopilot_since;

static void autopilot_flush(int frame)
{
  static const char names[] = "UDLRBCAS";
  char btn[16];
  int k, n = 0;
  if (autopilot_last == 0xFFFF || !autopilot_out) return;
  for (k = 0; k < 8; k++)
    if (autopilot_last & (1 << k)) btn[n++] = names[k];
  btn[n] = 0;
  if (n) fprintf(autopilot_out, "%d %d %s\n", autopilot_since, frame - 1, btn);
}

int sdl_input_update(void)
{
  int i;
  uint16 pad = 0;
  for (i = 0; i < nscript; i++)
    if (cur_frame >= script[i].first && cur_frame <= script[i].last)
      pad |= script[i].pad;
  if (autopilot_from >= 0 && cur_frame >= autopilot_from)
  {
    /* road curve: positive = off to one side */
    int16 curve = (int16)(READ_BYTE(work_ram, 0x0614) << 8 | READ_BYTE(work_ram, 0x0615));
    int th = getenv("AP_TH") ? atoi(getenv("AP_TH")) : 24;
    int avoid = getenv("AP_AVOID") ? atoi(getenv("AP_AVOID")) : 0, r;
    pad = INPUT_B | (getenv("AP_TURBO") ? INPUT_C : 0);
    /* steer around rivals close ahead (records at $FF0804: obj, lane, depth) */
    for (r = 0; avoid && r < 5; r++)
    {
      int a = 0x0804 + 8 * r;
      int16 lane = (int16)(READ_BYTE(work_ram, a + 4) << 8 | READ_BYTE(work_ram, a + 5));
      int16 depth = (int16)(READ_BYTE(work_ram, a + 6) << 8 | READ_BYTE(work_ram, a + 7));
      if (depth > 0x80 && depth < 0xC0 && lane > -0x50 && lane < 0x50)
      {
        curve = (int16)(curve - (lane >= 0 ? -avoid : avoid));
        break;
      }
    }
    /* AP_SLOW: off the throttle when this far to one side */
    if (getenv("AP_SLOW") && (curve > atoi(getenv("AP_SLOW")) || curve < -atoi(getenv("AP_SLOW"))))
      pad &= ~INPUT_B;
    if (curve > th) pad |= autopilot_dir > 0 ? INPUT_LEFT : INPUT_RIGHT;
    if (curve < -th) pad |= autopilot_dir > 0 ? INPUT_RIGHT : INPUT_LEFT;
  }
  if (autopilot_out && pad != autopilot_last)
  {
    autopilot_flush(cur_frame);
    autopilot_last = pad;
    autopilot_since = cur_frame;
  }
  input.pad[0] = pad;
  return 1;
}

static void load_script(const char *path)
{
  char line[256], btn[64];
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); exit(1); }
  while (fgets(line, sizeof line, f) && nscript < MAX_INPUT)
  {
    int a, b;
    char *c;
    uint16 pad = 0;
    if (line[0] == '#' || sscanf(line, "%d %d %63s", &a, &b, btn) != 3)
      continue;
    for (c = btn; *c; c++)
      switch (*c)
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

static void put32(FILE *f, uint32 v)
{
  uint8 b[4] = { v >> 24, v >> 16, v >> 8, v };
  fwrite(b, 1, 4, f);
}

static void write_shot(const char *dir, int frame)
{
  char path[512];
  int x, y;
  FILE *f;
  uint8 buf[0x10000];

  snprintf(path, sizeof path, "%s/frame_%05d.ppm", dir, frame);
  f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  fprintf(f, "P6\n%d %d\n255\n", bitmap.viewport.w, bitmap.viewport.h);
  for (y = 0; y < bitmap.viewport.h; y++)
  {
    uint16 *row = (uint16 *)(bitmap.data + (y + bitmap.viewport.y) * bitmap.pitch)
                  + bitmap.viewport.x;
    for (x = 0; x < bitmap.viewport.w; x++)
    {
      uint16 p = row[x];
      uint8 rgb[3] = { (p >> 11) << 3, ((p >> 5) & 0x3f) << 2, (p & 0x1f) << 3 };
      fwrite(rgb, 1, 3, f);
    }
  }
  fclose(f);

  /* VDP memories, byte-swapped back to big-endian where the core keeps
     them in host order */
  snprintf(path, sizeof path, "%s/frame_%05d.vdp", dir, frame);
  f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  for (x = 0; x < 0x10000; x++) buf[x] = READ_BYTE(vram, x);
  fwrite(buf, 1, 0x10000, f);
  for (x = 0; x < 0x80; x++) buf[x] = READ_BYTE(cram, x);
  fwrite(buf, 1, 0x80, f);
  for (x = 0; x < 0x80; x++) buf[x] = READ_BYTE(vsram, x);
  fwrite(buf, 1, 0x80, f);
  fwrite(reg, 1, 0x20, f);
  fclose(f);
}

int main(int argc, char **argv)
{
  static uint8 ram[0x10000];
  static int shots[256];
  int nshots = 0, frames, i, r;
  const char *dir = ".";
  FILE *out, *wav = NULL;
  long wav_frames = 0;

  if (argc < 4)
  {
    fprintf(stderr, "usage: %s ROM FRAMES OUT.trace [-i input.txt] [-s FRAME]... [-d DIR]\n", argv[0]);
    return 1;
  }
  frames = atoi(argv[2]);
  for (i = 4; i < argc; i++)
  {
    if (!strcmp(argv[i], "-i") && i + 1 < argc) load_script(argv[++i]);
    else if (!strcmp(argv[i], "-s") && i + 1 < argc && nshots < 256) shots[nshots++] = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
    else if (!strcmp(argv[i], "-a") && i + 2 < argc)
    {
      autopilot_from = atoi(argv[++i]);
      autopilot_out = fopen(argv[++i], "w");
    }
    else if (!strcmp(argv[i], "-r")) autopilot_dir = -1;
    else if (!strcmp(argv[i], "-w") && i + 1 < argc) { wav = fopen(argv[++i], "wb"); if (wav) fseek(wav, 44, SEEK_SET); }
  }

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

  if (!load_rom(argv[1]))
  {
    fprintf(stderr, "cannot load %s\n", argv[1]);
    return 1;
  }
  audio_init(44100, 0);
  system_init();
  system_reset();

  out = fopen(argv[3], "wb");
  if (!out) { perror(argv[3]); return 1; }
  fwrite("SHOTRACE", 1, 8, out);
  put32(out, 1);
  put32(out, frames);

  for (cur_frame = 0; cur_frame < frames; cur_frame++)
  {
    int16 snd[4096 * 2];
    int n;
    system_frame_gen(0);
    n = audio_update(snd);
    if (wav) { fwrite(snd, 4, n, wav); wav_frames += n; }

    put32(out, cur_frame);
    for (r = M68K_REG_D0; r <= M68K_REG_A7; r++)
      put32(out, m68k_get_reg(r));
    put32(out, m68k_get_reg(M68K_REG_PC));
    put32(out, m68k_get_reg(M68K_REG_SR));
    for (i = 0; i < 0x10000; i++) ram[i] = READ_BYTE(work_ram, i);
    fwrite(ram, 1, sizeof ram, out);

    for (i = 0; i < nshots; i++)
      if (shots[i] == cur_frame)
        write_shot(dir, cur_frame);
  }
  fclose(out);
  if (autopilot_out)
  {
    autopilot_flush(cur_frame);
    fclose(autopilot_out);
  }
  if (wav)
  {
    uint32 data = wav_frames * 4, riff = data + 36, rate = 44100, bps = rate * 4;
    uint8 h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
                    16,0,0,0, 1,0, 2,0, 0,0,0,0, 0,0,0,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &bps, 4); memcpy(h + 40, &data, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fclose(wav);
  }
  return 0;
}
