#---------------------------------------------------------------------------------
# NXShot オーバーレイ(.ovl) Makefile (devkitPro / libnx / libtesla)
# 使い方: overlay/libtesla に libtesla を置いて(下の「準備」)、`make -C overlay`
#         → out/NXShot.ovl ができる
# 必要:   switch-libjpeg-turbo(サムネ用。switch-sdl2_image を入れると、いっしょに入る)
# 準備:   git clone --depth 1 https://github.com/WerWolv/libtesla overlay/libtesla
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO が未設定です。devkitProをインストールしてください")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := NXShot
BUILD    := build
SOURCES  := source
OUT      := out

APP_TITLE   := NXShot
APP_AUTHOR  := Cokacolastic
APP_VERSION := 1.3.3

LIBTESLA ?= $(CURDIR)/libtesla

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__
# libtesla は dynamic_cast を使うので、-fno-rtti は付けない
CXXFLAGS := $(CFLAGS) -fno-exceptions -std=gnu++20
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -ljpeg -lnx -lm
LIBDIRS  := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(OUT)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export OFILES   := $(CPPFILES:.cpp=.o)
export LD       := $(CXX)
export INCLUDE  := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(LIBTESLA)/include -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS += --nacp=$(OUTPUT).nacp

.PHONY: all clean

all: $(BUILD) $(OUT)
	@test -f $(LIBTESLA)/include/tesla.hpp || { echo "libtesla がありません: git clone --depth 1 https://github.com/WerWolv/libtesla overlay/libtesla"; exit 1; }
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

$(OUT):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD) $(OUT)

else

.PHONY: all
DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).ovl

$(OUTPUT).ovl: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@echo "built ... $(notdir $@)"

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
