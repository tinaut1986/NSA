#pragma once

typedef struct ui_view_s ui_view;

#define PROMPT_OPTIONS_MAX 4
#define PROMPT_OPTION_TEXT_MAX 64

#define PROMPT_BUTTON_ANY 0xFFFFFFFF

#define PROMPT_OK 0

#define PROMPT_YES 0
#define PROMPT_NO 1

// The prompt on top, for the web page to show and answer (webserver.c): what the console asks and the buttons it offers.
#define PROMPT_WEB_OPTIONS_MAX 5

typedef struct {
    u32 id;                                         // Changes with every prompt.
    char title[64];
    char text[512];
    char options[PROMPT_WEB_OPTIONS_MAX][PROMPT_OPTION_TEXT_MAX];
    u32 numOptions;
    bool danger;
} prompt_web_info;

// False when no prompt is on top (one that is not drawn any more is not offered). Callable from any thread.
bool prompt_web_get(prompt_web_info* out);
// Answers prompt `id` with its option `option` (0 is the first button); the UI thread takes it on its next frame.
// False when that prompt is not the one on top. Callable from any thread.
bool prompt_web_answer(u32 id, u32 option);

// A view that asks something without being a prompt (a list of ways to go ahead) offers it to the web page from its update, every
// frame, with `owner` (any pointer that is the same while the question is open). Returns the option the web page chose, or -1.
// Call prompt_web_forget(owner) when it is answered or closed.
int prompt_web_offer(const void* owner, const char* title, const char* text, const char* const* options, u32 numOptions, bool danger);
void prompt_web_forget(const void* owner);

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