#include <stdio.h>

#include <3ds.h>

#include "relaunch.h"
#include "../core/core.h"

// osGetTime() the countdown ends at; 0 when none is running.
static volatile u64 relaunch_at = 0;

bool relaunch_request() {
    if(fs_get_3dsx_path() != NULL) {
        return false;
    }

    relaunch_at = osGetTime() + RELAUNCH_DELAY_MS;
    return true;
}

bool relaunch_pending() {
    return relaunch_at != 0;
}

bool relaunch_notice(char* out, size_t size, float* progress) {
    u64 at = relaunch_at;
    if(at == 0) {
        return false;
    }

    u64 now = osGetTime();
    u64 left = at > now ? at - now : 0;
    snprintf(out, size, _(APP_NAME " restarts into the new build in %lu s"), (u32) ((left + 999) / 1000));
    *progress = 1.0f - (float) left / RELAUNCH_DELAY_MS;
    return true;
}

bool relaunch_poll() {
    u64 at = relaunch_at;
    if(at == 0 || osGetTime() < at) {
        return false;
    }

    // libctru starts this title again once NSA has exited normally.
    aptSetChainloaderToSelf();
    return true;
}
