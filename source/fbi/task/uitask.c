#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "uitask.h"
#include "../../core/core.h"
#include "../../core/data/smdh.h"

// The top screen while an action asks or works: what it is about, as a card
// with the item's icon and name and a grid of cells under it (the same look as
// Titles and Files).

#define CARD_HEIGHT 76
#define CELL_HEIGHT 34
#define CELL_GAP 6

typedef struct {
    const char* label;
    char value[96];
    float weight;   // Share of the row: 1, or 1.5 for an ID.
} task_cell;

static void task_draw_cell(float x, float y, float w, const task_cell* cell) {
    const theme_current* t = theme();
    screen_frame_round_rect(x, y, w, CELL_HEIGHT, t->def->radiusSmall, t->def->border, t->palette->border, t->palette->surface);
    screen_draw_text(cell->label, x + 8, y + 3, WIDGET_SMALL_SCALE, t->palette->textMuted);
    float scale = widget_text_width(cell->value, WIDGET_TEXT_SCALE) <= w - 14 ? WIDGET_TEXT_SCALE : WIDGET_SMALL_SCALE;
    widget_text_fit(cell->value, x + 8, y + 3 + widget_text_height(WIDGET_SMALL_SCALE), scale, t->palette->text, w - 14);
}

// Cells fill rows three units wide; a cell of weight 1.5 (an ID) is wider than
// one of 1, and each row stretches to the full width.
static float task_draw_cells(float x, float y, float w, const task_cell* cells, u32 count) {
    u32 i = 0;
    u32 row = 0;
    while(i < count) {
        u32 first = i;
        float total = 0;
        while(i < count && (i == first || total + cells[i].weight <= 3.01f)) {
            total += cells[i].weight;
            i++;
        }

        float usable = w - (i - first - 1) * CELL_GAP;
        float cx = x;
        for(u32 c = first; c < i; c++) {
            float cw = usable * cells[c].weight / total;
            task_draw_cell(cx, y + row * (CELL_HEIGHT + CELL_GAP), cw, &cells[c]);
            cx += cw + CELL_GAP;
        }

        row++;
    }

    return y + row * (CELL_HEIGHT + CELL_GAP) - CELL_GAP;
}

// The icon (a texture, or a drawn icon over a wash), a title and up to two
// lines under it.
static void task_draw_card(float x, float y, float w, u32 texture, u32 icon, const char* title, const char* line1, const char* line2) {
    const theme_current* t = theme();
    widget_card(x, y, w, CARD_HEIGHT);

    float iconSize = 56;
    float iconX = x + 10;
    float iconY = y + (CARD_HEIGHT - iconSize) / 2;
    float depth = screen_get_depth();
    screen_set_depth(depth + 1);
    if(texture != 0) {
        screen_draw_texture(texture, iconX, iconY, iconSize, iconSize);
    } else {
        screen_fill_round_rect(iconX, iconY, iconSize, iconSize, t->def->radius, t->accentSoft);
        screen_draw_icon(icon, iconX + 14, iconY + 14, 28, t->accent);
    }

    screen_set_depth(depth);

    float tx = iconX + iconSize + 12;
    float right = x + w - 10;
    widget_text_fit(title, tx, y + 10, WIDGET_TITLE_SCALE, t->palette->text, right - tx);
    float lineY = y + 14 + widget_text_height(WIDGET_TITLE_SCALE);
    if(line1 != NULL) {
        widget_text_fit(line1, tx, lineY, WIDGET_TEXT_SCALE, t->palette->textMuted, right - tx);
    }

    if(line2 != NULL) {
        widget_text_fit(line2, tx, lineY + widget_text_height(WIDGET_TEXT_SCALE), WIDGET_SMALL_SCALE, t->palette->textMuted, right - tx);
    }
}

static void task_set(task_cell* cell, const char* label, const char* format, ...) {
    cell->label = label;
    cell->weight = 1;

    va_list list;
    va_start(list, format);
    vsnprintf(cell->value, sizeof(cell->value), format, list);
    va_end(list);
}

static void task_wide(task_cell* cell) {
    cell->weight = 1.5f;
}

