#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "resources.h"
#include "foldersize.h"
#include "../core/core.h"

// ---- what a folder holds, and what takes its room ----
//
// The console's FS keeps no size for a folder: each file in it, at every depth, has to be read from its folder's list.
// That takes from an instant to tens of seconds (it goes by the number of files, not their size), so the count runs on
// a thread of its own while the screen shows the total growing; B stops it. Every folder counted on the way is kept in
// a cache for the session, so a folder inside one already counted needs no count, Files shows the size in its row, and
// the breakdown (foldersize view below) has every subfolder's size at hand. Anything that changes files forgets it all.

#define FOLDERSIZE_STACK 0x8000
#define FOLDERSIZE_DEPTH 32   // Deeper folders are not counted (the console's paths are short long before this).

// ---- the cache: by archive and path ("/3ds/NSA/"), open addressing ----

typedef struct {
    u32 archiveId;
    char* path;   // NULL: a free slot.
    u64 bytes;
    u32 files;
    u32 folders;
} foldersize_entry;

static foldersize_entry* cache = NULL;
static u32 cache_capacity = 0;
static u32 cache_count = 0;
static LightLock cache_lock;
static bool cache_lock_ready = false;
static volatile u32 cache_version = 1;

static void foldersize_lock() {
    if(!cache_lock_ready) {
        LightLock_Init(&cache_lock);
        cache_lock_ready = true;
    }

    LightLock_Lock(&cache_lock);
}

static void foldersize_unlock() {
    LightLock_Unlock(&cache_lock);
}

static u32 foldersize_hash(u32 archiveId, const char* path) {
    u32 h = 2166136261u ^ archiveId;
    for(const char* c = path; *c != '\0'; c++) {
        h = (h ^ (u8) *c) * 16777619u;
    }

    return h;
}

// The slot of (archiveId, path), or the free slot where it would go. The lock is held.
static foldersize_entry* foldersize_slot(u32 archiveId, const char* path) {
    u32 mask = cache_capacity - 1;
    for(u32 i = foldersize_hash(archiveId, path) & mask;; i = (i + 1) & mask) {
        foldersize_entry* e = &cache[i];
        if(e->path == NULL || (e->archiveId == archiveId && strcmp(e->path, path) == 0)) {
            return e;
        }
    }
}

static void foldersize_put(u32 archiveId, const char* path, u64 bytes, u32 files, u32 folders) {
    foldersize_lock();

    // Kept at most half full.
    if((cache_count + 1) * 2 > cache_capacity) {
        u32 capacity = cache_capacity == 0 ? 256 : cache_capacity * 2;
        foldersize_entry* grown = (foldersize_entry*) calloc(capacity, sizeof(foldersize_entry));
        if(grown == NULL) {
            foldersize_unlock();
            return;
        }

        foldersize_entry* old = cache;
        u32 oldCapacity = cache_capacity;
        cache = grown;
        cache_capacity = capacity;
        for(u32 i = 0; i < oldCapacity; i++) {
            if(old[i].path != NULL) {
                *foldersize_slot(old[i].archiveId, old[i].path) = old[i];
            }
        }

        free(old);
    }

    foldersize_entry* e = foldersize_slot(archiveId, path);
    if(e->path == NULL) {
        e->path = strdup(path);
        if(e->path == NULL) {
            foldersize_unlock();
            return;
        }

        e->archiveId = archiveId;
        cache_count++;
    }

    e->bytes = bytes;
    e->files = files;
    e->folders = folders;
    foldersize_unlock();
}

bool foldersize_cached(u32 archiveId, const char* path, u64* bytes, u32* files, u32* folders) {
    // A path without its last '/' is the same folder.
    char key[FILE_PATH_MAX];
    size_t len = strlen(path);
    if(len == 0 || len + 2 > sizeof(key)) {
        return false;
    }

    memcpy(key, path, len + 1);
    if(key[len - 1] != '/') {
        key[len] = '/';
        key[len + 1] = '\0';
    }

    bool found = false;
    foldersize_lock();
    if(cache_count > 0) {
        foldersize_entry* e = foldersize_slot(archiveId, key);
        if(e->path != NULL) {
            found = true;
            if(bytes != NULL) {
                *bytes = e->bytes;
            }

            if(files != NULL) {
                *files = e->files;
            }

            if(folders != NULL) {
                *folders = e->folders;
            }
        }
    }

    foldersize_unlock();
    return found;
}

