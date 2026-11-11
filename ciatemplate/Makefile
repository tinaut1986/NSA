# The template of the CIAs NSA makes from a .3dsx (SD card -> a .3dsx's actions -> "Install in the HOME Menu",
# source/fbi/homeicon.c): builds ../romfs/ciatemplate.cia. NSA keeps its certificates, ticket, TMD, NCCH header,
# exheader and banner model and puts the .3dsx's code, RomFS, icon and banner picture in. The CIA is committed: run
# `make -C ciatemplate` only after changing something here.
#
# Needs devkitARM and libctru; bannertool and makerom come from ../tools/bin (Linux) or devkitPro's tools (Windows).

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM

# devkitPro's tools first: on Windows ../tools/bin holds Linux programs; on Linux devkitPro has no bannertool or makerom.
export PATH := $(DEVKITPRO)/tools/bin:$(CURDIR)/../tools/bin:$(DEVKITARM)/bin:$(PATH)

BUILD := build
OUTPUT := ../romfs/ciatemplate.cia

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH) -D__3DS__ -I$(DEVKITPRO)/libctru/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -L$(DEVKITPRO)/libctru/lib -lctru -lm

ICON_FLAGS := --flags visible,ratingrequired,recordusage --cero 153 --esrb 153 --usk 153 --pegigen 153 --pegiptr 153 --pegibbfc 153 --cob 153 --grb 153 --cgsrr 153

# The title ID, the names and the icon are replaced in each copy; these are the template's.
MAKEROM_FLAGS := -DAPP_TITLE="NSAHome" -DAPP_PRODUCT_CODE="CTR-P-NSAF" -DAPP_UNIQUE_ID="0xFB1A1" -DAPP_ENCRYPTED=false \
	-DAPP_SYSTEM_MODE="64MB" -DAPP_SYSTEM_MODE_EXT="124MB" -DAPP_CATEGORY="Application" -DAPP_USE_ON_SD="true" \
	-DAPP_MEMORY_TYPE="Application" -DAPP_CPU_SPEED="268MHz" -DAPP_ENABLE_L2_CACHE="false" -DAPP_VERSION_MAJOR="0"

all: $(OUTPUT)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/template.elf: source/main.c | $(BUILD)
	arm-none-eabi-gcc $(CFLAGS) source/main.c $(LDFLAGS) -o $@

$(BUILD)/icon.icn: ../meta/icon_3ds.png | $(BUILD)
	bannertool makesmdh -s "NSA" -l "NSA" -p "NSA" -i ../meta/icon_3ds.png -o $@ $(ICON_FLAGS)

$(BUILD)/banner.bnr: banner.png chime.wav | $(BUILD)
	bannertool makebanner -i banner.png -a chime.wav -o $@

$(OUTPUT): $(BUILD)/template.elf $(BUILD)/icon.icn $(BUILD)/banner.bnr template.rsf romfs/placeholder.txt
	makerom -f cia -o $@ -elf $(BUILD)/template.elf -rsf template.rsf -target t -icon $(BUILD)/icon.icn \
		-banner $(BUILD)/banner.bnr $(MAKEROM_FLAGS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
