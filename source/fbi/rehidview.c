#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "rehid.h"
#include "resources.h"
#include "section.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "../core/xferturn.h"

// Utilities -> Button remapping: Rehid (rehid.h) and its remappings. The list shows Rehid's state (install, update,
// turn on or off, what to do when the buttons stop working), the global remapping and the games that have one of their
// own, and adds a game. A remapping opens in an editor: one row per entry, each kind with its "Add" row, the switches and
// the HOME combo; every change is saved at once (Rehid reads the file when the game starts).
//
// Buttons are taken by pressing them: a screen waits for keys to go down and finishes when they are all let go, so any
// key (B too) can be taken, and leaving it is a tap on its button. NSA's own title keeps a "{}" file (rehid_protect_nsa),
// so the keys NSA reads are the console's, whatever the global remapping does.

#define G_A "\xEE\x80\x80"
#define G_B "\xEE\x80\x81"
#define G_X "\xEE\x80\x82"
#define G_Y "\xEE\x80\x83"

#define ROW_HEIGHT 34
#define COUNT_OF(a) (sizeof(a) / sizeof(*(a)))

// Set when a remapping changed, so the list reads the files again when it shows.
static bool rehidview_dirty = false;

// ---- the console, drawn with keys lit (the top screens) ----

// Keys the player presses in the accent, the keys the game gets in green, both: the accent.
static void rv_key(u32 press, u32 get, u32 key, float x, float y, float w, float h, float radius, const char* label) {
    const theme_current* t = theme();
    u32 fill = t->palette->surface;
    u32 ink = t->palette->text;
    u32 edge = t->palette->border;
    if(press & key) {
        fill = t->accent;
        ink = t->onAccent;
        edge = t->accent;
    } else if(get & key) {
        fill = t->palette->successSoft;
        ink = t->palette->success;
        edge = t->palette->success;
    }

    screen_frame_round_rect(x, y, w, h, radius, 1, edge, fill);
    if(label != NULL) {
        float tw = widget_text_width(label, WIDGET_SMALL_SCALE);
        widget_text_fit(label, x + (w - tw > 8 ? (w - tw) / 2 : 4), y + (h - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, ink, w - 8);
    }
}

static void rv_stick(u32 press, u32 get, float cx, float cy, float r, u32 up, u32 down, u32 left, u32 right, const char* label) {
    const theme_current* t = theme();
    screen_fill_circle(cx, cy, r + 1, t->palette->border);
    screen_fill_circle(cx, cy, r, t->palette->surface);
    const struct {
        u32 key;
        float dx;
        float dy;
    } marks[] = {{up, 0, -1}, {down, 0, 1}, {left, -1, 0}, {right, 1, 0}};
    for(u32 i = 0; i < COUNT_OF(marks); i++) {
        float mx = cx + marks[i].dx * (r - 6);
        float my = cy + marks[i].dy * (r - 6);
        u32 color = (press & marks[i].key) ? t->accent : (get & marks[i].key) ? t->palette->success : t->palette->border;
        screen_fill_circle(mx, my, (press | get) & marks[i].key ? (r > 14 ? 5 : 4) : 2.5f, color);
    }

    widget_text_centered(label, cx - r - 30, cx + r + 30, cy + r + 3, WIDGET_SMALL_SCALE, t->palette->textMuted);
}

// The console from the front, in the area x1..x2 from y: 128 pixels high.
static void rv_draw_console(u32 press, u32 get, float x1, float y, float x2) {
    const theme_current* t = theme();
    float mid = (x1 + x2) / 2;

    rv_key(press, get, KEY_L, x1 + 8, y, 64, 18, 8, "L");
    rv_key(press, get, KEY_ZL, x1 + 76, y, 40, 18, 8, "ZL");
    rv_key(press, get, KEY_R, x2 - 72, y, 64, 18, 8, "R");
    rv_key(press, get, KEY_ZR, x2 - 116, y, 40, 18, 8, "ZR");

    float lx = x1 + 52;
    rv_stick(press, get, lx, y + 44, 20, KEY_CPAD_UP, KEY_CPAD_DOWN, KEY_CPAD_LEFT, KEY_CPAD_RIGHT, _("Circle Pad"));
    float dpy = y + 106;
    float arm = 14;
    rv_key(press, get, KEY_DUP, lx - arm / 2, dpy - arm * 1.5f, arm, arm, 3, NULL);
    rv_key(press, get, KEY_DDOWN, lx - arm / 2, dpy + arm / 2, arm, arm, 3, NULL);
    rv_key(press, get, KEY_DLEFT, lx - arm * 1.5f, dpy - arm / 2, arm, arm, 3, NULL);
    rv_key(press, get, KEY_DRIGHT, lx + arm / 2, dpy - arm / 2, arm, arm, 3, NULL);
    screen_fill_rect(lx - arm / 2 + 1, dpy - arm / 2, arm - 2, arm, t->palette->border);

    float rx = x2 - 52;
    rv_stick(press, get, rx, y + 36, 12, KEY_CSTICK_UP, KEY_CSTICK_DOWN, KEY_CSTICK_LEFT, KEY_CSTICK_RIGHT, _("C-Stick"));
    float fy = y + 100;
    float fr = 10;
    float gap = 17;
    rv_key(press, get, KEY_X, rx - fr, fy - gap - fr, fr * 2, fr * 2, fr, "X");
    rv_key(press, get, KEY_B, rx - fr, fy + gap - fr, fr * 2, fr * 2, fr, "B");
    rv_key(press, get, KEY_Y, rx - gap - fr, fy - fr, fr * 2, fr * 2, fr, "Y");
    rv_key(press, get, KEY_A, rx + gap - fr, fy - fr, fr * 2, fr * 2, fr, "A");

    rv_key(press, get, KEY_SELECT, mid - 62, y + 110, 58, 16, 8, "SELECT");
    rv_key(press, get, KEY_START, mid + 4, y + 110, 58, 16, 8, "START");
}

// A card with an icon, a title and a text, over the whole top screen (or the top `height` of it).
static float rv_draw_card(float x1, float y1, float x2, float height, u32 icon, const char* title, const char* text) {
    const theme_current* t = theme();
    float cx = x1 + 12;
    float cw = x2 - x1 - 24;
    widget_card(cx, y1 + 10, cw, height);
    screen_draw_icon(icon, cx + 12, y1 + 22, 16, t->accent);
    widget_text_fit(title, cx + 36, y1 + 22, WIDGET_TITLE_SCALE, t->palette->text, cw - 48);
    if(text != NULL) {
        screen_draw_text_wrap(text, cx + 12, y1 + 48, WIDGET_TEXT_SCALE, t->palette->textMuted, cx + cw - 12);
    }

    return y1 + 10 + height;
}

// ---- taking buttons ----

typedef struct {
    const char* title;     // English, translated when drawn.
    const char* text;
    u32 ignore;            // Held when the screen opened: taken only once let go and pressed again.
    u32 taken;
    u32 held;
    bool cancelPressed;
    void* data;
    void (*done)(void* data, u32 keys);   // 0: left without keys.
} rv_capture;

#define CAPTURE_CANCEL_W 120
#define CAPTURE_CANCEL_H 30

static void rv_capture_cancel_rect(float bx1, float by1, float bx2, float by2, float* x, float* y) {
    *x = (bx1 + bx2 - CAPTURE_CANCEL_W) / 2;
    *y = by2 - CAPTURE_CANCEL_H - 10;
}

static void rv_capture_finish(ui_view* view, rv_capture* c, u32 keys) {
    void* data = c->data;
    void (*done)(void* data, u32 keys) = c->done;
    ui_pop();
    ui_destroy(view);
    free(c);
    // Taken: a sheet this came from (an entry's) closes instead of showing again.
    if(keys != 0) {
        sheet_confirmed();
    }

    done(data, keys);
}

static void rv_capture_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    rv_capture* c = (rv_capture*) data;
    u32 held = hidKeysHeld() & REHID_KEYS_ALL;
    c->ignore &= held;
    held &= ~c->ignore;
    c->held = held;
    c->taken |= held;

    // Done when keys were pressed and are all let go.
    if(c->taken != 0 && held == 0) {
        rv_capture_finish(view, c, c->taken);
        return;
    }

    float x = 0;
    float y = 0;
    rv_capture_cancel_rect(bx1, by1, bx2, by2, &x, &y);
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        c->cancelPressed = widget_touched(&pos, x, y, CAPTURE_CANCEL_W, CAPTURE_CANCEL_H);
    } else if(c->cancelPressed && (hidKeysUp() & KEY_TOUCH)) {
        c->cancelPressed = false;
        if(c->taken == 0) {
            rv_capture_finish(view, c, 0);
        }
    }
}

static void rv_capture_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    rv_capture* c = (rv_capture*) data;
    rv_draw_console(c->taken, 0, x1, y1 + 30, x2);
    widget_text_centered(i18n_translate_text(c->title), x1, x2, y1 + 8, WIDGET_TEXT_SCALE, theme()->palette->text);
}

