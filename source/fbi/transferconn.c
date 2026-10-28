#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <3ds.h>

#include "selftest.h"
#include "transferconn.h"
#include "udslink.h"
#include "../core/core.h"

#define CONN_MAGIC "NSAD"
#define CONN_THREAD_STACK 0x8000
// Lower than the main thread (0x30): the screen and the FTP server keep running
// however busy the connections are.
#define CONN_THREAD_PRIORITY 0x38
#define CONN_END_OFFSET 0xFFFFFFFFFFFFFFFFULL
#define CONN_HELLO_TIMEOUT 5000
#define TRANSFER_PENDING_MAX 8

static void conn_put32(u8* out, u32 value) {
    out[0] = (u8) (value >> 24);
    out[1] = (u8) (value >> 16);
    out[2] = (u8) (value >> 8);
    out[3] = (u8) value;
}

static void conn_put64(u8* out, u64 value) {
    conn_put32(out, (u32) (value >> 32));
    conn_put32(out + 4, (u32) value);
}

static u32 conn_get32(const u8* in) {
    return (u32) in[0] << 24 | (u32) in[1] << 16 | (u32) in[2] << 8 | in[3];
}

static u64 conn_get64(const u8* in) {
    return (u64) conn_get32(in) << 32 | conn_get32(in + 4);
}

Result transfer_conn_io(int sock, bool write, void* buf, size_t len, Handle cancel, volatile bool* stop) {
    size_t done = 0;
    u64 last = osGetTime();

    while(done < len) {
        if((cancel != 0 && svcWaitSynchronization(cancel, 0) == 0) || (stop != NULL && *stop)) {
            return R_APP_CANCELLED;
        }

        struct pollfd pfd = {sock, (short) (write ? POLLOUT : POLLIN), 0};
        int ready = poll(&pfd, 1, 200);
        if(ready < 0) {
            udslink_log("io: poll failed, errno %d (%s %zu of %zu bytes)\n", errno, write ? "writing" : "reading", done, len);
            return R_APP_IO_ERROR;
        }

        if(ready == 0) {
            if(osGetTime() - last > TRANSFER_IO_TIMEOUT) {
                udslink_log("io: nothing for %d s (%s %zu of %zu bytes)\n", TRANSFER_IO_TIMEOUT / 1000, write ? "writing" : "reading", done, len);
                return R_APP_IO_ERROR;
            }

            continue;
        }

        ssize_t n = write ? send(sock, (u8*) buf + done, len - done, 0) : recv(sock, (u8*) buf + done, len - done, 0);
        if(n > 0) {
            done += (size_t) n;
            last = osGetTime();
        } else if(n == 0 ? !write : (errno != EAGAIN && errno != EWOULDBLOCK)) {
            udslink_log("io: %s, errno %d (%s %zu of %zu bytes, poll events %d)\n", n == 0 ? "the other side closed" : "error", errno, write ? "writing" : "reading", done, len, pfd.revents);
            return R_APP_IO_ERROR;
        } else {
            // Ready, yet nothing moved: do not spin.
            svcSleepThread(1000000);
        }
    }

    return 0;
}

