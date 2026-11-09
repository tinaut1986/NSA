#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "chain.h"
#include "section.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "../core/data/smdh.h"
#include "settings.h"

// Titles: rows with each title's icon, or a grid of icons. The title bar's
// tools switch the view, filter and sort, and search; A or a tap opens a
// sheet with the title's actions.

typedef enum {
    SORT_NAME,
    SORT_ID,
    SORT_SIZE
} titles_sort;

typedef struct {
    populate_titles_data populateData;
    ui_view* view;

    bool showGameCard;
    bool showSD;
    bool showNAND;
    titles_sort sort;
    bool sortReverse;   // The sort turned round (choosing the same sort again).
    bool group;

    // Hidden in place (no new listing): by kind (title_kind) and by region.
    bool showKind[6];
    bool showRegion[5];

    bool populated;

    // When the list last had its grouping worked out.
    u32 groupedCount;
    bool groupedFinished;

    // Sorted as titles load (and once more when all have).
    bool sorted;
    u32 sortedLoaded;

    // The list's items, for the top screen's linked titles.
    linked_list* items;

    // The title the action sheet is for.
    linked_list* actionItems;
    list_item* actionSelected;
} titles_data;

#define ROW_HEIGHT 40
#define TILE_SIZE 50

static const char* titles_media_name(FS_MediaType mediaType) {
    return mediaType == MEDIATYPE_NAND ? "NAND" : mediaType == MEDIATYPE_SD ? "SD" : _("Game card");
}

static const char* titles_kind_name(title_kind kind) {
    switch(kind) {
        case TITLE_KIND_GAME: return _("Game");
        case TITLE_KIND_UPDATE: return _("Update");
        case TITLE_KIND_DLC: return "DLC";
        case TITLE_KIND_DEMO: return _("Demo");
        case TITLE_KIND_DSIWARE: return "DSiWare";
        default: return _("System");
    }
}

static bool titles_is_extra(title_kind kind) {
    return kind == TITLE_KIND_UPDATE || kind == TITLE_KIND_DLC;
}

// Works out which updates and DLC hang from a game in the list, when the list
// has changed (the populate task adds items as it goes).
static void titles_group(titles_data* data, linked_list* items) {
    u32 count = linked_list_size(items);
    bool finished = data->populateData.finished;
    if(count == data->groupedCount && finished == data->groupedFinished) {
        return;
    }

    data->groupedCount = count;
    data->groupedFinished = finished;

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        title_info* info = (title_info*) ((list_item*) linked_list_iter_next(&iter))->data;
        info->linkedCount = 0;
        info->linked = false;
    }

    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        title_info* extra = (title_info*) ((list_item*) linked_list_iter_next(&iter))->data;
        if(!titles_is_extra(title_kind_of(extra->titleId))) {
            continue;
        }

        linked_list_iter games;
        linked_list_iterate(items, &games);
        while(linked_list_iter_has_next(&games)) {
            title_info* game = (title_info*) ((list_item*) linked_list_iter_next(&games))->data;
            if(title_kind_of(game->titleId) == TITLE_KIND_GAME && (u32) game->titleId == (u32) extra->titleId) {
                game->linkedCount++;
                extra->linked = true;
                break;
            }
        }
    }
}

typedef enum {
    REGION_FREE,
    REGION_EUROPE,
    REGION_AMERICA,
    REGION_JAPAN,
    REGION_OTHER
} titles_region;

#define SMDH_REGION_FREE 0x7FFFFFFF

static bool titles_region_shown(titles_data* data, title_info* info) {
    // Without a banner (most DLC) there is no region to go by.
    if(!info->hasMeta) {
        return true;
    }

    u32 region = info->meta.region;
    if((region & SMDH_REGION_FREE) == SMDH_REGION_FREE) {
        return data->showRegion[REGION_FREE];
    }

    return (data->showRegion[REGION_JAPAN] && (region & 0x01))
           || (data->showRegion[REGION_AMERICA] && (region & 0x02))
           || (data->showRegion[REGION_EUROPE] && (region & (0x04 | 0x08)))
           || (data->showRegion[REGION_OTHER] && (region & (0x10 | 0x20 | 0x40)));
}

// Kind and region filters, and grouping: updates and DLC whose game is in the
// list hide behind it, while games are shown.
static bool titles_visible(void* data, list_item* item) {
    titles_data* listData = (titles_data*) data;
    title_info* info = (title_info*) item->data;

    if(!listData->showKind[title_kind_of(info->titleId)] || !titles_region_shown(listData, info)) {
        return false;
    }

    return !listData->group || !info->linked || !listData->showKind[TITLE_KIND_GAME];
}

// ---- items ----

static void titles_draw_icon(title_info* info, float x, float y, float size) {
    const theme_current* t = theme();
    if(info->hasMeta && info->meta.texture != 0) {
        screen_draw_texture(info->meta.texture, x, y, size, size);
    } else {
        widget_icon_placeholder(x, y, size, info->twl ? ICON_DEVICE_FLOPPY : ICON_APPS, t->accentSoft, t->accent);
    }
}

