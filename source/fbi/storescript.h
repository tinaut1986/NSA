#pragma once

#include <jansson.h>

// Universal-DB's install scripts (what Universal-Updater follows for each file of an app), for the Store: download the
// files, take what is needed out of a .zip or .7z, install the CIAs, move and delete. Only these steps, downloads first:
// downloadFile, extractFile, installCia, deleteFile and move.
//
// Safety: a script never writes to /Nintendo 3DS or NSA's /nsa, and may only delete, move or install what it wrote
// itself; anything else and the Store does not offer that file. Where the console starts from (boot.firm, every .firm)
// and Luma's, GodMode9's and /private's folders (but a game's /luma/titles/<id> and /luma/plugins) it writes only after
// a warning the user says yes to.

// Whether NSA can run `script` (the JSON array of steps).
bool storescript_supported(json_t* script);
// A plain CIA: download one .cia, install it, delete it. The Store installs those as it downloads (no room needed).
bool storescript_plain_cia(json_t* script);
// The first download's address.
const char* storescript_first_url(json_t* script);
// The .3dsx the script leaves (its file name without the extension), or "" when none.
void storescript_dsx_name(json_t* script, char* out, size_t size);
// A path as the catalogue writes it ("%3DSX%/app.3dsx", "sdmc:/luma/plugins") as a path on the card ("/3ds/app.3dsx");
// false for one NSA cannot place.
bool storescript_resolve_path(const char* in, char* out, size_t size);

// Runs a supported script, given as JSON text: asks `question` first, then the downloads (the usual progress screen), then
// the rest (a progress bar), then installs the CIAs (the usual install screens). `done` runs at the end, `ok` when every
// step went through (the CIAs' install says for itself how it went).
// After a run that went through: the last file it wrote that is still on the card (not a CIA it installs), as
// "/luma/plugins/.../plugin.3gx", or "" when none. Kept until the next run ends.
const char* storescript_kept_file(void);
void storescript_run(const char* scriptText, const char* question, void* userData, void (*done)(void* userData, bool ok));
