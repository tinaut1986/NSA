#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "thumbs.h"
#include "viewers.h"
#include "../core/data/smdh.h"
#include "settings.h"
#include "transfer.h"

// Files: rows with an icon by kind (a CIA shows its title's icon), the path in
// the title bar and tools for the clipboard, filter and sort, search and the
// current folder's actions. A folder opens; a file opens a sheet of actions.

typedef enum {
    SORT_NAME,
    SORT_SIZE,
    SORT_KIND
} files_sort;

typedef struct {
    populate_files_data populateData;
    ui_view* view;

    bool populated;

    FS_ArchiveID archiveId;
    FS_Path archivePath;
    FS_Archive archive;

    bool showHidden;
    bool showDirectories;
    bool showFiles;
    bool showCias;
    bool showTickets;
    files_sort sort;

    // When the list was last sorted (the populate task adds items as it goes).
    u32 sortedCount;
    bool sortedFinished;

    char currDir[FILE_PATH_MAX];
    char title[64];

    // The folder just left by going up: selected once the parent is listed.
    char reselect[FILE_PATH_MAX];

    // What the action sheet is for.
    linked_list* actionItems;
    list_item* actionSelected;
    // The folder's actions are open: the top screen shows the folder.
    bool folderSheet;
} files_data;

#define ROW_HEIGHT 34
#define BASE_NAME "<current directory>"

static bool files_is_base(list_item* item) {
    return strncmp(item->name, BASE_NAME, LIST_ITEM_NAME_MAX) == 0 || strncmp(item->name, "<current file>", LIST_ITEM_NAME_MAX) == 0;
}

