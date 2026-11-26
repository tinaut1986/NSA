#include <3ds.h>

#include "awake.h"

static LightLock awake_lock = 1;   // What LightLock_Init sets: ready before anyone calls.
static u32 awake_holds = 0;       // One bit per awake_reason.
static bool awake_dark = false;   // NSA turned the backlight off for a closed lid.
static u64 awake_checked = 0;

void awake_hold(awake_reason reason, bool hold) {
    if(reason >= AWAKE_COUNT) {
        return;
    }

    LightLock_Lock(&awake_lock);

    u32 before = awake_holds;
    if(hold) {
        awake_holds |= 1u << reason;
    } else {
        awake_holds &= ~(1u << reason);
    }

    if(before == 0 && awake_holds != 0) {
        aptSetSleepAllowed(false);
    } else if(before != 0 && awake_holds == 0) {
        aptSetSleepAllowed(true);
    }

    LightLock_Unlock(&awake_lock);
}

bool awake_held() {
    return awake_holds != 0;
}

static void awake_backlight(bool on) {
    if(R_SUCCEEDED(gspLcdInit())) {
        (on ? GSPLCD_PowerOnBacklight : GSPLCD_PowerOffBacklight)(GSPLCD_SCREEN_BOTH);
        gspLcdExit();
    }
}

void awake_backlight_restore() {
    if(awake_dark) {
        awake_backlight(true);
        awake_dark = false;
    }
}

void awake_update() {
    if(!awake_held()) {
        // Sleep is allowed again: with the lid still closed libctru has already asked to sleep; light the screens
        // for whatever comes next (the sleep itself, or the lid opening).
        awake_backlight_restore();
        return;
    }

    // ptm:u a few times a second is plenty for a lid.
    u64 now = osGetTime();
    if(now - awake_checked < 250) {
        return;
    }

    awake_checked = now;

    u8 open = 1;
    if(R_FAILED(PTMU_GetShellState(&open))) {
        return;
    }

    if(open == 0 && !awake_dark) {
        awake_backlight(false);
        awake_dark = true;
    } else if(open != 0) {
        awake_backlight_restore();
    }
}
