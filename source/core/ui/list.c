#include <malloc.h>

#include <3ds.h>

#include "error.h"
#include "list.h"
#include "ui.h"
#include "../screen.h"
#include "../linkedlist.h"
#include "../theme.h"

// Rows are widget rows: the list's pitch, and the padding around them.
#define LIST_PITCH (WIDGET_ROW_HEIGHT + WIDGET_ROW_GAP)
#define LIST_PAD 6

typedef struct {
    void* data;
    linked_list items;
    u32 selectedIndex;
    list_item* selectedItem;
    u32 selectionScroll;
    u64 nextSelectionScrollResetTime;
    float scrollPos;
    u32 lastScrollTouchY;
    u64 nextActionTime;
    void (*update)(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched);
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected);
} list_data;

static void list_validate(list_data* listData, float by1, float by2) {
    u32 size = linked_list_size(&listData->items);

    if(size == 0 || listData->selectedIndex < 0) {
        listData->selectedIndex = 0;
        listData->selectedItem = NULL;
        listData->scrollPos = 0;
    }

    if(size > 0) {
        if(listData->selectedIndex > size - 1) {
            listData->selectedIndex = size - 1;
            listData->selectedItem = NULL;
            listData->scrollPos = 0;
        }

        float viewSize = by2 - by1 - 2 * LIST_PAD;
        float fontHeight = LIST_PITCH;

        bool found = false;
        if(listData->selectedItem != NULL) {
            u32 oldIndex = listData->selectedIndex;

            int index = linked_list_index_of(&listData->items, listData->selectedItem);
            if(index != -1) {
                found = true;
                listData->selectedIndex = (u32) index;

                if(listData->selectedIndex != oldIndex) {
                    listData->scrollPos += (listData->selectedIndex * fontHeight) - (oldIndex * fontHeight);
                }
            }
        }

        if(!found) {
            listData->selectedItem = linked_list_get(&listData->items, listData->selectedIndex);

            listData->selectionScroll = 0;
            listData->nextSelectionScrollResetTime = 0;
        }

        float itemY = listData->selectedIndex * fontHeight;

        float minItemScrollPos = itemY - (viewSize - fontHeight);
        if(listData->scrollPos < minItemScrollPos) {
            listData->scrollPos = minItemScrollPos;
        }

        if(listData->scrollPos > itemY) {
            listData->scrollPos = itemY;
        }

        float maxScrollPos = (size * fontHeight) - WIDGET_ROW_GAP - viewSize;
        if(listData->scrollPos > maxScrollPos) {
            listData->scrollPos = maxScrollPos;
        }

        if(listData->scrollPos < 0) {
            listData->scrollPos = 0;
        }
    }
}