static bool files_is_dir(file_info* info) {
    return (info->attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
}

static bool files_filter(void* data, const char* name, u32 attributes) {
    files_data* listData = (files_data*) data;

    if((attributes & FS_ATTRIBUTE_HIDDEN) != 0 && !listData->showHidden) {
        return false;
    }

    if((attributes & FS_ATTRIBUTE_DIRECTORY) != 0) {
        return listData->showDirectories;
    } else {
        if((fs_filter_cias(NULL, name, attributes) && !listData->showCias) || (fs_filter_tickets(NULL, name, attributes) && !listData->showTickets)) {
            return false;
        }

        return listData->showFiles;
    }
}

// The current folder's item stays first in the list for its actions, but is not shown.
static bool files_visible(void* data, list_item* item) {
    return !files_is_base(item);
}

static list_item* files_base(linked_list* items) {
    list_item* first = (list_item*) linked_list_get(items, 0);
    return first != NULL && files_is_base(first) ? first : NULL;
}

static const char* files_archive_name(FS_ArchiveID id) {
    switch(id) {
        case ARCHIVE_SDMC: return "SD";
        case ARCHIVE_NAND_CTR_FS: return "CTR NAND";
        case ARCHIVE_NAND_TWL_FS: return "TWL NAND";
        case ARCHIVE_TWL_PHOTO: return "TWL Photo";
        case ARCHIVE_TWL_SOUND: return "TWL Sound";
        case ARCHIVE_USER_SAVEDATA:
        case ARCHIVE_SYSTEM_SAVEDATA: return _("Save data");
        default: return _("Extra data");
    }
}

// "SD /3ds/NSA/", or only the last folder when the whole path does not fit.
static void files_update_title(files_data* data) {
    const char* archive = files_archive_name(data->archiveId);
    snprintf(data->title, sizeof(data->title), "%s %s", archive, data->currDir);
    if(widget_text_width(data->title, WIDGET_TEXT_SCALE) <= 170) {
        return;
    }

    char name[FILE_NAME_MAX] = {'\0'};
    char dir[FILE_PATH_MAX];
    string_copy(dir, data->currDir, sizeof(dir));
    size_t len = strlen(dir);
    if(len > 1 && dir[len - 1] == '/') {
        dir[len - 1] = '\0';
    }

    string_get_path_file(name, dir, sizeof(name));
    snprintf(data->title, sizeof(data->title), "%s .../%s/", archive, name);
}

static void files_navigate(files_data* listData, linked_list* items, const char* path);

// ---- items ----

typedef enum {
    KIND_DIR,
    KIND_CIA,
    KIND_TICKET,
    KIND_FILE
} files_kind;

static files_kind files_kind_of(file_info* info) {
    return files_is_dir(info) ? KIND_DIR : info->isCia ? KIND_CIA : info->isTicket ? KIND_TICKET : KIND_FILE;
}


// What a file is by its extension: its icon and the name shown for its type.
typedef enum {
    TYPE_GENERIC,
    TYPE_TEXT,
    TYPE_CODE,
    TYPE_DATA,
    TYPE_IMAGE,
    TYPE_AUDIO,
    TYPE_VIDEO,
    TYPE_ARCHIVE,
    TYPE_PROGRAM,
    TYPE_ROM,
    TYPE_SAVE,
    TYPE_BINARY,
    TYPE_PDF
} files_type;

static files_type files_type_of(file_info* info) {
    const char* n = info->name;
    if(viewers_has_ext(n, "png jpg jpeg bmp gif tga mpo psd webp")) return TYPE_IMAGE;
    if(viewers_has_ext(n, "mp3 wav flac ogg oga bcstm bcwav bfstm m4a aac opus wma mid midi")) return TYPE_AUDIO;
    if(viewers_has_ext(n, "mp4 avi mkv mov wmv webm")) return TYPE_VIDEO;
    if(viewers_has_ext(n, "zip 7z rar gz tar bz2 xz")) return TYPE_ARCHIVE;
    if(viewers_has_ext(n, "3dsx elf")) return TYPE_PROGRAM;
    if(viewers_has_ext(n, "nds dsi gba gb gbc nes sfc smc gen md pce n64 z64 v64 3ds cci")) return TYPE_ROM;
    if(viewers_has_ext(n, "sav srm dsv eep fla mpk")) return TYPE_SAVE;
    if(viewers_has_ext(n, "pdf")) return TYPE_PDF;
    if(viewers_has_ext(n, "c h cpp hpp py lua sh js css html htm xml bat cmake")) return TYPE_CODE;
    if(viewers_has_ext(n, "ini cfg conf json yml yaml toml properties inf csv tsv")) return TYPE_DATA;
    if(viewers_has_ext(n, "txt md log nfo rtf srt") || viewers_kind_of(n) == VIEWER_TEXT) return TYPE_TEXT;
    if(viewers_has_ext(n, "firm bin cdc dat img nand")) return TYPE_BINARY;
    return TYPE_GENERIC;
}

static u32 files_type_icon(files_type type) {
    switch(type) {
        case TYPE_TEXT: return ICON_FILE_TEXT;
        case TYPE_CODE: return ICON_FILE_CODE;
        case TYPE_DATA: return ICON_FILE_SETTINGS;
        case TYPE_IMAGE: return ICON_PHOTO;
        case TYPE_AUDIO: return ICON_MUSIC;
        case TYPE_VIDEO: return ICON_MOVIE;
        case TYPE_ARCHIVE: return ICON_FILE_ZIP;
        case TYPE_PROGRAM: return ICON_APPS;
        case TYPE_ROM: return ICON_DEVICE_GAMEPAD_2;
        case TYPE_SAVE: return ICON_DEVICE_FLOPPY;
        case TYPE_BINARY: return ICON_CPU;
        case TYPE_PDF: return ICON_FILE_TYPE_PDF;
        default: return ICON_FILE;
    }
}

static const char* files_type_label(files_type type) {
    switch(type) {
        case TYPE_TEXT: return _("Text");
        case TYPE_CODE: return _("Code");
        case TYPE_DATA: return _("Data");
        case TYPE_IMAGE: return _("Picture");
        case TYPE_AUDIO: return _("Sound");
        case TYPE_VIDEO: return _("Video");
        case TYPE_ARCHIVE: return _("Compressed");
        case TYPE_PROGRAM: return _("Program");
        case TYPE_ROM: return _("Game ROM");
        case TYPE_SAVE: return _("Save file");
        case TYPE_BINARY: return _("Binary");
        case TYPE_PDF: return _("PDF");
        default: return _("File");
    }
}

// Folders the console and its tools make: their own icon.
static u32 files_folder_icon(const char* name) {
    static const struct { const char* names; u32 icon; } table[] = {
            {"3ds", ICON_APPS},
            {"nsa|fbi", ICON_3DS},
            {"cias", ICON_PACKAGE},
            {"roms|_nds|_gba|retroarch", ICON_DEVICE_GAMEPAD_2},
            {"dcim|photos", ICON_PHOTO},
            {"music", ICON_MUSIC},
            {"luma|boot9strap|gm9", ICON_CPU},
            {"saves", ICON_DEVICE_FLOPPY},
            {"themes|splashes", ICON_PALETTE},
            {"config", ICON_SETTINGS},
            {"nintendo 3ds|private", ICON_SHIELD_LOCK}
    };

    size_t len = strlen(name);
    for(u32 i = 0; i < sizeof(table) / sizeof(*table); i++) {
        const char* p = table[i].names;
        while(*p != '\0') {
            size_t n = strcspn(p, "|");
            if(n == len && strncasecmp(p, name, n) == 0) {
                return table[i].icon;
            }

            p += n;
            if(*p == '|') {
                p++;
            }
        }
    }

    return ICON_FOLDER;
}

static void files_draw_icon(file_info* info, float x, float y, float size) {
    const theme_current* t = theme();
    files_kind kind = files_kind_of(info);
    if(kind == KIND_CIA && info->ciaInfo.loaded && info->ciaInfo.hasMeta && info->ciaInfo.meta.texture != 0) {
        screen_draw_texture(info->ciaInfo.meta.texture, x, y, size, size);
        return;
    }

    if(kind == KIND_FILE && viewers_kind_of(info->name) == VIEWER_IMAGE) {
        u32 preview = thumbs_get(info->archive, info->path, info->size);
        if(preview != 0) {
            screen_draw_texture(preview, x, y, size, size);
            return;
        }
    }

    u32 back = kind == KIND_FILE ? t->palette->background : t->accentSoft;
    u32 ink = kind == KIND_FILE ? t->palette->textMuted : t->accent;
    u32 icon = kind == KIND_DIR ? files_folder_icon(info->name) : kind == KIND_CIA ? ICON_PACKAGE : kind == KIND_TICKET ? ICON_TICKET : files_type_icon(files_type_of(info));
    screen_fill_round_rect(x, y, size, size, size / 6, back);
    screen_draw_icon(icon, x + size * 0.2f, y + size * 0.2f, size * 0.6f, ink);
}

static void files_size_text(char* out, size_t size, u64 bytes) {
    snprintf(out, size, bytes < 1024 ? "%.0f %s" : "%.1f %s", ui_get_display_size(bytes), ui_get_display_size_units(bytes));
}

// The row's second line: what it is and how big.
static void files_subtitle(char* out, size_t size, file_info* info) {
    char bytes[32];
    files_size_text(bytes, sizeof(bytes), info->size);
    switch(files_kind_of(info)) {
        case KIND_DIR:
            snprintf(out, size, "%s", _("Folder"));
            break;
        case KIND_CIA:
            if(info->ciaInfo.loaded && info->ciaInfo.hasMeta) {
                snprintf(out, size, "%s   %s", info->ciaInfo.meta.shortDescription, bytes);
            } else {
                snprintf(out, size, "CIA   %s", bytes);
            }
            break;
        case KIND_TICKET:
            snprintf(out, size, "%s   %s", _("Ticket"), bytes);
            break;
        default:
            snprintf(out, size, "%s   %s", files_type_label(files_type_of(info)), bytes);
            break;
    }
}

static void files_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    file_info* info = (file_info*) item->data;

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float icon = height - 8;
    files_draw_icon(info, x + 4, y + 4, icon);

    float tx = x + 4 + icon + 8;
    float right = x + width - 8;
    if(files_is_dir(info)) {
        screen_draw_icon(ICON_CHEVRON_RIGHT, right - 14, y + (height - 14) / 2, 14, muted);
        right -= 18;
    }

    widget_text_fit(item->name, tx, y + 3, WIDGET_TEXT_SCALE, ink, right - tx);

    char subtitle[LIST_ITEM_NAME_MAX];
    files_subtitle(subtitle, sizeof(subtitle), info);
    widget_text_fit(subtitle, tx, y + height - 3 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, muted, right - tx);
}

