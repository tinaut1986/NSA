#pragma once

// User settings, kept in sdmc:/nsa/settings.json.

#include "../core/i18n.h"
#include "../core/theme.h"

typedef enum {
    UPDATE_CHANNEL_STABLE,
    UPDATE_CHANNEL_BETA
} update_channel;

typedef struct {
    update_channel channel;
    bool autoUpdate;
    bool ftpServer;

    // Appearance: indices into theme.h's tables and i18n.h's languages.
    u32 theme;
    u32 accent;
    theme_night_mode night;
    language_id language;
} settings_data;

// Pushes the appearance settings to the theme and the translation layer.
void settings_apply();

void settings_load();
Result settings_save();
settings_data* settings_get();
