#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
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

static void titles_draw_count_badge(u32 count, float x, float y) {
    const theme_current* t = theme();
    char text[8];
    snprintf(text, sizeof(text), "%lu", count);
    float r = 8;
    screen_fill_circle(x, y, r + 1.5f, t->palette->surface);
    screen_fill_circle(x, y, r, t->accent);
    float w = widget_text_width(text, WIDGET_SMALL_SCALE);
    screen_draw_text(text, x - w / 2, y - widget_text_height(WIDGET_SMALL_SCALE) / 2, WIDGET_SMALL_SCALE, t->onAccent);
}

// ---- items ----

static void titles_draw_icon(title_info* info, float x, float y, float size) {
    const theme_current* t = theme();
    if(info->hasMeta && info->meta.texture != 0) {
        screen_draw_texture(info->meta.texture, x, y, size, size);
    } else {
        screen_fill_round_rect(x, y, size, size, size / 6, t->accentSoft);
        screen_draw_icon(info->twl ? ICON_DEVICE_FLOPPY : ICON_APPS, x + size / 4, y + size / 4, size / 2, t->accent);
    }
}

// While a title loads: grey blocks where its icon and text will be.
static void titles_draw_skeleton(float x, float y, float w, float h, float icon, bool selected) {
    const theme_current* t = theme();
    u32 block = selected ? THEME_ALPHA(t->onAccent, 0x40) : t->palette->border;
    screen_fill_round_rect(x, y, icon, icon, icon / 6, block);
    if(w > icon + 30) {
        float tx = x + icon + 8;
        screen_fill_round_rect(tx, y + 4, (w - icon - 8) * 0.55f, 8, 4, block);
        screen_fill_round_rect(tx, y + h - 12, (w - icon - 8) * 0.35f, 6, 3, block);
    }
}

// A title's contents fade in once it has loaded.
static void titles_fade_in(title_info* info) {
    float e = widget_ease(info->loadedTime, 250);
    screen_set_content_motion(0, 0, (u8) (e * 0xFF));
}

static void titles_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    title_info* info = (title_info*) item->data;

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    if(!info->loaded) {
        titles_draw_skeleton(x + 5, y + 5, width - 18, height - 10, height - 10, selected);
        return;
    }

    titles_fade_in(info);

    float icon = height - 10;
    titles_draw_icon(info, x + 5, y + 5, icon);

    titles_data* listData = (titles_data*) data;
    if(listData != NULL && listData->group && info->linkedCount > 0) {
        titles_draw_count_badge(info->linkedCount, x + 5 + icon - 1, y + 7);
    }

    float tx = x + 5 + icon + 8;
    float right = x + width - 8;

    // Anything but a game says what it is.
    title_kind kind = title_kind_of(info->titleId);
    float nameRight = right;
    if(kind != TITLE_KIND_GAME) {
        const char* tag = titles_kind_name(kind);
        float tagW = widget_text_width(tag, WIDGET_SMALL_SCALE) + 12;
        widget_badge(right - tagW, y + 4, tag, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft, selected ? t->onAccent : t->accent);
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
    const theme_current* t = theme();
    title_info* info = (title_info*) item->data;

    if(selected) {
        screen_fill_round_rect(x - 3, y - 3, size + 6, size + 6, t->def->radius + 2, t->accent);
        screen_fill_round_rect(x, y, size, size, t->def->radius, t->accentSoft);
    } else {
        screen_frame_round_rect(x, y, size, size, t->def->radius, t->def->border, t->palette->border, t->palette->surface);
    }

    float icon = size - 8;
    if(!info->loaded) {
        titles_draw_skeleton(x + 4, y + 4, icon, icon, icon, selected);
        return;
    }

    titles_fade_in(info);
    titles_draw_icon(info, x + 4, y + 4, icon);
    screen_set_content_motion(0, 0, 0xFF);

    titles_data* listData = (titles_data*) data;
    if(listData != NULL && listData->group && info->linkedCount > 0) {
        titles_draw_count_badge(info->linkedCount, x + size - 4, y + 4);
    }
}

// ---- top screen: the selected title ----

