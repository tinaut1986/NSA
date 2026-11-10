#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "../core/core.h"

// System -> Hardware tests: the buttons and the touch screen live (hwtest_buttons_open), and the accelerometer and the
// gyroscope live with a recording to a CSV file (hwtest_motion_open). Issue #43; they stand in for 3DS-TEST and ROB.

#define COUNT_OF(a) (sizeof(a) / sizeof(*(a)))

static bool hwtest_is_new3ds() {
    bool isNew = false;
    return R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew;
}

// A title bar with the back arrow, for a screen where B itself is under test (ui.c's arrow injects a B).
#define HWTEST_ARROW_WIDTH 28
#define HWTEST_ARROW_SIZE 22

static void hwtest_draw_title_bar(ui_view* view, float x1, float y1, float x2, float y2, bool pressed) {
    const theme_current* t = theme();
    float cy = (y2 - y1 - HWTEST_ARROW_SIZE) / 2;
    if(pressed) {
        screen_fill_round_rect(3, cy, HWTEST_ARROW_SIZE, HWTEST_ARROW_SIZE, 6, THEME_ALPHA(t->palette->text, 0x22));
    }

    screen_draw_icon(ICON_ARROW_LEFT, 6, cy + 3, 16, t->palette->text);
    coach_anchor("back", 3, cy, HWTEST_ARROW_SIZE, HWTEST_ARROW_SIZE, true);
    widget_text_fit(view->name, HWTEST_ARROW_WIDTH, (y2 - y1 - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, t->palette->text, x2 - HWTEST_ARROW_WIDTH - 6);
}

static bool hwtest_arrow_at(const touchPosition* pos) {
    return pos->py < WIDGET_TITLE_BAR_HEIGHT && pos->px < HWTEST_ARROW_WIDTH;
}

// ---- buttons and touch ----

#define TRAIL_MAX 240
#define TRAIL_LIFE 1500      // ms a touched point stays on the screen.
#define BACK_HOLD 1000       // ms B is held to leave.
#define STICK_RANGE 156      // Raw units from the centre to the edge (Circle Pad and C-Stick).
#define DRIFT_SPAN 25        // Raw units from the centre to the edge of a stick's drift box.
#define DRIFT_OK 10          // A stick at rest within this many raw units of the centre is fine.
#define DRIFT_HISTORY 90     // Positions kept for the drift boxes: 1.5 s.

typedef struct {
    u16 x;
    u16 y;
    u64 time;
} trail_point;

typedef struct {
    bool isNew;
    bool has3d;          // A 2DS and a New 2DS XL have no 3D slider.
    u32 seen;            // Every key pressed at least once.
    u32 held;
    u32 ignore;          // Held when the screen opened (the A that opened it): not counted until let go.
    circlePosition pad;
    circlePosition stick;
    circlePosition padHistory[DRIFT_HISTORY];
    circlePosition stickHistory[DRIFT_HISTORY];
    u32 historyNext;
    u32 historyCount;

    float slider;
    float sliderMin;
    float sliderMax;
    u8 volume;
    u8 volumeMin;
    u8 volumeMax;
    bool volumeKnown;

    trail_point trail[TRAIL_MAX];
    u32 trailNext;
    bool touching;
    touchPosition touch;
    bool arrowTouched;   // The finger went down on the back arrow.

    u64 backSince;       // B went down then and is still held; 0: not held.
} buttons_data;

// What the test counts: the keys a console has.
static u32 buttons_all(const buttons_data* data) {
    u32 all = KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_START | KEY_SELECT | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT
              | KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT | KEY_TOUCH;
    if(data->isNew) {
        all |= KEY_ZL | KEY_ZR | KEY_CSTICK_UP | KEY_CSTICK_DOWN | KEY_CSTICK_LEFT | KEY_CSTICK_RIGHT;
    }

    return all;
}

static u32 buttons_count(u32 bits) {
    u32 n = 0;
    while(bits != 0) {
        n += bits & 1;
        bits >>= 1;
    }

    return n;
}

// A slider counts once it has been taken from one end to the other.
static bool buttons_slider_done(const buttons_data* d) {
    return d->sliderMin <= 0.02f && d->sliderMax >= 0.98f;
}

static bool buttons_volume_done(const buttons_data* d) {
    return d->volumeKnown && d->volumeMin <= 1 && d->volumeMax >= 62;
}

// A key's colours: held now, pressed before, never pressed, or absent from this console.
static void buttons_state_colors(bool present, bool held, bool seen, u32* fill, u32* ink) {
    const theme_current* t = theme();
    if(!present) {
        *fill = THEME_ALPHA(t->palette->border, 0x60);
        *ink = THEME_ALPHA(t->palette->textMuted, 0x80);
    } else if(held) {
        *fill = t->accent;
        *ink = t->onAccent;
    } else if(seen) {
        *fill = t->accentSoft;
        *ink = t->accent;
    } else {
        *fill = t->palette->surface;
        *ink = t->palette->text;
    }
}

static void buttons_pill(bool present, bool held, bool seen, float x, float y, float w, float h, float radius, const char* label) {
    const theme_current* t = theme();
    u32 fill = 0;
    u32 ink = 0;
    buttons_state_colors(present, held, seen, &fill, &ink);
    screen_frame_round_rect(x, y, w, h, radius, t->def->border > 0 ? t->def->border : 1, held ? t->accent : t->palette->border, fill);
    if(label != NULL) {
        widget_text_fit(label, x + 4 + (w - 8 - widget_text_width(label, WIDGET_SMALL_SCALE) > 0 ? (w - 8 - widget_text_width(label, WIDGET_SMALL_SCALE)) / 2 : 0),
                        y + (h - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, ink, w - 8);
    }
}

static void buttons_key(const buttons_data* data, u32 key, bool present, float x, float y, float w, float h, float radius, const char* label) {
    buttons_pill(present, (data->held & key) != 0, (data->seen & key) != 0, x, y, w, h, radius, label);
}

// A stick: its four directions as marks around a circle (hollow until pushed that way), a cross at the centre and a dot
// where it is.
static void buttons_stick(const buttons_data* data, float cx, float cy, float r, const circlePosition* pos, u32 up, u32 down, u32 left, u32 right,
                          bool present, const char* label) {
    const theme_current* t = theme();
    screen_fill_circle(cx, cy, r + 1, present ? t->palette->border : THEME_ALPHA(t->palette->border, 0x60));
    screen_fill_circle(cx, cy, r, present ? t->palette->surface : THEME_ALPHA(t->palette->surface, 0x80));
    if(present) {
        screen_fill_rect(cx - r + 3, cy, 2 * r - 6, 1, THEME_ALPHA(t->palette->border, 0xC0));
        screen_fill_rect(cx, cy - r + 3, 1, 2 * r - 6, THEME_ALPHA(t->palette->border, 0xC0));
    }

    const struct {
        u32 key;
        float dx;
        float dy;
    } marks[] = {{up, 0, -1}, {down, 0, 1}, {left, -1, 0}, {right, 1, 0}};
    float markR = r >= 24 ? 4 : 3;
    for(u32 i = 0; i < COUNT_OF(marks); i++) {
        float mx = cx + marks[i].dx * (r - markR - 2);
        float my = cy + marks[i].dy * (r - markR - 2);
        if(!present) {
            screen_fill_circle(mx, my, markR, THEME_ALPHA(t->palette->border, 0x60));
        } else if(data->seen & marks[i].key) {
            screen_fill_circle(mx, my, markR, (data->held & marks[i].key) ? t->accent : THEME_ALPHA(t->accent, 0xA0));
        } else {
            screen_fill_circle(mx, my, markR, t->palette->textMuted);
            screen_fill_circle(mx, my, markR - 1.5f, t->palette->surface);
        }
    }

    if(present) {
        float dx = (float) pos->dx / STICK_RANGE;
        float dy = (float) -pos->dy / STICK_RANGE;
        if(dx > 1) dx = 1;
        if(dx < -1) dx = -1;
        if(dy > 1) dy = 1;
        if(dy < -1) dy = -1;
        float dot = r >= 24 ? 6 : 4;
        screen_fill_circle(cx + dx * (r - dot - 2), cy + dy * (r - dot - 2), dot, t->accent);
    }

    widget_text_centered(label, cx - r - 30, cx + r + 30, cy + r + 4, WIDGET_SMALL_SCALE, present ? t->palette->textMuted : THEME_ALPHA(t->palette->textMuted, 0x80));
}

// A stick's centre, magnified: a ring where rest is fine, the last 1.5 s as faint dots and where it is now, green inside the
// ring and amber outside, with its distance from the centre under it.
static void buttons_drift(const buttons_data* d, const circlePosition* history, const circlePosition* now, float x, float y, float size, bool present) {
    const theme_current* t = theme();
    screen_frame_round_rect(x, y, size, size, 4, 1, present ? t->palette->border : THEME_ALPHA(t->palette->border, 0x60),
                            present ? t->palette->surface : THEME_ALPHA(t->palette->surface, 0x80));
    if(!present) {
        return;
    }

    float cx = x + size / 2;
    float cy = y + size / 2;
    float scale = (size / 2 - 2) / DRIFT_SPAN;
    screen_fill_circle(cx, cy, DRIFT_OK * scale + 1, THEME_ALPHA(t->palette->success, 0x90));
    screen_fill_circle(cx, cy, DRIFT_OK * scale, THEME_ALPHA(t->palette->successSoft, 0xFF));
    screen_fill_rect(x + 2, cy, size - 4, 1, THEME_ALPHA(t->palette->border, 0xC0));
    screen_fill_rect(cx, y + 2, 1, size - 4, THEME_ALPHA(t->palette->border, 0xC0));

    float lim = DRIFT_SPAN;
    for(u32 n = 0; n < d->historyCount; n++) {
        float hx = history[n].dx;
        float hy = -history[n].dy;
        if(hx > lim || hx < -lim || hy > lim || hy < -lim) {
            continue;
        }

        screen_fill_rect(cx + hx * scale - 0.5f, cy + hy * scale - 0.5f, 1.5f, 1.5f, THEME_ALPHA(t->accent, 0x60));
    }

    float px = now->dx;
    float py = -now->dy;
    float dist = sqrtf(px * px + py * py);
    if(px > lim) px = lim;
    if(px < -lim) px = -lim;
    if(py > lim) py = lim;
    if(py < -lim) py = -lim;
    screen_fill_circle(cx + px * scale, cy + py * scale, 3, dist <= DRIFT_OK ? t->palette->success : t->palette->warning);

    char text[32];
    snprintf(text, sizeof(text), _("Drift %d"), (int) (dist + 0.5f));
    widget_text_centered(text, x - 14, x + size + 14, y + size + 2, WIDGET_SMALL_SCALE, dist <= DRIFT_OK ? t->palette->textMuted : t->palette->warning);
}

// A slider of the console's side (3D, volume): a vertical track filled to `fraction`, marks at the lowest and highest
// it has been, and its name as a pill that fills in once it has gone from one end to the other.
static void buttons_slider(float x, float y, float h, float fraction, float low, float high, bool present, bool done, const char* label, const char* value) {
    const theme_current* t = theme();
    float w = 10;
    screen_fill_round_rect(x, y, w, h, w / 2, THEME_ALPHA(t->palette->border, present ? 0xC0 : 0x50));
    if(present && fraction > 0) {
        float fh = h * (fraction > 1 ? 1 : fraction);
        screen_fill_round_rect(x, y + h - fh, w, fh, w / 2, t->accent);
    }

    if(present) {
        screen_fill_rect(x - 4, y + h - h * low - 1, 3, 2, t->palette->textMuted);
        screen_fill_rect(x - 4, y + h - h * high - 1, 3, 2, t->palette->textMuted);
    }

    float pw = 56;
    buttons_pill(present, false, done, x + w / 2 - pw / 2, y + h + 4, pw, 16, 8, label);
    widget_text_centered(value, x - 30, x + w + 30, y + h + 22, WIDGET_SMALL_SCALE, present ? t->palette->text : t->palette->textMuted);
}

static void buttons_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    buttons_data* d = (buttons_data*) data;
    const theme_current* t = theme();

    // Shoulders.
    float sy = y1 + 8;
    buttons_key(d, KEY_L, true, x1 + 10, sy, 74, 22, 8, "L");
    buttons_key(d, KEY_ZL, d->isNew, x1 + 90, sy, 44, 22, 8, "ZL");
    buttons_key(d, KEY_R, true, x2 - 84, sy, 74, 22, 8, "R");
    buttons_key(d, KEY_ZR, d->isNew, x2 - 134, sy, 44, 22, 8, "ZR");

    // How far the test is: the keys, the touch screen and the sliders.
    u32 all = buttons_all(d);
    u32 total = buttons_count(all) + (d->has3d ? 1 : 0) + (d->volumeKnown ? 1 : 0);
    u32 done = buttons_count(d->seen & all) + (d->has3d && buttons_slider_done(d) ? 1 : 0) + (buttons_volume_done(d) ? 1 : 0);
    char count[64];
    snprintf(count, sizeof(count), _("Pressed %lu of %lu"), done, total);
    bool complete = done == total;
    float bw = widget_text_width(count, WIDGET_SMALL_SCALE) + 12;
    float mid = (x1 + x2) / 2;
    widget_badge(mid - bw / 2, sy + 3, count, complete ? t->palette->successSoft : t->accentSoft, complete ? t->palette->success : t->accent);

    // The touch screen counts too: lit while touched.
    buttons_key(d, KEY_TOUCH, true, mid - 52, sy + 28, 104, 18, 9, _("Touch screen"));

    // Left: the Circle Pad (and its centre, magnified) over the D-Pad.
    float lx = x1 + 60;
    float padY = y1 + 84;
    buttons_stick(d, lx, padY, 30, &d->pad, KEY_CPAD_UP, KEY_CPAD_DOWN, KEY_CPAD_LEFT, KEY_CPAD_RIGHT, true, _("Circle Pad"));
    buttons_drift(d, d->padHistory, &d->pad, lx + 38, padY - 26, 36, true);

    float dpy = y1 + 166;
    float arm = 18;
    buttons_key(d, KEY_DUP, true, lx - arm / 2, dpy - arm * 1.5f, arm, arm, 4, NULL);
    buttons_key(d, KEY_DDOWN, true, lx - arm / 2, dpy + arm / 2, arm, arm, 4, NULL);
    buttons_key(d, KEY_DLEFT, true, lx - arm * 1.5f, dpy - arm / 2, arm, arm, 4, NULL);
    buttons_key(d, KEY_DRIGHT, true, lx + arm / 2, dpy - arm / 2, arm, arm, 4, NULL);
    screen_fill_rect(lx - arm / 2 + 1, dpy - arm / 2, arm - 2, arm, t->palette->border);
    widget_text_centered(_("D-Pad"), lx - 50, lx + 50, dpy + arm * 1.5f + 3, WIDGET_SMALL_SCALE, t->palette->textMuted);

    // Right: the C-Stick (and its centre) over A, B, X and Y.
    float rx = x2 - 60;
    float stickY = y1 + 72;
    buttons_stick(d, rx, stickY, 18, &d->stick, KEY_CSTICK_UP, KEY_CSTICK_DOWN, KEY_CSTICK_LEFT, KEY_CSTICK_RIGHT, d->isNew, _("C-Stick"));
    buttons_drift(d, d->stickHistory, &d->stick, rx - 18 - 28 - 36, stickY - 18, 36, d->isNew);

    float fy = y1 + 158;
    float fr = 12;
    float gap = 21;
    const struct {
        u32 key;
        float dx;
        float dy;
        const char* label;
    } faces[] = {{KEY_X, 0, -1, "X"}, {KEY_B, 0, 1, "B"}, {KEY_Y, -1, 0, "Y"}, {KEY_A, 1, 0, "A"}};
    for(u32 i = 0; i < COUNT_OF(faces); i++) {
        float cx = rx + faces[i].dx * gap;
        float cy = fy + faces[i].dy * gap;
        buttons_key(d, faces[i].key, true, cx - fr, cy - fr, fr * 2, fr * 2, fr, faces[i].label);
    }

    // B held to leave: a bar under the buttons fills up.
    if(d->backSince != 0) {
        float progress = (float) (osGetTime() - d->backSince) / BACK_HOLD;
        widget_progress(rx - 40, fy + gap + fr + 4, 80, 4, progress > 1 ? 1 : progress);
    }

    // Middle: the sliders, then SELECT and START.
    char value[32];
    if(d->has3d) {
        snprintf(value, sizeof(value), "%d %%", (int) (d->slider * 100 + 0.5f));
    } else {
        string_copy(value, "-", sizeof(value));
    }

    float slY = y1 + 64;
    float slH = 76;
    buttons_slider(mid - 34, slY, slH, d->slider, d->sliderMin, d->sliderMax, d->has3d, buttons_slider_done(d), _("3D"), value);
    if(d->volumeKnown) {
        snprintf(value, sizeof(value), "%d %%", (int) (d->volume * 100 / 63));
    } else {
        string_copy(value, "-", sizeof(value));
    }

    buttons_slider(mid + 24, slY, slH, d->volumeKnown ? d->volume / 63.0f : 0, d->volumeMin / 63.0f, d->volumeMax / 63.0f, d->volumeKnown,
                   buttons_volume_done(d), _("Volume"), value);

    float by = y2 - 28;
    buttons_key(d, KEY_SELECT, true, mid - 70, by, 64, 20, 10, "SELECT");
    buttons_key(d, KEY_START, true, mid + 6, by, 64, 20, 10, "START");
}

static void buttons_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    buttons_data* d = (buttons_data*) data;
    const theme_current* t = theme();
    u64 now = osGetTime();

    // A light grid, for straight lines and the corners.
    for(float gx = x1 + 40; gx < x2; gx += 40) {
        screen_fill_rect(gx, y1, 1, y2 - y1, THEME_ALPHA(t->palette->border, 0x50));
    }

    for(float gy = y1 + 40; gy < y2; gy += 40) {
        screen_fill_rect(x1, gy, x2 - x1, 1, THEME_ALPHA(t->palette->border, 0x50));
    }

    bool any = false;
    for(u32 i = 0; i < TRAIL_MAX; i++) {
        const trail_point* p = &d->trail[i];
        if(p->time == 0 || now - p->time >= TRAIL_LIFE) {
            continue;
        }

        any = true;
        u8 alpha = (u8) (0xFF * (1.0f - (float) (now - p->time) / TRAIL_LIFE));
        screen_fill_circle(p->x, p->y, 3, THEME_ALPHA(t->accent, alpha));
    }

    if(d->touching && !d->arrowTouched) {
        float px = d->touch.px;
        float py = d->touch.py;
        screen_fill_rect(x1, py, x2 - x1, 1, THEME_ALPHA(t->accent, 0x80));
        screen_fill_rect(px, y1, 1, y2 - y1, THEME_ALPHA(t->accent, 0x80));
        screen_fill_circle(px, py, 6, t->accent);

        char pos[32];
        snprintf(pos, sizeof(pos), "%d, %d", d->touch.px, d->touch.py);
        float w = widget_text_width(pos, WIDGET_SMALL_SCALE) + 12;
        float bx = px + 10 + w > x2 ? px - 10 - w : px + 10;
        float by = py - 22 < y1 ? py + 8 : py - 22;
        widget_badge(bx, by, pos, t->palette->surface, t->palette->text);
    } else if(!any) {
        widget_text_centered(_("Touch the screen anywhere"), x1, x2, (y1 + y2) / 2 - widget_text_height(WIDGET_TEXT_SCALE) / 2, WIDGET_TEXT_SCALE, t->palette->textMuted);
    }
}

