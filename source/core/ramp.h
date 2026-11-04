#pragma once

#include <3ds.h>

// How many connections a transfer uses, found while it runs: more connections bring more only up to a point (the
// server, the other console, the Wi-Fi), and each one past it costs the rest of the console (the SOC service serves
// one request at a time, and the radio is shared with the FTP and the web page).
//
// It measures the speed with the connections it has, opens one more and measures again: kept if the transfer went
// faster by more than the speed's own swings (the owner's idea), else closed, and the transfer stays as it was for
// RAMP_HOLD_MS. Then it measures again and tries one more, and so on until the transfer ends: the right number
// changes with the network. While it holds, a speed fallen under half of what it settled at closes one (a server
// or a network gone bad is not helped by more of them).
//
// A measure takes a sample every second for RAMP_MEASURE_MS: the mean, and how far the mean itself may be off (the
// samples' spread over the square root of their count). One more is kept when the mean rose by more than twice
// both of those together, and by RAMP_GAIN_MIN at least: +11% on a steady 1000 KB/s counts, +15% on a download
// swinging between 100 and 600 KB/s (Google Drive, 2026-11-04) does not. A fixed 10% over 5 s took that one to eight.
//
// At most RAMP_MAX, as 3dsDL: eight saturated the New 3DS's network (its FTP could not send 6 bytes), and five
// brought 9% over four at best (catbox.moe).
//
// The caller asks ramp_update often (every 100 ms is plenty) with the bytes so far, and keeps `level` connections
// working: one past it finishes what it has (or stops) and takes nothing more.

#define RAMP_MAX 4
#define RAMP_MEASURE_MS 10000  // How long a speed is measured.
#define RAMP_SAMPLE_MS 1000    // One sample of it every this.
#define RAMP_SETTLE_MS 2000    // After opening one, before measuring: its handshake would count against it.
#define RAMP_GAIN_MIN 105      // Percent of the speed before that a new connection must bring at least.
#define RAMP_HOLD_MS 120000    // After one did not help, before trying again.

typedef struct {
    volatile u32 level;   // Connections to keep working.
    u32 max;
    u32 phase;            // RAMP_BASE, RAMP_TRY or RAMP_HOLD (ramp.c).
    u64 at;               // When the next measure may start, or the hold ends.
    u64 start;            // When this measure started (0: not yet).
    u64 startBytes;
    u64 sampleAt;         // The current sample's start.
    u64 sampleBytes;
    u32 samples;          // This measure's samples: count, sum and sum of squares (KB/s).
    float sum;
    float squares;
    float base;           // The mean with one less, while trying one more,
    float baseError;      // and how far it may be off.
    float settled;        // The mean with `level`, while holding.
} ramp;

void ramp_init(ramp* r, u32 start, u32 max);
// Fewer at most from now on (a server that turned connections away).
void ramp_cap(ramp* r, u32 max);
// With the bytes so far; true when `level` changed, with what happened in `note` (for the log).
bool ramp_update(ramp* r, u64 bytes, char* note, size_t noteSize);
