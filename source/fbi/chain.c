#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "chain.h"
#include "resources.h"
#include "task/uitask.h"
#include "../core/core.h"

typedef struct {
    const chain_def* def;
    void* parent;
    linked_list* parentItems;
    list_item* game;
    char name[LIST_ITEM_NAME_MAX];   // The game's item may be deleted while this is open.
} chain_data;

// Chain rows wrap the main list's items (item->data).
static list_item* chain_target(list_item* wrapper) {
    return (list_item*) wrapper->data;
}

static void chain_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    ((chain_data*) data)->def->drawRow(chain_target(item), x, y, width, height, selected);
}

static void chain_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    chain_data* chain = (chain_data*) data;
    chain->def->drawTop(view, chain->parent, chain->parentItems, selected != NULL ? chain_target(selected) : NULL, x1, y1, x2, y2);
}

static void chain_close(ui_view* view, linked_list* items, chain_data* chain) {
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

static void chain_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    chain_data* chain = (chain_data*) data;
    const chain_def* def = chain->def;

    view->info = _(" actions    back");

    if(hidKeysDown() & KEY_B) {
        chain_close(view, items, chain);
        return;
    }

    if(linked_list_size(items) == 0) {
        // The game first, then its update, then its DLC.
        u64 gameId = def->titleId(chain->game);
        for(int pass = 0; pass < 3; pass++) {
            linked_list_iter iter;
            linked_list_iterate(chain->parentItems, &iter);
            while(linked_list_iter_has_next(&iter)) {
                list_item* item = (list_item*) linked_list_iter_next(&iter);
                u64 titleId = def->titleId(item);
                title_kind kind = title_kind_of(titleId);
                bool wanted = pass == 0 ? item == chain->game : pass == 1 ? kind == TITLE_KIND_UPDATE : kind == TITLE_KIND_DLC;
                if(!wanted || (u32) titleId != (u32) gameId) {
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
        // An action may have deleted items from the main list: drop their rows.
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            list_item* wrapper = (list_item*) linked_list_iter_next(&iter);
            if(linked_list_index_of(chain->parentItems, chain_target(wrapper)) < 0) {
                free(wrapper);
                linked_list_iter_remove(&iter);
            }
        }

        if(linked_list_size(items) == 0) {
            chain_close(view, items, chain);
            return;
        }

        selected = selected != NULL && linked_list_index_of(items, selected) >= 0 ? selected : NULL;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A)) && (def->ready == NULL || def->ready(chain_target(selected)))) {
        def->actions(chain->parent, chain->parentItems, chain_target(selected));
    }
}

void chain_open(const chain_def* def, void* parent, linked_list* parentItems, list_item* game) {
    chain_data* chain = (chain_data*) calloc(1, sizeof(chain_data));
    if(chain == NULL) {
        error_display(NULL, NULL, def->allocError);
        return;
    }

    chain->def = def;
    chain->parent = parent;
    chain->parentItems = parentItems;
    chain->game = game;
    string_copy(chain->name, game->name, sizeof(chain->name));

    ui_view* view = list_display(chain->name, _(" actions    back"), chain, chain_update, chain_draw_top);
    if(view == NULL) {
        free(chain);
        return;
    }

    list_style style = {
            .rowHeight = def->rowHeight,
            .drawRow = chain_draw_row
    };
    list_set_style(view, &style);
    // The back arrow (and the count) of the other lists.
    list_show_title_bar(view);
}
