#define _GNU_SOURCE
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "error.h"
#include "list.h"
#include "ui.h"
#include "../i18n.h"
#include "../icons.h"
#include "../linkedlist.h"
#include "../screen.h"
#include "../theme.h"

#define LIST_PAD 6
#define LIST_TOOLS_MAX 6
#define LIST_TOOL_SIZE 22
#define LIST_GRID_GAP 8
#define LIST_SEARCH_MAX 64

typedef struct {
    u32 icon;
    bool (*active)(ui_view* view, void* data);
    void (*press)(ui_view* view, void* data);
} list_tool;

typedef struct {
    void* data;
    linked_list items;

    // Items that pass the search; rebuilt every frame.
    list_item** visible;
    u32 visibleCount;
    u32 visibleCapacity;

    u32 selectedIndex;       // Into `visible`.
    list_item* selectedItem;
    float scrollPos;
    u64 nextActionTime;

    bool touching;
    bool dragged;
    int touchIndex;
    u16 touchStartY;
    u16 lastTouchY;

    list_mode mode;
    list_style style;
    list_tool tools[LIST_TOOLS_MAX];
    u32 toolCount;
    char search[LIST_SEARCH_MAX];

    void (*update)(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched);
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected);
} list_data;

// ---- layout ----

typedef struct {
    float x1, y1, x2, y2;   // The area items scroll in.
    u32 columns;
    float pitchX, pitchY;   // Distance between items.
    float itemW, itemH;
} list_layout;

static bool list_grid(list_data* listData) {
    return listData->mode == LIST_MODE_GRID && listData->style.drawTile != NULL;
}

static list_layout list_get_layout(list_data* listData, float x1, float y1, float x2, float y2) {
    list_layout l;
    l.x1 = x1 + 8;
    l.x2 = x2 - 8 - WIDGET_SCROLLBAR_WIDTH - 4;
    l.y1 = y1 + LIST_PAD;
    l.y2 = y2 - LIST_PAD;

    if(list_grid(listData)) {
        float size = listData->style.tileSize;
        l.columns = (u32) ((l.x2 - l.x1 + LIST_GRID_GAP) / (size + LIST_GRID_GAP));
        if(l.columns < 1) {
            l.columns = 1;
        }

        // Spread the spare width between the columns.
        float gap = l.columns > 1 ? (l.x2 - l.x1 - l.columns * size) / (l.columns - 1) : 0;
        l.itemW = l.itemH = size;
        l.pitchX = size + gap;
        l.pitchY = size + LIST_GRID_GAP;
    } else {
        l.columns = 1;
        l.itemW = l.x2 - l.x1;
        l.itemH = listData->style.rowHeight > 0 ? listData->style.rowHeight : WIDGET_ROW_HEIGHT;
        l.pitchX = 0;
        l.pitchY = l.itemH + WIDGET_ROW_GAP;
    }

    return l;
}

static float list_total_height(list_data* listData, list_layout* l) {
    u32 rows = (listData->visibleCount + l->columns - 1) / l->columns;
    return rows > 0 ? rows * l->pitchY - (l->pitchY - l->itemH) : 0;
}

static void list_item_position(list_data* listData, list_layout* l, u32 index, float* x, float* y) {
    *x = l->x1 + (index % l->columns) * l->pitchX;
    *y = l->y1 + (index / l->columns) * l->pitchY - listData->scrollPos;
}

// ---- search and selection ----

static void list_rebuild_visible(list_data* listData) {
    u32 size = linked_list_size(&listData->items);
    if(size > listData->visibleCapacity) {
        list_item** grown = (list_item**) realloc(listData->visible, size * sizeof(list_item*));
        if(grown == NULL) {
            return;
        }

        listData->visible = grown;
        listData->visibleCapacity = size;
    }

    listData->visibleCount = 0;

    linked_list_iter iter;
    linked_list_iterate(&listData->items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        if(listData->search[0] == '\0' || strcasestr(item->name, listData->search) != NULL) {
            listData->visible[listData->visibleCount++] = item;
        }
    }
}

