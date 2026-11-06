#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "settings.h"

// Game mods: the folders of /luma/titles, where Luma3DS looks for a game's mods (LayeredFS's romfs, code patches,
// locale.txt). Rows with the game's icon and name, or a grid of icons; each says whether its game is still on the
// console, so the mods of deleted games can go. The title bar's tools switch the view, filter and sort, and search; A
// or a tap opens a sheet with the actions.

typedef enum {
    SORT_NAME,
    SORT_SIZE,
    SORT_ID
} mods_sort;

typedef struct {
    populate_mods_data populateData;
    ui_view* view;

    // Hidden in place (no new listing).
    bool showPresent;
    bool showMissing;
    mods_sort sort;

    bool populated;
    bool sorted;
    u32 sortedReady;

    linked_list* items;
    list_item* actionSelected;
} mods_data;

#define ROW_HEIGHT 40
#define TILE_SIZE 50

static bool mods_present(const mod_info* info) {
    return info->state != MOD_STATE_NOT_INSTALLED;
}

static void mods_store(mods_data* data) {
    mods_settings* saved = &settings_get()->mods;
    saved->states = (data->showPresent ? 1 : 0) | (data->showMissing ? 2 : 0);
    saved->sort = (u32) data->sort;
    saved->grid = data->view != NULL && list_get_mode(data->view) == LIST_MODE_GRID;
    settings_save();
}

static void mods_restore(mods_data* data) {
    mods_settings* saved = &settings_get()->mods;
    data->showPresent = (saved->states & 1) != 0;
    data->showMissing = (saved->states & 2) != 0;
    data->sort = saved->sort <= SORT_ID ? (mods_sort) saved->sort : SORT_NAME;
}

static bool mods_visible(void* data, list_item* item) {
    mods_data* listData = (mods_data*) data;
    mod_info* info = (mod_info*) item->data;
    if(!info->ready) {
        return true;
    }

    return mods_present(info) ? listData->showPresent : listData->showMissing;
}

// ---- items ----

static const char* mods_state_tag(const mod_info* info) {
    switch(info->state) {
        case MOD_STATE_INSTALLED: return _("Installed");
        case MOD_STATE_CARD: return _("Game card");
        case MOD_STATE_RELATED: return _("Update only");
        default: return _("Not installed");
    }
}

static void mods_draw_skeleton(float x, float y, float w, float h, float icon, bool selected) {
    const theme_current* t = theme();
    u32 block = selected ? THEME_ALPHA(t->onAccent, 0x40) : t->palette->border;
    screen_fill_round_rect(x, y, icon, icon, icon / 6, block);
    if(w > icon + 30) {
        float tx = x + icon + 8;
        screen_fill_round_rect(tx, y + 4, (w - icon - 8) * 0.55f, 8, 4, block);
        screen_fill_round_rect(tx, y + h - 12, (w - icon - 8) * 0.35f, 6, 3, block);
    }
}

static void mods_fade_in(mod_info* info) {
    screen_set_content_motion(0, 0, (u8) (widget_ease(info->readyTime, 250) * 0xFF));
}

static void mods_draw_icon(mod_info* info, float x, float y, float size, bool selected) {
    const theme_current* t = theme();
    if(info->hasMeta && info->meta.texture != 0) {
        screen_draw_texture(info->meta.texture, x, y, size, size);
    } else {
        bool present = mods_present(info);
        screen_fill_round_rect(x, y, size, size, size / 6, selected ? THEME_ALPHA(t->onAccent, 0x40) : present ? t->accentSoft : t->palette->background);
        screen_draw_icon(ICON_FOLDER, x + size / 4, y + size / 4, size / 2, selected ? t->onAccent : present ? t->accent : t->palette->textMuted);
    }
}

