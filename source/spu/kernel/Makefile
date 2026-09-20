# SPU worker kernel for the animation pool.
#
# Builds an SPU ELF and drops it at ../../../data/jf_spu_kernel.bin, where the
# main Makefile's bin2o rule embeds it as jf_spu_kernel_bin[].  Invoked from
# the top-level `spubin` target, not built standalone.

.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/spu_rules

TARGET   := jf_spu_kernel
BUILD    := build
SOURCES  := source
INCLUDES := ..

# -O3 with unrolling: the kernel is written in spu_intrinsics, but the SPU is
# statically scheduled and its even/odd pipes only stay busy if the compiler is
# free to unroll and interleave.  No -ffast-math -- the SPU result has to stay
# close enough to the PPU's AltiVec path to prove they are the same simulation
# (measured worst relative difference 7.9e-05 after 8 frames).
CFLAGS   := -O3 -Wall -funroll-loops $(MACHDEP) $(INCLUDE)
LDFLAGS  := $(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBS     := -lsputhread
LIBDIRS  :=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD := $(CC)
export OFILES := $(CFILES:.c=.o)

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(LIBPSL1GHT_INC) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@[ -d $(CURDIR)/../../../data ] || mkdir -p $(CURDIR)/../../../data
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(OUTPUT).elf $(CURDIR)/../../../data/jf_spu_kernel.bin

else

DEPENDS := $(OFILES:.o=.d)

all: ../../../../data/jf_spu_kernel.bin

../../../../data/jf_spu_kernel.bin: $(OUTPUT).elf
	@cp $< $@

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