static void list_clamp_scroll(list_data* listData, list_layout* l) {
    float max = list_total_height(listData, l) - (l->y2 - l->y1);
    if(listData->scrollPos > max) {
        listData->scrollPos = max;
    }

    if(listData->scrollPos < 0) {
        listData->scrollPos = 0;
    }
}

// Keeps the selection on the same item when the list changes, and in view.
static void list_validate(list_data* listData, list_layout* l, bool reveal) {
    list_rebuild_visible(listData);

    if(listData->visibleCount == 0) {
        listData->selectedIndex = 0;
        listData->selectedItem = NULL;
        listData->scrollPos = 0;
        return;
    }

    bool found = false;
    if(listData->selectedItem != NULL) {
        for(u32 i = 0; i < listData->visibleCount; i++) {
            if(listData->visible[i] == listData->selectedItem) {
                listData->selectedIndex = i;
                found = true;
                break;
            }
        }
    }

    if(!found) {
        if(listData->selectedIndex >= listData->visibleCount) {
            listData->selectedIndex = listData->visibleCount - 1;
        }

        listData->selectedItem = listData->visible[listData->selectedIndex];
    }

    if(reveal) {
        float top = (listData->selectedIndex / l->columns) * l->pitchY;
        float view = l->y2 - l->y1;
        if(top < listData->scrollPos) {
            listData->scrollPos = top;
        } else if(top + l->itemH > listData->scrollPos + view) {
            listData->scrollPos = top + l->itemH - view;
        }
    }

    list_clamp_scroll(listData, l);
}

static bool list_repeat(list_data* listData, u32 key) {
    if(hidKeysDown() & key) {
        listData->nextActionTime = osGetTime() + 400;
        return true;
    }

    if((hidKeysHeld() & key) && osGetTime() >= listData->nextActionTime) {
        listData->nextActionTime = osGetTime() + 90;
        return true;
    }

    return false;
}

// ---- title bar ----

static float list_tool_x(u32 index) {
    return BOTTOM_SCREEN_WIDTH - 6 - (index + 1) * (LIST_TOOL_SIZE + 3) + 3;
}