const char* task_kind_name(u64 titleId) {
    switch(title_kind_of(titleId)) {
        case TITLE_KIND_GAME: return _("Game");
        case TITLE_KIND_UPDATE: return _("Update");
        case TITLE_KIND_DLC: return "DLC";
        case TITLE_KIND_DEMO: return _("Demo");
        case TITLE_KIND_DSIWARE: return "DSiWare";
        default: return _("System");
    }
}

static const char* task_media_name(FS_MediaType mediaType) {
    return mediaType == MEDIATYPE_NAND ? "NAND" : mediaType == MEDIATYPE_SD ? "SD" : _("Game card");
}

void task_version_text(char* out, size_t size, u16 version) {
    snprintf(out, size, "%d.%d.%d (%hu)", (version >> 10) & 0x3F, (version >> 4) & 0x3F, version & 0xF, version);
}

static void task_size_text(char* out, size_t size, u64 bytes) {
    snprintf(out, size, "%.2f %s", ui_get_display_size(bytes), ui_get_display_size_units(bytes));
}

// A title's card from its banner: icon, name, description and publisher.
static void task_draw_meta_card(meta_info* meta, float x, float y, float w) {
    task_draw_card(x, y, w, meta->texture, ICON_APPS, meta->shortDescription, meta->longDescription, meta->publisher);
}

void task_draw_meta_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    task_draw_meta_card((meta_info*) data, x1 + 10, y1 + 10, x2 - x1 - 20);
}

// ---- how much an ext save data holds ----

// Found by walking its files, on a thread of its own the first time it is shown.
#define EXT_SIZE_CACHE 32

typedef struct {
    u64 id;
    u32 media;
    bool shared;
    volatile int state;   // 0 unused, 1 working, 2 done.
    u64 size;
} ext_size_entry;

static ext_size_entry ext_size_cache[EXT_SIZE_CACHE];
static u32 ext_size_next = 0;
static volatile bool ext_size_busy = false;

static u64 task_dir_size(FS_Archive archive, const char* path, u32 depth) {
    u64 total = 0;
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return 0;
    }

    Handle dir = 0;
    if(R_SUCCEEDED(FSUSER_OpenDirectory(&dir, archive, *fsPath))) {
        FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(64, sizeof(FS_DirectoryEntry));
        if(entries != NULL) {
            u32 read = 0;
            while(R_SUCCEEDED(FSDIR_Read(dir, &read, 64, entries)) && read > 0) {
                for(u32 i = 0; i < read; i++) {
                    if(entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) {
                        if(depth < 12) {
                            char name[FILE_NAME_MAX] = {'\0'};
                            utf16_to_utf8((uint8_t*) name, entries[i].name, FILE_NAME_MAX - 1);

                            char sub[FILE_PATH_MAX];
                            snprintf(sub, sizeof(sub), "%s%s/", path, name);
                            total += task_dir_size(archive, sub, depth + 1);
                        }
                    } else {
                        total += entries[i].fileSize;
                    }
                }
            }

            free(entries);
        }

        FSDIR_Close(dir);
    }

    fs_free_path_utf8(fsPath);
    return total;
}

static void task_ext_size_thread(void* arg) {
    ext_size_entry* entry = (ext_size_entry*) arg;

    u32 path[3] = {entry->media, (u32) (entry->id & 0xFFFFFFFF), (u32) ((entry->id >> 32) & 0xFFFFFFFF)};
    FS_Archive archive = 0;
    u64 size = 0;
    if(R_SUCCEEDED(fs_open_archive(&archive, entry->shared ? ARCHIVE_SHARED_EXTDATA : ARCHIVE_EXTDATA, fs_make_path_binary(path, sizeof(path))))) {
        size = task_dir_size(archive, "/", 0);
        fs_close_archive(archive);
    }

    entry->size = size;
    entry->state = 2;
    ext_size_busy = false;
}