void foldersize_forget_all() {
    foldersize_lock();
    for(u32 i = 0; i < cache_capacity; i++) {
        free(cache[i].path);
        cache[i].path = NULL;
    }

    cache_count = 0;
    cache_version++;
    foldersize_unlock();
}

u32 foldersize_version() {
    return cache_version;
}

// ---- counting ----

typedef struct {
    FS_Archive archive;
    u32 archiveId;
    char path[FILE_PATH_MAX];
    char name[FILE_NAME_MAX];

    Thread thread;
    volatile bool cancel;
    volatile bool finished;
    volatile u64 bytes;
    volatile u32 files;
    volatile u32 folders;
    char current[FILE_PATH_MAX];  // The folder being read, for the top screen (written by the thread, read as it goes).
    u64 started;
    u64 ended;
} foldersize_count;

static void foldersize_view_open(FS_Archive archive, u32 archiveId, const char* path, const char* name);

// Counts `path` into the totals given (and the running ones for the screen); a folder already in the cache is not read
// again. A folder counted whole goes into the cache. False when something in it could not be read.
static bool foldersize_walk(foldersize_count* data, const char* path, u32 depth, u64* bytes, u32* files, u32* folders) {
    if(data->cancel) {
        return false;
    }

    u64 cachedBytes = 0;
    u32 cachedFiles = 0, cachedFolders = 0;
    if(foldersize_cached(data->archiveId, path, &cachedBytes, &cachedFiles, &cachedFolders)) {
        *bytes += cachedBytes;
        *files += cachedFiles;
        *folders += cachedFolders;
        data->bytes += cachedBytes;
        data->files += cachedFiles;
        data->folders += cachedFolders;
        return true;
    }

    if(depth > FOLDERSIZE_DEPTH) {
        return false;
    }

    string_copy(data->current, path, sizeof(data->current));
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return false;
    }

    Handle dir = 0;
    Result res = FSUSER_OpenDirectory(&dir, data->archive, *fsPath);
    fs_free_path_utf8(fsPath);
    if(R_FAILED(res)) {
        return false;
    }

    FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(32, sizeof(FS_DirectoryEntry));
    if(entries == NULL) {
        FSDIR_Close(dir);
        return false;
    }

    u64 ownBytes = 0;
    u32 ownFiles = 0, ownFolders = 0;
    bool whole = true;
    u32 read = 0;
    while(!data->cancel && R_SUCCEEDED(FSDIR_Read(dir, &read, 32, entries)) && read > 0) {
        for(u32 i = 0; i < read && !data->cancel; i++) {
            if(entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) {
                char name[FILE_NAME_MAX] = {'\0'};
                utf16_to_utf8((uint8_t*) name, entries[i].name, FILE_NAME_MAX - 1);

                char sub[FILE_PATH_MAX];
                if((size_t) snprintf(sub, sizeof(sub), "%s%s/", path, name) >= sizeof(sub)) {
                    whole = false;
                    continue;
                }

                ownFolders++;
                data->folders++;
                whole &= foldersize_walk(data, sub, depth + 1, &ownBytes, &ownFiles, &ownFolders);
            } else {
                ownFiles++;
                ownBytes += entries[i].fileSize;
                data->files++;
                data->bytes += entries[i].fileSize;
            }
        }
    }

    free(entries);
    FSDIR_Close(dir);

    whole &= !data->cancel;
    if(whole) {
        foldersize_put(data->archiveId, path, ownBytes, ownFiles, ownFolders);
    }

    *bytes += ownBytes;
    *files += ownFiles;
    *folders += ownFolders;
    return whole;
}

static void foldersize_thread(void* arg) {
    foldersize_count* data = (foldersize_count*) arg;
    u64 bytes = 0;
    u32 files = 0, folders = 0;
    foldersize_walk(data, data->path, 0, &bytes, &files, &folders);
    data->ended = osGetTime();
    data->finished = true;
}

