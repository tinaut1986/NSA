#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "error.h"
#include "prompt.h"
#include "ui.h"
#include "../screen.h"
#include "../i18n.h"
#include "../icons.h"
#include "../stringutil.h"
#include "../theme.h"



typedef struct {
    const char* text;
    u32 color;
    u32 icon;          // The header's icon and its colour, by kind of dialog.
    u32 iconColor;
    char title[64];
    char options[PROMPT_OPTIONS_MAX][PROMPT_OPTION_TEXT_MAX];
    u32 optionButtons[PROMPT_OPTIONS_MAX];
    u32 numOptions;
    void* data;
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2);
    void (*onResponse)(ui_view* view, void* data, u32 response);
    widget_press press;   // Touched button, answered on release over it.
    u64 opened;           // For the card's entrance.
} prompt_data;

#define PROMPT_WIDTH 288
#define PROMPT_PAD 12
#define PROMPT_BUTTON_HEIGHT 30
#define PROMPT_TEXT_SCALE 0.5f
#define PROMPT_HEADER 24

// Where the modal's buttons are; the same for drawing and touch.
static void prompt_layout(prompt_data* promptData, float bx1, float by1, float bx2, float by2, float* cardX, float* cardY, float* cardH, float* textH) {
    float textWidth = 0;
    screen_get_string_size_wrap(&textWidth, textH, promptData->text, PROMPT_TEXT_SCALE, PROMPT_TEXT_SCALE, PROMPT_WIDTH - 2 * PROMPT_PAD);
    *cardH = PROMPT_PAD + PROMPT_HEADER + *textH + PROMPT_PAD + PROMPT_BUTTON_HEIGHT + PROMPT_PAD;
    float maxH = by2 - by1 - 8;
    if(*cardH > maxH) {
        *cardH = maxH;
    }

    *cardX = bx1 + (bx2 - bx1 - PROMPT_WIDTH) / 2;
    *cardY = by1 + (by2 - by1 - *cardH) / 2;
}

static void prompt_button_rect(prompt_data* promptData, u32 i, float cardX, float cardY, float cardH, float* x, float* y, float* w) {
    float gap = 8;
    *w = (PROMPT_WIDTH - 2 * PROMPT_PAD - gap * (promptData->numOptions - 1)) / promptData->numOptions;
    *x = cardX + PROMPT_PAD + (*w + gap) * i;
    *y = cardY + cardH - PROMPT_PAD - PROMPT_BUTTON_HEIGHT;
}

static void prompt_notify_response(ui_view* view, prompt_data* promptData, u32 response) {
    ui_pop();

    if(promptData->onResponse != NULL) {
        promptData->onResponse(view, promptData->data, response);
    }

    free(promptData);
    ui_destroy(view);
}

static void prompt_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    prompt_data* promptData = (prompt_data*) data;

    u32 down = hidKeysDown();
    for(u32 i = 0; i < promptData->numOptions; i++) {
        if(down & (promptData->optionButtons[i] & ~KEY_TOUCH)) {
            prompt_notify_response(view, promptData, i);
            return;
        }
    }

    touchPosition pos;
    hidTouchRead(&pos);

    float cardX, cardY, cardH, textH;
    prompt_layout(promptData, bx1, by1, bx2, by2, &cardX, &cardY, &cardH, &textH);

    if(hidKeysDown() & KEY_TOUCH) {
        widget_press_end(&promptData->press);
        for(u32 i = 0; i < promptData->numOptions; i++) {
            float x, y, w;
            prompt_button_rect(promptData, i, cardX, cardY, cardH, &x, &y, &w);
            if(widget_touched(&pos, x, y, w, PROMPT_BUTTON_HEIGHT)) {
                widget_press_begin(&promptData->press, (int) i, true, pos.px, pos.py);
            }
        }
    } else if((hidKeysHeld() & KEY_TOUCH) && promptData->press.index >= 0) {
        float x, y, w;
        prompt_button_rect(promptData, (u32) promptData->press.index, cardX, cardY, cardH, &x, &y, &w);
        if(!widget_touched(&pos, x, y, w, PROMPT_BUTTON_HEIGHT)) {
            promptData->press.cancelled = true;
        }
    } else if(hidKeysUp() & KEY_TOUCH) {
        int index = promptData->press.index;
        bool answer = widget_press_on(&promptData->press, index);
        widget_press_end(&promptData->press);
        if(answer) {
            prompt_notify_response(view, promptData, (u32) index);
            return;
        }
    }
}

static void prompt_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    prompt_data* promptData = (prompt_data*) data;

    if(promptData->drawTop != NULL) {
        promptData->drawTop(view, promptData->data, x1, y1, x2, y2);
    }
}

static const char* button_strings[32] = {
        "\uE000",
        "\uE001",
        "Select",
        "Start",
        "D-Pad Right",
        "D-Pad Left",
        "D-Pad Up",
        "D-Pad Down",
        "\uE005",
        "\uE004",
        "\uE002",
        "\uE003",
        "",
        "",
        "ZL",
        "ZR",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "C-Stick Right",
        "C-Stick Left",
        "C-Stick Up",
        "C-Stick Down",
        "Circle Pad Right",
        "Circle Pad Left",
        "Circle Pad Up",
        "Circle Pad Down"
};

static void prompt_button_to_string(char* out, size_t size, u32 button) {
    if(button == PROMPT_BUTTON_ANY) {
        // Any button closes it, but A is the one to show.
        snprintf(out, size, "\uE000");
        return;
    }

    size_t pos = 0;
    for(u8 bit = 0; bit < 32 && pos < size; bit++) {
        if(button & (1 << bit)) {
            if(pos > 0) {
                pos += snprintf(out + pos, size - pos, "/");
            }

            pos += snprintf(out + pos, size - pos, button_strings[bit]);
        }
    }
}

