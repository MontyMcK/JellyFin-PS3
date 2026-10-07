.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

ICON0  := $(CURDIR)/ICON0.PNG
SFOXML := $(PS3DEV)/bin/sfo.xml

include $(PSL1GHT)/ppu_rules

TARGET    := spubench
BUILD     := obj
SOURCES   := source
DATA      := data
INCLUDES  := source

TITLE     := Jellyfin PS3 SPU Benchmark
APPID     := JFSPUBENCH
CONTENTID := UP0001-$(APPID)_00-0000000000000000

# -maltivec explicitly: the PPU baseline is only an honest comparison against a
# hand-vectorised SPU if the PPU is allowed to use its own 128-bit unit.
CFLAGS    := -O3 -Wall -mcpu=cell -maltivec -mabi=altivec $(MACHDEP) $(INCLUDE)
CXXFLAGS  := $(CFLAGS)
LDFLAGS   := $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS      := -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm
LIBDIRS   :=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
BINFILES := spu.bin

export LD := $(CC)
export OFILES := $(addsuffix .o,$(BINFILES)) $(CFILES:.c=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(LIBPSL1GHT_INC) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean spubin pkg run

$(BUILD): spubin
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

spubin:
	@$(MAKE) --no-print-directory -C spu

clean:
	@$(MAKE) --no-print-directory -C spu clean
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).pkg $(DATA)/spu.bin *.map

pkg: $(BUILD) $(OUTPUT).pkg

run:
	ps3load $(OUTPUT).self

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

%.bin.o : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