static void foldersize_count_free(foldersize_count* data) {
    if(data->thread != NULL) {
        data->cancel = true;
        threadJoin(data->thread, U64_MAX);
        threadFree(data->thread);
    }

    fs_close_archive(data->archive);
    free(data);
}

static void foldersize_counts(char* out, size_t size, u32 files, u32 folders) {
    char f[48];
    snprintf(f, sizeof(f), files == 1 ? _("%lu file") : _("%lu files"), files);
    char d[48];
    snprintf(d, sizeof(d), folders == 1 ? _("%lu folder") : _("%lu folders"), folders);
    snprintf(out, size, "%s, %s", f, d);
}

// The folder's card: its icon, its name and, big, its size.
static void foldersize_draw_card(float cardX, float cardY, float cardW, float cardH, const char* name, u64 bytes, const char* counts, bool counting) {
    const theme_current* t = theme();
    widget_card(cardX, cardY, cardW, cardH);

    float tile = 64;
    float tileX = cardX + 16;
    float tileY = cardY + (cardH - tile) / 2;
    screen_fill_round_rect(tileX, tileY, tile, tile, t->def->radius, t->accentSoft);
    if(counting) {
        widget_busy(tileX + tile / 2, tileY + tile / 2, 18, t->accent);
    } else {
        screen_draw_icon(ICON_FOLDER, tileX + 16, tileY + 16, 32, t->accent);
    }

    float textX = tileX + tile + 14;
    float textW = cardX + cardW - 14 - textX;
    widget_text_fit(name, textX, cardY + 12, WIDGET_TEXT_SCALE, t->palette->textMuted, textW);

    char total[32];
    ui_format_size(total, sizeof(total), bytes);
    screen_draw_text(total, textX, cardY + 12 + widget_text_height(WIDGET_TEXT_SCALE) + 2, WIDGET_TITLE_SCALE * 1.6f, t->palette->text);
    widget_text_fit(counts, textX, cardY + cardH - 12 - widget_text_height(WIDGET_TEXT_SCALE), WIDGET_TEXT_SCALE, t->palette->textMuted, textW);
}

static void foldersize_count_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    foldersize_count* d = (foldersize_count*) data;
    float cardX = x1 + 12;
    float cardW = x2 - x1 - 24;
    char counts[96];
    foldersize_counts(counts, sizeof(counts), d->files, d->folders);
    foldersize_draw_card(cardX, y1 + 14, cardW, 96, d->name, d->bytes, counts, true);

    float cellY = y1 + 14 + 96 + 8;
    float timeW = 80;
    widget_cell(cardX, cellY, cardW - timeW - 6, _("Counting"), d->current);
    char took[32];
    u64 ms = osGetTime() - d->started;
    snprintf(took, sizeof(took), "%llu.%llu s", ms / 1000, (ms % 1000) / 100);
    widget_cell(cardX + cardW - timeW, cellY, timeW, _("Time"), took);
}

static void foldersize_count_update(ui_view* view, void* data, float* progress, char* text) {
    foldersize_count* d = (foldersize_count*) data;

    if(!d->finished) {
        char total[32];
        ui_format_size(total, sizeof(total), d->bytes);
        char counts[96];
        foldersize_counts(counts, sizeof(counts), d->files, d->folders);
        snprintf(text, PROGRESS_TEXT_MAX, "%s\n%s", total, counts);
        ui_wake();

        if(hidKeysDown() & KEY_B) {
            ui_pop();
            info_destroy(view);
            foldersize_count_free(d);
        }

        return;
    }

    ui_pop();
    info_destroy(view);

    // Counted: what takes the room, folder by folder (a folder that could not be read shows what could).
    FS_Archive archive = d->archive;
    fs_ref_archive(archive);
    char path[FILE_PATH_MAX];
    char name[FILE_NAME_MAX];
    string_copy(path, d->path, sizeof(path));
    string_copy(name, d->name, sizeof(name));
    u32 archiveId = d->archiveId;
    if(!foldersize_cached(archiveId, path, NULL, NULL, NULL)) {
        foldersize_put(archiveId, path, d->bytes, d->files, d->folders);
    }

    foldersize_count_free(d);
    foldersize_view_open(archive, archiveId, path, name);
    fs_close_archive(archive);
}