static void buttons_draw_title_bar(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    buttons_data* d = (buttons_data*) data;
    hwtest_draw_title_bar(view, x1, y1, x2, y2, d->arrowTouched && d->touching && hwtest_arrow_at(&d->touch));
}

static void buttons_close(ui_view* view, buttons_data* data) {
    ui_pop();
    ui_destroy(view);
    free(data);
}

static void buttons_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    buttons_data* d = (buttons_data*) data;
    u64 now = osGetTime();

    u32 down = hidKeysDown();
    u32 held = hidKeysHeld();
    // What was held when the screen opened counts once it is let go and pressed again.
    d->ignore &= held;
    d->held = held & ~d->ignore;
    d->seen |= d->held;

    hidCircleRead(&d->pad);
    if(d->isNew) {
        hidCstickRead(&d->stick);
    }

    d->padHistory[d->historyNext] = d->pad;
    d->stickHistory[d->historyNext] = d->stick;
    d->historyNext = (d->historyNext + 1) % DRIFT_HISTORY;
    if(d->historyCount < DRIFT_HISTORY) {
        d->historyCount++;
    }

    d->slider = d->has3d ? osGet3DSliderState() : 0;
    if(d->slider < d->sliderMin) {
        d->sliderMin = d->slider;
    }

    if(d->slider > d->sliderMax) {
        d->sliderMax = d->slider;
    }

    bool known = R_SUCCEEDED(HIDUSER_GetSoundVolume(&d->volume));
    if(known && !d->volumeKnown) {
        d->volumeMin = d->volume;
        d->volumeMax = d->volume;
    }

    d->volumeKnown = known;
    if(known) {
        if(d->volume < d->volumeMin) {
            d->volumeMin = d->volume;
        }

        if(d->volume > d->volumeMax) {
            d->volumeMax = d->volume;
        }
    }

    touchPosition pos;
    hidTouchRead(&pos);
    if(down & KEY_TOUCH) {
        d->arrowTouched = hwtest_arrow_at(&pos);
    }

    if(held & KEY_TOUCH) {
        d->touch = pos;
        d->touching = true;
        if(!d->arrowTouched && pos.py >= by1 && pos.py < by2) {
            trail_point* p = &d->trail[d->trailNext];
            p->x = pos.px;
            p->y = pos.py;
            p->time = now;
            d->trailNext = (d->trailNext + 1) % TRAIL_MAX;
        }
    } else {
        // The finger left the back arrow where it went down: back.
        if(d->touching && d->arrowTouched && hwtest_arrow_at(&d->touch)) {
            buttons_close(view, d);
            return;
        }

        d->touching = false;
        d->arrowTouched = false;
    }

    if(held & KEY_B) {
        if(d->backSince == 0) {
            d->backSince = now;
        } else if(now - d->backSince >= BACK_HOLD) {
            buttons_close(view, d);
            return;
        }
    } else {
        d->backSince = 0;
    }

    // The sticks, the sliders and the fading trail move without keys.
    ui_wake();
}

