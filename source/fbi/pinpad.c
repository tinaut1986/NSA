// NSA's numeric keypad for the web PIN: the six digits on the top screen, the keys on the bottom one (the same keys as the
// keyboards': keys.h). The key with the turning arrows makes a random PIN. A or START accepts (with six digits), B cancels,
// X deletes, Y makes a random one.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "pinpad.h"
#include "webserver.h"
#include "../core/core.h"
#include "../core/ui/keys.h"

#define PINPAD_DIGITS 6
#define PAD 8
#define GAP 4

static const key_def pinpad_digit_defs[10] = {
    KS("0"), KS("1"), KS("2"), KS("3"), KS("4"), KS("5"), KS("6"), KS("7"), KS("8"), KS("9")
};
static const key_def pinpad_back = KA(K_BACK, 1);
static const key_def pinpad_random = KA(K_REFRESH, 1);
static const key_def pinpad_ok = KA(K_OK, 1);
static const key_def pinpad_cancel = KA(K_CANCEL, 1);

typedef struct {
    const char* hint;
    char text[PINPAD_DIGITS + 1];
    u32 len;
    key_rect keys[14];
    u32 keyCount;
    bool built;
    int pressedKey;
    u64 repeatAt;
    u64 holdAt[2];   // X and Y.

    void* data;
    void (*onResponse)(ui_view* view, void* data, SwkbdButton button, const char* response);
    bool finished;
} pinpad_data;

static void pinpad_build_keys(pinpad_data* d, float x1, float y1, float x2, float y2) {
    // Four rows of three digits' keys and a row with Cancel and OK, filling the screen.
    float rows = 5;
    float w = x2 - x1 - 2 * PAD;
    float h = y2 - y1 - 2 * PAD;
    float keyH = (h - (rows - 1) * GAP) / rows;
    float keyW3 = (w - 2 * GAP) / 3;
    float keyW2 = (w - GAP) / 2;

    static const int order[4][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {-1, 0, -2}};
    d->keyCount = 0;
    for(int r = 0; r < 4; r++) {
        for(int c = 0; c < 3; c++) {
            int v = order[r][c];
            key_rect* k = &d->keys[d->keyCount++];
            k->x = x1 + PAD + c * (keyW3 + GAP);
            k->y = y1 + PAD + r * (keyH + GAP);
            k->w = keyW3;
            k->h = keyH;
            k->def = v >= 0 ? &pinpad_digit_defs[v] : v == -1 ? &pinpad_random : &pinpad_back;
        }
    }

    for(int c = 0; c < 2; c++) {
        key_rect* k = &d->keys[d->keyCount++];
        k->x = x1 + PAD + c * (keyW2 + GAP);
        k->y = y1 + PAD + 4 * (keyH + GAP);
        k->w = keyW2;
        k->h = keyH;
        k->def = c == 0 ? &pinpad_cancel : &pinpad_ok;
    }

    d->built = true;
}

static void pinpad_finish(ui_view* view, pinpad_data* d, bool accepted) {
    if(d->finished || (accepted && d->len != PINPAD_DIGITS)) {
        return;
    }

    d->finished = true;
    ui_pop();
    if(accepted) {
        sheet_confirmed();
    }

    if(d->onResponse != NULL) {
        d->onResponse(view, d->data, accepted ? SWKBD_BUTTON_CONFIRM : SWKBD_BUTTON_LEFT, d->text);
    }

    free(d);
    ui_destroy(view);
}

static void pinpad_backspace(pinpad_data* d) {
    if(d->len > 0) {
        d->text[--d->len] = '\0';
    }
}

static void pinpad_make_random(pinpad_data* d) {
    webserver_random_pin(d->text, sizeof(d->text));
    d->len = (u32) strlen(d->text);
}

// Runs a key; true when it repeats while held.
static bool pinpad_run_key(ui_view* view, pinpad_data* d, const key_def* def) {
    switch(def->action) {
        case K_CHAR:
            if(d->len < PINPAD_DIGITS) {
                d->text[d->len++] = def->lower[0];
                d->text[d->len] = '\0';
            }

            return false;
        case K_BACK: pinpad_backspace(d); return true;
        case K_REFRESH: pinpad_make_random(d); return false;
        case K_OK: pinpad_finish(view, d, true); return false;
        case K_CANCEL: pinpad_finish(view, d, false); return false;
        default: return false;
    }
}