void transfer_conn_set_buffers(int sock) {
    int size = 48 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

Result transfer_link_io(transfer_link* link, bool write, void* buf, size_t len, Handle cancel, volatile bool* stop) {
    if(link->uds != NULL) {
        return udslink_io(link->uds, write, buf, len, cancel, stop);
    }

    return transfer_conn_io(link->fd, write, buf, len, cancel, stop);
}

int transfer_link_recv_some(transfer_link* link, void* buf, size_t len) {
    if(link->uds != NULL) {
        return udslink_recv_some(link->uds, buf, len);
    }

    ssize_t n = recv(link->fd, buf, len, 0);
    if(n > 0) {
        return (int) n;
    }

    if(n == 0) {
        return 0;
    }

    return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : -2;
}

void transfer_link_wait_closed(transfer_link* link, u32 ms) {
    u64 begin = osGetTime();
    while(osGetTime() - begin < ms) {
        u8 byte;
        int n = transfer_link_recv_some(link, &byte, 1);
        if(n == 0 || n == -2) {
            return;
        }

        svcSleepThread(20000000);
    }
}

void transfer_link_close(transfer_link* link) {
    if(link->uds != NULL) {
        udslink_close(link->uds);
        link->uds = NULL;
    } else if(link->fd >= 0) {
        close(link->fd);
    }

    link->fd = -1;
}

transfer_chunk* transfer_chunks_build(const u64* segmentLengths, u32 segmentCount, u64 start, bool keepFirst, u32* count) {
    u64 total = 0;
    u32 chunks = 0;
    for(u32 i = 0; i < segmentCount; i++) {
        u64 from = (total > start || (keepFirst && i == 0)) ? 0 : start - total;
        if(from < segmentLengths[i]) {
            chunks += (u32) ((segmentLengths[i] - from + TRANSFER_CHUNK - 1) / TRANSFER_CHUNK);
        }

        total += segmentLengths[i];
    }

    *count = chunks;
    if(chunks == 0) {
        return NULL;
    }

    transfer_chunk* out = (transfer_chunk*) calloc(chunks, sizeof(transfer_chunk));
    if(out == NULL) {
        return NULL;
    }

    u32 index = 0;
    total = 0;
    for(u32 i = 0; i < segmentCount; i++) {
        u64 from = (total > start || (keepFirst && i == 0)) ? 0 : start - total;
        for(u64 offset = from; offset < segmentLengths[i]; offset += TRANSFER_CHUNK) {
            u64 left = segmentLengths[i] - offset;
            out[index].offset = total + offset;
            out[index].len = left < TRANSFER_CHUNK ? (u32) left : TRANSFER_CHUNK;
            out[index].segment = i;
            out[index].segmentOffset = offset;
            index++;
        }

        total += segmentLengths[i];
    }

    return out;
}

// Connections 2 and 3 try the New 3DS's extra core, as 3dsDL does; the others
// (and these on an Old 3DS) run on the app core.
static Thread conn_thread(ThreadFunc fn, void* arg, u32 index) {
    static int isNew = -1;
    if(isNew < 0) {
        bool value = false;
        isNew = R_SUCCEEDED(APT_CheckNew3DS(&value)) && value ? 1 : 0;
    }

    Thread thread = NULL;
    if(index >= 2 && isNew == 1) {
        thread = threadCreate(fn, arg, CONN_THREAD_STACK, CONN_THREAD_PRIORITY, 2, false);
    }

    if(thread == NULL) {
        thread = threadCreate(fn, arg, CONN_THREAD_STACK, CONN_THREAD_PRIORITY, -2, false);
    }

    return thread;
}

// ---- sending ----

struct transfer_sender {
    transfer_link* control;
    struct in_addr peer;
    u32 id;

    transfer_chunk* chunks;
    u32 count;
    u32 next;
    u64 sent;

    transfer_read_fn read;
    void* user;

    LightLock lock;
    volatile bool stop;
    Result result;
    u32 running;

    Thread threads[TRANSFER_CONNECTIONS];
    struct {
        transfer_sender* sender;
        u32 index;
    } args[TRANSFER_CONNECTIONS];
};

static void sender_fail(transfer_sender* s, Result res) {
    LightLock_Lock(&s->lock);
    if(s->result == 0) {
        s->result = res;
        udslink_log("sender: stopped with %08lX after %llu bytes, chunk %u of %u\n", res, s->sent, s->next, s->count);
    }

    s->stop = true;
    LightLock_Unlock(&s->lock);
}

static void sender_worker(void* arg) {
    transfer_sender* s = *(transfer_sender**) arg;
    u32 index = (u32) (((u8*) arg - (u8*) s->args) / sizeof(s->args[0]));

    transfer_link link = *s->control;
    u8* buffer = (u8*) malloc(TRANSFER_CHUNK);
    Result res = buffer != NULL ? 0 : R_APP_OUT_OF_MEMORY;

    if(R_SUCCEEDED(res) && index != 0) {
        link.uds = NULL;
        link.fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if(link.fd < 0) {
            res = R_APP_IO_ERROR;
        } else {
            transfer_conn_set_buffers(link.fd);

            struct sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_port = htons(TRANSFER_PORT);
            to.sin_addr = s->peer;

            if(connect(link.fd, (struct sockaddr*) &to, sizeof(to)) < 0) {
                res = R_APP_IO_ERROR;
            } else {
                fcntl(link.fd, F_SETFL, fcntl(link.fd, F_GETFL, 0) | O_NONBLOCK);

                u8 hello[8];
                memcpy(hello, CONN_MAGIC, 4);
                conn_put32(hello + 4, s->id);
                res = transfer_conn_io(link.fd, true, hello, sizeof(hello), 0, &s->stop);
            }
        }
    }

    while(R_SUCCEEDED(res)) {
        LightLock_Lock(&s->lock);
        u32 chunkIndex = s->stop ? s->count : s->next++;
        LightLock_Unlock(&s->lock);

        if(chunkIndex >= s->count) {
            break;
        }

        const transfer_chunk* chunk = &s->chunks[chunkIndex];

        u8 record[12];
        conn_put64(record, chunk->offset);
        conn_put32(record + 8, chunk->len);

        if(R_SUCCEEDED(res = s->read(s->user, chunk, buffer))
           && R_SUCCEEDED(res = transfer_link_io(&link, true, record, sizeof(record), 0, &s->stop))
           && R_SUCCEEDED(res = transfer_link_io(&link, true, buffer, chunk->len, 0, &s->stop))) {
            LightLock_Lock(&s->lock);
            s->sent += chunk->len;
            LightLock_Unlock(&s->lock);

            // As if the network dropped: the connection goes, the transfer fails.
            if(selftest_fault("send.chunk")) {
                res = R_APP_IO_ERROR;
            }
        }
    }

    if(R_FAILED(res)) {
        if(res != R_APP_CANCELLED) {
            udslink_log("sender: connection %u failed with %08lX\n", index, res);
        }

        sender_fail(s, res);
    } else if(!s->stop) {
        // The end of this connection's chunks. The receiver may be gone already
        // once it has everything: that is not a failure.
        u8 record[12];
        conn_put64(record, CONN_END_OFFSET);
        conn_put32(record + 8, 0);
        transfer_link_io(&link, true, record, sizeof(record), 0, &s->stop);

        // Closing a socket on the 3DS throws away what it has not sent yet and
        // resets the connection, which cost the receiver the last bytes of a chunk.
        // So an extra connection is only closed once the receiver says it has all.
        if(index != 0) {
            u8 ack = 0;
            Result ackRes = transfer_link_io(&link, false, &ack, 1, 0, &s->stop);
            if(R_FAILED(ackRes) && ackRes != R_APP_CANCELLED) {
                udslink_log("sender: connection %u was not told the end was received: %08lX\n", index, ackRes);
            }
        }
    }

    free(buffer);
    if(index != 0 && link.fd >= 0) {
        close(link.fd);
    }

    LightLock_Lock(&s->lock);
    s->running--;
    LightLock_Unlock(&s->lock);
}

Result transfer_sender_start(transfer_sender** out, transfer_link* control, struct in_addr peer, u32 sessionId, transfer_chunk* chunks, u32 count, transfer_read_fn read, void* user) {
    transfer_sender* s = (transfer_sender*) calloc(1, sizeof(transfer_sender));
    if(s == NULL) {
        free(chunks);
        return R_APP_OUT_OF_MEMORY;
    }

    s->control = control;
    s->peer = peer;
    s->id = sessionId;
    s->chunks = chunks;
    s->count = count;
    s->read = read;
    s->user = user;
    LightLock_Init(&s->lock);

    // The local wireless has one connection: it is the radio that is the limit.
    u32 workers = control->uds != NULL ? 1 : (count < TRANSFER_CONNECTIONS ? count : TRANSFER_CONNECTIONS);
    s->running = workers;

    u32 started = 0;
    for(u32 i = 0; i < workers; i++) {
        s->args[i].sender = s;
        s->args[i].index = i;
        s->threads[i] = conn_thread(sender_worker, &s->args[i], i);
        if(s->threads[i] == NULL) {
            // Fewer connections still work, but at least the control one is needed.
            LightLock_Lock(&s->lock);
            s->running--;
            LightLock_Unlock(&s->lock);

            if(i == 0) {
                break;
            }
        } else {
            started++;
        }
    }

    if(started == 0) {
        free(s->chunks);
        free(s);
        return R_APP_THREAD_CREATE_FAILED;
    }

    *out = s;
    return 0;
}

bool transfer_sender_progress(transfer_sender* s, u64* sent, Result* result) {
    LightLock_Lock(&s->lock);
    *sent = s->sent;
    *result = s->result;
    bool done = s->running == 0;
    LightLock_Unlock(&s->lock);

    return done;
}

void transfer_sender_cancel(transfer_sender* s) {
    sender_fail(s, R_APP_CANCELLED);
}

void transfer_sender_free(transfer_sender* s) {
    if(s == NULL) {
        return;
    }

    s->stop = true;
    for(u32 i = 0; i < TRANSFER_CONNECTIONS; i++) {
        if(s->threads[i] != NULL) {
            threadJoin(s->threads[i], U64_MAX);
            threadFree(s->threads[i]);
        }
    }

    free(s->chunks);
    free(s);
}

// ---- receiving ----

struct transfer_receiver {
    transfer_link* control;
    int listenSock;
    u32 id;
    u64 start;
    u64 end;
    u32 head;
    u64 needed;     // Bytes that must arrive for it to be whole.

    transfer_write_fn write;
    void* user;

    LightLock lock;
    volatile bool stop;
    Result result;
    u64 received;
    bool complete;
    u32 running;
    u32 used;

    // Connections accepted but not yet known to be this transfer's (see
    // transfer_receiver_accept). Only the main thread touches them.
    struct {
        int fd;
        u64 begin;
        u8 hello[8];
        u32 got;
    } pending[TRANSFER_PENDING_MAX];
    u64 idleSince;  // When the last worker ended without the transfer whole.

    Thread threads[TRANSFER_CONNECTIONS];
    struct {
        transfer_receiver* receiver;
        transfer_link link;
        bool control;
    } args[TRANSFER_CONNECTIONS];
};

static void receiver_fail(transfer_receiver* r, Result res) {
    LightLock_Lock(&r->lock);
    if(r->result == 0 && !r->complete) {
        r->result = res;
        udslink_log("receiver: stopped with %08lX after %llu of %llu bytes\n", res, r->received, r->needed);
    }

    LightLock_Unlock(&r->lock);
}

static void receiver_worker(void* arg) {
    transfer_receiver* r = *(transfer_receiver**) arg;
    u32 slot = (u32) (((u8*) arg - (u8*) r->args) / sizeof(r->args[0]));
    transfer_link link = r->args[slot].link;
    bool control = r->args[slot].control;

    u8* buffer = (u8*) malloc(TRANSFER_CHUNK);
    Result res = buffer != NULL ? 0 : R_APP_OUT_OF_MEMORY;

    bool endSeen = false;
    while(R_SUCCEEDED(res) && !r->stop) {
        u8 record[12];
        if(R_FAILED(res = transfer_link_io(&link, false, record, sizeof(record), 0, &r->stop))) {
            if(res != R_APP_CANCELLED) {
                udslink_log("receiver: connection %u could not read the next chunk header: %08lX\n", slot, res);
            }

            break;
        }

        u64 offset = conn_get64(record);
        u32 len = conn_get32(record + 8);
        if(offset == CONN_END_OFFSET) {
            endSeen = true;
            break;
        }

        if(len == 0 || len > TRANSFER_CHUNK || offset > r->end || len > r->end - offset || (offset < r->start && offset + len > r->head)) {
            res = R_APP_BAD_DATA;
            break;
        }

        if(R_FAILED(res = transfer_link_io(&link, false, buffer, len, 0, &r->stop))) {
            if(res != R_APP_CANCELLED) {
                udslink_log("receiver: connection %u lost the data of the chunk at %llu: %08lX\n", slot, offset, res);
            }

            break;
        }

        if(R_FAILED(res = r->write(r->user, offset, buffer, len))) {
            udslink_log("receiver: writing the chunk at %llu (%u bytes) failed: %08lX\n", offset, len, res);
            break;
        }

        LightLock_Lock(&r->lock);
        r->received += len;
        if(r->received >= r->needed) {
            r->complete = true;
        }

        LightLock_Unlock(&r->lock);

        if(selftest_fault("recv.chunk")) {
            res = R_APP_IO_ERROR;
            break;
        }
    }

    // Once everything is here a connection ending early costs nothing.
    if(R_FAILED(res) && res != R_APP_CANCELLED) {
        receiver_fail(r, res);
    }

    // The end of an extra connection: say everything came, and let the sender be the
    // one to close (a close here could cut the acknowledgement short).
    if(endSeen && !control) {
        u8 ack = 1;
        if(R_SUCCEEDED(transfer_link_io(&link, true, &ack, 1, 0, &r->stop))) {
            u64 begin = osGetTime();
            while(!r->stop && osGetTime() - begin < 3000) {
                u8 byte;
                ssize_t n = recv(link.fd, &byte, 1, 0);
                if(n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    break;
                }

                svcSleepThread(10000000);
            }
        }
    }

    free(buffer);
    if(!control) {
        close(link.fd);
    }

    LightLock_Lock(&r->lock);
    r->running--;
    LightLock_Unlock(&r->lock);
}

static bool receiver_spawn(transfer_receiver* r, transfer_link link, bool control) {
    LightLock_Lock(&r->lock);
    u32 slot = r->used;
    bool room = slot < TRANSFER_CONNECTIONS;
    if(room) {
        r->used++;
        r->running++;
    }

    LightLock_Unlock(&r->lock);

    if(!room) {
        return false;
    }

    r->args[slot].receiver = r;
    r->args[slot].link = link;
    r->args[slot].control = control;
    r->threads[slot] = conn_thread(receiver_worker, &r->args[slot], slot);
    if(r->threads[slot] == NULL) {
        LightLock_Lock(&r->lock);
        r->running--;
        LightLock_Unlock(&r->lock);
        return false;
    }

    return true;
}

Result transfer_receiver_start(transfer_receiver** out, transfer_link* control, int listenSock, u32 sessionId, u64 start, u64 end, u32 head, transfer_write_fn write, void* user) {
    transfer_receiver* r = (transfer_receiver*) calloc(1, sizeof(transfer_receiver));
    if(r == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    r->control = control;
    r->listenSock = listenSock;
    r->id = sessionId;
    r->start = start;
    r->end = end;
    r->head = head;
    for(u32 i = 0; i < TRANSFER_PENDING_MAX; i++) {
        r->pending[i].fd = -1;
    }

    r->needed = end - start + (start > 0 ? head : 0);
    r->write = write;
    r->user = user;
    LightLock_Init(&r->lock);

    if(!receiver_spawn(r, *control, true)) {
        free(r);
        return R_APP_THREAD_CREATE_FAILED;
    }

    *out = r;
    return 0;
}

void transfer_receiver_accept(transfer_receiver* r) {
    if(r->listenSock < 0) {
        return;
    }

    for(u32 i = 0; i < TRANSFER_PENDING_MAX; i++) {
        if(r->pending[i].fd >= 0) {
            continue;
        }

        int sock = accept(r->listenSock, NULL, NULL);
        if(sock < 0) {
            break;
        }

        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
        transfer_conn_set_buffers(sock);

        r->pending[i].fd = sock;
        r->pending[i].begin = osGetTime();
        r->pending[i].got = 0;
    }

    // A connection becomes a worker only once it has said which transfer it is
    // for: anything else (another console's hello, a late connection of an
    // earlier attempt, a scan) is closed without touching this transfer.
    for(u32 i = 0; i < TRANSFER_PENDING_MAX; i++) {
        int sock = r->pending[i].fd;
        if(sock < 0) {
            continue;
        }

        u8* hello = r->pending[i].hello;
        u32 want = sizeof(r->pending[i].hello);
        bool closed = false;
        while(r->pending[i].got < want) {
            ssize_t n = recv(sock, hello + r->pending[i].got, want - r->pending[i].got, 0);
            if(n <= 0) {
                closed = n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
                break;
            }

            r->pending[i].got += (u32) n;
        }

        if(r->pending[i].got < want && !closed && osGetTime() - r->pending[i].begin < CONN_HELLO_TIMEOUT) {
            continue;
        }

        r->pending[i].fd = -1;

        bool ours = r->pending[i].got == want && memcmp(hello, CONN_MAGIC, 4) == 0 && conn_get32(hello + 4) == r->id;
        if(!ours) {
            udslink_log("receiver: closed a connection that is not part of this transfer\n");
            close(sock);
            continue;
        }

        transfer_link link = {sock, NULL};
        if(!receiver_spawn(r, link, false)) {
            close(sock);
        }
    }
}

bool transfer_receiver_progress(transfer_receiver* r, u64* received, Result* result) {
    // Every worker gone without the transfer whole is a failure, unless another
    // connection is still on its way (just accepted, or not yet).
    bool pending = false;
    for(u32 i = 0; i < TRANSFER_PENDING_MAX; i++) {
        pending = pending || r->pending[i].fd >= 0;
    }

    LightLock_Lock(&r->lock);
    *received = r->received;
    bool idle = false;
    if(r->running == 0 && !r->complete && !pending) {
        if(r->idleSince == 0) {
            r->idleSince = osGetTime();
        }

        idle = osGetTime() - r->idleSince > 2000;
    } else {
        r->idleSince = 0;
    }

    bool done = r->complete || idle || r->result != 0;
    *result = r->complete ? 0 : (r->result != 0 ? r->result : (idle ? R_APP_IO_ERROR : 0));
    LightLock_Unlock(&r->lock);

    return done;
}

void transfer_receiver_cancel(transfer_receiver* r) {
    receiver_fail(r, R_APP_CANCELLED);
    r->stop = true;
}

void transfer_receiver_free(transfer_receiver* r) {
    if(r == NULL) {
        return;
    }

    r->stop = true;
    for(u32 i = 0; i < TRANSFER_CONNECTIONS; i++) {
        if(r->threads[i] != NULL) {
            threadJoin(r->threads[i], U64_MAX);
            threadFree(r->threads[i]);
        }
    }

    for(u32 i = 0; i < TRANSFER_PENDING_MAX; i++) {
        if(r->pending[i].fd >= 0) {
            close(r->pending[i].fd);
        }
    }

    free(r);
}