static void titles_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    title_info* info = (title_info*) item->data;

    u32 ink = 0;
    u32 muted = 0;
    widget_item_frame(x, y, width, height, selected, &ink, &muted);

    if(!info->loaded) {
        widget_skeleton(x + 5, y + 5, width - 18, height - 10, height - 10, selected);
        return;
    }

    widget_fade_in(info->loadedTime);

    float icon = height - 10;
    titles_draw_icon(info, x + 5, y + 5, icon);

    titles_data* listData = (titles_data*) data;
    if(listData != NULL && listData->group && info->linkedCount > 0) {
        widget_count_badge(info->linkedCount, x + 5 + icon - 1, y + 7);
    }

    float tx = x + 5 + icon + 8;
    float right = x + width - 8;

    // Anything but a game says what it is.
    title_kind kind = title_kind_of(info->titleId);
    float nameRight = right;
    if(kind != TITLE_KIND_GAME) {
        const char* tag = titles_kind_name(kind);
        float tagW = widget_badge_right(right, y + 4, tag, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft, selected ? t->onAccent : t->accent);
        nameRight = right - tagW - 6;
    }

    widget_text_fit(item->name, tx, y + 4, WIDGET_TEXT_SCALE, ink, nameRight - tx);

    // Second line: description on the left, publisher on the right.
    float lineY = y + height - 4 - widget_text_height(WIDGET_SMALL_SCALE);
    const char* description = info->hasMeta ? info->meta.longDescription : titles_media_name(info->mediaType);
    const char* publisher = info->hasMeta ? info->meta.publisher : "";
    float publisherWidth = widget_text_width(publisher, WIDGET_SMALL_SCALE);
    float maxPublisher = (right - tx) * 0.45f;
    if(publisherWidth > maxPublisher) {
        publisherWidth = maxPublisher;
    }

    widget_text_fit(publisher, right - publisherWidth, lineY, WIDGET_SMALL_SCALE, muted, publisherWidth);
    widget_text_fit(description, tx, lineY, WIDGET_SMALL_SCALE, muted, right - publisherWidth - 8 - tx);
    screen_set_content_motion(0, 0, 0xFF);
}

static void titles_draw_tile(void* data, list_item* item, float x, float y, float size, bool selected) {
    title_info* info = (title_info*) item->data;

    widget_tile_frame(x, y, size, selected);

    float icon = size - 8;
    if(!info->loaded) {
        widget_skeleton(x + 4, y + 4, icon, icon, icon, selected);
        return;
    }

    widget_fade_in(info->loadedTime);
    titles_draw_icon(info, x + 4, y + 4, icon);
    screen_set_content_motion(0, 0, 0xFF);

    titles_data* listData = (titles_data*) data;
    if(listData != NULL && listData->group && info->linkedCount > 0) {
        widget_count_badge(info->linkedCount, x + size - 4, y + 4);
    }
}

// ---- top screen: the selected title ----

// The updates and DLC of a game, as badges under its details.
static void titles_draw_linked(linked_list* items, title_info* game, float x, float y, float right) {
    const theme_current* t = theme();
    if(items == NULL || title_kind_of(game->titleId) != TITLE_KIND_GAME) {
        return;
    }

    float bx = x + widget_text_width(_("Linked:"), WIDGET_SMALL_SCALE) + 8;
    bool any = false;

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        title_info* other = (title_info*) ((list_item*) linked_list_iter_next(&iter))->data;
        title_kind kind = title_kind_of(other->titleId);
        if(!titles_is_extra(kind) || (u32) other->titleId != (u32) game->titleId) {
            continue;
        }

        char text[48];
        if(kind == TITLE_KIND_UPDATE) {
            snprintf(text, sizeof(text), "%s %d.%d.%d", titles_kind_name(kind), (other->version >> 10) & 0x3F, (other->version >> 4) & 0x3F, other->version & 0xF);
        } else {
            snprintf(text, sizeof(text), "%s %.0f %s", titles_kind_name(kind), ui_get_display_size(other->installedSize), ui_get_display_size_units(other->installedSize));
        }

        float w = widget_text_width(text, WIDGET_SMALL_SCALE) + 12;
        if(bx + w > right) {
            break;
        }

        widget_badge(bx, y, text, t->accentSoft, t->accent);
        bx += w + 6;
        any = true;
    }

    screen_draw_text(any ? _("Linked:") : _("Nothing linked"), x, y + 1, WIDGET_SMALL_SCALE, t->palette->textMuted);
}

static void titles_draw_details(linked_list* items, list_item* selected, float x1, float y1, float x2, float y2);

static void titles_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    titles_draw_details(((titles_data*) data)->items, selected, x1, y1, x2, y2);
}

