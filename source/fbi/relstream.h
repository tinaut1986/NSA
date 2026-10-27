#pragma once

// A reliable byte stream over frames that may be lost, repeated or arrive out of
// order, like the data frames of the 3DS's local wireless (UDS): sequence numbers,
// a window of frames in flight, selective acknowledgements and retransmission.
// Pure C with no system calls: the caller gives it the radio (rs_io) and drives
// it with rs_poll(). Not thread safe: one lock around every call.
//
// Frames (big endian):
//   DATA  1, seq (4), payload (up to RS_FRAME_MAX)
//   ACK   2, next expected seq (4), bitmap (4): bit i = frame next+1+i has arrived
//   FIN   3, seq (4)         the end of what this side sends; takes a sequence number

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RS_FRAME_MAX 1400
#define RS_WINDOW 48
#define RS_SLOTS 64
#define RS_BUFFER (128 * 1024)
#define RS_DEAD_MS 10000
#define RS_KEEPALIVE_MS 1000

typedef struct {
    // Sends one frame; false when the radio will not take it now (it is tried again later).
    bool (*send_frame)(void* user, const uint8_t* data, size_t len);
    // The next frame that has arrived: its length, or 0 when there is none.
    int (*recv_frame)(void* user, uint8_t* buffer, size_t max);
    uint64_t (*now)(void* user);   // Milliseconds.
    void* user;
} rs_io;

typedef struct rs rs;

rs* rs_create(const rs_io* io);
void rs_free(rs* stream);

// Both are non blocking: how many bytes were taken / are given now.
size_t rs_write(rs* stream, const void* data, size_t size);
size_t rs_read(rs* stream, void* data, size_t size);

// Room for writing, and bytes waiting to be read.
size_t rs_write_space(const rs* stream);
size_t rs_read_available(const rs* stream);

// Moves frames both ways. Call often (every few milliseconds).
void rs_poll(rs* stream);

// No more writes: the end is sent after what is queued.
void rs_close(rs* stream);
// The other side has closed and everything it sent has been read.
bool rs_peer_closed(const rs* stream);
// Nothing heard from the other side for RS_DEAD_MS.
bool rs_failed(const rs* stream);
// Everything written has been acknowledged.
bool rs_flushed(const rs* stream);

// For tests and the log.
typedef struct {
    uint64_t framesSent;
    uint64_t framesResent;
    uint64_t acksSent;
    uint32_t rto;
} rs_stats;

void rs_get_stats(const rs* stream, rs_stats* out);