void foldersize_open(FS_Archive archive, u32 archiveId, const char* path, const char* name) {
    char dir[FILE_PATH_MAX];
    string_copy(dir, path, sizeof(dir));
    size_t len = strlen(dir);
    if(len == 0 || (dir[len - 1] != '/' && len + 1 < sizeof(dir))) {
        dir[len] = '/';
        dir[len + 1] = '\0';
    }

    // Counted before: straight to what it holds.
    if(foldersize_cached(archiveId, dir, NULL, NULL, NULL)) {
        foldersize_view_open(archive, archiveId, dir, name);
        return;
    }

    foldersize_count* data = (foldersize_count*) calloc(1, sizeof(foldersize_count));
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
    data->archiveId = archiveId;
    string_copy(data->path, dir, sizeof(data->path));
    string_copy(data->name, name, sizeof(data->name));
    string_copy(data->current, dir, sizeof(data->current));
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

    info_display(_("Folder size"), _(" stop"), false, data, foldersize_count_update, foldersize_count_draw_top);
}

// ---- what takes the room: a folder's contents, biggest first, each with its share ----

typedef struct {
    char name[FILE_NAME_MAX];
    bool folder;
    bool known;     // A folder that could not be counted has no size.
    u64 bytes;
    u32 files;      // A folder's.
} foldersize_child;

typedef struct {
    FS_Archive archive;
    u32 archiveId;
    char root[FILE_PATH_MAX];   // Where it was opened: B there closes the view.
    char rootName[FILE_NAME_MAX];
    char path[FILE_PATH_MAX];

    foldersize_child* children;
    u32 count;
    u64 total;
    u32 files;
    u32 folders;
    bool stale;                  // The list must be built again (a new folder, or the cache forgot).
    u32 version;
    char reselect[FILE_NAME_MAX];   // Going up: the folder it came from gets the focus.
} foldersize_view;

// The shades of the accent for the biggest ones, the last for all the rest.
#define FOLDERSIZE_SHADES 7

static u32 foldersize_shade(u32 index) {
    const theme_current* t = theme();
    static const u32 weights[FOLDERSIZE_SHADES + 1] = {0x00, 0x30, 0x58, 0x7C, 0x9C, 0xB8, 0xD0, 0xE4};
    u32 i = index > FOLDERSIZE_SHADES ? FOLDERSIZE_SHADES : index;
    return theme_mix(t->accent, t->palette->surface, weights[i]);
}

static int foldersize_compare(const void* a, const void* b) {
    const foldersize_child* x = (const foldersize_child*) a;
    const foldersize_child* y = (const foldersize_child*) b;
    if(x->bytes != y->bytes) {
        return x->bytes > y->bytes ? -1 : 1;
    }

    return strcasecmp(x->name, y->name);
}

