TARGET := 3DS

NAME := NSA
TITLE := NSA

# bannertool and makerom are not in the devkitARM image; use the vendored ones.
# On Windows tools/bin's Linux programs cannot run: devkitPro's bannertool.exe instead, and tools/windows' stand-ins
# for what devkitPro's MSYS2 lacks or gets wrong: makerom (the fixed makerom.exe of tools/fetch-makerom.sh, because
# devkitPro's makerom.exe 0.19.0 drops the title version's minor number: 0.6.2 becomes 0.0.2, 3DSGuy/Project_CTR#169),
# xxd and zip. (uname, not OS=Windows_NT: devkitPro's MSYS2 shell does not always have OS.)
ifneq ($(filter MINGW% MSYS% CYGWIN%,$(shell uname -s)),)
    # buildtools/make_base tells the host by OS alone and stops without it ("Unsupported host OS.").
    ifeq ($(OS),)
        OS := Windows_NT
    endif
    export PATH := $(CURDIR)/tools/windows:$(DEVKITPRO)/tools/bin:$(PATH)
else
    export PATH := $(CURDIR)/tools/bin:$(PATH)
endif

BUILD_DIR := build
OUTPUT_DIR := output
INCLUDE_DIRS := include
SOURCE_DIRS := source
ROMFS_DIR := romfs

LIBRARY_DIRS += $(DEVKITPRO)/libctru $(DEVKITPRO)/portlibs/armv6k $(DEVKITPRO)/portlibs/3ds
LIBRARIES += vorbisidec ogg curl mbedtls mbedx509 mbedcrypto jansson z citro3d ctru

EXTRA_OUTPUT_FILES := servefiles

BUILD_FLAGS := -Wno-format-truncation
# source/libs/lzma (the Store's .7z): PPMd-compressed archives too.
BUILD_FLAGS += -DZ7_PPMD_SUPPORT

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

# The CIA's title version (makerom's -major/-minor/-micro) and the app's own version (VERSION_STRING: the tag, what
# Settings, the updater and GitHub show) need not go hand in hand. The updater compares VERSION_STRING, and a CIA of the
# same title version installs over the old one; the title version only has to go up for the HOME Menu to refresh the
# icon, banner and jingle it keeps. The micro version has 4 bits: it follows the tag up to 15 and stays at 15 (v0.9.16,
# v0.9.17... are still title 0.9.15) until the minor version goes up (v0.10.0 is title 0.10.0 again). "override": a
# VERSION_MICRO=N on the command line is capped too (and make_base's default of 0 no longer applies, hence the 0).
override VERSION_MICRO := $(shell m='$(VERSION_MICRO)'; case "$$m" in ('') echo 0 ;; (*[!0-9]*) echo "$$m" ;; (*) if [ "$$m" -gt 15 ]; then echo 15; else echo "$$m"; fi ;; esac)

DESCRIPTION := Open source title manager.
AUTHOR := Steveice10

# Separate title from upstream FBI (CTR-P-CFBI / 0xF8001) so both can be installed.
PRODUCT_CODE := CTR-P-CNSA
UNIQUE_ID := 0xFB1A0
BUILD_FLAGS += -DAPP_UNIQUE_ID=$(UNIQUE_ID)

ICON_FLAGS := --flags visible,ratingrequired,recordusage --cero 153 --esrb 153 --usk 153 --pegigen 153 --pegiptr 153 --pegibbfc 153 --cob 153 --grb 153 --cgsrr 153

BANNER_AUDIO := meta/audio_3ds.wav
BANNER_IMAGE := meta/banner_3ds.png
ICON := meta/icon_3ds.png
LOGO := meta/logo_3ds.bcma.lz

# INTERNAL #

include buildtools/make_base

# Every poll(), curl's too, goes through source/core/netpoll.c: one blocking
# poll() would hold up the whole process's socket calls.
LD_FLAGS += -Wl,--wrap=poll

.PHONY: print-version
print-version:
	@echo $(VERSION_STRING)