static void titles_draw_details(linked_list* items, list_item* selected, float x1, float y1, float x2, float y2) {
    if(selected == NULL || selected->data == NULL) {
        return;
    }

    const theme_current* t = theme();
    title_info* info = (title_info*) selected->data;

    // Not loaded yet: the card's place, with a word on why it is empty.
    if(!info->loaded) {
        widget_card(x1 + 10, y1 + 10, x2 - x1 - 20, 76);
        widget_skeleton(x1 + 20, y1 + 20, x2 - x1 - 40, 56, 56, false);
        widget_text_centered(_("Loading..."), x1, x2, y1 + 110, WIDGET_TEXT_SCALE, t->palette->textMuted);
        return;
    }

    float cardX = x1 + 10;
    float cardY = y1 + 10;
    float cardW = x2 - x1 - 20;
    float cardH = 76;
    widget_card(cardX, cardY, cardW, cardH);

    // In 3D the icon stands a little in front of its card.
    float icon = 56;
    float cardDepth = screen_get_depth();
    screen_set_depth(cardDepth + 1);
    titles_draw_icon(info, cardX + 10, cardY + (cardH - icon) / 2, icon);
    screen_set_depth(cardDepth);

    const char* media = titles_media_name(info->mediaType);
    float badgeW = widget_text_width(media, WIDGET_SMALL_SCALE) + 12;
    widget_badge(cardX + cardW - 10 - badgeW, cardY + 10, media, t->accentSoft, t->accent);

    float tx = cardX + 10 + icon + 12;
    float right = cardX + cardW - 10;
    widget_text_fit(selected->name, tx, cardY + 10, WIDGET_TITLE_SCALE, t->palette->text, right - badgeW - 8 - tx);
    if(info->hasMeta) {
        float lineH = widget_text_height(WIDGET_TEXT_SCALE);
        widget_text_fit(info->meta.longDescription, tx, cardY + 14 + widget_text_height(WIDGET_TITLE_SCALE), WIDGET_TEXT_SCALE, t->palette->textMuted, right - tx);
        float py = cardY + 14 + widget_text_height(WIDGET_TITLE_SCALE) + lineH;
        screen_draw_icon(ICON_BUILDING, tx, py + 1, 13, t->palette->textMuted);
        widget_text_fit(info->meta.publisher, tx + 17, py, WIDGET_SMALL_SCALE, t->palette->textMuted, right - tx - 17);
    }

    char id[32];
    char version[32];
    char size[32];
    char region[64];
    snprintf(id, sizeof(id), "%016llX", info->titleId);
    snprintf(version, sizeof(version), "%d.%d.%d (%hu)", (info->version >> 10) & 0x3F, (info->version >> 4) & 0x3F, info->version & 0xF, info->version);
    snprintf(size, sizeof(size), "%.1f %s", ui_get_display_size(info->installedSize), ui_get_display_size_units(info->installedSize));
    if(info->hasMeta) {
        smdh_region_to_string(region, info->meta.region, sizeof(region));
    } else {
        snprintf(region, sizeof(region), "%s", _("Unknown"));
    }

    float gap = 6;
    float cellW = (cardW - 2 * gap) / 3;
    float row1 = cardY + cardH + 8;
    float row2 = row1 + 34 + gap;
    // The ID needs more room than the version and size beside it.
    float idW = cellW * 1.4f;
    float smallW = (cardW - idW - 2 * gap) / 2;
    widget_cell(cardX, row1, idW, _("Title ID"), id);
    widget_cell(cardX + idW + gap, row1, smallW, _("Version"), version);
    widget_cell(cardX + idW + gap + smallW + gap, row1, smallW, _("Size"), size);
    widget_cell(cardX, row2, cellW, _("Product code"), info->productCode);
    widget_cell(cardX + cellW + gap, row2, cellW, _("Region"), region);
    widget_cell(cardX + 2 * (cellW + gap), row2, cellW, _("Type"), titles_kind_name(title_kind_of(info->titleId)));

    // Left: what hangs from the game; right: the space left on each medium.
    float half = cardW / 2;
    titles_draw_linked(items, info, cardX + 2, row2 + 34 + 10, cardX + half - 6);
    widget_storage(SYSTEM_MEDIATYPE_SD, cardX + half + 6, row2 + 34 + 5, half - 6);
    widget_storage(SYSTEM_MEDIATYPE_CTR_NAND, cardX + half + 6, row2 + 34 + 29, half - 6);
}

// ---- actions ----

static void titles_run(titles_data* data, void (*action)(linked_list*, list_item*)) {
    action(data->actionItems, data->actionSelected);
}

static void titles_launch(void* data) { titles_run(data, action_launch_title); }
static void titles_delete(void* data) { titles_run(data, action_delete_title); }
static void titles_delete_ticket(void* data) { titles_run(data, action_delete_title_ticket); }
static void titles_extract_smdh(void* data) { titles_run(data, action_extract_smdh); }
static void titles_export_cia(void* data) { titles_run(data, action_export_cia); }
static void titles_export_cia_send(void* data) { titles_run(data, action_export_cia_send); }
static void titles_import_seed(void* data) { titles_run(data, action_import_seed); }
static void titles_import_save(void* data) { titles_run(data, action_import_twl_save); }
static void titles_export_save(void* data) { titles_run(data, action_export_twl_save); }
static void titles_erase_save(void* data) { titles_run(data, action_erase_twl_save); }
static void titles_import_sv(void* data) { titles_run(data, action_import_secure_value); }
static void titles_export_sv(void* data) { titles_run(data, action_export_secure_value); }
static void titles_delete_sv(void* data) { titles_run(data, action_delete_secure_value); }

// The ID of the ext save data a game has on the SD card, if it has any: the one
// its exheader names, else its own ID or its own ID without the last byte
// (savetransfer.c, save_data_candidates).
static bool titles_ext_find(title_info* info, u32* extId) {
    return action_find_ext_data(info, extId);
}

// A list item for the game's ext save data, for the actions of that screen.
static list_item* titles_ext_item(titles_data* data) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    ext_save_data_info* info = (ext_save_data_info*) calloc(1, sizeof(ext_save_data_info));
    if(item == NULL || info == NULL) {
        free(item);
        free(info);
        return NULL;
    }

    u32 extId = 0;
    titles_ext_find((title_info*) data->actionSelected->data, &extId);
    info->mediaType = MEDIATYPE_SD;
    info->extSaveDataId = extId;
    info->shared = false;
    snprintf(item->name, LIST_ITEM_NAME_MAX, "%s", data->actionSelected->name);
    item->data = info;

    // Its own name and icon, so the dialogs show what is really being touched.
    task_ext_save_data_load(item);
    return item;
}

