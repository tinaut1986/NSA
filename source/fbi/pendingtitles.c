#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "settings.h"

// Pending titles: installs that were left half way, as rows (what it was, its ID, where it is).
// The title bar's tools filter by memory and search; A or a tap opens a sheet with the actions
// (deleting in red).

#define ROW_HEIGHT 40

typedef struct {
    populate_pending_titles_data populateData;
    ui_view* view;

    // Hidden in place (no new listing).
    bool showSD;
    bool showNAND;

    bool populated;
    bool sorted;

    // The pending title the action sheet is for.
    linked_list* actionItems;
    list_item* actionSelected;
} pendingtitles_data;

static void pendingtitles_store(pendingtitles_data* data) {
    pending_settings* saved = &settings_get()->pending;
    saved->sd = data->showSD;
    saved->nand = data->showNAND;
    settings_save();
}

static bool pendingtitles_visible(void* data, list_item* item) {
    pendingtitles_data* listData = (pendingtitles_data*) data;
    pending_title_info* info = (pending_title_info*) item->data;
    return info->mediaType == MEDIATYPE_SD ? listData->showSD : listData->showNAND;
}

// ---- items ----

static void pendingtitles_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    pending_title_info* info = (pending_title_info*) item->data;

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float icon = height - 10;
    screen_fill_round_rect(x + 5, y + 5, icon, icon, icon / 6, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft);
    screen_draw_icon(ICON_CLOCK, x + 5 + icon / 4, y + 5 + icon / 4, icon / 2, selected ? t->onAccent : t->accent);

    float tx = x + 5 + icon + 8;
    float right = x + width - 8;

    // Where it is, as a tag.
    const char* tag = info->mediaType == MEDIATYPE_SD ? "SD" : "NAND";
    float tagW = widget_text_width(tag, WIDGET_SMALL_SCALE) + 12;
    widget_badge(right - tagW, y + 4, tag, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft, selected ? t->onAccent : t->accent);

    widget_text_fit(task_kind_name(info->titleId), tx, y + 4, WIDGET_TEXT_SCALE, ink, right - tagW - 6 - tx);

    // Second line: the ID, and the version on the right.
    float lineY = y + height - 4 - widget_text_height(WIDGET_SMALL_SCALE);
    float idW = widget_text_width(item->name, WIDGET_SMALL_SCALE);
    widget_text_fit(item->name, tx, lineY, WIDGET_SMALL_SCALE, muted, right - tx);

    char version[48];
    task_version_text(version, sizeof(version), info->version);
    float versionW = widget_text_width(version, WIDGET_SMALL_SCALE);
    if(tx + idW + 14 + versionW <= right) {
        screen_draw_text(version, right - versionW, lineY, WIDGET_SMALL_SCALE, muted);
    }
}

static void pendingtitles_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected != NULL && selected->data != NULL) {
        task_draw_pending_title_info(view, selected->data, x1, y1, x2, y2);
    }
}

// ---- actions: a sheet, deleting in red ----

static void pendingtitles_delete(void* data) {
    pendingtitles_data* listData = (pendingtitles_data*) data;
    action_delete_pending_title(listData->actionItems, listData->actionSelected);
}

static void pendingtitles_delete_all(void* data) {
    pendingtitles_data* listData = (pendingtitles_data*) data;
    action_delete_all_pending_titles(listData->actionItems, listData->actionSelected);
}

static void pendingtitles_actions_open(pendingtitles_data* data, linked_list* items, list_item* selected) {
    data->actionItems = items;
    data->actionSelected = selected;

    sheet_action actions[] = {
            {ICON_TRASH, _("Delete pending title"), true, pendingtitles_delete, false},
            {ICON_TRASH, _("Delete all pending titles"), true, pendingtitles_delete_all, false}
    };

    // No header: the top screen already shows it.
    sheet_display_list(NULL, 0, actions, sizeof(actions) / sizeof(*actions), data);
}

// ---- tools: filter, search ----

static bool pendingtitles_filter_active(ui_view* view, void* data) {
    pendingtitles_data* listData = (pendingtitles_data*) data;
    return !listData->showSD || !listData->showNAND;
}

static bool pendingtitles_popover_is_on(void* data, u32 group, u32 option) {
    pendingtitles_data* listData = (pendingtitles_data*) data;
    return option == 0 ? listData->showSD : listData->showNAND;
}

