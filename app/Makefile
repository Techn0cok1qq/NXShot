#---------------------------------------------------------------------------------
# NXShot アプリ(.nro) Makefile (devkitPro / libnx)
# 使い方: devkitPro環境で `make` → out/ に NXShot.nro ができる
# 必要: switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-curl (dkp-pacman -S で入れる)
# あると良い: switch-ffmpeg (入っていれば、動画のサムネとプレビューが使える。無くてもビルドできる)
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

PKGCONF  := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# SDL2 / SDL2_ttf / SDL2_image / libcurl の設定。pkg-configで取れなければ既定の値を使う
PKG_MODS := sdl2 SDL2_ttf SDL2_image libcurl
PKG_CFLAGS := $(shell $(PKGCONF) --cflags $(PKG_MODS) 2>/dev/null)
ifeq ($(strip $(PKG_CFLAGS)),)
PKG_CFLAGS := -I$(DEVKITPRO)/portlibs/switch/include/SDL2 -D_REENTRANT
endif

# ffmpeg(switch-ffmpeg)が入っていれば、動画のサムネ・プレビューを有効にする
FF_MODS := libavformat libavcodec libswscale libavutil
ifneq ($(strip $(shell $(PKGCONF) --exists $(FF_MODS) 2>/dev/null && echo yes)),)
PKG_MODS += $(FF_MODS)
PKG_CFLAGS += -DNXSHOT_FFMPEG
endif

PKG_LIBS := $(shell $(PKGCONF) --libs --static $(PKG_MODS) 2>/dev/null)
ifeq ($(strip $(PKG_LIBS)),)
PKG_LIBS := -lSDL2_ttf -lSDL2_image -lfreetype -lbz2 -ljpeg -lpng16 -lz -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lcurl -lmbedtls -lmbedx509 -lmbedcrypto
endif

# pkg-configの結果にSDL2_imageが入っていない場合でも、IMG_Load等がリンクできるように補う
ifeq ($(findstring SDL2_image,$(PKG_LIBS)),)
PKG_LIBS := -lSDL2_image -ljpeg -lpng16 -lz $(PKG_LIBS)
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
export NROFLAGS += --nacp=$(OUTPUT).nacp --icon=$(CURDIR)/icon.jpg

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