// ---- top screen: the selected file, or the current folder ----

static void files_draw_cell(float x, float y, float w, const char* label, const char* value) {
    const theme_current* t = theme();
    float h = 34;
    screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, t->palette->border, t->palette->surface);
    screen_draw_text(label, x + 8, y + 3, WIDGET_SMALL_SCALE, t->palette->textMuted);
    float scale = widget_text_width(value, WIDGET_TEXT_SCALE) <= w - 14 ? WIDGET_TEXT_SCALE : WIDGET_SMALL_SCALE;
    widget_text_fit(value, x + 8, y + 3 + widget_text_height(WIDGET_SMALL_SCALE), scale, t->palette->text, w - 14);
}

// The card on top: icon, name, two muted lines and a badge.
static void files_draw_card(file_info* info, const char* name, const char* line1, const char* line2, const char* badge,
                            float x, float y, float w, float h) {
    const theme_current* t = theme();
    widget_card(x, y, w, h);

    // In 3D the icon stands a little in front of its card.
    float icon = 56;
    float cardDepth = screen_get_depth();
    screen_set_depth(cardDepth + 1);
    files_draw_icon(info, x + 10, y + (h - icon) / 2, icon);
    screen_set_depth(cardDepth);

    float badgeW = widget_text_width(badge, WIDGET_SMALL_SCALE) + 12;
    widget_badge(x + w - 10 - badgeW, y + 10, badge, t->accentSoft, t->accent);

    float tx = x + 10 + icon + 12;
    float right = x + w - 10;
    widget_text_fit(name, tx, y + 10, WIDGET_TITLE_SCALE, t->palette->text, right - badgeW - 8 - tx);
    float lineY = y + 14 + widget_text_height(WIDGET_TITLE_SCALE);
    widget_text_fit(line1, tx, lineY, WIDGET_TEXT_SCALE, t->palette->textMuted, right - tx);
    widget_text_fit(line2, tx, lineY + widget_text_height(WIDGET_TEXT_SCALE), WIDGET_SMALL_SCALE, t->palette->textMuted, right - tx);
}

static void files_draw_folder(files_data* data, linked_list* items, list_item* base, float x, float y, float w, float h) {
    u32 counts[4] = {0, 0, 0, 0};
    u32 hidden = 0;

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        if(files_is_base(item)) {
            continue;
        }

        file_info* info = (file_info*) item->data;
        counts[files_kind_of(info)]++;
        if(info->attributes & FS_ATTRIBUTE_HIDDEN) {
            hidden++;
        }
    }

    file_info* info = (file_info*) base->data;
    char name[FILE_NAME_MAX];
    if(strcmp(data->currDir, "/") == 0) {
        snprintf(name, sizeof(name), "%s", files_archive_name(data->archiveId));
    } else {
        snprintf(name, sizeof(name), "%s", info->name);
    }

    char clipboard[FILE_NAME_MAX] = {'\0'};
    if(clipboard_has_contents()) {
        string_get_path_file(clipboard, clipboard_get_path(), sizeof(clipboard));
    }

    files_draw_card(info, name, data->currDir, "", _("Folder"), x, y, w, 76);

    char values[6][FILE_NAME_MAX];
    snprintf(values[0], sizeof(values[0]), "%lu", counts[KIND_DIR]);
    snprintf(values[1], sizeof(values[1]), "%lu", counts[KIND_FILE]);
    snprintf(values[2], sizeof(values[2]), "%lu", counts[KIND_CIA]);
    snprintf(values[3], sizeof(values[3]), "%lu", counts[KIND_TICKET]);
    snprintf(values[4], sizeof(values[4]), "%lu", hidden);
    snprintf(values[5], sizeof(values[5]), "%s", clipboard[0] != '\0' ? clipboard : _("Empty"));
    const char* labels[6] = {_("Folders"), _("Files"), "CIA", _("Tickets"), _("Hidden"), _("Clipboard")};

    float gap = 6;
    float cellW = (w - 2 * gap) / 3;
    for(u32 i = 0; i < 6; i++) {
        files_draw_cell(x + (i % 3) * (cellW + gap), y + 76 + 8 + (i / 3) * (34 + gap), cellW, labels[i], values[i]);
    }
}

