#pragma once

// Who keeps the console out of sleep mode. aptSetSleepAllowed is a single switch, so two users (a data operation
// and the servers, say) would turn it on and off under each other: each takes a hold instead, and sleep is allowed
// again only when none is left (libctru then sleeps at once if the lid is already closed).
//
// While sleep is held off the lid no longer puts the console to sleep: awake_update turns the screens' backlight off
// while it is closed, as ftpd does, and on again when it opens or the hold goes. AWAKE_SERVERS also locks ndm (the
// network daemon) in infrastructure mode, or the lid would still take the Wi-Fi: release it before the local wireless.
typedef enum {
    AWAKE_DATA_OP,   // An install, a copy, a delete... (task/dataop.c).
    AWAKE_DUMP,      // Dump NAND.
    AWAKE_MOTION,    // The motion sensors' recording (hwtest.c).
    AWAKE_SERVERS,   // The FTP or web server with Settings -> "Servers with the lid closed" on.
    AWAKE_COUNT
} awake_reason;

// Thread-safe; a reason held twice is still one hold.
void awake_hold(awake_reason reason, bool hold);
bool awake_held();

// From the main loop, once a frame: the lid and the backlight.
void awake_update();
// The backlight on again (before sleep, HOME or the exit).
void awake_backlight_restore();
