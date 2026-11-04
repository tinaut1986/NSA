#pragma once

#include <3ds.h>

// Speed traces of the transfers (FTP, web page, downloads), to find out where a slow one spends its
// time: every second of a transfer gives a line with the bytes moved and how long the thread spent
// waiting for the network, inside the socket calls and inside the card (or the install). Kept in
// memory while the transfer runs and written to sdmc:/nsa/speed-log.txt when it ends, so the trace
// does not slow down what it measures. On in DEBUG builds, and in the others while
// sdmc:/nsa/speed.flag exists (a user can be asked to create it). tools/speed_tests.py reads it.
//
// Every call takes NULL (a trace that is off) and does nothing then.

typedef struct speedlog speedlog;

void speedlog_init();

// `kind` says what moves ("ftp-up", "web-down", "url"...), `name` which file or address; `total`
// is the size when known, else 0. NULL when traces are off or memory is short.
speedlog* speedlog_begin(const char* kind, const char* name, u64 total);
// One wait for the network (nothing to read, no room to send): time only.
void speedlog_wait(speedlog* l, u64 ticks);
// One socket call that moved `bytes` (0 for one that moved nothing) in `ticks`.
void speedlog_net(speedlog* l, u64 bytes, u64 ticks);
// One read or write of the card (or the install) of `bytes` in `ticks`.
void speedlog_disk(speedlog* l, u64 bytes, u64 ticks);
// A number of the transfer's own shown on each line (a download's segments ahead).
void speedlog_gauge(speedlog* l, u32 value);
// Writes the trace and frees it.
void speedlog_end(speedlog* l, bool ok);

// For the lines: the UI loop counts its frames, and NSA may add a few words (music, CPU).
void speedlog_frame();
void speedlog_set_context(void (*context)(char* out, size_t size));

static inline u64 speedlog_ticks() {
    return svcGetSystemTick();
}
