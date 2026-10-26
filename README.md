# Super Hang-On decompilation

A work-in-progress decompilation of *Super Hang-On* for the Sega Mega Drive /
Genesis (GM 00004011-00, 1989). The goal is readable C that runs as a native
port on Windows and Linux.

This repository holds **no game data**. To build anything, you need your own
copy of the ROM:

| File     | SHA-1                                      |
|----------|--------------------------------------------|
| `sho.md` | `ecfd7b3bf4dcbee472ddf2f9cdbe968a05b814e0` |

Put it in the repo root. It is gitignored.

## Layout

- `tools/m68k.py`: exact MC68000 instruction decoder
- `tools/shodis.py`: tracing disassembler (control flow, jump tables, report)
- `config/hints.txt`: manual hints: code entry points, jump tables, names
- `tools/recomp.py`: static recompiler, 68000 to C (one C function per entry point)
- `src/runtime/m68k_rt.h`: CPU state, memory access, and flag semantics for the generated C
- `src/runtime/genesis.c`: machine model: memory map, controller, Z80 mailbox, interrupts, video timing
- `src/runtime/vdp.c`: VDP (video chip): ports, DMA, and a per-scanline renderer
- `src/runtime/sound_host.c`: runs the sound driver at the Z80's times; its view of RAM, chips and bank window
- `src/sound/driver.c`: the sound driver (the Z80 program), handwritten
- `src/sound/ym2612.c`, `src/sound/psg.c`: native FM and PSG synthesizers
- `src/port/main.c`: SDL3 frontend for the native port
- `src/port/options.c`: options overlay (sound mix), saved between runs
- `tools/z80.py`, `tools/z80dis.py`, `tools/z80recomp.py`: Z80 decoder, sound driver disassembler, and recompiler (the original driver, for verification)
- `src/runtime/sound_verify.c`: runs the original driver and the C driver side by side in the replay and `tests/sndtest.c` (every sound, plus fuzzing)
- `tools/trace/`: reference tools built on a local Genesis Plus GX checkout (not included):
  - `gpgx_trace`: headless run with scripted input; dumps registers and RAM every frame, plus screenshots
  - `gpgx_fncap`: records function entry/exit state and hardware access logs
- `tools/flow.py`: register dataflow; each function's real inputs and outputs (`build/recomp/interfaces.txt`)
- `src/game/`: handwritten game code, replacing recompiled functions listed in `config/handwritten.txt`
- `src/runtime/verify.c`: runs each handwritten function side by side with its original during a replay
- `tests/fntest.c`: replays the recorded samples against the recompiled C
- `tests/replay.c`: whole-game replay; interrupts and hardware reads come from a reference recording, and the state is compared at every VBlank
- `tests/livetest.c`: headless live-mode run with scripted input and screenshots
- `tests/inputs/`: controller input scripts for the reference runs
- `build/`: generated output (assembly, symbols, C, traces); not committed

## Usage

    make check    # verify the ROM
    make report   # code coverage and unresolved indirect jumps
    make disasm   # writes build/sho.s and build/symbols.txt
    make rom      # reassembles build/sho.s with vasm, compares with the ROM
    make recomp   # writes build/recomp/*.c

Playing:

    make port
    build/shangon sho.md     # arrows, Z/X/C = A/B/C, Enter = Start, F1 = sound options, Esc = quit

Sound options (F1) set master, music, engine and effects volume and the output
filter; they are saved to `settings.ini` in the user's preferences folder.

or with CMake (Linux, or Windows with SDL3 installed):

    cmake -S . -B build/cmake -DSHO_ROM=path/to/sho.md
    cmake --build build/cmake

Differential testing needs Genesis Plus GX cloned into `genesis-plus-gx/`:

    make -C genesis-plus-gx/sdl -f Makefile.sdl2 -k   # core objects (frontend may fail; that's fine)
    make -C tools/trace all
    tools/trace/gpgx_fncap sho.md 3000 build/recomp/entries.txt build/ref/race.samples \
        -i tests/inputs/race.txt -v build/ref/race.vtrace -e build/ref/race.events \
        -s build/ref/race.snd -z build/ref/race.zram
    make fntest    # per-function samples
    make replay    # whole game, compared at every VBlank
    make verify    # replay + every handwritten function checked against its original
    make sndtest   # every sound through the original and the C sound driver side by side
    build/replay sho.md build/ref/race.events build/ref/race.vtrace --zram build/ref/race.zram
                   # also compares sound driver RAM at every Z80 interrupt

## ROM map (so far)

| Range             | Contents                                      |
|-------------------|-----------------------------------------------|
| `000000-0001FF`   | vectors, header                               |
| `000204-004511`   | data tables (read via `$7084`)                |
| `004512-00FFFF`   | 68000 code, mixed with inline data tables     |
| `010000-07FFFF`   | graphics, track and other data                |
| `07D7F4-07F217`   | Z80 sound driver (copied to Z80 RAM at `$6E50`) |

Entry points: entry point `$6BAC`, VBlank `$6D2A`, HBlank `$FF05D2` (RAM copy of `$6DA6`).

## Rewriting in readable C

All of the game is now handwritten C: the 68000 code in `src/game/` and the
Z80 sound driver in `src/sound/driver.c`; the native builds
(`build/shangon`, `build/livetest`, CMake) run only that C
(`src/runtime/entry_native.c`). The recompiled code is kept as the
reference to verify against: each handwritten function is registered in
`config/handwritten.txt` against its original, with a small `_68k` adapter
for the original register interface (from `tools/flow.py`). `make verify`
replays every recorded session (`tests/inputs/*.txt`, recorded with
`tools/record.sh`) and runs each handwritten function next to its original
on the same inputs, comparing RAM, output registers and hardware accesses.
Code that is not a function in the original, such as a game mode's setup up
to its first VBlank wait, is checked as a segment (`until=` in
`config/handwritten.txt`). Code that waits for VBlanks (the waits, the
fade, the frame loops) cannot be compared this way; the functions it calls
are.

The sound driver is checked the same way at its entry points: the replay
(and `make sndtest`, which plays every sound the game uses and fuzzes the
command set) runs the original Z80 program and the C driver from the same
state and compares the Z80 RAM and every chip and bank write.