static void rv_capture_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    rv_capture* c = (rv_capture*) data;
    const theme_current* t = theme();
    screen_draw_text_wrap(i18n_translate_text(c->text), x1 + 14, y1 + 12, WIDGET_TEXT_SCALE, t->palette->text, x2 - 14);

    char keys[160];
    rehid_keys_label(c->taken, keys, sizeof(keys));
    if(keys[0] != '\0') {
        widget_text_centered(keys, x1, x2, y1 + 92, WIDGET_TITLE_SCALE, t->accent);
    }

    float bx = 0;
    float by = 0;
    rv_capture_cancel_rect(x1, y1, x2, y2, &bx, &by);
    widget_button_centered(bx, by, CAPTURE_CANCEL_W, CAPTURE_CANCEL_H, _("Cancel"), c->cancelPressed);
}

// Opens the screen that takes keys; `done` gets them (0 when cancelled), after it closed.
static void rv_capture_open(const char* title, const char* text, void* data, void (*done)(void* data, u32 keys)) {
    rv_capture* c = (rv_capture*) calloc(1, sizeof(rv_capture));
    if(c == NULL) {
        done(data, 0);
        return;
    }

    c->title = title;
    c->text = text;
    c->ignore = hidKeysHeld() & REHID_KEYS_ALL;
    c->data = data;
    c->done = done;

    ui_view* view = ui_create();
    view->name = _("Press the buttons");
    view->info = _("Let go of every button to finish");
    view->data = c;
    view->update = rv_capture_update;
    view->drawTop = rv_capture_draw_top;
    view->drawBottom = rv_capture_draw_bottom;
    ui_push(view);
}

// ---- a point or a box of the bottom screen ----

typedef struct {
    bool box;                 // A box (dragged) instead of a point (tapped).
    const char* text;
    bool touching;
    u16 startX;
    u16 startY;
    u16 nowX;
    u16 nowY;
    void* data;
    void (*done)(void* data, bool ok, u16 x, u16 y, u16 w, u16 h);
} rv_place;

// The game's 320x240 screen, drawn at 4:3 in the area: where it is.
static void rv_place_frame(float bx1, float by1, float bx2, float by2, float* x, float* y, float* w, float* h) {
    *h = by2 - by1 - 6;
    *w = *h * 4 / 3;
    *x = (bx1 + bx2 - *w) / 2;
    *y = by1 + 3;
}

static void rv_place_to_screen(float fx, float fy, float fw, float fh, const touchPosition* pos, u16* x, u16* y) {
    float px = (pos->px - fx) * 320 / fw;
    float py = (pos->py - fy) * 240 / fh;
    *x = (u16) (px < 1 ? 1 : px > 319 ? 319 : px);
    *y = (u16) (py < 1 ? 1 : py > 239 ? 239 : py);
}

static void rv_place_finish(ui_view* view, rv_place* p, bool ok) {
    u16 x = p->box ? (p->startX < p->nowX ? p->startX : p->nowX) : p->nowX;
    u16 y = p->box ? (p->startY < p->nowY ? p->startY : p->nowY) : p->nowY;
    u16 w = p->box ? (u16) abs((int) p->nowX - (int) p->startX) : 0;
    u16 h = p->box ? (u16) abs((int) p->nowY - (int) p->startY) : 0;
    void* data = p->data;
    void (*done)(void* data, bool ok, u16 x, u16 y, u16 w, u16 h) = p->done;
    ui_pop();
    ui_destroy(view);
    free(p);
    if(ok) {
        sheet_confirmed();
    }

    done(data, ok, x, y, w, h);
}

static void rv_place_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    rv_place* p = (rv_place*) data;
    if(hidKeysDown() & KEY_B) {
        rv_place_finish(view, p, false);
        return;
    }

    float fx, fy, fw, fh;
    rv_place_frame(bx1, by1, bx2, by2, &fx, &fy, &fw, &fh);
    touchPosition pos;
    hidTouchRead(&pos);
    if((hidKeysDown() & KEY_TOUCH) && widget_touched(&pos, fx, fy, fw, fh)) {
        p->touching = true;
        rv_place_to_screen(fx, fy, fw, fh, &pos, &p->startX, &p->startY);
        p->nowX = p->startX;
        p->nowY = p->startY;
    } else if(p->touching && (hidKeysHeld() & KEY_TOUCH)) {
        rv_place_to_screen(fx, fy, fw, fh, &pos, &p->nowX, &p->nowY);
    } else if(p->touching && !(hidKeysHeld() & KEY_TOUCH)) {
        p->touching = false;
        // A box needs some size: a mere tap starts again.
        if(!p->box || (abs((int) p->nowX - (int) p->startX) >= 8 && abs((int) p->nowY - (int) p->startY) >= 8)) {
            rv_place_finish(view, p, true);
        }
    }
}

static void rv_place_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    rv_place* p = (rv_place*) data;
    rv_draw_card(x1, y1, x2, y2 - y1 - 20, p->box ? ICON_SQUARE : ICON_DEVICE_MOBILE, p->box ? _("Draw the zone") : _("Tap the point"), i18n_translate_text(p->text));
}

static void rv_place_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    rv_place* p = (rv_place*) data;
    const theme_current* t = theme();
    float fx, fy, fw, fh;
    rv_place_frame(x1, y1, x2, y2, &fx, &fy, &fw, &fh);
    screen_frame_round_rect(fx, fy, fw, fh, 4, 1, t->palette->border, t->palette->surface);
    for(u32 i = 1; i < 8; i++) {
        screen_fill_rect(fx + fw * i / 8, fy + 1, 1, fh - 2, THEME_ALPHA(t->palette->border, 0x60));
    }

    for(u32 i = 1; i < 6; i++) {
        screen_fill_rect(fx + 1, fy + fh * i / 6, fw - 2, 1, THEME_ALPHA(t->palette->border, 0x60));
    }

    if(!p->touching) {
        return;
    }

    float sx = fx + p->startX * fw / 320;
    float sy = fy + p->startY * fh / 240;
    float nx = fx + p->nowX * fw / 320;
    float ny = fy + p->nowY * fh / 240;
    if(p->box) {
        float bx = sx < nx ? sx : nx;
        float by = sy < ny ? sy : ny;
        screen_frame_round_rect(bx, by, (sx < nx ? nx - sx : sx - nx) + 1, (sy < ny ? ny - sy : sy - ny) + 1, 2, 1, t->accent, THEME_ALPHA(t->accent, 0x50));
    } else {
        screen_fill_circle(nx, ny, 5, t->accent);
    }

    char where[48];
    snprintf(where, sizeof(where), "%u, %u", p->nowX, p->nowY);
    widget_badge(fx + 4, fy + 4, where, t->palette->surface, t->palette->text);
}

static void rv_place_open(bool box, const char* text, void* data, void (*done)(void* data, bool ok, u16 x, u16 y, u16 w, u16 h)) {
    rv_place* p = (rv_place*) calloc(1, sizeof(rv_place));
    if(p == NULL) {
        done(data, false, 0, 0, 0, 0);
        return;
    }

    p->box = box;
    p->text = text;
    p->data = data;
    p->done = done;

    ui_view* view = ui_create();
    view->name = box ? _("Touch zone") : _("Point on the screen");
    view->info = _(G_B " cancel");
    view->backArrow = true;
    view->data = p;
    view->update = rv_place_update;
    view->drawTop = rv_place_draw_top;
    view->drawBottom = rv_place_draw_bottom;
    ui_push(view);
}

// ---- the editor of one remapping ----

typedef enum {
    ED_KEY,
    ED_ADD_KEY,
    ED_TURBO,
    ED_ADD_TURBO,
    ED_TOUCH,
    ED_ADD_TOUCH,
    ED_BOX,
    ED_ADD_BOX,
    ED_CPAD,
    ED_ADD_CPAD,
    ED_CPAD_TO_DPAD,
    ED_DPAD_TO_CPAD,
    ED_HOME,
    ED_MORE
} ed_kind;

typedef struct {
    ed_kind kind;
    u32 index;
} ed_row;

typedef struct {
    u64 titleId;              // 0: the global remapping.
    char name[128];           // The game's, or "Every game".
    rehid_config config;
    bool fileExists;          // The title has a file of its own (it does not use the global remapping).
    bool built;
    ed_kind actionKind;
    u32 actionIndex;
    u32 pending;              // Keys taken for an entry still being made.
    rehid_box pendingBox;     // A zone drawn, waiting for its button.
    ui_view* view;
} ed_data;

static void rv_open_named(u64 titleId, const char* name);

static const char* ed_cpad_names[8] = {"Up", "Down", "Left", "Right", "Up and left", "Up and right", "Down and left", "Down and right"};
static const s16 ed_cpad_xy[8][2] = {{0, 155}, {0, -155}, {-155, 0}, {155, 0}, {-110, 110}, {110, 110}, {-110, -110}, {110, -110}};

