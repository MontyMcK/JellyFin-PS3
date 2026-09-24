.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

ICON0       := $(CURDIR)/ICON0.PNG
SFOXML      := $(PS3DEV)/bin/sfo.xml

include $(PSL1GHT)/ppu_rules

#---------------------------------------------------------------------------
# Project identity
#---------------------------------------------------------------------------
TARGET      := $(notdir $(CURDIR))
BUILD       := obj
SOURCES     := source source/audio source/audio/a52 source/audio/dca \
               source/gfx source/net source/api \
               source/player source/player/core source/player/hud source/player/gpu \
               source/player/threads source/player/stream \
               source/ui source/ui/input source/ui/osk source/ui/xmb source/ui/render \
               source/util source/cache source/video source/music source/spu
DATA        := data
INCLUDES    := source/audio source/audio/dcahd \
               source/audio/mlp/ff source/audio/mlp/ff/libavcodec \
               source/gfx source/net source/api \
               source/player source/player/hud source/player/gpu source/player/stream \
               source/ui source/ui/render source/ui/fonts \
               source/util source/cache source/video source/music source/spu

TITLE       := Jellyfin PS3
APPID       := JFPS30000
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

#---------------------------------------------------------------------------
# Compiler flags
#---------------------------------------------------------------------------
CFLAGS      := -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)

# .S files: -mregnames so r0/r1/r2 assemble as REGISTERS rather than as
# undefined symbols.  PSL1GHT's own sprx Makefile passes this for the same
# reason; without it the stub trampolines in source/audio/audio_out_stub.S
# fail with "unsupported relocation against r1".
ASFLAGS     := -mregnames -mcpu=cell $(MACHDEP) $(INCLUDE) -D__ASSEMBLY__
CXXFLAGS    := $(CFLAGS)
LDFLAGS     := $(MACHDEP) -Wl,-Map,$(notdir $@).map

# Vendored liba52 (source/audio/a52): upstream imdct.c passes `roots128 - 32`
# as a deliberate table base and GCC's -Warray-bounds flags every use.  The
# file is untouched upstream source (see a52/PROVENANCE.md); silence that one
# warning for that one object instead of editing the vendored code.
imdct.o: CFLAGS += -Wno-array-bounds

# Vendored FFmpeg MLP/TrueHD decoder (source/audio/mlp): mlp_api.c compiles the
# whole upstream decoder as one translation unit (see mlp/PROVENANCE.md), and
# upstream mlpdec.c has an `if` without braces that GCC flags.  Untouched
# upstream source, so silence that one warning for that one object rather than
# editing the vendored code — same treatment as liba52's imdct.c above.
# The vendored directory is NOT in SOURCES; only its headers are on the
# include path, which is why it needs no -I of its own here.
mlp_api.o: CFLAGS += -Wno-dangling-else

# The audio decoders get -O3 where the rest of the app stays at -O2.
#
# Measured reason, not a hunch: the heartbeat showed playback collapsing
# exactly when the decoded-PCM buffer emptied, while the network was still
# delivering 20+ Mbps -- lossless TrueHD/DTS-HD MA decode is what runs out of
# PPU, not delivery.  -O2 does NOT enable -ftree-vectorize, so with -mcpu=cell
# the PPU's AltiVec unit was sitting idle through the hottest code in the app.
#
# Safe because it is verified, not assumed: tests/test_dts_xll_dump.c decodes a
# real DTS-HD MA fixture and compares byte-for-byte against ffmpeg, and the
# result is bit-exact at -O3 on BOTH x86-64 and big-endian PPC64 (the PPU's
# byte order) -- 806 of 806 frames.  Lossless output is a property that fails
# loudly under that test, so it is the right thing to gate an optimisation on.
# jf_spu.cpp hand-vectorises the animation kernel with AltiVec intrinsics.
# -mcpu=cell targets the right core but does not by itself put the compiler in
# the ABI the <altivec.h> intrinsics need, so state both.  Measured 13.3x over
# the scalar form -- that factor is the whole reason the PPU path is viable
# and the SPU pool is optional (docs/spu-feasibility.md).
jf_spu.o:      CFLAGS += -maltivec -mabi=altivec -O3

mlp_api.o:     CFLAGS += -O3
dcahd_api.o:   CFLAGS += -O3
dcahd_xll.o:   CFLAGS += -O3
dcahd_compat.o:CFLAGS += -O3
adec.o:        CFLAGS += -O3
adec_truehd.o: CFLAGS += -O3
adec_dts.o:    CFLAGS += -O3

LIBS        := -lvdec -laudio -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm \
               -lnet -lsysmodule -lssl -lhttp -lhttputil

LIBDIRS     :=

#---------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
# data/*.bin is found by a wildcard, and a wildcard is evaluated when this
# Makefile is PARSED -- which happens before the `spubin` recipe has run and
# created data/jf_spu_kernel.bin.  On a clean tree the SPU kernel object was
# therefore never added to OFILES and the link failed with an undefined
# reference to jf_spu_kernel_bin.  It only ever appeared to work because an
# earlier build had left the .bin lying around for the next parse to find, and
# `make pkg` after a failed `make` then "succeeded" on that second pass.
#
# Name it explicitly instead of discovering it; keep the wildcard for the rest.
SPU_KERNEL_BIN := jf_spu_kernel.bin
BINFILES := $(filter-out $(SPU_KERNEL_BIN),\
                $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.bin))))
TTFFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.ttf)))
PNGFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.png)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES := $(SPU_KERNEL_BIN).o \
                 $(addsuffix .o,$(BINFILES)) \
                 $(addsuffix .o,$(TTFFILES)) \
                 $(addsuffix .o,$(PNGFILES)) \
                 $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
                 $(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  $(LIBPSL1GHT_INC) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                   $(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean run pkg spubin

# `spubin` is defined below but must NOT become the default goal.  Adding it as
# the first target in this file silently made a bare `make` build only the SPU
# kernel and exit 0 -- so the app was never compiled, "BUILD OK" meant nothing,
# and the real errors only surfaced later under `make pkg`.  State the goal
# explicitly rather than depending on target order.
.DEFAULT_GOAL := $(BUILD)

# The SPU worker kernel is built first: it produces data/jf_spu_kernel.bin,
# which the bin2o rule below turns into an object the PPU side links against.
spubin:
	@$(MAKE) --no-print-directory -C source/spu/kernel

$(BUILD): spubin
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@$(MAKE) --no-print-directory -C source/spu/kernel clean
	@rm -fr $(BUILD) *.elf *.self *.pkg *.gnpdrm.pkg *.map data/jf_spu_kernel.bin

run:
	make
	ps3load $(OUTPUT).self

pkg: $(BUILD) $(OUTPUT).pkg

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

%.bin.o : %.bin
	@echo $(notdir $<)
	@$(bin2o)

%.ttf.o : %.ttf
	@echo $(notdir $<)
	@$(bin2o)

%.png.o : %.png
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
