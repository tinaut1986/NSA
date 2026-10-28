#include <stdlib.h>
#include <string.h>

#include "relstream.h"

#define FRAME_DATA 1
#define FRAME_ACK 2
#define FRAME_FIN 3

#define RTO_MIN 60
#define RTO_MAX 1000
#define RTO_START 200
#define SENDS_PER_POLL 24
#define ACK_DELAY_MS 12

typedef struct {
    uint32_t seq;
    uint16_t len;
    bool fin;
    bool sent;
    bool acked;
    bool resent;
    uint64_t sentAt;
    uint8_t data[RS_FRAME_MAX];
} rs_flight;

typedef struct {
    bool valid;
    bool fin;
    uint16_t len;
    uint8_t data[RS_FRAME_MAX];
} rs_slot;

struct rs {
    rs_io io;

    // Sending.
    uint8_t* sbuf;
    size_t sHead, sCount;       // The ring of bytes not framed yet.
    uint32_t una;               // The oldest frame not acknowledged.
    uint32_t next;              // The next frame to make.
    rs_flight flight[RS_WINDOW];
    bool closing;               // rs_close was called.
    bool finQueued;

    // Receiving.
    uint8_t* rbuf;
    size_t rHead, rCount;
    uint32_t recvNext;
    rs_slot slots[RS_SLOTS];
    bool finReceived;
    bool ackPending;
    unsigned ackCount;
    uint64_t ackSince;

    // Timing.
    uint64_t lastRx;
    uint64_t lastTx;
    uint32_t srtt;
    uint32_t rto;
    rs_stats stats;
};

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

rs* rs_create(const rs_io* io) {
    rs* s = (rs*) calloc(1, sizeof(rs));
    if(s == NULL) {
        return NULL;
    }

    s->io = *io;
    s->sbuf = (uint8_t*) malloc(RS_BUFFER);
    s->rbuf = (uint8_t*) malloc(RS_BUFFER);
    if(s->sbuf == NULL || s->rbuf == NULL) {
        rs_free(s);
        return NULL;
    }

    s->rto = RTO_START;
    s->lastRx = s->io.now(s->io.user);
    s->lastTx = s->lastRx;
    return s;
}

void rs_free(rs* s) {
    if(s != NULL) {
        free(s->sbuf);
        free(s->rbuf);
        free(s);
    }
}

size_t rs_write_space(const rs* s) {
    return s->closing ? 0 : RS_BUFFER - s->sCount;
}

size_t rs_write(rs* s, const void* data, size_t size) {
    size_t room = rs_write_space(s);
    if(size > room) {
        size = room;
    }

    const uint8_t* in = (const uint8_t*) data;
    for(size_t i = 0; i < size; i++) {
        s->sbuf[(s->sHead + s->sCount + i) % RS_BUFFER] = in[i];
    }

    s->sCount += size;
    return size;
}

size_t rs_read_available(const rs* s) {
    return s->rCount;
}

size_t rs_read(rs* s, void* data, size_t size) {
    if(size > s->rCount) {
        size = s->rCount;
    }

    uint8_t* out = (uint8_t*) data;
    for(size_t i = 0; i < size; i++) {
        out[i] = s->rbuf[(s->rHead + i) % RS_BUFFER];
    }

    s->rHead = (s->rHead + size) % RS_BUFFER;
    s->rCount -= size;
    return size;
}

void rs_close(rs* s) {
    s->closing = true;
}

bool rs_peer_closed(const rs* s) {
    return s->finReceived && s->rCount == 0;
}

bool rs_failed(const rs* s) {
    return s->io.now(s->io.user) - s->lastRx > RS_DEAD_MS;
}

bool rs_flushed(const rs* s) {
    // After rs_close the end has to go out (and be acknowledged) too.
    return s->sCount == 0 && s->una == s->next && (!s->closing || s->finQueued);
}

void rs_get_stats(const rs* s, rs_stats* out) {
    *out = s->stats;
    out->rto = s->rto;
}

static void send_ack(rs* s, uint64_t now) {
    uint8_t frame[9];
    frame[0] = FRAME_ACK;
    put32(frame + 1, s->recvNext);

    uint32_t bitmap = 0;
    for(unsigned i = 0; i < 32; i++) {
        uint32_t seq = s->recvNext + 1 + i;
        const rs_slot* slot = &s->slots[seq % RS_SLOTS];
        // A slot is only meaningful for the frames inside the window.
        if(slot->valid && seq - s->recvNext < RS_SLOTS) {
            bitmap |= 1u << i;
        }
    }

    put32(frame + 5, bitmap);

    if(s->io.send_frame(s->io.user, frame, sizeof(frame))) {
        s->stats.acksSent++;
        s->ackPending = false;
        s->ackCount = 0;
        s->lastTx = now;
    }
}

