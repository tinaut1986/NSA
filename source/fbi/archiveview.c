#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "resources.h"
#include "viewers.h"
#include "action/action.h"
#include "../core/core.h"
#include "../core/unarchive.h"

// Files -> a .zip or .7z -> View contents (issue #47): what it holds, folder by folder, without taking anything out.
// A on a folder opens it, A on a file or X on either takes only that out, into the archive's own folder.

#define ARCHIVEVIEW_MAX 8192

typedef struct {
    char* name;   // Its path inside the archive, no leading slash; a folder's ends with '/'.
    u64 size;
} archiveview_entry;

typedef struct {
    char path[FILE_PATH_MAX];      // The archive, "/dir/name.zip".
    char dir[FILE_PATH_MAX];       // Its folder, "/dir/".
    char name[FILE_NAME_MAX];
    archiveview_entry* entries;
    u32 count;
    u32 capacity;
    u32 files;
    u64 total;
    bool truncated;                // More than ARCHIVEVIEW_MAX entries: the rest is not shown.
    char folder[FILE_PATH_MAX];    // The folder shown: "" for the archive's root, else "3ds/App/".
    char from[FILE_PATH_MAX];      // The folder just left by going up ("App/"), selected again; "": the first item.
    bool populated;
    void (*done)(void* userData);  // After something was taken out (the Files list reads its folder again).
    void* userData;

    // What A or X asked to take out, while the question is up.
    char pattern[FILE_PATH_MAX + 8];
    char output[FILE_PATH_MAX + 8];
    char shown[FILE_PATH_MAX];
} archiveview_data;

static void archiveview_listed(void* userData, const char* name, u64 size, bool dir) {
    archiveview_data* data = (archiveview_data*) userData;
    while(name[0] == '/' || (name[0] == '.' && name[1] == '/')) {
        name += name[0] == '/' ? 1 : 2;
    }

    if(name[0] == '\0' || (dir && strcmp(name, "/") == 0)) {
        return;
    }

    if(!dir) {
        data->files++;
        data->total += size;
    }

    if(data->count >= ARCHIVEVIEW_MAX) {
        data->truncated = true;
        return;
    }

    if(data->count == data->capacity) {
        u32 more = data->capacity > 0 ? data->capacity * 2 : 64;
        archiveview_entry* grown = (archiveview_entry*) realloc(data->entries, more * sizeof(archiveview_entry));
        if(grown == NULL) {
            data->truncated = true;
            return;
        }

        data->entries = grown;
        data->capacity = more;
    }

    char* copy = strdup(name);
    if(copy == NULL) {
        data->truncated = true;
        return;
    }

    data->entries[data->count].name = copy;
    data->entries[data->count].size = dir ? 0 : size;
    data->count++;
}

static void archiveview_free(archiveview_data* data) {
    for(u32 i = 0; i < data->count; i++) {
        free(data->entries[i].name);
    }

    free(data->entries);
    free(data);
}

