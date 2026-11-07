#pragma once

// A game's chain: the list a grouped game opens, with the game (or its ticket) first, then its update, then its DLC.
// Its rows wrap the items of the screen's own list, which keep owning them; rows whose item that list deletes go, and
// the chain closes when none is left. Titles and Tickets open one (chain.c).

typedef struct linked_list_s linked_list;
typedef struct list_item_s list_item;
typedef struct ui_view_s ui_view;

typedef struct {
    float rowHeight;
    u64 (*titleId)(list_item* item);   // Of an item of the screen's list.
    void (*drawRow)(list_item* item, float x, float y, float width, float height, bool selected);
    // The top screen for `item` (NULL while nothing is selected).
    void (*drawTop)(ui_view* view, void* parent, linked_list* parentItems, list_item* item, float x1, float y1, float x2, float y2);
    bool (*ready)(list_item* item);    // Whether A may act on it yet; NULL: always.
    void (*actions)(void* parent, linked_list* parentItems, list_item* item);   // A on a row.
    const char* allocError;
} chain_def;

// `def` must outlive the chain (a static const). `parent` is the screen's data, given back to the callbacks.
void chain_open(const chain_def* def, void* parent, linked_list* parentItems, list_item* game);
