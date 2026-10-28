#pragma once

typedef struct ui_view_s {
    const char* name;
    const char* id;     // The name before translation, for the tests (selftest.c).
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
// How many views are open (1: the main menu).
u32 ui_depth();
bool ui_push(ui_view* view);
void ui_pop();
bool ui_update();

// Background status drawn over every view, set by the app (the FTP server):
// `corner` fills a short line for the top screen's bottom bar (empty: none);
// `notice` fills a toast for the bottom screen and its progress (0..1, < 0
// when unknown), returning false when there is nothing to say.
void ui_set_status(void (*corner)(char* out, size_t size), bool (*notice)(char* out, size_t size, float* progress));

// What a view shows, for the tests (selftest.c): filled by the module that made it.
#define UI_DESCRIPTION_OPTIONS_MAX 4

typedef struct ui_description_s {
    const char* kind;   // "prompt", "info", "list", or "other".
    const char* text;   // A prompt's message (untranslated when it was a literal); an info's line.
    const char* options[UI_DESCRIPTION_OPTIONS_MAX];
    u32 buttons[UI_DESCRIPTION_OPTIONS_MAX];
    u32 optionCount;
    float progress;     // An info's bar, < 0 without one.
    u32 count;          // A list's visible items.
    int selected;       // Its selection, -1 with none.
    const char* selectedName;
} ui_description;

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