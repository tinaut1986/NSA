#pragma once

#define PROGRESS_TEXT_MAX 512

typedef struct ui_view_s ui_view;

ui_view* info_display(const char* name, const char* info, bool bar, void* data, void (*update)(ui_view* view, void* data, float* progress, char* text),
                                                                                void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2));
void info_destroy(ui_view* view);
// What the info `view` shows (selftest.c); false when `view` is not one.
typedef struct ui_description_s ui_description;
bool info_describe(ui_view* view, ui_description* out);

// A button under the card (touch, or X): `label` is translated here. Setting it again
// changes the label; a NULL label takes the button away.
void info_set_button(ui_view* view, const char* label, void (*onPress)(ui_view* view, void* data));
