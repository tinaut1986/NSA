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
    float scrollTarget;      // Where a D-Pad move glides to.
    bool scrollGliding;
    float scrollVelocity;    // Left by a flick, slowing down.

    // The top screen's card slides in when the selection changes.
    list_item* shownItem;
    list_item* leavingItem;
    u64 shownTime;
    u64 nextActionTime;

    bool touching;
    bool dragged;
    u16 touchStartY;
    u16 lastTouchY;

    // The item held (by touch, or by A when there is a long press), and the
    // title bar target held: -1 back, else a tool's index.
    widget_press press;
    list_item* pressItem;       // What is held: its index changes if the list does.
    list_item* pressPrevious;
    bool revealPending;   // Scroll the selection into view on the next update.   // Selected before the touch, back when it turns into a drag.
    bool barTouching;
    int barTarget;
    void (*longPress)(ui_view* view, void* data, linked_list* items, list_item* item);

    list_mode mode;
    list_style style;
    list_tool tools[LIST_TOOLS_MAX];
    u32 toolCount;
    char search[LIST_SEARCH_MAX];
    // While items load: how many are ready of how many (0 total: the count).
    u32 progressDone;
    u32 progressTotal;
    bool (*visibleFilter)(void* data, list_item* item);

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

    // A populate task may add items while this runs: never past the room
    // counted above (the rest show on the next frame).
    linked_list_iter iter;
    linked_list_iterate(&listData->items, &iter);
    while(listData->visibleCount < listData->visibleCapacity && linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        if((listData->search[0] == '\0' || strcasestr(item->name, listData->search) != NULL)
           && (listData->visibleFilter == NULL || listData->visibleFilter(listData->data, item))) {
            listData->visible[listData->visibleCount++] = item;
        }
    }
}

static void list_clamp_scroll(list_data* listData, list_layout* l) {
    float max = list_total_height(listData, l) - (l->y2 - l->y1);
    if(max < 0) {
        max = 0;
    }

    if(listData->scrollPos > max) {
        listData->scrollPos = max;
        listData->scrollVelocity = 0;
    }

    if(listData->scrollPos < 0) {
        listData->scrollPos = 0;
        listData->scrollVelocity = 0;
    }

    if(listData->scrollTarget > max) {
        listData->scrollTarget = max;
    }

    if(listData->scrollTarget < 0) {
        listData->scrollTarget = 0;
    }
}

