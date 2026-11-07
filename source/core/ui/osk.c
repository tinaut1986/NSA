// NSA's own on-screen keyboard for a line of text (the same keys as the text editor's). The text is on the top screen and the
// keys on the bottom one. D-Pad left/right move the cursor, X deletes, Y types a space, A or START accepts, B cancels.

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "keys.h"
#include "osk.h"
#include "../core.h"

static const key_row osk_toolbar = {5, {KA(K_CANCEL, 3), KA(K_CLEAR, 1), KA(K_LEFT, 1), KA(K_RIGHT, 1), KA(K_OK, 4)}};

#define OSK_KEYS_MAX 48

typedef struct {
    const char* hint;
    char* text;
    u32 len;
    u32 cursor;
    u32 caretCursor;   // keys_caret_visible's.
    u64 caretMovedAt;
    u32 maxSize;   // With the final NUL.
    u32 flags;

    u32 page;
    u32 shift;     // 0 off, 1 next letter, 2 caps lock.
    key_rect keys[OSK_KEYS_MAX];
    u32 keyCount;
    int pressedKey;
    u64 repeatAt;
    u64 holdAt[4];   // D-Pad left and right, X, Y.

    void* data;
    void (*onResponse)(ui_view* view, void* data, SwkbdButton button, const char* response);
    bool finished;
} osk_data;

static void osk_build_keys(osk_data* d) {
    d->keyCount = keys_build(d->keys, OSK_KEYS_MAX, &osk_toolbar, d->page, (d->flags & OSK_MULTILINE) ? K_ENTER : K_OK);
}

// ---- the text ----

static u32 osk_prev_char(const osk_data* d, u32 at) {
    if(at == 0) {
        return 0;
    }

    at--;
    while(at > 0 && ((u8) d->text[at] & 0xC0) == 0x80) {
        at--;
    }

    return at;
}

static u32 osk_next_char(const osk_data* d, u32 at) {
    if(at >= d->len) {
        return d->len;
    }

    at++;
    while(at < d->len && ((u8) d->text[at] & 0xC0) == 0x80) {
        at++;
    }

    return at;
}

static void osk_insert(osk_data* d, const char* s) {
    u32 n = (u32) strlen(s);
    if(d->len + n + 1 > d->maxSize) {
        return;
    }

    memmove(d->text + d->cursor + n, d->text + d->cursor, d->len - d->cursor + 1);
    memcpy(d->text + d->cursor, s, n);
    d->cursor += n;
    d->len += n;
}

static void osk_backspace(osk_data* d) {
    if(d->cursor == 0) {
        return;
    }

    u32 from = osk_prev_char(d, d->cursor);
    memmove(d->text + from, d->text + d->cursor, d->len - d->cursor + 1);
    d->len -= d->cursor - from;
    d->cursor = from;
}

static void osk_finish(ui_view* view, osk_data* d, bool accepted) {
    if(d->finished) {
        return;
    }

    if(accepted && (d->flags & OSK_NOT_BLANK)) {
        bool text = false;
        for(u32 i = 0; i < d->len; i++) {
            if(d->text[i] != ' ' && d->text[i] != '\n' && d->text[i] != '\t') {
                text = true;
            }
        }

        if(!text) {
            return;
        }
    }

    d->finished = true;
    ui_pop();
    if(accepted) {
        sheet_confirmed();
    }

    if(d->onResponse != NULL) {
        d->onResponse(view, d->data, accepted ? SWKBD_BUTTON_CONFIRM : SWKBD_BUTTON_LEFT, d->text);
    }

    free(d->text);
    free(d);
    ui_destroy(view);
}

// ---- drawing ----

