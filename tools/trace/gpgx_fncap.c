/*
 * Function-level reference capture, built on Genesis Plus GX with HOOK_CPU.
 *
 * Plays the ROM with scripted input and, for every call to a known function
 * entry, snapshots registers + work RAM at entry and at return, along with
 * every non-RAM/ROM bus access in between (VDP, Z80, I/O). Calls during
 * which an interrupt was taken are discarded, so each sample is a pure
 * function of its entry state and its logged hardware reads.
 *
 * usage: gpgx_fncap ROM FRAMES entries.txt OUT.samples [-i input.txt] [-k PER_FUNC]
 *                   [-v OUT.vtrace] [-e OUT.events] [-s OUT.sndlog]
 *
 * -v also writes a VBlank trace (gzip): one record each time the VBlank
 * handler ($6D2A) starts, i.e. after the exception frame is pushed:
 *   u32 frame, u32 pad, u32 stacked_pc, u32 regs[18], u8 vdpreg[32],
 *   u32 crc_vram, u16 cram[64] (9-bit packed BBBGGGRRR, as the core keeps it),
 *   u16 vsram[64], u64 icount, u8 ram[65536]
 *
 * -s writes a sound log (text), one line per write:
 *   F frame z80cycles port data    YM2612 (port 0-3)
 *   P frame z80cycles data         PSG
 *   M frame 68kcycles addr data    68000 write into Z80 space (mailbox)
 *
 * -e writes an event log (gzip) for exact replay: every interrupt and every
 * hardware read, in order. icount = instructions executed before the event.
 *   'V' u64 icount u32 stacked_pc     VBlank handler entered
 *   'H' u64 icount u32 stacked_pc     HBlank handler entered
 *   'R' u8 width u32 addr u32 value   hardware read
 *   'Z' u64 icount u32 frame          Z80 accepted its VBlank interrupt
 * -z writes Z80 RAM (8 KB, gzip) each time the Z80 takes an interrupt.
 *
 *   'T' u64 icount u32 0              sound driver timer tick (Z80 rewrote
 *                                     YM register $27 with $6F)
 *   'K' u64 icount u32 frame          Z80 entered the driver tick ($0091)
 *
 * Sample file: "SHOFNCAP" u32 version, then samples until EOF. Each sample
 * (big-endian): u32 entry, u32 ret, u32 frame, u32 nio,
 *   u32 regs_in[18], u8 ram_in[65536], u32 regs_out[18], u8 ram_out[65536],
 *   nio x { u8 kind ('R'/'W'), u8 width (1/2), u32 addr, u32 value }
 * regs order: d0-d7, a0-a7, sr, usp.
 */

#include "shared.h"
#include "md_ntsc.h"
#include "sms_ntsc.h"
#include "cpuhook.h"
#include <zlib.h>

int debug_on = 0;
int log_error = 0;
md_ntsc_t *md_ntsc;
sms_ntsc_t *sms_ntsc;

#define MAX_INPUT 4096
static struct { int first, last; uint16 pad; } script[MAX_INPUT];
static int nscript;
static int cur_frame;