static void mods_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    mod_info* info = (mod_info*) item->data;

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float icon = height - 10;
    if(!info->ready) {
        mods_draw_skeleton(x + 5, y + 5, width - 18, height - 10, icon, selected);
        return;
    }

    mods_fade_in(info);
    mods_draw_icon(info, x + 5, y + 5, icon, selected);

    float tx = x + 5 + icon + 8;
    float right = x + width - 8;

    bool present = mods_present(info);
    const char* tag = mods_state_tag(info);
    float tagW = widget_text_width(tag, WIDGET_SMALL_SCALE) + 12;
    widget_badge(right - tagW, y + 4, tag, selected ? THEME_ALPHA(t->onAccent, 0x40) : present ? t->accentSoft : t->palette->background, selected ? t->onAccent : present ? t->accent : t->palette->textMuted);

    widget_text_fit(item->name, tx, y + 4, WIDGET_TEXT_SCALE, ink, right - tagW - 6 - tx);

    char line[96];
    if(info->hasMeta) {
        snprintf(line, sizeof(line), "%.2f %s   %016llX", ui_get_display_size(info->size), ui_get_display_size_units(info->size), info->titleId);
    } else {
        snprintf(line, sizeof(line), "%.2f %s   %s", ui_get_display_size(info->size), ui_get_display_size_units(info->size), task_kind_name(info->titleId));
    }

    widget_text_fit(line, tx, y + height - 4 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, muted, right - tx);
    screen_set_content_motion(0, 0, 0xFF);
}

static void mods_draw_tile(void* data, list_item* item, float x, float y, float size, bool selected) {
    const theme_current* t = theme();
    mod_info* info = (mod_info*) item->data;

    if(selected) {
        screen_fill_round_rect(x - 3, y - 3, size + 6, size + 6, t->def->radius + 2, t->accent);
        screen_fill_round_rect(x, y, size, size, t->def->radius, t->accentSoft);
    } else {
        screen_frame_round_rect(x, y, size, size, t->def->radius, t->def->border, t->palette->border, t->palette->surface);
    }

    if(!info->ready) {
        mods_draw_skeleton(x + 4, y + 4, size - 8, size - 8, size - 8, selected);
        return;
    }

    mods_fade_in(info);
    mods_draw_icon(info, x + 4, y + 4, size - 8, false);
    screen_set_content_motion(0, 0, 0xFF);

    // A game that is gone: a dot in the corner.
    if(!mods_present(info)) {
        screen_fill_circle(x + size - 6, y + 6, 5.5f, t->palette->surface);
        screen_fill_circle(x + size - 6, y + 6, 4, t->palette->textMuted);
    }
}

static void mods_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL || selected->data == NULL) {
        mods_data* listData = (mods_data*) data;
        if(listData->populateData.finished && listData->items != NULL && linked_list_size(listData->items) == 0) {
            const theme_current* t = theme();
            widget_text_centered(_("No mods in /luma/titles."), x1, x2, y1 + 100, WIDGET_TEXT_SCALE, t->palette->textMuted);
        }

        return;
    }

    if(!((mod_info*) selected->data)->ready) {
        const theme_current* t = theme();
        widget_card(x1 + 10, y1 + 10, x2 - x1 - 20, 76);
        mods_draw_skeleton(x1 + 20, y1 + 20, x2 - x1 - 40, 56, 56, false);
        widget_text_centered(_("Loading..."), x1, x2, y1 + 110, WIDGET_TEXT_SCALE, t->palette->textMuted);
        return;
    }

    task_draw_mod_info(view, selected->data, x1, y1, x2, y2);
}

// ---- deleting: one folder, or those of every game that is gone ----

typedef struct {
    linked_list* items;
    list_item** targets;   // Items of the screen's list; each is freed once its folder is deleted.
    u32 count;
    char message[512];
    data_op_data deleteInfo;
} mods_delete_data;

static void mods_delete_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    mods_delete_data* deleteData = (mods_delete_data*) data;
    u32 curr = deleteData->deleteInfo.processed;
    if(curr < deleteData->count && deleteData->targets[curr] != NULL) {
        task_draw_mod_info(view, deleteData->targets[curr]->data, x1, y1, x2, y2);
    }
}

static Result mods_delete_one(void* data, u32 index) {
    mods_delete_data* deleteData = (mods_delete_data*) data;
    list_item* item = deleteData->targets[index];
    if(item == NULL) {
        // Already deleted (a retry starts again from the first).
        return 0;
    }

    Result res = task_mods_delete(((mod_info*) item->data)->titleId);
    if(R_SUCCEEDED(res)) {
        deleteData->targets[index] = NULL;
        linked_list_remove(deleteData->items, item);
        task_free_mod(item);
    }

    return res;
}

static Result mods_delete_nothing(void* data, u32 index) {
    return 0;
}