// The entry for this ext save data, starting its count when it has none (one
// count at a time); NULL while another is running.
static ext_size_entry* task_ext_size(ext_save_data_info* info) {
    for(u32 i = 0; i < EXT_SIZE_CACHE; i++) {
        ext_size_entry* entry = &ext_size_cache[i];
        if(entry->state != 0 && entry->id == info->extSaveDataId && entry->media == (u32) info->mediaType && entry->shared == info->shared) {
            return entry;
        }
    }

    if(ext_size_busy) {
        return NULL;
    }

    ext_size_entry* entry = &ext_size_cache[ext_size_next++ % EXT_SIZE_CACHE];
    entry->id = info->extSaveDataId;
    entry->media = (u32) info->mediaType;
    entry->shared = info->shared;
    entry->size = 0;
    entry->state = 1;
    ext_size_busy = true;

    if(threadCreate(task_ext_size_thread, entry, 0x20000, 0x19, 1, true) == NULL) {
        entry->state = 2;
        ext_size_busy = false;
    }

    return entry;
}

// The installed games and demos, listed once in a while for the lookups below.
#define TITLE_CACHE_MAX 1024

static struct {
    u64 id[TITLE_CACHE_MAX];
    FS_MediaType media[TITLE_CACHE_MAX];
    u32 count;
    u64 time;
} title_cache;

static void task_title_cache_refresh() {
    if(title_cache.time != 0 && osGetTime() < title_cache.time + 10000) {
        return;
    }

    title_cache.count = 0;
    title_cache.time = osGetTime();

    for(FS_MediaType mediaType = MEDIATYPE_NAND; mediaType != MEDIATYPE_GAME_CARD; mediaType++) {
        u32 total = 0;
        if(R_FAILED(AM_GetTitleCount(mediaType, &total)) || total == 0) {
            continue;
        }

        u64* ids = (u64*) calloc(total, sizeof(u64));
        if(ids == NULL) {
            continue;
        }

        u32 read = 0;
        if(R_SUCCEEDED(AM_GetTitleList(&read, mediaType, total, ids))) {
            for(u32 i = 0; i < read && title_cache.count < TITLE_CACHE_MAX; i++) {
                title_kind kind = title_kind_of(ids[i]);
                if(kind == TITLE_KIND_GAME || kind == TITLE_KIND_DEMO) {
                    title_cache.id[title_cache.count] = ids[i];
                    title_cache.media[title_cache.count++] = mediaType;
                }
            }
        }

        free(ids);
    }
}

// The installed game these data belong to, told by the ID: the data of many
// games are named like the game (same ID), or like its unique ID without the
// last byte (Mario Kart 7 is 00030700, its data 307).
static void task_ext_title_lookup(ext_save_data_info* info) {
    if(info->looked) {
        return;
    }

    info->looked = true;
    if(info->shared || (info->extSaveDataId >> 32) != 0) {
        return;
    }

    task_title_cache_refresh();

    u32 ext = (u32) info->extSaveDataId;
    int found = -1;
    for(u32 i = 0; i < title_cache.count && found < 0; i++) {
        if((u32) title_cache.id[i] == ext) {
            found = (int) i;
        }
    }

    for(u32 i = 0; i < title_cache.count && found < 0; i++) {
        if(((u32) title_cache.id[i] >> 8) == ext) {
            found = (int) i;
        }
    }

    if(found >= 0) {
        info->hasTitle = true;
        info->titleMedia = title_cache.media[found];
        info->hasTitleMeta = task_title_load_meta(info->titleMedia, title_cache.id[found], &info->titleMeta);
    }
}

