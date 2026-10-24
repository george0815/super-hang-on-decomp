# Builds the headless reference tracer against a Genesis Plus GX checkout.
# Build the core objects first:  make -C $(GPGX)/sdl -f Makefile.sdl2 -k
GPGX   ?= ../../genesis-plus-gx
OBJDIR := $(GPGX)/sdl/build_sdl2
OBJS   := $(filter-out $(OBJDIR)/main.o,$(wildcard $(OBJDIR)/*.o))
CFLAGS := -O2 -std=c99 -DLSB_FIRST -DUSE_16BPP_RENDERING -DUSE_LIBTREMOR -DUSE_LIBCHDR \
          -DMAXROMSIZE=33554432 -DHAVE_YM3438_CORE -DHAVE_OPLL_CORE -DHAVE_ALLOCA_H \
          -I$(GPGX)/core -I$(GPGX)/core/z80 -I$(GPGX)/core/m68k -I$(GPGX)/core/sound \
          -I$(GPGX)/core/input_hw -I$(GPGX)/core/cart_hw -I$(GPGX)/core/cart_hw/svp \
          -I$(GPGX)/core/cd_hw -I$(GPGX)/core/ntsc -I$(GPGX)/sdl -I$(GPGX)/sdl/sdl2 \
          -I$(GPGX)/core/cd_hw/libchdr/include

gpgx_trace: gpgx_trace.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm -lz

all: gpgx_trace gpgx_fncap

clean:
	rm -rf gpgx_trace gpgx_fncap $(HOOKDIR)

# Variant with the CPU hook compiled in, for per-function capture
HOOKDIR  := obj_hook
HOOKSRCS := $(GPGX)/core/m68k/m68kcpu.c $(GPGX)/core/vdp_ctrl.c $(GPGX)/core/debug/cpuhook.c $(GPGX)/core/z80/z80.c
HOOKOBJS := $(patsubst %.c,$(HOOKDIR)/%.o,$(notdir $(HOOKSRCS)))
BASEOBJS := $(filter-out $(OBJDIR)/m68kcpu.o $(OBJDIR)/vdp_ctrl.o $(OBJDIR)/z80.o,$(OBJS))

# The core runs the instruction after an interrupt-enabling VDP write from
# inside m68k_set_irq_delay(), bypassing the execute hook; a patched copy
# adds the hook there so instruction counts stay exact.
$(HOOKDIR)/m68kcpu.c: $(GPGX)/core/m68k/m68kcpu.c | $(HOOKDIR)
	awk '!done && /REG_IR = m68ki_read_imm_16\(\);/ { print "      if (cpu_hook) cpu_hook(HOOK_M68K_E, 0, REG_PC, 0);"; done = 1 } { print }' $< > $@
# Likewise report the moment the Z80 accepts an interrupt (it can miss the
# one-line VBlank pulse while the 68000 holds its bus), and each Z80 PC so
# the sound driver's tick start can be located.
$(HOOKDIR)/z80.c: $(GPGX)/core/z80/z80.c | $(HOOKDIR)
	awk '{ print } /^static void take_interrupt\(void\)/ { getline; print; print "  { extern void (*z80_irq_hook)(void); if (z80_irq_hook) z80_irq_hook(); }" } \
	     /^    R\+\+;/ { print "    { extern void (*z80_pc_hook)(unsigned); if (z80_pc_hook) z80_pc_hook(PC); }" }' $< > $@
$(HOOKDIR)/z80.o: $(HOOKDIR)/z80.c
	$(CC) $(CFLAGS) -DHOOK_CPU -fcommon -I$(GPGX)/core/z80 -I$(GPGX)/core/debug -c $< -o $@

$(HOOKDIR)/m68kcpu.o: $(HOOKDIR)/m68kcpu.c
	$(CC) $(CFLAGS) -DHOOK_CPU -fcommon -I$(GPGX)/core/m68k -I$(GPGX)/core/debug -c $< -o $@
$(HOOKDIR)/%.o: $(GPGX)/core/%.c | $(HOOKDIR)
	$(CC) $(CFLAGS) -DHOOK_CPU -fcommon -I$(GPGX)/core/debug -c $< -o $@
$(HOOKDIR)/%.o: $(GPGX)/core/debug/%.c | $(HOOKDIR)
	$(CC) $(CFLAGS) -DHOOK_CPU -fcommon -I$(GPGX)/core/debug -c $< -o $@
$(HOOKDIR):
	mkdir -p $@

gpgx_fncap: gpgx_fncap.c $(HOOKOBJS) $(BASEOBJS)
	$(CC) $(CFLAGS) -DHOOK_CPU -fcommon -I$(GPGX)/core/debug -o $@ $^ -lm -lz -Wl,--wrap=psg_write

# FM synthesizer comparison: reference core vs the port's synth
PORTSND := ../../src/sound/ym2612.c ../../src/sound/psg.c
ymcmp: ymcmp.c $(PORTSND) $(OBJDIR)/ym2612.o
	$(CC) -O2 -std=c99 -D_DEFAULT_SOURCE -I../../src/runtime -o $@ ymcmp.c $(PORTSND) $(OBJDIR)/ym2612.o -lm