void hwtest_buttons_open() {
    buttons_data* data = (buttons_data*) calloc(1, sizeof(buttons_data));
    if(data == NULL) {
        error_display(NULL, NULL, _("Failed to allocate buttons test data."));
        return;
    }

    data->isNew = hwtest_is_new3ds();
    u8 model = CFG_MODEL_3DS;
    CFGU_GetSystemModel(&model);
    data->has3d = model != CFG_MODEL_2DS && model != CFG_MODEL_N2DSXL;
    data->ignore = hidKeysHeld();
    data->slider = data->has3d ? osGet3DSliderState() : 0;
    data->sliderMin = data->slider;
    data->sliderMax = data->slider;

    ui_view* view = ui_create();
    view->name = _("Buttons and touch");
    view->id = "Buttons and touch";
    view->info = _("Hold  to go back");
    view->data = data;
    view->update = buttons_update;
    view->drawTop = buttons_draw_top;
    view->drawBottom = buttons_draw_bottom;
    view->drawTitleBar = buttons_draw_title_bar;
    ui_push(view);
}

// ---- motion sensors ----

#define MOTION_HISTORY 150    // Frames in the graphs: 2.5 s.
#define MOTION_LOG_DIR "sdmc:" APP_DATA_DIR "logs"
#define MOTION_G 512.0f       // Raw accelerometer units in one g (the console lying still reads 512).

