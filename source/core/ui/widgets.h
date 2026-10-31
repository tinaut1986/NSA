#pragma once

// The themed building blocks every screen draws with (theme.h decides the
// look). Coordinates are in the current screen's pixels.

#define WIDGET_TEXT_SCALE 0.5f
#define WIDGET_SMALL_SCALE 0.42f
#define WIDGET_TITLE_SCALE 0.6f

// Bars of the screen chrome (ui.c).
#define WIDGET_TOP_BAR_HEIGHT 22
#define WIDGET_TITLE_BAR_HEIGHT 26
#define WIDGET_HINT_BAR_HEIGHT 22

#define WIDGET_ROW_HEIGHT 26
#define WIDGET_ROW_GAP 4
#define WIDGET_SCROLLBAR_WIDTH 4

float widget_text_width(const char* text, float scale);
float widget_text_height(float scale);
// Text cut to fit `maxWidth`, with "..." when it does not.
void widget_text_fit(const char* text, float x, float y, float scale, u32 color, float maxWidth);
void widget_text_centered(const char* text, float x1, float x2, float y, float scale, u32 color);

void widget_background(float width, float height);
void widget_card(float x, float y, float width, float height);
// A pill with text, for statuses ("Installed").
void widget_badge(float x, float y, const char* text, u32 background, u32 foreground);

// A button: icon (or -1) and label; `selected` fills it with the accent.
void widget_button(float x, float y, float width, float height, int icon, const char* label, bool selected);
// The same in a soft red, for what can harm the console.
void widget_button_danger(float x, float y, float width, float height, int icon, const char* label, bool selected);
void widget_button_centered_danger(float x, float y, float width, float height, const char* label, bool selected);
// A button with its label centred (modal buttons).
void widget_button_centered(float x, float y, float width, float height, const char* label, bool selected);
// A list row: icon (or -1), label on the left and, if given, a value on the
// right between ‹ › (an option that changes in place).
void widget_row(float x, float y, float width, float height, int icon, const char* label, u32 labelColor, const char* value, bool selected);

void widget_progress(float x, float y, float width, float height, float progress);
void widget_scrollbar(float x, float y, float height, float offset, float visible, float total);

// The bottom screen's toast, just above `bottom`: a line and, when progress
// is known (>= 0), a bar.
void widget_toast(const char* text, float progress, float bottom);

// A modal: dims the screen and draws the card; returns its content area.
void widget_modal(float screenWidth, float screenHeight, float width, float height, float* x, float* y);

bool widget_touched(const touchPosition* pos, float x, float y, float width, float height);

// ---- presses ----

// A finger (or a button) held on a target: it looks pressed while held, acts
// on release over it, and a long hold fills it in a circle from where it was
// pressed until the long press is due.
#define WIDGET_LONG_PRESS_DELAY 150   // ms before the fill shows (a tap never sees it).
#define WIDGET_LONG_PRESS_TIME 650    // ms until the long press.

typedef struct {
    int index;         // The target held, or -1.
    bool touch;        // By touch; else by a button.
    bool cancelled;    // Moved off or dragged: nothing on release.
    bool fired;        // The long press has run.
    float x, y;        // Where the fill grows from.
    u64 start;
} widget_press;

void widget_press_begin(widget_press* p, int index, bool touch, float x, float y);
void widget_press_end(widget_press* p);
// True for the target held and not cancelled.
bool widget_press_on(const widget_press* p, int index);
// 0..1 towards the long press.
float widget_press_progress(const widget_press* p);
// True once, when a long press becomes due.
bool widget_press_long_due(widget_press* p);
// The pressed look over a target drawn at x, y, w, h with corner `radius`;
// `ink` is the colour of what is on it (text). `fill` draws the long press fill.
void widget_press_draw(const widget_press* p, int index, float x, float y, float width, float height, float radius, u32 ink, bool fill);

// ---- motion ----

// 0..1 from `start` over `duration` ms, easing out (fast, then settling).
float widget_ease(u64 start, u32 duration);

#define WIDGET_CROSSFADE_TIME 220
#define WIDGET_CROSSFADE_DISTANCE 8

// A card's contents changing (`progress` 0..1): `draw` runs for what arrives
// (leaving false) and, while `hasLeaving`, for what leaves (true).
void widget_crossfade(float progress, void (*draw)(void* context, bool leaving), void* context, bool hasLeaving);

// ---- storage ----

// One line with the medium's name and "X free of Y" over a bar of how full it
// is, `width` wide (about 22 px tall). The space is looked up every couple of
// seconds. Not drawn when it cannot be read.
void widget_storage(FS_SystemMediaType media, float x, float y, float width);

// ---- status ----

// The battery as the system reports it, looked up every couple of seconds:
// `percent` is -1 when only the 0..5 `level` is known.
void widget_battery_state(int* percent, u8* level, bool* charging);
// A battery drawn with its percentage inside, coloured by how much is left,
// and a bolt on its left while charging. Right-aligned at `right`; returns
// the width used.
float widget_battery(float right, float y, float height);
// Wi-Fi strength as three bars rising left to right. Returns the width used.
float widget_wifi(float x, float y, float height);