static const char* ed_cpad_direction(s16 x, s16 y) {
    for(u32 i = 0; i < 8; i++) {
        if(ed_cpad_xy[i][0] == x && ed_cpad_xy[i][1] == y) {
            return i18n_translate_text(ed_cpad_names[i]);
        }
    }

    return NULL;
}

static void ed_save(ed_data* ed) {
    rehid_protect_nsa();
    Result res = rehid_config_save(ed->titleId, &ed->config);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to save the remapping in /rehid."));
    } else {
        ed->fileExists = true;
    }

    rehidview_dirty = true;
    ed->built = false;
}

static void ed_row_text(ed_data* ed, ed_kind kind, u32 index, char* out, size_t size) {
    char a[160];
    char b[160];
    const rehid_config* c = &ed->config;
    switch(kind) {
        case ED_KEY:
            rehid_keys_label(c->keys[index].press, a, sizeof(a));
            rehid_keys_label(c->keys[index].get, b, sizeof(b));
            snprintf(out, size, "%s  >  %s", a, b);
            break;
        case ED_TURBO:
            rehid_keys_label(c->turbo[index].press, a, sizeof(a));
            rehid_keys_label(c->turbo[index].get, b, sizeof(b));
            snprintf(out, size, i18n_translate_text("%s: turbo on %s"), a, b);
            break;
        case ED_TOUCH:
            rehid_keys_label(c->touch[index].press, a, sizeof(a));
            snprintf(out, size, i18n_translate_text("%s  >  touch at %d, %d"), a, c->touch[index].x, c->touch[index].y);
            break;
        case ED_BOX:
            rehid_keys_label(c->boxes[index].get, b, sizeof(b));
            snprintf(out, size, i18n_translate_text("Zone %u, %u (%u x %u)  >  %s"), c->boxes[index].x, c->boxes[index].y, c->boxes[index].w, c->boxes[index].h, b);
            break;
        case ED_CPAD: {
            rehid_keys_label(c->cpad[index].press, a, sizeof(a));
            const char* direction = ed_cpad_direction(c->cpad[index].x, c->cpad[index].y);
            if(direction != NULL) {
                snprintf(out, size, i18n_translate_text("%s  >  Circle Pad %s"), a, direction);
            } else {
                snprintf(out, size, i18n_translate_text("%s  >  Circle Pad at %d, %d"), a, c->cpad[index].x, c->cpad[index].y);
            }

            break;
        }
        case ED_ADD_KEY: string_copy(out, _("Add a button"), size); break;
        case ED_ADD_TURBO: string_copy(out, _("Add a turbo button"), size); break;
        case ED_ADD_TOUCH: string_copy(out, _("Add a button that touches the screen"), size); break;
        case ED_ADD_BOX: string_copy(out, _("Add a touch zone that presses a button"), size); break;
        case ED_ADD_CPAD: string_copy(out, _("Add a button that moves the Circle Pad"), size); break;
        case ED_CPAD_TO_DPAD: string_copy(out, _("Circle Pad as D-Pad"), size); break;
        case ED_DPAD_TO_CPAD: string_copy(out, _("D-Pad as Circle Pad"), size); break;
        case ED_HOME: string_copy(out, _("HOME button"), size); break;
        default: string_copy(out, _("More"), size); break;
    }
}

static void ed_add(linked_list* items, ed_data* ed, ed_kind kind, u32 index) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    ed_row* row = (ed_row*) calloc(1, sizeof(ed_row));
    if(item == NULL || row == NULL) {
        free(item);
        free(row);
        return;
    }

    row->kind = kind;
    row->index = index;
    item->data = row;
    item->color = COLOR_TEXT;
    ed_row_text(ed, kind, index, item->name, sizeof(item->name));
    linked_list_add(items, item);
}

static void ed_clear(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        free(item->data);
        free(item);
        linked_list_iter_remove(&iter);
    }
}

static void ed_build(ed_data* ed, linked_list* items) {
    // The selection stays on the same row index when the list is made again.
    ed_clear(items);
    const rehid_config* c = &ed->config;
    for(u32 i = 0; i < c->keyCount; i++) ed_add(items, ed, ED_KEY, i);
    if(c->keyCount < REHID_ENTRIES_MAX) ed_add(items, ed, ED_ADD_KEY, 0);
    for(u32 i = 0; i < c->turboCount; i++) ed_add(items, ed, ED_TURBO, i);
    if(c->turboCount < REHID_ENTRIES_MAX) ed_add(items, ed, ED_ADD_TURBO, 0);
    for(u32 i = 0; i < c->touchCount; i++) ed_add(items, ed, ED_TOUCH, i);
    if(c->touchCount < REHID_ENTRIES_MAX) ed_add(items, ed, ED_ADD_TOUCH, 0);
    for(u32 i = 0; i < c->boxCount; i++) ed_add(items, ed, ED_BOX, i);
    if(c->boxCount < REHID_ENTRIES_MAX) ed_add(items, ed, ED_ADD_BOX, 0);
    for(u32 i = 0; i < c->cpadCount; i++) ed_add(items, ed, ED_CPAD, i);
    if(c->cpadCount < REHID_ENTRIES_MAX) ed_add(items, ed, ED_ADD_CPAD, 0);
    ed_add(items, ed, ED_CPAD_TO_DPAD, 0);
    ed_add(items, ed, ED_DPAD_TO_CPAD, 0);
    ed_add(items, ed, ED_HOME, 0);
    ed_add(items, ed, ED_MORE, 0);
    ed->built = true;
}

static u32 ed_icon(ed_kind kind) {
    switch(kind) {
        case ED_KEY: return ICON_DEVICE_GAMEPAD_2;
        case ED_TURBO: return ICON_BOLT;
        case ED_TOUCH: return ICON_DEVICE_MOBILE;
        case ED_BOX: return ICON_SQUARE;
        case ED_CPAD: return ICON_ARROWS_MAXIMIZE;
        case ED_CPAD_TO_DPAD:
        case ED_DPAD_TO_CPAD: return ICON_ARROWS_SORT;
        case ED_HOME: return ICON_3DS;
        case ED_MORE: return ICON_DOTS;
        default: return ICON_PLUS;
    }
}

static void ed_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    ed_data* ed = (ed_data*) data;
    ed_row* row = (ed_row*) item->data;
    const theme_current* t = theme();
    u32 ink = 0;
    u32 muted = 0;
    widget_item_frame(x, y, width, height, selected, &ink, &muted);

    bool adding = row->kind == ED_ADD_KEY || row->kind == ED_ADD_TURBO || row->kind == ED_ADD_TOUCH || row->kind == ED_ADD_BOX || row->kind == ED_ADD_CPAD;
    screen_draw_icon(ed_icon(row->kind), x + 8, y + (height - 16) / 2, 16, adding && !selected ? t->accent : ink);

    const char* value = NULL;
    char home[160];
    if(row->kind == ED_CPAD_TO_DPAD) {
        value = ed->config.cpadToDpad ? _("On") : _("Off");
    } else if(row->kind == ED_DPAD_TO_CPAD) {
        value = ed->config.dpadToCpad ? _("On") : _("Off");
    } else if(row->kind == ED_HOME) {
        rehid_keys_label(ed->config.home, home, sizeof(home));
        value = ed->config.home != 0 ? home : _("None");
    }

    float right = x + width - 8;
    if(value != NULL) {
        float vw = widget_text_width(value, WIDGET_TEXT_SCALE);
        if(vw > width / 2) {
            vw = width / 2;
        }

        widget_text_fit(value, right - vw, y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, muted, vw);
        right -= vw + 8;
    }

    widget_text_fit(item->name, x + 32, y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, adding && !selected ? t->accent : ink, right - x - 32);
}

static const char* ed_help(ed_kind kind) {
    switch(kind) {
        case ED_KEY: return "Pressing the first buttons gives the game the second ones. Both can be several buttons together.";
        case ED_ADD_KEY: return "A button (or several together) that the game sees as other buttons: press the ones you will press, then the ones the game gets.";
        case ED_TURBO:
        case ED_ADD_TURBO: return "A button that turns a turbo on and off: while it is on, the game sees the other button pressed and let go again and again.";
        case ED_TOUCH:
        case ED_ADD_TOUCH: return "A button that touches the bottom screen at a point, for games that need the touch screen for something often.";
        case ED_BOX:
        case ED_ADD_BOX: return "Touching a zone of the bottom screen presses a button: the game does not see that touch.";
        case ED_CPAD:
        case ED_ADD_CPAD: return "A button that holds the Circle Pad in one direction.";
        case ED_CPAD_TO_DPAD: return "The Circle Pad works as the D-Pad (and the game does not see the Circle Pad move).";
        case ED_DPAD_TO_CPAD: return "The D-Pad works as the Circle Pad, for games that only read the Circle Pad.";
        case ED_HOME: return "Buttons that act as the HOME button when pressed together: for a console whose HOME button is broken.";
        default: return "Copy, delete, and what to do if the buttons stop working.";
    }
}