static void titles_browse_ext(void* data) {
    list_item* item = titles_ext_item((titles_data*) data);
    if(item != NULL) {
        action_browse_user_ext_save_data(((titles_data*) data)->actionItems, item);
        task_free_ext_save_data(item);
    }
}

static void titles_delete_ext(void* data) {
    list_item* item = titles_ext_item((titles_data*) data);
    if(item != NULL) {
        // No list to take it from: the action frees the copy.
        action_delete_ext_save_data(NULL, item);
    }
}

// ---- one kind of a title's data and its backups ----
//
// "Save data", "Extra data" and "SpotPass data" in the sheet open this list: what can be done with the data that is on
// the console (back it up now, send it, browse it), then every backup of it (NSA's exports and Checkpoint's), newest
// first; a backup's own sheet restores or deletes it.

typedef enum {
    DATA_ROW_BACKUP_NOW,
    DATA_ROW_SEND,
    DATA_ROW_BROWSE,
    DATA_ROW_DELETE,
    DATA_ROW_COUNT
} titles_data_row;

static u8 titles_data_rows[DATA_ROW_COUNT];   // Their addresses tell the action rows from the backups.

typedef struct {
    titles_data* owner;
    title_info title;
    char name[LIST_ITEM_NAME_MAX];
    save_kind kind;
    u32 extId;

    save_backup* backups;
    u32 count;
    bool filled;
    u32 generation;
    save_backup chosen;
} titles_data_manager;

static int titles_data_row_of(list_item* item) {
    u8* row = (u8*) item->data;
    return row >= titles_data_rows && row < titles_data_rows + DATA_ROW_COUNT ? (int) (row - titles_data_rows) : -1;
}

static void titles_data_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    task_draw_title_info(view, &((titles_data_manager*) data)->title, x1, y1, x2, y2);
}

static void titles_data_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    static const u32 icons[DATA_ROW_COUNT] = {ICON_DEVICE_FLOPPY, ICON_3DS, ICON_FOLDER, ICON_TRASH};
    const theme_current* t = theme();
    int row = titles_data_row_of(item);
    if(row >= 0) {
        widget_row(x, y, width, height, (int) icons[row], item->name, row == DATA_ROW_DELETE ? t->palette->danger : t->palette->text, NULL, selected);
    } else {
        save_backup* b = (save_backup*) item->data;
        widget_row(x, y, width, height, b->pksm ? WIDGET_ICON_PKSM : b->jksm ? WIDGET_ICON_JKSM : b->checkpoint ? ICON_CHECKPOINT : WIDGET_ICON_NSA, item->name, t->palette->text, NULL, selected);
    }
}

static void titles_data_clear(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static void titles_data_add(linked_list* items, const char* name, void* data) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    if(item != NULL) {
        string_copy(item->name, name, sizeof(item->name));
        item->color = COLOR_TEXT;
        item->data = data;
        linked_list_add(items, item);
    }
}

static void titles_data_fill(titles_data_manager* mgr, linked_list* items) {
    titles_data_clear(items);
    free(mgr->backups);
    mgr->backups = NULL;
    mgr->generation = action_save_generation();
    mgr->count = action_save_backups(&mgr->title, mgr->kind, &mgr->backups);
    mgr->filled = true;

    titles_data_add(items, _("Back up now"), &titles_data_rows[DATA_ROW_BACKUP_NOW]);
    titles_data_add(items, _("Send the current data to another 3DS"), &titles_data_rows[DATA_ROW_SEND]);
    if(mgr->kind == SAVE_KIND_SAVE) {
        titles_data_add(items, _("Browse save data"), &titles_data_rows[DATA_ROW_BROWSE]);
    } else if(mgr->kind == SAVE_KIND_EXT) {
        titles_data_add(items, _("Browse extra data"), &titles_data_rows[DATA_ROW_BROWSE]);
        titles_data_add(items, _("Delete extra data"), &titles_data_rows[DATA_ROW_DELETE]);
    }

    for(u32 i = 0; i < mgr->count; i++) {
        titles_data_add(items, mgr->backups[i].label + mgr->backups[i].nameAt, &mgr->backups[i]);
    }
}

static void titles_data_restore(void* data) {
    titles_data_manager* mgr = (titles_data_manager*) data;
    action_save_import_backup(&mgr->title, mgr->name, mgr->kind, mgr->extId, mgr->chosen.path);
}

static void titles_data_send(void* data) {
    titles_data_manager* mgr = (titles_data_manager*) data;
    action_send_save_backup(&mgr->title, mgr->name, mgr->kind, mgr->chosen.path);
}

static void titles_data_prompt_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    task_draw_title_info(view, &((titles_data_manager*) data)->title, x1, y1, x2, y2);
}

static void titles_data_delete_answer(ui_view* view, void* data, u32 response) {
    titles_data_manager* mgr = (titles_data_manager*) data;
    if(response != PROMPT_YES) {
        return;
    }

    sheet_confirmed();
    Result res = action_save_delete_backup(&mgr->title, mgr->kind, mgr->chosen.path);
    if(R_FAILED(res)) {
        error_display_res(&mgr->title, task_draw_title_info, res, "Failed to delete the backup.");
    }
}

