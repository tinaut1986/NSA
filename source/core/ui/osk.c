// NSA's own on-screen keyboard for a line of text (the same keys as the text editor's). The text is on the top screen and the
// keys on the bottom one. D-Pad left/right move the cursor, X deletes, Y types a space, A or START accepts, B cancels.

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "osk.h"
#include "../core.h"

typedef enum {
    K_CHAR,
    K_SHIFT,
    K_BACK,
    K_PAGE_LETTERS,
    K_PAGE_SYMBOLS,
    K_PAGE_EXTRA,
    K_SPACE,
    K_OK,
    K_CANCEL,
    K_LEFT,
    K_RIGHT,
    K_CLEAR
} key_action;

typedef struct {
    const char* lower;
    const char* upper;
    float width;
    key_action action;
} key_def;

#define KC(l, u) {l, u, 1, K_CHAR}
#define KS(s) {s, s, 1, K_CHAR}
#define KA(act, w) {"", "", w, act}

#define KB_ROW_MAX 11

typedef struct {
    u32 count;
    key_def keys[KB_ROW_MAX];
} key_row;

static const key_row osk_toolbar = {5, {KA(K_CANCEL, 3), KA(K_CLEAR, 1), KA(K_LEFT, 1), KA(K_RIGHT, 1), KA(K_OK, 4)}};

static const key_row osk_letters[4] = {
        {10, {KC("q", "Q"), KC("w", "W"), KC("e", "E"), KC("r", "R"), KC("t", "T"), KC("y", "Y"), KC("u", "U"), KC("i", "I"), KC("o", "O"), KC("p", "P")}},
        {10, {KC("a", "A"), KC("s", "S"), KC("d", "D"), KC("f", "F"), KC("g", "G"), KC("h", "H"), KC("j", "J"), KC("k", "K"), KC("l", "L"), KC("ñ", "Ñ")}},
        {9, {KA(K_SHIFT, 1.5f), KC("z", "Z"), KC("x", "X"), KC("c", "C"), KC("v", "V"), KC("b", "B"), KC("n", "N"), KC("m", "M"), KA(K_BACK, 1.5f)}},
        {5, {KA(K_PAGE_SYMBOLS, 2), KS(","), KA(K_SPACE, 4), KS("."), KA(K_OK, 2)}}
};

static const key_row osk_symbols[4] = {
        {10, {KS("1"), KS("2"), KS("3"), KS("4"), KS("5"), KS("6"), KS("7"), KS("8"), KS("9"), KS("0")}},
        {10, {KS("@"), KS("#"), KS("$"), KS("%"), KS("&"), KS("*"), KS("-"), KS("+"), KS("("), KS(")")}},
        {9, {KA(K_PAGE_EXTRA, 1.5f), KS("!"), KS("\""), KS("'"), KS(":"), KS(";"), KS("/"), KS("?"), KA(K_BACK, 1.5f)}},
        {5, {KA(K_PAGE_LETTERS, 2), KS(","), KA(K_SPACE, 4), KS("."), KA(K_OK, 2)}}
};

static const key_row osk_extra[4] = {
        {10, {KC("á", "Á"), KC("é", "É"), KC("í", "Í"), KC("ó", "Ó"), KC("ú", "Ú"), KC("ü", "Ü"), KC("ñ", "Ñ"), KS("¿"), KS("¡"), KS("€")}},
        {10, {KS("="), KS("<"), KS(">"), KS("["), KS("]"), KS("{"), KS("}"), KS("\\"), KS("|"), KS("~")}},
        {9, {KA(K_PAGE_SYMBOLS, 1.5f), KS("_"), KS("^"), KS("`"), KC("ç", "Ç"), KS("°"), KS("§"), KS("´"), KA(K_BACK, 1.5f)}},
        {5, {KA(K_PAGE_LETTERS, 2), KS(","), KA(K_SPACE, 4), KS("."), KA(K_OK, 2)}}
};

#define OSK_KEYS_MAX 48

typedef struct {
    float x, y, w, h;
    const key_def* def;
} key_rect;

