#pragma once

typedef struct ui_view_s ui_view;

#define PROMPT_OPTIONS_MAX 4
#define PROMPT_OPTION_TEXT_MAX 64

#define PROMPT_BUTTON_ANY 0xFFFFFFFF

#define PROMPT_OK 0

#define PROMPT_YES 0
#define PROMPT_NO 1

typedef struct ui_description_s ui_description;
// What the prompt `view` asks (selftest.c); false when `view` is not a prompt.
bool prompt_describe(ui_view* view, ui_description* out);

// Yes / No for something that can harm the console: drawn in a soft red.
ui_view* prompt_display_warning(const char* name, const char* text, u32 color, void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2),
                                                                                          void (*onResponse)(ui_view* view, void* data, u32 response));

ui_view* prompt_display_multi_choice(const char* name, const char* text, u32 color, const char** options, u32* optionButtons, u32 numOptions, void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2),
                                                                                                                                                          void (*onResponse)(ui_view* view, void* data, u32 response));
ui_view* prompt_display_notify(const char* name, const char* text, u32 color, void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2),
                                                                                          void (*onResponse)(ui_view* view, void* data, u32 response));
ui_view* prompt_display_yes_no(const char* name, const char* text, u32 color, void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2),
                                                                                         void (*onResponse)(ui_view* view, void* data, u32 response));