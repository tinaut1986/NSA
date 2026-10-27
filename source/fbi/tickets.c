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

// Tickets: rows with the icon and name of the title each belongs to, or a grid
// of icons. The title bar's tools switch the view, filter and sort, and search;
// A or a tap opens a sheet with the actions, and a game's update and DLC
// tickets can be grouped under its own.

typedef enum {
    SORT_NAME,
    SORT_ID
} tickets_sort;

typedef struct {
    populate_tickets_data populateData;
    ui_view* view;

    // Hidden in place (no new listing).
    bool showInUse;
    bool showNotInUse;
    bool showKind[6];
    bool showSD;
    bool showNAND;
    bool group;
    tickets_sort sort;

    bool populated;
    bool sorted;

    // Sorted as tickets load (and once more when all have).
    u32 sortedReady;

    // When the list last had its grouping worked out.
    u32 groupedCount;
    bool groupedFinished;

    // The list's items, and the ticket the action sheet is for.
    linked_list* items;
    linked_list* actionItems;
    list_item* actionSelected;
} tickets_data;

#define ROW_HEIGHT 40
#define TILE_SIZE 50

static const char* tickets_kind_name(u64 titleId) {
    switch(title_kind_of(titleId)) {
        case TITLE_KIND_GAME: return _("Game");
        case TITLE_KIND_UPDATE: return _("Update");
        case TITLE_KIND_DLC: return "DLC";
        case TITLE_KIND_DEMO: return _("Demo");
        case TITLE_KIND_DSIWARE: return "DSiWare";
        default: return _("System");
    }
}

static bool tickets_is_extra(title_kind kind) {
    return kind == TITLE_KIND_UPDATE || kind == TITLE_KIND_DLC;
}

// ---- grouping ----

// Works out which update and DLC tickets hang from a game's, when the list has
// changed (the populate task adds items as it goes).
static void tickets_group(tickets_data* data, linked_list* items) {
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
        ticket_info* info = (ticket_info*) ((list_item*) linked_list_iter_next(&iter))->data;
        info->linkedCount = 0;
        info->linked = false;
    }

    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        ticket_info* extra = (ticket_info*) ((list_item*) linked_list_iter_next(&iter))->data;
        if(!tickets_is_extra(title_kind_of(extra->titleId))) {
            continue;
        }

        linked_list_iter games;
        linked_list_iterate(items, &games);
        while(linked_list_iter_has_next(&games)) {
            ticket_info* game = (ticket_info*) ((list_item*) linked_list_iter_next(&games))->data;
            if(title_kind_of(game->titleId) == TITLE_KIND_GAME && (u32) game->titleId == (u32) extra->titleId) {
                game->linkedCount++;
                extra->linked = true;
                break;
            }
        }
    }
}

// What the screen shows is kept in the settings, for the next time.
static void tickets_store(tickets_data* data) {
    tickets_settings* saved = &settings_get()->tickets;
    saved->states = (data->showInUse ? 1 : 0) | (data->showNotInUse ? 2 : 0);
    saved->kinds = 0;
    for(u32 i = 0; i < 6; i++) {
        saved->kinds |= data->showKind[i] ? 1u << i : 0;
    }

    saved->media = (data->showSD ? 1 : 0) | (data->showNAND ? 2 : 0);
    saved->group = data->group;
    saved->sort = (u32) data->sort;
    saved->grid = data->view != NULL && list_get_mode(data->view) == LIST_MODE_GRID;
    settings_save();
}

static void tickets_restore(tickets_data* data) {
    tickets_settings* saved = &settings_get()->tickets;
    data->showInUse = (saved->states & 1) != 0;
    data->showNotInUse = (saved->states & 2) != 0;
    for(u32 i = 0; i < 6; i++) {
        data->showKind[i] = (saved->kinds >> i) & 1;
    }

    data->showSD = (saved->media & 1) != 0;
    data->showNAND = (saved->media & 2) != 0;
    data->group = saved->group;
    data->sort = saved->sort <= SORT_ID ? (tickets_sort) saved->sort : SORT_NAME;
}

