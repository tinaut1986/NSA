#pragma once

// User settings, kept in sdmc:/nsa/settings.json.

#include "../core/i18n.h"
#include "../core/theme.h"

typedef enum {
    UPDATE_CHANNEL_STABLE,
    UPDATE_CHANNEL_BETA
} update_channel;

// What each screen showed last time (filters, sort, view), restored when it
// opens again.
typedef struct {
    u32 media;       // Bits: 1 SD, 2 NAND, 4 game card.
    u32 kinds;       // One bit per title_kind.
    u32 regions;     // One bit per region.
    bool group;
    u32 sort;
    bool grid;
    u32 tile;        // Size of the tiles of the grid: a level of list_tile_size_of.
} titles_settings;

typedef struct {
    u32 states;      // Bits: 1 in use, 2 not in use.
    u32 kinds;       // One bit per title_kind.
    u32 media;       // Bits: 1 SD, 2 NAND.
    bool group;
    u32 sort;
    bool grid;
} tickets_settings;

typedef struct {
    bool directories;
    bool files;
    bool cias;
    bool tickets;
    bool hidden;
    u32 sort;
} files_settings;

typedef struct {
    bool sd;
    bool nand;
    bool byId;
    bool grid;
} extsave_settings;

typedef struct {
    bool sd;
    bool nand;
} pending_settings;

typedef struct {
    u32 states;      // Bits: 1 the game is on the console (installed, its card or its update), 2 it is not, 4 empty folders.
    u32 sort;        // 0 name, 1 size, 2 ID.
    bool grid;
} mods_settings;

typedef struct {
    u32 kinds;        // Bits: 1 CIA, 2 3DSX.
    bool others;      // What is neither (files for other consoles, mods, skins...) shows too.
    u32 categories;   // One bit per category the Store knows (the last one is "other").
    u32 sort;
    bool sortReverse; // The sort turned round (choosing the same sort again).
    bool grid;
    u32 tile;
    u32 shown;        // 0 all, 1 what is on the console (installed or downloaded), 2 what is not.
    u32 updated;      // 0 any time, else updated in the last month, 6 months or year (store.c's store_updated_days).
} store_settings;

typedef struct {
    update_channel channel;
    bool autoUpdate;
    bool ftpServer;
    bool webServer;   // The web page for a computer's browser: off until the owner turns it on.
    // The background music stops while a file goes through the FTP server: 2-3% faster on an Old
    // 3DS since transfers keep only 5% of the system core (10-25% before), 3-6% on a New 3DS. Off by default.
    bool ftpPausesMusic;
    // An Old 3DS that had ftpPausesMusic on (its old default) was asked once whether to let the music play.
    bool ftpMusicAsked;

    // Appearance: indices into theme.h's tables and i18n.h's languages.
    u32 theme;
    u32 accent;
    theme_night_mode night;
    language_id language;

    // Interface sounds: a sound_mode and a volume in percent.
    u32 sounds;
    u32 soundVolume;
    // Background music: a bgmusic_style and a volume in percent.
    u32 music;
    u32 musicVolume;

    // The main menu's layout: the ids of the entries shown in order, then "|" and the hidden ones; "" for the default.
    char menu[128];
    // Remote install's four tiles in order, by id ("3ds,pc,qr,url"); "" for the default.
    char remoteMenu[32];

    // The guide (guide.h): the ids of the chapters seen ("basics,titles"), and the newest chapter version this
    // console was offered (0 on a first start; consoles that ran NSA before the guide start at v0.7).
    char guideSeen[256];
    u32 guideKnown;

    titles_settings titles;
    tickets_settings tickets;
    files_settings files;
    extsave_settings extsave;
    pending_settings pending;
    mods_settings mods;
    store_settings store;
} settings_data;

// Pushes the appearance settings to the theme and the translation layer.
void settings_apply();

void settings_load();
Result settings_save();
settings_data* settings_get();
