#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "settings.h"

// Ext save data: rows with each one's icon and name, or a grid of icons. The
// title bar's tools switch the view, filter and sort, and search; A or a tap
// opens a sheet with the actions (deleting in red).

typedef enum {
    SORT_NAME,
    SORT_ID
} extsavedata_sort;

typedef struct {
    populate_ext_save_data_data populateData;
    ui_view* view;

    // Hidden in place (no new listing).
    bool showSD;
    bool showNAND;
    extsavedata_sort sort;

    bool populated;
    bool sorted;

    // Sorted as they load (and once more when all have).
    u32 sortedReady;

    // The ext save data the action sheet is for.
    linked_list* actionItems;
    list_item* actionSelected;
} extsavedata_data;

#define ROW_HEIGHT 40
#define TILE_SIZE 50

// What the screen shows is kept in the settings, for the next time.
static void extsavedata_store(extsavedata_data* data) {
    extsave_settings* saved = &settings_get()->extsave;
    saved->sd = data->showSD;
    saved->nand = data->showNAND;
    saved->byId = data->sort == SORT_ID;
    saved->grid = data->view != NULL && list_get_mode(data->view) == LIST_MODE_GRID;
    settings_save();
}

static bool extsavedata_visible(void* data, list_item* item) {
    extsavedata_data* listData = (extsavedata_data*) data;
    ext_save_data_info* info = (ext_save_data_info*) item->data;
    return info->mediaType == MEDIATYPE_SD ? listData->showSD : listData->showNAND;
}

// ---- items ----

// While one's name and icon are found: grey blocks where they will be.
static void extsavedata_draw_skeleton(float x, float y, float w, float h, float icon, bool selected) {
    const theme_current* t = theme();
    u32 block = selected ? THEME_ALPHA(t->onAccent, 0x40) : t->palette->border;
    screen_fill_round_rect(x, y, icon, icon, icon / 6, block);
    if(w > icon + 30) {
        float tx = x + icon + 8;
        screen_fill_round_rect(tx, y + 4, (w - icon - 8) * 0.55f, 8, 4, block);
        screen_fill_round_rect(tx, y + h - 12, (w - icon - 8) * 0.35f, 6, 3, block);
    }
}

// The contents fade in once the name and icon have been found.
static void extsavedata_fade_in(ext_save_data_info* info) {
    screen_set_content_motion(0, 0, (u8) (widget_ease(info->readyTime, 250) * 0xFF));
}

static void extsavedata_draw_icon(ext_save_data_info* info, float x, float y, float size, bool selected) {
    const theme_current* t = theme();
    if(info->hasMeta && info->meta.texture != 0) {
        screen_draw_texture(info->meta.texture, x, y, size, size);
    } else {
        screen_fill_round_rect(x, y, size, size, size / 6, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft);
        screen_draw_icon(ICON_DATABASE, x + size / 4, y + size / 4, size / 2, selected ? t->onAccent : t->accent);
    }
}

static void extsavedata_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    ext_save_data_info* info = (ext_save_data_info*) item->data;

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float icon = height - 10;
    if(!info->ready) {
        extsavedata_draw_skeleton(x + 5, y + 5, width - 18, height - 10, icon, selected);
        return;
    }

    extsavedata_fade_in(info);
    extsavedata_draw_icon(info, x + 5, y + 5, icon, selected);

    float tx = x + 5 + icon + 8;
    float right = x + width - 8;

    // Where it is, as a tag.
    const char* tag = info->mediaType == MEDIATYPE_SD ? "SD" : "NAND";
    float tagW = widget_text_width(tag, WIDGET_SMALL_SCALE) + 12;
    widget_badge(right - tagW, y + 4, tag, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft, selected ? t->onAccent : t->accent);

    widget_text_fit(item->name, tx, y + 4, WIDGET_TEXT_SCALE, ink, right - tagW - 6 - tx);

    // Second line: the ID, and the publisher when it has one.
    char line[64];
    snprintf(line, sizeof(line), "%016llX", info->extSaveDataId);
    float lineY = y + height - 4 - widget_text_height(WIDGET_SMALL_SCALE);
    float idW = widget_text_width(line, WIDGET_SMALL_SCALE);
    widget_text_fit(line, tx, lineY, WIDGET_SMALL_SCALE, muted, right - tx);
    if(info->hasMeta && !string_is_empty(info->meta.publisher) && tx + idW + 14 < right) {
        float publisherW = widget_text_width(info->meta.publisher, WIDGET_SMALL_SCALE);
        float room = right - (tx + idW + 14);
        if(publisherW > room) {
            publisherW = room;
        }

        widget_text_fit(info->meta.publisher, right - publisherW, lineY, WIDGET_SMALL_SCALE, muted, publisherW);
    }

    screen_set_content_motion(0, 0, 0xFF);
}