// Reads the folder's own list; each subfolder's size comes from the cache, which the count filled.
static void foldersize_view_load(foldersize_view* v) {
    free(v->children);
    v->children = NULL;
    v->count = 0;
    v->total = 0;
    v->files = 0;
    v->folders = 0;
    v->version = foldersize_version();

    FS_Path* fsPath = fs_make_path_utf8(v->path);
    if(fsPath == NULL) {
        return;
    }

    Handle dir = 0;
    Result res = FSUSER_OpenDirectory(&dir, v->archive, *fsPath);
    fs_free_path_utf8(fsPath);
    if(R_FAILED(res)) {
        return;
    }

    FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(16, sizeof(FS_DirectoryEntry));
    if(entries == NULL) {
        FSDIR_Close(dir);
        return;
    }

    u32 capacity = 0;
    u32 read = 0;
    while(R_SUCCEEDED(FSDIR_Read(dir, &read, 16, entries)) && read > 0) {
        for(u32 i = 0; i < read; i++) {
            if(v->count == capacity) {
                u32 grown = capacity == 0 ? 64 : capacity * 2;
                foldersize_child* more = (foldersize_child*) realloc(v->children, grown * sizeof(foldersize_child));
                if(more == NULL) {
                    break;
                }

                v->children = more;
                capacity = grown;
            }

            foldersize_child* c = &v->children[v->count];
            memset(c, 0, sizeof(*c));
            utf16_to_utf8((uint8_t*) c->name, entries[i].name, FILE_NAME_MAX - 1);
            c->folder = (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
            if(c->folder) {
                char sub[FILE_PATH_MAX];
                snprintf(sub, sizeof(sub), "%s%s/", v->path, c->name);
                u32 folders = 0;
                c->known = foldersize_cached(v->archiveId, sub, &c->bytes, &c->files, &folders);
                v->files += c->files;
                v->folders += 1 + folders;
            } else {
                c->known = true;
                c->bytes = entries[i].fileSize;
                v->files++;
            }

            v->total += c->bytes;
            v->count++;
        }
    }

    free(entries);
    FSDIR_Close(dir);
    if(v->count > 1) {
        qsort(v->children, v->count, sizeof(foldersize_child), foldersize_compare);
    }
}

static foldersize_child* foldersize_view_child(foldersize_view* v, list_item* item) {
    u32 index = (u32) (uintptr_t) item->data;
    return index < v->count ? &v->children[index] : NULL;
}

// The share of the whole: "42 %", or "<1 %" for a little one.
static void foldersize_percent(char* out, size_t size, u64 part, u64 total) {
    if(total == 0) {
        snprintf(out, size, "-");
        return;
    }

    u32 tenths = (u32) (part * 1000 / total);
    if(part > 0 && tenths < 10) {
        snprintf(out, size, "<1 %%");
    } else {
        snprintf(out, size, "%lu %%", (tenths + 5) / 10);
    }
}

static void foldersize_view_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    foldersize_view* v = (foldersize_view*) data;
    foldersize_child* c = foldersize_view_child(v, item);
    if(c == NULL) {
        return;
    }

    const theme_current* t = theme();
    u32 index = (u32) (uintptr_t) item->data;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, selected ? t->accent : t->palette->border, selected ? t->accent : t->palette->surface);

    // Its colour in the bar on the top screen, beside its icon.
    float sw = 6;
    screen_fill_round_rect(x + 5, y + 6, sw, height - 12, 2, selected ? t->onAccent : foldersize_shade(index));
    float iconX = x + 5 + sw + 6;
    screen_draw_icon(c->folder ? ICON_FOLDER : ICON_FILE, iconX, y + 5, 16, selected ? t->onAccent : t->accent);

    char size[32];
    if(c->known) {
        ui_format_size(size, sizeof(size), c->bytes);
    } else {
        string_copy(size, "?", sizeof(size));
    }

    float right = x + width - 8;
    if(c->folder) {
        screen_draw_icon(ICON_CHEVRON_RIGHT, right - 14, y + (height - 14) / 2, 14, muted);
        right -= 18;
    }

    float sizeW = widget_text_width(size, WIDGET_TEXT_SCALE);
    screen_draw_text(size, right - sizeW, y + 4, WIDGET_TEXT_SCALE, ink);
    float tx = iconX + 16 + 6;
    widget_text_fit(c->name, tx, y + 4, WIDGET_TEXT_SCALE, ink, right - sizeW - 8 - tx);

    // Its share: a bar along the row, and the percentage at its end.
    char percent[16];
    foldersize_percent(percent, sizeof(percent), c->bytes, v->total);
    float percentW = widget_text_width(percent, WIDGET_SMALL_SCALE);
    float barY = y + height - 10;
    float barW = right - percentW - 8 - tx;
    screen_fill_round_rect(tx, barY, barW, 5, 2, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->palette->background);
    float share = v->total > 0 ? (float) ((double) c->bytes / (double) v->total) : 0;
    if(share > 0) {
        float w = barW * share;
        screen_fill_round_rect(tx, barY, w < 3 ? 3 : w, 5, 2, selected ? t->onAccent : t->accent);
    }

    screen_draw_text(percent, right - percentW, barY + 2.5f - widget_text_height(WIDGET_SMALL_SCALE) / 2, WIDGET_SMALL_SCALE, muted);
}