static void titles_data_delete(void* data) {
    titles_data_manager* mgr = (titles_data_manager*) data;
    char text[FILE_PATH_MAX + 160];
    snprintf(text, sizeof(text), "%s\n%s%s", _("Delete this backup? It cannot be undone."), mgr->chosen.label,
             mgr->chosen.pksm ? _("\n(PKSM's: it will not see it any more.)") : mgr->chosen.jksm ? _("\n(JKSM's: it will not see it any more.)") : mgr->chosen.checkpoint ? _("\n(Checkpoint's: it will not see it any more.)") : "");
    prompt_display_warning("Confirmation", text, COLOR_TEXT, mgr, titles_data_prompt_top, titles_data_delete_answer);
}

static void titles_data_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    titles_data_manager* mgr = (titles_data_manager*) data;

    if(!mgr->filled || mgr->generation != action_save_generation()) {
        titles_data_fill(mgr, items);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        titles_data_clear(items);
        list_destroy(view);
        free(mgr->backups);
        free(mgr);
        return;
    }

    if(selected == NULL || !(selectedTouched || (hidKeysDown() & KEY_A))) {
        return;
    }

    switch(titles_data_row_of(selected)) {
        case DATA_ROW_BACKUP_NOW:
            action_export_save_files(&mgr->title, mgr->name, mgr->kind, mgr->extId);
            break;
        case DATA_ROW_SEND:
            action_send_save_files(&mgr->title, mgr->name, mgr->kind, mgr->extId);
            break;
        case DATA_ROW_BROWSE:
            if(mgr->kind == SAVE_KIND_EXT) {
                titles_browse_ext(mgr->owner);
            } else {
                titles_run(mgr->owner, action_browse_title_save_data);
            }

            break;
        case DATA_ROW_DELETE:
            titles_delete_ext(mgr->owner);
            break;
        default: {
            mgr->chosen = *(save_backup*) selected->data;
            sheet_action actions[3] = {
                    {ICON_DOWNLOAD, _("Restore this backup"), true, titles_data_restore},
                    {ICON_3DS, _("Send to another 3DS"), false, titles_data_send},
                    {ICON_TRASH, _("Delete this backup"), true, titles_data_delete}
            };
            sheet_display_list(mgr->chosen.label, 0, actions, 3, mgr);
            break;
        }
    }
}

static void titles_data_open(titles_data* data, save_kind kind) {
    title_info* info = (title_info*) data->actionSelected->data;
    u32 extId = 0;
    if((kind == SAVE_KIND_EXT && !titles_ext_find(info, &extId)) || (kind == SAVE_KIND_BOSS && !action_find_boss_data(info, &extId))) {
        return;
    }

    titles_data_manager* mgr = (titles_data_manager*) calloc(1, sizeof(titles_data_manager));
    if(mgr == NULL) {
        error_display(NULL, NULL, "Failed to allocate the list.");
        return;
    }

    mgr->owner = data;
    mgr->title = *info;
    string_copy(mgr->name, data->actionSelected->name, sizeof(mgr->name));
    mgr->kind = kind;
    mgr->extId = extId;

    ui_view* view = list_display(kind == SAVE_KIND_EXT ? "Extra data" : kind == SAVE_KIND_BOSS ? "SpotPass data" : "Save data", " open     back", mgr, titles_data_update, titles_data_draw_top);
    if(view == NULL) {
        free(mgr);
        return;
    }

    list_style style = {
            .rowHeight = 30,
            .drawRow = titles_data_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
}

static void titles_manage_save(void* data) { titles_data_open(data, SAVE_KIND_SAVE); }
static void titles_manage_ext(void* data) { titles_data_open(data, SAVE_KIND_EXT); }
static void titles_manage_boss(void* data) { titles_data_open(data, SAVE_KIND_BOSS); }

// ---- the game's mods (/luma/titles, LayeredFS) ----

static void titles_mods_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    task_draw_title_info(view, ((titles_data*) data)->actionSelected->data, x1, y1, x2, y2);
}

static void titles_delete_mods_answer(ui_view* view, void* data, u32 response) {
    if(response != PROMPT_YES) {
        return;
    }

    titles_data* listData = (titles_data*) data;
    Result res = task_mods_delete(((title_info*) listData->actionSelected->data)->titleId);
    if(R_FAILED(res)) {
        error_display_res(listData, titles_mods_draw_top, res, "Failed to delete the mods.");
    } else {
        prompt_display_notify("Success", "Mods deleted.", COLOR_TEXT, NULL, NULL, NULL);
    }
}

static void titles_delete_mods(void* data) {
    titles_data* listData = (titles_data*) data;
    title_info* info = (title_info*) listData->actionSelected->data;

    char message[256];
    snprintf(message, sizeof(message), "%s\n" MODS_ROOT "%016llX/", _("Delete these mods? The game stays, without them."), info->titleId);
    prompt_display_warning("Confirmation", message, COLOR_TEXT, data, titles_mods_draw_top, titles_delete_mods_answer);
}

