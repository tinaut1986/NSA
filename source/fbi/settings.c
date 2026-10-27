#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <jansson.h>

#include "settings.h"
#include "../core/core.h"

#define SETTINGS_PATH "sdmc:" APP_DATA_DIR "settings.json"

static settings_data settings = {
        .channel = UPDATE_CHANNEL_STABLE,
        .autoUpdate = true,
        .ftpServer = NSA_DEBUG,
        .theme = 0,
        .accent = 0,
        .night = THEME_NIGHT_OFF,
        .language = LANGUAGE_SYSTEM
};

static const char* night_ids[] = {"off", "on", "auto"};
static const char* language_ids[] = {"system", "en", "es"};

static u32 settings_find(const char** ids, u32 count, const char* id) {
    for(u32 i = 0; i < count; i++) {
        if(strcmp(ids[i], id) == 0) {
            return i;
        }
    }

    return 0;
}

void settings_load() {
    json_error_t err;
    json_t* json = json_load_file(SETTINGS_PATH, 0, &err);
    if(!json_is_object(json)) {
        json_decref(json);
        return;
    }

    json_t* channel = json_object_get(json, "channel");
    if(json_is_string(channel)) {
        settings.channel = strcmp(json_string_value(channel), "beta") == 0 ? UPDATE_CHANNEL_BETA : UPDATE_CHANNEL_STABLE;
    }

    json_t* autoUpdate = json_object_get(json, "auto_update");
    if(json_is_boolean(autoUpdate)) {
        settings.autoUpdate = json_is_true(autoUpdate);
    }

    json_t* ftpServer = json_object_get(json, "ftp_server");
    if(json_is_boolean(ftpServer)) {
        settings.ftpServer = json_is_true(ftpServer);
    }

    json_t* theme = json_object_get(json, "theme");
    if(json_is_string(theme)) {
        settings.theme = theme_find(json_string_value(theme));
    }

    json_t* accent = json_object_get(json, "accent");
    if(json_is_string(accent)) {
        settings.accent = theme_accent_find(json_string_value(accent));
    }

    json_t* night = json_object_get(json, "night_mode");
    if(json_is_string(night)) {
        settings.night = (theme_night_mode) settings_find(night_ids, 3, json_string_value(night));
    }

    json_t* language = json_object_get(json, "language");
    if(json_is_string(language)) {
        settings.language = (language_id) settings_find(language_ids, LANGUAGE_COUNT, json_string_value(language));
    }

    json_decref(json);
}

void settings_apply() {
    theme_select(settings.theme, settings.accent, settings.night);
    i18n_set_language(settings.language);
}

Result settings_save() {
    Result res = 0;

    FS_Archive sdmcArchive = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        res = fs_ensure_dir(sdmcArchive, APP_DATA_DIR);
        FSUSER_CloseArchive(sdmcArchive);
    }

    if(R_FAILED(res)) {
        return res;
    }

    json_t* json = json_pack("{s:s,s:b,s:b,s:s,s:s,s:s,s:s}",
                             "channel", settings.channel == UPDATE_CHANNEL_BETA ? "beta" : "stable",
                             "auto_update", settings.autoUpdate,
                             "ftp_server", settings.ftpServer,
                             "theme", theme_get(settings.theme)->id,
                             "accent", theme_accent_get(settings.accent)->id,
                             "night_mode", night_ids[settings.night < 3 ? settings.night : 0],
                             "language", language_ids[settings.language < LANGUAGE_COUNT ? settings.language : 0]);
    if(json == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    if(json_dump_file(json, SETTINGS_PATH, JSON_INDENT(2)) != 0) {
        res = R_APP_IO_ERROR;
    }

    json_decref(json);
    return res;
}

settings_data* settings_get() {
    return &settings;
}