void task_draw_ext_save_data_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    ext_save_data_info* info = (ext_save_data_info*) data;
    float x = x1 + 10;
    float w = x2 - x1 - 20;

    task_ext_title_lookup(info);

    char id[32];
    snprintf(id, sizeof(id), "%016llX", info->extSaveDataId);
    if(info->hasMeta) {
        task_draw_meta_card(&info->meta, x, y1 + 10, w);
    } else if(info->hasTitleMeta) {
        task_draw_meta_card(&info->titleMeta, x, y1 + 10, w);
    } else {
        task_draw_card(x, y1 + 10, w, 0, ICON_DATABASE, _("Extra data"), id, NULL);
    }

    ext_size_entry* entry = task_ext_size(info);

    task_cell cells[4];
    task_set(&cells[0], _("ID"), "%s", id);
    task_wide(&cells[0]);
    if(entry != NULL && entry->state == 2) {
        char size[32];
        task_size_text(size, sizeof(size), entry->size);
        task_set(&cells[1], _("Size"), "%s", size);
    } else {
        task_set(&cells[1], _("Size"), "%s", _("Calculating..."));
    }

    task_set(&cells[2], _("Shared"), "%s", info->shared ? _("Yes") : _("No"));
    task_set(&cells[3], _("Memory"), "%s", task_media_name(info->mediaType));
    float bottom = task_draw_cells(x, y1 + 10 + CARD_HEIGHT + 8, w, cells, 4);

    // Which game they are of, when it can be told by the ID.
    if(info->hasTitle) {
        char note[256];
        snprintf(note, sizeof(note), _("Probably the data of the installed title: %s"), info->hasTitleMeta ? info->titleMeta.shortDescription : id);
        screen_draw_text_wrap(note, x + 2, bottom + 10, WIDGET_SMALL_SCALE + 0.04f, theme()->palette->textMuted, x + w - 2);
    }
}

// Attributes as words, "Hidden, Read only", or "None".
static void task_attributes_text(char* out, size_t size, u32 attributes) {
    out[0] = '\0';
    static const struct {
        u32 flag;
        const char* name;
    } names[] = {
            {FS_ATTRIBUTE_DIRECTORY, "Folder"},
            {FS_ATTRIBUTE_HIDDEN, "Hidden"},
            {FS_ATTRIBUTE_ARCHIVE, "Archive"},
            {FS_ATTRIBUTE_READ_ONLY, "Read only"}
    };

    for(u32 i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if(attributes & names[i].flag) {
            size_t len = strlen(out);
            snprintf(out + len, size - len, "%s%s", len > 0 ? ", " : "", _(names[i].name));
        }
    }

    if(out[0] == '\0') {
        snprintf(out, size, "%s", _("None"));
    }
}

void task_draw_file_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    file_info* info = (file_info*) data;
    float x = x1 + 10;
    float y = y1 + 10;
    float w = x2 - x1 - 20;

    bool directory = (info->attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
    bool cia = !directory && info->isCia && info->ciaInfo.loaded;

    if(cia && info->ciaInfo.hasMeta) {
        task_draw_meta_card(&info->ciaInfo.meta, x, y, w);
    } else {
        task_draw_card(x, y, w, 0, directory ? ICON_FOLDER : info->isCia ? ICON_PACKAGE : info->isTicket ? ICON_TICKET : ICON_FILE, info->name, info->path, NULL);
    }

    char attributes[96];
    task_attributes_text(attributes, sizeof(attributes), info->attributes);

    task_cell cells[6];
    u32 count = 0;
    if(!directory) {
        char size[32];
        task_size_text(size, sizeof(size), info->size);
        task_set(&cells[count++], _("Size"), "%s", size);
    }

    task_set(&cells[count++], _("Attributes"), "%s", attributes);

    if(cia) {
        char version[48];
        char region[64];
        char installed[32];
        task_version_text(version, sizeof(version), info->ciaInfo.version);
        task_size_text(installed, sizeof(installed), info->ciaInfo.installedSize);
        if(info->ciaInfo.hasMeta) {
            smdh_region_to_string(region, info->ciaInfo.meta.region, sizeof(region));
        } else {
            snprintf(region, sizeof(region), "%s", _("Unknown"));
        }

        task_set(&cells[count], _("Title ID"), "%016llX", info->ciaInfo.titleId);
        task_wide(&cells[count++]);
        task_set(&cells[count++], _("Version"), "%s", version);
        task_set(&cells[count++], _("Region"), "%s", region);
        task_set(&cells[count++], _("Installed size"), "%s", installed);
    } else if(!directory && info->isTicket && info->ticketInfo.loaded) {
        task_set(&cells[count], _("Title ID"), "%016llX", info->ticketInfo.titleId);
        task_wide(&cells[count++]);
    }

    task_draw_cells(x, y + CARD_HEIGHT + 8, w, cells, count);
}