static void titles_actions_open(titles_data* data, linked_list* items, list_item* selected) {
    title_info* info = (title_info*) selected->data;
    data->actionItems = items;
    data->actionSelected = selected;

    sheet_action actions[SHEET_ACTIONS_MAX];
    u32 count = 0;
    bool card = info->mediaType == MEDIATYPE_GAME_CARD;
    u32 extId = 0;
    bool hasExt = titles_ext_find(info, &extId);
    u32 bossId = 0;
    bool hasBoss = action_find_boss_data(info, &bossId);

    actions[count++] = (sheet_action) {ICON_PLAYER_PLAY, _("Launch"), false, titles_launch};
    if(!info->twl) {
        // Each opens its list (on top of the sheet: B comes back to it).
        actions[count++] = (sheet_action) {ICON_DEVICE_FLOPPY, _("Save data"), false, titles_manage_save, true};
        if(hasExt) {
            actions[count++] = (sheet_action) {ICON_DATABASE, _("Extra data"), false, titles_manage_ext, true};
        }

        if(hasBoss) {
            actions[count++] = (sheet_action) {ICON_WORLD_DOWNLOAD, _("SpotPass data"), false, titles_manage_boss, true};
        }

        actions[count++] = (sheet_action) {ICON_PHOTO, _("Extract icon"), false, titles_extract_smdh};

        if(!card) {
            actions[count++] = (sheet_action) {ICON_PACKAGE, _("Export as CIA"), false, titles_export_cia};
            actions[count++] = (sheet_action) {ICON_3DS, _("Send to another 3DS"), false, titles_export_cia_send};
            actions[count++] = (sheet_action) {ICON_KEY, _("Import seed"), false, titles_import_seed};
            actions[count++] = (sheet_action) {ICON_SHIELD_LOCK, _("Import secure value"), false, titles_import_sv};
            actions[count++] = (sheet_action) {ICON_SHIELD_LOCK, _("Export secure value"), false, titles_export_sv};
            actions[count++] = (sheet_action) {ICON_SHIELD_LOCK, _("Delete secure value"), true, titles_delete_sv};
        }
    } else if(card) {
        actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Import save"), false, titles_import_save};
        actions[count++] = (sheet_action) {ICON_DEVICE_FLOPPY, _("Export save"), false, titles_export_save};
        actions[count++] = (sheet_action) {ICON_TRASH, _("Erase save"), true, titles_erase_save};
    }

    // Its mods in /luma/titles, if it has any (a game card's too: LayeredFS works for it).
    if(!info->twl && task_mods_exist(info->titleId)) {
        actions[count++] = (sheet_action) {ICON_TRASH, _("Delete mods"), true, titles_delete_mods};
    }

    if(!card) {
        actions[count++] = (sheet_action) {ICON_TRASH, _("Delete"), true, titles_delete};
        actions[count++] = (sheet_action) {ICON_TRASH_X, _("Delete with ticket"), true, titles_delete_ticket};
    }

    // No header: the top screen already shows the title.
    sheet_display_list(NULL, 0, actions, count, data);
}

// ---- tools: view, filter and sort, search ----

static void titles_store(titles_data* data);

// The grid / rows mode is kept in the settings.
static void titles_view_changed(void* data) {
    titles_store((titles_data*) data);
}

static bool titles_filter_active(ui_view* view, void* data) {
    titles_data* listData = (titles_data*) data;
    bool all = listData->showGameCard && listData->showSD && listData->showNAND && listData->group;
    for(u32 i = 0; i < 6; i++) {
        all = all && listData->showKind[i];
    }

    for(u32 i = 0; i < 5; i++) {
        all = all && listData->showRegion[i];
    }

    return !all;
}

// Popover groups: storage, kind, region, grouping, sort.
static bool titles_popover_is_on(void* data, u32 group, u32 option) {
    titles_data* listData = (titles_data*) data;
    switch(group) {
        case 0: return option == 0 ? listData->showSD : option == 1 ? listData->showNAND : listData->showGameCard;
        case 1: return listData->showKind[option];
        case 2: return listData->showRegion[option];
        case 3: return listData->group == (option == 0);
        default: return listData->sort == (titles_sort) option;
    }
}

// What the screen shows is kept in the settings, for the next time.
static void titles_store(titles_data* data) {
    titles_settings* saved = &settings_get()->titles;
    saved->media = (data->showSD ? 1 : 0) | (data->showNAND ? 2 : 0) | (data->showGameCard ? 4 : 0);
    saved->kinds = 0;
    for(u32 i = 0; i < 6; i++) {
        saved->kinds |= data->showKind[i] ? 1u << i : 0;
    }

    saved->regions = 0;
    for(u32 i = 0; i < 5; i++) {
        saved->regions |= data->showRegion[i] ? 1u << i : 0;
    }

    saved->group = data->group;
    saved->sort = (u32) data->sort;
    saved->sortReverse = data->sortReverse;
    saved->grid = data->view != NULL && list_get_mode(data->view) == LIST_MODE_GRID;
    settings_save();
}

static void titles_restore(titles_data* data) {
    titles_settings* saved = &settings_get()->titles;
    data->showSD = (saved->media & 1) != 0;
    data->showNAND = (saved->media & 2) != 0;
    data->showGameCard = (saved->media & 4) != 0;
    for(u32 i = 0; i < 6; i++) {
        data->showKind[i] = (saved->kinds >> i) & 1;
    }

    for(u32 i = 0; i < 5; i++) {
        data->showRegion[i] = (saved->regions >> i) & 1;
    }

    data->group = saved->group;
    data->sort = saved->sort <= SORT_SIZE ? (titles_sort) saved->sort : SORT_NAME;
    data->sortReverse = saved->sortReverse;
}