// Glides towards the D-Pad's target, or coasts after a flick.
static void list_animate_scroll(list_data* listData, list_layout* l) {
    if(listData->touching && listData->dragged) {
        return;
    }

    if(listData->scrollVelocity != 0) {
        listData->scrollPos -= listData->scrollVelocity;
        listData->scrollVelocity *= 0.92f;
        if(listData->scrollVelocity > -0.3f && listData->scrollVelocity < 0.3f) {
            listData->scrollVelocity = 0;
        }
    } else if(listData->scrollGliding) {
        float diff = listData->scrollTarget - listData->scrollPos;
        if(diff > -0.5f && diff < 0.5f) {
            listData->scrollPos = listData->scrollTarget;
            listData->scrollGliding = false;
        } else {
            listData->scrollPos += diff * 0.3f;
        }
    }

    list_clamp_scroll(listData, l);
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
                // Items came or went above it (loading, sorting, deleting):
                // scroll with it so it stays where it was on screen.
                if(i != listData->selectedIndex && !reveal) {
                    float moved = ((float) (i / l->columns) - (float) (listData->selectedIndex / l->columns)) * l->pitchY;
                    listData->scrollPos += moved;
                    listData->scrollTarget += moved;
                }

                listData->selectedIndex = i;
                found = true;
                break;
            }
        }
    }

    // The item held follows the same way.
    if(listData->press.index >= 0 && listData->pressItem != NULL) {
        int index = -1;
        for(u32 i = 0; i < listData->visibleCount; i++) {
            if(listData->visible[i] == listData->pressItem) {
                index = (int) i;
                break;
            }
        }

        if(index < 0) {
            listData->press.cancelled = true;
        } else {
            listData->press.index = index;
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
        float from = listData->scrollGliding ? listData->scrollTarget : listData->scrollPos;
        float to = from;
        if(top < from) {
            to = top;
        } else if(top + l->itemH > from + view) {
            to = top + l->itemH - view;
        }

        if(to != from) {
            listData->scrollTarget = to;
            listData->scrollGliding = true;
            listData->scrollVelocity = 0;
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
    if(listData->barTouching && listData->barTarget == -1) {
        screen_fill_round_rect(x1 + 3, cy, LIST_TOOL_SIZE, LIST_TOOL_SIZE, 6, THEME_ALPHA(t->palette->text, 0x22));
    }

    screen_draw_icon(ICON_ARROW_LEFT, x1 + 6, cy + 3, 16, t->palette->text);

    float textY = y1 + (y2 - y1 - widget_text_height(WIDGET_TEXT_SCALE)) / 2;
    float titleX = x1 + 28;
    float right = listData->toolCount > 0 ? list_tool_x(listData->toolCount - 1) - 6 : x2 - 6;

    char count[24];
    if(listData->progressTotal > 0 && listData->progressDone < listData->progressTotal) {
        snprintf(count, sizeof(count), "%lu/%lu", listData->progressDone, listData->progressTotal);
    } else {
        snprintf(count, sizeof(count), "%lu", listData->visibleCount);
    }
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

        if(listData->barTouching && listData->barTarget == (int) i) {
            screen_fill_round_rect(tx, cy, LIST_TOOL_SIZE, LIST_TOOL_SIZE, 6, THEME_ALPHA(t->palette->text, 0x22));
        }

        screen_draw_icon(tool->icon, tx + 3, cy + 3, 16, active ? t->accent : t->palette->textMuted);
    }
}

// The title bar target under a touch: -1 back, a tool's index, or -2 none.
static int list_title_bar_target(list_data* listData, touchPosition* pos) {
    if(pos->py >= WIDGET_TITLE_BAR_HEIGHT) {
        return -2;
    }

    if(pos->px < 28) {
        return -1;
    }

    for(u32 i = 0; i < listData->toolCount; i++) {
        float tx = list_tool_x(i);
        if(pos->px >= tx - 1 && pos->px < tx + LIST_TOOL_SIZE + 1) {
            return (int) i;
        }
    }

    return -2;
}

// Runs a title bar target when the finger is lifted on it.
static void list_title_bar_release(ui_view* view, list_data* listData) {
    if(listData->barTarget == -1) {
        input_inject_keys(KEY_B, 1);
    } else if(listData->barTarget >= 0 && listData->tools[listData->barTarget].press != NULL) {
        listData->tools[listData->barTarget].press(view, listData->data);
    }
}

// ---- update and draw ----

static void list_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    list_data* listData = (list_data*) data;
    list_layout l = list_get_layout(listData, bx1, by1, bx2, by2);

    list_validate(listData, &l, listData->revealPending);
    listData->revealPending = false;
    list_animate_scroll(listData, &l);

    bool selectedTouched = false;
    u32 last = listData->selectedIndex;
    u32 count = listData->visibleCount;

    if(count > 0) {
        u32 cols = l.columns;
        // Rows turn a page with Left and Right: the first row that was not
        // wholly shown (or the one a page above) becomes the top one, and is selected.
        float from = listData->scrollGliding ? listData->scrollTarget : listData->scrollPos;
        u32 firstShown = (u32) ((from + l.pitchY - 1) / l.pitchY);
        u32 whole = (u32) ((l.y2 - l.y1 - l.itemH) / l.pitchY) + 1;
        bool paged = false;

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
                listData->selectedIndex = firstShown + whole < count ? firstShown + whole : count - 1;
                paged = true;
            }
        } else if(list_repeat(listData, KEY_LEFT)) {
            if(cols > 1) {
                listData->selectedIndex = (listData->selectedIndex + count - 1) % count;
            } else {
                listData->selectedIndex = firstShown > whole ? firstShown - whole : 0;
                paged = true;
            }
        }

        if(listData->selectedIndex != last) {
            listData->selectedItem = listData->visible[listData->selectedIndex];
            list_validate(listData, &l, true);
        }

        if(paged) {
            listData->scrollTarget = listData->selectedIndex * l.pitchY;
            listData->scrollGliding = true;
            listData->scrollVelocity = 0;
            list_clamp_scroll(listData, &l);
        }
    }

    // A on an item, when it has a long press: acts on release, a long hold
    // runs the long press. D-Pad moves cancel it.
    list_item* longPressed = NULL;
    if(listData->longPress != NULL && count > 0) {
        if(hidKeysDown() & KEY_A) {
            float x, y;
            list_item_position(listData, &l, listData->selectedIndex, &x, &y);
            widget_press_begin(&listData->press, (int) listData->selectedIndex, false, x + l.itemW / 2, y + l.itemH / 2);
            listData->pressItem = listData->selectedItem;
        } else if(listData->press.index >= 0 && !listData->press.touch) {
            if(listData->selectedIndex != last) {
                listData->press.cancelled = true;
            }

            if(hidKeysUp() & KEY_A) {
                if(widget_press_on(&listData->press, (int) listData->selectedIndex)) {
                    selectedTouched = true;
                }

                widget_press_end(&listData->press);
            } else if(widget_press_long_due(&listData->press)) {
                longPressed = listData->selectedItem;
                widget_press_end(&listData->press);
            }
        }
    }

    // Touch: an item looks pressed while held and opens on release over it; a
    // drag scrolls; a long hold runs the long press. The title bar's targets
    // act on release too.
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        listData->touching = false;
        listData->barTouching = false;
        int target = list_title_bar_target(listData, &pos);
        if(pos.py < WIDGET_TITLE_BAR_HEIGHT) {
            listData->barTouching = view->drawTitleBar != NULL && target != -2;
            listData->barTarget = target;
        } else {
            listData->touching = true;
            listData->dragged = false;
            listData->touchStartY = listData->lastTouchY = pos.py;
            listData->scrollVelocity = 0;
            listData->scrollGliding = false;
            widget_press_end(&listData->press);
            for(u32 i = 0; i < count && pos.py >= l.y1 && pos.py < l.y2; i++) {
                float x, y;
                list_item_position(listData, &l, i, &x, &y);
                if(widget_touched(&pos, x, y, l.itemW, l.itemH)) {
                    widget_press_begin(&listData->press, (int) i, true, pos.px, pos.py);
                    listData->pressItem = listData->visible[i];
                    listData->pressPrevious = listData->selectedItem;
                    listData->selectedIndex = i;
                    listData->selectedItem = listData->visible[i];
                    break;
                }
            }
        }
    } else if(listData->barTouching && (hidKeysHeld() & KEY_TOUCH)) {
        if(list_title_bar_target(listData, &pos) != listData->barTarget) {
            listData->barTarget = -2;
        }
    } else if(listData->barTouching && (hidKeysUp() & KEY_TOUCH)) {
        listData->barTouching = false;
        list_title_bar_release(view, listData);
    } else if(listData->touching && (hidKeysHeld() & KEY_TOUCH)) {
        if(!listData->dragged && abs((int) pos.py - (int) listData->touchStartY) > 6) {
            listData->dragged = true;
            // The touch was not a selection after all: the item selected before comes back,
            // with its index. (The index of the item touched was left, and list_validate took
            // the difference for items that had come or gone above it and scrolled the
            // list by it, which put the view back where the selection was: a drag
            // could never get anywhere.)
            if(listData->press.index >= 0 && listData->pressPrevious != NULL) {
                listData->selectedItem = listData->pressPrevious;
                for(u32 i = 0; i < listData->visibleCount; i++) {
                    if(listData->visible[i] == listData->pressPrevious) {
                        listData->selectedIndex = i;
                        break;
                    }
                }
            }

            listData->press.cancelled = true;
        }

        if(listData->dragged) {
            int dy = (int) pos.py - (int) listData->lastTouchY;
            listData->scrollPos -= dy;
            // Remembered for the flick when the finger lifts.
            listData->scrollVelocity = listData->scrollVelocity * 0.5f + dy * 0.5f;
            list_clamp_scroll(listData, &l);
        }

        listData->lastTouchY = pos.py;

        if(listData->longPress != NULL && listData->press.touch && widget_press_long_due(&listData->press)
           && (u32) listData->press.index < count) {
            longPressed = listData->visible[listData->press.index];
        }
    } else if(listData->touching && (hidKeysUp() & KEY_TOUCH)) {
        listData->touching = false;
        if(!listData->dragged) {
            listData->scrollVelocity = 0;
        }

        if(listData->press.touch && widget_press_on(&listData->press, listData->press.index) && (u32) listData->press.index < count) {
            listData->selectedIndex = (u32) listData->press.index;
            listData->selectedItem = listData->visible[listData->selectedIndex];
            selectedTouched = true;
        }

        widget_press_end(&listData->press);
    }

    if(longPressed != NULL) {
        listData->longPress(view, listData->data, &listData->items, longPressed);
        return;
    }

    if(listData->update != NULL) {
        listData->update(view, listData->data, &listData->items, listData->selectedItem, selectedTouched);
    }
}

