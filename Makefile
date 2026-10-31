TARGET := 3DS

NAME := NSA
TITLE := NSA

# bannertool and makerom are not in the devkitARM image; use the vendored ones.
export PATH := $(CURDIR)/tools/bin:$(PATH)

BUILD_DIR := build
OUTPUT_DIR := output
INCLUDE_DIRS := include
SOURCE_DIRS := source
ROMFS_DIR := romfs

LIBRARY_DIRS += $(DEVKITPRO)/libctru $(DEVKITPRO)/portlibs/armv6k $(DEVKITPRO)/portlibs/3ds
LIBRARIES += vorbisidec ogg curl mbedtls mbedx509 mbedcrypto jansson z citro3d ctru

EXTRA_OUTPUT_FILES := servefiles

BUILD_FLAGS := -Wno-format-truncation

# tools/version.sh: X.Y.Z, X.Y.Z-beta or X.Y.Z-dev.N+hash. The release
# workflow passes VERSION and CHANNEL (beta|release).
VERSION_STRING := $(shell sh tools/version.sh)
VERSION_PARTS := $(subst ., ,$(firstword $(subst -, ,$(VERSION_STRING))))
BUILD_FLAGS += -DVERSION_STRING=\"$(VERSION_STRING)\"

# DEBUG=1: development build. The FTP server starts with NSA.
DEBUG ?= 0
BUILD_FLAGS += -DNSA_DEBUG=$(DEBUG)


VERSION_MAJOR := $(word 1, $(VERSION_PARTS))
VERSION_MINOR := $(word 2, $(VERSION_PARTS))
VERSION_MICRO := $(word 3, $(VERSION_PARTS))

DESCRIPTION := Open source title manager.
AUTHOR := Steveice10

# Separate title from upstream FBI (CTR-P-CFBI / 0xF8001) so both can be installed.
PRODUCT_CODE := CTR-P-CNSA
UNIQUE_ID := 0xFB1A0
BUILD_FLAGS += -DAPP_UNIQUE_ID=$(UNIQUE_ID)

ICON_FLAGS := --flags visible,ratingrequired,recordusage --cero 153 --esrb 153 --usk 153 --pegigen 153 --pegiptr 153 --pegibbfc 153 --cob 153 --grb 153 --cgsrr 153

BANNER_AUDIO := meta/audio_3ds.wav
BANNER_IMAGE := meta/banner_3ds.cgfx
ICON := meta/icon_3ds.png
LOGO := meta/logo_3ds.bcma.lz

# INTERNAL #

include buildtools/make_base

.PHONY: print-version
print-version:
	@echo $(VERSION_STRING)
