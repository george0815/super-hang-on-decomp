ROM     ?= sho.md
SHA1    := ecfd7b3bf4dcbee472ddf2f9cdbe968a05b814e0
PYTHON  ?= python3
VASM    ?= tools/bin/vasmm68k_mot

.PHONY: all check disasm report rom recomp fntest replay verify sndtest port snes snes-shots snes-snap clean FORCE

all: disasm

check:
	@echo "$(SHA1)  $(ROM)" | sha1sum -c -

build:
	mkdir -p build

disasm: check | build
	$(PYTHON) tools/shodis.py $(ROM) --asm build/sho.s --syms build/symbols.txt

rom: disasm
	cp $(ROM) build/baserom.md
	cd build && ../$(VASM) -quiet -Fbin -m68000 -no-opt -o sho.bin sho.s
	@cmp build/sho.bin $(ROM) && echo "build/sho.bin: matches $(ROM)"

CC      ?= cc
RCFLAGS := -O2 -std=c99 -D_DEFAULT_SOURCE -Wall -Wno-unused-label -Wno-unused-variable -Isrc/runtime -Isrc/game -Isrc/sound -Ibuild/recomp

recomp: build/recomp/sho_code.c

build/recomp/sho_code.c: tools/recomp.py tools/shodis.py tools/m68k.py tools/flow.py config/hints.txt config/handwritten.txt | build
	$(PYTHON) tools/recomp.py $(ROM) build/recomp

build/recomp/z80_code.c: tools/z80recomp.py tools/z80dis.py tools/z80.py | build
	$(PYTHON) tools/z80recomp.py $(ROM) build/recomp

build/recomp/sho_code_verify.o: build/recomp/sho_code.c src/runtime/m68k_rt.h src/runtime/verify.h
	$(CC) $(RCFLAGS) -DSHO_VERIFY -c build/recomp/sho_code.c -o $@

# tests the recompiled originals (the verify build keeps them)
build/fntest: tests/fntest.c build/recomp/sho_code_verify.o $(GAME) src/game/game.h
	$(CC) $(RCFLAGS) -o $@ tests/fntest.c build/recomp/sho_code_verify.o $(GAME)

# per-function differential test against reference samples
SAMPLES ?= build/ref/race.samples
fntest: build/fntest
	build/fntest $(ROM) $(SAMPLES)

