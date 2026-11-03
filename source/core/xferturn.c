#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "xferturn.h"
#include "stringutil.h"

static LightLock xferturn_lock;
static u32 xferturn_next = 0;

static struct {
    u32 ticket;             // 0: nobody holds the turn.
    xfer_kind kind;
    char name[96];
    u64 done;
    u64 total;
    u64 started;
    u64 touched;            // The last sign of life.
    u64 rateTime;           // The speed: bytes since rateTime, measured every 2 s.
    u64 rateDone;
    u32 kbs;
} xferturn;

void xferturn_init() {
    LightLock_Init(&xferturn_lock);
}

// Under the lock: a turn left untouched for too long is free again.
static bool xferturn_taken(u64 now) {
    return xferturn.ticket != 0 && now - xferturn.touched < XFERTURN_STALE_MS;
}

u32 xferturn_take(xfer_kind kind, const char* name, u64 total) {
    u64 now = osGetTime();
    u32 ticket = 0;

    LightLock_Lock(&xferturn_lock);
    if(!xferturn_taken(now)) {
        if(++xferturn_next == 0) {
            xferturn_next = 1;
        }

        ticket = xferturn_next;
        xferturn.ticket = ticket;
        xferturn.kind = kind;
        string_copy(xferturn.name, name != NULL ? name : "", sizeof(xferturn.name));
        xferturn.done = 0;
        xferturn.total = total;
        xferturn.started = xferturn.touched = xferturn.rateTime = now;
        xferturn.rateDone = 0;
        xferturn.kbs = 0;
    }
    LightLock_Unlock(&xferturn_lock);

    return ticket;
}

void xferturn_progress(u32 ticket, u64 done) {
    if(ticket == 0) {
        return;
    }

    u64 now = osGetTime();
    LightLock_Lock(&xferturn_lock);
    if(xferturn.ticket == ticket) {
        xferturn.done = done;
        xferturn.touched = now;
        if(now - xferturn.rateTime >= 2000) {
            xferturn.kbs = done > xferturn.rateDone ? (u32) ((done - xferturn.rateDone) * 1000 / (now - xferturn.rateTime) / 1024) : 0;
            xferturn.rateTime = now;
            xferturn.rateDone = done;
        }
    }
    LightLock_Unlock(&xferturn_lock);
}

void xferturn_release(u32 ticket) {
    if(ticket == 0) {
        return;
    }

    LightLock_Lock(&xferturn_lock);
    if(xferturn.ticket == ticket) {
        xferturn.ticket = 0;
    }
    LightLock_Unlock(&xferturn_lock);
}

bool xferturn_holds(u32 ticket) {
    if(ticket == 0) {
        return false;
    }

    LightLock_Lock(&xferturn_lock);
    bool holds = xferturn.ticket == ticket && xferturn_taken(osGetTime());
    LightLock_Unlock(&xferturn_lock);
    return holds;
}

void xferturn_get(xferturn_status* out) {
    u64 now = osGetTime();
    memset(out, 0, sizeof(*out));

    LightLock_Lock(&xferturn_lock);
    if(xferturn_taken(now)) {
        out->busy = true;
        out->kind = xferturn.kind;
        string_copy(out->name, xferturn.name, sizeof(out->name));
        out->done = xferturn.done;
        out->total = xferturn.total;
        out->kbs = xferturn.kbs;
        out->seconds = (u32) ((now - xferturn.started) / 1000);
    }
    LightLock_Unlock(&xferturn_lock);
}

const char* xferturn_kind_name(xfer_kind kind) {
    static const char* names[XFER_KINDS] = {"FTP", "web page", "download", "another 3DS"};
    return kind < XFER_KINDS ? names[kind] : "transfer";
}

bool xferturn_describe(char* out, size_t size) {
    xferturn_status status;
    xferturn_get(&status);
    if(!status.busy) {
        out[0] = '\0';
        return false;
    }

    const char* name = strrchr(status.name, '/');
    name = name != NULL && name[1] != '\0' ? name + 1 : status.name;
    if(status.total > 0) {
        snprintf(out, size, "Busy: %s, %s (%u%%)", xferturn_kind_name(status.kind), name, (unsigned) (status.done * 100 / status.total));
    } else {
        snprintf(out, size, "Busy: %s, %s", xferturn_kind_name(status.kind), name);
    }

    return true;
}
