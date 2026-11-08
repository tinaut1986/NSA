#pragma once

#include "wifislots.h"

// Edits a Wi-Fi connection (`connection` 0-2, `backup` NULL) or a saved copy (`connection` -1), starting from `start`
// (NULL or empty: a new connection set up by hand). `saved` is called once the changes are written.
void wifiedit_open(const wifislots_slot* start, int connection, wifislots_backup* backup, void (*saved)(void* data), void* data);
