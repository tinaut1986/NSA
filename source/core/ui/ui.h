#pragma once

typedef struct ui_view_s {
    const char* name;
    const char* info;
    void* data;
    void (*update)(struct ui_view_s* view, void* data, float bx1, float by1, float bx2, float by2);
    void (*drawTop)(struct ui_view_s* view, void* data, float x1, float y1, float x2, float y2);
    void (*drawBottom)(struct ui_view_s* view, void* data, float x1, float y1, float x2, float y2);
    // Optional: draws the title bar's contents itself (title, tool icons).
    void (*drawTitleBar)(struct ui_view_s* view, void* data, float x1, float y1, float x2, float y2);

    Handle active;
} ui_view;

void ui_init();
void ui_exit();

ui_view* ui_create();
void ui_destroy(ui_view* view);

ui_view* ui_top();
bool ui_push(ui_view* view);
void ui_pop();
bool ui_update();

// Background status drawn over every view, set by the app (the FTP server):
// `corner` fills a short line for the top screen's bottom bar (empty: none);
// `notice` fills a toast for the bottom screen and its progress (0..1, < 0
// when unknown), returning false when there is nothing to say.
void ui_set_status(void (*corner)(char* out, size_t size), bool (*notice)(char* out, size_t size, float* progress));

const char* ui_get_display_eta(u32 seconds);
double ui_get_display_size(u64 size);
const char* ui_get_display_size_units(u64 size);

#include "error.h"
#include "info.h"
#include "kbd.h"
#include "list.h"
#include "prompt.h"
#include "widgets.h"
#include "overlay.h"
#include "../input.h"