static void ed_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    ed_data* ed = (ed_data*) data;
    const theme_current* t = theme();
    ed_row* row = selected != NULL ? (ed_row*) selected->data : NULL;

    u32 press = 0;
    u32 get = 0;
    if(row != NULL) {
        const rehid_config* c = &ed->config;
        switch(row->kind) {
            case ED_KEY: press = c->keys[row->index].press; get = c->keys[row->index].get; break;
            case ED_TURBO: press = c->turbo[row->index].press; get = c->turbo[row->index].get; break;
            case ED_TOUCH: press = c->touch[row->index].press; break;
            case ED_BOX: get = c->boxes[row->index].get; break;
            case ED_CPAD: press = c->cpad[row->index].press; break;
            case ED_CPAD_TO_DPAD: press = c->cpadToDpad ? KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT : 0; get = c->cpadToDpad ? KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT : 0; break;
            case ED_DPAD_TO_CPAD: press = c->dpadToCpad ? KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT : 0; get = c->dpadToCpad ? KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT : 0; break;
            case ED_HOME: press = c->home; break;
            default: break;
        }
    }

    // The game's name and whose remapping this is, the console with the entry lit, what the row does.
    const char* whose = ed->titleId == 0 ? _("Every game that has no remapping of its own, HOME Menu too.")
                        : ed->fileExists ? _("This game's own remapping: the global one is not used for it.")
                                         : _("This game uses the global remapping until you change something here.");
    widget_text_fit(ed->name, x1 + 10, y1 + 4, WIDGET_TEXT_SCALE, t->palette->text, x2 - x1 - 20);
    widget_text_fit(whose, x1 + 10, y1 + 20, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - x1 - 20);

    rv_draw_console(press, get, x1 + 40, y1 + 42, x2 - 40);
    if(row != NULL) {
        screen_draw_text_wrap(i18n_translate_text(ed_help(row->kind)), x1 + 12, y2 - 30, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - 12);
    }

    if(ed->config.dropped > 0) {
        char note[128];
        snprintf(note, sizeof(note), i18n_translate_text("%lu entries of the file were not ones Rehid reads: they go when it is saved."), ed->config.dropped);
        widget_badge(x1 + 10, y2 - 46, note, t->palette->surface, t->palette->warning);
    }
}

// -- making entries --

