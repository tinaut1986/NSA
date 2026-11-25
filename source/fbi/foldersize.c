#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "foldersize.h"
#include "../core/core.h"

// ---- a folder's size: everything in it, counted on a thread while the total shows ----
//
// The console's FS keeps no size for a folder: each file in it, at every depth, has to be read from its folder's list.
// That takes from an instant to tens of seconds (it goes by the number of files, not their size), so the count runs on
// a thread of its own and the screen shows the total as it grows. B stops it.

#define FOLDERSIZE_STACK 0x8000
#define FOLDERSIZE_DEPTH 32   // Deeper folders are not counted (the console's paths are short long before this).

typedef struct {
    FS_Archive archive;
    char path[FILE_PATH_MAX];
    char name[FILE_NAME_MAX];

    Thread thread;
    volatile bool cancel;
    volatile bool finished;
    volatile u64 bytes;
    volatile u32 files;
    volatile u32 folders;
    volatile bool incomplete;    // A folder could not be read, or was too deep: the total is at least this.
    char current[FILE_PATH_MAX];  // The folder being read, for the top screen (written by the thread, read as it goes).
    u64 started;
    u64 ended;
} foldersize_data;

static void foldersize_walk(foldersize_data* data, const char* path, u32 depth) {
    if(data->cancel) {
        return;
    }

    if(depth > FOLDERSIZE_DEPTH) {
        data->incomplete = true;
        return;
    }

    string_copy(data->current, path, sizeof(data->current));
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        data->incomplete = true;
        return;
    }

    Handle dir = 0;
    if(R_FAILED(FSUSER_OpenDirectory(&dir, data->archive, *fsPath))) {
        fs_free_path_utf8(fsPath);
        data->incomplete = true;
        return;
    }

    fs_free_path_utf8(fsPath);

    FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(32, sizeof(FS_DirectoryEntry));
    if(entries == NULL) {
        FSDIR_Close(dir);
        data->incomplete = true;
        return;
    }

    u32 read = 0;
    while(!data->cancel && R_SUCCEEDED(FSDIR_Read(dir, &read, 32, entries)) && read > 0) {
        for(u32 i = 0; i < read && !data->cancel; i++) {
            if(entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) {
                char name[FILE_NAME_MAX] = {'\0'};
                utf16_to_utf8((uint8_t*) name, entries[i].name, FILE_NAME_MAX - 1);

                char sub[FILE_PATH_MAX];
                if((size_t) snprintf(sub, sizeof(sub), "%s%s/", path, name) >= sizeof(sub)) {
                    data->incomplete = true;
                    continue;
                }

                data->folders++;
                foldersize_walk(data, sub, depth + 1);
            } else {
                data->files++;
                data->bytes += entries[i].fileSize;
            }
        }
    }

    free(entries);
    FSDIR_Close(dir);
}

static void foldersize_thread(void* arg) {
    foldersize_data* data = (foldersize_data*) arg;
    foldersize_walk(data, data->path, 0);
    data->ended = osGetTime();
    data->finished = true;
}

static void foldersize_free(foldersize_data* data) {
    if(data->thread != NULL) {
        data->cancel = true;
        threadJoin(data->thread, U64_MAX);
        threadFree(data->thread);
    }

    fs_close_archive(data->archive);
    free(data);
}

// "1.2 GB in 340 files and 12 folders".
static void foldersize_summary(const foldersize_data* data, char* out, size_t size) {
    char total[32];
    ui_format_size(total, sizeof(total), data->bytes);
    char files[48];
    snprintf(files, sizeof(files), data->files == 1 ? _("%lu file") : _("%lu files"), data->files);
    char folders[48];
    snprintf(folders, sizeof(folders), data->folders == 1 ? _("%lu folder") : _("%lu folders"), data->folders);
    snprintf(out, size, _("%s in %s and %s"), total, files, folders);
}