static void archiveview_clear(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static bool archiveview_is_dir(const list_item* item) {
    size_t len = strlen(item->name);
    return len > 0 && item->name[len - 1] == '/';
}

// Folders first, then by name.
static int archiveview_compare(void* userData, const void* p1, const void* p2) {
    const list_item* a = (const list_item*) p1;
    const list_item* b = (const list_item*) p2;
    bool da = archiveview_is_dir(a);
    bool db = archiveview_is_dir(b);
    if(da != db) {
        return da ? -1 : 1;
    }

    return strcasecmp(a->name, b->name);
}

// What the folder shown holds: its files, and its folders (also those the archive only names in a file's path).
static void archiveview_populate(archiveview_data* data, linked_list* items) {
    archiveview_clear(items);
    size_t folderLen = strlen(data->folder);
    for(u32 i = 0; i < data->count; i++) {
        const char* name = data->entries[i].name;
        if(strncmp(name, data->folder, folderLen) != 0 || name[folderLen] == '\0') {
            continue;
        }

        const char* rest = name + folderLen;
        const char* slash = strchr(rest, '/');
        size_t len = slash != NULL ? (size_t) (slash - rest) + 1 : strlen(rest);
        if(len >= LIST_ITEM_NAME_MAX) {
            continue;
        }

        bool seen = false;
        if(slash != NULL) {
            linked_list_iter iter;
            linked_list_iterate(items, &iter);
            while(linked_list_iter_has_next(&iter)) {
                list_item* other = (list_item*) linked_list_iter_next(&iter);
                if(strlen(other->name) == len && strncmp(other->name, rest, len) == 0) {
                    seen = true;
                    break;
                }
            }
        }

        if(seen) {
            continue;
        }

        list_item* item = (list_item*) calloc(1, sizeof(list_item));
        if(item == NULL) {
            break;
        }

        memcpy(item->name, rest, len);
        item->name[len] = '\0';
        item->color = COLOR_TEXT;
        item->data = (void*) (uintptr_t) i;
        linked_list_add(items, item);
    }

    linked_list_sort(items, NULL, archiveview_compare);
}

// "^3ds/App/App\.3dsx$" for one file, "^3ds/App/" for a folder: its special characters taken as they are (minire.h).
static void archiveview_pattern(char* out, size_t size, const char* path, bool dir) {
    size_t at = 0;
    out[at++] = '^';
    for(const char* c = path; *c != '\0' && at + 3 < size; c++) {
        if(strchr(".[]*+?^$\\", *c) != NULL) {
            out[at++] = '\\';
        }

        out[at++] = *c;
    }

    if(!dir && at + 1 < size) {
        out[at++] = '$';
    }

    out[at] = '\0';
}

static void archiveview_extracted(void* userData) {
    archiveview_data* data = (archiveview_data*) userData;
    if(data->done != NULL) {
        data->done(data->userData);
    }
}

static void archiveview_extract_answered(ui_view* view, void* userData, u32 response) {
    archiveview_data* data = (archiveview_data*) userData;
    if(response == PROMPT_YES) {
        action_extract_archive_to(data->path, data->pattern, data->output, data->shown, false, NULL, archiveview_extracted, data);
    }
}

// Only `item` of the folder shown, into the archive's own folder (a folder with all it holds).
static void archiveview_ask_extract(archiveview_data* data, const list_item* item) {
    char inside[FILE_PATH_MAX * 2];
    snprintf(inside, sizeof(inside), "%s%s", data->folder, item->name);
    bool dir = archiveview_is_dir(item);
    archiveview_pattern(data->pattern, sizeof(data->pattern), inside, dir);
    snprintf(data->shown, sizeof(data->shown), "%s%s", data->dir, item->name);
    snprintf(data->output, sizeof(data->output), "sdmc:%s", data->shown);

    static char text[FILE_PATH_MAX * 2 + 256];
    snprintf(text, sizeof(text), dir ? _("Extract the folder %s to %s?\n\nFiles with the same names there are replaced.")
                                     : _("Extract %s to %s?\n\nA file with the same name there is replaced."),
             item->name, data->dir);
    prompt_display_yes_no(_("Extract"), text, COLOR_TEXT, data, NULL, archiveview_extract_answered);
}

static void archiveview_draw_top(ui_view* view, void* userData, float x1, float y1, float x2, float y2, list_item* selected) {
    archiveview_data* data = (archiveview_data*) userData;
    const theme_current* t = theme();
    float x = x1 + 12;
    float width = x2 - x1 - 24;
    float y = y1 + 10;
    float line = widget_text_height(0.5f) + 4;

    widget_text_fit(data->name, x, y, 0.5f, t->palette->text, width);
    y += line;

    char total[32];
    ui_format_size(total, sizeof(total), data->total);
    char text[FILE_PATH_MAX + 64];
    snprintf(text, sizeof(text), data->files == 1 ? _("%lu file, %s unpacked") : _("%lu files, %s unpacked"), data->files, total);
    widget_text_fit(text, x, y, 0.45f, t->palette->textMuted, width);
    y += line;

    if(data->truncated) {
        widget_text_fit(_("Too many entries: only the first ones are shown."), x, y, 0.45f, t->palette->textMuted, width);
        y += line;
    }

    snprintf(text, sizeof(text), "/%s", data->folder);
    widget_text_fit(text, x, y, 0.45f, t->accent, width);
    y += line + 6;

    if(selected != NULL) {
        widget_text_fit(selected->name, x, y, 0.5f, t->palette->text, width);
        y += line;
        if(archiveview_is_dir(selected)) {
            widget_text_fit(_("Folder"), x, y, 0.45f, t->palette->textMuted, width);
        } else {
            u32 index = (u32) (uintptr_t) selected->data;
            char size[32];
            ui_format_size(size, sizeof(size), index < data->count ? data->entries[index].size : 0);
            widget_text_fit(size, x, y, 0.45f, t->palette->textMuted, width);
        }
    }
}

static void archiveview_update(ui_view* view, void* userData, linked_list* items, list_item* selected, bool selectedTouched) {
    archiveview_data* data = (archiveview_data*) userData;
    u32 down = hidKeysDown();

    if(down & KEY_B) {
        if(data->folder[0] == '\0') {
            ui_pop();
            archiveview_clear(items);
            list_destroy(view);
            archiveview_free(data);
            return;
        }

        // Up one folder: "3ds/App/" -> "3ds/", with "App/" selected there.
        size_t len = strlen(data->folder);
        data->folder[len - 1] = '\0';
        char* slash = strrchr(data->folder, '/');
        snprintf(data->from, sizeof(data->from), "%s/", slash != NULL ? slash + 1 : data->folder);
        if(slash != NULL) {
            slash[1] = '\0';
        } else {
            data->folder[0] = '\0';
        }

        data->populated = false;
    }

    if(!data->populated) {
        archiveview_populate(data, items);
        data->populated = true;
        list_item* select = linked_list_size(items) > 0 ? (list_item*) linked_list_get(items, 0) : NULL;
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(data->from[0] != '\0' && linked_list_iter_has_next(&iter)) {
            list_item* item = (list_item*) linked_list_iter_next(&iter);
            if(strcmp(item->name, data->from) == 0) {
                select = item;
                break;
            }
        }

        data->from[0] = '\0';
        if(select != NULL) {
            list_select(view, select);
        }

        return;
    }

    if(selected == NULL) {
        return;
    }

    if(selectedTouched || (down & KEY_A)) {
        if(archiveview_is_dir(selected)) {
            if(strlen(data->folder) + strlen(selected->name) < sizeof(data->folder)) {
                strcat(data->folder, selected->name);
                data->populated = false;
            }
        } else {
            archiveview_ask_extract(data, selected);
        }
    } else if(down & KEY_X) {
        archiveview_ask_extract(data, selected);
    }
}

void archiveview_open(const char* path, void (*done)(void* userData), void* userData) {
    archiveview_data* data = (archiveview_data*) calloc(1, sizeof(archiveview_data));
    if(data == NULL) {
        error_display(NULL, NULL, _("Failed to read the archive."));
        return;
    }

    data->done = done;
    data->userData = userData;
    string_copy(data->path, path, sizeof(data->path));
    const char* slash = strrchr(path, '/');
    string_copy(data->name, slash != NULL ? slash + 1 : path, sizeof(data->name));
    size_t dirLen = slash != NULL ? (size_t) (slash - path) + 1 : 0;
    if(dirLen >= sizeof(data->dir)) {
        dirLen = sizeof(data->dir) - 1;
    }

    memcpy(data->dir, path, dirLen);
    data->dir[dirLen] = '\0';

    unarchive_progress listing;
    memset(&listing, 0, sizeof(listing));
    listing.userData = data;
    listing.listed = archiveview_listed;
    char local[FILE_PATH_MAX + 8];
    snprintf(local, sizeof(local), "sdmc:%s", path);
    u32 count = 0;
    Result res = unarchive_extract(local, "^", "", &listing, &count);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to read the archive."));
        archiveview_free(data);
        return;
    }

    ui_view* view = list_display(_("Archive"), _(" open    extract    up"), data, archiveview_update, archiveview_draw_top);
    if(view == NULL) {
        archiveview_free(data);
        return;
    }

    list_show_title_bar(view);
}
