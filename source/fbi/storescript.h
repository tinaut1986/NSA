#pragma once

#include <jansson.h>

// Universal-DB's install scripts (what Universal-Updater follows for each file of an app), for the Store: download the
// files, take what is needed out of a .zip or .7z, install the CIAs, copy, move and delete, as Universal-Updater does:
// downloadFile, downloadRelease, extractFile, installCia, deleteFile, move, copy, mkdir, rmdir, promptMessage, skip and
// exit (bootTitle is taken as done, without starting the title).
//
// Safety: a script never writes to or deletes from /Nintendo 3DS or NSA's /nsa; anything else and the Store does not offer
// that file. Where the console starts from (boot.firm, every .firm) and Luma's, GodMode9's and /private's folders (but a
// game's /luma/titles/<id> and /luma/plugins) it touches only after a warning the user says yes to; a download over plain
// http gets a warning too.

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
// The first question also offers "Add to the download queue" (issue #41): then, past the warnings, everything the script
// will ask (its own yes/no messages, each folder to delete) is asked at once, and the run goes to the queue (dlqueue.c) as
// `name`, to run later with those answers and no screen.
void storescript_run(const char* scriptText, const char* question, const char* name, void* userData, void (*done)(void* userData, bool ok));

// The download queue's side of a queued run (`run` as dlqueue_add_script got it). Start runs it from where it stands (the
// start, or where a pause left it); `done` says how it ended (the run is gone then, but after a pause). Poll moves it on
// from the main loop. Stop pauses it between two steps (a download under way starts again on resume) or cancels it.
// Discard lets go of a run that is not running (its `done` for the Store runs, not the queue's).
bool storescript_queued_start(void* run, void (*done)(void* item, u32 ok, u32 failed, bool paused, bool cancelled, const char* error), void* item);
void storescript_queued_poll(void* run);
void storescript_queued_stop(void* run, bool pause);
void storescript_queued_discard(void* run);
void storescript_queued_open(void* run);
bool storescript_queued_on_screen(void* run);
void storescript_queued_progress(void* run, u64* done, u64* total, char* name, size_t size);
