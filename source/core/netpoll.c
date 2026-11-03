#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>

#include <3ds.h>

#include "netpoll.h"

u32 net_nap_ms(u32 waitedMs) {
    return waitedMs < 5 ? 1 : waitedMs < 50 ? 5 : waitedMs < 1000 ? 20 : 50;
}

void net_set_buffers(int fd) {
    int size = NET_SOCKET_BUFFER;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

void net_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
}

ssize_t net_recv(int fd, void* buffer, size_t size, u32 timeoutMs) {
    u32 waited = 0;
    while(true) {
        ssize_t r = recv(fd, buffer, size, 0);
        if(r >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return r;
        }

        if(waited >= timeoutMs) {
            return -1;
        }

        u32 nap = net_nap_ms(waited);
        svcSleepThread((s64) nap * 1000000);
        waited += nap;
    }
}

void net_close_gently(int fd) {
    if(fd < 0) {
        return;
    }

    shutdown(fd, SHUT_WR);

    char rest[512];
    u32 waited = 0;
    while(waited < 2000) {
        ssize_t r = recv(fd, rest, sizeof(rest), 0);
        if(r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            break;
        }

        if(r < 0) {
            u32 nap = net_nap_ms(waited);
            svcSleepThread((s64) nap * 1000000);
            waited += nap;
        }
    }

    close(fd);
}

Thread net_server_thread(ThreadFunc entry, size_t stackSize) {
    bool isNew = false;
    Thread thread = NULL;
    if(R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
        thread = threadCreate(entry, NULL, stackSize, 0x31, 2, false);
    }

    if(thread == NULL) {
        thread = threadCreate(entry, NULL, stackSize, 0x31, -2, false);
    }

    return thread;
}

int __real_poll(struct pollfd* fds, nfds_t nfds, int timeout);

static volatile net_wait_mode net_wait = NET_WAIT_POLL;

void net_set_wait_mode(net_wait_mode mode) {
    net_wait = mode;
}

net_wait_mode net_get_wait_mode() {
    return net_wait;
}

bool net_owner_polls(u32 waitedMs) {
    net_wait_mode mode = net_wait;
    return mode == NET_WAIT_POLL || (mode == NET_WAIT_HYBRID && waitedMs >= NET_HYBRID_NAP_MS);
}

int net_poll_owner(struct pollfd* fds, u32 count, u32 timeoutMs) {
    return __real_poll(fds, (nfds_t) count, (int) (timeoutMs < NET_OWNER_POLL_MS ? timeoutMs : NET_OWNER_POLL_MS));
}

int __wrap_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    if(nfds == 0) {
        // How curl sleeps when it has no socket to watch; libctru turns that
        // down with EINVAL, and curl would spin.
        if(timeout > 0) {
            svcSleepThread((s64) timeout * 1000000);
        }

        return 0;
    }

    if(timeout == 0) {
        return __real_poll(fds, nfds, 0);
    }

    u64 start = osGetTime();
    while(true) {
        int ready = __real_poll(fds, nfds, 0);
        if(ready != 0) {
            return ready;
        }

        u32 waited = (u32) (osGetTime() - start);
        if(timeout > 0 && waited >= (u32) timeout) {
            return 0;
        }

        u32 nap = net_nap_ms(waited);
        if(timeout > 0 && nap > (u32) timeout - waited) {
            nap = (u32) timeout - waited;
        }

        svcSleepThread((s64) nap * 1000000);
    }
}
