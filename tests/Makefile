# Host tests: plain C programs built with the host compiler.
# Needs Jansson for the host (libjansson-dev, or JANSSON_PREFIX=<prefix>).

CC ?= cc
CFLAGS += -std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined
ifdef JANSSON_PREFIX
CFLAGS += -I$(JANSSON_PREFIX)/include
LDLIBS += $(JANSSON_PREFIX)/lib/libjansson.a
else
LDLIBS += -ljansson
endif

BUILD := build

TESTS := update_manifest_test relstream_test foldermanifest_test

.PHONY: all clean
all: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $^; do ./$$t || exit 1; done

$(BUILD)/update_manifest_test: update_manifest_test.c ../source/fbi/update_manifest.c ../source/fbi/update_manifest.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ update_manifest_test.c ../source/fbi/update_manifest.c $(LDLIBS)

$(BUILD)/relstream_test: relstream_test.c ../source/fbi/relstream.c ../source/fbi/relstream.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ relstream_test.c ../source/fbi/relstream.c

$(BUILD)/foldermanifest_test: foldermanifest_test.c ../source/fbi/foldermanifest.c ../source/fbi/foldermanifest.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ foldermanifest_test.c ../source/fbi/foldermanifest.c

clean:
	rm -rf $(BUILD)