typedef struct {
    ui_view* view;
    list_data* listData;
    float x1, y1, x2, y2;
} list_top_context;

static void list_draw_top_item(void* data, bool leaving) {
    list_top_context* c = (list_top_context*) data;
    list_item* item = leaving ? c->listData->leavingItem : c->listData->selectedItem;
    c->listData->drawTop(c->view, c->listData->data, c->x1, c->y1, c->x2, c->y2, item);
}

static void list_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    list_data* listData = (list_data*) data;

    if(listData->drawTop != NULL) {
        if(listData->selectedItem != listData->shownItem) {
            listData->leavingItem = listData->shownItem;
            listData->shownItem = listData->selectedItem;
            listData->shownTime = osGetTime();
        }

        list_top_context context = {view, listData, x1, y1, x2, y2};
        // The item left may have been deleted meanwhile.
        bool leaving = listData->leavingItem != NULL && linked_list_index_of(&listData->items, listData->leavingItem) >= 0;
        widget_crossfade(widget_ease(listData->shownTime, WIDGET_CROSSFADE_TIME), list_draw_top_item, &context, leaving);
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
            widget_row(x, y, l.itemW, l.itemH, -1, i18n_translate_text(item->name), screen_get_color(item->color), NULL, selected);
        }

        widget_press_draw(&listData->press, (int) i, x, y, l.itemW, l.itemH, list_grid(listData) ? t->def->radius : t->def->radiusSmall,
                          selected ? t->onAccent : t->palette->text, listData->longPress != NULL);
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
    listData->press.index = -1;
    listData->barTarget = -2;
    listData->update = update;
    listData->drawTop = drawTop;

    ui_view* view = ui_create();
    view->name = i18n_translate_text(name);
    view->id = name;
    view->info = i18n_translate_text(info);
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
    listData->scrollPos = listData->scrollTarget = listData->scrollVelocity = 0;
    listData->scrollGliding = false;
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

