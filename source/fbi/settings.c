#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <jansson.h>

#include "settings.h"
#include "../core/core.h"

#define SETTINGS_PATH "sdmc:" APP_DATA_DIR "settings.json"

static settings_data settings = {
        .channel = UPDATE_CHANNEL_STABLE,
        .autoUpdate = true
};

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

    json_decref(json);
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

    json_t* json = json_pack("{s:s,s:b}",
                             "channel", settings.channel == UPDATE_CHANNEL_BETA ? "beta" : "stable",
                             "auto_update", settings.autoUpdate);
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