static void list_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    list_data* listData = (list_data*) data;

    u32 size = linked_list_size(&listData->items);

    list_validate(listData, by1, by2);

    bool selectedTouched = false;
    if(size > 0) {
        bool scrolls = false;
        if(listData->selectedItem != NULL) {
            float itemWidth;
            screen_get_string_size(&itemWidth, NULL, listData->selectedItem->name, 0.5f, 0.5f);
            if(itemWidth > bx2 - bx1) {
                scrolls = true;

                if(listData->selectionScroll == 0 || listData->selectionScroll >= itemWidth - (bx2 - bx1)) {
                    if(listData->nextSelectionScrollResetTime == 0) {
                        listData->nextSelectionScrollResetTime = osGetTime() + 2000;
                    } else if(osGetTime() >= listData->nextSelectionScrollResetTime) {
                        listData->selectionScroll = listData->selectionScroll == 0 ? 1 : 0;
                        listData->nextSelectionScrollResetTime = 0;
                    }
                } else {
                    listData->selectionScroll++;
                }
            }
        }

        if(!scrolls) {
            listData->selectionScroll = 0;
            listData->nextSelectionScrollResetTime = 0;
        }

        u32 lastSelectedIndex = listData->selectedIndex;

        if((hidKeysDown() & KEY_DOWN) || ((hidKeysHeld() & KEY_DOWN) && osGetTime() >= listData->nextActionTime)) {
            if(listData->selectedIndex < size - 1) {
                listData->selectedIndex++;
            } else {
                listData->selectedIndex = 0;
            }

            listData->nextActionTime = osGetTime() + ((hidKeysDown() & KEY_DOWN) ? 500 : 100);
        }

        if((hidKeysDown() & KEY_UP) || ((hidKeysHeld() & KEY_UP) && osGetTime() >= listData->nextActionTime)) {
            if(listData->selectedIndex > 0) {
                listData->selectedIndex--;
            } else {
                listData->selectedIndex = size - 1;
            }

            listData->nextActionTime = osGetTime() + ((hidKeysDown() & KEY_UP) ? 500 : 100);
        }

        if((hidKeysDown() & KEY_RIGHT) || ((hidKeysHeld() & KEY_RIGHT) && osGetTime() >= listData->nextActionTime)) {
            if(listData->selectedIndex < size - 1) {
                u32 remaining = size - 1 - listData->selectedIndex;
                listData->selectedIndex += remaining < 13 ? remaining : 13;
            } else {
                listData->selectedIndex = 0;
            }

            listData->nextActionTime = osGetTime() + ((hidKeysDown() & KEY_RIGHT) ? 500 : 100);
        }

        if((hidKeysDown() & KEY_LEFT) || ((hidKeysHeld() & KEY_LEFT) && osGetTime() >= listData->nextActionTime)) {
            if(listData->selectedIndex > 0) {
                u32 remaining = listData->selectedIndex;
                listData->selectedIndex -= remaining < 13 ? remaining : 13;
            } else {
                listData->selectedIndex = size - 1;
            }

            listData->nextActionTime = osGetTime() + ((hidKeysDown() & KEY_LEFT) ? 500 : 100);
        }

        if((hidKeysDown() | hidKeysHeld()) & KEY_TOUCH) {
            touchPosition pos;
            hidTouchRead(&pos);

            if(hidKeysDown() & KEY_TOUCH) {
                int touchY = (int) pos.py - (int) (by1 + LIST_PAD);
                u32 index = touchY >= 0 ? (u32) ((listData->scrollPos + touchY) / LIST_PITCH) : size;
                if(index < size) {
                    if(listData->selectedIndex == index) {
                        selectedTouched = true;
                    } else {
                        listData->selectedIndex = (u32) index;
                    }
                }
            } else if(hidKeysHeld() & KEY_TOUCH) {
                listData->scrollPos += -((int) pos.py - (int) listData->lastScrollTouchY);
            }

            listData->lastScrollTouchY = pos.py;
        }

        if(listData->selectedIndex != lastSelectedIndex) {
            listData->selectedItem = linked_list_get(&listData->items, listData->selectedIndex);

            listData->selectionScroll = 0;
            listData->nextSelectionScrollResetTime = 0;
        }

        list_validate(listData, by1, by2);
    }

    if(listData->update != NULL) {
        listData->update(view, listData->data, &listData->items, listData->selectedItem, selectedTouched);
    }
}

static void list_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    list_data* listData = (list_data*) data;

    if(listData->drawTop != NULL) {
        list_validate(listData, y1, y2);

        listData->drawTop(view, listData->data, x1, y1, x2, y2, listData->selectedItem);
    }
}

static void list_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    list_data* listData = (list_data*) data;

    list_validate(listData, y1, y2);

    u32 size = linked_list_size(&listData->items);
    float top = y1 + LIST_PAD;
    float bottom = y2 - LIST_PAD;
    float totalHeight = size * LIST_PITCH - WIDGET_ROW_GAP;
    bool scrolls = totalHeight > bottom - top;
    float rowX = x1 + 8;
    float rowWidth = x2 - x1 - 16 - (scrolls ? WIDGET_SCROLLBAR_WIDTH + 4 : 0);

    float y = top - listData->scrollPos;

    screen_set_clip(x1, y1 + LIST_PAD / 2, x2 - x1, y2 - y1 - LIST_PAD);

    linked_list_iter iter;
    linked_list_iterate(&listData->items, &iter);

    while(linked_list_iter_has_next(&iter)) {
        if(y > y2) {
            break;
        }

        list_item* item = linked_list_iter_next(&iter);

        if(y + WIDGET_ROW_HEIGHT >= y1) {
            widget_row(rowX, y, rowWidth, WIDGET_ROW_HEIGHT, -1, item->name, screen_get_color(item->color), NULL, item == listData->selectedItem);
        }

        y += LIST_PITCH;
    }

    screen_clear_clip();

    if(scrolls) {
        widget_scrollbar(x2 - 8 - WIDGET_SCROLLBAR_WIDTH, top, bottom - top, listData->scrollPos, bottom - top, totalHeight);
    }
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
    listData->selectedIndex = 0;
    listData->selectedItem = NULL;
    listData->selectionScroll = 0;
    listData->nextSelectionScrollResetTime = 0;
    listData->scrollPos = 0;
    listData->lastScrollTouchY = 0;
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
        linked_list_destroy(&((list_data*) view->data)->items);

        free(view->data);
        ui_destroy(view);
    }
}