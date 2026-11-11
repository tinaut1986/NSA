#pragma once

#include <3ds.h>

#include "../core/data/smdh.h"

// A .3dsx installed as a title of its own in the HOME Menu (issue #46): NSA turns it into a CIA on the console. The
// program is copied into the title (its code placed at a title's fixed address, its RomFS made a title's RomFS), so it
// starts and quits like any other title and needs neither the Homebrew Launcher nor Luma's 3DSX loader; the .3dsx stays
// where it was and is not needed afterwards (making it again updates the title). The banner is a picture: one drawn
// from the icon and name, or one the user picks.

#define HOMEICON_BANNER_WIDTH 256
#define HOMEICON_BANNER_HEIGHT 128
#define HOMEICON_PATH_MAX 512

typedef struct {
    char path[HOMEICON_PATH_MAX];   // On the SD card: "/3ds/foo.3dsx".
    const u8* banner;               // 256x128, R G B A bytes, top row first; NULL: homeicon_plain_banner's.

    char name[128];                 // Out: the name the HOME Menu shows.
    u64 titleId;                    // Out.

    volatile u64 done;              // Progress, in bytes read and written.
    volatile u64 total;
    volatile bool cancel;
} homeicon_job;

// The .3dsx's SMDH; false when it has none (the title then gets its file name and NSA's icon).
bool homeicon_read_smdh(const char* path, SMDH* smdh);

// The name the title will have: the .3dsx's, or its file name.
void homeicon_name(const char* path, const SMDH* smdh, char* name, size_t size);

// The icon's main colour, for the banner's background (`smdh` may be NULL).
u32 homeicon_icon_color(const SMDH* smdh);

// A banner without text, drawn on the CPU: the icon on its colour. For the tests, and when drawing one fails.
void homeicon_plain_banner(const SMDH* smdh, u8* rgba);

// A picture (PNG, JPEG, BMP...) fitted into the banner: shrunk to fit whole, on its own average colour. Refuses files
// and pictures too big to decode safely; R_APP_BAD_DATA when it is not a picture.
Result homeicon_picture_banner(const char* path, u8* rgba);

// Makes the CIA and installs it (on a task's thread; `job->cancel` stops it).
Result homeicon_install(homeicon_job* job);

// The same CIA written to `out` ("sdmc:/..."), not installed: for the tests (DEBUG "SITE HOMEICON").
Result homeicon_save(homeicon_job* job, const char* out);
