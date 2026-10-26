# Host tools for the SNES port. The emulator core and the assembler are
# built from local checkouts in tools/ext (not committed):
#   git clone https://github.com/cc65/cc65 tools/ext/cc65 && make -C tools/ext/cc65 bin
#   git clone https://github.com/snes9xgit/snes9x tools/ext/snes9x && make -C tools/ext/snes9x/libretro
SNES9X ?= ../ext/snes9x

snesrun: snesrun.c
	$(CC) -O2 -Wall -Wextra -I$(SNES9X)/libretro -o $@ $< -ldl -lz

clean:
	rm -f snesrun