// State, kind and memory filters, and grouping: update and DLC tickets whose
// game's ticket is in the list hide behind it.
static bool tickets_visible(void* data, list_item* item) {
    tickets_data* listData = (tickets_data*) data;
    ticket_info* info = (ticket_info*) item->data;

    if(!listData->showKind[title_kind_of(info->titleId)]) {
        return false;
    }

    // Whether it is in use, and where, is known once its title is found.
    if(info->ready) {
        if(info->inUse ? !listData->showInUse : !listData->showNotInUse) {
            return false;
        }

        // Memory only tells installed titles apart.
        if(info->inUse && (info->mediaType == MEDIATYPE_SD ? !listData->showSD : !listData->showNAND)) {
            return false;
        }
    }

    return !listData->group || !info->linked || !listData->showKind[TITLE_KIND_GAME];
}

// ---- items ----

static void tickets_draw_count_badge(u32 count, float x, float y) {
    const theme_current* t = theme();
    char text[8];
    snprintf(text, sizeof(text), "%lu", count);
    float r = 8;
    screen_fill_circle(x, y, r + 1.5f, t->palette->surface);
    screen_fill_circle(x, y, r, t->accent);
    float w = widget_text_width(text, WIDGET_SMALL_SCALE);
    screen_draw_text(text, x - w / 2, y - widget_text_height(WIDGET_SMALL_SCALE) / 2, WIDGET_SMALL_SCALE, t->onAccent);
}

// While a ticket's title is found: grey blocks where its icon and text will be.
static void tickets_draw_skeleton(float x, float y, float w, float h, float icon, bool selected) {
    const theme_current* t = theme();
    u32 block = selected ? THEME_ALPHA(t->onAccent, 0x40) : t->palette->border;
    screen_fill_round_rect(x, y, icon, icon, icon / 6, block);
    if(w > icon + 30) {
        float tx = x + icon + 8;
        screen_fill_round_rect(tx, y + 4, (w - icon - 8) * 0.55f, 8, 4, block);
        screen_fill_round_rect(tx, y + h - 12, (w - icon - 8) * 0.35f, 6, 3, block);
    }
}

// A ticket's contents fade in once its title has been found.
static void tickets_fade_in(ticket_info* info) {
    screen_set_content_motion(0, 0, (u8) (widget_ease(info->readyTime, 250) * 0xFF));
}

static void tickets_draw_icon(ticket_info* info, float x, float y, float size, bool selected) {
    const theme_current* t = theme();
    if(info->hasMeta && info->meta.texture != 0) {
        screen_draw_texture(info->meta.texture, x, y, size, size);
    } else {
        screen_fill_round_rect(x, y, size, size, size / 6, selected ? THEME_ALPHA(t->onAccent, 0x40) : info->inUse ? t->accentSoft : t->palette->background);
        screen_draw_icon(ICON_TICKET, x + size / 4, y + size / 4, size / 2, selected ? t->onAccent : info->inUse ? t->accent : t->palette->textMuted);
    }
}