static void handle_ack(rs* s, const uint8_t* frame, uint64_t now) {
    uint32_t ackNext = get32(frame + 1);
    uint32_t bitmap = get32(frame + 5);

    // Cumulative part.
    while(s->una != s->next && (int32_t) (ackNext - s->una) > 0) {
        rs_flight* f = &s->flight[s->una % RS_WINDOW];
        if(f->sent && !f->resent && !f->acked) {
            uint32_t sample = (uint32_t) (now - f->sentAt);
            s->srtt = s->srtt == 0 ? sample : (s->srtt * 7 + sample) / 8;
            uint32_t rto = s->srtt * 2 + 20;
            s->rto = rto < RTO_MIN ? RTO_MIN : rto > RTO_MAX ? RTO_MAX : rto;
        }

        f->acked = true;
        s->una++;
    }

    // Selective part.
    for(unsigned i = 0; i < 32; i++) {
        if(bitmap & (1u << i)) {
            uint32_t seq = ackNext + 1 + i;
            if((int32_t) (seq - s->una) >= 0 && (int32_t) (seq - s->next) < 0) {
                s->flight[seq % RS_WINDOW].acked = true;
            }
        }
    }

    // A frame is lost when frames sent after it have been acknowledged: send it
    // again at once, without waiting for the timer. Only once it has had time to
    // be acknowledged itself.
    if(bitmap != 0) {
        int32_t highest = -1;
        for(uint32_t seq = s->una; seq != s->next; seq++) {
            if(s->flight[seq % RS_WINDOW].acked) {
                highest = (int32_t) (seq - s->una);
            }
        }

        uint32_t patience = s->srtt + 10;
        for(int32_t i = 0; i < highest; i++) {
            rs_flight* f = &s->flight[(s->una + (uint32_t) i) % RS_WINDOW];
            if(f->sent && !f->acked && now - f->sentAt > patience) {
                f->sent = false;
                f->resent = true;
            }
        }
    }
}

static void handle_data(rs* s, const uint8_t* frame, int length, bool fin, uint64_t now) {
    uint32_t seq = get32(frame + 1);
    int32_t distance = (int32_t) (seq - s->recvNext);

    if(distance >= 0 && distance < RS_SLOTS) {
        rs_slot* slot = &s->slots[seq % RS_SLOTS];
        if(!slot->valid) {
            int payload = fin ? 0 : length - 5;
            if(payload >= 0 && payload <= RS_FRAME_MAX) {
                slot->valid = true;
                slot->fin = fin;
                slot->len = (uint16_t) payload;
                memcpy(slot->data, frame + 5, (size_t) payload);
            }
        }
    }

    // In order, as far as there is room to read them.
    for(;;) {
        rs_slot* slot = &s->slots[s->recvNext % RS_SLOTS];
        if(!slot->valid || RS_BUFFER - s->rCount < slot->len) {
            break;
        }

        for(size_t i = 0; i < slot->len; i++) {
            s->rbuf[(s->rHead + s->rCount + i) % RS_BUFFER] = slot->data[i];
        }

        s->rCount += slot->len;
        if(slot->fin) {
            s->finReceived = true;
        }

        slot->valid = false;
        s->recvNext++;
    }

    if(!s->ackPending) {
        s->ackPending = true;
        s->ackSince = now;
    }

    s->ackCount++;
    // A gap is reported at once, so the other side can repair it.
    if(distance != 0) {
        s->ackCount += 2;
    }
}

void rs_poll(rs* s) {
    uint64_t now = s->io.now(s->io.user);

    // Incoming.
    uint8_t frame[RS_FRAME_MAX + 16];
    for(int i = 0; i < 64; i++) {
        int n = s->io.recv_frame(s->io.user, frame, sizeof(frame));
        if(n <= 0) {
            break;
        }

        s->lastRx = now;

        if(frame[0] == FRAME_ACK && n >= 9) {
            handle_ack(s, frame, now);
        } else if(frame[0] == FRAME_DATA && n >= 5) {
            handle_data(s, frame, n, false, now);
        } else if(frame[0] == FRAME_FIN && n >= 5) {
            handle_data(s, frame, n, true, now);
        }
    }

    // New frames from what was written.
    while(s->next - s->una < RS_WINDOW && (s->sCount > 0 || (s->closing && !s->finQueued))) {
        rs_flight* f = &s->flight[s->next % RS_WINDOW];
        memset(f, 0, offsetof(rs_flight, data));
        f->seq = s->next;

        if(s->sCount > 0) {
            size_t take = s->sCount < RS_FRAME_MAX ? s->sCount : RS_FRAME_MAX;
            for(size_t i = 0; i < take; i++) {
                f->data[i] = s->sbuf[(s->sHead + i) % RS_BUFFER];
            }

            f->len = (uint16_t) take;
            s->sHead = (s->sHead + take) % RS_BUFFER;
            s->sCount -= take;
        } else {
            f->fin = true;
            s->finQueued = true;
        }

        s->next++;
    }

    // Sending and sending again.
    int sends = 0;
    for(uint32_t seq = s->una; seq != s->next && sends < SENDS_PER_POLL; seq++) {
        rs_flight* f = &s->flight[seq % RS_WINDOW];
        if(f->acked) {
            continue;
        }

        bool resend = f->sent && now - f->sentAt > s->rto;
        if(f->sent && !resend) {
            continue;
        }

        uint8_t out[RS_FRAME_MAX + 5];
        out[0] = f->fin ? FRAME_FIN : FRAME_DATA;
        put32(out + 1, f->seq);
        memcpy(out + 5, f->data, f->len);

        if(!s->io.send_frame(s->io.user, out, 5 + (size_t) f->len)) {
            break;   // The radio is full: the rest waits.
        }

        if(f->sent) {
            f->resent = true;
        }

        if(f->resent) {
            s->stats.framesResent++;
        }

        f->sent = true;
        f->sentAt = now;
        s->stats.framesSent++;
        s->lastTx = now;
        sends++;
    }

    // Acknowledgements: after two frames, or soon, or at once for a gap; and now
    // and then when nothing else is said, so the other side knows we are here.
    if(s->ackPending && (s->ackCount >= 2 || now - s->ackSince >= ACK_DELAY_MS)) {
        send_ack(s, now);
    } else if(now - s->lastTx >= RS_KEEPALIVE_MS) {
        send_ack(s, now);
    }

    // Space is used up by the frames in flight: the oldest acknowledged ones go.
    while(s->una != s->next && s->flight[s->una % RS_WINDOW].acked) {
        s->una++;
    }
}