static bool mods_delete_error(void* data, u32 index, Result res, ui_view** errorView) {
    *errorView = error_display_res(data, mods_delete_draw_top, res, "Failed to delete the mods.");
    return true;
}

static void mods_delete_free(mods_delete_data* data) {
    free(data->targets);
    free(data);
}

static void mods_delete_update(ui_view* view, void* data, float* progress, char* text) {
    mods_delete_data* deleteData = (mods_delete_data*) data;

    if(deleteData->deleteInfo.finished) {
        ui_pop();
        info_destroy(view);

        if(R_SUCCEEDED(deleteData->deleteInfo.result)) {
            prompt_display_notify("Success", "Mods deleted.", COLOR_TEXT, NULL, NULL, NULL);
        }

        mods_delete_free(deleteData);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        svcSignalEvent(deleteData->deleteInfo.cancelEvent);
    }

    *progress = deleteData->deleteInfo.total > 0 ? (float) deleteData->deleteInfo.processed / (float) deleteData->deleteInfo.total : 0;
    snprintf(text, PROGRESS_TEXT_MAX, _("%lu / %lu"), deleteData->deleteInfo.processed, deleteData->deleteInfo.total);
}

static void mods_delete_answer(ui_view* view, void* data, u32 response) {
    mods_delete_data* deleteData = (mods_delete_data*) data;
    if(response != PROMPT_YES) {
        mods_delete_free(deleteData);
        return;
    }

    deleteData->deleteInfo.processed = 0;
    Result res = task_data_op(&deleteData->deleteInfo);
    if(R_SUCCEEDED(res)) {
        info_display("Deleting", "Press B to cancel.", true, data, mods_delete_update, mods_delete_draw_top);
    } else {
        error_display_res(NULL, NULL, res, "Failed to initiate delete operation.");
        mods_delete_free(deleteData);
    }
}

static mods_delete_data* mods_delete_new(linked_list* items, u32 count) {
    mods_delete_data* data = (mods_delete_data*) calloc(1, sizeof(mods_delete_data));
    if(data == NULL || (data->targets = (list_item**) calloc(count, sizeof(list_item*))) == NULL) {
        free(data);
        error_display(NULL, NULL, "Failed to allocate delete data.");
        return NULL;
    }

    data->items = items;
    data->deleteInfo.data = data;
    data->deleteInfo.op = DATAOP_DELETE;
    data->deleteInfo.delete = mods_delete_one;
    data->deleteInfo.suspend = mods_delete_nothing;
    data->deleteInfo.restore = mods_delete_nothing;
    data->deleteInfo.error = mods_delete_error;
    data->deleteInfo.finished = false;
    return data;
}

// The prompt's top screen shows the first folder (processed = 0 is the first).
static void mods_delete_ask(mods_delete_data* data) {
    data->deleteInfo.total = data->count;
    data->deleteInfo.processed = 0;
    prompt_display_warning("Confirmation", data->message, COLOR_TEXT, data, mods_delete_draw_top, mods_delete_answer);
}

static void mods_delete(void* data) {
    mods_data* listData = (mods_data*) data;
    mods_delete_data* deleteData = mods_delete_new(listData->items, 1);
    if(deleteData == NULL) {
        return;
    }

    deleteData->targets[0] = listData->actionSelected;
    deleteData->count = 1;

    mod_info* info = (mod_info*) listData->actionSelected->data;
    const char* question = mods_present(info) ? _("Delete these mods? The game stays, without them.") : _("Delete these mods?");
    snprintf(deleteData->message, sizeof(deleteData->message), "%s\n" MODS_ROOT "%s/", question, info->folder);
    mods_delete_ask(deleteData);
}

static u32 mods_count_unused(linked_list* items, u64* size) {
    u32 count = 0;
    *size = 0;
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        mod_info* info = (mod_info*) ((list_item*) linked_list_iter_next(&iter))->data;
        if(task_mod_unused(info)) {
            count++;
            *size += info->size;
        }
    }

    return count;
}

