#pragma once

// The 3DS's local wireless (UDS) as a connection: no router needed. One console
// hosts a network named after itself, the other scans, finds it and joins; a
// reliable stream (relstream.c) runs over the network's data frames. Using it
// takes the console off its Wi-Fi: udslink_enter() / udslink_leave() bracket it
// and put the FTP server back afterwards.

#include <3ds.h>

typedef struct udslink udslink;

#define UDSLINK_NAME_MAX 32
#define UDSLINK_PEERS_MAX 8

typedef struct {
    char name[UDSLINK_NAME_MAX + 1];
    u8 network[sizeof(udsNetworkStruct)];   // The network as found, to join it.
} udslink_peer;

// Is the console on a Wi-Fi network (with a router)? Then the usual connection is the way.
bool udslink_wifi_connected();

// A line in sdmc:/nsa/transfer-log.txt: every send and receive writes there.
void udslink_log(const char* format, ...);

// Tests: /nsa/nearby.flag forces the local wireless; "auto" in it also skips the questions.
bool udslink_forced();
bool udslink_auto();

// Takes the radio: pauses the FTP server and starts UDS. False when it can not.
bool udslink_enter();
// Gives it back: stops UDS and starts the FTP server again as the Wi-Fi returns.
void udslink_leave();
bool udslink_active();

// Hosting: the network exists until udslink_host_stop().
Result udslink_host_start(const char* name);
void udslink_host_stop();
// A console has joined: a link to it (one at a time; NULL while none).
udslink* udslink_host_accept();

// Joining: scans in the background. The list of what was found so far.
void udslink_scan_start();
void udslink_scan_stop();
u32 udslink_scan_results(udslink_peer* out, u32 max);
Result udslink_connect(const udslink_peer* peer, udslink** out);

// The connection. `io` moves exactly `len` bytes (like transfer_conn_io); `recv_some`
// is non blocking: bytes, 0 when closed, -1 when none yet, -2 on error.
Result udslink_io(udslink* link, bool write, void* buffer, size_t len, Handle cancel, volatile bool* stop);
int udslink_recv_some(udslink* link, void* buffer, size_t len);
// Closes the stream after what is written has been sent, and frees it.
void udslink_close(udslink* link);
