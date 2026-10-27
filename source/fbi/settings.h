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
    update_channel channel;
    bool autoUpdate;
    bool ftpServer;

    // Appearance: indices into theme.h's tables and i18n.h's languages.
    u32 theme;
    u32 accent;
    theme_night_mode night;
    language_id language;

    titles_settings titles;
    tickets_settings tickets;
    files_settings files;
    extsave_settings extsave;
} settings_data;

// Pushes the appearance settings to the theme and the translation layer.
void settings_apply();

void settings_load();
Result settings_save();
settings_data* settings_get();