typedef struct {
    const char* hint;
    char* text;
    u32 len;
    u32 cursor;
    u32 maxSize;   // With the final NUL.

    u32 page;
    u32 shift;     // 0 off, 1 next letter, 2 caps lock.
    key_rect keys[OSK_KEYS_MAX];
    u32 keyCount;
    int pressedKey;
    u64 repeatAt;
    u64 holdAt[2];

    void* data;
    void (*onResponse)(ui_view* view, void* data, SwkbdButton button, const char* response);
    bool finished;
} osk_data;

static void osk_build_keys(osk_data* d) {
    const key_row* rows = d->page == 0 ? osk_letters : d->page == 1 ? osk_symbols : osk_extra;
    float unit = (BOTTOM_SCREEN_WIDTH - 8) / 10.0f;
    float y = WIDGET_TITLE_BAR_HEIGHT + 4;
    d->keyCount = 0;

    for(int r = -1; r < 4; r++) {
        const key_row* row = r < 0 ? &osk_toolbar : &rows[r];
        float h = r < 0 ? 28 : 34;
        float x = 4;
        for(u32 i = 0; i < row->count && d->keyCount < OSK_KEYS_MAX; i++) {
            const key_def* def = &row->keys[i];
            float w = def->width * unit;
            d->keys[d->keyCount++] = (key_rect) {x + 1, y, w - 2, h, def};
            x += w;
        }

        y += h + 3;
    }
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

    // When the text is wider than the box, the part before the cursor that does not fit is left out.
    float room = boxW - 20;
    u32 start = 0;
    while(start < d->cursor) {
        char saved = d->text[d->cursor];
        d->text[d->cursor] = '\0';
        float w = widget_text_width(d->text + start, WIDGET_TEXT_SCALE);
        d->text[d->cursor] = saved;
        if(w <= room - 6) {
            break;
        }

        start = osk_next_char(d, start);
    }

    float textY = boxY + (boxH - widget_text_height(WIDGET_TEXT_SCALE)) / 2;
    if(d->len > start) {
        widget_text_fit(d->text + start, boxX + 10, textY, WIDGET_TEXT_SCALE, t->palette->text, room);
    }

    if((osGetTime() / 500) % 2 == 0) {
        char saved = d->text[d->cursor];
        d->text[d->cursor] = '\0';
        float cx = boxX + 10 + widget_text_width(d->text + start, WIDGET_TEXT_SCALE);
        d->text[d->cursor] = saved;
        screen_fill_rect(cx, boxY + 6, 1.5f, boxH - 12, t->accent);
    }

    char count[24];
    snprintf(count, sizeof(count), "%lu/%lu", d->len, d->maxSize - 1);
    widget_text_centered(count, boxX + boxW - 70, boxX + boxW, boxY + boxH + 4, WIDGET_SMALL_SCALE, t->palette->textMuted);
    ui_wake();
}