static void tickets_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    ticket_info* info = (ticket_info*) item->data;

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float icon = height - 10;
    if(!info->ready) {
        tickets_draw_skeleton(x + 5, y + 5, width - 18, height - 10, icon, selected);
        return;
    }

    tickets_fade_in(info);
    tickets_draw_icon(info, x + 5, y + 5, icon, selected);

    tickets_data* listData = (tickets_data*) data;
    if(listData != NULL && listData->group && info->linkedCount > 0) {
        tickets_draw_count_badge(info->linkedCount, x + 5 + icon - 1, y + 7);
    }

    float tx = x + 5 + icon + 8;
    float right = x + width - 8;

    // The state as a tag; without an installed title, a ticket is only an ID.
    const char* tag = info->inUse ? _("In use") : _("Not in use");
    float tagW = widget_text_width(tag, WIDGET_SMALL_SCALE) + 12;
    widget_badge(right - tagW, y + 4, tag, selected ? THEME_ALPHA(t->onAccent, 0x40) : info->inUse ? t->accentSoft : t->palette->background, selected ? t->onAccent : info->inUse ? t->accent : t->palette->textMuted);

    widget_text_fit(item->name, tx, y + 4, WIDGET_TEXT_SCALE, ink, right - tagW - 6 - tx);

    char line[64];
    if(info->hasMeta) {
        snprintf(line, sizeof(line), "%s   %016llX", tickets_kind_name(info->titleId), info->titleId);
    } else {
        snprintf(line, sizeof(line), "%s", info->inUse ? tickets_kind_name(info->titleId) : _("No installed title"));
    }

    widget_text_fit(line, tx, y + height - 4 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, muted, right - tx);
    screen_set_content_motion(0, 0, 0xFF);
}

static void tickets_draw_tile(void* data, list_item* item, float x, float y, float size, bool selected) {
    const theme_current* t = theme();
    ticket_info* info = (ticket_info*) item->data;

    if(selected) {
        screen_fill_round_rect(x - 3, y - 3, size + 6, size + 6, t->def->radius + 2, t->accent);
        screen_fill_round_rect(x, y, size, size, t->def->radius, t->accentSoft);
    } else {
        screen_frame_round_rect(x, y, size, size, t->def->radius, t->def->border, t->palette->border, t->palette->surface);
    }

    if(!info->ready) {
        tickets_draw_skeleton(x + 4, y + 4, size - 8, size - 8, size - 8, selected);
        return;
    }

    tickets_fade_in(info);
    tickets_draw_icon(info, x + 4, y + 4, size - 8, false);
    screen_set_content_motion(0, 0, 0xFF);

    tickets_data* listData = (tickets_data*) data;
    if(listData != NULL && listData->group && info->linkedCount > 0) {
        tickets_draw_count_badge(info->linkedCount, x + size - 4, y + 4);
    }
}

// Draws the selected ticket's details, or its place while its title is found.
static void tickets_draw_details(ui_view* view, list_item* selected, float x1, float y1, float x2, float y2) {
    if(selected == NULL || selected->data == NULL) {
        return;
    }

    if(!((ticket_info*) selected->data)->ready) {
        const theme_current* t = theme();
        widget_card(x1 + 10, y1 + 10, x2 - x1 - 20, 76);
        tickets_draw_skeleton(x1 + 20, y1 + 20, x2 - x1 - 40, 56, 56, false);
        widget_text_centered(_("Loading..."), x1, x2, y1 + 110, WIDGET_TEXT_SCALE, t->palette->textMuted);
        return;
    }

    task_draw_ticket_info(view, selected->data, x1, y1, x2, y2);
}

static void tickets_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    tickets_draw_details(view, selected, x1, y1, x2, y2);
}

// ---- actions: a sheet, deleting in red ----

static void tickets_delete(void* data) {
    tickets_data* listData = (tickets_data*) data;
    action_delete_ticket(listData->actionItems, listData->actionSelected);
}

static void tickets_delete_unused(void* data) {
    tickets_data* listData = (tickets_data*) data;
    action_delete_tickets_unused(listData->actionItems, listData->actionSelected);
}

static void tickets_actions_open(tickets_data* data, linked_list* items, list_item* selected) {
    data->actionItems = items;
    data->actionSelected = selected;

    sheet_action actions[] = {
            {ICON_TRASH, _("Delete ticket"), true, tickets_delete},
            {ICON_TRASH_X, _("Delete unused tickets"), true, tickets_delete_unused}
    };

    // No header: the top screen already shows the ticket.
    sheet_display_list(NULL, 0, actions, sizeof(actions) / sizeof(*actions), data);
}

// ---- tools: view, filter and sort, search ----

