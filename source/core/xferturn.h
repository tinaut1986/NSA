#pragma once

#include <3ds.h>

// One data transfer at a time (the owner's rule): a download by URL, QR or the updater, an FTP upload or download, a
// file going up or down through the web page and a console to console send or receive each take the turn while they
// move data, and whatever asks while it is taken is told so at once (never waits for it). Control stays free: the FTP
// control connection, the web page's browsing, settings and buttons.
//
// With the turn, the transfer may wait in a short blocking poll() (net_poll_owner) instead of napping: nothing else
// moves data, so holding the SOC session for a few milliseconds costs only a control request that much.
//
// A turn not touched (xferturn_progress) for XFERTURN_STALE_MS is given up: a transfer that forgot to release it
// never locks the others out for good.

typedef enum {
    XFER_FTP,
    XFER_WEB,
    XFER_DOWNLOAD,
    XFER_CONSOLE,
    XFER_KINDS
} xfer_kind;

#define XFERTURN_STALE_MS 60000

// On an Old 3DS the network runs on the system core, and the share of it the app keeps for its own threads there
// (APT_SetAppCpuTimeLimit, 30% since FBI) slows every send and receive. While a turn is held the app keeps only this
// much. Measured on a 2DS, 30% -> 5%: FTP download 485 -> 650 KB/s, a URL download 560-680 -> 880-930, a URL install
// 650 -> 760; but an install from the SD card, which moves no network data, took 43 s -> 50 s, so 30% stays outside
// transfers.
#define XFERTURN_CPU_LIMIT 5

typedef struct {
    bool busy;
    xfer_kind kind;
    char name[96];
    u64 done;
    u64 total;          // 0 when not known.
    u32 kbs;            // Over the last seconds.
    u32 seconds;        // Since it began.
    bool incoming;      // Coming in to the console (xferturn_incoming), else going out.
} xferturn_status;

void xferturn_init();
// The app's usual share of the system core: a held turn lowers it to XFERTURN_CPU_LIMIT, the release restores it.
// Only for an Old 3DS (a New 3DS runs the network elsewhere); without the call the limit is left alone.
void xferturn_cpu_limits(u32 normal);

// The turn for `name` (a file name or an address); 0 when it is taken, else a ticket for the calls below.
u32 xferturn_take(xfer_kind kind, const char* name, u64 total);
// How far it is: also says the transfer is alive. A ticket that lost the turn is ignored.
void xferturn_progress(u32 ticket, u64 done);
// The size, once known (a download learns it from the server's first answer).
void xferturn_total(u32 ticket, u64 total);
// Whether the data comes in to the console (an upload to it); a take starts as going out.
void xferturn_incoming(u32 ticket, bool incoming);
void xferturn_release(u32 ticket);
// Whether `ticket` still holds the turn.
bool xferturn_holds(u32 ticket);

void xferturn_get(xferturn_status* out);
// "Busy: FTP, game.cia (45%)", in English (callers translate the start): false when nobody holds it.
bool xferturn_describe(char* out, size_t size);
const char* xferturn_kind_name(xfer_kind kind);
// The bottom screen's toast (ui_set_status) for an FTP or web transfer: what, how far and how fast; false for none.
bool xferturn_notice(char* out, size_t size, float* progress);
