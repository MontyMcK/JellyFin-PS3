#---------------------------------------------------------------------------
# Jellyfin PS3 — build system
#---------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT environment variable not set. export PSL1GHT=<path>")
endif

include $(PSL1GHT)/ppu_rules

#---------------------------------------------------------------------------
# Project metadata
#---------------------------------------------------------------------------
TARGET      := $(notdir $(CURDIR))
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include

TITLE       := Jellyfin PS3
APPID       := DEBUGPR01
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

#---------------------------------------------------------------------------
# Tool paths (using the system-installed PSL1GHT toolchain)
#---------------------------------------------------------------------------
PS3PY_DIR       := $(HOME)/ps3dev/ps3py
PYTHON          := env -u LD_LIBRARY_PATH python3.13
PKG_TOOL        := $(PYTHON) $(PS3PY_DIR)/pkg.py
SFO_TOOL        := $(PYTHON) $(PS3PY_DIR)/sfo.py
SFO_TEMPLATE    := $(PS3PY_DIR)/sfo.xml
MAKE_SELF_NPDRM    := $(PSL1GHT)/tools/geohot/make_self_npdrm
PACKAGE_FINALIZE   := $(PSL1GHT)/tools/geohot/package_finalize

#---------------------------------------------------------------------------
# Compiler flags
#---------------------------------------------------------------------------
CFLAGS    = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS  = $(CFLAGS)
LDFLAGS   = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS := -lvdec -laudio -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm \
        -lnet -lsysmodule -lssl -lhttp -lhttputil

LIBDIRS :=

#---------------------------------------------------------------------------
# Build setup (don't modify below unless adding extra file extensions)
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
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES := $(addsuffix .o,$(BINFILES)) \
                 $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
                 $(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE := $(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
                 $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                 $(LIBPSL1GHT_INC) \
                 -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                   $(LIBPSL1GHT_LIB) -L$(PS3DEV)/ppu/lib

.PHONY: $(BUILD) clean pkg run all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo cleaning...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self pkg $(TARGET).pkg

run:
	ps3load $(OUTPUT).self

pkg: $(OUTPUT).self
	@echo "Building $(TARGET).pkg..."
	@rm -rf pkg
	@mkdir -p pkg/USRDIR
	@cp $(OUTPUT).fake.self pkg/USRDIR/EBOOT.BIN
	@cp ICON0.PNG pkg/ICON0.PNG
	@$(SFO_TOOL) --title "$(TITLE)" --appid "$(APPID)" -f $(SFO_TEMPLATE) pkg/PARAM.SFO
	@$(PKG_TOOL) --contentid $(CONTENTID) pkg/ $(TARGET).pkg
	@$(PACKAGE_FINALIZE) $(TARGET).pkg
	@echo "Created $(TARGET).pkg"

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

%.bin.o: %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