static void titles_draw_cell(float x, float y, float w, const char* label, const char* value) {
    const theme_current* t = theme();
    float h = 34;
    screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, t->palette->border, t->palette->surface);
    screen_draw_text(label, x + 8, y + 3, WIDGET_SMALL_SCALE, t->palette->textMuted);
    float scale = widget_text_width(value, WIDGET_TEXT_SCALE) <= w - 14 ? WIDGET_TEXT_SCALE : WIDGET_SMALL_SCALE;
    widget_text_fit(value, x + 8, y + 3 + widget_text_height(WIDGET_SMALL_SCALE), scale, t->palette->text, w - 14);
}

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
        titles_draw_skeleton(x1 + 20, y1 + 20, x2 - x1 - 40, 56, 56, false);
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
    titles_draw_cell(cardX, row1, idW, _("Title ID"), id);
    titles_draw_cell(cardX + idW + gap, row1, smallW, _("Version"), version);
    titles_draw_cell(cardX + idW + gap + smallW + gap, row1, smallW, _("Size"), size);
    titles_draw_cell(cardX, row2, cellW, _("Product code"), info->productCode);
    titles_draw_cell(cardX + cellW + gap, row2, cellW, _("Region"), region);
    titles_draw_cell(cardX + 2 * (cellW + gap), row2, cellW, _("Type"), titles_kind_name(title_kind_of(info->titleId)));

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
static void titles_browse_save(void* data) { titles_run(data, action_browse_title_save_data); }
static void titles_import_save(void* data) { titles_run(data, action_import_twl_save); }
static void titles_export_save(void* data) { titles_run(data, action_export_twl_save); }
static void titles_erase_save(void* data) { titles_run(data, action_erase_twl_save); }
static void titles_import_sv(void* data) { titles_run(data, action_import_secure_value); }
static void titles_export_sv(void* data) { titles_run(data, action_export_secure_value); }
static void titles_delete_sv(void* data) { titles_run(data, action_delete_secure_value); }

// The ID of the ext save data a game has on the SD card, if it has any: under
// its own ID, or under it without the last byte (Mario Kart 7, 00030700, keeps
// its data as 307).
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

// `mode`: 0 export, 1 import, 2 send to another 3DS.
static void titles_save_files(titles_data* data, int mode, save_kind kind) {
    title_info* info = (title_info*) data->actionSelected->data;
    u32 extId = 0;
    if(kind == SAVE_KIND_EXT && !titles_ext_find(info, &extId)) {
        return;
    }

    if(mode == 1) {
        action_import_save_files(info, data->actionSelected->name, kind, extId);
    } else if(mode == 2) {
        action_send_save_files(info, data->actionSelected->name, kind, extId);
    } else {
        action_export_save_files(info, data->actionSelected->name, kind, extId);
    }
}

static void titles_export_save_files(void* data) { titles_save_files(data, 0, SAVE_KIND_SAVE); }
static void titles_import_save_files(void* data) { titles_save_files(data, 1, SAVE_KIND_SAVE); }
static void titles_export_ext_files(void* data) { titles_save_files(data, 0, SAVE_KIND_EXT); }
static void titles_import_ext_files(void* data) { titles_save_files(data, 1, SAVE_KIND_EXT); }
static void titles_send_save_files(void* data) { titles_save_files(data, 2, SAVE_KIND_SAVE); }
static void titles_send_ext_files(void* data) { titles_save_files(data, 2, SAVE_KIND_EXT); }

static void titles_actions_open(titles_data* data, linked_list* items, list_item* selected) {
    title_info* info = (title_info*) selected->data;
    data->actionItems = items;
    data->actionSelected = selected;

    sheet_action actions[SHEET_ACTIONS_MAX];
    u32 count = 0;
    bool card = info->mediaType == MEDIATYPE_GAME_CARD;
    u32 extId = 0;
    bool hasExt = titles_ext_find(info, &extId);

    actions[count++] = (sheet_action) {ICON_PLAYER_PLAY, _("Launch"), false, titles_launch};
    if(!info->twl) {
        actions[count++] = (sheet_action) {ICON_DEVICE_FLOPPY, _("Save data"), false, titles_browse_save};
        actions[count++] = (sheet_action) {ICON_DEVICE_FLOPPY, _("Export save"), false, titles_export_save_files};
        actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Import save"), true, titles_import_save_files};
        actions[count++] = (sheet_action) {ICON_3DS, _("Send save to another 3DS"), false, titles_send_save_files};
        actions[count++] = (sheet_action) {ICON_PHOTO, _("Extract icon"), false, titles_extract_smdh};
        if(hasExt) {
            // Browsing leaves the sheet open under it: B comes back to the sheet.
            actions[count++] = (sheet_action) {ICON_DATABASE, _("Browse extra data"), false, titles_browse_ext, true};
            actions[count++] = (sheet_action) {ICON_DATABASE, _("Export extra data"), false, titles_export_ext_files};
            actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Import extra data"), true, titles_import_ext_files};
            actions[count++] = (sheet_action) {ICON_3DS, _("Send extra data to another 3DS"), false, titles_send_ext_files};
        }

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

    if(!card) {
        if(hasExt) {
            actions[count++] = (sheet_action) {ICON_TRASH, _("Delete extra data"), true, titles_delete_ext, false};
        }

        actions[count++] = (sheet_action) {ICON_TRASH, _("Delete"), true, titles_delete};
        actions[count++] = (sheet_action) {ICON_TRASH_X, _("Delete with ticket"), true, titles_delete_ticket};
    }

    // No header: the top screen already shows the title.
    sheet_display_list(NULL, 0, actions, count, data);
}

// ---- tools: view, filter and sort, search ----

static bool titles_is_rows(ui_view* view, void* data) {
    return list_get_mode(view) == LIST_MODE_ROWS;
}

static bool titles_is_grid(ui_view* view, void* data) {
    return list_get_mode(view) == LIST_MODE_GRID;
}

static void titles_store(titles_data* data);

static void titles_show_rows(ui_view* view, void* data) {
    list_set_mode(view, LIST_MODE_ROWS);
    titles_store((titles_data*) data);
}

static void titles_show_grid(ui_view* view, void* data) {
    list_set_mode(view, LIST_MODE_GRID);
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
        default:
            listData->sort = (titles_sort) option;
            relist = true;
            break;
    }

    if(relist) {
        listData->populated = false;
    }

    titles_store(listData);
}

