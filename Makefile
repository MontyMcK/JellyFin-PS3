.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

ICON0       := $(CURDIR)/ICON0.PNG
SFOXML      := $(HOME)/ps3dev/ps3py/sfo.xml

include $(PSL1GHT)/ppu_rules

#---------------------------------------------------------------------------
# Project identity
#---------------------------------------------------------------------------
TARGET      := $(notdir $(CURDIR))
BUILD       := obj
SOURCES     := source source/audio source/gfx source/net source/api \
               source/player source/player/core source/player/hud source/player/gpu \
               source/player/threads source/player/stream \
               source/ui source/ui/input source/ui/osk source/ui/xmb source/ui/render \
               source/util source/cache source/video
DATA        := data
INCLUDES    := source/audio source/gfx source/net source/api \
               source/player source/player/hud source/player/gpu source/player/stream \
               source/ui source/ui/render source/ui/fonts \
               source/util source/cache source/video

TITLE       := Jellyfin PS3
APPID       := JFPS30000
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

#---------------------------------------------------------------------------
# Compiler flags
#---------------------------------------------------------------------------
CFLAGS      := -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS    := $(CFLAGS)
LDFLAGS     := $(MACHDEP) -Wl,-Map,$(notdir $@).map

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
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.bin)))
TTFFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.ttf)))
PNGFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.png)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES := $(addsuffix .o,$(BINFILES)) \
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

.PHONY: $(BUILD) clean run pkg

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) *.elf *.self *.pkg *.gnpdrm.pkg *.map

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
