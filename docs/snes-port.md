# SNES port: design notes

The goal is a SNES version of Super Hang-On built from the Mega Drive ROM:
`make` reads `sho.md`, converts its graphics, sound and tables, and
assembles a SNES ROM around them. Nothing taken from the ROM is committed.

The handwritten C in `src/game` and `src/sound` is the specification. It is
checked against the original at every VBlank of every recording, so a SNES
routine that matches it matches the game.

## What exists

- `tools/snes/snesrun`: a headless SNES (Snes9x as a libretro core) that
  plays the same input scripts as the Genesis tools (`tests/inputs/*.txt`;
  Genesis A, B, C and Start map to SNES Y, B, A and Start) and writes
  screenshots and RAM dumps.
- `make snes-snap SNAP=x.vdp`: shows one Genesis screen on the SNES.
  `build/livetest` saves the VDP state next to each screenshot, and
  `tools/snes/vdp2snes.py` converts it. The title screen and a race frame
  match the Genesis pictures apart from the cropped edges. This is how
  the video mapping below was checked.
- Assembler: ca65/ld65 (cc65), built from a local checkout in `tools/ext`.

## CPU budget

During races the 68000 (7.67 MHz) is busy 84-88% of the time on average.
It peaks at 99.9% of the three VBlanks that each 20 fps race frame takes.
This was measured with the replay, by counting the instructions spent in
the VBlank wait loop ($6D54) over the race and courses recordings.

The SNES's 65816 runs at 3.58 MHz at best, with 8- and 16-bit operations,
an 8x8 multiplier and no 32-bit arithmetic. The race logic does a lot of
32-bit fixed point and divides. A straight port would run at roughly a
third of the speed, about 7 race frames a second.

**Recommendation: an SA-1 cartridge.** The SA-1 is a second 65816 at
10.74 MHz with a 16x16 multiplier and a 16/16 divider. It is the same
design as the Genesis: one CPU computes the frame and the other uploads it
in VBlank.
- The SA-1 runs the game logic from ROM, BW-RAM and I-RAM.
- The S-CPU runs the NMI handler, which DMAs the prepared tiles, maps,
  OAM and HDMA tables from BW-RAM to the PPU. It also reads the pads and
  talks to the sound CPU.

SA-1 games run in Snes9x, bsnes and Mesen, and on hardware through the
FXPAK Pro. The other option is a plain cartridge with the race logic cut
down or run at a lower rate, which would no longer match the original.

## Language

The game code is `src/game` itself, compiled for the SA-1 by
`tools/snes/c816.py`. The compiler handles the C subset the game uses:
- `rd`/`wr` become accesses to the Genesis RAM mirror (bank $40,
  big-endian) or the Genesis ROM copy. Constant VDP port writes become
  calls into the VDP layer, and anything else goes to the runtime
  (`snes/rt.s`).
- C's integer rules are kept, with 16-bit code where values are known to
  fit.

Hand-written 65816 covers:
- the S-CPU (`snes/boot.s`);
- the SA-1 shell and its VBlank interrupt (`snes/sa1.s`);
- the Genesis VDP layer: ports, DMA, maps, tile slots and HDMA scroll
  (`snes/vdp.s`);
- the sprite renderer (`snes/sprites.s`);
- the hot spots that the profiler shows (`snes/road.s`). These replace
  compiled functions of the same name (`SNES_C_SKIP` in the Makefile).

Debug builds:
- `make snes SNES_TRACE=1` records each compiled function's entry in a
  ring buffer.
- `SNES_PROFILE=1` samples the SA-1's address at every VBlank.
- For exact profiles, `tools/snes/snes9x-sa1prof.patch` adds a profiler
  to the snes9x core (apply it in `tools/ext/snes9x`, then rebuild with
  `make -C tools/ext/snes9x/libretro`). With `SA1PROF=file` set (and
  optionally `SA1PROF_FROM`/`SA1PROF_TO`, a range of video frames), the
  core counts the SA-1's cycles per instruction address and writes them
  at exit. `tools/snes/sa1prof.py file` sums them per function.

## Checking the port

The race logic is deterministic given the pad input and the frame
cadence. The plan is to keep the SNES's game variables in a known layout,
run the recordings' input scripts through `snesrun`, and compare the
variables with the Genesis reference (`build/ref/*.vtrace`, 68000 RAM at
every VBlank) race frame by race frame. A mismatch then points at the
routine that diverged, as `make verify` does for the C.

**How it is checked so far:**
- `make snes-shots` takes screenshots and dumps BW-RAM.
- Bank $40 is compared byte for byte with the 68000 RAM that
  `build/livetest` saves next to its screenshots.
- The SNES runs behind the Genesis: its screen setups take longer, and it
  waits for their uploads before turning the display on.

**Timing.** The recorded input scripts press buttons at given video
frames, and the SNES takes more video frames per game frame. So a test
build plays a recording by game frame instead:
`make snes SNES_PADLOG=file` builds in a table of the pad the game read in
each game frame, taken from a native-port pad log (livetest with
`SHO_PADLOG=file`). With it, every complete race frame of
`tests/inputs/race.txt` matches the Genesis's RAM at the end of the frame,
apart from the bytes that depend on how many VBlanks a frame takes (the
VBlank's DMA stream at $FFC7CC).

**Interrupt safety.** The VBlank interrupt runs compiled C and parts of
the VDP layer. It saves the compiled code's temporaries ($E0-$FF) and the
VDP port state. Hand-written code that it can interrupt keeps its direct
page apart from the interrupt's (road.s's $80-$B5 against vdp.s's DMA
bytes), and the SA-1's multiplier is used with the interrupt masked.