static void pinpad_draw_top(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    pinpad_data* d = (pinpad_data*) v;
    const theme_current* t = theme();

    screen_set_depth(0);
    screen_fill_rect(x1, y1, x2 - x1, y2 - y1, t->palette->surface);

    float boxW = 42;
    float boxH = 56;
    float gap = 8;
    float total = PINPAD_DIGITS * boxW + (PINPAD_DIGITS - 1) * gap;
    float x = x1 + (x2 - x1 - total) / 2;
    float y = y1 + (y2 - y1 - boxH) / 2;

    widget_text_centered(d->hint, x1, x2, y - 30, WIDGET_TEXT_SCALE, t->palette->textMuted);
    for(u32 i = 0; i < PINPAD_DIGITS; i++) {
        float bx = x + i * (boxW + gap);
        bool current = i == d->len;
        screen_frame_round_rect(bx, y, boxW, boxH, t->def->radiusSmall, t->def->border, current ? t->accent : t->palette->border, t->palette->background);
        if(i < d->len) {
            char digit[2] = {d->text[i], '\0'};
            widget_text_centered(digit, bx, bx + boxW, y + (boxH - widget_text_height(WIDGET_TITLE_SCALE)) / 2, WIDGET_TITLE_SCALE, t->palette->text);
        }
    }
}

static void pinpad_draw_bottom(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    pinpad_data* d = (pinpad_data*) v;
    if(!d->built) {
        pinpad_build_keys(d, x1, y1, x2, y2);
    }

    for(u32 i = 0; i < d->keyCount; i++) {
        keys_draw(&d->keys[i], d->pressedKey == (int) i, 0);
    }
}

static void pinpad_update(ui_view* view, void* v, float bx1, float by1, float bx2, float by2) {
    pinpad_data* d = (pinpad_data*) v;

    view->info = _(" ok    delete    random    cancel");
    if(!d->built) {
        pinpad_build_keys(d, bx1, by1, bx2, by2);
    }

    if(hidKeysDown() & KEY_B) {
        pinpad_finish(view, d, false);
        return;
    }

    if(hidKeysDown() & (KEY_A | KEY_START)) {
        pinpad_finish(view, d, true);
        return;
    }

    if(keys_hold_repeat(&d->holdAt[0], KEY_X)) pinpad_backspace(d);
    if(hidKeysDown() & KEY_Y) pinpad_make_random(d);

    touchPosition pos;
    hidTouchRead(&pos);

    if(hidKeysDown() & KEY_TOUCH) {
        d->pressedKey = keys_hit(d->keys, d->keyCount, &pos);
        if(d->pressedKey >= 0) {
            const key_def* def = d->keys[d->pressedKey].def;
            key_action action = def->action;
            bool repeats = pinpad_run_key(view, d, def);
            if(action == K_OK || action == K_CANCEL) {
                if(d->finished) {
                    return;   // Gone, and its data freed: touch nothing of it.
                }
            }

            d->repeatAt = repeats ? osGetTime() + 450 : 0;
        }
    } else if(hidKeysHeld() & KEY_TOUCH) {
        if(keys_touch_repeat(d->keys, d->pressedKey, &d->repeatAt, &pos)) {
            pinpad_run_key(view, d, d->keys[d->pressedKey].def);
        }
    } else if(hidKeysUp() & KEY_TOUCH) {
        d->pressedKey = -1;
        d->repeatAt = 0;
    }
}

ui_view* pinpad_display(const char* hint, const char* initialPin, void* data, void (*onResponse)(ui_view* view, void* data, SwkbdButton button, const char* response)) {
    pinpad_data* d = (pinpad_data*) calloc(1, sizeof(pinpad_data));
    if(d == NULL) {
        error_display(NULL, NULL, "Failed to allocate keypad data.");
        return NULL;
    }

    d->hint = i18n_translate_text(hint);
    if(initialPin != NULL && strlen(initialPin) == PINPAD_DIGITS) {
        memcpy(d->text, initialPin, PINPAD_DIGITS);
        d->len = PINPAD_DIGITS;
    }

    d->pressedKey = -1;
    d->data = data;
    d->onResponse = onResponse;

    ui_view* view = ui_create();
    view->name = d->hint;
    view->info = "";
    view->data = d;
    view->update = pinpad_update;
    view->drawTop = pinpad_draw_top;
    view->drawBottom = pinpad_draw_bottom;
    ui_push(view);
    return view;
}
