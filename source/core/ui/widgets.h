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