static void titles_popover_select(void* data, u32 group, u32 option) {
    titles_data* listData = (titles_data*) data;
    bool relist = false;
    switch(group) {
        case 0: {
            bool* flag = option == 0 ? &listData->showSD : option == 1 ? &listData->showNAND : &listData->showGameCard;
            *flag = !*flag;
            relist = true;
            break;
        }
        // Kind, region and grouping only hide items: no need to list the titles again.
        case 1:
            listData->showKind[option] = !listData->showKind[option];
            break;
        case 2:
            listData->showRegion[option] = !listData->showRegion[option];
            break;
        case 3:
            listData->group = option == 0;
            break;
        // Sorting needs no new listing either: the titles already loaded are sorted again in place.
        default:
            listData->sortReverse = listData->sort == (titles_sort) option ? !listData->sortReverse : false;
            listData->sort = (titles_sort) option;
            listData->sorted = false;
            listData->sortedLoaded = (u32) -1;
            break;
    }

    if(relist) {
        listData->populated = false;
    }

    titles_store(listData);
}

static bool titles_popover_is_reversed(void* data, u32 group, u32 option) {
    titles_data* listData = (titles_data*) data;
    return group == 4 && listData->sort == (titles_sort) option && listData->sortReverse;
}

static void titles_filter_open(ui_view* view, void* data) {
    popover_def def = {
            .groupCount = 5,
            .groups = {
                    {_("Show"), true, 3, {"SD", "NAND", _("Game card")}, {ICON_DEVICE_SD_CARD, ICON_CPU, ICON_DEVICE_GAMEPAD_2}},
                    // In title_kind's order.
                    {_("Type"), true, 6, {_("Games"), _("Updates"), "DLC", _("Demos"), "DSiWare", _("System")}, {ICON_DEVICE_GAMEPAD_2, ICON_REFRESH, ICON_PUZZLE, ICON_FLASK, ICON_DEVICE_MOBILE, ICON_SETTINGS}},
                    {_("Region"), true, 5, {_("Region free"), _("Europe"), _("America"), _("Japan"), _("Other")}, {ICON_WORLD, ICON_FLAG_EU, ICON_FLAG_US, ICON_FLAG_JP, ICON_DOTS}},
                    {_("Group updates and DLC"), false, 2, {_("Yes"), _("No")}, {ICON_CHECK, ICON_X}},
                    {_("Sort by"), false, 3, {_("Name"), "ID", _("Size")}, {ICON_ABC, ICON_KEY, ICON_DATABASE}, true, true}
            },
            .isOn = titles_popover_is_on,
            .select = titles_popover_select,
            .isReversed = titles_popover_is_reversed
    };

    popover_display(&def, data);
}

// ---- a game's chain: the game, its update and its DLC (chain.c) ----

static u64 titles_chain_id(list_item* item) {
    return ((title_info*) item->data)->titleId;
}

static void titles_chain_draw_row(list_item* item, float x, float y, float width, float height, bool selected) {
    titles_draw_row(NULL, item, x, y, width, height, selected);
}

static void titles_chain_draw_top(ui_view* view, void* parent, linked_list* parentItems, list_item* item, float x1, float y1, float x2, float y2) {
    coach_anchor("titles.chain", x1, y1, x2 - x1, y2 - y1, false);   // A game's updates and DLC are open (guide.c).
    titles_draw_details(parentItems, item, x1, y1, x2, y2);
}

static void titles_chain_actions(void* parent, linked_list* parentItems, list_item* item) {
    titles_actions_open((titles_data*) parent, parentItems, item);
}

static const chain_def titles_chain = {
        .rowHeight = ROW_HEIGHT,
        .titleId = titles_chain_id,
        .drawRow = titles_chain_draw_row,
        .drawTop = titles_chain_draw_top,
        .actions = titles_chain_actions,
        .allocError = "Failed to allocate title chain."
};

static void titles_chain_open(titles_data* data, linked_list* items, list_item* game) {
    chain_open(&titles_chain, data, items, game);
}

// ---- the list ----

static int titles_compare(void* data, const void* p1, const void* p2);

// Loaded, for the count of the title bar.
static bool titles_item_ready(list_item* item) {
    return ((title_info*) item->data)->loaded;
}

