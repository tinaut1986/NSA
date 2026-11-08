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

    // The time left under the bar, from the average since the bar started to move (or went back, as a
    // copy of several files does at each one): when, and from where.
    u64 etaSince;
    float etaFrom;
    float etaLast;
} info_data;

#define INFO_BUTTON_HEIGHT 28
#define INFO_BUTTON_GAP 10
#define INFO_WHEEL_RADIUS 11   // The wheel shown when there is no bar.
#define INFO_ETA_AFTER_MS 2000  // No time left before this: the first seconds' speed says little.

// The bar, the line of time left under it, and the gap to the text.
static float info_bar_height(const info_data* infoData) {
    return infoData->bar ? 10 + 5 + widget_text_height(WIDGET_SMALL_SCALE) + 8 : INFO_WHEEL_RADIUS * 2 + 4 + 10;
}

// Where the card and the button go on the bottom screen.
static void info_layout(info_data* infoData, float x1, float y1, float x2, float y2, float* cardX, float* cardY, float* cardW, float* cardH,
                        float* btnX, float* btnY, float* btnW) {
    float width = x2 - x1 - 24;
    float textWidth = 0;
    float textHeight = 0;
    screen_get_string_size_wrap(&textWidth, &textHeight, infoData->text, WIDGET_TEXT_SCALE, WIDGET_TEXT_SCALE, width - 24);

    float barHeight = info_bar_height(infoData);
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

    if(infoData->bar) {
        float progress = infoData->progress;
        if(infoData->etaSince == 0 || progress < infoData->etaLast - 0.001f) {
            infoData->etaSince = osGetTime();
            infoData->etaFrom = progress;
        }

        infoData->etaLast = progress;
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
    float barHeight = info_bar_height(infoData);
    widget_card(x, y, width, height);

    if(infoData->onButton != NULL) {
        widget_button_centered(btnX, btnY, btnW, INFO_BUTTON_HEIGHT, infoData->buttonLabel, false);
        widget_press_draw(&infoData->press, 0, btnX, btnY, btnW, INFO_BUTTON_HEIGHT, t->def->radiusSmall, t->palette->text, false);
    }

    float cy = y + 14;
    if(infoData->bar) {
        // The bar with its percentage on the right.
        // Two decimals, rounded down so it never shows 100.00 before the work is done.
        char percent[16];
        float shown = infoData->progress < 0 ? 0 : infoData->progress > 1 ? 1 : infoData->progress;
        u32 hundredths = (u32) (shown * 10000.0f);
        snprintf(percent, sizeof(percent), "%lu.%02lu %%", hundredths / 100, hundredths % 100);
        float percentW = widget_text_width(percent, WIDGET_SMALL_SCALE);
        widget_progress(x + 12, cy, width - 24 - percentW - 8, 10, infoData->progress);
        screen_draw_text(percent, x + width - 12 - percentW, cy + 5 - widget_text_height(WIDGET_SMALL_SCALE) / 2, WIDGET_SMALL_SCALE, t->palette->textMuted);

        // The time left, once there is something to go by; its line is kept empty until then.
        u64 elapsed = infoData->etaSince != 0 ? osGetTime() - infoData->etaSince : 0;
        float moved = shown - infoData->etaFrom;
        if(elapsed >= INFO_ETA_AFTER_MS && moved > 0.0001f && shown < 1) {
            double seconds = (double) elapsed / 1000.0 * (1.0 - shown) / moved;
            char eta[48];
            snprintf(eta, sizeof(eta), _("%s left"), ui_get_display_eta(seconds > 359999 ? 359999 : (u32) seconds));
            widget_text_centered(eta, x, x + width, cy + 10 + 5, WIDGET_SMALL_SCALE, t->palette->textMuted);
        }

        cy += barHeight;
    } else {
        // No measure: a turning wheel.
        widget_busy(x + width / 2, cy + INFO_WHEEL_RADIUS + 2, INFO_WHEEL_RADIUS, t->accent);

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
    view->id = name;
    view->info = i18n_translate_text(info);
    view->data = infoData;
    view->update = info_update;
    view->drawTop = info_draw_top;
    view->drawBottom = info_draw_bottom;
    ui_push(view);

    return view;
}

bool info_task_finished(ui_view* view, volatile bool* finished, Handle cancelEvent) {
    if(*finished) {
        ui_pop();
        info_destroy(view);
        return true;
    }

    if(hidKeysDown() & KEY_B) {
        svcSignalEvent(cancelEvent);
    }

    return false;
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

bool info_describe(ui_view* view, ui_description* out) {
    if(view == NULL || view->update != info_update) {
        return false;
    }

    info_data* infoData = (info_data*) view->data;
    out->kind = "info";
    out->text = infoData->text;
    out->progress = infoData->bar ? infoData->progress : -1;
    return true;
}