typedef struct {
    s16 accel[MOTION_HISTORY][3];
    s16 gyro[MOTION_HISTORY][3];   // Roll (x), pitch (y), yaw (z).
    u32 count;
    u32 next;
    accelVector accelNow;
    angularRate gyroNow;
    float dps;                     // Raw gyroscope units per degree per second.

    FILE* log;
    char logPath[64];
    u64 logStart;
    u32 logRows;
    bool logFailed;
    char lastSaved[64];
    u32 lastRows;
    bool buttonTouched;
} motion_data;

static void motion_series_colors(u32 colors[3]) {
    const theme_current* t = theme();
    colors[0] = t->palette->danger;
    colors[1] = t->palette->warning;
    colors[2] = t->accent;
}

// One sensor: its three values, and a graph of the last frames.
static void motion_card(motion_data* d, float x, float y, float w, float h, const char* title, const char* names[3], const float values[3], const char* unit,
                        bool gyro) {
    const theme_current* t = theme();
    widget_card(x, y, w, h);
    screen_draw_text(title, x + 10, y + 8, WIDGET_TEXT_SCALE, t->palette->text);

    u32 colors[3];
    motion_series_colors(colors);
    float ry = y + 8 + widget_text_height(WIDGET_TEXT_SCALE) + 6;
    float rowH = widget_text_height(WIDGET_SMALL_SCALE) + 3;
    for(u32 i = 0; i < 3; i++) {
        screen_fill_circle(x + 14, ry + i * rowH + rowH / 2 - 1, 3, colors[i]);
        screen_draw_text(names[i], x + 22, ry + i * rowH, WIDGET_SMALL_SCALE, t->palette->textMuted);

        char value[32];
        snprintf(value, sizeof(value), gyro ? "%+.1f %s" : "%+.2f %s", (double) values[i], unit);
        float vw = widget_text_width(value, WIDGET_SMALL_SCALE);
        screen_draw_text(value, x + w - 10 - vw, ry + i * rowH, WIDGET_SMALL_SCALE, t->palette->text);
    }

    // The graph, scaled to what it holds (with a floor, so stillness stays flat).
    float gx = x + 8;
    float gy = ry + 3 * rowH + 6;
    float gw = w - 16;
    float gh = y + h - 8 - gy;
    screen_fill_round_rect(gx, gy, gw, gh, 4, THEME_ALPHA(t->palette->border, 0x30));
    screen_fill_rect(gx, gy + gh / 2, gw, 1, THEME_ALPHA(t->palette->border, 0xA0));

    float peak = gyro ? 20 * d->dps : MOTION_G / 8;
    for(u32 n = 0; n < d->count; n++) {
        for(u32 i = 0; i < 3; i++) {
            s16 v = gyro ? d->gyro[n][i] : d->accel[n][i];
            float a = v < 0 ? -v : v;
            if(a > peak) {
                peak = a;
            }
        }
    }

    float step = gw / (MOTION_HISTORY - 1);
    for(u32 n = 0; n < d->count; n++) {
        // Oldest first, from the left.
        u32 index = (d->next + MOTION_HISTORY - d->count + n) % MOTION_HISTORY;
        float px = gx + (MOTION_HISTORY - d->count + n) * step;
        for(u32 i = 0; i < 3; i++) {
            s16 v = gyro ? d->gyro[index][i] : d->accel[index][i];
            float py = gy + gh / 2 - (v / peak) * (gh / 2 - 2);
            screen_fill_rect(px - 1, py - 1, 2, 2, colors[i]);
        }
    }
}

