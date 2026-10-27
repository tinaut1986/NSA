#include <3ds.h>

// This file reads the real hardware, so it must not see the redirections.
#include "input.h"
#undef hidKeysDown
#undef hidKeysHeld
#undef hidKeysUp
#undef hidTouchRead

static u32 keys_down = 0;
static u32 keys_held = 0;
static u32 keys_up = 0;
static touchPosition touch = {0, 0};

// Written by the FTP thread, consumed by input_scan on the main one.
static volatile u32 inject_keys = 0;
static volatile u32 inject_frames = 0;
static volatile u16 inject_x = 0;
static volatile u16 inject_y = 0;
static u32 injected_last = 0;

void input_scan() {
    hidScanInput();

    u32 held = hidKeysHeld();
    hidTouchRead(&touch);

    u32 injected = 0;
    if(inject_frames > 0) {
        injected = inject_keys;
        inject_frames--;

        if(injected & KEY_TOUCH) {
            touch.px = inject_x;
            touch.py = inject_y;
        }
    }

    // Down and up come from the combined state, so an injected press behaves
    // exactly like a real one.
    u32 previous = keys_held;
    keys_held = held | injected;
    keys_down = keys_held & ~previous;
    keys_up = previous & ~keys_held;
    injected_last = injected;
}

u32 input_keys_down() {
    return keys_down;
}

u32 input_keys_held() {
    return keys_held;
}

u32 input_keys_up() {
    return keys_up;
}

void input_touch_read(touchPosition* pos) {
    *pos = touch;
}

void input_inject_keys(u32 keys, u32 frames) {
    inject_keys = keys;
    inject_frames = frames > 0 ? frames : 1;
}

void input_inject_tap(u16 x, u16 y, u32 frames) {
    inject_x = x;
    inject_y = y;
    input_inject_keys(KEY_TOUCH, frames);
}