GAME    := $(wildcard src/game/*.c)
RUNTIME := src/runtime/genesis.c src/runtime/vdp.c src/runtime/sound_host.c src/runtime/verify.c src/sound/ym2612.c src/sound/psg.c src/sound/driver.c $(GAME)
RTHDRS  := src/sound/driver.h src/runtime/m68k_rt.h src/runtime/genesis.h src/runtime/vdp.h src/runtime/z80_rt.h src/runtime/sound.h src/runtime/verify.h src/game/game.h

# the replay keeps the recompiled originals to verify the C against
build/replay: tests/replay.c $(RUNTIME) $(RTHDRS) src/runtime/entry_recomp.c src/runtime/sound_verify.c build/recomp/sho_code_verify.o build/recomp/z80_code.c
	$(CC) $(RCFLAGS) -o $@ tests/replay.c $(RUNTIME) src/runtime/entry_recomp.c src/runtime/sound_verify.c build/recomp/sho_code_verify.o build/recomp/z80_code.c -lz -lm

# every sound through the original Z80 driver and the C driver side by side
build/sndtest: tests/sndtest.c $(RUNTIME) $(RTHDRS) src/runtime/entry_recomp.c src/runtime/sound_verify.c build/recomp/sho_code_verify.o build/recomp/z80_code.c
	$(CC) $(RCFLAGS) -o $@ tests/sndtest.c $(RUNTIME) src/runtime/entry_recomp.c src/runtime/sound_verify.c build/recomp/sho_code_verify.o build/recomp/z80_code.c -lm

sndtest: build/sndtest
	build/sndtest $(ROM)

# native port (SDL3)
SDL_CFLAGS ?= $(shell pkg-config --cflags sdl3)
SDL_LIBS   ?= $(shell pkg-config --libs sdl3)

PORTSRC := src/port/main.c src/port/options.c

# the native builds run the handwritten C only
build/shangon: $(PORTSRC) src/port/options.h $(RUNTIME) $(RTHDRS) src/runtime/entry_native.c
	$(CC) $(RCFLAGS) $(SDL_CFLAGS) -o $@ $(PORTSRC) $(RUNTIME) src/runtime/entry_native.c $(SDL_LIBS) -lm

port: build/shangon

build/spritecat: tests/spritecat.c $(RUNTIME) $(RTHDRS) src/runtime/entry_native.c
	$(CC) $(RCFLAGS) -o $@ tests/spritecat.c $(RUNTIME) src/runtime/entry_native.c -lm

build/livetest: tests/livetest.c $(RUNTIME) $(RTHDRS) src/runtime/entry_native.c
	$(CC) $(RCFLAGS) -o $@ tests/livetest.c $(RUNTIME) src/runtime/entry_native.c -lm

# whole-game replay against the reference recording
replay: build/replay
	build/replay $(ROM) build/ref/race.events build/ref/race.vtrace

# replay every recorded session (tools/record.sh) with each handwritten
# function checked against its original; VERIFY_ALL=0 samples long runs
RECORDINGS ?= $(patsubst build/ref/%.events,%,$(wildcard build/ref/*.events))
VERIFY_ALL ?= 1
verify: build/replay
	@for r in $(RECORDINGS); do \
	    echo "== $$r"; \
	    VERIFY_ALL=$(VERIFY_ALL) build/replay $(ROM) build/ref/$$r.events build/ref/$$r.vtrace --verify || exit 1; \
	done

# ---------------------------------------------------------------- SNES
# Tools from local checkouts (see tools/snes/Makefile)
CA65    ?= tools/ext/cc65/bin/ca65
LD65    ?= tools/ext/cc65/bin/ld65
SNESCORE ?= tools/ext/snes9x/libretro/snes9x_libretro.so

tools/snes/snesrun: tools/snes/snesrun.c
	$(MAKE) -C tools/snes snesrun

# The SNES port (snes/, docs/snes-port.md): build/snes/shangon.sfc. The
# game code is src/game compiled for the SA-1 (tools/snes/c816.py); snes/
# holds the hand-written rest. CCODE1-4 are 64 KB code banks.
SNES_C_BANK = race:CCODE1 objects:CCODE2 vblank:CCODE4
# C functions the SNES does its own way (snes/rt.s)
SNES_C_SKIP = nemesis_to_vdp,nemesis_to_ram,scroll_tables,road_rows,race_build_road
SNES_TRACE ?=           # 1: compiled functions note their entry (debugging)
SNES_PROFILE ?=         # 1: the VBlank interrupt samples the SA-1's address
SNES_PADLOG ?=          # a livetest SHO_PADLOG: the build plays that input
SNES_DEBUG ?=           # 1: the VBlank copies I-RAM to BW-RAM (iram_copy)
SNES_C_OBJS := $(patsubst src/game/%.c,build/snes/c/%.o,$(GAME))
SNES_OBJS := $(patsubst snes/%.s,build/snes/%.o,$(wildcard snes/*.s)) build/snes/assets.o \
             $(SNES_C_OBJS) build/snes/c/init.o $(if $(SNES_PADLOG),build/snes/padtable.o)
SNES_INCS := $(wildcard snes/inc/*.inc) build/snes/gen/assets.inc build/snes/flags
SNES_ASFLAGS := $(if $(SNES_PROFILE),-D PROFILE) $(if $(SNES_PADLOG),-D PADTABLE) $(if $(SNES_DEBUG),-D DEBUG)

snes: build/snes/shangon.sfc

build/snes/gen/assets.s build/snes/gen/assets.inc &: tools/snes/assets.py tools/snes/nemesis.py $(GAME) | check
	$(PYTHON) tools/snes/assets.py $(ROM) build/snes/gen

# (rewritten only when the options change, so changing them rebuilds the asm)
build/snes/flags: FORCE
	@mkdir -p build/snes
	@echo '$(SNES_ASFLAGS)' | cmp -s - $@ || echo '$(SNES_ASFLAGS)' > $@

build/snes/padtable.o: $(SNES_PADLOG) tools/snes/padtable.py
	$(PYTHON) tools/snes/padtable.py $(SNES_PADLOG) build/snes/gen/padtable.s
	$(CA65) --cpu 65816 -o $@ build/snes/gen/padtable.s

build/snes/assets.o: build/snes/gen/assets.s
	$(CA65) --cpu 65816 -I build/snes/gen --bin-include-dir build/snes/gen -o $@ $<

build/snes/%.o: snes/%.s $(SNES_INCS)
	$(CA65) --cpu 65816 -g -I snes -I build/snes $(SNES_ASFLAGS) -o $@ $<

build/snes/c/%.s: src/game/%.c src/game/game.h tools/snes/c816.py $(wildcard tools/snes/include/*.h)
	@mkdir -p build/snes/c
	$(PYTHON) tools/snes/c816.py $@ $(or $(patsubst $*:%,%,$(filter $*:%,$(SNES_C_BANK))),CCODE3) $< --skip $(SNES_C_SKIP) $(if $(SNES_TRACE),--trace)

build/snes/c/%.o: build/snes/c/%.s $(SNES_INCS)
	$(CA65) --cpu 65816 -g -I snes -I build/snes -o $@ $<

# Calls every compiled file's initialiser of statics
build/snes/c/init.s: $(patsubst src/game/%.c,build/snes/c/%.s,$(GAME))
	@( echo '.p816'; echo '.export c_init'; \
	   for f in $$(grep -h '^.export [a-z_]*__init$$' $^ | cut -d' ' -f2); do echo ".import $$f"; done; \
	   echo '.segment "CODE2"'; echo 'c_init:'; \
	   for f in $$(grep -h '^.export [a-z_]*__init$$' $^ | cut -d' ' -f2); do echo "    jsl $$f"; done; \
	   echo '    rtl' ) > $@

build/snes/shangon.sfc: $(SNES_OBJS) snes/shangon.cfg tools/snes/checksum.py
	$(LD65) -C snes/shangon.cfg -m build/snes/shangon.map --dbgfile build/snes/shangon.dbg -o $@ $(SNES_OBJS)
	$(PYTHON) tools/snes/checksum.py $@

# Screenshots of the SNES build: SHOTS frames of a FRAMES-frame run with
# the INPUT script, to build/snes/shot_N.png
FRAMES ?= 400
SHOTS  ?= 100 200 300
BWSHOTS ?=
PADLOG ?=               # drive the pad by the game's reads (livetest SHO_PADLOG)
FRAMEDUMP ?=            # 1 (with SNES_PADLOG builds): build/snes/shot_fN.ram as game frame N ends
INPUT  ?= tests/inputs/attract.txt
snes-shots: build/snes/shangon.sfc tools/snes/snesrun
	tools/snes/snesrun $(SNESCORE) $< $(FRAMES) -i $(INPUT) $(foreach f,$(SHOTS),-s $(f)) $(foreach f,$(BWSHOTS),-B $(f)) $(if $(PADLOG),-P $(PADLOG)) $(if $(FRAMEDUMP),-F) -o build/snes/shot -w build/snes/wram.bin -b build/snes/bwram.bin -v build/snes/vram.bin

# A Genesis screen on the SNES: SNAP is a .vdp snapshot from livetest
# (build/livetest ROM FRAMES INPUT OUTDIR FRAME); writes SNAPDIR/snap.sfc
# and the SNES's picture of it, SNAPDIR/snes_9.png.
SNAPDIR ?= $(basename $(SNAP))
snes-snap: tools/snes/snesrun
	$(PYTHON) tools/snes/vdp2snes.py $(SNAP) $(SNAPDIR) $(CROP)
	$(CA65) --cpu 65816 --bin-include-dir $(SNAPDIR) -o $(SNAPDIR)/snap.o snes/snap/snap.s
	$(LD65) -C snes/snap/snap.cfg -o $(SNAPDIR)/snap.sfc $(SNAPDIR)/snap.o
	$(PYTHON) tools/snes/checksum.py $(SNAPDIR)/snap.sfc
	tools/snes/snesrun $(SNESCORE) $(SNAPDIR)/snap.sfc 10 -s 9 -o $(SNAPDIR)/snes

report: check
	$(PYTHON) tools/shodis.py $(ROM) --report

clean:
	rm -rf build

FORCE:
