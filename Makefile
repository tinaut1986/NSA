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
LIBRARIES += curl mbedtls mbedx509 mbedcrypto jansson z citro3d ctru

EXTRA_OUTPUT_FILES := servefiles

BUILD_FLAGS := -Wno-format-truncation

# Our tags are vX.Y.Z; upstream's are X.Y.Z. CI passes VERSION explicitly.
VERSION ?= $(patsubst v%,%,$(shell git describe --tags --abbrev=0))
VERSION_PARTS := $(subst ., ,$(VERSION))

VERSION_MAJOR := $(word 1, $(VERSION_PARTS))
VERSION_MINOR := $(word 2, $(VERSION_PARTS))
VERSION_MICRO := $(word 3, $(VERSION_PARTS))

DESCRIPTION := Open source title manager.
AUTHOR := Steveice10

# Separate title from upstream FBI (CTR-P-CFBI / 0xF8001) so both can be installed.
PRODUCT_CODE := CTR-P-CNSA
UNIQUE_ID := 0xFB1A0

ICON_FLAGS := --flags visible,ratingrequired,recordusage --cero 153 --esrb 153 --usk 153 --pegigen 153 --pegiptr 153 --pegibbfc 153 --cob 153 --grb 153 --cgsrr 153

BANNER_AUDIO := meta/audio_3ds.wav
BANNER_IMAGE := meta/banner_3ds.cgfx
ICON := meta/icon_3ds.png
LOGO := meta/logo_3ds.bcma.lz

# INTERNAL #

include buildtools/make_base
