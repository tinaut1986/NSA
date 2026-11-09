#include <string.h>

#include <3ds.h>

#include "input.h"   // hidKeysDown with the presses of the web page and SITE PRESS too.
#include "led.h"

// Slots of 75 ms (the 32 last 38 / 16 s): short 0.3 s, a gap, long 0.9 s, a gap, short 0.3 s, then dark
// for 0.3 s and the 2 s of the loop delay.
#define LED_DELAY 38
#define LED_LOOP_DELAY 32

static volatile int led_asked = 0;   // 1: green, 2: red.
static bool led_on = false;

static bool led_hwc() {
    static bool tried = false;
    static bool hwc = false;
    if(!tried) {
        tried = true;
        hwc = R_SUCCEEDED(mcuHwcInit());
    }

    return hwc;
}

static Result led_set(const InfoLedPattern* pattern) {
    return led_hwc() ? MCUHWC_SetInfoLedPattern(pattern) : -1;
}

Result led_pattern(bool ok, u8 delay, u8 smoothing, u8 loopDelay, u8 blinkSpeed, u32 blinks, u32 on, u32 off) {
    InfoLedPattern pattern;
    memset(&pattern, 0, sizeof(pattern));
    pattern.delay = delay;
    pattern.smoothing = smoothing;
    pattern.loopDelay = loopDelay;
    pattern.blinkSpeed = blinkSpeed;

    u8* colour = ok ? pattern.greenPattern : pattern.redPattern;
    u32 at = 0;
    for(u32 i = 0; i < blinks && at < 32; i++) {
        for(u32 j = 0; j < on && at < 32; j++) {
            colour[at++] = 0xFF;
        }

        at += off;
    }

    return led_set(&pattern);
}

static void led_signal(bool ok) {
    static const u8 shape[32] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};

    InfoLedPattern pattern;
    memset(&pattern, 0, sizeof(pattern));
    pattern.delay = LED_DELAY;
    pattern.loopDelay = LED_LOOP_DELAY;

    u8* colour = ok ? pattern.greenPattern : pattern.redPattern;
    for(int i = 0; i < 32; i++) {
        colour[i] = shape[i] ? 0xFF : 0;
    }

    led_on = R_SUCCEEDED(led_set(&pattern));
}

static void led_off() {
    InfoLedPattern pattern;
    memset(&pattern, 0, sizeof(pattern));
    pattern.loopDelay = 0xFF;
    led_set(&pattern);
    led_on = false;
}

void led_blink(bool ok) {
    led_asked = ok ? 1 : 2;
}

void led_update() {
    // A press puts it out: the user has seen what ended (the message it came with is closed by the same press).
    if(led_on && (hidKeysDown() & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_START | KEY_SELECT | KEY_TOUCH
                                   | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) != 0) {
        led_off();
    }

    int asked = led_asked;
    if(asked != 0) {
        led_asked = 0;
        led_signal(asked == 1);
    }
}

void led_exit() {
    if(led_on) {
        led_off();
    }
}