static void titles_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    titles_data* listData = (titles_data*) data;

    view->name = _("Titles");
    listData->items = items;
    titles_group(listData, items);

    // How many have loaded, for the title bar.
    u32 loaded = list_count_progress(view, items, !listData->populateData.finished, titles_item_ready);
    view->info = _(" actions    view    filter    search    back");

    if(hidKeysDown() & KEY_B) {
        // B first clears a search.
        if(list_clear_search(view)) {
            return;
        }

        if(!listData->populateData.finished) {
            task_stop_populate(listData->populateData.cancelEvent, &listData->populateData.finished);
        }

        ui_pop();

        task_clear_titles(items);
        list_destroy(view);

        free(listData);
        return;
    }

    if(hidKeysDown() & KEY_SELECT) {
        list_next_tile_size(view);
    }

    if(hidKeysDown() & KEY_Y) {
        list_toggle_mode(view);
    }

    if(hidKeysDown() & KEY_L) {
        titles_filter_open(view, listData);
        return;
    }

    if(hidKeysDown() & KEY_R) {
        list_open_search(view);
        return;
    }

    if(!listData->populated || (hidKeysDown() & KEY_X)) {
        if(!listData->populateData.finished) {
            task_stop_populate(listData->populateData.cancelEvent, &listData->populateData.finished);
        }

        listData->populateData.items = items;
        listData->populateData.refresh = listData->populated;
        listData->sorted = false;
        listData->sortedLoaded = 0;
        Result res = task_populate_titles(&listData->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate title list population.");
        }

        listData->populated = true;
    }

    // Sorted as titles load: loaded ones in order at the top, the rest
    // after; the selected title keeps its place on screen meanwhile.
    if(!listData->populateData.finished && loaded != listData->sortedLoaded) {
        linked_list_sort(items, listData, titles_compare);
        listData->sortedLoaded = loaded;
    } else if(listData->populateData.finished && !listData->sorted) {
        linked_list_sort(items, listData, titles_compare);
        listData->sorted = true;
    }

    if(listData->populateData.finished && R_FAILED(listData->populateData.result)) {
        error_display_res(NULL, NULL, listData->populateData.result, "Failed to populate title list.");

        listData->populateData.result = 0;
    }

    // A title still loading has nothing to act on yet.
    if(selected != NULL && selected->data != NULL && selectedTouched && ((title_info*) selected->data)->loaded) {
        // Grouped, a game with an update or DLC opens its chain to pick one.
        if(listData->group && ((title_info*) selected->data)->linkedCount > 0) {
            titles_chain_open(listData, items, selected);
        } else {
            titles_actions_open(listData, items, selected);
        }

        return;
    }
}

// A long hold: the title's own actions, even for a game with its chain.
static void titles_long_press(ui_view* view, void* data, linked_list* items, list_item* item) {
    if(item->data != NULL && ((title_info*) item->data)->loaded) {
        titles_actions_open((titles_data*) data, items, item);
    }
}

static bool titles_filter(void* data, u64 titleId, FS_MediaType mediaType) {
    titles_data* listData = (titles_data*) data;

    if(mediaType == MEDIATYPE_GAME_CARD) {
        return listData->showGameCard;
    } else if(mediaType == MEDIATYPE_SD) {
        return listData->showSD;
    } else {
        return listData->showNAND;
    }
}

static int titles_compare(void* data, const void* p1, const void* p2) {
    titles_data* listData = (titles_data*) data;
    int dir = listData->sortReverse ? -1 : 1;

    list_item* info1 = (list_item*) p1;
    list_item* info2 = (list_item*) p2;

    title_info* title1 = (title_info*) info1->data;
    title_info* title2 = (title_info*) info2->data;

    // While loading: loaded titles first, the rest by ID until they load.
    if(title1->loaded != title2->loaded) {
        return title1->loaded ? -1 : 1;
    } else if(!title1->loaded) {
        return title1->titleId > title2->titleId ? 1 : title1->titleId < title2->titleId ? -1 : 0;
    }

    if(title1->mediaType > title2->mediaType) {
        return -1;
    } else if(title1->mediaType < title2->mediaType) {
        return 1;
    } else if(!title1->twl && title2->twl) {
        return -1;
    } else if(title1->twl && !title2->twl) {
        return 1;
    } else if(listData->sort == SORT_ID) {
        u64 id1 = title1->titleId;
        u64 id2 = title2->titleId;

        return dir * (id1 > id2 ? 1 : id1 < id2 ? -1 : 0);
    } else if(listData->sort == SORT_SIZE) {
        u64 size1 = title1->installedSize;
        u64 size2 = title2->installedSize;

        return dir * (size1 > size2 ? -1 : size1 < size2 ? 1 : 0);
    } else {
        bool title1HasName = title1->hasMeta && !string_is_empty(title1->meta.shortDescription);
        bool title2HasName = title2->hasMeta && !string_is_empty(title2->meta.shortDescription);

        if(title1HasName && !title2HasName) {
            return -1;
        } else if(!title1HasName && title2HasName) {
            return 1;
        } else {
            return dir * strncasecmp(info1->name, info2->name, sizeof(info1->name));
        }
    }
}

void titles_open() {
    titles_data* data = (titles_data*) calloc(1, sizeof(titles_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate titles data.");

        return;
    }

    data->populateData.userData = data;
    data->populateData.filter = titles_filter;
    data->populateData.compare = titles_compare;

    data->populateData.finished = true;

    titles_restore(data);

    ui_view* view = list_display(_("Titles"), "", data, titles_update, titles_draw_top);
    if(view == NULL) {
        free(data);
        return;
    }

    data->view = view;
    if(settings_get()->titles.grid) {
        list_set_mode(view, LIST_MODE_GRID);
    }

    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = titles_draw_row,
            .tileSize = list_tile_size_of(settings_get()->titles.tile),
            .drawTile = titles_draw_tile
    };
    list_set_style(view, &style);
    list_set_filter(view, titles_visible);
    list_set_long_press(view, titles_long_press);

    // Right to left.
    list_add_search_tool(view, _("Search titles"));
    list_add_tool(view, ICON_FILTER, titles_filter_active, titles_filter_open);
    list_add_tile_tool(view, &settings_get()->titles.tile);
    list_add_view_tools(view, titles_view_changed);
}