static void motion_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    motion_data* d = (motion_data*) data;
    float gap = 8;
    float w = (x2 - x1 - 16 - gap) / 2;
    float h = y2 - y1 - 16;

    const char* accelNames[3] = {"X", "Y", "Z"};
    float accel[3] = {d->accelNow.x / MOTION_G, d->accelNow.y / MOTION_G, d->accelNow.z / MOTION_G};
    motion_card(d, x1 + 8, y1 + 8, w, h, _("Accelerometer"), accelNames, accel, "g", false);

    const char* gyroNames[3] = {_("Roll"), _("Pitch"), _("Yaw")};
    float gyro[3] = {d->gyroNow.x / d->dps, d->gyroNow.y / d->dps, d->gyroNow.z / d->dps};
    motion_card(d, x1 + 8 + w + gap, y1 + 8, w, h, _("Gyroscope"), gyroNames, gyro, "°/s", true);
}

#define MOTION_BUTTON_X 40
#define MOTION_BUTTON_W (BOTTOM_SCREEN_WIDTH - 80)
#define MOTION_BUTTON_H 36

static float motion_button_y(float y1) {
    return y1 + 16;
}

static void motion_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    motion_data* d = (motion_data*) data;
    const theme_current* t = theme();

    float by = motion_button_y(y1);
    if(d->log != NULL) {
        widget_button_danger(MOTION_BUTTON_X, by, MOTION_BUTTON_W, MOTION_BUTTON_H, ICON_PLAYER_PAUSE, _("Stop recording"), d->buttonTouched);
    } else {
        widget_button(MOTION_BUTTON_X, by, MOTION_BUTTON_W, MOTION_BUTTON_H, ICON_PLAYER_PLAY, _("Start recording"), d->buttonTouched);
    }

    coach_anchor("motion.record", MOTION_BUTTON_X, by, MOTION_BUTTON_W, MOTION_BUTTON_H, true);

    float cy = by + MOTION_BUTTON_H + 12;
    float lineH = widget_text_height(WIDGET_SMALL_SCALE) + 3;
    char line[128];
    if(d->log != NULL) {
        u64 ms = osGetTime() - d->logStart;
        snprintf(line, sizeof(line), _("Recording: %lu rows, %lu s"), d->logRows, (u32) (ms / 1000));
        widget_text_centered(line, x1 + 8, x2 - 8, cy, WIDGET_SMALL_SCALE, t->palette->danger);
        widget_text_centered(d->logPath + strlen("sdmc:"), x1 + 8, x2 - 8, cy + lineH, WIDGET_SMALL_SCALE, t->palette->textMuted);
        screen_draw_text_wrap_centered(_("It goes on with the lid closed. Leaving this screen stops it."), x1 + 16, cy + 2 * lineH + 6, WIDGET_SMALL_SCALE,
                                       t->palette->textMuted, x2 - 16);
    } else if(d->logFailed) {
        widget_text_centered(_("Could not write the file on the SD card."), x1 + 8, x2 - 8, cy, WIDGET_SMALL_SCALE, t->palette->danger);
    } else if(d->lastSaved[0] != '\0') {
        snprintf(line, sizeof(line), _("Saved: %lu rows"), d->lastRows);
        widget_text_centered(line, x1 + 8, x2 - 8, cy, WIDGET_SMALL_SCALE, t->palette->success);
        widget_text_centered(d->lastSaved + strlen("sdmc:"), x1 + 8, x2 - 8, cy + lineH, WIDGET_SMALL_SCALE, t->palette->textMuted);
    } else {
        screen_draw_text_wrap_centered(_("Records both sensors every frame to a CSV file in /nsa/logs on the SD card, with the time of each reading."), x1 + 16, cy, WIDGET_SMALL_SCALE,
                                       t->palette->textMuted, x2 - 16);
    }
}

