#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "uitask.h"
#include "listmods.h"
#include "../resources.h"
#include "../../core/core.h"

// Mods for games in /luma/titles: one folder per title ID, which Luma3DS loads into that game alone. A folder is left
// behind when its game is deleted; this finds whether each one's game is still on the console.

static bool task_mods_parse_id(const char* name, u64* titleId) {
    if(strlen(name) != 16) {
        return false;
    }

    u64 id = 0;
    for(u32 i = 0; i < 16; i++) {
        char c = name[i];
        u32 digit = c >= '0' && c <= '9' ? (u32) (c - '0') : c >= 'a' && c <= 'f' ? (u32) (c - 'a' + 10) : c >= 'A' && c <= 'F' ? (u32) (c - 'A' + 10) : 16;
        if(digit > 15) {
            return false;
        }

        id = (id << 4) | digit;
    }

    *titleId = id;
    return true;
}

static void task_mods_path(char* out, size_t size, u64 titleId) {
    snprintf(out, size, MODS_ROOT "%016llX/", titleId);
}

bool task_mod_unused(const mod_info* info) {
    return info->ready && info->state == MOD_STATE_NOT_INSTALLED && title_kind_of(info->titleId) != TITLE_KIND_SYSTEM;
}

void task_mod_contents_text(char* out, size_t size, u32 contents) {
    static const struct {
        u32 bit;
        const char* name;
    } names[] = {
            {MOD_CONTENT_ROMFS, "RomFS"},
            {MOD_CONTENT_CODE, "Code patch"},
            {MOD_CONTENT_EXHEADER, "ExHeader"},
            {MOD_CONTENT_LOCALE, "Locale"},
            {MOD_CONTENT_OTHER, "Other files"},
    };

    out[0] = '\0';
    size_t len = 0;
    for(u32 i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if(contents & names[i].bit) {
            len += snprintf(out + len, len < size ? size - len : 0, "%s%s", len > 0 ? ", " : "", i18n_translate_text(names[i].name));
        }
    }

    if(len == 0) {
        snprintf(out, size, "%s", _("Empty"));
    }
}