static void foldersize_view_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    foldersize_view* v = (foldersize_view*) data;
    const theme_current* t = theme();

    float cardX = x1 + 12;
    float cardW = x2 - x1 - 24;
    char counts[96];
    foldersize_counts(counts, sizeof(counts), v->files, v->folders);

    // The folder's name: the last part of its path, or what it was opened as at the top.
    char name[FILE_NAME_MAX];
    if(strcmp(v->path, v->root) == 0) {
        string_copy(name, v->rootName, sizeof(name));
    } else {
        size_t len = strlen(v->path);
        size_t start = len > 1 ? len - 1 : 0;
        while(start > 0 && v->path[start - 1] != '/') {
            start--;
        }

        snprintf(name, sizeof(name), "%.*s", (int) (len - 1 - start), v->path + start);
    }

    foldersize_draw_card(cardX, y1 + 10, cardW, 84, name, v->total, counts, false);

    // The biggest ones as one bar, a shade each, all the rest in the last.
    float barY = y1 + 10 + 84 + 10;
    float barH = 14;
    screen_fill_round_rect(cardX, barY, cardW, barH, t->def->radiusSmall > 4 ? 4 : t->def->radiusSmall, t->palette->border);
    if(v->total > 0) {
        float bx = cardX;
        u64 shown = 0;
        for(u32 i = 0; i < v->count && i < FOLDERSIZE_SHADES; i++) {
            float w = (float) ((double) v->children[i].bytes / (double) v->total * cardW);
            if(w >= 1) {
                screen_fill_rect(bx, barY, w, barH, foldersize_shade(i));
                bx += w;
            }

            shown += v->children[i].bytes;
        }

        if(v->total > shown && bx < cardX + cardW) {
            screen_fill_rect(bx, barY, cardX + cardW - bx, barH, foldersize_shade(FOLDERSIZE_SHADES));
        }

        // The one with the focus, outlined in the bar.
        if(selected != NULL) {
            u32 index = (u32) (uintptr_t) selected->data;
            if(index < FOLDERSIZE_SHADES && index < v->count) {
                float sx = cardX;
                for(u32 i = 0; i < index; i++) {
                    sx += (float) ((double) v->children[i].bytes / (double) v->total * cardW);
                }

                float w = (float) ((double) v->children[index].bytes / (double) v->total * cardW);
                if(w >= 1) {
                    // An outline, its four sides.
                    screen_fill_rect(sx - 1, barY - 2, w + 2, 2, t->palette->text);
                    screen_fill_rect(sx - 1, barY + barH, w + 2, 2, t->palette->text);
                    screen_fill_rect(sx - 1, barY, 2, barH, t->palette->text);
                    screen_fill_rect(sx + w - 1, barY, 2, barH, t->palette->text);
                }
            }
        }
    }

    // The legend: the biggest ones, two columns.
    float legendY = barY + barH + 8;
    float colW = (cardW - 8) / 2;
    float lineH = widget_text_height(WIDGET_SMALL_SCALE) + 5;
    u32 rows = (u32) ((y2 - 6 - legendY) / lineH);
    u32 slots = rows * 2;
    for(u32 i = 0; i < v->count && i < slots && i <= FOLDERSIZE_SHADES; i++) {
        float lx = cardX + (i / rows) * (colW + 8);
        float ly = legendY + (i % rows) * lineH;
        bool rest = i == FOLDERSIZE_SHADES;
        screen_fill_round_rect(lx, ly + 2, 9, 9, 2, foldersize_shade(i));

        char label[FILE_NAME_MAX + 32];
        char size[32];
        if(rest) {
            u64 others = 0;
            for(u32 j = FOLDERSIZE_SHADES; j < v->count; j++) {
                others += v->children[j].bytes;
            }

            ui_format_size(size, sizeof(size), others);
            snprintf(label, sizeof(label), _("%lu more"), v->count - FOLDERSIZE_SHADES);
        } else {
            ui_format_size(size, sizeof(size), v->children[i].bytes);
            string_copy(label, v->children[i].name, sizeof(label));
        }

        float sizeW = widget_text_width(size, WIDGET_SMALL_SCALE);
        widget_text_fit(label, lx + 14, ly, WIDGET_SMALL_SCALE, t->palette->text, colW - 14 - sizeW - 6);
        screen_draw_text(size, lx + colW - sizeW, ly, WIDGET_SMALL_SCALE, t->palette->textMuted);
    }
}