## Video

| Genesis | SNES |
|---|---|
| 320x224 (H40) | 256x224: the middle 256 columns (crop 32 each side); the HUD gets its own layout |
| Plane A, the window plane on the lines it covers | BG1 (mode 1, 4bpp); HDMA switches its map at the window's edge line |
| Plane B | BG2 |
| Per-line horizontal scroll, and the road's per-line vertical scroll set in HBlank | HDMA tables for BG1/BG2 HOFS/VOFS |
| 80 sprites, 20 per line | 128 OBJs, 32 per line, and 34 8-pixel OBJ slices per line |
| 4 palettes of 16, 9-bit colour | BG palettes 0-3 and OBJ palettes 0-3 (CGRAM 0-63, 128-191), 15-bit |
| Sprite high/low priority over planes A/B high/low | OBJ priority 3/2 sits between BG1/BG2 high and low tiles in mode 1, the same order |

**Sprites.** The Genesis builds big objects from overlapping sprites of 1
to 4 tiles a side. On the SNES, the overlap runs into the 34-slice limit,
which drops the lowest-numbered OBJs first; that is how the first title
conversion lost part of "HANG-ON". Drawing the sprites first and cutting
the picture on an aligned 16x16 grid fixes it. Aligned boxes never
overlap, so a line needs at most 32 slices. The port will cut every
sprite frame (bike, rivals, scenery sizes) on a 16-pixel grid at build
time, relative to its origin.

**Sprite frames at run time.** Objects show sprite maps: lists of pieces
of 1 to 4 tiles a side, at offsets from the object, with tiles counted
from the object's tile base. `build/spritecat` plays input scripts and
lists every distinct map, tile base and tile contents shown. Five
recordings (attract, courses, rival_lose, goal, dirt) show 914 frames
from 344 maps, 47,000 tiles in all. Most frames are 32x32 or smaller;
two maps are 224x192.
- 314 of the 344 maps put every piece on one 8-pixel grid with no overlap.
  For these, the SNES can cut the frame into 16x16 OBJs by moving whole
  tiles. The plan: keep a SNES-format copy of the sprite tiles in BW-RAM,
  indexed by Genesis tile number. A frame that comes into view gets OBJ
  VRAM slots, and the NMI copies its tiles there tile by tile. A frame
  stays cached while it is on screen.
- The other 30 (some small maps, and the two big ones) need their pixels
  combined, either on the SA-1 or at build time.
- OBJ VRAM holds 512 tiles. A snapshot shows 43-68 distinct sprite tiles
  at once, and a race frame lasts three VBlanks, each able to upload
  about 170 tiles, so streaming keeps up.

**Background tiles.** A screen shows at most about 200 distinct background
tiles, but their Genesis tile numbers go up to 1790, and SNES BG tile
numbers stop at 1023. Each mode will renumber them by 16-tile blocks.
Name table entries written at run time, such as HUD digits, go through
the same table.

**Road.** The race builds per-line tables: bend (`$FF0B00`), centre
(`$FF0D00`) and the road line drawn on each screen row (`$FF1300`). From
them it fills the per-line horizontal scroll and a per-line vertical
scroll table (`$FF1500`), which the HBlank handler copies into VSRAM a
line at a time. On the SNES both become HDMA tables. This needs no
interrupt per line and no CPU time.

**Width.** Game coordinates stay in the Genesis's 320-pixel space.
Rendering subtracts 32. Scenery placed at the far edges is clipped, as
the right side of the first race snapshot shows.

## Sound

The SNES has no FM chip. Its sound CPU (SPC700, 64 KB of RAM) plays BRR
samples through 8 voices. The plan:
- Render each FM instrument the songs use to a looped BRR sample at build
  time, with the existing YM2612 emulation (`src/sound/ym2612.c`). Render
  the PSG square waves and noise the same way.
- Write an SPC700 driver that plays the songs' sequence data (the commands
  `src/sound/driver.c` handles: notes, durations, transpose, volume,
  modulation, loops and calls), converted at build time.
- 6 FM and 4 PSG channels fit in 8 voices only with sharing. Sound
  effects take voices from the music the way the Genesis driver's sfx
  tracks override music tracks.

The music will not sound identical: FM envelopes and modulation are
approximated by samples and the DSP's ADSR.

## Milestones

1. Done: toolchain, headless runner, screen converter.
2. Done: SA-1 shell, VBlank interrupt, upload lists, assets from the ROM.
3. Done: the whole game compiled from `src/game`.
   - The logo and title match the Genesis's RAM byte for byte.
   - The course and music select screens work.
   - The attract demo race draws its road by HDMA and runs at 20 race
     frames a second.
   - At the end of every race frame, its 64 KB of RAM matches the
     Genesis's, apart from the VBlank counters.
4. Next:
   - The logo and setup speed. The compiled logo is slow, and setups take
     longer than the Genesis's.
   - Checking the real race (with input) and the other modes against the
     recordings. Input must be keyed to game frames, see Timing.
   - Sprite slices in busy race scenes.
5. Sound: an SPC700 driver.

## Open decisions

- SA-1 or a plain cartridge (see CPU budget).
- Crop the screen to the middle 256 columns, or squeeze the 320-pixel
  layout. Cropping keeps the pixel art; squeezing would need every
  graphic redrawn.
- How faithful the music has to be: sampled FM instruments, or MSU-1
  streaming of recorded audio as an option.