static bool tickets_is_rows(ui_view* view, void* data) {
    return list_get_mode(view) == LIST_MODE_ROWS;
}

static bool tickets_is_grid(ui_view* view, void* data) {
    return list_get_mode(view) == LIST_MODE_GRID;
}

static void tickets_show_rows(ui_view* view, void* data) {
    list_set_mode(view, LIST_MODE_ROWS);
    tickets_store((tickets_data*) data);
}

static void tickets_show_grid(ui_view* view, void* data) {
    list_set_mode(view, LIST_MODE_GRID);
    tickets_store((tickets_data*) data);
}

static bool tickets_filter_active(ui_view* view, void* data) {
    tickets_data* listData = (tickets_data*) data;
    bool all = listData->showInUse && listData->showNotInUse && listData->showSD && listData->showNAND && listData->group;
    for(u32 i = 0; i < 6; i++) {
        all = all && listData->showKind[i];
    }

    return !all;
}

// Popover groups: state, kind, memory, grouping, sort.
static bool tickets_popover_is_on(void* data, u32 group, u32 option) {
    tickets_data* listData = (tickets_data*) data;
    switch(group) {
        case 0: return option == 0 ? listData->showInUse : listData->showNotInUse;
        case 1: return listData->showKind[option];
        case 2: return option == 0 ? listData->showSD : listData->showNAND;
        case 3: return listData->group == (option == 0);
        default: return listData->sort == (tickets_sort) option;
    }
}

static void tickets_popover_select(void* data, u32 group, u32 option) {
    tickets_data* listData = (tickets_data*) data;
    switch(group) {
        case 0: {
            bool* flag = option == 0 ? &listData->showInUse : &listData->showNotInUse;
            *flag = !*flag;
            break;
        }
        case 1:
            listData->showKind[option] = !listData->showKind[option];
            break;
        case 2: {
            bool* flag = option == 0 ? &listData->showSD : &listData->showNAND;
            *flag = !*flag;
            break;
        }
        case 3:
            listData->group = option == 0;
            break;
        default:
            listData->sort = (tickets_sort) option;
            // Sorted again at the next frame.
            listData->sorted = false;
            break;
    }

    tickets_store(listData);
}

static void tickets_filter_open(ui_view* view, void* data) {
    popover_def def = {
            .groupCount = 5,
            .groups = {
                    {_("Show"), true, 2, {_("In use"), _("Not in use")}, {-1, -1}},
                    // In title_kind's order.
                    {_("Type"), true, 6, {_("Games"), _("Updates"), "DLC", _("Demos"), "DSiWare", _("System")}, {-1, -1, -1, -1, -1, -1}},
                    {_("Memory"), true, 2, {"SD", "NAND"}, {ICON_DEVICE_SD_CARD, ICON_CPU}},
                    {_("Group updates and DLC"), false, 2, {_("Yes"), _("No")}, {-1, -1}},
                    {_("Sort by"), false, 2, {_("Name"), "ID"}, {-1, -1}}
            },
            .isOn = tickets_popover_is_on,
            .select = tickets_popover_select
    };

    popover_display(&def, data);
}

static bool tickets_search_active(ui_view* view, void* data) {
    return list_get_search(view)[0] != '\0';
}

static void tickets_search_done(ui_view* kbdView, void* data, SwkbdButton button, const char* response) {
    tickets_data* listData = (tickets_data*) data;
    if(button == SWKBD_BUTTON_CONFIRM) {
        list_set_search(listData->view, response);
    }
}

static void tickets_search_open(ui_view* view, void* data) {
    kbd_display(_("Search tickets"), list_get_search(view), SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, 63, data, tickets_search_done);
}

// ---- a game's chain: its ticket, its update and its DLC ----

typedef struct {
    tickets_data* parent;
    linked_list* parentItems;
    list_item* game;
    char name[LIST_ITEM_NAME_MAX];   // The game's item may be deleted while this is open.
} tickets_chain_data;

