#pragma once

#define LIST_ITEM_NAME_MAX 512

typedef struct linked_list_s linked_list;
typedef struct ui_view_s ui_view;

typedef struct list_item_s {
    char name[LIST_ITEM_NAME_MAX];
    u32 color;
    void* data;
} list_item;

// The list every collection screen is built on: rows or a grid of tiles,
// D-Pad and touch (one tap opens: the update callback gets selectedTouched),
// optional tool icons in the title bar and a search filter on item names.
ui_view* list_display(const char* name, const char* info, void* data, void (*update)(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched),
                                                                      void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected));
void list_destroy(ui_view* view);

typedef enum {
    LIST_MODE_ROWS,
    LIST_MODE_GRID
} list_mode;

// How items are drawn; NULL callbacks keep the default row (name in its colour).
// A grid needs drawTile.
typedef struct {
    float rowHeight;
    void (*drawRow)(void* data, list_item* item, float x, float y, float width, float height, bool selected);
    float tileSize;
    void (*drawTile)(void* data, list_item* item, float x, float y, float size, bool selected);
} list_style;

void list_set_style(ui_view* view, const list_style* style);
void list_set_mode(ui_view* view, list_mode mode);
list_mode list_get_mode(ui_view* view);

// A tool icon in the title bar, right to left in the order added. `active`
// (may be NULL) highlights it; `press` runs on a tap.
void list_add_tool(ui_view* view, u32 icon, bool (*active)(ui_view* view, void* data), void (*press)(ui_view* view, void* data));

// Hides items for which `visible` returns false (NULL shows all), on top of the search.
void list_set_filter(ui_view* view, bool (*visible)(void* data, list_item* item));

// Shows only items whose name contains `text` (case-insensitive); "" for all.
void list_set_search(ui_view* view, const char* text);
const char* list_get_search(ui_view* view);
// Selects an item and scrolls it into view.
void list_select(ui_view* view, list_item* item);
// Items shown after the search filter.
u32 list_visible_count(ui_view* view);

// A long hold on an item (touch, or A) runs `longPress` with it, after a fill
// that grows from where it was pressed. With it set, A acts on release: the
// update callback gets selectedTouched for a short A as for a tap, and should
// not read KEY_A itself.
void list_set_long_press(ui_view* view, void (*longPress)(ui_view* view, void* data, linked_list* items, list_item* item));
