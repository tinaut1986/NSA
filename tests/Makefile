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

TESTS := update_manifest_test

.PHONY: all clean
all: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $^; do ./$$t || exit 1; done

$(BUILD)/update_manifest_test: update_manifest_test.c ../source/fbi/update_manifest.c ../source/fbi/update_manifest.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ update_manifest_test.c ../source/fbi/update_manifest.c $(LDLIBS)

clean:
	rm -rf $(BUILD)