void list_set_filter(ui_view* view, bool (*visible)(void* data, list_item* item)) {
    ((list_data*) view->data)->visibleFilter = visible;
}

void list_set_search(ui_view* view, const char* text) {
    list_data* listData = (list_data*) view->data;
    snprintf(listData->search, sizeof(listData->search), "%s", text != NULL ? text : "");
    listData->scrollPos = listData->scrollTarget = listData->scrollVelocity = 0;
    listData->scrollGliding = false;
}

const char* list_get_search(ui_view* view) {
    return ((list_data*) view->data)->search;
}

u32 list_visible_count(ui_view* view) {
    return ((list_data*) view->data)->visibleCount;
}

void list_set_long_press(ui_view* view, void (*longPress)(ui_view* view, void* data, linked_list* items, list_item* item)) {
    ((list_data*) view->data)->longPress = longPress;
}

void list_select(ui_view* view, list_item* item) {
    list_data* listData = (list_data*) view->data;
    listData->selectedItem = item;
    listData->revealPending = true;
}

void list_show_title_bar(ui_view* view) {
    view->drawTitleBar = list_draw_title_bar;
}

void list_set_progress(ui_view* view, u32 done, u32 total) {
    list_data* listData = (list_data*) view->data;
    listData->progressDone = done;
    listData->progressTotal = total;
}

bool list_describe(ui_view* view, ui_description* out) {
    if(view == NULL || view->update != list_update) {
        return false;
    }

    list_data* listData = (list_data*) view->data;
    out->kind = "list";
    out->count = listData->visibleCount;
    out->selected = listData->selectedItem != NULL ? linked_list_index_of(&listData->items, listData->selectedItem) : -1;
    out->selectedName = listData->selectedItem != NULL ? listData->selectedItem->name : NULL;
    return true;
}