static void files_draw_file(file_info* info, const char* name, float x, float y, float w, float h) {
    char size[32];
    files_size_text(size, sizeof(size), info->size);

    float gap = 6;
    float cellW = (w - 2 * gap) / 3;
    float row1 = y + 76 + 8;
    float row2 = row1 + 34 + gap;

    if(files_kind_of(info) == KIND_CIA && info->ciaInfo.loaded) {
        cia_info* cia = &info->ciaInfo;
        const char* title = cia->hasMeta ? cia->meta.shortDescription : name;
        const char* publisher = cia->hasMeta ? cia->meta.publisher : "";
        files_draw_card(info, title, name, publisher, "CIA", x, y, w, 76);

        char id[32];
        char version[32];
        char installed[32];
        char region[64];
        snprintf(id, sizeof(id), "%016llX", cia->titleId);
        snprintf(version, sizeof(version), "%d.%d.%d (%hu)", (cia->version >> 10) & 0x3F, (cia->version >> 4) & 0x3F, cia->version & 0xF, cia->version);
        files_size_text(installed, sizeof(installed), cia->installedSize);
        if(cia->hasMeta) {
            smdh_region_to_string(region, cia->meta.region, sizeof(region));
        } else {
            snprintf(region, sizeof(region), "%s", _("Unknown"));
        }

        float idW = cellW * 1.4f;
        float smallW = (w - idW - 2 * gap) / 2;
        files_draw_cell(x, row1, idW, _("Title ID"), id);
        files_draw_cell(x + idW + gap, row1, smallW, _("Version"), version);
        files_draw_cell(x + idW + gap + smallW + gap, row1, smallW, _("Region"), region);
        files_draw_cell(x, row2, cellW, _("File size"), size);
        files_draw_cell(x + cellW + gap, row2, cellW, _("Installed size"), installed);
        files_draw_cell(x + 2 * (cellW + gap), row2, cellW, _("Type"), "CIA");
        return;
    }

    bool ticket = files_kind_of(info) == KIND_TICKET;
    const char* typeName = ticket ? _("Ticket") : files_type_label(files_type_of(info));
    files_draw_card(info, name, info->path, "", typeName, x, y, w, 76);

    files_draw_cell(x, row1, cellW, _("Size"), size);
    files_draw_cell(x + cellW + gap, row1, cellW, _("Type"), typeName);
    files_draw_cell(x + 2 * (cellW + gap), row1, cellW, _("Hidden file"), (info->attributes & FS_ATTRIBUTE_HIDDEN) ? _("Yes") : _("No"));
    if(ticket && info->ticketInfo.loaded) {
        char id[32];
        snprintf(id, sizeof(id), "%016llX", info->ticketInfo.titleId);
        files_draw_cell(x, row2, cellW * 1.4f, _("Title ID"), id);
    }
}

// The medium the archive lives on, or false for save and extra data.
static bool files_storage_media(FS_ArchiveID id, FS_SystemMediaType* media) {
    switch(id) {
        case ARCHIVE_SDMC: *media = SYSTEM_MEDIATYPE_SD; return true;
        case ARCHIVE_NAND_CTR_FS: *media = SYSTEM_MEDIATYPE_CTR_NAND; return true;
        case ARCHIVE_NAND_TWL_FS: *media = SYSTEM_MEDIATYPE_TWL_NAND; return true;
        case ARCHIVE_TWL_PHOTO: *media = SYSTEM_MEDIATYPE_TWL_PHOTO; return true;
        default: return false;
    }
}

// A bar along the bottom of the top screen: how full the medium is.
static void files_draw_storage(files_data* data, float x, float y, float w) {
    FS_SystemMediaType media;
    if(files_storage_media(data->archiveId, &media)) {
        widget_storage(media, x, y, w);
    }
}

static void files_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    files_data* listData = (files_data*) data;
    files_draw_storage(listData, x1 + 12, y2 - 26, x2 - x1 - 24);

    float x = x1 + 10;
    float y = y1 + 10;
    float w = x2 - x1 - 20;
    float h = y2 - y1 - 20;

    // With nothing selected (an empty folder) or the folder's actions open, the folder itself.
    bool folder = listData->folderSheet && ui_top() != listData->view;
    if(folder || selected == NULL || selected->data == NULL) {
        list_item* base = listData->actionItems != NULL ? files_base(listData->actionItems) : NULL;
        if(base != NULL) {
            files_draw_folder(listData, listData->actionItems, base, x, y, w, h);
        }

        return;
    }

    file_info* info = (file_info*) selected->data;
    if(files_is_dir(info)) {
        files_draw_card(info, selected->name, info->path, "", _("Folder"), x, y, w, 76);
    } else {
        files_draw_file(info, selected->name, x, y, w, h);
    }
}

// ---- actions ----

static void files_run(files_data* data, void (*action)(linked_list*, list_item*)) {
    action(data->actionItems, data->actionSelected);
}

static void files_run_filtered(files_data* data, void (*action)(linked_list*, list_item*, bool (*)(void*, const char*, u32), void*)) {
    action(data->actionItems, data->actionSelected, files_filter, data);
}

static void files_copy_item(files_data* data, bool contentsOnly) {
    file_info* info = (file_info*) data->actionSelected->data;

    Result res = 0;
    if(R_SUCCEEDED(res = clipboard_set_contents(data->archive, info->path, contentsOnly))) {
        prompt_display_notify(_("Copied"), contentsOnly ? _("The folder's contents are in the clipboard.") : files_is_dir(info) ? _("The folder is in the clipboard.") : _("The file is in the clipboard."),
                              COLOR_TEXT, info, task_draw_file_info, NULL);
    } else {
        error_display_res(info, task_draw_file_info, res, "Failed to copy to clipboard.");
    }
}