// Each folder can be checked on the SD card's own name (the ID in any case: Luma reads it in upper case, and FAT
// does not mind).
bool task_mods_exist(u64 titleId) {
    char path[64];
    task_mods_path(path, sizeof(path), titleId);

    bool exists = false;
    FS_Archive sd = 0;
    if(R_SUCCEEDED(fs_open_archive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        FS_Path* fsPath = fs_make_path_utf8(path);
        if(fsPath != NULL) {
            Handle dir = 0;
            if(R_SUCCEEDED(FSUSER_OpenDirectory(&dir, sd, *fsPath))) {
                exists = true;
                FSDIR_Close(dir);
            }

            fs_free_path_utf8(fsPath);
        }

        fs_close_archive(sd);
    }

    return exists;
}

Result task_mods_delete(u64 titleId) {
    // Built from the ID alone: never anything in /luma but a title's own folder.
    char path[64];
    task_mods_path(path, sizeof(path), titleId);

    Result res = 0;
    FS_Archive sd = 0;
    if(R_SUCCEEDED(res = fs_open_archive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        FS_Path* fsPath = fs_make_path_utf8(path);
        if(fsPath != NULL) {
            res = FSUSER_DeleteDirectoryRecursively(sd, *fsPath);
            fs_free_path_utf8(fsPath);
        } else {
            res = R_APP_OUT_OF_MEMORY;
        }

        fs_close_archive(sd);
    }

    return res;
}

// Size and files of a folder; at the top level, what kinds of mods it holds.
static void task_mods_walk(FS_Archive archive, const char* path, u32 depth, mod_info* info) {
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return;
    }

    Handle dir = 0;
    if(R_SUCCEEDED(FSUSER_OpenDirectory(&dir, archive, *fsPath))) {
        FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(32, sizeof(FS_DirectoryEntry));
        if(entries != NULL) {
            u32 read = 0;
            while(R_SUCCEEDED(FSDIR_Read(dir, &read, 32, entries)) && read > 0) {
                for(u32 i = 0; i < read; i++) {
                    char name[FILE_NAME_MAX] = {'\0'};
                    utf16_to_utf8((uint8_t*) name, entries[i].name, FILE_NAME_MAX - 1);

                    bool directory = (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
                    if(depth == 0) {
                        if(directory && strcasecmp(name, "romfs") == 0) {
                            info->contents |= MOD_CONTENT_ROMFS;
                        } else if(!directory && (strcasecmp(name, "code.ips") == 0 || strcasecmp(name, "code.bps") == 0 || strcasecmp(name, "code.bin") == 0)) {
                            info->contents |= MOD_CONTENT_CODE;
                        } else if(!directory && strcasecmp(name, "exheader.bin") == 0) {
                            info->contents |= MOD_CONTENT_EXHEADER;
                        } else if(!directory && strcasecmp(name, "locale.txt") == 0) {
                            info->contents |= MOD_CONTENT_LOCALE;
                        } else {
                            info->contents |= MOD_CONTENT_OTHER;
                        }
                    }

                    if(directory) {
                        if(depth < 16) {
                            char sub[FILE_PATH_MAX];
                            snprintf(sub, sizeof(sub), "%s%s/", path, name);
                            task_mods_walk(archive, sub, depth + 1, info);
                        }
                    } else {
                        info->size += entries[i].fileSize;
                        info->files++;
                    }
                }
            }

            free(entries);
        }

        FSDIR_Close(dir);
    }

    fs_free_path_utf8(fsPath);
}

// The titles installed on one medium (sorted), or NULL.
static u64* task_mods_title_list(FS_MediaType mediaType, u32* count) {
    *count = 0;

    u32 total = 0;
    if(R_FAILED(AM_GetTitleCount(mediaType, &total)) || total == 0) {
        return NULL;
    }

    u64* ids = (u64*) calloc(total, sizeof(u64));
    if(ids != NULL && R_FAILED(AM_GetTitleList(count, mediaType, total, ids))) {
        free(ids);
        return NULL;
    }

    return ids;
}

static bool task_mods_card_inserted() {
    bool inserted = false;
    FS_CardType type;
    return R_SUCCEEDED(FSUSER_CardSlotIsInserted(&inserted)) && inserted && R_SUCCEEDED(FSUSER_GetCardType(&type)) && type == CARD_CTR;
}

typedef struct {
    FS_MediaType mediaType;
    u64* ids;
    u32 count;
} task_mods_media;

static void task_mods_lookup(mod_info* info, task_mods_media* media, u32 mediaCount) {
    // The game itself: installed, or the card in the slot.
    for(u32 m = 0; m < mediaCount; m++) {
        for(u32 i = 0; i < media[m].count; i++) {
            if(media[m].ids[i] == info->titleId) {
                info->state = media[m].mediaType == MEDIATYPE_GAME_CARD ? MOD_STATE_CARD : MOD_STATE_INSTALLED;
                info->mediaType = media[m].mediaType;
                info->hasMeta = task_title_load_meta(media[m].mediaType, info->titleId, &info->meta);
                return;
            }
        }
    }

    // Its update or DLC (same unique ID, another kind): the game is most likely a card that is not in the slot.
    if(title_kind_of(info->titleId) == TITLE_KIND_GAME) {
        for(u32 m = 0; m < mediaCount; m++) {
            for(u32 i = 0; i < media[m].count; i++) {
                u64 id = media[m].ids[i];
                title_kind kind = title_kind_of(id);
                if((u32) id == (u32) info->titleId && (kind == TITLE_KIND_UPDATE || kind == TITLE_KIND_DLC)) {
                    info->state = MOD_STATE_RELATED;
                    info->mediaType = media[m].mediaType;
                    info->hasMeta = task_title_load_meta(media[m].mediaType, id, &info->meta);
                    return;
                }
            }
        }
    }

    info->state = MOD_STATE_NOT_INSTALLED;
}

static void task_populate_mods_thread(void* arg) {
    populate_mods_data* data = (populate_mods_data*) arg;

    Result res = 0;
    FS_Archive sd = 0;
    if(R_SUCCEEDED(res = fs_open_archive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        // The folders first, as rows; no /luma/titles is no mods.
        FS_Path* fsPath = fs_make_path_utf8(MODS_ROOT);
        Handle dir = 0;
        if(fsPath != NULL && R_SUCCEEDED(FSUSER_OpenDirectory(&dir, sd, *fsPath))) {
            FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(32, sizeof(FS_DirectoryEntry));
            if(entries != NULL) {
                u32 read = 0;
                bool quit = false;
                while(!quit && R_SUCCEEDED(res) && R_SUCCEEDED(FSDIR_Read(dir, &read, 32, entries)) && read > 0) {
                    for(u32 i = 0; i < read; i++) {
                        if(task_is_quit_all() || svcWaitSynchronization(data->cancelEvent, 0) == 0) {
                            quit = true;
                            break;
                        }

                        if(!(entries[i].attributes & FS_ATTRIBUTE_DIRECTORY)) {
                            continue;
                        }

                        char name[FILE_NAME_MAX] = {'\0'};
                        utf16_to_utf8((uint8_t*) name, entries[i].name, FILE_NAME_MAX - 1);

                        u64 titleId = 0;
                        if(!task_mods_parse_id(name, &titleId)) {
                            continue;
                        }

                        list_item* item = (list_item*) calloc(1, sizeof(list_item));
                        mod_info* info = (mod_info*) calloc(1, sizeof(mod_info));
                        if(item == NULL || info == NULL) {
                            free(item);
                            free(info);
                            res = R_APP_OUT_OF_MEMORY;
                            break;
                        }

                        info->titleId = titleId;
                        string_copy(info->folder, name, sizeof(info->folder));
                        snprintf(item->name, LIST_ITEM_NAME_MAX, "%016llX", titleId);
                        item->color = COLOR_TEXT;
                        item->data = info;
                        linked_list_add(data->items, item);
                    }
                }

                free(entries);
            } else {
                res = R_APP_OUT_OF_MEMORY;
            }

            FSDIR_Close(dir);
        }

        fs_free_path_utf8(fsPath);

        // What is on the console, once.
        task_mods_media media[3];
        u32 mediaCount = 0;
        media[mediaCount].mediaType = MEDIATYPE_SD;
        media[mediaCount].ids = task_mods_title_list(MEDIATYPE_SD, &media[mediaCount].count);
        mediaCount++;
        media[mediaCount].mediaType = MEDIATYPE_NAND;
        media[mediaCount].ids = task_mods_title_list(MEDIATYPE_NAND, &media[mediaCount].count);
        mediaCount++;
        if(task_mods_card_inserted()) {
            media[mediaCount].mediaType = MEDIATYPE_GAME_CARD;
            media[mediaCount].ids = task_mods_title_list(MEDIATYPE_GAME_CARD, &media[mediaCount].count);
            mediaCount++;
        }

        // Then each folder's game, size and contents. The screen sorts the list as they come, which moves rows between
        // passes: go over it again until none is missing.
        bool missing = true;
        bool quit = false;
        while(R_SUCCEEDED(res) && missing && !quit) {
            missing = false;
            for(u32 i = 0; i < linked_list_size(data->items); i++) {
                svcWaitSynchronization(task_get_pause_event(), U64_MAX);
                if(task_is_quit_all() || svcWaitSynchronization(data->cancelEvent, 0) == 0) {
                    quit = true;
                    break;
                }

                list_item* item = (list_item*) linked_list_get(data->items, i);
                if(item == NULL || ((mod_info*) item->data)->ready) {
                    continue;
                }

                missing = true;
                mod_info* info = (mod_info*) item->data;
                task_mods_lookup(info, media, mediaCount);

                char path[64];
                snprintf(path, sizeof(path), MODS_ROOT "%s/", info->folder);
                task_mods_walk(sd, path, 0, info);

                if(info->hasMeta && !string_is_empty(info->meta.shortDescription)) {
                    string_copy(item->name, info->meta.shortDescription, LIST_ITEM_NAME_MAX);
                }

                info->readyTime = osGetTime();
                info->ready = true;
            }
        }

        for(u32 m = 0; m < mediaCount; m++) {
            free(media[m].ids);
        }

        fs_close_archive(sd);
    }

    svcCloseHandle(data->cancelEvent);

    data->result = res;
    data->finished = true;
}

void task_free_mod(list_item* item) {
    if(item == NULL) {
        return;
    }

    if(item->data != NULL) {
        mod_info* info = (mod_info*) item->data;
        if(info->hasMeta) {
            screen_unload_texture(info->meta.texture);
        }

        free(item->data);
    }

    free(item);
}

void task_clear_mods(linked_list* items) {
    if(items == NULL) {
        return;
    }

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);

        linked_list_iter_remove(&iter);
        task_free_mod(item);
    }
}

Result task_populate_mods(populate_mods_data* data) {
    if(data == NULL || data->items == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    task_clear_mods(data->items);

    data->finished = false;
    data->result = 0;
    data->cancelEvent = 0;

    Result res = 0;
    if(R_SUCCEEDED(res = svcCreateEvent(&data->cancelEvent, RESET_STICKY))) {
        if(threadCreate(task_populate_mods_thread, data, 0x10000, 0x19, 1, true) == NULL) {
            res = R_APP_THREAD_CREATE_FAILED;
        }
    }

    if(R_FAILED(res)) {
        data->finished = true;

        if(data->cancelEvent != 0) {
            svcCloseHandle(data->cancelEvent);
            data->cancelEvent = 0;
        }
    }

    return res;
}