static void prompt_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    prompt_data* promptData = (prompt_data*) data;
    const theme_current* t = theme();

    float cardX, cardY, cardH, textH;
    prompt_layout(promptData, x1, y1, x2, y2, &cardX, &cardY, &cardH, &textH);

    // The scrim darkens in while the card rises into place.
    float e = widget_ease(promptData->opened, 180);
    screen_fill_rect(x1, y1, x2 - x1, y2 - y1, THEME_ALPHA(t->palette->scrim, (u32) (((t->palette->scrim >> 24) & 0xFF) * e)));
    screen_set_offset(0, (1 - e) * 14);
    screen_set_base_alpha((u8) (0x40 + e * 0xBF));

    screen_frame_round_rect(cardX, cardY, PROMPT_WIDTH, cardH, t->def->radius, t->def->border, t->palette->border, t->palette->surface);

    // Header: what kind of dialog it is, by icon and colour.
    screen_draw_icon(promptData->icon, cardX + PROMPT_PAD, cardY + PROMPT_PAD - 1, 16, promptData->iconColor);
    widget_text_fit(promptData->title, cardX + PROMPT_PAD + 22, cardY + PROMPT_PAD, WIDGET_TEXT_SCALE, t->palette->text, PROMPT_WIDTH - 2 * PROMPT_PAD - 22);

    u32 textColor = promptData->color == COLOR_TEXT ? t->palette->text : screen_get_color(promptData->color);
    screen_draw_text_wrap(promptData->text, cardX + PROMPT_PAD, cardY + PROMPT_PAD + PROMPT_HEADER, PROMPT_TEXT_SCALE, textColor, cardX + PROMPT_WIDTH - PROMPT_PAD);

    char button[64];
    char label[PROMPT_OPTION_TEXT_MAX + 65];
    for(u32 i = 0; i < promptData->numOptions; i++) {
        float x, y, w;
        prompt_button_rect(promptData, i, cardX, cardY, cardH, &x, &y, &w);

        prompt_button_to_string(button, sizeof(button), promptData->optionButtons[i]);
        snprintf(label, sizeof(label), "%s  %s", button, promptData->options[i]);

        // The first option (Yes, OK) is the one that goes ahead.
        widget_button_centered(x, y, w, PROMPT_BUTTON_HEIGHT, label, i == 0);
        widget_press_draw(&promptData->press, (int) i, x, y, w, PROMPT_BUTTON_HEIGHT, t->def->radiusSmall, i == 0 ? t->onAccent : t->palette->text, false);
    }

    screen_set_offset(0, 0);
    screen_set_base_alpha(0xFF);
}

ui_view* prompt_display_multi_choice(const char* name, const char* text, u32 color, const char** options, u32* optionButtons, u32 numOptions, void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2),
                                                                                                                                                          void (*onResponse)(ui_view* view, void* data, u32 response)) {
    prompt_data* promptData = (prompt_data*) calloc(1, sizeof(prompt_data));
    if(promptData == NULL) {
        error_display(NULL, NULL, "Failed to allocate prompt data.");

        return NULL;
    }

    promptData->press.index = -1;
    promptData->opened = osGetTime();

    // Messages are mostly literals, some built at run time: looked up by content.
    promptData->text = i18n_translate_text(text);
    promptData->color = color;

    const theme_current* t = theme();
    if(name != NULL && (strcmp(name, "Success") == 0)) {
        promptData->icon = ICON_CHECK;
        promptData->iconColor = t->palette->success;
    } else if(name != NULL && (strcmp(name, "Error") == 0 || strcmp(name, "Failure") == 0)) {
        promptData->icon = ICON_ALERT_TRIANGLE;
        promptData->iconColor = t->palette->danger;
    } else {
        promptData->icon = ICON_INFO_CIRCLE;
        promptData->iconColor = t->accent;
    }

    string_copy(promptData->title, i18n_translate_text(name != NULL ? name : ""), sizeof(promptData->title));

    for(u32 i = 0; i < numOptions && i < PROMPT_OPTIONS_MAX; i++) {
        // Options are literals: translated here, as they are copied.
        string_copy(promptData->options[i], _(options[i]), PROMPT_OPTION_TEXT_MAX);
        promptData->optionButtons[i] = optionButtons[i];
    }

    promptData->numOptions = numOptions;
    promptData->data = data;
    promptData->drawTop = drawTop;
    promptData->onResponse = onResponse;

    ui_view* view = ui_create();
    view->name = i18n_translate_text(name);
    view->info = "";
    view->data = promptData;
    view->update = prompt_update;
    view->drawTop = prompt_draw_top;
    view->drawBottom = prompt_draw_bottom;
    ui_push(view);

    return view;
}

ui_view* prompt_display_notify(const char* name, const char* text, u32 color, void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2),
                                                                                          void (*onResponse)(ui_view* view, void* data, u32 response)) {
    static const char* options[1] = {"OK"};
    static u32 optionButtons[1] = {PROMPT_BUTTON_ANY};
    return prompt_display_multi_choice(name, text, color, options, optionButtons, 1, data, drawTop, onResponse);
}

ui_view* prompt_display_yes_no(const char* name, const char* text, u32 color, void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2),
                                                                                          void (*onResponse)(ui_view* view, void* data, u32 response)) {
    static const char* options[2] = {"Yes", "No"};
    static u32 optionButtons[2] = {KEY_A, KEY_B};
    return prompt_display_multi_choice(name, text, color, options, optionButtons, 2, data, drawTop, onResponse);
}