static void foldersize_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    foldersize_data* d = (foldersize_data*) data;
    const theme_current* t = theme();

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = 96;
    widget_card(cardX, cardY, cardW, cardH);

    float tile = 64;
    float tileX = cardX + 16;
    float tileY = cardY + (cardH - tile) / 2;
    screen_fill_round_rect(tileX, tileY, tile, tile, t->def->radius, t->accentSoft);
    screen_draw_icon(ICON_FOLDER, tileX + 16, tileY + 16, 32, t->accent);

    float textX = tileX + tile + 14;
    float textW = cardX + cardW - 14 - textX;
    widget_text_fit(d->name, textX, cardY + 14, WIDGET_TITLE_SCALE, t->palette->text, textW);

    // The total, big, growing as it is counted.
    char total[32];
    ui_format_size(total, sizeof(total), d->bytes);
    float totalY = cardY + 14 + widget_text_height(WIDGET_TITLE_SCALE) + 6;
    screen_draw_text(total, textX, totalY, WIDGET_TITLE_SCALE * 1.4f, t->accent);

    char counts[96];
    char files[48];
    snprintf(files, sizeof(files), d->files == 1 ? _("%lu file") : _("%lu files"), d->files);
    char folders[48];
    snprintf(folders, sizeof(folders), d->folders == 1 ? _("%lu folder") : _("%lu folders"), d->folders);
    snprintf(counts, sizeof(counts), "%s, %s", files, folders);
    widget_text_fit(counts, textX, cardY + cardH - 14 - widget_text_height(WIDGET_TEXT_SCALE), WIDGET_TEXT_SCALE, t->palette->textMuted, textW);

    // Under the card: where it is now, and how long it has taken.
    float cellY = cardY + cardH + 8;
    float timeW = 96;
    widget_cell(cardX, cellY, cardW - timeW - 6, d->finished ? _("Folder") : _("Counting"), d->finished ? d->path : d->current);
    char took[32];
    u64 ms = (d->finished ? d->ended : osGetTime()) - d->started;
    snprintf(took, sizeof(took), "%llu.%llu s", ms / 1000, (ms % 1000) / 100);
    widget_cell(cardX + cardW - timeW, cellY, timeW, _("Time"), took);
}

static void foldersize_update(ui_view* view, void* data, float* progress, char* text) {
    foldersize_data* d = (foldersize_data*) data;

    if(!d->finished) {
        char counts[96];
        char files[48];
        snprintf(files, sizeof(files), d->files == 1 ? _("%lu file") : _("%lu files"), d->files);
        char total[32];
        ui_format_size(total, sizeof(total), d->bytes);
        snprintf(counts, sizeof(counts), "%s\n%s", total, files);
        snprintf(text, PROGRESS_TEXT_MAX, "%s", counts);
        ui_wake();

        if(hidKeysDown() & KEY_B) {
            ui_pop();
            info_destroy(view);
            foldersize_free(d);
        }

        return;
    }

    ui_pop();
    info_destroy(view);

    char summary[160];
    foldersize_summary(d, summary, sizeof(summary));
    char message[320];
    if(d->incomplete) {
        snprintf(message, sizeof(message), _("%s: %s. Some folders could not be read: it takes at least that."), d->name, summary);
    } else {
        snprintf(message, sizeof(message), "%s: %s.", d->name, summary);
    }

    foldersize_free(d);
    prompt_display_notify(_("Folder size"), message, COLOR_TEXT, NULL, NULL, NULL);
}

void foldersize_open(FS_Archive archive, const char* path, const char* name) {
    foldersize_data* data = (foldersize_data*) calloc(1, sizeof(foldersize_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate folder size data.");
        return;
    }

    // The folder's archive stays open while it is counted, whatever the list behind does.
    if(R_FAILED(fs_ref_archive(archive))) {
        free(data);
        error_display(NULL, NULL, "Failed to open the folder.");
        return;
    }

    data->archive = archive;
    string_copy(data->path, path, sizeof(data->path));
    size_t len = strlen(data->path);
    if(len == 0 || data->path[len - 1] != '/') {
        string_copy(data->path + len, "/", sizeof(data->path) - len);
    }

    string_copy(data->name, name, sizeof(data->name));
    string_copy(data->current, data->path, sizeof(data->current));
    data->started = osGetTime();

    // Below the UI's priority: the screen keeps moving while the SD card is read.
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    data->thread = threadCreate(foldersize_thread, data, FOLDERSIZE_STACK, priority + 1, -2, false);
    if(data->thread == NULL) {
        fs_close_archive(archive);
        free(data);
        error_display(NULL, NULL, "Failed to create the folder size thread.");
        return;
    }

    info_display(_("Folder size"), _(" stop"), false, data, foldersize_update, foldersize_draw_top);
}
