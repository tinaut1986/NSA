#include <3ds.h>

#include "keys.h"
#include "widgets.h"
#include "../i18n.h"
#include "../icons.h"
#include "../screen.h"
#include "../theme.h"

static const key_row keys_letters[4] = {
        {10, {KC("q", "Q"), KC("w", "W"), KC("e", "E"), KC("r", "R"), KC("t", "T"), KC("y", "Y"), KC("u", "U"), KC("i", "I"), KC("o", "O"), KC("p", "P")}},
        {10, {KC("a", "A"), KC("s", "S"), KC("d", "D"), KC("f", "F"), KC("g", "G"), KC("h", "H"), KC("j", "J"), KC("k", "K"), KC("l", "L"), KC("ñ", "Ñ")}},
        {9, {KA(K_SHIFT, 1.5f), KC("z", "Z"), KC("x", "X"), KC("c", "C"), KC("v", "V"), KC("b", "B"), KC("n", "N"), KC("m", "M"), KA(K_BACK, 1.5f)}},
        {5, {KA(K_PAGE_SYMBOLS, 2), KS(","), KA(K_SPACE, 4), KS("."), KA(K_OK, 2)}}
};

static const key_row keys_symbols[4] = {
        {10, {KS("1"), KS("2"), KS("3"), KS("4"), KS("5"), KS("6"), KS("7"), KS("8"), KS("9"), KS("0")}},
        {10, {KS("@"), KS("#"), KS("$"), KS("%"), KS("&"), KS("*"), KS("-"), KS("+"), KS("("), KS(")")}},
        {9, {KA(K_PAGE_EXTRA, 1.5f), KS("!"), KS("\""), KS("'"), KS(":"), KS(";"), KS("/"), KS("?"), KA(K_BACK, 1.5f)}},
        {5, {KA(K_PAGE_LETTERS, 2), KS(","), KA(K_SPACE, 4), KS("."), KA(K_OK, 2)}}
};

static const key_row keys_extra[4] = {
        {10, {KC("á", "Á"), KC("é", "É"), KC("í", "Í"), KC("ó", "Ó"), KC("ú", "Ú"), KC("ü", "Ü"), KC("ñ", "Ñ"), KS("¿"), KS("¡"), KS("€")}},
        {10, {KS("="), KS("<"), KS(">"), KS("["), KS("]"), KS("{"), KS("}"), KS("\\"), KS("|"), KS("~")}},
        {9, {KA(K_PAGE_SYMBOLS, 1.5f), KS("_"), KS("^"), KS("`"), KC("ç", "Ç"), KS("°"), KS("§"), KS("´"), KA(K_BACK, 1.5f)}},
        {5, {KA(K_PAGE_LETTERS, 2), KS(","), KA(K_SPACE, 4), KS("."), KA(K_OK, 2)}}
};

// The bottom row's right key when it is not OK.
static const key_def keys_enter = KA(K_ENTER, 2);

u32 keys_build(key_rect* out, u32 max, const key_row* toolbar, u32 page, key_action confirm) {
    const key_row* rows = page == 0 ? keys_letters : page == 1 ? keys_symbols : keys_extra;
    float unit = (BOTTOM_SCREEN_WIDTH - 8) / 10.0f;
    float y = WIDGET_TITLE_BAR_HEIGHT + 4;
    u32 count = 0;

    for(int r = -1; r < 4; r++) {
        const key_row* row = r < 0 ? toolbar : &rows[r];
        float h = r < 0 ? 28 : 34;
        float x = 4;
        for(u32 i = 0; i < row->count && count < max; i++) {
            const key_def* def = confirm == K_ENTER && r == 3 && row->keys[i].action == K_OK ? &keys_enter : &row->keys[i];
            float w = def->width * unit;
            out[count++] = (key_rect) {x + 1, y, w - 2, h, def};
            x += w;
        }

        y += h + 3;
    }

    return count;
}

void keys_draw(const key_rect* k, bool pressed, u32 shift) {
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
        case K_CHAR: label = shift != 0 ? k->def->upper : k->def->lower; break;
        case K_SHIFT: icon = ICON_CHEVRON_UP; if(shift == 2) { ink = pressed ? t->onAccent : t->accent; } break;
        case K_BACK: icon = ICON_ARROW_LEFT; break;
        case K_PAGE_LETTERS: label = "abc"; break;
        case K_PAGE_SYMBOLS: label = "123"; break;
        case K_PAGE_EXTRA: label = "áé="; break;
        case K_SPACE: break;
        case K_ENTER: icon = ICON_ARROW_BACK_UP; break;
        case K_OK: icon = ICON_CHECK; if(k->w > 80) { label = _("OK"); } break;
        case K_CANCEL: icon = ICON_X; label = _("Cancel"); break;
        case K_LEFT: icon = ICON_CHEVRON_LEFT; break;
        case K_RIGHT: icon = ICON_CHEVRON_RIGHT; break;
        case K_UP: icon = ICON_CHEVRON_UP; break;
        case K_DOWN: icon = ICON_CHEVRON_DOWN; break;
        case K_CLEAR: icon = ICON_TRASH; break;
        case K_SAVE: icon = ICON_DEVICE_FLOPPY; label = _("Save"); break;
        case K_DONE: icon = ICON_CHECK; label = _("Done"); break;
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

int keys_hit(const key_rect* keys, u32 count, const touchPosition* pos) {
    for(u32 i = 0; i < count; i++) {
        const key_rect* k = &keys[i];
        if(widget_touched(pos, k->x, k->y, k->w, k->h)) {
            return (int) i;
        }
    }

    return -1;
}

bool keys_touch_repeat(const key_rect* keys, int pressed, u64* repeatAt, const touchPosition* pos) {
    if(pressed < 0 || *repeatAt == 0 || osGetTime() < *repeatAt) {
        return false;
    }

    const key_rect* k = &keys[pressed];
    if(!widget_touched(pos, k->x, k->y, k->w, k->h)) {
        return false;
    }

    *repeatAt = osGetTime() + 60;
    return true;
}

bool keys_hold_repeat(u64* holdAt, u32 mask) {
    if(hidKeysDown() & mask) {
        *holdAt = osGetTime() + 400;
        return true;
    }

    if((hidKeysHeld() & mask) && *holdAt != 0 && osGetTime() >= *holdAt) {
        *holdAt = osGetTime() + 60;
        return true;
    }

    if(!(hidKeysHeld() & mask)) {
        *holdAt = 0;
    }

    return false;
}