static void osk_draw_top(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    osk_data* d = (osk_data*) v;
    const theme_current* t = theme();

    screen_set_depth(0);
    screen_fill_rect(x1, y1, x2 - x1, y2 - y1, t->palette->surface);

    float boxX = x1 + 14;
    float boxW = x2 - x1 - 28;
    float boxH = 34;
    float boxY = y1 + (y2 - y1 - boxH) / 2;
    widget_text_fit(d->hint, boxX, boxY - 22, WIDGET_TEXT_SCALE, t->palette->textMuted, boxW);
    screen_frame_round_rect(boxX, boxY, boxW, boxH, t->def->radiusSmall, t->def->border, t->accent, t->palette->background);

    // New lines show as "|" while the text is drawn (and are put back below).
    for(u32 i = 0; (d->flags & OSK_MULTILINE) && i < d->len; i++) {
        if(d->text[i] == '\n') {
            d->text[i] = '\r';
        }
    }

    // When the text is wider than the box, the part before the cursor that does not fit is left out: the window starts
    // as far back from the cursor as still fits.
    float room = boxW - 20;
    u32 start = d->cursor;
    while(start > 0) {
        u32 back = osk_prev_char(d, start);
        char saved = d->text[d->cursor];
        d->text[d->cursor] = '\0';
        float w = widget_text_width(d->text + back, WIDGET_TEXT_SCALE);
        d->text[d->cursor] = saved;
        if(w > room - 6) {
            break;
        }

        start = back;
    }

    float textY = boxY + (boxH - widget_text_height(WIDGET_TEXT_SCALE)) / 2;
    if(d->len > start) {
        // Only what can be seen is handed over (the text may be thousands of characters long).
        char shown[256];
        u32 n = d->len - start < sizeof(shown) - 1 ? d->len - start : (u32) sizeof(shown) - 1;
        while(n > 0 && start + n < d->len && ((u8) d->text[start + n] & 0xC0) == 0x80) {
            n--;
        }

        memcpy(shown, d->text + start, n);
        shown[n] = '\0';
        for(u32 i = 0; i < n; i++) {
            if(shown[i] == '\r') {
                shown[i] = '|';
            }
        }

        widget_text_fit(shown, boxX + 10, textY, WIDGET_TEXT_SCALE, t->palette->text, room);
    }

    if(keys_caret_visible(d->cursor, &d->caretCursor, &d->caretMovedAt)) {
        char saved = d->text[d->cursor];
        d->text[d->cursor] = '\0';
        float cx = boxX + 10 + widget_text_width(d->text + start, WIDGET_TEXT_SCALE);
        d->text[d->cursor] = saved;
        screen_fill_rect(cx, boxY + 6, 1.5f, boxH - 12, t->accent);
    }

    for(u32 i = 0; (d->flags & OSK_MULTILINE) && i < d->len; i++) {
        if(d->text[i] == '\r') {
            d->text[i] = '\n';
        }
    }

    char count[24];
    snprintf(count, sizeof(count), "%lu/%lu", d->len, d->maxSize - 1);
    widget_text_centered(count, boxX + boxW - 70, boxX + boxW, boxY + boxH + 4, WIDGET_SMALL_SCALE, t->palette->textMuted);
    ui_wake();
}

static void osk_draw_bottom(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    osk_data* d = (osk_data*) v;
    for(u32 i = 0; i < d->keyCount; i++) {
        keys_draw(&d->keys[i], d->pressedKey == (int) i, d->shift);
    }
}

// ---- input ----

// Runs a key; true when it repeats while held.
static bool osk_run_key(ui_view* view, osk_data* d, const key_def* def) {
    switch(def->action) {
        case K_CHAR:
            osk_insert(d, d->shift != 0 ? def->upper : def->lower);
            if(d->shift == 1) {
                d->shift = 0;
            }

            return true;
        case K_SHIFT: d->shift = (d->shift + 1) % 3; return false;
        case K_BACK: osk_backspace(d); return true;
        case K_PAGE_LETTERS: d->page = 0; osk_build_keys(d); return false;
        case K_PAGE_SYMBOLS: d->page = 1; osk_build_keys(d); return false;
        case K_PAGE_EXTRA: d->page = 2; osk_build_keys(d); return false;
        case K_SPACE: osk_insert(d, " "); return true;
        case K_LEFT: d->cursor = osk_prev_char(d, d->cursor); return true;
        case K_RIGHT: d->cursor = osk_next_char(d, d->cursor); return true;
        case K_CLEAR: d->text[0] = '\0'; d->len = 0; d->cursor = 0; return false;
        case K_ENTER: osk_insert(d, "\n"); return true;
        case K_OK: osk_finish(view, d, true); return false;
        case K_CANCEL: osk_finish(view, d, false); return false;
        default: break;   // The text editor's keys (keys.h).
    }

    return false;
}

