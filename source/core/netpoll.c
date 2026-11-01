#include <poll.h>
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