static void titles_filter_open(ui_view* view, void* data) {
    popover_def def = {
            .groupCount = 5,
            .groups = {
                    {_("Show"), true, 3, {"SD", "NAND", _("Game card")}, {ICON_DEVICE_SD_CARD, ICON_CPU, ICON_DEVICE_GAMEPAD_2}},
                    // In title_kind's order.
                    {_("Type"), true, 6, {_("Games"), _("Updates"), "DLC", _("Demos"), "DSiWare", _("System")}, {-1, -1, -1, -1, -1, -1}},
                    {_("Region"), true, 5, {_("Region free"), _("Europe"), _("America"), _("Japan"), _("Other")}, {-1, -1, -1, -1, -1}},
                    {_("Group updates and DLC"), false, 2, {_("Yes"), _("No")}, {-1, -1}},
                    {_("Sort by"), false, 3, {_("Name"), "ID", _("Size")}, {-1, -1, -1}}
            },
            .isOn = titles_popover_is_on,
            .select = titles_popover_select
    };

    popover_display(&def, data);
}

static bool titles_search_active(ui_view* view, void* data) {
    return list_get_search(view)[0] != '\0';
}

static void titles_search_done(ui_view* kbdView, void* data, SwkbdButton button, const char* response) {
    titles_data* listData = (titles_data*) data;
    if(button == SWKBD_BUTTON_CONFIRM) {
        list_set_search(listData->view, response);
    }
}

static void titles_search_open(ui_view* view, void* data) {
    kbd_display(_("Search titles"), list_get_search(view), SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, 63, data, titles_search_done);
}

// ---- a game's chain: the game, its update and its DLC ----

typedef struct {
    titles_data* parent;
    linked_list* parentItems;
    list_item* game;
    char name[LIST_ITEM_NAME_MAX];   // The game's item may be deleted while this is open.
} titles_chain_data;

// Chain rows wrap the main list's items (item->data), which keep owning the titles.
static list_item* titles_chain_target(list_item* wrapper) {
    return (list_item*) wrapper->data;
}

static void titles_chain_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    titles_draw_row(NULL, titles_chain_target(item), x, y, width, height, selected);
}

static void titles_chain_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    titles_chain_data* chain = (titles_chain_data*) data;
    titles_draw_details(chain->parentItems, selected != NULL ? titles_chain_target(selected) : NULL, x1, y1, x2, y2);
}

static void titles_chain_close(ui_view* view, linked_list* items, titles_chain_data* chain) {
    ui_pop();

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }

    list_destroy(view);
    free(chain);
}

static void titles_chain_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    titles_chain_data* chain = (titles_chain_data*) data;

    view->info = _("\uE000 actions   \uE001 back");

    if(hidKeysDown() & KEY_B) {
        titles_chain_close(view, items, chain);
        return;
    }

    if(linked_list_size(items) == 0) {
        // The game first, then its update, then its DLC.
        title_info* game = (title_info*) chain->game->data;
        for(int pass = 0; pass < 3; pass++) {
            linked_list_iter iter;
            linked_list_iterate(chain->parentItems, &iter);
            while(linked_list_iter_has_next(&iter)) {
                list_item* item = (list_item*) linked_list_iter_next(&iter);
                title_info* info = (title_info*) item->data;
                title_kind kind = title_kind_of(info->titleId);
                bool wanted = pass == 0 ? item == chain->game : pass == 1 ? kind == TITLE_KIND_UPDATE : kind == TITLE_KIND_DLC;
                if(!wanted || (u32) info->titleId != (u32) game->titleId) {
                    continue;
                }

                list_item* wrapper = (list_item*) calloc(1, sizeof(list_item));
                if(wrapper != NULL) {
                    string_copy(wrapper->name, item->name, sizeof(wrapper->name));
                    wrapper->color = item->color;
                    wrapper->data = item;
                    linked_list_add(items, wrapper);
                }
            }
        }
    } else {
        // An action may have deleted titles from the main list: drop their rows.
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            list_item* wrapper = (list_item*) linked_list_iter_next(&iter);
            if(linked_list_index_of(chain->parentItems, titles_chain_target(wrapper)) < 0) {
                free(wrapper);
                linked_list_iter_remove(&iter);
            }
        }

        if(linked_list_size(items) == 0) {
            titles_chain_close(view, items, chain);
            return;
        }

        selected = selected != NULL && linked_list_index_of(items, selected) >= 0 ? selected : NULL;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        titles_actions_open(chain->parent, chain->parentItems, titles_chain_target(selected));
    }
}