static void pendingtitles_popover_select(void* data, u32 group, u32 option) {
    pendingtitles_data* listData = (pendingtitles_data*) data;
    bool* flag = option == 0 ? &listData->showSD : &listData->showNAND;
    *flag = !*flag;
    pendingtitles_store(listData);
}

static void pendingtitles_filter_open(ui_view* view, void* data) {
    popover_def def = {
            .groupCount = 1,
            .groups = {
                    {_("Show"), true, 2, {"SD", "NAND"}, {ICON_DEVICE_SD_CARD, ICON_CPU}}
            },
            .isOn = pendingtitles_popover_is_on,
            .select = pendingtitles_popover_select
    };

    popover_display(&def, data);
}

static bool pendingtitles_search_active(ui_view* view, void* data) {
    return list_get_search(view)[0] != '\0';
}

static void pendingtitles_search_done(ui_view* kbdView, void* data, SwkbdButton button, const char* response) {
    pendingtitles_data* listData = (pendingtitles_data*) data;
    if(button == SWKBD_BUTTON_CONFIRM) {
        list_set_search(listData->view, response);
    }
}

static void pendingtitles_search_open(ui_view* view, void* data) {
    kbd_display(_("Search titles"), list_get_search(view), SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, 63, data, pendingtitles_search_done);
}

// ---- the list ----

// SD before NAND, then by title ID.
static int pendingtitles_compare(void* data, const void* p1, const void* p2) {
    pending_title_info* info1 = (pending_title_info*) ((list_item*) p1)->data;
    pending_title_info* info2 = (pending_title_info*) ((list_item*) p2)->data;

    if(info1->mediaType != info2->mediaType) {
        return info1->mediaType > info2->mediaType ? -1 : 1;
    }

    return info1->titleId > info2->titleId ? 1 : info1->titleId < info2->titleId ? -1 : 0;
}

static void pendingtitles_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    pendingtitles_data* listData = (pendingtitles_data*) data;

    view->name = _("Pending Titles");
    view->info = _("\uE000 actions   \uE004 filter   \uE005 search   \uE002 refresh   \uE001 back");

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

        task_clear_pending_titles(items);
        list_destroy(view);

        free(listData);
        return;
    }

    if(hidKeysDown() & KEY_L) {
        pendingtitles_filter_open(view, listData);
        return;
    }

    if(hidKeysDown() & KEY_R) {
        pendingtitles_search_open(view, listData);
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
        Result res = task_populate_pending_titles(&listData->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate pending title list population.");
        }

        listData->populated = true;
    }

    if(listData->populateData.finished && !listData->sorted) {
        linked_list_sort(items, listData, pendingtitles_compare);
        listData->sorted = true;
    }

    if(listData->populateData.finished && R_FAILED(listData->populateData.result)) {
        error_display_res(NULL, NULL, listData->populateData.result, "Failed to populate pending title list.");

        listData->populateData.result = 0;
    }

    if(selected != NULL && selected->data != NULL && selectedTouched) {
        pendingtitles_actions_open(listData, items, selected);
        return;
    }
}

// A long hold: the actions of that one.
static void pendingtitles_long_press(ui_view* view, void* data, linked_list* items, list_item* item) {
    if(item->data != NULL) {
        pendingtitles_actions_open((pendingtitles_data*) data, items, item);
    }
}

void pendingtitles_open() {
    pendingtitles_data* data = (pendingtitles_data*) calloc(1, sizeof(pendingtitles_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate pending titles data.");

        return;
    }

    data->populateData.finished = true;

    pending_settings* saved = &settings_get()->pending;
    data->showSD = saved->sd;
    data->showNAND = saved->nand;

    ui_view* view = list_display(_("Pending Titles"), "", data, pendingtitles_update, pendingtitles_draw_top);
    if(view == NULL) {
        free(data);
        return;
    }

    data->view = view;

    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = pendingtitles_draw_row
    };
    list_set_style(view, &style);
    list_set_filter(view, pendingtitles_visible);
    list_set_long_press(view, pendingtitles_long_press);

    // Right to left.
    list_add_tool(view, ICON_SEARCH, pendingtitles_search_active, pendingtitles_search_open);
    list_add_tool(view, ICON_FILTER, pendingtitles_filter_active, pendingtitles_filter_open);
}
