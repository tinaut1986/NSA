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
        .ftpServer = true,   // Serving files is on by default; driving the console needs sdmc:/nsa/remote.flag.
        .theme = 0,
        .accent = 0,
        .night = THEME_NIGHT_OFF,
        .sounds = SOUNDS_ALL,
        .soundVolume = 60,
        .language = LANGUAGE_SYSTEM,
        .titles = {.media = 7, .kinds = 0x3F, .regions = 0x1F, .group = true, .sort = 0, .grid = false},
        .tickets = {.states = 3, .kinds = 0x3F, .media = 3, .group = true, .sort = 0, .grid = false},
        .files = {.directories = true, .files = true, .cias = true, .tickets = true, .hidden = false, .sort = 0},
        .extsave = {.sd = true, .nand = true, .byId = false, .grid = false},
        .pending = {.sd = true, .nand = true}
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

static bool settings_get_bool(json_t* object, const char* key, bool fallback) {
    json_t* value = json_object_get(object, key);
    return json_is_boolean(value) ? json_is_true(value) : fallback;
}

static u32 settings_get_uint(json_t* object, const char* key, u32 fallback, u32 max) {
    json_t* value = json_object_get(object, key);
    if(!json_is_integer(value)) {
        return fallback;
    }

    json_int_t number = json_integer_value(value);
    return number >= 0 && number <= (json_int_t) max ? (u32) number : fallback;
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

    settings.sounds = settings_get_uint(json, "sounds", settings.sounds, SOUNDS_ALL);
    settings.soundVolume = settings_get_uint(json, "sound_volume", settings.soundVolume, 100);

    json_t* night = json_object_get(json, "night_mode");
    if(json_is_string(night)) {
        settings.night = (theme_night_mode) settings_find(night_ids, 3, json_string_value(night));
    }

    json_t* language = json_object_get(json, "language");
    if(json_is_string(language)) {
        settings.language = (language_id) settings_find(language_ids, LANGUAGE_COUNT, json_string_value(language));
    }

    json_t* titles = json_object_get(json, "titles");
    if(json_is_object(titles)) {
        settings.titles.media = settings_get_uint(titles, "media", settings.titles.media, 7);
        settings.titles.kinds = settings_get_uint(titles, "kinds", settings.titles.kinds, 0x3F);
        settings.titles.regions = settings_get_uint(titles, "regions", settings.titles.regions, 0x1F);
        settings.titles.group = settings_get_bool(titles, "group", settings.titles.group);
        settings.titles.sort = settings_get_uint(titles, "sort", settings.titles.sort, 2);
        settings.titles.grid = settings_get_bool(titles, "grid", settings.titles.grid);
    }

    json_t* tickets = json_object_get(json, "tickets");
    if(json_is_object(tickets)) {
        settings.tickets.states = settings_get_uint(tickets, "states", settings.tickets.states, 3);
        settings.tickets.kinds = settings_get_uint(tickets, "kinds", settings.tickets.kinds, 0x3F);
        settings.tickets.media = settings_get_uint(tickets, "media", settings.tickets.media, 3);
        settings.tickets.group = settings_get_bool(tickets, "group", settings.tickets.group);
        settings.tickets.sort = settings_get_uint(tickets, "sort", settings.tickets.sort, 1);
        settings.tickets.grid = settings_get_bool(tickets, "grid", settings.tickets.grid);
    }

    json_t* files = json_object_get(json, "files");
    if(json_is_object(files)) {
        settings.files.directories = settings_get_bool(files, "directories", settings.files.directories);
        settings.files.files = settings_get_bool(files, "files", settings.files.files);
        settings.files.cias = settings_get_bool(files, "cias", settings.files.cias);
        settings.files.tickets = settings_get_bool(files, "tickets", settings.files.tickets);
        settings.files.hidden = settings_get_bool(files, "hidden", settings.files.hidden);
        settings.files.sort = settings_get_uint(files, "sort", settings.files.sort, 2);
    }

    json_t* extsave = json_object_get(json, "extsave");
    if(json_is_object(extsave)) {
        settings.extsave.sd = settings_get_bool(extsave, "sd", settings.extsave.sd);
        settings.extsave.nand = settings_get_bool(extsave, "nand", settings.extsave.nand);
        settings.extsave.byId = settings_get_bool(extsave, "by_id", settings.extsave.byId);
        settings.extsave.grid = settings_get_bool(extsave, "grid", settings.extsave.grid);
    }

    json_t* pending = json_object_get(json, "pending");
    if(json_is_object(pending)) {
        settings.pending.sd = settings_get_bool(pending, "sd", settings.pending.sd);
        settings.pending.nand = settings_get_bool(pending, "nand", settings.pending.nand);
    }

    json_decref(json);
}

void settings_apply() {
    theme_select(settings.theme, settings.accent, settings.night);
    i18n_set_language(settings.language);
    sound_set((sound_mode) settings.sounds, settings.soundVolume);
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

    json_t* json = json_pack("{s:s,s:b,s:b,s:s,s:s,s:s,s:s,s:i,s:i,s:{s:i,s:i,s:i,s:b,s:i,s:b},s:{s:i,s:i,s:i,s:b,s:i,s:b},s:{s:b,s:b,s:b,s:b,s:b,s:i},s:{s:b,s:b,s:b,s:b},s:{s:b,s:b}}",
                             "channel", settings.channel == UPDATE_CHANNEL_BETA ? "beta" : "stable",
                             "auto_update", settings.autoUpdate,
                             "ftp_server", settings.ftpServer,
                             "theme", theme_get(settings.theme)->id,
                             "accent", theme_accent_get(settings.accent)->id,
                             "night_mode", night_ids[settings.night < 3 ? settings.night : 0],
                             "language", language_ids[settings.language < LANGUAGE_COUNT ? settings.language : 0],
                             "sounds", (int) settings.sounds,
                             "sound_volume", (int) settings.soundVolume,
                             "titles",
                             "media", (int) settings.titles.media,
                             "kinds", (int) settings.titles.kinds,
                             "regions", (int) settings.titles.regions,
                             "group", settings.titles.group,
                             "sort", (int) settings.titles.sort,
                             "grid", settings.titles.grid,
                             "tickets",
                             "states", (int) settings.tickets.states,
                             "kinds", (int) settings.tickets.kinds,
                             "media", (int) settings.tickets.media,
                             "group", settings.tickets.group,
                             "sort", (int) settings.tickets.sort,
                             "grid", settings.tickets.grid,
                             "files",
                             "directories", settings.files.directories,
                             "files", settings.files.files,
                             "cias", settings.files.cias,
                             "tickets", settings.files.tickets,
                             "hidden", settings.files.hidden,
                             "sort", (int) settings.files.sort,
                             "extsave",
                             "sd", settings.extsave.sd,
                             "nand", settings.extsave.nand,
                             "by_id", settings.extsave.byId,
                             "grid", settings.extsave.grid,
                             "pending",
                             "sd", settings.pending.sd,
                             "nand", settings.pending.nand);
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
