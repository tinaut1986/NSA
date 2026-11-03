#pragma once

#include <3ds.h>

// How many connections a transfer uses, found while it runs: more connections bring more only up to a point (the
// server, the other console, the Wi-Fi), and each one past it costs the rest of the console (the SOC service serves
// one request at a time, and the radio is shared with the FTP and the web page).
//
// It measures the speed with the connections it has, opens one more and measures again: kept if it made the
// transfer RAMP_GAIN faster (and then another is tried), else closed, and the transfer stays as it was for
// RAMP_HOLD_MS. Then it measures again and tries one more, and so on until the transfer ends: the right number
// changes with the network.
//
// The caller asks ramp_update often (every 100 ms is plenty) with the bytes so far, and keeps `level` connections
// working: one past it finishes what it has (or stops) and takes nothing more.

#define RAMP_MEASURE_MS 5000   // How long a speed is measured.
#define RAMP_SETTLE_MS 1500    // After opening one, before measuring: its handshake would count against it.
#define RAMP_GAIN 110          // Percent of the speed before that a new connection must bring.
#define RAMP_HOLD_MS 120000    // After one did not help, before trying again.

typedef struct {
    volatile u32 level;   // Connections to keep working.
    u32 max;
    u32 phase;            // RAMP_BASE, RAMP_TRY or RAMP_HOLD (ramp.c).
    u64 at;               // When the next measure may start, or the hold ends.
    u64 start;            // When this measure started (0: not yet).
    u64 startBytes;
    u32 base;             // KB/s with one less, while trying one more.
} ramp;

void ramp_init(ramp* r, u32 start, u32 max);
// Fewer at most from now on (a server that turned connections away).
void ramp_cap(ramp* r, u32 max);
// With the bytes so far; true when `level` changed, with what happened in `note` (for the log).
bool ramp_update(ramp* r, u64 bytes, char* note, size_t noteSize);