static void list_draw_title_bar(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    list_data* listData = (list_data*) data;
    const theme_current* t = theme();

    float cy = y1 + (y2 - y1 - LIST_TOOL_SIZE) / 2;
    screen_draw_icon(ICON_ARROW_LEFT, x1 + 6, cy + 3, 16, t->palette->text);

    float textY = y1 + (y2 - y1 - widget_text_height(WIDGET_TEXT_SCALE)) / 2;
    float titleX = x1 + 28;
    float right = listData->toolCount > 0 ? list_tool_x(listData->toolCount - 1) - 6 : x2 - 6;

    char count[16];
    snprintf(count, sizeof(count), "%lu", listData->visibleCount);
    float countWidth = widget_text_width(count, WIDGET_SMALL_SCALE);
    float titleRoom = right - titleX - countWidth - 6;
    widget_text_fit(view->name, titleX, textY, WIDGET_TEXT_SCALE, t->palette->text, titleRoom);
    float titleWidth = widget_text_width(view->name, WIDGET_TEXT_SCALE);
    float countX = titleX + (titleWidth < titleRoom ? titleWidth : titleRoom) + 6;
    screen_draw_text(count, countX, y1 + (y2 - y1 - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, t->palette->textMuted);

    for(u32 i = 0; i < listData->toolCount; i++) {
        list_tool* tool = &listData->tools[i];
        float tx = list_tool_x(i);
        bool active = tool->active != NULL && tool->active(view, listData->data);
        if(active) {
            screen_fill_round_rect(tx, cy, LIST_TOOL_SIZE, LIST_TOOL_SIZE, 6, t->accentSoft);
        }

        screen_draw_icon(tool->icon, tx + 3, cy + 3, 16, active ? t->accent : t->palette->textMuted);
    }
}

// A tap on the title bar: back arrow (as B) or a tool. True when it was there.
static bool list_title_bar_touch(ui_view* view, list_data* listData, touchPosition* pos) {
    if(pos->py >= WIDGET_TITLE_BAR_HEIGHT) {
        return false;
    }

    if(view->drawTitleBar == NULL) {
        return true;
    }

    if(pos->px < 26) {
        input_inject_keys(KEY_B, 1);
        return true;
    }

    for(u32 i = 0; i < listData->toolCount; i++) {
        float tx = list_tool_x(i);
        if(pos->px >= tx - 1 && pos->px < tx + LIST_TOOL_SIZE + 1) {
            if(listData->tools[i].press != NULL) {
                listData->tools[i].press(view, listData->data);
            }

            return true;
        }
    }

    return true;
}

// ---- update and draw ----

static void list_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    list_data* listData = (list_data*) data;
    list_layout l = list_get_layout(listData, bx1, by1, bx2, by2);

    list_validate(listData, &l, false);

    bool selectedTouched = false;
    u32 last = listData->selectedIndex;
    u32 count = listData->visibleCount;

    if(count > 0) {
        u32 cols = l.columns;
        u32 page = (u32) ((l.y2 - l.y1) / l.pitchY);
        if(page < 1) {
            page = 1;
        }

        if(list_repeat(listData, KEY_DOWN)) {
            if(listData->selectedIndex + cols < count) {
                listData->selectedIndex += cols;
            } else if(listData->selectedIndex / cols < (count - 1) / cols) {
                listData->selectedIndex = count - 1;
            } else {
                listData->selectedIndex %= cols;
            }
        } else if(list_repeat(listData, KEY_UP)) {
            if(listData->selectedIndex >= cols) {
                listData->selectedIndex -= cols;
            } else {
                u32 lastRow = ((count - 1) / cols) * cols;
                listData->selectedIndex = lastRow + listData->selectedIndex < count ? lastRow + listData->selectedIndex : count - 1;
            }
        } else if(list_repeat(listData, KEY_RIGHT)) {
            if(cols > 1) {
                listData->selectedIndex = (listData->selectedIndex + 1) % count;
            } else {
                // Rows: Left/Right jump a page.
                listData->selectedIndex = listData->selectedIndex + page < count ? listData->selectedIndex + page : count - 1;
            }
        } else if(list_repeat(listData, KEY_LEFT)) {
            if(cols > 1) {
                listData->selectedIndex = (listData->selectedIndex + count - 1) % count;
            } else {
                listData->selectedIndex = listData->selectedIndex > page ? listData->selectedIndex - page : 0;
            }
        }

        if(listData->selectedIndex != last) {
            listData->selectedItem = listData->visible[listData->selectedIndex];
            list_validate(listData, &l, true);
        }
    }

    // Touch: a tap on an item opens it, a drag scrolls, the title bar has its own targets.
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        listData->touching = false;
        if(!list_title_bar_touch(view, listData, &pos)) {
            listData->touching = true;
            listData->dragged = false;
            listData->touchStartY = listData->lastTouchY = pos.py;
            listData->touchIndex = -1;
            for(u32 i = 0; i < count && pos.py >= l.y1 && pos.py < l.y2; i++) {
                float x, y;
                list_item_position(listData, &l, i, &x, &y);
                if(widget_touched(&pos, x, y, l.itemW, l.itemH)) {
                    listData->touchIndex = (int) i;
                    break;
                }
            }
        }
    } else if(listData->touching && (hidKeysHeld() & KEY_TOUCH)) {
        if(abs((int) pos.py - (int) listData->touchStartY) > 6) {
            listData->dragged = true;
        }

        if(listData->dragged) {
            listData->scrollPos -= (int) pos.py - (int) listData->lastTouchY;
            list_clamp_scroll(listData, &l);
        }

        listData->lastTouchY = pos.py;
    } else if(listData->touching && (hidKeysUp() & KEY_TOUCH)) {
        listData->touching = false;
        if(!listData->dragged && listData->touchIndex >= 0 && (u32) listData->touchIndex < count) {
            listData->selectedIndex = (u32) listData->touchIndex;
            listData->selectedItem = listData->visible[listData->selectedIndex];
            selectedTouched = true;
        }
    }

    if(listData->update != NULL) {
        listData->update(view, listData->data, &listData->items, listData->selectedItem, selectedTouched);
    }
}

