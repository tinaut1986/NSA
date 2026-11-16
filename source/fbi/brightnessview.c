#include <stdio.h>
#include <stdlib.h>

#include <3ds.h>

#include "brightness.h"
#include "section.h"
#include "settings.h"
#include "../core/core.h"

// System -> Console settings -> Screen brightness (brightness.h): System or Custom, and with Custom two bars, the
// lowest and the highest brightness. The screens follow a bar as it moves; the range is saved when it lets go.

typedef enum {
    ROW_MODE,
    ROW_LOW,
    ROW_HIGH,
    ROW_COUNT
} brightness_row;

#define PAD 8
#define STEP 2

typedef struct {
    u32 selected;
    bool custom;        // A range of the user's is in use.
    bool moved;         // A bar moved and is not saved yet.
    int sliding;        // The row whose bar a finger holds, or -1.
    bool touching;
    widget_press press;
    u64 nextMove;
    u32 now;            // The brightness the top screen has, asked twice a second.
    u64 nextAsk;
} brightness_data;

static u32* brightnessview_value(brightness_row row) {
    settings_data* settings = settings_get();
    return row == ROW_LOW ? &settings->brightnessLowest : row == ROW_HIGH ? &settings->brightnessHighest : NULL;
}

static u32 brightnessview_row_count(brightness_data* data) {
    return data->custom ? ROW_COUNT : 1;
}

static float brightnessview_row_height(brightness_row row) {
    return row == ROW_MODE ? WIDGET_ROW_HEIGHT : widget_row_slider_height();
}

static float brightnessview_row_top(brightness_row row) {
    float top = 0;
    for(u32 i = 0; i < (u32) row; i++) {
        top += brightnessview_row_height((brightness_row) i) + WIDGET_ROW_GAP;
    }

    return top;
}

static const char* brightnessview_label(brightness_row row) {
    return row == ROW_MODE ? _("Screen brightness") : row == ROW_LOW ? _("Lowest brightness") : _("Highest brightness");
}

static const char* brightnessview_help(brightness_data* data, brightness_row row) {
    switch(row) {
        case ROW_MODE:
            return data->custom ? _("Custom: the console's five brightness levels are spread between the lowest and the highest below, in every game and in HOME, auto-brightness too. It stays after NSA closes; System puts the console's own levels back.")
                                : _("System: the console's own brightness levels. Custom spreads its five levels between a lowest and a highest you choose, in every game, auto-brightness too: so it never gets too dark.");
        case ROW_LOW: return _("How dark the screens can get: level 1, and the least auto-brightness goes down to. The console's own is 16.");
        default: return _("How bright the screens can get: level 5, and how high auto-brightness goes. The console's own is 142.");
    }
}

static void brightnessview_save_settings() {
    Result res = settings_save();
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to save settings."));
    }
}

// System <-> Custom: the console's own levels, or the range last chosen; at once and saved.
static void brightnessview_toggle(brightness_data* data) {
    settings_data* settings = settings_get();
    Result res = data->custom ? brightness_restore() : brightness_set(settings->brightnessLowest, settings->brightnessHighest, true);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to set the brightness."));
        return;
    }

    data->custom = !data->custom;
    settings->brightnessCustom = data->custom;
    brightnessview_save_settings();
}

// A bar moved to value: the other end never passes it. Applied at once; saved later (brightnessview_update).
static void brightnessview_move(brightness_data* data, brightness_row row, u32 value) {
    settings_data* settings = settings_get();
    if(value < BRIGHTNESS_LOWEST) {
        value = BRIGHTNESS_LOWEST;
    } else if(value > BRIGHTNESS_HIGHEST) {
        value = BRIGHTNESS_HIGHEST;
    }

    if(row == ROW_LOW) {
        settings->brightnessLowest = value > settings->brightnessHighest ? settings->brightnessHighest : value;
    } else {
        settings->brightnessHighest = value < settings->brightnessLowest ? settings->brightnessLowest : value;
    }

    Result res = brightness_set(settings->brightnessLowest, settings->brightnessHighest, false);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to set the brightness."));
    }

    data->moved = true;
}