static void extsavedata_draw_tile(void* data, list_item* item, float x, float y, float size, bool selected) {
    const theme_current* t = theme();
    ext_save_data_info* info = (ext_save_data_info*) item->data;

    if(selected) {
        screen_fill_round_rect(x - 3, y - 3, size + 6, size + 6, t->def->radius + 2, t->accent);
        screen_fill_round_rect(x, y, size, size, t->def->radius, t->accentSoft);
    } else {
        screen_frame_round_rect(x, y, size, size, t->def->radius, t->def->border, t->palette->border, t->palette->surface);
    }

    if(!info->ready) {
        extsavedata_draw_skeleton(x + 4, y + 4, size - 8, size - 8, size - 8, selected);
        return;
    }

    extsavedata_fade_in(info);
    extsavedata_draw_icon(info, x + 4, y + 4, size - 8, false);
    screen_set_content_motion(0, 0, 0xFF);
}

static void extsavedata_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL || selected->data == NULL) {
        return;
    }

    // Not found yet: the card's place, with a word on why it is empty.
    if(!((ext_save_data_info*) selected->data)->ready) {
        const theme_current* t = theme();
        widget_card(x1 + 10, y1 + 10, x2 - x1 - 20, 76);
        extsavedata_draw_skeleton(x1 + 20, y1 + 20, x2 - x1 - 40, 56, 56, false);
        widget_text_centered(_("Loading..."), x1, x2, y1 + 110, WIDGET_TEXT_SCALE, t->palette->textMuted);
        return;
    }

    task_draw_ext_save_data_info(view, selected->data, x1, y1, x2, y2);
}

// ---- actions: a sheet, deleting in red ----

static void extsavedata_browse_user(void* data) {
    extsavedata_data* listData = (extsavedata_data*) data;
    action_browse_user_ext_save_data(listData->actionItems, listData->actionSelected);
}

static void extsavedata_browse_spotpass(void* data) {
    extsavedata_data* listData = (extsavedata_data*) data;
    action_browse_boss_ext_save_data(listData->actionItems, listData->actionSelected);
}

static void extsavedata_delete(void* data) {
    extsavedata_data* listData = (extsavedata_data*) data;
    action_delete_ext_save_data(listData->actionItems, listData->actionSelected);
}

static void extsavedata_actions_open(extsavedata_data* data, linked_list* items, list_item* selected) {
    data->actionItems = items;
    data->actionSelected = selected;

    // Browsing leaves the sheet open under it: B comes back to the sheet.
    sheet_action actions[] = {
            {ICON_FOLDER, _("Browse user data"), false, extsavedata_browse_user, true},
            {ICON_WORLD_DOWNLOAD, _("Browse SpotPass data"), false, extsavedata_browse_spotpass, true},
            {ICON_TRASH, _("Delete data"), true, extsavedata_delete, false}
    };

    // No header: the top screen already shows it.
    sheet_display_list(NULL, 0, actions, sizeof(actions) / sizeof(*actions), data);
}

// ---- tools: view, filter and sort, search ----