static void mods_delete_unused(void* data) {
    mods_data* listData = (mods_data*) data;
    u64 size = 0;
    u32 count = mods_count_unused(listData->items, &size);
    if(count == 0) {
        return;
    }

    mods_delete_data* deleteData = mods_delete_new(listData->items, count);
    if(deleteData == NULL) {
        return;
    }

    linked_list_iter iter;
    linked_list_iterate(listData->items, &iter);
    while(linked_list_iter_has_next(&iter) && deleteData->count < count) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        if(task_mod_unused((mod_info*) item->data)) {
            deleteData->targets[deleteData->count++] = item;
        }
    }

    snprintf(deleteData->message, sizeof(deleteData->message), i18n_translate_text("Delete the mods of %lu games that are not installed (%.2f %s)?\nA game card's mods are among them while the card is out of the slot."),
             deleteData->count, ui_get_display_size(size), ui_get_display_size_units(size));
    mods_delete_ask(deleteData);
}

static void mods_actions_open(mods_data* data, list_item* selected) {
    data->actionSelected = selected;

    sheet_action actions[2];
    u32 count = 0;
    actions[count++] = (sheet_action) {ICON_TRASH, _("Delete mods"), true, mods_delete};

    // Only once every game is known: until then a game on the console could look gone.
    u64 size = 0;
    if(data->populateData.finished && mods_count_unused(data->items, &size) > 0) {
        actions[count++] = (sheet_action) {ICON_TRASH_X, _("Delete mods of games not installed"), true, mods_delete_unused};
    }

    sheet_display_list(NULL, 0, actions, count, data);
}

// ---- tools: view, filter and sort, search ----

static bool mods_is_rows(ui_view* view, void* data) {
    return list_get_mode(view) == LIST_MODE_ROWS;
}

static bool mods_is_grid(ui_view* view, void* data) {
    return list_get_mode(view) == LIST_MODE_GRID;
}

static void mods_show_rows(ui_view* view, void* data) {
    list_set_mode(view, LIST_MODE_ROWS);
    mods_store((mods_data*) data);
}

static void mods_show_grid(ui_view* view, void* data) {
    list_set_mode(view, LIST_MODE_GRID);
    mods_store((mods_data*) data);
}

static bool mods_filter_active(ui_view* view, void* data) {
    mods_data* listData = (mods_data*) data;
    return !listData->showPresent || !listData->showMissing;
}

// Popover groups: what to show, sort.
static bool mods_popover_is_on(void* data, u32 group, u32 option) {
    mods_data* listData = (mods_data*) data;
    if(group == 0) {
        return option == 0 ? listData->showPresent : listData->showMissing;
    }

    return listData->sort == (mods_sort) option;
}

static void mods_popover_select(void* data, u32 group, u32 option) {
    mods_data* listData = (mods_data*) data;
    if(group == 0) {
        bool* flag = option == 0 ? &listData->showPresent : &listData->showMissing;
        *flag = !*flag;
    } else {
        listData->sort = (mods_sort) option;
        listData->sorted = false;
    }

    mods_store(listData);
}

static void mods_filter_open(ui_view* view, void* data) {
    popover_def def = {
            .groupCount = 2,
            .groups = {
                    {_("Show"), true, 2, {_("Game on the console"), _("Not installed")}, {-1, -1}},
                    {_("Sort by"), false, 3, {_("Name"), _("Size"), "ID"}, {ICON_ABC, ICON_DATABASE, ICON_KEY}, true}
            },
            .isOn = mods_popover_is_on,
            .select = mods_popover_select
    };

    popover_display(&def, data);
}

static bool mods_search_active(ui_view* view, void* data) {
    return list_get_search(view)[0] != '\0';
}

static void mods_search_done(ui_view* kbdView, void* data, SwkbdButton button, const char* response) {
    mods_data* listData = (mods_data*) data;
    if(button == SWKBD_BUTTON_CONFIRM) {
        list_set_search(listData->view, response);
    }
}

static void mods_search_open(ui_view* view, void* data) {
    osk_display(_("Search mods"), list_get_search(view), 64, 0, data, mods_search_done);
}

// ---- the list ----