int sdl_input_update(void)
{
  int i;
  uint16 pad = 0;
  for (i = 0; i < nscript; i++)
    if (cur_frame >= script[i].first && cur_frame <= script[i].last)
      pad |= script[i].pad;
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

/* ------------------------------------------------------------ capture */

#define MAX_IO     8192
#define MAX_DEPTH  64
#define NUM_REGS   18

typedef struct { uint8 kind, width; uint32 addr, value; } io_t;

typedef struct {
  uint32 entry, ret, sp, frame;
  int keep;          /* sampling this call */
  int tainted;       /* interrupt or overflow during the call */
  uint32 regs[NUM_REGS];
  uint8 *ram;
  io_t *io;
  int nio;
} call_t;

static uint8 is_entry[0x10000];   /* function entries live below $10000 */
static int per_func = 3;
static uint16 taken[0x10000];
static call_t stack[MAX_DEPTH];
static int depth;
static uint32 prev_pc;
static FILE *out;
static long nsamples, ndiscard;
static gzFile vtrace;
static FILE *pclog, *sndlog;
static gzFile events;
static uint64_t icount;
static void (*real_fm_write)(unsigned int cycles, unsigned int address, unsigned int data);

static void ev64(uint64_t v);
static void ev32(uint32 v);

/* called by the patched Z80 core when it takes an interrupt */
void (*z80_irq_hook)(void);
static gzFile zsnap;
static void on_z80_irq(void)
{
  if (events)
  {
    gzputc(events, 'Z');
    ev64(icount);
    ev32(cur_frame);
  }
  if (zsnap)  /* Z80 RAM as the interrupt is taken */
    gzwrite(zsnap, zram, 0x2000);
}

void (*z80_pc_hook)(unsigned pc);
static void on_z80_pc(unsigned pc)
{
  if (pc == 0x0091)   /* sound driver tick routine */
  {
    gzputc(events, 'K');
    ev64(icount);
    ev32(cur_frame);
  }
}

static void log_fm_write(unsigned int cycles, unsigned int address, unsigned int data)
{
  static unsigned latch;
  if (sndlog)
    fprintf(sndlog, "F %d %u %u %02X\n", cur_frame, cycles, address & 3, data & 0xff);
  if ((address & 3) == 0)
    latch = data & 0xff;
  else if ((address & 3) == 1 && latch == 0x27 && (data & 0xff) == 0x6f && events)
  {
    gzputc(events, 'T');
    ev64(icount);
    ev32(0);
  }
  real_fm_write(cycles, address, data);
}

void __real_psg_write(unsigned int cycles, unsigned int data);
void __wrap_psg_write(unsigned int cycles, unsigned int data)
{
  if (sndlog)
    fprintf(sndlog, "P %d %u %02X\n", cur_frame, cycles, data & 0xff);
  __real_psg_write(cycles, data);
}
static uint64_t pclog_limit;

static void gzput(gzFile f, const void *p, int n) { gzwrite(f, p, n); }

static void ev64(uint64_t v)
{
  uint8 b[8] = { v >> 56, v >> 48, v >> 40, v >> 32, v >> 24, v >> 16, v >> 8, v };
  gzput(events, b, 8);
}

static void ev32(uint32 v)
{
  uint8 b[4] = { v >> 24, v >> 16, v >> 8, v };
  gzput(events, b, 4);
}

static void get_regs(uint32 *r);
static void get_ram(uint8 *dst);
static uint32 ram32(uint32 a);

static void gz32(uint32 v)
{
  uint8 b[4] = { v >> 24, v >> 16, v >> 8, v };
  gzwrite(vtrace, b, 4);
}

static uint32 crc_mem(const uint8 *p, int len)
{
  /* memories are kept word-swapped on little-endian hosts; checksum the
     big-endian view so the native port can compute the same value */
  static uint8 tmp[0x10000];
  int i;
  for (i = 0; i < len; i++) tmp[i] = READ_BYTE(p, i);
  return crc32(0, tmp, len);
}

static void vblank_record(void)
{
  static uint8 ram[0x10000];
  uint32 regs[NUM_REGS];
  int i;
  get_regs(regs);
  get_ram(ram);
  gz32(cur_frame);
  gz32(input.pad[0]);
  gz32(ram32(regs[15] + 2));
  for (i = 0; i < NUM_REGS; i++) gz32(regs[i]);
  gzwrite(vtrace, reg, 32);
  gz32(crc_mem(vram, 0x10000));
  for (i = 0; i < 64; i++)
  {
    uint16 c = ((uint16 *)cram)[i];
    uint8 b[2] = { c >> 8, c };
    gzwrite(vtrace, b, 2);
  }
  for (i = 0; i < 64; i++)
  {
    uint16 v = ((uint16 *)vsram)[i];
    uint8 b[2] = { v >> 8, v };
    gzwrite(vtrace, b, 2);
  }
  gz32(icount >> 32);
  gz32((uint32)icount);
  gzwrite(vtrace, ram, 0x10000);
}

static void get_regs(uint32 *r)
{
  int i;
  for (i = 0; i < 16; i++) r[i] = m68k_get_reg(M68K_REG_D0 + i);
  r[16] = m68k_get_reg(M68K_REG_SR);
  r[17] = m68k_get_reg(M68K_REG_USP);
}

static void get_ram(uint8 *dst)
{
  int i;
  for (i = 0; i < 0x10000; i++) dst[i] = READ_BYTE(work_ram, i);
}

static uint32 ram32(uint32 a)
{
  a &= 0xffff;
  return READ_BYTE(work_ram, a) << 24 | READ_BYTE(work_ram, (a + 1) & 0xffff) << 16 |
         READ_BYTE(work_ram, (a + 2) & 0xffff) << 8 | READ_BYTE(work_ram, (a + 3) & 0xffff);
}

static void put32(uint32 v)
{
  uint8 b[4] = { v >> 24, v >> 16, v >> 8, v };
  fwrite(b, 1, 4, out);
}

static void finish(call_t *c)
{
  static uint8 ram_out[0x10000];
  uint32 regs[NUM_REGS];
  int i;

  if (!c->keep) return;
  if (c->tainted || taken[c->entry] >= per_func) { ndiscard++; goto done; }
  get_regs(regs);
  get_ram(ram_out);
  put32(c->entry); put32(c->ret); put32(c->frame); put32(c->nio);
  for (i = 0; i < NUM_REGS; i++) put32(c->regs[i]);
  fwrite(c->ram, 1, 0x10000, out);
  for (i = 0; i < NUM_REGS; i++) put32(regs[i]);
  fwrite(ram_out, 1, 0x10000, out);
  for (i = 0; i < c->nio; i++)
  {
    fputc(c->io[i].kind, out);
    fputc(c->io[i].width, out);
    put32(c->io[i].addr);
    put32(c->io[i].value);
  }
  taken[c->entry]++;
  nsamples++;
done:
  free(c->ram);
  free(c->io);
  c->ram = NULL;
  c->io = NULL;
}

static void log_io(uint8 kind, int width, uint32 addr, uint32 value)
{
  int d;
  for (d = 0; d < depth; d++)
  {
    call_t *c = &stack[d];
    if (!c->keep || c->tainted) continue;
    if (width == 4)  /* the runtime splits long accesses into two words */
    {
      if (c->nio + 2 > MAX_IO) { c->tainted = 1; continue; }
      c->io[c->nio++] = (io_t){ kind, 2, addr, value >> 16 };
      c->io[c->nio++] = (io_t){ kind, 2, addr + 2, value & 0xffff };
    }
    else
    {
      if (c->nio + 1 > MAX_IO) { c->tainted = 1; continue; }
      c->io[c->nio++] = (io_t){ kind, width, addr, value };
    }
  }
}

static void hook(hook_type_t type, int width, unsigned int addr, unsigned int value)
{
  uint32 pc, sp;
  int d;

  if (type == HOOK_M68K_R || type == HOOK_M68K_W)
  {
    addr &= 0xffffff;
    if (addr < 0x400000 || addr >= 0xe00000) return;  /* ROM / RAM */
    log_io(type == HOOK_M68K_R ? 'R' : 'W', width, addr, value);
    if (sndlog && type == HOOK_M68K_W && addr >= 0xa00000 && addr < 0xa10000)
      fprintf(sndlog, "M %d %u %04X %02X\n", cur_frame, m68k.cycles, addr & 0xffff,
              width == 2 ? (value >> 8) & 0xff : value & 0xff);
    if (events && type == HOOK_M68K_R)
    {
      if (width == 4)
      {
        gzputc(events, 'R'); gzputc(events, 2); ev32(addr); ev32(value >> 16);
        gzputc(events, 'R'); gzputc(events, 2); ev32(addr + 2); ev32(value & 0xffff);
      }
      else
      {
        gzputc(events, 'R'); gzputc(events, width); ev32(addr); ev32(value);
      }
    }
    return;
  }
  if (type != HOOK_M68K_E) return;

  pc = addr & 0xffffff;
  sp = m68k_get_reg(M68K_REG_SP);

  /* interrupt handlers: every call in progress saw outside interference */
  if (pc == 0x6d2a || pc == 0xff05d2)
    for (d = 0; d < depth; d++) stack[d].tainted = 1;
  if (events && (pc == 0x6d2a || pc == 0xff05d2))
  {
    gzputc(events, pc == 0x6d2a ? 'V' : 'H');
    ev64(icount);
    ev32(ram32(sp + 2));
  }
  if (pc == 0x6d2a && vtrace)
    vblank_record();
  if (pclog && icount < pclog_limit)
    fprintf(pclog, "%llu %06X\n", (unsigned long long)icount, pc);
  icount++;

  /* returns: pop every call whose return point we just reached (calls
     abandoned by stack tricks are dropped when an outer one returns) */
  while (depth > 0)
  {
    for (d = depth - 1; d >= 0; d--)
      if (pc == stack[d].ret && sp == stack[d].sp + 4) break;
    if (d < 0) break;
    while (depth > d + 1)
    {
      depth--;
      if (stack[depth].keep) ndiscard++;
      free(stack[depth].ram);
      free(stack[depth].io);
    }
    depth--;
    finish(&stack[depth]);
  }

  /* calls: an entry point whose stacked return address follows prev_pc */
  if (pc < 0x10000 && is_entry[pc])
  {
    uint32 ret = ram32(sp);
    if (ret > prev_pc && ret <= prev_pc + 6 && depth < MAX_DEPTH)
    {
      call_t *c = &stack[depth++];
      memset(c, 0, sizeof *c);
      c->entry = pc;
      c->ret = ret;
      c->sp = sp;
      c->frame = cur_frame;
      c->keep = taken[pc] < per_func;
      if (c->keep)
      {
        get_regs(c->regs);
        c->ram = malloc(0x10000);
        c->io = malloc(sizeof(io_t) * MAX_IO);
        get_ram(c->ram);
      }
    }
  }
  prev_pc = pc;
}

int main(int argc, char **argv)
{
  char line[64];
  int frames, i;
  FILE *f;

  if (argc < 5)
  {
    fprintf(stderr, "usage: %s ROM FRAMES entries.txt OUT.samples [-i input.txt] [-k PER_FUNC]\n", argv[0]);
    return 1;
  }
  frames = atoi(argv[2]);
  f = fopen(argv[3], "r");
  if (!f) { perror(argv[3]); return 1; }
  while (fgets(line, sizeof line, f))
  {
    unsigned a = strtoul(line, NULL, 16);
    if (a < 0x10000) is_entry[a] = 1;
  }
  fclose(f);
  for (i = 5; i < argc; i++)
  {
    if (!strcmp(argv[i], "-i") && i + 1 < argc) load_script(argv[++i]);
    else if (!strcmp(argv[i], "-k") && i + 1 < argc) per_func = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-v") && i + 1 < argc) vtrace = gzopen(argv[++i], "wb1");
    else if (!strcmp(argv[i], "-e") && i + 1 < argc) events = gzopen(argv[++i], "wb1");
    else if (!strcmp(argv[i], "-s") && i + 1 < argc) sndlog = fopen(argv[++i], "w");
    else if (!strcmp(argv[i], "-z") && i + 1 < argc) zsnap = gzopen(argv[++i], "wb1");
    else if (!strcmp(argv[i], "-p") && i + 2 < argc)  /* debug: log first N pcs */
    {
      pclog = fopen(argv[++i], "w");
      pclog_limit = strtoull(argv[++i], NULL, 0);
    }
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
  if (!load_rom(argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
  audio_init(44100, 0);
  system_init();
  system_reset();

  out = fopen(argv[4], "wb");
  if (!out) { perror(argv[4]); return 1; }
  fwrite("SHOFNCAP", 1, 8, out);
  put32(1);

  if (events || zsnap)
    z80_irq_hook = on_z80_irq;
  if (events)
    z80_pc_hook = on_z80_pc;
  if (sndlog || events)
  {
    real_fm_write = fm_write;
    fm_write = log_fm_write;
  }
  set_cpu_hook(hook);
  for (cur_frame = 0; cur_frame < frames; cur_frame++)
  {
    int16 snd[4096 * 2];
    system_frame_gen(0);
    audio_update(snd);
  }
  fclose(out);
  if (vtrace) gzclose(vtrace);
  if (events) gzclose(events);
  if (zsnap) gzclose(zsnap);
  {
    int funcs = 0;
    for (i = 0; i < 0x10000; i++) funcs += taken[i] > 0;
    fprintf(stderr, "%ld samples from %d functions (%ld discarded)\n", nsamples, funcs, ndiscard);
  }
  return 0;
}