static void motion_log_stop(motion_data* d) {
    if(d->log == NULL) {
        return;
    }

    bool ok = fflush(d->log) == 0;
    ok = fclose(d->log) == 0 && ok;
    d->log = NULL;
    aptSetSleepAllowed(true);

    if(ok) {
        string_copy(d->lastSaved, d->logPath, sizeof(d->lastSaved));
        d->lastRows = d->logRows;
    } else {
        d->logFailed = true;
    }
}

static void motion_log_start(motion_data* d) {
    mkdir("sdmc:" APP_DATA_DIR, 0777);
    mkdir(MOTION_LOG_DIR, 0777);

    time_t now = time(NULL);
    struct tm* tm = gmtime(&now);
    snprintf(d->logPath, sizeof(d->logPath), MOTION_LOG_DIR "/motion-%04d%02d%02d-%02d%02d%02d.csv", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    d->logFailed = false;
    d->log = fopen(d->logPath, "w");
    if(d->log == NULL) {
        d->logFailed = true;
        return;
    }

    setvbuf(d->log, NULL, _IOFBF, 32 * 1024);
    // Raw values as the HID service gives them, and the gyroscope also in degrees per second.
    fprintf(d->log, "ms,accel_x,accel_y,accel_z,gyro_roll,gyro_pitch,gyro_yaw,x_g,y_g,z_g,roll_dps,pitch_dps,yaw_dps\n");
    d->logStart = osGetTime();
    d->logRows = 0;
    // Sleep mode would stop the readings with the lid closed.
    aptSetSleepAllowed(false);
}

static void motion_log_row(motion_data* d) {
    if(d->log == NULL) {
        return;
    }

    u64 ms = osGetTime() - d->logStart;
    int written = fprintf(d->log, "%llu,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f\n", ms, d->accelNow.x, d->accelNow.y, d->accelNow.z, d->gyroNow.x,
                          d->gyroNow.y, d->gyroNow.z, (double) (d->accelNow.x / MOTION_G), (double) (d->accelNow.y / MOTION_G),
                          (double) (d->accelNow.z / MOTION_G), (double) (d->gyroNow.x / d->dps), (double) (d->gyroNow.y / d->dps),
                          (double) (d->gyroNow.z / d->dps));
    if(written < 0) {
        fclose(d->log);
        d->log = NULL;
        d->logFailed = true;
        aptSetSleepAllowed(true);
        return;
    }

    d->logRows++;
}

static void motion_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    motion_data* d = (motion_data*) data;

    if(hidKeysDown() & KEY_B) {
        motion_log_stop(d);
        HIDUSER_DisableGyroscope();
        HIDUSER_DisableAccelerometer();

        ui_pop();
        ui_destroy(view);
        free(d);
        return;
    }

    hidAccelRead(&d->accelNow);
    hidGyroRead(&d->gyroNow);
    d->accel[d->next][0] = d->accelNow.x;
    d->accel[d->next][1] = d->accelNow.y;
    d->accel[d->next][2] = d->accelNow.z;
    d->gyro[d->next][0] = d->gyroNow.x;
    d->gyro[d->next][1] = d->gyroNow.y;
    d->gyro[d->next][2] = d->gyroNow.z;
    d->next = (d->next + 1) % MOTION_HISTORY;
    if(d->count < MOTION_HISTORY) {
        d->count++;
    }

    motion_log_row(d);

    // The record button: A, or a tap that lifts where it went down.
    bool toggle = (hidKeysDown() & KEY_A) != 0;
    touchPosition pos;
    hidTouchRead(&pos);
    float by = motion_button_y(by1);
    bool on = widget_touched(&pos, MOTION_BUTTON_X, by, MOTION_BUTTON_W, MOTION_BUTTON_H);
    if(hidKeysDown() & KEY_TOUCH) {
        d->buttonTouched = on;
    } else if(hidKeysHeld() & KEY_TOUCH) {
        d->buttonTouched = d->buttonTouched && on;
    } else if(hidKeysUp() & KEY_TOUCH) {
        toggle = toggle || d->buttonTouched;
        d->buttonTouched = false;
    }

    if(toggle) {
        if(d->log != NULL) {
            motion_log_stop(d);
        } else {
            motion_log_start(d);
        }
    }

    view->info = d->log != NULL ? _(" stop recording     back") : _(" start recording     back");

    // The numbers and the graphs move every frame.
    ui_wake();
}

void hwtest_motion_open() {
    motion_data* data = (motion_data*) calloc(1, sizeof(motion_data));
    if(data == NULL) {
        error_display(NULL, NULL, _("Failed to allocate motion test data."));
        return;
    }

    HIDUSER_EnableAccelerometer();
    HIDUSER_EnableGyroscope();
    if(R_FAILED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&data->dps)) || data->dps <= 0) {
        data->dps = 14.375f;
    }

    ui_view* view = ui_create();
    view->name = _("Motion sensors");
    view->id = "Motion sensors";
    view->info = _(" start recording     back");
    view->data = data;
    view->update = motion_update;
    view->drawTop = motion_draw_top;
    view->drawBottom = motion_draw_bottom;
    view->backArrow = true;
    ui_push(view);
}
