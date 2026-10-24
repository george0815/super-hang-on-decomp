ROM     ?= sho.md
SHA1    := ecfd7b3bf4dcbee472ddf2f9cdbe968a05b814e0
PYTHON  ?= python3
VASM    ?= tools/bin/vasmm68k_mot

.PHONY: all check disasm report rom recomp fntest replay port clean

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
RCFLAGS := -O2 -std=c99 -D_DEFAULT_SOURCE -Wall -Wno-unused-label -Wno-unused-variable -Isrc/runtime -Ibuild/recomp

recomp: build/recomp/sho_code.c

build/recomp/sho_code.c: tools/recomp.py tools/shodis.py tools/m68k.py config/hints.txt | build
	$(PYTHON) tools/recomp.py $(ROM) build/recomp

build/recomp/z80_code.c: tools/z80recomp.py tools/z80dis.py tools/z80.py | build
	$(PYTHON) tools/z80recomp.py $(ROM) build/recomp

build/recomp/sho_code.o: build/recomp/sho_code.c src/runtime/m68k_rt.h
	$(CC) $(RCFLAGS) -c build/recomp/sho_code.c -o $@

build/fntest: tests/fntest.c build/recomp/sho_code.o
	$(CC) $(RCFLAGS) -o $@ tests/fntest.c build/recomp/sho_code.o

# per-function differential test against reference samples
SAMPLES ?= build/ref/race.samples
fntest: build/fntest
	build/fntest $(ROM) $(SAMPLES)

RUNTIME := src/runtime/genesis.c src/runtime/vdp.c src/runtime/sound_host.c src/sound/ym2612.c src/sound/psg.c build/recomp/z80_code.c
RTHDRS  := src/runtime/m68k_rt.h src/runtime/genesis.h src/runtime/vdp.h src/runtime/z80_rt.h src/runtime/sound.h

build/replay: tests/replay.c $(RUNTIME) $(RTHDRS) build/recomp/sho_code.o
	$(CC) $(RCFLAGS) -o $@ tests/replay.c $(RUNTIME) build/recomp/sho_code.o -lz -lm

# native port (SDL3)
SDL_CFLAGS ?= $(shell pkg-config --cflags sdl3)
SDL_LIBS   ?= $(shell pkg-config --libs sdl3)

build/shangon: src/port/main.c $(RUNTIME) $(RTHDRS) build/recomp/sho_code.o
	$(CC) $(RCFLAGS) $(SDL_CFLAGS) -o $@ src/port/main.c $(RUNTIME) build/recomp/sho_code.o $(SDL_LIBS) -lm

port: build/shangon

build/livetest: tests/livetest.c $(RUNTIME) $(RTHDRS) build/recomp/sho_code.o
	$(CC) $(RCFLAGS) -o $@ tests/livetest.c $(RUNTIME) build/recomp/sho_code.o -lm

# whole-game replay against the reference recording
replay: build/replay
	build/replay $(ROM) build/ref/race.events build/ref/race.vtrace

report: check
	$(PYTHON) tools/shodis.py $(ROM) --report

clean:
	rm -rf build