static bool brightnessview_repeat(brightness_data* data, u32 key) {
    if(hidKeysDown() & key) {
        data->nextMove = osGetTime() + 400;
        return true;
    }

    if((hidKeysHeld() & key) && osGetTime() >= data->nextMove) {
        data->nextMove = osGetTime() + 60;
        return true;
    }

    return false;
}

static void brightnessview_track(float bx1, float bx2, float* trackX, float* trackWidth) {
    float centerY;
    widget_row_slider_track(bx1 + PAD, 0, bx2 - bx1 - 2 * PAD, trackX, trackWidth, &centerY);
}

static void brightnessview_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    brightness_data* d = (brightness_data*) data;
    brightness_row row = (brightness_row) d->selected;

    view->info = row == ROW_MODE ? _(" change    back") : _("  brightness    back");

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        free(d);
        ui_destroy(view);
        return;
    }

    u32 rows = brightnessview_row_count(d);
    if(hidKeysDown() & KEY_DOWN) {
        d->selected = (d->selected + 1) % rows;
    } else if(hidKeysDown() & KEY_UP) {
        d->selected = (d->selected + rows - 1) % rows;
    } else if(row == ROW_MODE && (hidKeysDown() & (KEY_A | KEY_LEFT | KEY_RIGHT))) {
        brightnessview_toggle(d);
    } else if(row != ROW_MODE && (brightnessview_repeat(d, KEY_RIGHT) || brightnessview_repeat(d, KEY_LEFT) || brightnessview_repeat(d, KEY_X) || brightnessview_repeat(d, KEY_Y))) {
        u32 value = *brightnessview_value(row);
        bool up = ((hidKeysHeld() | hidKeysDown()) & (KEY_RIGHT | KEY_X)) != 0;
        brightnessview_move(d, row, up ? value + STEP : (value > STEP ? value - STEP : 0));
    }

    // Touch: a tap selects a row, a tap on the selected mode row changes it; a finger on a bar takes it.
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        d->touching = true;
        d->sliding = -1;
        widget_press_end(&d->press);
        float offset = pos.py - by1 - PAD;
        for(u32 i = 0; i < brightnessview_row_count(d); i++) {
            float top = brightnessview_row_top((brightness_row) i);
            if(offset < top || offset >= top + brightnessview_row_height((brightness_row) i)) {
                continue;
            }

            d->selected = i;
            if(i != ROW_MODE && offset - top >= WIDGET_ROW_HEIGHT - 2) {
                d->sliding = (int) i;
            } else {
                widget_press_begin(&d->press, (int) i, true, pos.px, pos.py);
            }

            break;
        }
    }

    if(d->touching && d->sliding >= 0 && (hidKeysHeld() & KEY_TOUCH)) {
        float trackX, trackWidth;
        brightnessview_track(bx1, bx2, &trackX, &trackWidth);
        float fraction = ((float) pos.px - trackX) / trackWidth;
        fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
        u32 value = BRIGHTNESS_LOWEST + (u32) (fraction * (BRIGHTNESS_HIGHEST - BRIGHTNESS_LOWEST) + 0.5f);
        if(value != *brightnessview_value((brightness_row) d->sliding)) {
            brightnessview_move(d, (brightness_row) d->sliding, value);
        }
    } else if(d->touching && (hidKeysUp() & KEY_TOUCH)) {
        d->touching = false;
        if(d->sliding >= 0) {
            d->sliding = -1;
            sound_play(SOUND_TICK);
        } else {
            int index = d->press.index;
            bool released = widget_press_on(&d->press, index);
            widget_press_end(&d->press);
            if(released && index == ROW_MODE && d->selected == ROW_MODE) {
                brightnessview_toggle(d);
            }
        }
    }

    // Nothing holds a bar any more: the range goes to the NAND and settings.json.
    if(d->moved && d->sliding < 0 && !(hidKeysHeld() & (KEY_LEFT | KEY_RIGHT | KEY_X | KEY_Y))) {
        settings_data* settings = settings_get();
        d->moved = false;
        Result res = brightness_set(settings->brightnessLowest, settings->brightnessHighest, true);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, _("Failed to set the brightness."));
        }

        brightnessview_save_settings();
    }

    if(d->selected >= brightnessview_row_count(d)) {
        d->selected = 0;
    }
}