static void osk_draw_key(osk_data* d, const key_rect* k, bool pressed) {
    const theme_current* t = theme();
    bool special = k->def->action != K_CHAR;
    bool ok = k->def->action == K_OK;
    u32 fill = pressed ? t->accent : ok ? t->accentSoft : special ? t->palette->background : t->palette->surface;
    u32 edge = pressed ? t->accent : t->palette->border;
    u32 ink = pressed ? t->onAccent : ok ? t->accent : t->palette->text;
    screen_frame_round_rect(k->x, k->y, k->w, k->h, t->def->radiusSmall, t->def->border, edge, fill);

    int icon = -1;
    const char* label = NULL;
    switch(k->def->action) {
        case K_CHAR: label = d->shift != 0 ? k->def->upper : k->def->lower; break;
        case K_SHIFT: icon = ICON_CHEVRON_UP; if(d->shift == 2) { ink = pressed ? t->onAccent : t->accent; } break;
        case K_BACK: icon = ICON_ARROW_LEFT; break;
        case K_PAGE_LETTERS: label = "abc"; break;
        case K_PAGE_SYMBOLS: label = "123"; break;
        case K_PAGE_EXTRA: label = "áé="; break;
        case K_SPACE: break;
        case K_OK: icon = ICON_CHECK; if(k->w > 80) { label = _("OK"); } break;
        case K_CANCEL: icon = ICON_X; label = _("Cancel"); break;
        case K_LEFT: icon = ICON_CHEVRON_LEFT; break;
        case K_RIGHT: icon = ICON_CHEVRON_RIGHT; break;
        case K_CLEAR: icon = ICON_TRASH; break;
    }

    if(icon >= 0 && label != NULL) {
        float tw = widget_text_width(label, WIDGET_TEXT_SCALE);
        float total = 16 + 6 + tw;
        float x = k->x + (k->w - total) / 2;
        screen_draw_icon((u32) icon, x, k->y + (k->h - 16) / 2, 16, ink);
        screen_draw_text(label, x + 22, k->y + (k->h - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink);
    } else if(icon >= 0) {
        screen_draw_icon((u32) icon, k->x + (k->w - 16) / 2, k->y + (k->h - 16) / 2, 16, ink);
    } else if(label != NULL) {
        widget_text_centered(label, k->x, k->x + k->w, k->y + (k->h - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink);
    }
}

static void osk_draw_bottom(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    osk_data* d = (osk_data*) v;
    for(u32 i = 0; i < d->keyCount; i++) {
        osk_draw_key(d, &d->keys[i], d->pressedKey == (int) i);
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
        case K_OK: osk_finish(view, d, true); return false;
        case K_CANCEL: osk_finish(view, d, false); return false;
    }

    return false;
}

static bool osk_repeat(osk_data* d, int slot, u32 mask) {
    if(hidKeysDown() & mask) {
        d->holdAt[slot] = osGetTime() + 400;
        return true;
    }

    if((hidKeysHeld() & mask) && d->holdAt[slot] != 0 && osGetTime() >= d->holdAt[slot]) {
        d->holdAt[slot] = osGetTime() + 60;
        return true;
    }

    if(!(hidKeysHeld() & mask)) {
        d->holdAt[slot] = 0;
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

    if(osk_repeat(d, 0, KEY_DLEFT)) d->cursor = osk_prev_char(d, d->cursor);
    if(osk_repeat(d, 1, KEY_DRIGHT)) d->cursor = osk_next_char(d, d->cursor);
    if(hidKeysDown() & KEY_X) osk_backspace(d);
    if(hidKeysDown() & KEY_Y) osk_insert(d, " ");
    if(hidKeysDown() & KEY_SELECT) d->shift = (d->shift + 1) % 3;

    touchPosition pos;
    hidTouchRead(&pos);

    if(hidKeysDown() & KEY_TOUCH) {
        d->pressedKey = -1;
        for(u32 i = 0; i < d->keyCount; i++) {
            const key_rect* k = &d->keys[i];
            if(widget_touched(&pos, k->x, k->y, k->w, k->h)) {
                d->pressedKey = (int) i;
                break;
            }
        }

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
        if(d->pressedKey >= 0 && d->repeatAt != 0 && osGetTime() >= d->repeatAt) {
            const key_rect* k = &d->keys[d->pressedKey];
            if(widget_touched(&pos, k->x, k->y, k->w, k->h)) {
                osk_run_key(view, d, k->def);
                d->repeatAt = osGetTime() + 60;
            }
        }
    } else if(hidKeysUp() & KEY_TOUCH) {
        d->pressedKey = -1;
        d->repeatAt = 0;
    }
}

ui_view* osk_display(const char* hint, const char* initialText, u32 maxSize, void* data, void (*onResponse)(ui_view* view, void* data, SwkbdButton button, const char* response)) {
    osk_data* d = (osk_data*) calloc(1, sizeof(osk_data));
    if(d == NULL) {
        error_display(NULL, NULL, "Failed to allocate keyboard data.");
        return NULL;
    }

    d->maxSize = maxSize < 2 ? 2 : maxSize;
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