static void ed_key_got(void* data, u32 keys) {
    ed_data* ed = (ed_data*) data;
    if(keys == 0) {
        return;
    }

    if(keys == ed->pending) {
        prompt_display_notify(_("Button remapping"), _("The game would get the same buttons that are pressed: nothing to change."), COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    rehid_config* c = &ed->config;
    if(ed->actionKind == ED_KEY && ed->actionIndex < c->keyCount) {
        c->keys[ed->actionIndex].press = ed->pending;
        c->keys[ed->actionIndex].get = keys;
    } else if(c->keyCount < REHID_ENTRIES_MAX) {
        c->keys[c->keyCount].press = ed->pending;
        c->keys[c->keyCount].get = keys;
        c->keyCount++;
    }

    ed_save(ed);
}

static void ed_key_pressed(void* data, u32 keys) {
    ed_data* ed = (ed_data*) data;
    if(keys == 0) {
        return;
    }

    ed->pending = keys;
    rv_capture_open("The buttons the game gets", "Now press the buttons the game should see instead (one, or several together), and let go.", ed, ed_key_got);
}

static void ed_start_key(ed_data* ed) {
    rv_capture_open("The buttons you will press", "Press the button you want to change (or several together, as a combo), and let go.", ed, ed_key_pressed);
}

static u8 ed_turbo_frames = 0;

static void ed_turbo_got(void* data, u32 keys) {
    ed_data* ed = (ed_data*) data;
    if(keys == 0) {
        return;
    }

    rehid_config* c = &ed->config;
    rehid_turbo* t = NULL;
    if(ed->actionKind == ED_TURBO && ed->actionIndex < c->turboCount) {
        t = &c->turbo[ed->actionIndex];
    } else if(c->turboCount < REHID_ENTRIES_MAX) {
        t = &c->turbo[c->turboCount++];
    }

    if(t != NULL) {
        t->press = ed->pending;
        t->get = keys;
        t->frames = ed_turbo_frames;
        ed_save(ed);
    }
}

static void ed_turbo_speed(ed_data* ed, u8 frames) {
    ed_turbo_frames = frames;
    rv_capture_open("The button the turbo presses", "Press the button the turbo should press again and again (A, for example), and let go.", ed, ed_turbo_got);
}

static void ed_turbo_fast(void* data) { ed_turbo_speed((ed_data*) data, 2); }
static void ed_turbo_medium(void* data) { ed_turbo_speed((ed_data*) data, 5); }
static void ed_turbo_slow(void* data) { ed_turbo_speed((ed_data*) data, 10); }

static void ed_turbo_pressed(void* data, u32 keys) {
    ed_data* ed = (ed_data*) data;
    if(keys == 0) {
        return;
    }

    ed->pending = keys;
    sheet_action actions[] = {
            {ICON_BOLT, _("Fast"), false, ed_turbo_fast, false, _("Every 2 frames")},
            {ICON_BOLT, _("Medium"), false, ed_turbo_medium, false, _("Every 5 frames")},
            {ICON_BOLT, _("Slow"), false, ed_turbo_slow, false, _("Every 10 frames")},
    };
    sheet_display_list(_("Turbo speed"), 0, actions, COUNT_OF(actions), ed);
}

static void ed_start_turbo(ed_data* ed) {
    rv_capture_open("The button that turns the turbo on", "Press the button that will turn the turbo on and off (ZL, for example), and let go.", ed, ed_turbo_pressed);
}

static void ed_touch_placed(void* data, bool ok, u16 x, u16 y, u16 w, u16 h) {
    ed_data* ed = (ed_data*) data;
    if(!ok) {
        return;
    }

    rehid_config* c = &ed->config;
    rehid_point* p = NULL;
    if(ed->actionKind == ED_TOUCH && ed->actionIndex < c->touchCount) {
        p = &c->touch[ed->actionIndex];
    } else if(c->touchCount < REHID_ENTRIES_MAX) {
        p = &c->touch[c->touchCount++];
    }

    if(p != NULL) {
        p->press = ed->pending;
        p->x = (s16) x;
        p->y = (s16) y;
        ed_save(ed);
    }
}

static void ed_touch_pressed(void* data, u32 keys) {
    ed_data* ed = (ed_data*) data;
    if(keys == 0) {
        return;
    }

    ed->pending = keys;
    rv_place_open(false, "Tap the point of the bottom screen the button will touch. The frame is the game's whole bottom screen.", ed, ed_touch_placed);
}

static void ed_start_touch(ed_data* ed) {
    rv_capture_open("The button that touches the screen", "Press the button that will touch the bottom screen, and let go.", ed, ed_touch_pressed);
}

static void ed_box_got(void* data, u32 keys) {
    ed_data* ed = (ed_data*) data;
    if(keys == 0) {
        return;
    }

    rehid_config* c = &ed->config;
    rehid_box* b = NULL;
    if(ed->actionKind == ED_BOX && ed->actionIndex < c->boxCount) {
        b = &c->boxes[ed->actionIndex];
    } else if(c->boxCount < REHID_ENTRIES_MAX) {
        b = &c->boxes[c->boxCount++];
    }

    if(b != NULL) {
        *b = ed->pendingBox;
        b->get = keys;
        ed_save(ed);
    }
}

static void ed_box_placed(void* data, bool ok, u16 x, u16 y, u16 w, u16 h) {
    ed_data* ed = (ed_data*) data;
    if(!ok) {
        return;
    }

    // Changing a zone only moves it; a new one waits in `pendingBox` while its button is taken.
    if(ed->actionKind == ED_BOX && ed->actionIndex < ed->config.boxCount) {
        rehid_box* b = &ed->config.boxes[ed->actionIndex];
        b->x = x;
        b->y = y;
        b->w = w;
        b->h = h;
        ed_save(ed);
        return;
    }

    ed->pendingBox = (rehid_box) {0, x, y, w, h};
    rv_capture_open("The button the zone presses", "Press the button a touch in that zone should give the game, and let go.", ed, ed_box_got);
}

static void ed_start_box(ed_data* ed) {
    rv_place_open(true, "Draw the zone with your finger, from one corner to the other. A touch in it presses the button you choose next.", ed, ed_box_placed);
}

static void ed_cpad_direction_chosen(ed_data* ed, u32 direction) {
    rehid_config* c = &ed->config;
    rehid_point* p = NULL;
    if(ed->actionKind == ED_CPAD && ed->actionIndex < c->cpadCount) {
        p = &c->cpad[ed->actionIndex];
    } else if(c->cpadCount < REHID_ENTRIES_MAX) {
        p = &c->cpad[c->cpadCount++];
    }

    if(p != NULL) {
        p->press = ed->pending;
        p->x = ed_cpad_xy[direction][0];
        p->y = ed_cpad_xy[direction][1];
        ed_save(ed);
    }
}

static void ed_cpad_0(void* data) { ed_cpad_direction_chosen((ed_data*) data, 0); }
static void ed_cpad_1(void* data) { ed_cpad_direction_chosen((ed_data*) data, 1); }
static void ed_cpad_2(void* data) { ed_cpad_direction_chosen((ed_data*) data, 2); }
static void ed_cpad_3(void* data) { ed_cpad_direction_chosen((ed_data*) data, 3); }
static void ed_cpad_4(void* data) { ed_cpad_direction_chosen((ed_data*) data, 4); }
static void ed_cpad_5(void* data) { ed_cpad_direction_chosen((ed_data*) data, 5); }
static void ed_cpad_6(void* data) { ed_cpad_direction_chosen((ed_data*) data, 6); }
static void ed_cpad_7(void* data) { ed_cpad_direction_chosen((ed_data*) data, 7); }

static void ed_cpad_pressed(void* data, u32 keys) {
    ed_data* ed = (ed_data*) data;
    if(keys == 0) {
        return;
    }

    ed->pending = keys;
    void (*runs[8])(void*) = {ed_cpad_0, ed_cpad_1, ed_cpad_2, ed_cpad_3, ed_cpad_4, ed_cpad_5, ed_cpad_6, ed_cpad_7};
    sheet_action actions[8];
    for(u32 i = 0; i < 8; i++) {
        actions[i] = (sheet_action) {ICON_ARROWS_MAXIMIZE, i18n_translate_text(ed_cpad_names[i]), false, runs[i]};
    }

    sheet_display_list(_("Circle Pad direction"), 0, actions, COUNT_OF(actions), ed);
}

static void ed_start_cpad(ed_data* ed) {
    rv_capture_open("The button that moves the Circle Pad", "Press the button that will hold the Circle Pad, and let go. Its direction comes next.", ed, ed_cpad_pressed);
}

static void ed_home_got(void* data, u32 keys) {
    ed_data* ed = (ed_data*) data;
    if(keys == 0) {
        return;
    }

    u32 count = 0;
    for(u32 bits = keys; bits != 0; bits >>= 1) {
        count += bits & 1;
    }

    if(count < 2) {
        prompt_display_notify(_("HOME button"), _("Choose two buttons or more, pressed together: a single button would leave the game for HOME Menu every time it is pressed."), COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    ed->config.home = keys;
    ed_save(ed);
}

// -- an entry's sheet --

static void ed_entry_change(void* data) {
    ed_data* ed = (ed_data*) data;
    switch(ed->actionKind) {
        case ED_KEY: ed_start_key(ed); break;
        case ED_TURBO: ed_start_turbo(ed); break;
        case ED_TOUCH: ed_start_touch(ed); break;
        case ED_BOX: ed_start_box(ed); break;
        case ED_CPAD: ed_start_cpad(ed); break;
        default: break;
    }
}

static void ed_remove_at(void* list, u32 size, u32* count, u32 index) {
    if(index >= *count) {
        return;
    }

    u8* bytes = (u8*) list;
    memmove(bytes + index * size, bytes + (index + 1) * size, (*count - index - 1) * size);
    (*count)--;
}

static void ed_entry_delete(void* data) {
    ed_data* ed = (ed_data*) data;
    rehid_config* c = &ed->config;
    switch(ed->actionKind) {
        case ED_KEY: ed_remove_at(c->keys, sizeof(*c->keys), &c->keyCount, ed->actionIndex); break;
        case ED_TURBO: ed_remove_at(c->turbo, sizeof(*c->turbo), &c->turboCount, ed->actionIndex); break;
        case ED_TOUCH: ed_remove_at(c->touch, sizeof(*c->touch), &c->touchCount, ed->actionIndex); break;
        case ED_BOX: ed_remove_at(c->boxes, sizeof(*c->boxes), &c->boxCount, ed->actionIndex); break;
        case ED_CPAD: ed_remove_at(c->cpad, sizeof(*c->cpad), &c->cpadCount, ed->actionIndex); break;
        default: return;
    }

    ed_save(ed);
}

static void ed_home_change(void* data) {
    rv_capture_open("The HOME combo", "Press the buttons that will act as HOME together (L + R + START, for example), and let go.", data, ed_home_got);
}

static void ed_home_clear(void* data) {
    ed_data* ed = (ed_data*) data;
    ed->config.home = 0;
    ed_save(ed);
}

// -- More --

static void rv_help_open(void* data);

static void ed_copy_global(void* data) {
    ed_data* ed = (ed_data*) data;
    rehid_config global;
    const char* why = NULL;
    Result res = rehid_config_load(0, &global, &why);
    if(R_FAILED(res)) {
        prompt_display_notify(_("Failure"), why != NULL ? i18n_translate_text(why) : _("Failed to read the global remapping."), COLOR_TEXT, NULL, NULL, NULL);
        rehid_config_free(&global);
        return;
    }

    rehid_config_free(&ed->config);
    ed->config = global;   // Takes its extra fields too.
    ed_save(ed);
}

static void ed_delete_answered(ui_view* view, void* data, u32 response) {
    ed_data* ed = (ed_data*) data;
    if(response != PROMPT_YES) {
        return;
    }

    if(!rehid_config_delete(ed->titleId)) {
        error_display(NULL, NULL, _("Failed to delete the remapping file."));
        return;
    }

    rehid_config_free(&ed->config);
    ed->fileExists = false;
    ed->built = false;
    rehidview_dirty = true;
}

static void ed_delete(void* data) {
    ed_data* ed = (ed_data*) data;
    prompt_display_warning(_("Confirmation"), ed->titleId == 0 ? _("Delete the global remapping? Every game without one of its own gets the console's buttons again.")
                                                               : _("Delete this game's remapping? It uses the global one again (or none)."),
                           COLOR_TEXT, ed, NULL, ed_delete_answered);
}

static void ed_more(ed_data* ed) {
    sheet_action actions[3];
    u32 count = 0;
    if(ed->titleId != 0) {
        actions[count++] = (sheet_action) {ICON_COPY, _("Copy the global remapping here"), false, ed_copy_global};
    }

    if(ed->fileExists) {
        actions[count++] = (sheet_action) {ICON_TRASH, ed->titleId == 0 ? _("Delete the global remapping") : _("Delete this game's remapping"), true, ed_delete};
    }

    actions[count++] = (sheet_action) {ICON_INFO_CIRCLE, _("If the buttons stop working"), false, rv_help_open};
    sheet_display_list(NULL, 0, actions, count, ed);
}

static void ed_free(ed_data* ed) {
    rehid_config_free(&ed->config);
    free(ed);
}

static void ed_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    ed_data* ed = (ed_data*) data;
    if(hidKeysDown() & KEY_B) {
        ui_pop();
        ed_clear(items);
        list_destroy(view);
        ed_free(ed);
        return;
    }

    if(!ed->built) {
        ed_build(ed, items);
    }

    if(selected == NULL || selected->data == NULL || !(selectedTouched || (hidKeysDown() & KEY_A))) {
        return;
    }

    ed_row* row = (ed_row*) selected->data;
    ed->actionKind = row->kind;
    ed->actionIndex = row->index;
    switch(row->kind) {
        case ED_ADD_KEY: ed_start_key(ed); break;
        case ED_ADD_TURBO: ed_start_turbo(ed); break;
        case ED_ADD_TOUCH: ed_start_touch(ed); break;
        case ED_ADD_BOX: ed_start_box(ed); break;
        case ED_ADD_CPAD: ed_start_cpad(ed); break;
        case ED_CPAD_TO_DPAD:
            ed->config.cpadToDpad = !ed->config.cpadToDpad;
            ed_save(ed);
            break;
        case ED_DPAD_TO_CPAD:
            ed->config.dpadToCpad = !ed->config.dpadToCpad;
            ed_save(ed);
            break;
        case ED_HOME: {
            sheet_action actions[2];
            u32 count = 0;
            actions[count++] = (sheet_action) {ICON_PENCIL, _("Choose the buttons"), false, ed_home_change};
            if(ed->config.home != 0) {
                actions[count++] = (sheet_action) {ICON_X, _("No HOME combo"), false, ed_home_clear};
            }

            sheet_display_list(selected->name, 0, actions, count, ed);
            break;
        }
        case ED_MORE: ed_more(ed); break;
        default: {
            sheet_action actions[] = {
                    {ICON_PENCIL, _("Change"), false, ed_entry_change},
                    {ICON_TRASH, _("Delete"), true, ed_entry_delete},
            };
            sheet_display_list(selected->name, 0, actions, COUNT_OF(actions), ed);
            break;
        }
    }
}

// A file that is not one NSA can edit, offered for deletion.
static u64 rv_damaged_title = 0;

static void rv_damaged_answered(ui_view* view, void* data, u32 response) {
    if(response != PROMPT_YES) {
        return;
    }

    if(!rehid_config_delete(rv_damaged_title)) {
        error_display(NULL, NULL, _("Failed to delete the remapping file."));
    }

    rehidview_dirty = true;
}

static void ed_open(u64 titleId, const char* name) {
    ed_data* ed = (ed_data*) calloc(1, sizeof(ed_data));
    if(ed == NULL) {
        error_display(NULL, NULL, "Failed to allocate remapping data.");
        return;
    }

    ed->titleId = titleId;
    string_copy(ed->name, name, sizeof(ed->name));
    ed->fileExists = rehid_config_exists(titleId);
    const char* why = NULL;
    Result res = rehid_config_load(titleId, &ed->config, &why);
    if(R_FAILED(res)) {
        // Not one NSA can edit: it can go (Rehid then reads the global one, or none).
        static char text[512];
        snprintf(text, sizeof(text), "%s\n\n%s", why != NULL ? i18n_translate_text(why) : _("Failed to read the remapping."), _("Delete this file now?"));
        rv_damaged_title = titleId;
        prompt_display_warning(_("Damaged remapping"), text, COLOR_TEXT, NULL, NULL, rv_damaged_answered);
        ed_free(ed);
        return;
    }

    ui_view* view = list_display(titleId == 0 ? _("Global remapping") : _("Game remapping"), _(G_A " change    " G_B " back"), ed, ed_update, ed_draw_top);
    if(view == NULL) {
        ed_free(ed);
        return;
    }

    ed->view = view;
    list_style style = {.rowHeight = 28, .drawRow = ed_draw_row};
    list_set_style(view, &style);
    list_show_title_bar(view);

    if(titleId == 0 && !ed->fileExists) {
        prompt_display_notify(_("Global remapping"), _("The global remapping is used by every title that has none of its own, HOME Menu too. NSA keeps the console's buttons (it has a file of its own), so it can always undo it."),
                              COLOR_TEXT, NULL, NULL, NULL);
    }
}

// ---- recovery ----

static void rv_help_open(void* data) {
    prompt_display_notify(_("If the buttons stop working"),
                          _("1. Hold POWER for a few seconds: the console turns off, whatever is on the screen.\n"
                            "2. Turn it on holding SELECT: Luma3DS's menu opens before Rehid is loaded. Untick \"Enable loading external FIRMs and modules\" and start: the console has its own buttons again.\n"
                            "3. Or take the SD card to a computer and rename /luma/sysmodules/0004013000001D02.cxi (or delete the game's /rehid/<title ID>/rehid.json).\n\n"
                            "Then open NSA and fix or delete the remapping."),
                          COLOR_TEXT, NULL, NULL, NULL);
}

// ---- the list: Rehid's state, the remappings, adding a game ----

typedef enum {
    RV_STATE,
    RV_GLOBAL,
    RV_TITLE,
    RV_ADD,
    RV_HELP
} rv_kind;

typedef struct {
    rv_kind kind;
    u64 titleId;
    list_item* title;      // task_title_item's (name and icon), or NULL.
    bool installed;
    u32 entries;           // How many things its file changes.
    bool readable;
} rv_entry;

typedef struct {
    ui_view* view;
    bool populated;
    rehid_state state;
    char version[32];
    bool hasPrevious;

    // The install, on a thread of its own.
    Thread thread;
    volatile bool finished;
    Result result;
    const char* why;
    char newVersion[32];
} rv_data;

static u32 rv_count_entries(u64 titleId, bool* readable) {
    rehid_config config;
    const char* why = NULL;
    *readable = R_SUCCEEDED(rehid_config_load(titleId, &config, &why));
    u32 count = config.keyCount + config.turboCount + config.touchCount + config.boxCount + config.cpadCount + (config.cpadToDpad ? 1 : 0)
                + (config.dpadToCpad ? 1 : 0) + (config.home != 0 ? 1 : 0);
    rehid_config_free(&config);
    return count;
}

static bool rv_title_installed(u64 titleId, FS_MediaType* media) {
    static const FS_MediaType medias[] = {MEDIATYPE_SD, MEDIATYPE_NAND, MEDIATYPE_GAME_CARD};
    for(u32 i = 0; i < COUNT_OF(medias); i++) {
        AM_TitleEntry entry;
        if(R_SUCCEEDED(AM_GetTitleInfo(medias[i], 1, &titleId, &entry))) {
            *media = medias[i];
            return true;
        }
    }

    return false;
}

static void rv_free_entries(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        rv_entry* entry = (rv_entry*) item->data;
        if(entry != NULL) {
            task_free_title(entry->title);
            free(entry);
        }

        free(item);
        linked_list_iter_remove(&iter);
    }
}

static void rv_add_entry(linked_list* items, rv_kind kind, u64 titleId, const char* name) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    rv_entry* entry = (rv_entry*) calloc(1, sizeof(rv_entry));
    if(item == NULL || entry == NULL) {
        free(item);
        free(entry);
        return;
    }

    entry->kind = kind;
    entry->titleId = titleId;
    item->data = entry;
    item->color = COLOR_TEXT;
    string_copy(item->name, name, sizeof(item->name));

    if(kind == RV_GLOBAL || kind == RV_TITLE) {
        entry->entries = rv_count_entries(titleId, &entry->readable);
    }

    if(kind == RV_TITLE) {
        FS_MediaType media = MEDIATYPE_SD;
        entry->installed = rv_title_installed(titleId, &media);
        if(entry->installed) {
            entry->title = task_title_item(media, titleId);
            if(entry->title != NULL && ((title_info*) entry->title->data)->hasMeta) {
                string_copy(item->name, entry->title->name, sizeof(item->name));
            }
        }
    }

    linked_list_add(items, item);
}

static int rv_compare(void* data, const void* p1, const void* p2) {
    const list_item* a = (const list_item*) p1;
    const list_item* b = (const list_item*) p2;
    const rv_entry* ea = (const rv_entry*) a->data;
    const rv_entry* eb = (const rv_entry*) b->data;
    if(ea->kind != eb->kind) {
        return (int) ea->kind - (int) eb->kind;
    }

    return strcasecmp(a->name, b->name);
}

static void rv_populate(rv_data* d, linked_list* items) {
    rv_free_entries(items);
    d->state = rehid_get_state();
    rehid_installed_version(d->version, sizeof(d->version));
    d->hasPrevious = rehid_has_previous();
    if(d->state != REHID_NOT_INSTALLED) {
        rehid_protect_nsa();
    }

    rv_add_entry(items, RV_STATE, 0, "Rehid");
    rv_add_entry(items, RV_GLOBAL, 0, _("Every game (global)"));

    u64 titles[128];
    u32 count = rehid_list_titles(titles, COUNT_OF(titles));
    for(u32 i = 0; i < count; i++) {
        if(titles[i] == REHID_NSA_TITLE_ID) {
            continue;   // Kept as "{}" for NSA itself: not something to edit.
        }

        char name[32];
        snprintf(name, sizeof(name), "%016llX", titles[i]);
        rv_add_entry(items, RV_TITLE, titles[i], name);
    }

    rv_add_entry(items, RV_ADD, 0, _("Add a game"));
    rv_add_entry(items, RV_HELP, 0, _("If the buttons stop working"));
    linked_list_sort(items, d, rv_compare);
    d->populated = true;
    rehidview_dirty = false;
}

static const char* rv_state_text(rehid_state state) {
    switch(state) {
        case REHID_NOT_INSTALLED: return _("Not installed");
        case REHID_OFF: return _("Turned off");
        case REHID_NEEDS_LUMA: return _("Luma3DS does not load it");
        case REHID_NEEDS_RESTART: return _("Restart to start it");
        default: return _("Running");
    }
}

static void rv_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    rv_data* d = (rv_data*) data;
    rv_entry* entry = (rv_entry*) item->data;
    const theme_current* t = theme();
    u32 ink = 0;
    u32 muted = 0;
    widget_item_frame(x, y, width, height, selected, &ink, &muted);

    float icon = height - 10;
    title_info* info = entry->title != NULL ? (title_info*) entry->title->data : NULL;
    if(info != NULL && info->hasMeta && info->meta.texture != 0) {
        widget_texture_rounded(info->meta.texture, x + 5, y + 5, icon, 4, t->palette->surface);
    } else {
        u32 glyph = entry->kind == RV_STATE ? ICON_PUZZLE : entry->kind == RV_GLOBAL ? ICON_WORLD : entry->kind == RV_ADD ? ICON_PLUS
                    : entry->kind == RV_HELP ? ICON_INFO_CIRCLE : ICON_DEVICE_GAMEPAD_2;
        widget_icon_placeholder(x + 5, y + 5, icon, glyph, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->palette->background,
                                selected ? t->onAccent : entry->kind == RV_ADD ? t->accent : t->palette->textMuted);
    }

    float tx = x + 5 + icon + 8;
    float right = x + width - 8;
    char line[96];
    line[0] = '\0';
    const char* tag = NULL;
    bool good = false;
    bool bad = false;
    switch(entry->kind) {
        case RV_STATE:
            tag = rv_state_text(d->state);
            good = d->state == REHID_RUNNING;
            bad = d->state == REHID_NEEDS_LUMA;
            if(d->version[0] != '\0') {
                snprintf(line, sizeof(line), "%s %s", _("Version"), d->version);
            } else {
                string_copy(line, _("A HID module for Luma3DS, by hax0kartik"), sizeof(line));
            }

            break;
        case RV_GLOBAL:
        case RV_TITLE:
            if(!entry->readable) {
                tag = _("Damaged");
                bad = true;
            } else if(entry->kind == RV_GLOBAL && !rehid_config_exists(0)) {
                tag = _("None");
            } else {
                snprintf(line, sizeof(line), i18n_translate_text(entry->entries == 1 ? "%lu change" : "%lu changes"), entry->entries);
            }

            if(entry->kind == RV_TITLE && !entry->installed) {
                string_copy(line, _("Not on the console"), sizeof(line));
            } else if(entry->kind == RV_TITLE && line[0] != '\0' && entry->title != NULL) {
                char more[96];
                snprintf(more, sizeof(more), "%s   %016llX", line, entry->titleId);
                string_copy(line, more, sizeof(line));
            }

            break;
        case RV_ADD: string_copy(line, _("A remapping for one game only"), sizeof(line)); break;
        default: string_copy(line, _("How to get the buttons back"), sizeof(line)); break;
    }

    // Running in green, a damaged file in amber, the rest (not installed, none) in the accent.
    float tagW = 0;
    if(tag != NULL) {
        u32 back = selected ? THEME_ALPHA(t->onAccent, 0x40) : good ? t->palette->successSoft : bad ? THEME_ALPHA(t->palette->warning, 0x30) : t->accentSoft;
        u32 fore = selected ? t->onAccent : good ? t->palette->success : bad ? t->palette->warning : t->accent;
        tagW = widget_badge_right(right, y + 4, tag, back, fore);
    }

    widget_text_fit(item->name, tx, y + 4, WIDGET_TEXT_SCALE, ink, right - tagW - 6 - tx);
    widget_text_fit(line, tx, y + height - 4 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, muted, right - tx);
}