static void brightnessview_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    brightness_data* d = (brightness_data*) data;
    const theme_current* t = theme();
    brightness_row row = (brightness_row) d->selected;

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = y2 - y1 - 28;
    widget_card(cardX, cardY, cardW, cardH);

    float x = cardX + 16;
    float y = cardY + 14;
    screen_draw_icon(ICON_SUN, x, y, 22, t->accent);
    screen_draw_text(brightnessview_label(row), x + 30, y + 1, WIDGET_TITLE_SCALE, t->palette->text);
    screen_draw_text_wrap(brightnessview_help(d, row), x, y + 32, WIDGET_TEXT_SCALE, t->palette->textMuted, cardX + cardW - 16);

    // The five levels in use and the console's own, and what the screens have now (a New 3DS can tell).
    u16 now[5];
    u16 factory[5];
    if(!brightness_levels(now)) {
        return;
    }

    brightness_factory_levels(factory);
    float ly = cardY + cardH - 56;
    char line[96];
    snprintf(line, sizeof(line), "%s %u  %u  %u  %u  %u", _("Levels:"), now[0], now[1], now[2], now[3], now[4]);
    screen_draw_text(line, x, ly, WIDGET_TEXT_SCALE, t->palette->text);
    snprintf(line, sizeof(line), "%s %u  %u  %u  %u  %u", _("The console's own:"), factory[0], factory[1], factory[2], factory[3], factory[4]);
    screen_draw_text(line, x, ly + 18, WIDGET_SMALL_SCALE, t->palette->textMuted);

    if(osGetTime() >= d->nextAsk) {
        d->now = brightness_now();
        d->nextAsk = osGetTime() + 500;
    }

    if(d->now > 0) {
        snprintf(line, sizeof(line), "%s %lu", _("Now:"), d->now);
        screen_draw_text(line, x, ly + 34, WIDGET_SMALL_SCALE, t->palette->textMuted);
    }
}

static void brightnessview_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    brightness_data* d = (brightness_data*) data;
    float rowX = x1 + PAD;
    float rowW = x2 - x1 - 2 * PAD;

    static const char* const anchors[ROW_COUNT] = {"brightness.mode", "brightness.low", "brightness.high"};
    for(u32 i = 0; i < brightnessview_row_count(d); i++) {
        brightness_row row = (brightness_row) i;
        float y = y1 + PAD + brightnessview_row_top(row);
        bool selected = i == d->selected;
        coach_anchor(anchors[i], rowX, y, rowW, brightnessview_row_height(row), selected);
        if(row == ROW_MODE) {
            widget_row(rowX, y, rowW, WIDGET_ROW_HEIGHT, ICON_SUN, brightnessview_label(row), theme()->palette->text, d->custom ? _("Custom") : _("System"), selected);
        } else {
            u32 value = *brightnessview_value(row);
            char number[8];
            snprintf(number, sizeof(number), "%lu", value);
            float fraction = (float) (value - BRIGHTNESS_LOWEST) / (float) (BRIGHTNESS_HIGHEST - BRIGHTNESS_LOWEST);
            widget_row_with_slider(rowX, y, rowW, ICON_SUN, brightnessview_label(row), theme()->palette->text, NULL, selected, fraction, number);
        }

        widget_press_draw(&d->press, (int) i, rowX, y, rowW, WIDGET_ROW_HEIGHT, theme()->def->radiusSmall, selected ? theme()->onAccent : theme()->palette->text, false);
    }
}

void brightnessview_open() {
    if(!brightness_supported()) {
        error_display(NULL, NULL, _("This console's brightness settings are not the ones NSA knows."));
        return;
    }

    brightness_data* data = (brightness_data*) calloc(1, sizeof(brightness_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate brightness data.");
        return;
    }

    data->press.index = -1;
    data->sliding = -1;
    // Custom when it was picked, or when the console's levels are not its own (changed by NSA before, or elsewhere).
    data->custom = settings_get()->brightnessCustom || brightness_custom();

    ui_view* view = ui_create();
    view->name = _("Screen brightness");
    view->info = _(" change    back");
    view->backArrow = true;
    view->data = data;
    view->update = brightnessview_update;
    view->drawTop = brightnessview_draw_top;
    view->drawBottom = brightnessview_draw_bottom;
    ui_push(view);
}
