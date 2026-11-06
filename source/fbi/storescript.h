#pragma once

#include <jansson.h>

// Universal-DB's install scripts (what Universal-Updater follows for each file of an app), for the Store: download the
// files, take what is needed out of a .zip or .7z, install the CIAs, move and delete. Only these steps, downloads first:
// downloadFile, extractFile, installCia, deleteFile and move.
//
// Safety: a script may only write under the SD card's own folders (never /luma, /boot.firm, /Nintendo 3DS or NSA's
// /nsa), and may only delete, move or install what it wrote itself. Anything else and the Store does not offer that file.

// Whether NSA can run `script` (the JSON array of steps).
bool storescript_supported(json_t* script);
// A plain CIA: download one .cia, install it, delete it. The Store installs those as it downloads (no room needed).
bool storescript_plain_cia(json_t* script);
// The first download's address.
const char* storescript_first_url(json_t* script);
// The .3dsx the script leaves (its file name without the extension), or "" when none.
void storescript_dsx_name(json_t* script, char* out, size_t size);

// Runs a supported script, given as JSON text: asks `question` first, then the downloads (the usual progress screen), then
// the rest (a progress bar), then installs the CIAs (the usual install screens). `done` runs at the end, `ok` when every
// step went through (the CIAs' install says for itself how it went).
void storescript_run(const char* scriptText, const char* question, void* userData, void (*done)(void* userData, bool ok));