static void rv_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    rv_data* d = (rv_data*) data;
    rv_entry* entry = selected != NULL ? (rv_entry*) selected->data : NULL;
    if(entry == NULL) {
        return;
    }

    switch(entry->kind) {
        case RV_STATE: {
            const char* text = NULL;
            switch(d->state) {
                case REHID_NOT_INSTALLED: text = _("Rehid changes what the buttons do in every game. It is a module Luma3DS loads instead of the console's own: NSA downloads it from its author's GitHub (hax0kartik/rehid) and edits its remappings. Press " G_A " to install it."); break;
                case REHID_OFF: text = _("Rehid is on the SD card but turned off: the console starts with its own buttons. Press " G_A " to turn it on."); break;
                case REHID_NEEDS_LUMA: text = _("Luma3DS does not load modules from the SD card. NSA can turn on its option \"Enable loading external FIRMs and modules\": press " G_A "."); break;
                case REHID_NEEDS_RESTART: text = _("Rehid is installed and Luma3DS will load it: it starts with the console. Press " G_A " to restart now."); break;
                default: text = _("Rehid is running. Remappings apply the next time a game starts. Press " G_A " to update it, turn it off or go back to the version before."); break;
            }

            rv_draw_card(x1, y1, x2, y2 - y1 - 20, ICON_PUZZLE, "Rehid", text);
            break;
        }
        case RV_GLOBAL:
            rv_draw_card(x1, y1, x2, y2 - y1 - 20, ICON_WORLD, _("Every game (global)"),
                         _("The remapping of every title that has none of its own, HOME Menu too. A game with a remapping of its own uses only that one."));
            break;
        case RV_TITLE:
            if(entry->title != NULL) {
                task_draw_title_info(view, entry->title->data, x1, y1, x2, y2);
            } else {
                char text[200];
                snprintf(text, sizeof(text), i18n_translate_text("Title %016llX is not on the console. Its remapping stays in /rehid until you delete it."), entry->titleId);
                rv_draw_card(x1, y1, x2, y2 - y1 - 20, ICON_DEVICE_GAMEPAD_2, selected->name, text);
            }

            break;
        case RV_ADD:
            rv_draw_card(x1, y1, x2, y2 - y1 - 20, ICON_PLUS, _("Add a game"), _("Choose a game installed on the console: its remapping is used instead of the global one."));
            break;
        default:
            rv_draw_card(x1, y1, x2, y2 - y1 - 20, ICON_INFO_CIRCLE, _("If the buttons stop working"),
                         _("Turn the console on holding SELECT: Luma3DS's menu opens before Rehid is loaded, and turning off \"Enable loading external FIRMs and modules\" gives the console its own buttons back. Press " G_A " for every step."));
            break;
    }
}

