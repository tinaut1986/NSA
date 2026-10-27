#pragma once

// The part of a console to console transfer that moves the bytes: the payload
// (a file, or a title package) is cut into chunks and carried over several
// TCP connections at once. The 3DS limits the speed of each connection, so
// four of them are about four times as fast (3dsDL found the same).
//
// The control connection (transfer.c: hello and answer) is the first of the
// four; the sender opens the others and says which transfer they belong to:
//   "NSAD", session id (4)                    once, on each extra connection
//   then chunk records until a terminator:
//   offset (8), length (4), the bytes         offset 0xFFFFFFFFFFFFFFFF ends it
// Chunks are taken by whichever connection is free, so a slow one holds back
// only its own chunk. All numbers are big endian.

#include <arpa/inet.h>

struct udslink;

#define TRANSFER_PORT 5002
#define TRANSFER_CONNECTIONS 4
#define TRANSFER_CHUNK (256 * 1024)
#define TRANSFER_IO_TIMEOUT 15000

// A chunk lies inside one segment of the payload (a title package has its
// header and each content as segments), so the one who reads it needs no more.
typedef struct {
    u64 offset;           // In the payload.
    u32 len;
    u32 segment;
    u64 segmentOffset;
} transfer_chunk;

// Sends or receives exactly `len` bytes on a non-blocking socket. It gives up
// when `cancel` (a handle, or 0) or `*stop` (or NULL) says so, or when nothing
// moves for TRANSFER_IO_TIMEOUT.
Result transfer_conn_io(int sock, bool write, void* buf, size_t len, Handle cancel, volatile bool* stop);

// A connection: a socket, or the local wireless (no router) when `uds` is set.
typedef struct {
    int fd;
    struct udslink* uds;
} transfer_link;

Result transfer_link_io(transfer_link* link, bool write, void* buf, size_t len, Handle cancel, volatile bool* stop);
// Non blocking: bytes, 0 when closed, -1 when there are none yet, -2 on an error.
int transfer_link_recv_some(transfer_link* link, void* buf, size_t len);
void transfer_link_close(transfer_link* link);

// A bigger window than the stack's default, set before connecting.
void transfer_conn_set_buffers(int sock);

// The chunks that cover the segments from payload byte `start` on (malloc'd; NULL when
// there is no memory or nothing is left). With `keepFirst` the first segment (the
// header of a title package, which the receiver needs again) is whole whatever `start` is.
transfer_chunk* transfer_chunks_build(const u64* segmentLengths, u32 segmentCount, u64 start, bool keepFirst, u32* count);

// ---- sending ----

// Fills `buffer` (TRANSFER_CHUNK bytes) with the chunk. Runs on several threads.
typedef Result (*transfer_read_fn)(void* user, const transfer_chunk* chunk, void* buffer);

typedef struct transfer_sender transfer_sender;

// Starts the connections. `chunks` (from transfer_chunks_build) belongs to the sender now.
Result transfer_sender_start(transfer_sender** out, transfer_link* control, struct in_addr peer, u32 sessionId, transfer_chunk* chunks, u32 count, transfer_read_fn read, void* user);
// Bytes sent so far. True when every connection is done, with how it went.
bool transfer_sender_progress(transfer_sender* sender, u64* sent, Result* result);
void transfer_sender_cancel(transfer_sender* sender);
// Waits for the threads; closes the extra connections, not the control one.
void transfer_sender_free(transfer_sender* sender);

// ---- receiving ----

// Puts the bytes at `offset` of the payload. Runs on several threads.
typedef Result (*transfer_write_fn)(void* user, u64 offset, const void* buffer, u32 size);

typedef struct transfer_receiver transfer_receiver;

// Expects the payload from `start` to `end` (`listenSock` is -1 over the local wireless, which carries
// everything on the one connection), and also its first `head` bytes
// when `start` is past them (a title package's header, sent again). `listenSock`
// is where the extra connections arrive.
Result transfer_receiver_start(transfer_receiver** out, transfer_link* control, int listenSock, u32 sessionId, u64 start, u64 end, u32 head, transfer_write_fn write, void* user);
// From the main thread, every frame: takes the extra connections that have arrived.
void transfer_receiver_accept(transfer_receiver* receiver);
// Bytes received so far. True once everything is here, or it failed.
bool transfer_receiver_progress(transfer_receiver* receiver, u64* received, Result* result);
void transfer_receiver_cancel(transfer_receiver* receiver);
void transfer_receiver_free(transfer_receiver* receiver);