static bool extsavedata_is_rows(ui_view* view, void* data) {
    return list_get_mode(view) == LIST_MODE_ROWS;
}

static bool extsavedata_is_grid(ui_view* view, void* data) {
    return list_get_mode(view) == LIST_MODE_GRID;
}

static void extsavedata_show_rows(ui_view* view, void* data) {
    list_set_mode(view, LIST_MODE_ROWS);
    extsavedata_store((extsavedata_data*) data);
}

static void extsavedata_show_grid(ui_view* view, void* data) {
    list_set_mode(view, LIST_MODE_GRID);
    extsavedata_store((extsavedata_data*) data);
}

static bool extsavedata_filter_active(ui_view* view, void* data) {
    extsavedata_data* listData = (extsavedata_data*) data;
    return !listData->showSD || !listData->showNAND;
}

// Popover groups: memory, sort.
static bool extsavedata_popover_is_on(void* data, u32 group, u32 option) {
    extsavedata_data* listData = (extsavedata_data*) data;
    if(group == 0) {
        return option == 0 ? listData->showSD : listData->showNAND;
    }

    return listData->sort == (extsavedata_sort) option;
}

static void extsavedata_popover_select(void* data, u32 group, u32 option) {
    extsavedata_data* listData = (extsavedata_data*) data;
    if(group == 0) {
        bool* flag = option == 0 ? &listData->showSD : &listData->showNAND;
        *flag = !*flag;
    } else {
        listData->sort = (extsavedata_sort) option;
        // Sorted again at the next frame.
        listData->sorted = false;
    }

    extsavedata_store(listData);
}

static void extsavedata_filter_open(ui_view* view, void* data) {
    popover_def def = {
            .groupCount = 2,
            .groups = {
                    {_("Show"), true, 2, {"SD", "NAND"}, {ICON_DEVICE_SD_CARD, ICON_CPU}},
                    {_("Sort by"), false, 2, {_("Name"), "ID"}, {-1, -1}}
            },
            .isOn = extsavedata_popover_is_on,
            .select = extsavedata_popover_select
    };

    popover_display(&def, data);
}

static bool extsavedata_search_active(ui_view* view, void* data) {
    return list_get_search(view)[0] != '\0';
}

static void extsavedata_search_done(ui_view* kbdView, void* data, SwkbdButton button, const char* response) {
    extsavedata_data* listData = (extsavedata_data*) data;
    if(button == SWKBD_BUTTON_CONFIRM) {
        list_set_search(listData->view, response);
    }
}

static void extsavedata_search_open(ui_view* view, void* data) {
    kbd_display(_("Search data"), list_get_search(view), SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, 63, data, extsavedata_search_done);
}

// ---- the list ----

// SD before NAND; ready ones first while loading; then by name or ID.
static int extsavedata_compare(void* data, const void* p1, const void* p2) {
    extsavedata_data* listData = (extsavedata_data*) data;

    list_item* item1 = (list_item*) p1;
    list_item* item2 = (list_item*) p2;
    ext_save_data_info* info1 = (ext_save_data_info*) item1->data;
    ext_save_data_info* info2 = (ext_save_data_info*) item2->data;

    if(info1->mediaType != info2->mediaType) {
        return info1->mediaType > info2->mediaType ? -1 : 1;
    }

    if(info1->ready != info2->ready) {
        return info1->ready ? -1 : 1;
    }

    if(info1->ready && listData->sort == SORT_NAME && info1->hasMeta != info2->hasMeta) {
        return info1->hasMeta ? -1 : 1;
    }

    if(info1->ready && listData->sort == SORT_NAME && info1->hasMeta) {
        int byName = strncasecmp(item1->name, item2->name, sizeof(item1->name));
        if(byName != 0) {
            return byName;
        }
    }

    return info1->extSaveDataId > info2->extSaveDataId ? 1 : info1->extSaveDataId < info2->extSaveDataId ? -1 : 0;
}