static void osk_update(ui_view* view, void* v, float bx1, float by1, float bx2, float by2) {
    osk_data* d = (osk_data*) v;

    view->info = _(" ok    delete    space    cancel");

    if(hidKeysDown() & KEY_B) {
        osk_finish(view, d, false);
        return;
    }

    if(hidKeysDown() & (KEY_A | KEY_START)) {
        osk_finish(view, d, true);
        return;
    }

    if(keys_hold_repeat(&d->holdAt[0], KEY_DLEFT)) d->cursor = osk_prev_char(d, d->cursor);
    if(keys_hold_repeat(&d->holdAt[1], KEY_DRIGHT)) d->cursor = osk_next_char(d, d->cursor);
    if(keys_hold_repeat(&d->holdAt[2], KEY_X)) osk_backspace(d);
    if(keys_hold_repeat(&d->holdAt[3], KEY_Y)) osk_insert(d, " ");
    if(hidKeysDown() & KEY_SELECT) d->shift = (d->shift + 1) % 3;

    touchPosition pos;
    hidTouchRead(&pos);

    if(hidKeysDown() & KEY_TOUCH) {
        d->pressedKey = keys_hit(d->keys, d->keyCount, &pos);

        if(d->pressedKey >= 0) {
            const key_def* def = d->keys[d->pressedKey].def;
            key_action action = def->action;
            bool repeats = osk_run_key(view, d, def);
            if(action == K_OK || action == K_CANCEL) {
                // The keyboard is gone (and its data freed): touch nothing of it.
                return;
            }

            d->repeatAt = repeats ? osGetTime() + 450 : 0;
            // The keys may have been rebuilt (page change): no stale press.
            if(!repeats && action != K_SHIFT) {
                d->pressedKey = -1;
            }
        }
    } else if(hidKeysHeld() & KEY_TOUCH) {
        if(keys_touch_repeat(d->keys, d->pressedKey, &d->repeatAt, &pos)) {
            osk_run_key(view, d, d->keys[d->pressedKey].def);
        }
    } else if(hidKeysUp() & KEY_TOUCH) {
        d->pressedKey = -1;
        d->repeatAt = 0;
    }
}

ui_view* osk_display(const char* hint, const char* initialText, u32 maxSize, u32 flags, void* data, void (*onResponse)(ui_view* view, void* data, SwkbdButton button, const char* response)) {
    osk_data* d = (osk_data*) calloc(1, sizeof(osk_data));
    if(d == NULL) {
        error_display(NULL, NULL, "Failed to allocate keyboard data.");
        return NULL;
    }

    d->maxSize = maxSize < 2 ? 2 : maxSize;
    d->flags = flags;
    d->text = (char*) calloc(1, d->maxSize);
    if(d->text == NULL) {
        free(d);
        error_display(NULL, NULL, "Failed to allocate keyboard data.");
        return NULL;
    }

    d->hint = i18n_translate_text(hint);
    if(initialText != NULL) {
        strncpy(d->text, initialText, d->maxSize - 1);
    }

    d->len = (u32) strlen(d->text);
    d->cursor = d->len;
    d->pressedKey = -1;
    d->data = data;
    d->onResponse = onResponse;
    osk_build_keys(d);

    ui_view* view = ui_create();
    view->name = d->hint;
    view->info = "";
    view->data = d;
    view->update = osk_update;
    view->drawTop = osk_draw_top;
    view->drawBottom = osk_draw_bottom;
    ui_push(view);
    return view;
}
