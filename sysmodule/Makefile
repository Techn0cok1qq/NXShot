#---------------------------------------------------------------------------------
# NXDiscord sysmodule Makefile (devkitPro / libnx)
# 使い方: devkitPro環境で `make` → out/ に nxdiscord.nsp 等ができる(要 switch-curl)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO が未設定です。devkitProをインストールしてください")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET   := nxdiscord
BUILD    := build
SOURCES  := source
DATA     :=
INCLUDES :=
OUT      := out

APP_VERSION := 1.0.0
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++20
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
# libcurl(switch-curl)とその依存ライブラリ。pkg-configで取れなければ既定の並びを使う
CURL_LIBS := $(shell $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config --libs --static libcurl 2>/dev/null)
ifeq ($(strip $(CURL_LIBS)),)
CURL_LIBS := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz
endif
LIBS     := $(CURL_LIBS) -lnx
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
export APP_JSON := $(TOPDIR)/$(TARGET).json

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

all: $(OUTPUT).nsp

$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