static void titles_chain_open(titles_data* data, linked_list* items, list_item* game) {
    titles_chain_data* chain = (titles_chain_data*) calloc(1, sizeof(titles_chain_data));
    if(chain == NULL) {
        error_display(NULL, NULL, "Failed to allocate title chain.");
        return;
    }

    chain->parent = data;
    chain->parentItems = items;
    chain->game = game;
    string_copy(chain->name, game->name, sizeof(chain->name));

    ui_view* view = list_display(chain->name, _("\uE000 actions   \uE001 back"), chain, titles_chain_update, titles_chain_draw_top);
    if(view == NULL) {
        free(chain);
        return;
    }

    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = titles_chain_draw_row
    };
    list_set_style(view, &style);
}

// ---- the list ----

static int titles_compare(void* data, const void* p1, const void* p2);

static void titles_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    titles_data* listData = (titles_data*) data;

    view->name = _("Titles");
    listData->items = items;
    titles_group(listData, items);

    // How many have loaded, for the title bar.
    u32 loaded = 0;
    u32 total = 0;
    if(!listData->populateData.finished) {
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            total++;
            loaded += ((title_info*) ((list_item*) linked_list_iter_next(&iter))->data)->loaded ? 1 : 0;
        }
    }

    list_set_progress(view, loaded, total);
    view->info = _(" actions    view    filter    search    back");

    if(hidKeysDown() & KEY_B) {
        // B first clears a search.
        if(list_get_search(view)[0] != '\0') {
            list_set_search(view, "");
            return;
        }

        if(!listData->populateData.finished) {
            svcSignalEvent(listData->populateData.cancelEvent);
            while(!listData->populateData.finished) {
                svcSleepThread(1000000);
            }
        }

        ui_pop();

        task_clear_titles(items);
        list_destroy(view);

        free(listData);
        return;
    }

    if(hidKeysDown() & KEY_Y) {
        list_set_mode(view, list_get_mode(view) == LIST_MODE_GRID ? LIST_MODE_ROWS : LIST_MODE_GRID);
        titles_store(listData);
    }

    if(hidKeysDown() & (KEY_L | KEY_SELECT)) {
        titles_filter_open(view, listData);
        return;
    }

    if(hidKeysDown() & KEY_R) {
        titles_search_open(view, listData);
        return;
    }

    if(!listData->populated || (hidKeysDown() & KEY_X)) {
        if(!listData->populateData.finished) {
            svcSignalEvent(listData->populateData.cancelEvent);
            while(!listData->populateData.finished) {
                svcSleepThread(1000000);
            }
        }

        listData->populateData.items = items;
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

        return id1 > id2 ? 1 : id1 < id2 ? -1 : 0;
    } else if(listData->sort == SORT_SIZE) {
        u64 size1 = title1->installedSize;
        u64 size2 = title2->installedSize;

        return size1 > size2 ? -1 : size1 < size2 ? 1 : 0;
    } else {
        bool title1HasName = title1->hasMeta && !string_is_empty(title1->meta.shortDescription);
        bool title2HasName = title2->hasMeta && !string_is_empty(title2->meta.shortDescription);

        if(title1HasName && !title2HasName) {
            return -1;
        } else if(!title1HasName && title2HasName) {
            return 1;
        } else {
            return strncasecmp(info1->name, info2->name, sizeof(info1->name));
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
            .tileSize = TILE_SIZE,
            .drawTile = titles_draw_tile
    };
    list_set_style(view, &style);
    list_set_filter(view, titles_visible);
    list_set_long_press(view, titles_long_press);

    // Right to left.
    list_add_tool(view, ICON_SEARCH, titles_search_active, titles_search_open);
    list_add_tool(view, ICON_FILTER, titles_filter_active, titles_filter_open);
    list_add_tool(view, ICON_LAYOUT_GRID, titles_is_grid, titles_show_grid);
    list_add_tool(view, ICON_LIST, titles_is_rows, titles_show_rows);
}