static void files_install_cia(void* data) { files_run(data, action_install_cia); }
static void files_install_cia_delete(void* data) { files_run(data, action_install_cia_delete); }
static void files_install_ticket(void* data) { files_run(data, action_install_ticket); }
static void files_install_ticket_delete(void* data) { files_run(data, action_install_ticket_delete); }
static void files_delete_file(void* data) { files_run(data, action_delete_file); }
static void files_rename(void* data) { files_run(data, action_rename); }
static void files_paste(void* data) {
    files_data* listData = (files_data*) data;
    action_paste_contents_in(listData->actionItems, listData->actionSelected, listData->archiveId);
}

static void files_new_folder(void* data) { files_run(data, action_new_folder); }
static void files_delete_dir(void* data) { files_run(data, action_delete_dir); }
static void files_delete_contents(void* data) { files_run(data, action_delete_dir_contents); }
static void files_delete_cias(void* data) { files_run(data, action_delete_dir_cias); }
static void files_delete_tickets(void* data) { files_run(data, action_delete_dir_tickets); }
static void files_install_cias(void* data) { files_run_filtered(data, action_install_cias); }
static void files_install_cias_delete(void* data) { files_run_filtered(data, action_install_cias_delete); }
static void files_install_tickets(void* data) { files_run_filtered(data, action_install_tickets); }
static void files_install_tickets_delete(void* data) { files_run_filtered(data, action_install_tickets_delete); }
static void files_cut(void* data) {
    files_data* listData = (files_data*) data;
    file_info* info = (file_info*) listData->actionSelected->data;

    Result res = 0;
    if(R_SUCCEEDED(res = clipboard_set_cut(listData->archive, listData->archiveId, info->path))) {
        prompt_display_notify(_("Cut"), files_is_dir(info) ? _("The folder will be moved when you paste it.") : _("The file will be moved when you paste it."),
                              COLOR_TEXT, info, task_draw_file_info, NULL);
    } else {
        error_display_res(info, task_draw_file_info, res, "Failed to copy to clipboard.");
    }
}

static void files_copy(void* data) { files_copy_item(data, false); }
static void files_copy_contents(void* data) { files_copy_item(data, true); }
static void files_send(void* data) {
    file_info* info = (file_info*) ((files_data*) data)->actionSelected->data;
    transfer_send_file(info->archive, info->path, info->name, info->size);
}

static void files_open_text(void* data) {
    file_info* info = (file_info*) ((files_data*) data)->actionSelected->data;
    textview_open(info->archive, info->path, info->name, false);
}

// The pictures of the folder (in the list's order) with `selected` among them; opens the viewer.
static void files_open_media_in(files_data* data, linked_list* items, list_item* selected, viewer_kind kind) {
    u32 total = linked_list_size(items);
    const char** paths = (const char**) calloc(total > 0 ? total : 1, sizeof(char*));
    if(paths == NULL) {
        error_display(NULL, NULL, "Failed to allocate picture viewer data.");
        return;
    }

    u32 count = 0;
    u32 index = 0;
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        if(files_is_base(item) || item->data == NULL) {
            continue;
        }

        file_info* info = (file_info*) item->data;
        if(!files_is_dir(info) && viewers_kind_of(info->name) == kind) {
            if(item == selected) {
                index = count;
            }

            paths[count++] = info->path;
        }
    }

    if(kind == VIEWER_AUDIO) {
        audioview_open(((file_info*) selected->data)->archive, paths, count, index);
    } else {
        imageview_open(((file_info*) selected->data)->archive, paths, count, index);
    }

    free(paths);
}

static void files_open_image(void* data) {
    files_data* listData = (files_data*) data;
    files_open_media_in(listData, listData->actionItems, listData->actionSelected, VIEWER_IMAGE);
}

static void files_open_audio(void* data) {
    files_data* listData = (files_data*) data;
    files_open_media_in(listData, listData->actionItems, listData->actionSelected, VIEWER_AUDIO);
}

static void files_refresh(void* data) { ((files_data*) data)->populated = false; }

static void files_file_actions_open(files_data* data, linked_list* items, list_item* selected) {
    file_info* info = (file_info*) selected->data;
    data->actionItems = items;
    data->actionSelected = selected;

    sheet_action actions[SHEET_ACTIONS_MAX];
    u32 count = 0;

    viewer_kind viewer = viewers_kind_of(info->name);
    if(viewer == VIEWER_IMAGE) {
        actions[count++] = (sheet_action) {ICON_PHOTO, _("View picture"), false, files_open_image};
    }

    if(viewer == VIEWER_AUDIO) {
        actions[count++] = (sheet_action) {ICON_MUSIC, _("Play"), false, files_open_audio};
    }

    if(viewer == VIEWER_TEXT) {
        actions[count++] = (sheet_action) {ICON_FILE_TEXT, _("Open text"), false, files_open_text};
    } else if(viewer == VIEWER_NONE && !info->isCia && !info->isTicket) {
        actions[count++] = (sheet_action) {ICON_FILE_TEXT, _("Open as text"), false, files_open_text};
    }

    if(info->isCia) {
        actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Install"), false, files_install_cia};
        actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Install and delete"), false, files_install_cia_delete};
    }

    if(info->isTicket) {
        actions[count++] = (sheet_action) {ICON_TICKET, _("Install ticket"), false, files_install_ticket};
        actions[count++] = (sheet_action) {ICON_TICKET, _("Install and delete ticket"), false, files_install_ticket_delete};
    }

    actions[count++] = (sheet_action) {ICON_3DS, _("Send to another 3DS"), false, files_send};
    actions[count++] = (sheet_action) {ICON_COPY, _("Copy"), false, files_copy};
    actions[count++] = (sheet_action) {ICON_SCISSORS, _("Cut (move)"), false, files_cut};
    actions[count++] = (sheet_action) {ICON_PENCIL, _("Rename"), false, files_rename};
    actions[count++] = (sheet_action) {ICON_TRASH, _("Delete"), true, files_delete_file};

    sheet_display_list(NULL, 0, actions, count, data);
}