static void foldersize_view_clear(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static void foldersize_view_free(foldersize_view* v) {
    fs_close_archive(v->archive);
    free(v->children);
    free(v);
}

static void foldersize_view_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    foldersize_view* v = (foldersize_view*) data;

    view->name = _("What takes the room");
    view->info = selected != NULL && foldersize_view_child(v, selected) != NULL && foldersize_view_child(v, selected)->folder ? _(" open    back") : _(" back");

    if(hidKeysDown() & KEY_B) {
        // Up a folder, until the one it was opened on.
        if(strcmp(v->path, v->root) != 0) {
            size_t len = strlen(v->path);
            size_t start = len - 1;
            while(start > 0 && v->path[start - 1] != '/') {
                start--;
            }

            snprintf(v->reselect, sizeof(v->reselect), "%.*s", (int) (len - 1 - start), v->path + start);
            v->path[start] = '\0';
            v->stale = true;
            return;
        }

        ui_pop();
        foldersize_view_clear(items);
        list_destroy(view);
        foldersize_view_free(v);
        return;
    }

    // Something changed files: what is known now (the folders not counted again show no size).
    if(v->version != foldersize_version()) {
        v->stale = true;
    }

    if(v->stale) {
        v->stale = false;
        foldersize_view_clear(items);
        foldersize_view_load(v);
        list_item* focus = NULL;
        for(u32 i = 0; i < v->count; i++) {
            list_item* item = (list_item*) calloc(1, sizeof(list_item));
            if(item == NULL) {
                break;
            }

            string_copy(item->name, v->children[i].name, sizeof(item->name));
            item->color = COLOR_TEXT;
            item->data = (void*) (uintptr_t) i;
            linked_list_add(items, item);
            if(v->reselect[0] != '\0' && strcmp(v->children[i].name, v->reselect) == 0) {
                focus = item;
            }
        }

        v->reselect[0] = '\0';
        if(focus != NULL) {
            list_select(view, focus);
        }

        return;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        foldersize_child* c = foldersize_view_child(v, selected);
        if(c != NULL && c->folder) {
            size_t len = strlen(v->path);
            snprintf(v->path + len, sizeof(v->path) - len, "%s/", c->name);
            v->stale = true;
        }
    }
}

static void foldersize_view_open(FS_Archive archive, u32 archiveId, const char* path, const char* name) {
    foldersize_view* v = (foldersize_view*) calloc(1, sizeof(foldersize_view));
    if(v == NULL || R_FAILED(fs_ref_archive(archive))) {
        free(v);
        error_display(NULL, NULL, "Failed to open the folder.");
        return;
    }

    v->archive = archive;
    v->archiveId = archiveId;
    string_copy(v->root, path, sizeof(v->root));
    string_copy(v->path, path, sizeof(v->path));
    string_copy(v->rootName, name, sizeof(v->rootName));
    v->stale = true;

    ui_view* view = list_display(_("What takes the room"), _(" open    back"), v, foldersize_view_update, foldersize_view_draw_top);
    if(view == NULL) {
        foldersize_view_free(v);
        return;
    }

    view->id = "What takes the room";
    list_style style = {
            .rowHeight = 30,
            .drawRow = foldersize_view_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
}

void foldersize_open_sd() {
    FS_Archive archive = 0;
    if(R_FAILED(fs_open_archive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        error_display(NULL, NULL, "Failed to open the SD card.");
        return;
    }

    foldersize_open(archive, ARCHIVE_SDMC, "/", _("SD card"));
    fs_close_archive(archive);
}
