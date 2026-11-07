#---------------------------------------------------------------------------------
# NXDiscord アプリ(.nro) Makefile (devkitPro / libnx)
# 使い方: devkitPro環境で `make` → out/ に NXDiscord.nro ができる
# 必要: switch-sdl2 switch-sdl2_ttf switch-curl (dkp-pacman -S で入れる)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO が未設定です。devkitProをインストールしてください")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := NXDiscord
BUILD    := build
SOURCES  := source
OUT      := out

APP_TITLE   := NXDiscord
APP_AUTHOR  := Techn0cok1qq
APP_VERSION := 1.0.0

PKGCONF  := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# SDL2 / SDL2_ttf / libcurl の設定。pkg-configで取れなければ既定の値を使う
PKG_CFLAGS := $(shell $(PKGCONF) --cflags sdl2 SDL2_ttf libcurl 2>/dev/null)
ifeq ($(strip $(PKG_CFLAGS)),)
PKG_CFLAGS := -I$(DEVKITPRO)/portlibs/switch/include/SDL2 -D_REENTRANT
endif
PKG_LIBS := $(shell $(PKGCONF) --libs --static sdl2 SDL2_ttf libcurl 2>/dev/null)
ifeq ($(strip $(PKG_LIBS)),)
PKG_LIBS := -lSDL2_ttf -lfreetype -lbz2 -lpng16 -lz -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lcurl -lmbedtls -lmbedx509 -lmbedcrypto
endif

CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__ $(PKG_CFLAGS)
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++20
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := $(PKG_LIBS) -lnx -lm
LIBDIRS  := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(OUT)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
export OFILES   := $(CPPFILES:.cpp=.o)
export LD       := $(CXX)
export INCLUDE  := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS += --nacp=$(OUTPUT).nacp --icon=$(LIBNX)/default_icon.jpg

.PHONY: all clean

all: $(BUILD) $(OUT)
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

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
