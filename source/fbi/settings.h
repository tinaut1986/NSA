#pragma once

// User settings, kept in sdmc:/nsa/settings.json.

typedef enum {
    UPDATE_CHANNEL_STABLE,
    UPDATE_CHANNEL_BETA
} update_channel;

typedef struct {
    update_channel channel;
    bool autoUpdate;
} settings_data;

void settings_load();
Result settings_save();
settings_data* settings_get();