// Named games first by name (the rest by ID), or by size, biggest first, or all by ID.
static int mods_compare(void* data, const void* p1, const void* p2) {
    mods_data* listData = (mods_data*) data;

    list_item* item1 = (list_item*) p1;
    list_item* item2 = (list_item*) p2;
    mod_info* mod1 = (mod_info*) item1->data;
    mod_info* mod2 = (mod_info*) item2->data;

    if(mod1->ready != mod2->ready) {
        return mod1->ready ? -1 : 1;
    } else if(mod1->ready) {
        if(listData->sort == SORT_NAME && mod1->hasMeta != mod2->hasMeta) {
            return mod1->hasMeta ? -1 : 1;
        }

        if(listData->sort == SORT_NAME && mod1->hasMeta) {
            int byName = strncasecmp(item1->name, item2->name, sizeof(item1->name));
            if(byName != 0) {
                return byName;
            }
        }

        if(listData->sort == SORT_SIZE && mod1->size != mod2->size) {
            return mod1->size > mod2->size ? -1 : 1;
        }
    }

    return mod1->titleId > mod2->titleId ? 1 : mod1->titleId < mod2->titleId ? -1 : 0;
}

static void mods_stop(mods_data* listData) {
    if(!listData->populateData.finished) {
        svcSignalEvent(listData->populateData.cancelEvent);
        while(!listData->populateData.finished) {
            svcSleepThread(1000000);
        }
    }
}

static void mods_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    mods_data* listData = (mods_data*) data;

    view->name = _("Game mods");
    listData->items = items;

    u32 ready = 0;
    u32 total = 0;
    if(!listData->populateData.finished) {
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            total++;
            ready += ((mod_info*) ((list_item*) linked_list_iter_next(&iter))->data)->ready ? 1 : 0;
        }
    }

    list_set_progress(view, ready, total);
    view->info = _(" actions    view    filter    search    back");

    if(hidKeysDown() & KEY_B) {
        if(list_get_search(view)[0] != '\0') {
            list_set_search(view, "");
            return;
        }

        mods_stop(listData);
        ui_pop();

        task_clear_mods(items);
        list_destroy(view);

        free(listData);
        return;
    }

    if(hidKeysDown() & KEY_Y) {
        list_set_mode(view, list_get_mode(view) == LIST_MODE_GRID ? LIST_MODE_ROWS : LIST_MODE_GRID);
        mods_store(listData);
    }

    if(hidKeysDown() & KEY_L) {
        mods_filter_open(view, listData);
        return;
    }

    if(hidKeysDown() & KEY_R) {
        mods_search_open(view, listData);
        return;
    }

    if(!listData->populated || (hidKeysDown() & KEY_X)) {
        mods_stop(listData);

        listData->populateData.items = items;
        listData->sorted = false;
        listData->sortedReady = 0;
        Result res = task_populate_mods(&listData->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate mods list population.");
        }

        listData->populated = true;
    }

    if(!listData->populateData.finished && ready != listData->sortedReady) {
        linked_list_sort(items, listData, mods_compare);
        listData->sortedReady = ready;
    } else if(listData->populateData.finished && !listData->sorted) {
        linked_list_sort(items, listData, mods_compare);
        listData->sorted = true;
    }

    if(listData->populateData.finished && R_FAILED(listData->populateData.result)) {
        error_display_res(NULL, NULL, listData->populateData.result, "Failed to populate mods list.");
        listData->populateData.result = 0;
    }

    // Deleting removes items, which the populate thread walks: only once it is done (it takes a moment).
    if(selected != NULL && selected->data != NULL && selectedTouched && listData->populateData.finished) {
        mods_actions_open(listData, selected);
    }
}

void mods_open() {
    mods_data* data = (mods_data*) calloc(1, sizeof(mods_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate mods data.");
        return;
    }

    data->populateData.finished = true;
    mods_restore(data);

    ui_view* view = list_display(_("Game mods"), "", data, mods_update, mods_draw_top);
    if(view == NULL) {
        free(data);
        return;
    }

    data->view = view;
    if(settings_get()->mods.grid) {
        list_set_mode(view, LIST_MODE_GRID);
    }

    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = mods_draw_row,
            .tileSize = TILE_SIZE,
            .drawTile = mods_draw_tile
    };
    list_set_style(view, &style);
    list_set_filter(view, mods_visible);

    // Right to left.
    list_add_tool(view, ICON_SEARCH, mods_search_active, mods_search_open);
    list_add_tool(view, ICON_FILTER, mods_filter_active, mods_filter_open);
    list_add_tool(view, ICON_LAYOUT_GRID, mods_is_grid, mods_show_grid);
    list_add_tool(view, ICON_LIST, mods_is_rows, mods_show_rows);
}
