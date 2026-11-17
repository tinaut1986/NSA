#pragma once

#include "wifislots.h"

// Edits a Wi-Fi connection (`connection` 0-2, `backup` NULL) or a saved copy (`connection` -1), starting from `start`
// (NULL or empty: a new connection set up by hand). `saved` is called once the changes are written.
// Connects with connection `index` while the console is not connected, showing it, then says how it went after
// `before` (the result of what came first, or empty). wificonnections.c.
void wificonnections_connect(u32 index, const char* before);

void wifiedit_open(const wifislots_slot* start, int connection, wifislots_backup* backup, void (*saved)(void* data), void* data);
// Edits connection `connection` set up for another network (from a QR code): its name, security and password filled,
// the addresses automatic. `current` is what the connection holds now: saving asks, red, before replacing it.
void wifiedit_open_network(u32 connection, const wifislots_slot* current, const char* ssid, u8 security, const char* password,
                           void (*saved)(void* data), void* data);
