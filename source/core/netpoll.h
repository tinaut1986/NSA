#pragma once

// libctru sends every socket call of the process through one SOC session,
// which serves one request at a time: a thread waiting inside poll() holds up
// every other thread's send, recv and poll for as long as it waits. So no
// thread waits there. The link wraps poll() (-Wl,--wrap=poll, also the one
// curl calls) to ask without waiting and nap between asks; the servers' own
// waits nap the same way through net_nap_ms.

// How long to nap after `waitedMs` of waiting: short at first, as data
// usually comes soon during a transfer, longer once a socket is idle.
u32 net_nap_ms(u32 waitedMs);

// The SOC service's default buffers keep the TCP window small, which caps a
// connection with any latency; 3dsDL measured 48 KB fastest on a New 3DS and
// 64 KB or more breaking connections mid-handshake. For the connections NSA
// makes (downloads). Not on the servers' listening sockets: on the FTP's
// passive one the console reset every data connection, and ftpd, which is
// faster, leaves the 3DS's buffers alone.
#define NET_SOCKET_BUFFER (48 * 1024)

void net_set_buffers(int fd);

// The servers' sockets never block: a send to a client that has gone (no
// room, no answer) would wait inside the SOC session until TCP gives up,
// minutes, holding every other socket call of NSA, the shutdown() meant to
// wake it included (the 2DS froze stopping the FTP server so).
void net_set_nonblocking(int fd);

// recv() on a non-blocking socket: waits (napping) while there is nothing yet;
// -1 after timeoutMs or on an error, 0 when the other side closed.
ssize_t net_recv(int fd, void* buffer, size_t size, u32 timeoutMs);

// Closes a non-blocking socket without losing what is still to send: the SOC
// service drops unsent data when such a socket closes, so say we are done,
// wait (up to 2 s) for the other side to close once it has it all, then close.
void net_close_gently(int fd);

// A thread for the FTP and web servers, as ftpd runs its own: on the New 3DS's
// third core, else on the app core just below the UI thread, so the UI stays
// smooth on an Old 3DS. Not on the system core, where NSA gets 30% of the CPU
// and the tasks run.
Thread net_server_thread(ThreadFunc entry, size_t stackSize);

// For the transfer that holds the turn (xferturn.h) only: a real poll() that waits inside the SOC
// service, at most NET_OWNER_POLL_MS (less when `timeoutMs` is smaller), and wakes as soon as the
// socket is ready, as ftpd waits. Nothing else moves data meanwhile, so the session is held only
// that long for a control request. Never longer: a wait with no end was what froze NSA before.
#define NET_OWNER_POLL_MS 20

struct pollfd;
int net_poll_owner(struct pollfd* fds, u32 count, u32 timeoutMs);

// How the turn's transfer waits, to compare them on a console (SITE POLL, DEBUG builds): NET_WAIT_POLL
// (net_poll_owner every time), NET_WAIT_NAP (naps, as everything else) or NET_WAIT_HYBRID (1 ms naps for
// the first NET_HYBRID_NAP_MS of a wait, then net_poll_owner).
typedef enum {
    NET_WAIT_POLL,
    NET_WAIT_NAP,
    NET_WAIT_HYBRID
} net_wait_mode;

#define NET_HYBRID_NAP_MS 5

void net_set_wait_mode(net_wait_mode mode);
net_wait_mode net_get_wait_mode();
// Whether a turn's wait that has lasted `waitedMs` polls (true) or naps (false).
bool net_owner_polls(u32 waitedMs);
