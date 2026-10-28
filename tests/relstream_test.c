// The reliable stream over a bad link: loss, repeated frames, reordering and a
// rate limit, checking that every byte arrives in order and how long it takes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/fbi/relstream.h"

#define QUEUE 4096

typedef struct {
    uint8_t data[RS_FRAME_MAX + 16];
    size_t len;
    uint64_t at;
} packet;

typedef struct {
    packet q[QUEUE];
    int head, count;
} wire;

typedef struct {
    wire* out;
    wire* in;
    uint64_t* clock;
    double loss;
    double dup;
    int latency;
    int jitter;
    int perMs;          // Frames the radio takes per millisecond (the cap).
    int sentThisMs;
    uint64_t sentMs;
} end;

static double rnd(void) {
    return (double) rand() / RAND_MAX;
}

static bool send_frame(void* user, const uint8_t* data, size_t len) {
    end* e = (end*) user;
    if(e->sentMs != *e->clock) {
        e->sentMs = *e->clock;
        e->sentThisMs = 0;
    }

    if(e->sentThisMs >= e->perMs || e->out->count >= QUEUE - 2) {
        return false;
    }

    e->sentThisMs++;

    for(int copy = 0; copy < (rnd() < e->dup ? 2 : 1); copy++) {
        if(rnd() < e->loss) {
            continue;
        }

        packet* p = &e->out->q[(e->out->head + e->out->count) % QUEUE];
        memcpy(p->data, data, len);
        p->len = len;
        p->at = *e->clock + e->latency + (e->jitter ? rand() % e->jitter : 0);
        e->out->count++;
    }

    return true;
}

static int recv_frame(void* user, uint8_t* buffer, size_t max) {
    end* e = (end*) user;

    // Any packet that is due, not only the first: the link reorders.
    for(int i = 0; i < e->in->count; i++) {
        packet* p = &e->in->q[(e->in->head + i) % QUEUE];
        if(p->at <= *e->clock) {
            size_t n = p->len < max ? p->len : max;
            memcpy(buffer, p->data, n);

            // Close the gap.
            for(int j = i; j > 0; j--) {
                e->in->q[(e->in->head + j) % QUEUE] = e->in->q[(e->in->head + j - 1) % QUEUE];
            }

            e->in->head = (e->in->head + 1) % QUEUE;
            e->in->count--;
            return (int) n;
        }
    }

    return 0;
}

static uint64_t now(void* user) {
    return *((end*) user)->clock;
}

typedef struct {
    double loss, dup;
    int latency, jitter, perMs;
    size_t bytes;
    const char* name;
} scenario;

static int run(const scenario* sc) {
    uint64_t clock = 1000;
    static wire ab, ba;
    memset(&ab, 0, sizeof(ab));
    memset(&ba, 0, sizeof(ba));

    end ea = {&ab, &ba, &clock, sc->loss, sc->dup, sc->latency, sc->jitter, sc->perMs, 0, 0};
    end eb = {&ba, &ab, &clock, sc->loss, sc->dup, sc->latency, sc->jitter, sc->perMs, 0, 0};

    rs_io ioa = {send_frame, recv_frame, now, &ea};
    rs_io iob = {send_frame, recv_frame, now, &eb};
    rs* a = rs_create(&ioa);
    rs* b = rs_create(&iob);

    uint8_t* source = (uint8_t*) malloc(sc->bytes);
    uint8_t* sink = (uint8_t*) malloc(sc->bytes);
    srand(7);
    for(size_t i = 0; i < sc->bytes; i++) {
        source[i] = (uint8_t) rand();
    }

    srand(11);
    size_t written = 0, got = 0;
    uint64_t start = clock;
    int ok = 1;
    bool closed = false;

    // A also gets a reply from B, to check both directions.
    const char* reply = "thanks";
    size_t replyGot = 0;
    char replyBuf[16] = {0};
    bool replied = false;

    while(clock - start < 600000) {
        if(written < sc->bytes) {
            written += rs_write(a, source + written, sc->bytes - written < 3000 ? sc->bytes - written : 3000);
        } else if(!closed) {
            rs_close(a);
            closed = true;

            // The end itself still has to go out.
            if(rs_flushed(a)) {
                printf("  FAIL %s: flushed before the end was sent\n", sc->name);
                ok = 0;
            }
        }

        rs_poll(a);
        rs_poll(b);

        got += rs_read(b, sink + got, sc->bytes - got);
        if(got == sc->bytes && !replied) {
            rs_write(b, reply, strlen(reply));
            rs_close(b);
            replied = true;
        }

        replyGot += rs_read(a, replyBuf + replyGot, sizeof(replyBuf) - 1 - replyGot);

        if(rs_peer_closed(b) && rs_peer_closed(a) && got == sc->bytes) {
            break;
        }

        clock++;
    }

    if(got != sc->bytes || memcmp(source, sink, sc->bytes) != 0) {
        printf("  FAIL %s: got %zu of %zu bytes, content %s\n", sc->name, got, sc->bytes, got == sc->bytes ? "differs" : "incomplete");
        ok = 0;
    } else if(strcmp(replyBuf, reply) != 0 || !rs_peer_closed(a) || !rs_peer_closed(b)) {
        printf("  FAIL %s: reply '%s', closed a=%d b=%d\n", sc->name, replyBuf, rs_peer_closed(a), rs_peer_closed(b));
        ok = 0;
    } else {
        rs_stats st;
        rs_get_stats(a, &st);
        double secs = (clock - start) / 1000.0;
        printf("  ok   %-34s %6.0f KiB/s  (%llu frames, %llu resent, rto %u ms)\n", sc->name, sc->bytes / 1024.0 / secs,
               (unsigned long long) st.framesSent, (unsigned long long) st.framesResent, st.rto);
    }

    free(source);
    free(sink);
    rs_free(a);
    rs_free(b);
    return ok;
}

int main(void) {
    scenario cases[] = {
        {0.00, 0.00, 3, 0, 5, 2 * 1024 * 1024, "clean link"},
        {0.30, 0.00, 3, 2, 5, 2 * 1024 * 1024, "30% loss"},
        {0.30, 0.05, 4, 8, 5, 2 * 1024 * 1024, "30% loss, repeats, reordering"},
        {0.50, 0.10, 6, 15, 5, 1 * 1024 * 1024, "50% loss, repeats, reordering"},
        {0.10, 0.00, 15, 5, 2, 2 * 1024 * 1024, "slow radio, 10% loss"},
        {0.00, 0.00, 3, 0, 5, 1000, "tiny message"},
    };

    int ok = 1;
    printf("relstream:\n");
    for(size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        ok &= run(&cases[i]);
    }

    return ok ? 0 : 1;
}
