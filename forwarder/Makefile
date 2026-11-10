# NSA's forwarder template: builds ../romfs/forwarder.cia, which NSA copies and patches for each .3dsx it makes a HOME
# Menu icon for (source/fbi/forwarder.c). The CIA is committed: run `make -C forwarder` only after changing the stub.
#
# Needs devkitARM and libctru; bannertool and makerom come from ../tools/bin (Linux) or devkitPro's tools (Windows).

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM

# devkitPro's tools first: on Windows ../tools/bin holds Linux programs; on Linux devkitPro has no bannertool or makerom.
export PATH := $(DEVKITPRO)/tools/bin:$(CURDIR)/../tools/bin:$(DEVKITARM)/bin:$(PATH)

BUILD := build
OUTPUT := ../romfs/forwarder.cia

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) -D__3DS__ -I$(DEVKITPRO)/libctru/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -L$(DEVKITPRO)/libctru/lib -lctru -lm

ICON_FLAGS := --flags visible,ratingrequired,recordusage --cero 153 --esrb 153 --usk 153 --pegigen 153 --pegiptr 153 --pegibbfc 153 --cob 153 --grb 153 --cgsrr 153

# The title ID, the names and the icon are replaced in each copy; these are the template's.
MAKEROM_FLAGS := -DAPP_TITLE="NSAFwd" -DAPP_PRODUCT_CODE="CTR-P-NSAF" -DAPP_UNIQUE_ID="0xFB1A1" -DAPP_ENCRYPTED=false \
	-DAPP_SYSTEM_MODE="64MB" -DAPP_SYSTEM_MODE_EXT="Legacy" -DAPP_CATEGORY="Application" -DAPP_USE_ON_SD="true" \
	-DAPP_MEMORY_TYPE="Application" -DAPP_CPU_SPEED="268MHz" -DAPP_ENABLE_L2_CACHE="false" -DAPP_VERSION_MAJOR="0"

all: $(OUTPUT)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/forwarder.elf: source/main.c | $(BUILD)
	arm-none-eabi-gcc $(CFLAGS) source/main.c $(LDFLAGS) -o $@

$(BUILD)/icon.icn: ../meta/icon_3ds.png | $(BUILD)
	bannertool makesmdh -s "NSA forwarder" -l "NSA forwarder" -p "NSA" -i ../meta/icon_3ds.png -o $@ $(ICON_FLAGS)

$(BUILD)/banner.bnr: ../meta/banner_3ds.png ../meta/audio_3ds.wav | $(BUILD)
	bannertool makebanner -i ../meta/banner_3ds.png -a ../meta/audio_3ds.wav -o $@

$(OUTPUT): $(BUILD)/forwarder.elf $(BUILD)/icon.icn $(BUILD)/banner.bnr forwarder.rsf
	makerom -f cia -o $@ -elf $(BUILD)/forwarder.elf -rsf forwarder.rsf -target t -icon $(BUILD)/icon.icn \
		-banner $(BUILD)/banner.bnr $(MAKEROM_FLAGS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
