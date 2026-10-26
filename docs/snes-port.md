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

The port is written in 65816 assembly (ca65). C compilers for the 65816
(tcc-816 in PVSnesLib, Calypsi) produce code several times slower than
hand-written assembly, and the budget above has no room for that.

## Checking the port

The race logic is deterministic given the pad input and the frame
cadence. The plan is to keep the SNES's game variables in a known layout,
run the recordings' input scripts through `snesrun`, and compare the
variables with the Genesis reference (`build/ref/*.vtrace`, 68000 RAM at
every VBlank) race frame by race frame. A mismatch then points at the
routine that diverged, as `make verify` does for the C.

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

1. Done: toolchain, headless runner, screen converter and viewer.
2. Done: SA-1 skeleton. Both CPUs boot, there is a VBlank upload queue
   with PPU register shadows, the pad is read, and assets are extracted
   from `sho.md` at build time (Nemesis decoding).
3. Logo done: `snes/logo.s` matches the Genesis frame for frame (three
   frames later). Title next, which needs the object system and sprites.
4. Race renderer: road by HDMA, sprite frames, HUD.
5. Race logic, compared variable by variable with the recordings.
6. Menus: course and music select, password, shop, ranking, options.
7. Sound driver on the SPC700.

## Open decisions

- SA-1 or a plain cartridge (see CPU budget).
- Crop the screen to the middle 256 columns, or squeeze the 320-pixel
  layout. Cropping keeps the pixel art; squeezing would need every
  graphic redrawn.
- How faithful the music has to be: sampled FM instruments, or MSU-1
  streaming of recorded audio as an option.