static void files_open_dir(void* data) {
    files_data* listData = (files_data*) data;
    files_navigate(listData, listData->actionItems, ((file_info*) listData->actionSelected->data)->path);
}

// A folder in the list (not the current one): what can be done without
// listing it. Its contents' actions are in its own ⋯ once opened.
static void files_dir_actions_open(files_data* data, linked_list* items, list_item* selected) {
    data->actionItems = items;
    data->actionSelected = selected;

    sheet_action actions[] = {
            {ICON_FOLDER, _("Open"), false, files_open_dir},
            {ICON_COPY, _("Copy folder"), false, files_copy},
            {ICON_COPY, _("Copy contents"), false, files_copy_contents},
            {ICON_SCISSORS, _("Cut folder (move)"), false, files_cut},
            {ICON_PENCIL, _("Rename folder"), false, files_rename},
            {ICON_TRASH, _("Delete contents"), true, files_delete_contents},
            {ICON_TRASH_X, _("Delete folder"), true, files_delete_dir}
    };

    sheet_display_list(NULL, 0, actions, sizeof(actions) / sizeof(*actions), data);
}

static void files_folder_actions_open(files_data* data, linked_list* items) {
    list_item* base = files_base(items);
    if(base == NULL) {
        return;
    }

    data->actionItems = items;
    data->actionSelected = base;
    data->folderSheet = true;

    bool cias = false;
    bool tickets = false;
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        file_info* info = (file_info*) ((list_item*) linked_list_iter_next(&iter))->data;
        cias = cias || info->isCia;
        tickets = tickets || info->isTicket;
    }

    bool root = strcmp(data->currDir, "/") == 0;

    sheet_action actions[SHEET_ACTIONS_MAX];
    u32 count = 0;

    if(cias) {
        actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Install all CIAs"), false, files_install_cias};
        actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Install and delete all CIAs"), false, files_install_cias_delete};
    }

    if(tickets) {
        actions[count++] = (sheet_action) {ICON_TICKET, _("Install all tickets"), false, files_install_tickets};
        actions[count++] = (sheet_action) {ICON_TICKET, _("Install and delete all tickets"), false, files_install_tickets_delete};
    }

    actions[count++] = (sheet_action) {ICON_FOLDER_PLUS, _("New folder"), false, files_new_folder};
    if(clipboard_has_contents()) {
        actions[count++] = (sheet_action) {ICON_CLIPBOARD, _("Paste here"), false, files_paste};
    }

    actions[count++] = (sheet_action) {ICON_COPY, _("Copy contents"), false, files_copy_contents};
    if(!root) {
        actions[count++] = (sheet_action) {ICON_COPY, _("Copy folder"), false, files_copy};
        actions[count++] = (sheet_action) {ICON_SCISSORS, _("Cut folder (move)"), false, files_cut};
        actions[count++] = (sheet_action) {ICON_PENCIL, _("Rename folder"), false, files_rename};
    }

    actions[count++] = (sheet_action) {ICON_REFRESH, _("Refresh"), false, files_refresh};

    if(cias) {
        actions[count++] = (sheet_action) {ICON_TRASH, _("Delete all CIAs"), true, files_delete_cias};
    }

    if(tickets) {
        actions[count++] = (sheet_action) {ICON_TRASH, _("Delete all tickets"), true, files_delete_tickets};
    }

    actions[count++] = (sheet_action) {ICON_TRASH, _("Delete contents"), true, files_delete_contents};
    if(!root) {
        actions[count++] = (sheet_action) {ICON_TRASH_X, _("Delete folder"), true, files_delete_dir};
    }

    sheet_display_list(NULL, 0, actions, count, data);
}

// ---- tools: clipboard, filter and sort, search, folder actions ----

static bool files_clipboard_active(ui_view* view, void* data) {
    return clipboard_has_contents();
}

static linked_list* files_items(files_data* data) {
    return data->actionItems;
}

