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

// A thread for the FTP and web servers, as ftpd runs its own: on the New 3DS's
// third core, else on the app core just below the UI thread, so the UI stays
// smooth on an Old 3DS. Not on the system core, where NSA gets 30% of the CPU
// and the tasks run.
Thread net_server_thread(ThreadFunc entry, size_t stackSize);
