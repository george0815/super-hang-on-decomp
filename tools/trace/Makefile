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
HOOKSRCS := $(GPGX)/core/m68k/m68kcpu.c $(GPGX)/core/vdp_ctrl.c $(GPGX)/core/debug/cpuhook.c
HOOKOBJS := $(patsubst %.c,$(HOOKDIR)/%.o,$(notdir $(HOOKSRCS)))
BASEOBJS := $(filter-out $(OBJDIR)/m68kcpu.o $(OBJDIR)/vdp_ctrl.o,$(OBJS))

$(HOOKDIR)/%.o: $(GPGX)/core/m68k/%.c | $(HOOKDIR)
	$(CC) $(CFLAGS) -DHOOK_CPU -fcommon -I$(GPGX)/core/debug -c $< -o $@
$(HOOKDIR)/%.o: $(GPGX)/core/%.c | $(HOOKDIR)
	$(CC) $(CFLAGS) -DHOOK_CPU -fcommon -I$(GPGX)/core/debug -c $< -o $@
$(HOOKDIR)/%.o: $(GPGX)/core/debug/%.c | $(HOOKDIR)
	$(CC) $(CFLAGS) -DHOOK_CPU -fcommon -I$(GPGX)/core/debug -c $< -o $@
$(HOOKDIR):
	mkdir -p $@

gpgx_fncap: gpgx_fncap.c $(HOOKOBJS) $(BASEOBJS)
	$(CC) $(CFLAGS) -DHOOK_CPU -fcommon -I$(GPGX)/core/debug -o $@ $^ -lm -lz
