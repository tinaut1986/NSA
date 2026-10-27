#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "error.h"
#include "info.h"
#include "ui.h"
#include "../screen.h"
#include "../i18n.h"
#include "../input.h"
#include "../theme.h"

typedef struct {
    bool bar;
    void* data;
    float progress;
    char text[PROGRESS_TEXT_MAX];
    void (*update)(ui_view* view, void* data, float* progress, char* text);
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2);

    // The button under the card, if there is one.
    char buttonLabel[64];
    void (*onButton)(ui_view* view, void* data);
    widget_press press;
} info_data;

#define INFO_BUTTON_HEIGHT 28
#define INFO_BUTTON_GAP 10

// Where the card and the button go on the bottom screen.
static void info_layout(info_data* infoData, float x1, float y1, float x2, float y2, float* cardX, float* cardY, float* cardW, float* cardH,
                        float* btnX, float* btnY, float* btnW) {
    float width = x2 - x1 - 24;
    float textWidth = 0;
    float textHeight = 0;
    screen_get_string_size_wrap(&textWidth, &textHeight, infoData->text, WIDGET_TEXT_SCALE, WIDGET_TEXT_SCALE, width - 24);

    float barHeight = infoData->bar ? 10 + 12 : 18 + 6;
    float height = 14 + barHeight + textHeight + 14;
    float total = height + (infoData->onButton != NULL ? INFO_BUTTON_GAP + INFO_BUTTON_HEIGHT : 0);

    *cardX = x1 + 12;
    *cardY = y1 + (y2 - y1 - total) / 2;
    *cardW = width;
    *cardH = height;
    *btnW = width > 240 ? 240 : width;
    *btnX = x1 + (x2 - x1 - *btnW) / 2;
    *btnY = *cardY + height + INFO_BUTTON_GAP;
}

static void info_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    info_data* infoData = (info_data*) data;

    if(infoData->onButton != NULL) {
        float cx, cy, cw, ch, bx, by, bw;
        info_layout(infoData, 0, WIDGET_TITLE_BAR_HEIGHT, 320, 240 - WIDGET_HINT_BAR_HEIGHT, &cx, &cy, &cw, &ch, &bx, &by, &bw);

        touchPosition pos;
        hidTouchRead(&pos);
        if(hidKeysDown() & KEY_TOUCH) {
            if(widget_touched(&pos, bx, by, bw, INFO_BUTTON_HEIGHT)) {
                widget_press_begin(&infoData->press, 0, true, pos.px, pos.py);
            }
        } else if(infoData->press.index >= 0 && (hidKeysHeld() & KEY_TOUCH)) {
            if(!widget_touched(&pos, bx, by, bw, INFO_BUTTON_HEIGHT)) {
                infoData->press.cancelled = true;
            }
        } else if(infoData->press.index >= 0 && (hidKeysUp() & KEY_TOUCH)) {
            bool fire = widget_press_on(&infoData->press, 0);
            widget_press_end(&infoData->press);
            if(fire) {
                infoData->onButton(view, infoData->data);
                return;
            }
        }

        if(hidKeysDown() & KEY_X) {
            infoData->onButton(view, infoData->data);
            return;
        }
    }

    if(infoData->update != NULL) {
        infoData->update(view, infoData->data, &infoData->progress, infoData->text);
    }
}

static void info_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    info_data* infoData = (info_data*) data;

    if(infoData->drawTop != NULL) {
        infoData->drawTop(view, infoData->data, x1, y1, x2, y2);
    }
}

static void info_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    info_data* infoData = (info_data*) data;
    const theme_current* t = theme();

    float x, y, width, height, btnX, btnY, btnW;
    info_layout(infoData, x1, y1, x2, y2, &x, &y, &width, &height, &btnX, &btnY, &btnW);
    float barHeight = infoData->bar ? 10 + 12 : 18 + 6;
    widget_card(x, y, width, height);

    if(infoData->onButton != NULL) {
        widget_button_centered(btnX, btnY, btnW, INFO_BUTTON_HEIGHT, infoData->buttonLabel, false);
        widget_press_draw(&infoData->press, 0, btnX, btnY, btnW, INFO_BUTTON_HEIGHT, t->def->radiusSmall, t->palette->text, false);
    }

    float cy = y + 14;
    if(infoData->bar) {
        // The bar with its percentage on the right.
        char percent[8];
        snprintf(percent, sizeof(percent), "%d %%", (int) (infoData->progress * 100 + 0.5f));
        float percentW = widget_text_width(percent, WIDGET_SMALL_SCALE);
        widget_progress(x + 12, cy, width - 24 - percentW - 8, 10, infoData->progress);
        screen_draw_text(percent, x + width - 12 - percentW, cy + 5 - widget_text_height(WIDGET_SMALL_SCALE) / 2, WIDGET_SMALL_SCALE, t->palette->textMuted);
        cy += barHeight;
    } else {
        // No measure: three dots, one lit at a time.
        u32 lit = (u32) (osGetTime() / 250) % 3;
        for(u32 i = 0; i < 3; i++) {
            screen_fill_circle(x + width / 2 + (i - 1) * 14, cy + 6, 4, i == lit ? t->accent : t->palette->border);
        }

        cy += barHeight;
    }

    // Each line centred, as FBI laid out its progress text.
    float lineY = cy;
    const char* line = infoData->text;
    char buf[PROGRESS_TEXT_MAX];
    while(line != NULL && *line) {
        const char* end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t) (end - line) : strlen(line);
        if(len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }

        memcpy(buf, line, len);
        buf[len] = '\0';
        widget_text_centered(buf, x, x + width, lineY, WIDGET_TEXT_SCALE, t->palette->text);
        lineY += widget_text_height(WIDGET_TEXT_SCALE);
        line = end != NULL ? end + 1 : NULL;
    }
}

ui_view* info_display(const char* name, const char* info, bool bar, void* data, void (*update)(ui_view* view, void* data, float* progress, char* text),
                                                                                void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2)) {
    info_data* infoData = (info_data*) calloc(1, sizeof(info_data));
    if(infoData == NULL) {
        error_display(NULL, NULL, "Failed to allocate info data.");

        return NULL;
    }

    infoData->bar = bar;
    infoData->data = data;
    infoData->progress = 0;
    snprintf(infoData->text, PROGRESS_TEXT_MAX, "%s", _("Please wait..."));
    infoData->update = update;
    infoData->drawTop = drawTop;
    infoData->press.index = -1;

    ui_view* view = ui_create();
    view->name = i18n_translate_text(name);
    view->info = i18n_translate_text(info);
    view->data = infoData;
    view->update = info_update;
    view->drawTop = info_draw_top;
    view->drawBottom = info_draw_bottom;
    ui_push(view);

    return view;
}

void info_destroy(ui_view* view) {
    if(view != NULL) {
        free(view->data);
        ui_destroy(view);
    }
}

void info_set_button(ui_view* view, const char* label, void (*onPress)(ui_view* view, void* data)) {
    info_data* infoData = (info_data*) view->data;

    if(label == NULL || onPress == NULL) {
        infoData->onButton = NULL;
        infoData->buttonLabel[0] = '\0';
        return;
    }

    snprintf(infoData->buttonLabel, sizeof(infoData->buttonLabel), "\uE002  %s", i18n_translate_text(label));
    infoData->onButton = onPress;
}