// Chain rows wrap the main list's items (item->data), which keep owning the tickets.
static list_item* tickets_chain_target(list_item* wrapper) {
    return (list_item*) wrapper->data;
}

static void tickets_chain_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    tickets_draw_row(NULL, tickets_chain_target(item), x, y, width, height, selected);
}

static void tickets_chain_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected != NULL) {
        tickets_draw_details(view, tickets_chain_target(selected), x1, y1, x2, y2);
    }
}

static void tickets_chain_close(ui_view* view, linked_list* items, tickets_chain_data* chain) {
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

static void tickets_chain_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    tickets_chain_data* chain = (tickets_chain_data*) data;

    view->info = _(" actions    back");

    if(hidKeysDown() & KEY_B) {
        tickets_chain_close(view, items, chain);
        return;
    }

    if(linked_list_size(items) == 0) {
        // The game's ticket first, then its update, then its DLC.
        ticket_info* game = (ticket_info*) chain->game->data;
        for(int pass = 0; pass < 3; pass++) {
            linked_list_iter iter;
            linked_list_iterate(chain->parentItems, &iter);
            while(linked_list_iter_has_next(&iter)) {
                list_item* item = (list_item*) linked_list_iter_next(&iter);
                ticket_info* info = (ticket_info*) item->data;
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
        // An action may have deleted tickets from the main list: drop their rows.
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            list_item* wrapper = (list_item*) linked_list_iter_next(&iter);
            if(linked_list_index_of(chain->parentItems, tickets_chain_target(wrapper)) < 0) {
                free(wrapper);
                linked_list_iter_remove(&iter);
            }
        }

        if(linked_list_size(items) == 0) {
            tickets_chain_close(view, items, chain);
            return;
        }

        selected = selected != NULL && linked_list_index_of(items, selected) >= 0 ? selected : NULL;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A)) && ((ticket_info*) tickets_chain_target(selected)->data)->ready) {
        tickets_actions_open(chain->parent, chain->parentItems, tickets_chain_target(selected));
    }
}

static void tickets_chain_open(tickets_data* data, linked_list* items, list_item* game) {
    tickets_chain_data* chain = (tickets_chain_data*) calloc(1, sizeof(tickets_chain_data));
    if(chain == NULL) {
        error_display(NULL, NULL, "Failed to allocate ticket chain.");
        return;
    }

    chain->parent = data;
    chain->parentItems = items;
    chain->game = game;
    string_copy(chain->name, game->name, sizeof(chain->name));

    ui_view* view = list_display(chain->name, _(" actions    back"), chain, tickets_chain_update, tickets_chain_draw_top);
    if(view == NULL) {
        free(chain);
        return;
    }

    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = tickets_chain_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
}

// ---- the list ----

// Named tickets first by name, the rest by ID; or all by ID.
static int tickets_compare(void* data, const void* p1, const void* p2) {
    tickets_data* listData = (tickets_data*) data;

    list_item* item1 = (list_item*) p1;
    list_item* item2 = (list_item*) p2;
    ticket_info* ticket1 = (ticket_info*) item1->data;
    ticket_info* ticket2 = (ticket_info*) item2->data;

    // While loading: ready tickets first, the rest by ID until they are.
    if(ticket1->ready != ticket2->ready) {
        return ticket1->ready ? -1 : 1;
    } else if(!ticket1->ready) {
        return ticket1->titleId > ticket2->titleId ? 1 : ticket1->titleId < ticket2->titleId ? -1 : 0;
    }

    if(listData->sort == SORT_NAME) {
        if(ticket1->hasMeta != ticket2->hasMeta) {
            return ticket1->hasMeta ? -1 : 1;
        }

        if(ticket1->hasMeta) {
            int byName = strncasecmp(item1->name, item2->name, sizeof(item1->name));
            if(byName != 0) {
                return byName;
            }
        }
    }

    return ticket1->titleId > ticket2->titleId ? 1 : ticket1->titleId < ticket2->titleId ? -1 : 0;
}

