#include <stdio.h>

#include <3ds.h>

#include "ramp.h"

#define RAMP_BASE 0   // Measuring the speed with `level` connections.
#define RAMP_TRY 1    // Measuring it with one more.
#define RAMP_HOLD 2   // Waiting to try again.

void ramp_init(ramp* r, u32 start, u32 max) {
    r->max = max < 1 ? 1 : max;
    r->level = start < 1 ? 1 : start > r->max ? r->max : start;
    r->phase = RAMP_BASE;
    r->at = 0;
    r->start = 0;
    r->startBytes = 0;
    r->base = 0;
}

void ramp_cap(ramp* r, u32 max) {
    r->max = max < 1 ? 1 : max;
    if(r->level > r->max) {
        r->level = r->max;
    }
}

bool ramp_update(ramp* r, u64 bytes, char* note, size_t noteSize) {
    u64 now = osGetTime();

    if(r->phase == RAMP_HOLD) {
        if(now < r->at) {
            return false;
        }

        // The network may have changed meanwhile: the speed with what there is, first.
        r->phase = RAMP_BASE;
        r->at = now;
        r->start = 0;
    }

    // Timed from the first data.
    if(r->start == 0) {
        if(bytes > 0 && now >= r->at) {
            r->start = now;
            r->startBytes = bytes;
        }

        return false;
    }

    if(now - r->start < RAMP_MEASURE_MS) {
        return false;
    }

    u32 rate = (u32) ((bytes - r->startBytes) * 1000 / (now - r->start) / 1024);
    u32 level = r->level;
    r->start = 0;

    if(r->phase == RAMP_TRY && rate * 100 < r->base * RAMP_GAIN) {
        // It did not pay off: back to what there was, for a while.
        r->level = level - 1;
        r->phase = RAMP_HOLD;
        r->at = now + RAMP_HOLD_MS;
        snprintf(note, noteSize, "%lu KB/s over %lu connections, %lu KB/s over %lu: back to %lu for %lu s", rate, level, r->base,
                 level - 1, level - 1, (u32) (RAMP_HOLD_MS / 1000));
        return true;
    }

    if(level >= r->max) {
        // Nothing more to try: as it is, measured again later.
        r->phase = RAMP_HOLD;
        r->at = now + RAMP_HOLD_MS;
        return false;
    }

    // The speed to beat, and one more.
    snprintf(note, noteSize, "%lu KB/s over %lu connections%s: trying %lu", rate, level, r->phase == RAMP_TRY ? " (it paid off)" : "",
             level + 1);
    r->base = rate;
    r->level = level + 1;
    r->phase = RAMP_TRY;
    r->at = now + RAMP_SETTLE_MS;
    return true;
}