// -- installing --

static void rv_restart_answered(ui_view* view, void* data, u32 response) {
    if(response != PROMPT_YES) {
        return;
    }

    char busy[128];
    if(xferturn_describe(busy, sizeof(busy))) {
        prompt_display_notify(_("Busy"), _("Wait until the transfer ends."), COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    APT_HardwareResetAsync();
}

static void rv_ask_restart(const char* text) {
    prompt_display_yes_no(_("Restart"), text, COLOR_TEXT, NULL, NULL, rv_restart_answered);
}

static void rv_luma_answered(ui_view* view, void* data, u32 response) {
    rv_data* d = (rv_data*) data;
    if(response != PROMPT_YES) {
        return;
    }

    if(!rehid_set_luma_modules(true)) {
        prompt_display_notify(_("Luma3DS"), _("Luma3DS's options were not found (/luma/config.ini). Luma3DS 13 or later writes that file; with an older one, change them in its menu (hold SELECT while the console starts)."),
                              COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    d->populated = false;
    rv_ask_restart(_("Luma3DS will load Rehid from the next start of the console.\n\nRestart it now?"));
}

static void rv_ask_luma(rv_data* d) {
    prompt_display_yes_no(_("Luma3DS"), _("Rehid needs Luma3DS's option \"Enable loading external FIRMs and modules\". Turn it on? It changes that one line of /luma/config.ini."),
                          COLOR_TEXT, d, NULL, rv_luma_answered);
}

static void rv_install_thread(void* arg) {
    rv_data* d = (rv_data*) arg;
    d->result = rehid_install_latest(d->newVersion, sizeof(d->newVersion), &d->why);
    d->finished = true;
}

static void rv_install_progress(ui_view* view, void* data, float* progress, char* text) {
    rv_data* d = (rv_data*) data;
    if(!d->finished) {
        return;
    }

    threadJoin(d->thread, U64_MAX);
    threadFree(d->thread);
    d->thread = NULL;
    ui_pop();
    info_destroy(view);
    d->populated = false;

    if(R_FAILED(d->result)) {
        if(d->why != NULL) {
            prompt_display_notify("Failure", d->why, COLOR_TEXT, NULL, NULL, NULL);
        } else {
            error_display_res(NULL, NULL, d->result, "Failed to install Rehid.");
        }

        return;
    }

    rehid_protect_nsa();
    if(rehid_luma_modules() != 1) {
        rv_ask_luma(d);
        return;
    }

    if(d->state == REHID_RUNNING) {
        rv_ask_restart(_("Rehid is updated. The new version starts with the console.\n\nRestart it now?"));
    } else {
        rv_ask_restart(_("Rehid is installed. It starts with the console.\n\nRestart it now?"));
    }
}

static void rv_install_answered(ui_view* view, void* data, u32 response) {
    rv_data* d = (rv_data*) data;
    if(response != PROMPT_YES) {
        return;
    }

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    d->finished = false;
    d->why = NULL;
    d->thread = threadCreate(rv_install_thread, d, 0x8000, priority + 1, -2, false);
    if(d->thread == NULL) {
        error_display(NULL, NULL, "Failed to create the install thread.");
        return;
    }

    info_display("Installing Rehid", "", false, d, rv_install_progress, NULL);
}

static void rv_install(void* data) {
    rv_data* d = (rv_data*) data;
    prompt_display_yes_no(_("Rehid"), d->state == REHID_NOT_INSTALLED
                                          ? _("Download Rehid's latest release from GitHub (hax0kartik/rehid, about 70 KB) and put it in /luma/sysmodules?\n\nIf the buttons ever stop working: turn the console on holding SELECT and untick \"Enable loading external FIRMs and modules\".")
                                          : _("Download Rehid's latest release from GitHub and put it in place of this one? This one is kept, to go back to."),
                          COLOR_TEXT, d, NULL, rv_install_answered);
}

static void rv_toggle_answered(ui_view* view, void* data, u32 response) {
    rv_data* d = (rv_data*) data;
    if(response != PROMPT_YES) {
        return;
    }

    bool on = d->state == REHID_OFF;
    if(!rehid_set_enabled(on)) {
        error_display(NULL, NULL, _("Failed to move Rehid's files in /luma/sysmodules."));
        return;
    }

    d->populated = false;
    if(on && rehid_luma_modules() != 1) {
        rv_ask_luma(d);
        return;
    }

    rv_ask_restart(on ? _("Rehid is on again from the next start of the console.\n\nRestart it now?")
                      : _("Rehid is off from the next start of the console: the buttons are the console's own again. Your remappings stay in /rehid.\n\nRestart it now?"));
}

static void rv_toggle(void* data) {
    rv_data* d = (rv_data*) data;
    prompt_display_yes_no(_("Rehid"), d->state == REHID_OFF ? _("Turn Rehid on again? It is loaded from the next start of the console.")
                                                            : _("Turn Rehid off? From the next start the console has its own buttons; your remappings stay."),
                          COLOR_TEXT, d, NULL, rv_toggle_answered);
}

static void rv_previous_answered(ui_view* view, void* data, u32 response) {
    rv_data* d = (rv_data*) data;
    if(response != PROMPT_YES) {
        return;
    }

    if(!rehid_restore_previous()) {
        error_display(NULL, NULL, _("Failed to put the previous Rehid back."));
        return;
    }

    d->populated = false;
    rv_ask_restart(_("The previous Rehid is back. It starts with the console.\n\nRestart it now?"));
}

static void rv_previous(void* data) {
    prompt_display_yes_no(_("Rehid"), _("Go back to the Rehid that was installed before the last update?"), COLOR_TEXT, data, NULL, rv_previous_answered);
}

static void rv_luma(void* data) {
    rv_ask_luma((rv_data*) data);
}

static void rv_restart(void* data) {
    rv_ask_restart(_("Restart the console now? Rehid starts with it."));
}

static void rv_state_open(rv_data* d) {
    sheet_action actions[5];
    u32 count = 0;
    switch(d->state) {
        case REHID_NOT_INSTALLED:
            actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Install Rehid"), false, rv_install};
            break;
        case REHID_OFF:
            actions[count++] = (sheet_action) {ICON_POWER, _("Turn Rehid on"), false, rv_toggle};
            break;
        case REHID_NEEDS_LUMA:
            actions[count++] = (sheet_action) {ICON_PUZZLE, _("Let Luma3DS load it"), false, rv_luma};
            actions[count++] = (sheet_action) {ICON_POWER, _("Turn Rehid off"), true, rv_toggle};
            break;
        case REHID_NEEDS_RESTART:
            actions[count++] = (sheet_action) {ICON_REFRESH, _("Restart the console"), false, rv_restart};
            actions[count++] = (sheet_action) {ICON_POWER, _("Turn Rehid off"), true, rv_toggle};
            break;
        default:
            actions[count++] = (sheet_action) {ICON_DOWNLOAD, _("Update Rehid"), false, rv_install};
            actions[count++] = (sheet_action) {ICON_POWER, _("Turn Rehid off"), true, rv_toggle};
            break;
    }

    if(d->hasPrevious && d->state != REHID_NOT_INSTALLED) {
        actions[count++] = (sheet_action) {ICON_HISTORY, _("Go back to the previous version"), false, rv_previous};
    }

    actions[count++] = (sheet_action) {ICON_INFO_CIRCLE, _("If the buttons stop working"), false, rv_help_open};
    sheet_display_list("Rehid", 0, actions, count, d);
}

// -- picking a game --

typedef struct {
    populate_titles_data populateData;
    bool populated;
    bool sorted;
    u32 sortedReady;
} rv_pick;

static int rv_pick_compare(void* data, const void* p1, const void* p2) {
    return strcasecmp(((const list_item*) p1)->name, ((const list_item*) p2)->name);
}

static bool rv_pick_filter(void* data, u64 titleId, FS_MediaType mediaType) {
    return title_kind_of(titleId) == TITLE_KIND_GAME || title_kind_of(titleId) == TITLE_KIND_DEMO;
}

static void rv_pick_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected != NULL && selected->data != NULL) {
        task_draw_title_info(view, selected->data, x1, y1, x2, y2);
    }
}

static void rv_pick_close(ui_view* view, rv_pick* p, linked_list* items) {
    if(!p->populateData.finished) {
        task_stop_populate(p->populateData.cancelEvent, &p->populateData.finished);
    }

    ui_pop();
    task_clear_titles(items);
    list_destroy(view);
    free(p);
}

// Its name and icon are loaded, for the title bar's count.
static bool rv_pick_ready(list_item* item) {
    return ((title_info*) item->data)->loaded;
}

static void rv_pick_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    rv_pick* p = (rv_pick*) data;
    if(hidKeysDown() & KEY_B) {
        if(!list_clear_search(view)) {
            rv_pick_close(view, p, items);
        }

        return;
    }

    if(hidKeysDown() & KEY_R) {
        list_open_search(view);
        return;
    }

    if(!p->populated) {
        p->populateData.items = items;
        p->populateData.filter = rv_pick_filter;
        p->populateData.userData = p;
        Result res = task_populate_titles(&p->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate title list population.");
        }

        p->populated = true;
    }

    // By name, again as names come in (they load after the list is made).
    u32 ready = list_count_progress(view, items, !p->populateData.finished, rv_pick_ready);
    if(ready != p->sortedReady || (p->populateData.finished && !p->sorted)) {
        linked_list_sort(items, p, rv_pick_compare);
        p->sortedReady = ready;
        p->sorted = p->populateData.finished;
    }

    if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        title_info* info = (title_info*) selected->data;
        u64 titleId = info->titleId;
        char name[128];
        string_copy(name, selected->name, sizeof(name));
        rv_pick_close(view, p, items);
        rv_open_named(titleId, name);
    }
}