static void files_clipboard_press(ui_view* view, void* data) {
    files_data* listData = (files_data*) data;
    list_item* base = files_base(files_items(listData));
    if(!clipboard_has_contents() || base == NULL) {
        prompt_display_notify(_("Clipboard"), _("The clipboard is empty. Copy or cut a file or folder from its actions first."), COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    listData->actionSelected = base;
    files_paste(listData);
}

static bool files_filter_active(ui_view* view, void* data) {
    files_data* listData = (files_data*) data;
    return listData->showHidden || !listData->showDirectories || !listData->showFiles || !listData->showCias || !listData->showTickets;
}

static bool files_popover_is_on(void* data, u32 group, u32 option) {
    files_data* listData = (files_data*) data;
    if(group == 0) {
        bool* flags[5] = {&listData->showDirectories, &listData->showFiles, &listData->showCias, &listData->showTickets, &listData->showHidden};
        return *flags[option];
    }

    return listData->sort == (files_sort) option;
}

// What the screen shows is kept in the settings, for the next time.
static void files_store(files_data* data) {
    files_settings* saved = &settings_get()->files;
    saved->directories = data->showDirectories;
    saved->files = data->showFiles;
    saved->cias = data->showCias;
    saved->tickets = data->showTickets;
    saved->hidden = data->showHidden;
    saved->sort = (u32) data->sort;
    settings_save();
}

static void files_popover_select(void* data, u32 group, u32 option) {
    files_data* listData = (files_data*) data;
    if(group == 0) {
        bool* flags[5] = {&listData->showDirectories, &listData->showFiles, &listData->showCias, &listData->showTickets, &listData->showHidden};
        *flags[option] = !*flags[option];
        listData->populated = false;
    } else {
        // Sorting needs no new listing.
        listData->sort = (files_sort) option;
        listData->sortedCount = 0;
        listData->sortedFinished = false;
    }

    files_store(listData);
}

static void files_filter_open(ui_view* view, void* data) {
    popover_def def = {
            .groupCount = 2,
            .groups = {
                    {_("Show"), true, 5, {_("Folders"), _("Files"), "CIA", _("Tickets"), _("Hidden")}, {ICON_FOLDER, ICON_FILE, ICON_PACKAGE, ICON_TICKET, -1}},
                    {_("Sort by"), false, 3, {_("Name"), _("Size"), _("Type")}, {-1, -1, -1}}
            },
            .isOn = files_popover_is_on,
            .select = files_popover_select
    };

    popover_display(&def, data);
}

static bool files_search_active(ui_view* view, void* data) {
    return list_get_search(view)[0] != '\0';
}

static void files_search_done(ui_view* kbdView, void* data, SwkbdButton button, const char* response) {
    files_data* listData = (files_data*) data;
    if(button == SWKBD_BUTTON_CONFIRM) {
        list_set_search(listData->view, response);
    }
}

static void files_search_open(ui_view* view, void* data) {
    kbd_display(_("Search files"), list_get_search(view), SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, 63, data, files_search_done);
}

static void files_folder_press(ui_view* view, void* data) {
    files_data* listData = (files_data*) data;
    files_folder_actions_open(listData, files_items(listData));
}

// ---- sorting: folders first, then by name, size or kind ----

static int files_compare(void* data, const void* p1, const void* p2) {
    files_data* listData = (files_data*) data;
    list_item* item1 = (list_item*) p1;
    list_item* item2 = (list_item*) p2;

    bool base1 = files_is_base(item1);
    bool base2 = files_is_base(item2);
    if(base1 != base2) {
        return base1 ? -1 : 1;
    }

    file_info* f1 = (file_info*) item1->data;
    file_info* f2 = (file_info*) item2->data;
    files_kind k1 = files_kind_of(f1);
    files_kind k2 = files_kind_of(f2);
    if((k1 == KIND_DIR) != (k2 == KIND_DIR)) {
        return k1 == KIND_DIR ? -1 : 1;
    }

    if(listData->sort == SORT_KIND && k1 != k2) {
        return k1 < k2 ? -1 : 1;
    }

    if(listData->sort == SORT_SIZE && k1 != KIND_DIR && f1->size != f2->size) {
        return f1->size > f2->size ? -1 : 1;
    }

    return strncasecmp(f1->name, f2->name, FILE_NAME_MAX);
}

static void files_sort_items(files_data* data, linked_list* items) {
    u32 count = linked_list_size(items);
    bool finished = data->populateData.finished;
    if(!finished || (count == data->sortedCount && finished == data->sortedFinished)) {
        return;
    }

    data->sortedCount = count;
    data->sortedFinished = finished;
    linked_list_sort(items, data, files_compare);
}

// ---- the list ----

// A long hold on an item: its actions, a folder's too.
static void files_long_press(ui_view* view, void* data, linked_list* items, list_item* item) {
    files_data* listData = (files_data*) data;
    if(item->data == NULL) {
        return;
    }

    if(files_is_dir((file_info*) item->data)) {
        files_dir_actions_open(listData, items, item);
    } else {
        files_file_actions_open(listData, items, item);
    }
}

static void files_repopulate(files_data* listData, linked_list* items) {
    if(!listData->populateData.finished) {
        svcSignalEvent(listData->populateData.cancelEvent);
        while(!listData->populateData.finished) {
            svcSleepThread(1000000);
        }
    }

    listData->populateData.items = items;
    listData->populateData.archive = listData->archive;
    string_copy(listData->populateData.path, listData->currDir, FILE_PATH_MAX);

    Result res = task_populate_files(&listData->populateData);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to initiate file list population.");
    }

    listData->populated = true;
    listData->sortedCount = 0;
    listData->sortedFinished = false;
}

static void files_navigate(files_data* listData, linked_list* items, const char* path) {
    string_copy(listData->currDir, path, FILE_PATH_MAX);
    list_set_search(listData->view, "");

    listData->populated = false;
}

static void files_free_data(files_data* data) {
    if(!data->populateData.finished) {
        svcSignalEvent(data->populateData.cancelEvent);
        while(!data->populateData.finished) {
            svcSleepThread(1000000);
        }
    }

    if(data->archive != 0) {
        fs_close_archive(data->archive);
        data->archive = 0;
    }

    if(data->archivePath.data != NULL) {
        free((void*) data->archivePath.data);
        data->archivePath.data = NULL;
    }

    free(data);
}

static void files_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    files_data* listData = (files_data*) data;
    listData->actionItems = items;
    listData->folderSheet = false;

    if(listData->populated) {
        // Detect whether the current directory was renamed by an action.
        list_item* currDirItem = linked_list_get(items, 0);
        if(currDirItem != NULL && strncmp(listData->currDir, ((file_info*) currDirItem->data)->path, FILE_PATH_MAX) != 0) {
            string_copy(listData->currDir, ((file_info*) currDirItem->data)->path, FILE_PATH_MAX);
        }
    }

    while(!fs_is_dir(listData->archive, listData->currDir)) {
        char parentDir[FILE_PATH_MAX] = {'\0'};
        string_get_parent_path(parentDir, listData->currDir, FILE_PATH_MAX);

        files_navigate(listData, items, parentDir);
    }

    files_update_title(listData);
    view->name = listData->title;
    view->info = _(" open    folder    filter    search    up");

    if(hidKeysDown() & KEY_B) {
        // B first clears a search.
        if(list_get_search(view)[0] != '\0') {
            list_set_search(view, "");
            return;
        }

        if(strncmp(listData->currDir, "/", FILE_PATH_MAX) == 0) {
            ui_pop();

            files_free_data(listData);

            task_clear_files(items);
            list_destroy(view);

            return;
        } else {
            char parentDir[FILE_PATH_MAX] = {'\0'};
            string_get_parent_path(parentDir, listData->currDir, FILE_PATH_MAX);

            string_copy(listData->reselect, listData->currDir, FILE_PATH_MAX);
            files_navigate(listData, items, parentDir);
        }
    }

    if(hidKeysDown() & (KEY_L | KEY_SELECT)) {
        files_filter_open(view, listData);
        return;
    }

    if(hidKeysDown() & KEY_R) {
        files_search_open(view, listData);
        return;
    }

    if(!listData->populated) {
        files_repopulate(listData, items);
    }

    files_sort_items(listData, items);

    if(listData->reselect[0] != '\0' && listData->populated && listData->populateData.finished) {
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            list_item* item = (list_item*) linked_list_iter_next(&iter);
            if(!files_is_base(item) && strncmp(((file_info*) item->data)->path, listData->reselect, FILE_PATH_MAX) == 0) {
                list_select(view, item);
                break;
            }
        }

        listData->reselect[0] = '\0';
    }

    if(listData->populateData.finished && R_FAILED(listData->populateData.result)) {
        error_display_res(NULL, NULL, listData->populateData.result, "Failed to populate file list.");

        listData->populateData.result = 0;
    }

    if(hidKeysDown() & KEY_X) {
        files_folder_actions_open(listData, items);
        return;
    }

    if(selected != NULL && selected->data != NULL && selectedTouched) {
        file_info* fileInfo = (file_info*) selected->data;

        viewer_kind viewer = files_is_dir(fileInfo) ? VIEWER_NONE : viewers_kind_of(fileInfo->name);
        if(files_is_dir(fileInfo)) {
            files_navigate(listData, items, fileInfo->path);
        } else if(viewer == VIEWER_IMAGE) {
            files_open_media_in(listData, items, selected, VIEWER_IMAGE);
        } else if(viewer == VIEWER_AUDIO) {
            files_open_media_in(listData, items, selected, VIEWER_AUDIO);
        } else if(viewer == VIEWER_TEXT) {
            textview_open(fileInfo->archive, fileInfo->path, fileInfo->name, false);
        } else {
            files_file_actions_open(listData, items, selected);
        }

        return;
    }
}