static void list_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    list_data* listData = (list_data*) data;

    if(listData->drawTop != NULL) {
        listData->drawTop(view, listData->data, x1, y1, x2, y2, listData->selectedItem);
    }
}

static void list_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    list_data* listData = (list_data*) data;
    const theme_current* t = theme();
    list_layout l = list_get_layout(listData, x1, y1, x2, y2);

    list_validate(listData, &l, false);

    if(listData->visibleCount == 0) {
        if(listData->search[0] != '\0') {
            widget_text_centered(_("Nothing matches the search."), x1, x2, y1 + (y2 - y1) / 2 - 8, WIDGET_TEXT_SCALE, t->palette->textMuted);
        }

        return;
    }

    screen_set_clip(x1, y1 + LIST_PAD / 2, x2 - x1, y2 - y1 - LIST_PAD);

    // Only the rows on screen.
    u32 firstRow = (u32) (listData->scrollPos / l.pitchY);
    for(u32 i = firstRow * l.columns; i < listData->visibleCount; i++) {
        float x, y;
        list_item_position(listData, &l, i, &x, &y);
        if(y > y2) {
            break;
        }

        list_item* item = listData->visible[i];
        bool selected = item == listData->selectedItem;
        if(list_grid(listData)) {
            listData->style.drawTile(listData->data, item, x, y, l.itemW, selected);
        } else if(listData->style.drawRow != NULL) {
            listData->style.drawRow(listData->data, item, x, y, l.itemW, l.itemH, selected);
        } else {
            widget_row(x, y, l.itemW, l.itemH, -1, item->name, screen_get_color(item->color), NULL, selected);
        }
    }

    screen_clear_clip();

    widget_scrollbar(x2 - 8 - WIDGET_SCROLLBAR_WIDTH, l.y1, l.y2 - l.y1, listData->scrollPos, l.y2 - l.y1, list_total_height(listData, &l));
}

ui_view* list_display(const char* name, const char* info, void* data, void (*update)(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched),
                                                                      void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected)) {
    list_data* listData = (list_data*) calloc(1, sizeof(list_data));
    if(listData == NULL) {
        error_display(NULL, NULL, "Failed to allocate list data.");

        return NULL;
    }

    listData->data = data;
    linked_list_init(&listData->items);
    listData->mode = LIST_MODE_ROWS;
    listData->update = update;
    listData->drawTop = drawTop;

    ui_view* view = ui_create();
    view->name = name;
    view->info = info;
    view->data = listData;
    view->update = list_update;
    view->drawTop = list_draw_top;
    view->drawBottom = list_draw_bottom;
    ui_push(view);

    return view;
}

void list_destroy(ui_view* view) {
    if(view != NULL) {
        list_data* listData = (list_data*) view->data;
        linked_list_destroy(&listData->items);
        free(listData->visible);

        free(listData);
        ui_destroy(view);
    }
}

void list_set_style(ui_view* view, const list_style* style) {
    ((list_data*) view->data)->style = *style;
}

void list_set_mode(ui_view* view, list_mode mode) {
    list_data* listData = (list_data*) view->data;
    listData->mode = mode;
    listData->scrollPos = 0;
}

list_mode list_get_mode(ui_view* view) {
    return ((list_data*) view->data)->mode;
}

void list_add_tool(ui_view* view, u32 icon, bool (*active)(ui_view* view, void* data), void (*press)(ui_view* view, void* data)) {
    list_data* listData = (list_data*) view->data;
    if(listData->toolCount < LIST_TOOLS_MAX) {
        listData->tools[listData->toolCount++] = (list_tool) {icon, active, press};
    }

    view->drawTitleBar = list_draw_title_bar;
}

void list_set_search(ui_view* view, const char* text) {
    list_data* listData = (list_data*) view->data;
    snprintf(listData->search, sizeof(listData->search), "%s", text != NULL ? text : "");
    listData->scrollPos = 0;
}

const char* list_get_search(ui_view* view) {
    return ((list_data*) view->data)->search;
}

u32 list_visible_count(ui_view* view) {
    return ((list_data*) view->data)->visibleCount;
}