static void tickets_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    tickets_data* listData = (tickets_data*) data;

    view->name = _("Tickets");
    listData->items = items;
    tickets_group(listData, items);

    // How many have their title found, for the title bar.
    u32 ready = 0;
    u32 total = 0;
    if(!listData->populateData.finished) {
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            total++;
            ready += ((ticket_info*) ((list_item*) linked_list_iter_next(&iter))->data)->ready ? 1 : 0;
        }
    }

    list_set_progress(view, ready, total);
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

        task_clear_tickets(items);
        list_destroy(view);

        free(listData);
        return;
    }

    if(hidKeysDown() & KEY_Y) {
        list_set_mode(view, list_get_mode(view) == LIST_MODE_GRID ? LIST_MODE_ROWS : LIST_MODE_GRID);
        tickets_store(listData);
    }

    if(hidKeysDown() & KEY_L) {
        tickets_filter_open(view, listData);
        return;
    }

    if(hidKeysDown() & KEY_R) {
        tickets_search_open(view, listData);
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
        Result res = task_populate_tickets(&listData->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate ticket list population.");
        }

        listData->populated = true;
    }

    // Sorted as tickets load: ready ones in order at the top, the rest after;
    // the selected ticket keeps its place on screen meanwhile.
    if(!listData->populateData.finished && ready != listData->sortedReady) {
        linked_list_sort(items, listData, tickets_compare);
        listData->sortedReady = ready;
    } else if(listData->populateData.finished && !listData->sorted) {
        linked_list_sort(items, listData, tickets_compare);
        listData->sorted = true;
    }

    if(listData->populateData.finished && R_FAILED(listData->populateData.result)) {
        error_display_res(NULL, NULL, listData->populateData.result, "Failed to populate ticket list.");

        listData->populateData.result = 0;
    }

    // A ticket whose title is still being found has nothing to act on yet.
    if(selected != NULL && selected->data != NULL && selectedTouched && ((ticket_info*) selected->data)->ready) {
        // Grouped, a game with an update or DLC ticket opens its chain to pick one.
        if(listData->group && ((ticket_info*) selected->data)->linkedCount > 0) {
            tickets_chain_open(listData, items, selected);
        } else {
            tickets_actions_open(listData, items, selected);
        }

        return;
    }
}

// A long hold: the ticket's own actions, even for a game with its chain.
static void tickets_long_press(ui_view* view, void* data, linked_list* items, list_item* item) {
    if(item->data != NULL && ((ticket_info*) item->data)->ready) {
        tickets_actions_open((tickets_data*) data, items, item);
    }
}

void tickets_open() {
    tickets_data* data = (tickets_data*) calloc(1, sizeof(tickets_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate tickets data.");

        return;
    }

    data->populateData.finished = true;
    data->populateData.withTitles = true;
    tickets_restore(data);

    ui_view* view = list_display(_("Tickets"), "", data, tickets_update, tickets_draw_top);
    if(view == NULL) {
        free(data);
        return;
    }

    data->view = view;
    if(settings_get()->tickets.grid) {
        list_set_mode(view, LIST_MODE_GRID);
    }

    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = tickets_draw_row,
            .tileSize = TILE_SIZE,
            .drawTile = tickets_draw_tile
    };
    list_set_style(view, &style);
    list_set_filter(view, tickets_visible);
    list_set_long_press(view, tickets_long_press);

    // Right to left.
    list_add_tool(view, ICON_SEARCH, tickets_search_active, tickets_search_open);
    list_add_tool(view, ICON_FILTER, tickets_filter_active, tickets_filter_open);
    list_add_tool(view, ICON_LAYOUT_GRID, tickets_is_grid, tickets_show_grid);
    list_add_tool(view, ICON_LIST, tickets_is_rows, tickets_show_rows);
}
