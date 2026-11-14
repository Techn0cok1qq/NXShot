# NX PCK Studio (Nintendo Switch homebrew) — devkitPro / libnx
ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO が未設定です (devkitPro を入れてください)")
endif
LIBNX ?= $(DEVKITPRO)/libnx
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := nxpckstudio
APP_TITLE   := NX PCK Studio
APP_AUTHOR  := cokacolastic
APP_VERSION := 1.0.3
ROMFS       := romfs
ICON        := icon.jpg

PORTLIBS := $(DEVKITPRO)/portlibs/switch
PKGCONF  := $(PORTLIBS)/bin/aarch64-none-elf-pkg-config
export PKG_CONFIG_PATH := $(PORTLIBS)/lib/pkgconfig

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CXXFLAGS := -g -O2 -Wall -ffunction-sections $(ARCH) -D__SWITCH__ -std=gnu++17 -fno-rtti -fno-exceptions \
            -I$(LIBNX)/include -I$(PORTLIBS)/include \
            -DAPP_VERSION=\"$(APP_VERSION)\" \
            $(shell $(PKGCONF) --cflags sdl2 SDL2_image SDL2_ttf SDL2_mixer libcurl)
LDFLAGS  := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-Map,$(TARGET).map
# アップデート確認 (GitHub) 用の libcurl。pkg-config で取れなければ既定の並びを使う
CURLLIBS := $(shell $(PKGCONF) --libs --static libcurl 2>/dev/null)
ifeq ($(strip $(CURLLIBS)),)
CURLLIBS := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz
endif
LIBS     := $(shell $(PKGCONF) --libs --static SDL2_image SDL2_ttf SDL2_mixer sdl2) $(CURLLIBS) -lz -lnx

CXX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-g++
OBJS := $(patsubst source/%.cpp,build/%.o,$(wildcard source/*.cpp))
NROFLAGS += --nacp=$(TARGET).nacp --romfsdir=$(ROMFS) --icon=$(ICON)

# レシピ行の先頭を Tab でなく ">" にする (貼り付けで Tab が空白に化けても動く)
.RECIPEPREFIX := >

.PHONY: all clean
all: $(TARGET).nro

$(TARGET).nacp:
> @nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp $(ICON) $(shell find $(ROMFS) -type f)

$(TARGET).elf: $(OBJS)
> $(CXX) $(LDFLAGS) $^ $(LIBS) -o $@

build/%.o: source/%.cpp $(wildcard source/*.hpp)
> @mkdir -p build
> $(CXX) $(CXXFLAGS) -c $< -o $@

clean:
> rm -rf build $(TARGET).elf $(TARGET).nro $(TARGET).nacp $(TARGET).map