static void extsavedata_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    extsavedata_data* listData = (extsavedata_data*) data;

    view->name = _("Ext Save Data");
    view->info = _(" actions    view    filter    search    back");

    // How many have their name and icon, for the title bar.
    u32 ready = 0;
    u32 total = 0;
    if(!listData->populateData.finished) {
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            total++;
            ready += ((ext_save_data_info*) ((list_item*) linked_list_iter_next(&iter))->data)->ready ? 1 : 0;
        }
    }

    list_set_progress(view, ready, total);

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

        task_clear_ext_save_data(items);
        list_destroy(view);

        free(listData);
        return;
    }

    if(hidKeysDown() & KEY_Y) {
        list_set_mode(view, list_get_mode(view) == LIST_MODE_GRID ? LIST_MODE_ROWS : LIST_MODE_GRID);
        extsavedata_store(listData);
    }

    if(hidKeysDown() & KEY_L) {
        extsavedata_filter_open(view, listData);
        return;
    }

    if(hidKeysDown() & KEY_R) {
        extsavedata_search_open(view, listData);
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
        listData->sortedReady = 0;
        Result res = task_populate_ext_save_data(&listData->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate ext save data list population.");
        }

        listData->populated = true;
    }

    // Sorted as they load: ready ones in order at the top, the rest after; the
    // selected one keeps its place on screen meanwhile.
    if(!listData->populateData.finished && ready != listData->sortedReady) {
        linked_list_sort(items, listData, extsavedata_compare);
        listData->sortedReady = ready;
    } else if(listData->populateData.finished && !listData->sorted) {
        linked_list_sort(items, listData, extsavedata_compare);
        listData->sorted = true;
    }

    if(listData->populateData.finished && R_FAILED(listData->populateData.result)) {
        error_display_res(NULL, NULL, listData->populateData.result, "Failed to populate ext save data list.");

        listData->populateData.result = 0;
    }

    // One still being found has nothing to act on yet.
    if(selected != NULL && selected->data != NULL && selectedTouched && ((ext_save_data_info*) selected->data)->ready) {
        extsavedata_actions_open(listData, items, selected);
        return;
    }
}

// A long hold: the actions of that one.
static void extsavedata_long_press(ui_view* view, void* data, linked_list* items, list_item* item) {
    if(item->data != NULL && ((ext_save_data_info*) item->data)->ready) {
        extsavedata_actions_open((extsavedata_data*) data, items, item);
    }
}

void extsavedata_open() {
    extsavedata_data* data = (extsavedata_data*) calloc(1, sizeof(extsavedata_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate ext save data data.");

        return;
    }

    data->populateData.userData = data;
    data->populateData.placeholders = true;
    data->populateData.finished = true;

    extsave_settings* saved = &settings_get()->extsave;
    data->showSD = saved->sd;
    data->showNAND = saved->nand;
    data->sort = saved->byId ? SORT_ID : SORT_NAME;

    ui_view* view = list_display(_("Ext Save Data"), "", data, extsavedata_update, extsavedata_draw_top);
    if(view == NULL) {
        free(data);
        return;
    }

    data->view = view;
    if(saved->grid) {
        list_set_mode(view, LIST_MODE_GRID);
    }

    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = extsavedata_draw_row,
            .tileSize = TILE_SIZE,
            .drawTile = extsavedata_draw_tile
    };
    list_set_style(view, &style);
    list_set_filter(view, extsavedata_visible);
    list_set_long_press(view, extsavedata_long_press);

    // Right to left.
    list_add_tool(view, ICON_SEARCH, extsavedata_search_active, extsavedata_search_open);
    list_add_tool(view, ICON_FILTER, extsavedata_filter_active, extsavedata_filter_open);
    list_add_tool(view, ICON_LAYOUT_GRID, extsavedata_is_grid, extsavedata_show_grid);
    list_add_tool(view, ICON_LIST, extsavedata_is_rows, extsavedata_show_rows);
}
