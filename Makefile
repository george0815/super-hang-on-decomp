ROM     ?= sho.md
SHA1    := ecfd7b3bf4dcbee472ddf2f9cdbe968a05b814e0
PYTHON  ?= python3
VASM    ?= tools/bin/vasmm68k_mot

.PHONY: all check disasm report rom recomp fntest clean

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
RCFLAGS := -O2 -std=c99 -Wall -Wno-unused-label -Wno-unused-variable -Isrc/runtime -Ibuild/recomp

recomp: check | build
	$(PYTHON) tools/recomp.py $(ROM) build/recomp

build/recomp/sho_code.o: recomp src/runtime/m68k_rt.h
	$(CC) $(RCFLAGS) -c build/recomp/sho_code.c -o $@

build/fntest: tests/fntest.c build/recomp/sho_code.o
	$(CC) $(RCFLAGS) -o $@ tests/fntest.c build/recomp/sho_code.o

# per-function differential test against reference samples
SAMPLES ?= build/ref/race.samples
fntest: build/fntest
	build/fntest $(ROM) $(SAMPLES)

report: check
	$(PYTHON) tools/shodis.py $(ROM) --report

clean:
	rm -rf build
