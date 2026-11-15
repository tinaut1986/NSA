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
    // A screen that is not a list but opens from another: the default title bar gets the back arrow of the
    // lists' (left of the title), and a tap on it is a press of B.
    bool backArrow;

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
// Counts ui_update calls: what draws more than once a frame (the top screen, once per eye in 3D) reads input once.
u32 ui_frame();
// Draws every frame again for a moment: for what moves without input.
void ui_wake();

// Background status drawn over every view, set by the app (the FTP server):
// `corner` fills a short line for the top screen's bottom bar (empty: none);
// `notice` fills a toast for the bottom screen and its progress (0..1, < 0
// when unknown), returning false when there is nothing to say.
// What the top bar shows besides the clock: `activity` gives UI_STATUS_* bits.
#define UI_STATUS_FTP_BUSY 1   // The FTP server is moving data: its icon blinks.
#define UI_STATUS_WEB 2        // The web server runs: its icon is shown.
#define UI_STATUS_PLAYER 4     // The audio player plays in the background: a note.

void ui_set_status(void (*corner)(char* out, size_t size), bool (*notice)(char* out, size_t size, float* progress), u32 (*activity)());
// A tap on the bottom toast (the notice above) calls `tap`; true when it did something, and the view under it does not
// see that touch.
void ui_set_status_tap(bool (*tap)());

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
// A size in short form: "512 B", "4.7 KiB".
void ui_format_size(char* out, size_t size, u64 bytes);
double ui_get_display_size(u64 size);
const char* ui_get_display_size_units(u64 size);

#include "error.h"
#include "info.h"
#include "kbd.h"
#include "osk.h"
#include "list.h"
#include "prompt.h"
#include "widgets.h"
#include "overlay.h"
#include "coach.h"
#include "../input.h"