void files_open(FS_ArchiveID archiveId, FS_Path archivePath) {
    files_data* data = (files_data*) calloc(1, sizeof(files_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate files data.");

        return;
    }

    data->populateData.recursive = false;
    data->populateData.includeBase = true;
    data->populateData.meta = true;

    data->populateData.filter = files_filter;
    data->populateData.filterData = data;

    data->populateData.finished = true;

    data->populated = false;

    files_settings* saved = &settings_get()->files;
    data->showHidden = saved->hidden;
    data->showDirectories = saved->directories;
    data->showFiles = saved->files;
    data->showCias = saved->cias;
    data->showTickets = saved->tickets;
    data->sort = saved->sort <= SORT_KIND ? (files_sort) saved->sort : SORT_NAME;

    data->archiveId = archiveId;
    data->archivePath.type = archivePath.type;
    data->archivePath.size = archivePath.size;
    if(archivePath.data != NULL) {
        data->archivePath.data = calloc(1, data->archivePath.size);
        if(data->archivePath.data == NULL) {
            error_display(NULL, NULL, "Failed to allocate files data.");

            files_free_data(data);
            return;
        }

        memcpy((void*) data->archivePath.data, archivePath.data, data->archivePath.size);
    } else {
        data->archivePath.data = NULL;
    }

    snprintf(data->currDir, FILE_PATH_MAX, "/");
    files_update_title(data);

    Result res = 0;
    if(R_FAILED(res = fs_open_archive(&data->archive, archiveId, archivePath))) {
        error_display_res(NULL, NULL, res, "Failed to open file listing archive.");

        files_free_data(data);
        return;
    }

    ui_view* view = list_display(data->title, "", data, files_update, files_draw_top);
    if(view == NULL) {
        files_free_data(data);
        return;
    }

    data->view = view;

    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = files_draw_row
    };
    list_set_style(view, &style);
    list_set_filter(view, files_visible);
    list_set_long_press(view, files_long_press);

    // Right to left.
    list_add_tool(view, ICON_DOTS, NULL, files_folder_press);
    list_add_tool(view, ICON_SEARCH, files_search_active, files_search_open);
    list_add_tool(view, ICON_FILTER, files_filter_active, files_filter_open);
    list_add_tool(view, ICON_CLIPBOARD, files_clipboard_active, files_clipboard_press);
}

void files_open_sd() {
    files_open(ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
}

void files_open_ctr_nand() {
    files_open(ARCHIVE_NAND_CTR_FS, fsMakePath(PATH_EMPTY, ""));
}

void files_open_twl_nand() {
    files_open(ARCHIVE_NAND_TWL_FS, fsMakePath(PATH_EMPTY, ""));
}

void files_open_twl_photo() {
    files_open(ARCHIVE_TWL_PHOTO, fsMakePath(PATH_EMPTY, ""));
}

void files_open_twl_sound() {
    files_open(ARCHIVE_TWL_SOUND, fsMakePath(PATH_EMPTY, ""));
}
