#include <math.h>
#include <stdio.h>

#include <3ds.h>

#include "ramp.h"

#define RAMP_BASE 0   // Measuring the speed with `level` connections.
#define RAMP_TRY 1    // Measuring it with one more.
#define RAMP_HOLD 2   // As it is until `at`, watching that the speed holds.

void ramp_init(ramp* r, u32 start, u32 max) {
    r->max = max < 1 ? 1 : max > RAMP_MAX ? RAMP_MAX : max;
    r->level = start < 1 ? 1 : start > r->max ? r->max : start;
    r->phase = RAMP_BASE;
    r->at = 0;
    r->start = 0;
    r->base = 0;
    r->baseError = 0;
    r->settled = 0;
}

void ramp_cap(ramp* r, u32 max) {
    r->max = max < 1 ? 1 : max > RAMP_MAX ? RAMP_MAX : max;
    if(r->level > r->max) {
        r->level = r->max;
    }
}

static void ramp_hold(ramp* r, u64 now, float settled) {
    r->phase = RAMP_HOLD;
    r->at = now + RAMP_HOLD_MS;
    r->settled = settled;
}

bool ramp_update(ramp* r, u64 bytes, char* note, size_t noteSize) {
    u64 now = osGetTime();

    if(r->phase == RAMP_HOLD && now >= r->at) {
        // The network may have changed meanwhile: the speed with what there is, first.
        r->phase = RAMP_BASE;
        r->start = 0;
    }

    // Timed from the first data (and, after one more opened, from RAMP_SETTLE_MS on).
    if(r->start == 0) {
        if(bytes > 0 && (r->phase == RAMP_HOLD || now >= r->at)) {
            r->start = r->sampleAt = now;
            r->startBytes = r->sampleBytes = bytes;
            r->samples = 0;
            r->sum = r->squares = 0;
        }

        return false;
    }

    // One sample a second.
    if(now - r->sampleAt >= RAMP_SAMPLE_MS) {
        float kbs = (float) (bytes - r->sampleBytes) * 1000.0f / (float) (now - r->sampleAt) / 1024.0f;
        r->samples++;
        r->sum += kbs;
        r->squares += kbs * kbs;
        r->sampleAt = now;
        r->sampleBytes = bytes;
    }

    if(now - r->start < RAMP_MEASURE_MS || r->samples < 2) {
        return false;
    }

    // The mean, and how far it may be off: the samples' spread over the square root of their count.
    float n = (float) r->samples;
    float mean = r->sum / n;
    float variance = (r->squares - n * mean * mean) / (n - 1);
    float error = variance > 0 ? sqrtf(variance / n) : 0;
    u32 level = r->level;
    r->start = 0;

    if(r->phase == RAMP_HOLD) {
        // Fallen under half of what it settled at: one fewer, and measured again.
        if(level > 1 && mean * 2 < r->settled) {
            snprintf(note, noteSize, "%.0f KB/s over %lu connections, down from %.0f: back to %lu", mean, level, r->settled, level - 1);
            r->level = level - 1;
            r->phase = RAMP_BASE;
            r->at = now + RAMP_SETTLE_MS;
            return true;
        }

        return false;
    }

    if(r->phase == RAMP_TRY) {
        // More than the swings of both measures, and RAMP_GAIN_MIN at least.
        float noise = 2.0f * sqrtf(error * error + r->baseError * r->baseError);
        bool better = mean - r->base > noise && mean * 100.0f >= r->base * RAMP_GAIN_MIN;
        if(!better) {
            // It did not pay off: back to what there was, for a while.
            r->level = level - 1;
            ramp_hold(r, now, r->base);
            snprintf(note, noteSize, "%.0f+-%.0f KB/s over %lu connections, %.0f+-%.0f over %lu: back to %lu for %lu s", mean, error, level, r->base,
                     r->baseError, level - 1, level - 1, (u32) (RAMP_HOLD_MS / 1000));
            return true;
        }
    }

    if(level >= r->max) {
        // Nothing more to try: as it is, measured again later.
        ramp_hold(r, now, mean);
        return false;
    }

    // The speed to beat, and one more.
    snprintf(note, noteSize, "%.0f+-%.0f KB/s over %lu connections%s: trying %lu", mean, error, level, r->phase == RAMP_TRY ? " (it paid off)" : "",
             level + 1);
    r->base = mean;
    r->baseError = error;
    r->level = level + 1;
    r->phase = RAMP_TRY;
    r->at = now + RAMP_SETTLE_MS;
    return true;
}