static void rv_pick_open() {
    rv_pick* p = (rv_pick*) calloc(1, sizeof(rv_pick));
    if(p == NULL) {
        return;
    }

    p->populateData.finished = true;
    ui_view* view = list_display(_("Choose a game"), _(G_A " choose    R search    " G_B " back"), p, rv_pick_update, rv_pick_draw_top);
    if(view == NULL) {
        free(p);
        return;
    }

    list_add_search_tool(view, _("Search games"));
}

// -- the list itself --

static void rv_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    rv_data* d = (rv_data*) data;
    if(hidKeysDown() & KEY_B) {
        ui_pop();
        rv_free_entries(items);
        list_destroy(view);
        free(d);
        return;
    }

    if(!d->populated || rehidview_dirty) {
        rv_populate(d, items);
    }

    if(selected == NULL || selected->data == NULL || !(selectedTouched || (hidKeysDown() & KEY_A))) {
        return;
    }

    rv_entry* entry = (rv_entry*) selected->data;
    switch(entry->kind) {
        case RV_STATE: rv_state_open(d); break;
        case RV_GLOBAL: ed_open(0, selected->name); break;
        case RV_TITLE: ed_open(entry->titleId, selected->name); break;
        case RV_ADD: rv_pick_open(); break;
        default: rv_help_open(NULL); break;
    }
}

void rehidview_open() {
    rv_data* d = (rv_data*) calloc(1, sizeof(rv_data));
    if(d == NULL) {
        error_display(NULL, NULL, "Failed to allocate remapping data.");
        return;
    }

    ui_view* view = list_display(_("Button remapping"), _(G_A " open    " G_B " back"), d, rv_update, rv_draw_top);
    if(view == NULL) {
        free(d);
        return;
    }

    d->view = view;
    list_style style = {.rowHeight = ROW_HEIGHT, .drawRow = rv_draw_row};
    list_set_style(view, &style);
    list_show_title_bar(view);
}

static void rv_open_named(u64 titleId, const char* name) {
    ed_open(titleId, name);
}

void rehidview_open_title(u64 titleId) {
    char name[128];
    snprintf(name, sizeof(name), "%016llX", titleId);
    FS_MediaType media = MEDIATYPE_SD;
    if(rv_title_installed(titleId, &media)) {
        list_item* item = task_title_item(media, titleId);
        if(item != NULL) {
            if(((title_info*) item->data)->hasMeta) {
                string_copy(name, item->name, sizeof(name));
            }

            task_free_title(item);
        }
    }

    ed_open(titleId, name);
}
