#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "xferturn.h"
#include "stringutil.h"
#include "i18n.h"
#include "ui/ui.h"

static LightLock xferturn_lock;
static xfer_kind xferturn_want_kind;
static char xferturn_want_name[96];
static u64 xferturn_want_at;
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
    bool incoming;
} xferturn;

// The share of the system core the app keeps (APT_SetAppCpuTimeLimit) while it holds no turn; 0 leaves it alone.
static u32 xferturn_cpu_normal = 0;

void xferturn_init() {
    LightLock_Init(&xferturn_lock);
}

void xferturn_cpu_limits(u32 normal) {
    xferturn_cpu_normal = normal;
}

// Under the lock, so a release and the next take cannot set the limits out of order.
static void xferturn_cpu(bool transfer) {
    if(xferturn_cpu_normal != 0) {
        APT_SetAppCpuTimeLimit(transfer ? XFERTURN_CPU_LIMIT : xferturn_cpu_normal);
    }
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
        xferturn.incoming = false;
        xferturn_cpu(true);
    } else if(kind == XFER_FTP || kind == XFER_WEB) {
        xferturn_want_kind = kind;
        string_copy(xferturn_want_name, name != NULL ? name : "", sizeof(xferturn_want_name));
        xferturn_want_at = now;
    }
    LightLock_Unlock(&xferturn_lock);

    return ticket;
}

bool xferturn_wanted(xfer_kind* kind, char* name, size_t size) {
    u64 now = osGetTime();
    bool wanted = false;
    LightLock_Lock(&xferturn_lock);
    if(xferturn_want_at != 0 && now - xferturn_want_at < 5000) {
        wanted = true;
        *kind = xferturn_want_kind;
        string_copy(name, xferturn_want_name, size);
    }

    xferturn_want_at = 0;
    LightLock_Unlock(&xferturn_lock);
    return wanted;
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

void xferturn_total(u32 ticket, u64 total) {
    if(ticket == 0) {
        return;
    }

    LightLock_Lock(&xferturn_lock);
    if(xferturn.ticket == ticket) {
        xferturn.total = total;
    }
    LightLock_Unlock(&xferturn_lock);
}

void xferturn_incoming(u32 ticket, bool incoming) {
    if(ticket == 0) {
        return;
    }

    LightLock_Lock(&xferturn_lock);
    if(xferturn.ticket == ticket) {
        xferturn.incoming = incoming;
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
        xferturn_cpu(false);
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
        out->incoming = xferturn.incoming;
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
        u32 hundredths = status.done >= status.total ? 10000 : (u32) (status.done * 10000 / status.total);
        snprintf(out, size, "Busy: %s, %s (%lu.%02lu%%)", xferturn_kind_name(status.kind), name, hundredths / 100, hundredths % 100);
    } else {
        snprintf(out, size, "Busy: %s, %s", xferturn_kind_name(status.kind), name);
    }

    return true;
}

// A bottom screen toast for what comes in or goes out over FTP or the web page, which have no screen of their own
// on the console, on two lines: "FTP, receiving: 3.2 / 12.0 MiB, 650 KiB/s", then "00:00:14 left · name" (the time
// left only when the size is known, from the average speed since the start). The bar shows how far when the size is
// known; when it is not (FileZilla does not say it before an upload) a piece of it moves.
bool xferturn_notice(char* out, size_t size, float* progress) {
    xferturn_status status;
    xferturn_get(&status);
    if(!status.busy || (status.kind != XFER_FTP && status.kind != XFER_WEB)) {
        return false;
    }

    const char* name = strrchr(status.name, '/');
    name = name != NULL && name[1] != '\0' ? name + 1 : status.name;

    const char* what = status.kind == XFER_FTP ? (status.incoming ? _("FTP, receiving") : _("FTP, sending"))
                                               : (status.incoming ? _("Web, receiving") : _("Web, sending"));

    // How much so far (and of how much), then the speed.
    char figures[80] = {'\0'};
    size_t used = snprintf(figures, sizeof(figures), "%.1f %s", ui_get_display_size(status.done), ui_get_display_size_units(status.done));
    if(status.total > 0 && used < sizeof(figures)) {
        used += snprintf(figures + used, sizeof(figures) - used, " / %.1f %s", ui_get_display_size(status.total), ui_get_display_size_units(status.total));
    }

    u64 bps = (u64) status.kbs * 1024;
    if(status.kbs > 0 && used < sizeof(figures)) {
        snprintf(figures + used, sizeof(figures) - used, ", %.1f %s/s", ui_get_display_size(bps), ui_get_display_size_units(bps));
    }

    // The time left, from the average since the start: the last seconds' speed would make it jump.
    char eta[48] = {'\0'};
    if(status.total > status.done && status.done > 0 && status.seconds >= 2) {
        u64 seconds = (status.total - status.done) * status.seconds / status.done;
        snprintf(eta, sizeof(eta), _("%s left"), ui_get_display_eta(seconds > 359999 ? 359999 : (u32) seconds));
    }

    // The name last: a long one is what the buffer or the card cuts.
    if(eta[0] != '\0') {
        snprintf(out, size, "%s: %s\n%s \u00B7 %s", what, figures, eta, name);
    } else {
        snprintf(out, size, "%s: %s\n%s", what, figures, name);
    }
    *progress = status.total > 0 ? (float) ((double) status.done / (double) status.total) : WIDGET_PROGRESS_UNKNOWN;
    return true;
}
