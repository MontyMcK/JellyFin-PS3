.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/spu_rules

TARGET   := spubench_spu
BUILD    := build
SOURCES  := source
INCLUDES := ../source

# -O3 with vectorisation: the kernel is hand-written in spu_intrinsics, but the
# scheduler still has to dual-issue it, and the SPU's even/odd pipes only pay
# off if the compiler is allowed to unroll.  No -ffast-math: the SPU result has
# to match the PPU's bit-for-bit closely enough to prove the two simulations
# are the same one.
CFLAGS   := -O3 -Wall -funroll-loops $(MACHDEP) $(INCLUDE)
CXXFLAGS := $(CFLAGS)
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

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(LIBPSL1GHT_INC) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(OUTPUT).elf ../data/spu.bin

else

DEPENDS := $(OFILES:.o=.d)

all: ../../data/spu.bin

../../data/spu.bin: $(OUTPUT).elf
	@cp $< $@

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
