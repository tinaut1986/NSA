#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"

// System save data: the console's own saves, as rows with their ID. The title bar's search
// filters them; A or a tap opens a sheet with the actions (deleting in red, with a warning).

#define ROW_HEIGHT 36

typedef struct {
    populate_system_save_data_data populateData;
    ui_view* view;

    bool populated;
    bool sorted;

    // The save data the action sheet is for.
    linked_list* actionItems;
    list_item* actionSelected;
} systemsavedata_data;

static void systemsavedata_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float icon = height - 10;
    screen_fill_round_rect(x + 5, y + 5, icon, icon, icon / 6, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft);
    screen_draw_icon(ICON_DEVICE_FLOPPY, x + 5 + icon / 4, y + 5 + icon / 4, icon / 2, selected ? t->onAccent : t->accent);

    float tx = x + 5 + icon + 8;
    float right = x + width - 8;
    widget_text_fit(item->name, tx, y + 4, WIDGET_TEXT_SCALE, ink, right - tx);
    widget_text_fit(_("System save data"), tx, y + height - 4 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, muted, right - tx);
}

static void systemsavedata_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected != NULL && selected->data != NULL) {
        task_draw_system_save_data_info(view, selected->data, x1, y1, x2, y2);
    }
}

// ---- actions: a sheet, deleting in red ----

static void systemsavedata_browse(void* data) {
    systemsavedata_data* listData = (systemsavedata_data*) data;
    action_browse_system_save_data(listData->actionItems, listData->actionSelected);
}

static void systemsavedata_delete(void* data) {
    systemsavedata_data* listData = (systemsavedata_data*) data;
    action_delete_system_save_data(listData->actionItems, listData->actionSelected);
}

static void systemsavedata_actions_open(systemsavedata_data* data, linked_list* items, list_item* selected) {
    data->actionItems = items;
    data->actionSelected = selected;

    // Browsing leaves the sheet open under it: B comes back to the sheet.
    sheet_action actions[] = {
            {ICON_FOLDER, _("Browse save data"), false, systemsavedata_browse, true},
            {ICON_TRASH, _("Delete save data"), true, systemsavedata_delete, false}
    };

    // No header: the top screen already shows it.
    sheet_display_list(NULL, 0, actions, sizeof(actions) / sizeof(*actions), data);
}

// ---- search ----

static bool systemsavedata_search_active(ui_view* view, void* data) {
    return list_get_search(view)[0] != '\0';
}

static void systemsavedata_search_done(ui_view* kbdView, void* data, SwkbdButton button, const char* response) {
    systemsavedata_data* listData = (systemsavedata_data*) data;
    if(button == SWKBD_BUTTON_CONFIRM) {
        list_set_search(listData->view, response);
    }
}

static void systemsavedata_search_open(ui_view* view, void* data) {
    kbd_display(_("Search data"), list_get_search(view), SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, 63, data, systemsavedata_search_done);
}

// ---- the list ----

static void systemsavedata_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    systemsavedata_data* listData = (systemsavedata_data*) data;

    view->name = _("System save data");
    view->info = _(" actions    search    refresh    back");

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

        task_clear_system_save_data(items);
        list_destroy(view);

        free(listData);
        return;
    }

    if(hidKeysDown() & KEY_R) {
        systemsavedata_search_open(view, listData);
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
        Result res = task_populate_system_save_data(&listData->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate system save data list population.");
        }

        listData->populated = true;
    }

    if(listData->populateData.finished && R_FAILED(listData->populateData.result)) {
        error_display_res(NULL, NULL, listData->populateData.result, "Failed to populate system save data list.");

        listData->populateData.result = 0;
    }

    if(selected != NULL && selected->data != NULL && selectedTouched) {
        systemsavedata_actions_open(listData, items, selected);
        return;
    }
}

// A long hold: the actions of that one.
static void systemsavedata_long_press(ui_view* view, void* data, linked_list* items, list_item* item) {
    if(item->data != NULL) {
        systemsavedata_actions_open((systemsavedata_data*) data, items, item);
    }
}

void systemsavedata_open() {
    systemsavedata_data* data = (systemsavedata_data*) calloc(1, sizeof(systemsavedata_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate system save data data.");

        return;
    }

    data->populateData.finished = true;

    ui_view* view = list_display(_("System save data"), "", data, systemsavedata_update, systemsavedata_draw_top);
    if(view == NULL) {
        free(data);
        return;
    }

    data->view = view;

    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = systemsavedata_draw_row
    };
    list_set_style(view, &style);
    list_set_long_press(view, systemsavedata_long_press);

    list_add_tool(view, ICON_SEARCH, systemsavedata_search_active, systemsavedata_search_open);
}