void task_draw_pending_title_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    pending_title_info* info = (pending_title_info*) data;
    float x = x1 + 10;
    float w = x2 - x1 - 20;

    char id[32];
    snprintf(id, sizeof(id), "%016llX", info->titleId);
    task_draw_card(x, y1 + 10, w, 0, ICON_CLOCK, _("Pending title"), id, NULL);

    char version[48];
    task_version_text(version, sizeof(version), info->version);

    task_cell cells[2];
    task_set(&cells[0], _("Memory"), "%s", task_media_name(info->mediaType));
    task_set(&cells[1], _("Version"), "%s", version);
    task_draw_cells(x, y1 + 10 + CARD_HEIGHT + 8, w, cells, 2);
}

void task_draw_system_save_data_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    system_save_data_info* info = (system_save_data_info*) data;
    float x = x1 + 10;
    float w = x2 - x1 - 20;

    char id[32];
    snprintf(id, sizeof(id), "%08lX", info->systemSaveDataId);
    task_draw_card(x, y1 + 10, w, 0, ICON_DEVICE_FLOPPY, _("System save data"), id, NULL);

    task_cell cells[1];
    task_set(&cells[0], _("ID"), "%s", id);
    task_draw_cells(x, y1 + 10 + CARD_HEIGHT + 8, w, cells, 1);
}

void task_draw_ticket_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    ticket_info* info = (ticket_info*) data;
    if(!info->loaded) {
        return;
    }

    task_ticket_lookup(info);

    const theme_current* t = theme();
    float x = x1 + 10;
    float w = x2 - x1 - 20;

    char id[32];
    snprintf(id, sizeof(id), "%016llX", info->titleId);
    if(info->hasMeta) {
        task_draw_meta_card(&info->meta, x, y1 + 10, w);
    } else {
        task_draw_card(x, y1 + 10, w, 0, ICON_TICKET, info->inUse ? task_kind_name(info->titleId) : _("Ticket"), id, info->inUse ? NULL : _("No installed title"));
    }

    task_cell cells[4];
    u32 count = 0;
    task_set(&cells[count], _("Title ID"), "%s", id);
    task_wide(&cells[count++]);
    task_set(&cells[count++], _("Type"), "%s", task_kind_name(info->titleId));
    task_set(&cells[count++], _("State"), "%s", info->inUse ? _("In use") : _("Not in use"));
    if(info->inUse) {
        task_set(&cells[count++], _("Memory"), "%s", task_media_name(info->mediaType));
    }

    float bottom = task_draw_cells(x, y1 + 10 + CARD_HEIGHT + 8, w, cells, count);

    // What "in use" means, in a sentence.
    const char* note = info->inUse ? _("A title using it is installed: this is its licence.")
                                   : _("No installed title uses it: it is left from a deleted title, or the title was never installed.");
    screen_draw_text_wrap(note, x + 2, bottom + 10, WIDGET_SMALL_SCALE + 0.04f, t->palette->textMuted, x + w - 2);
}

void task_draw_title_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    title_info* info = (title_info*) data;
    float x = x1 + 10;
    float y = y1 + 10;
    float w = x2 - x1 - 20;

    char region[64];
    if(info->hasMeta) {
        task_draw_meta_card(&info->meta, x, y, w);
        smdh_region_to_string(region, info->meta.region, sizeof(region));
    } else {
        char id[32];
        snprintf(id, sizeof(id), "%016llX", info->titleId);
        task_draw_card(x, y, w, 0, ICON_APPS, info->productCode, id, NULL);
        snprintf(region, sizeof(region), "%s", _("Unknown"));
    }

    char version[48];
    char size[32];
    task_version_text(version, sizeof(version), info->version);
    task_size_text(size, sizeof(size), info->installedSize);

    task_cell cells[6];
    task_set(&cells[0], _("Title ID"), "%016llX", info->titleId);
    task_wide(&cells[0]);
    task_set(&cells[1], _("Version"), "%s", version);
    task_set(&cells[2], _("Size"), "%s", size);
    task_set(&cells[3], _("Product code"), "%s", info->productCode);
    task_set(&cells[4], _("Region"), "%s", region);
    task_set(&cells[5], _("Memory"), "%s", task_media_name(info->mediaType));
    task_draw_cells(x, y + CARD_HEIGHT + 8, w, cells, 6);
}
