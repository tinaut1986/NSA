#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "coach.h"
#include "ui.h"
#include "../i18n.h"
#include "../screen.h"
#include "../stringutil.h"
#include "../theme.h"

// See coach.h. The anchors of the frame being drawn are recorded in `cur`; when the frame ends they become `last`,
// which the input filter and the step logic read (the overlay draws from `cur`, so it follows what is on screen).

#define COACH_STEPS_MAX 160
#define COACH_ANCHORS_MAX 64
#define COACH_ID_MAX 64

#define COACH_MISSING_FRAMES 45      // An anchor not drawn for this long: the bubble offers to go on without it.
#define COACH_WAIT_LIMIT 8000        // Waiting this long after the action: the same.
#define COACH_REWIND_LIMIT 6000      // Going back to the main menu gives up after this long.

#define BUBBLE_PAD 8
#define BUBBLE_SCALE 0.5f
#define BUBBLE_SMALL 0.42f
#define BUBBLE_ARROW 7
#define HOLE_MARGIN 3

// Depth on the top screen in 3D (ui.c draws cards 1 unit in front of the screen and their icons 2): the dimming at the
// cards' depth, so the hole frames what it points at, and the bubble in front of everything.
#define COACH_DEPTH_DIM 1.0f
#define COACH_DEPTH_BUBBLE 3.0f

typedef struct {
    char id[COACH_ID_MAX];
    float x, y, w, h;
    bool selected;
    gfxScreen_t screen;
} coach_anchor_rec;

typedef enum {
    COACH_OFF,
    COACH_REWIND,
    COACH_STEP
} coach_phase;

static coach_step* steps = NULL;
static u32 stepCount = 0;
static u32 stepIndex = 0;
static void (*stepCallback)(u32 tag, bool last) = NULL;
static void (*endCallback)(bool finished) = NULL;

static coach_phase phase = COACH_OFF;
static u64 phaseStart = 0;
static u32 rewindFrames = 0;

static bool acted = false;          // The user did what the step asks.
static u64 actedTime = 0;
static u32 missingFrames = 0;
static bool touchInside = false;    // The finger on the screen went down on the anchor.
static bool onlyChecked = false;    // The step's `only` was looked at.
static float bubbleRect[4];         // The bubble on the bottom screen in the last frame (x, y, w, h), for a tap on it.
static bool bubbleOnBottom = false;
static u32 swallowKeys = 0;         // Held when the tour ended: kept from the views until let go (START would exit NSA).
static u32 freshKeys = 0;           // Pressed since the step began (a press that ended the last one does not count).

static u32 rawPrev = 0;
static u32 rawDown = 0;
static u32 rawHeld = 0;
static touchPosition rawTouch;

static coach_anchor_rec cur[COACH_ANCHORS_MAX];
static u32 curCount = 0;
static coach_anchor_rec last[COACH_ANCHORS_MAX];
static u32 lastCount = 0;
static gfxScreen_t recordScreen = GFX_BOTTOM;
static bool recording = false;

// An explanation: A goes on.
static bool coach_info_step(const coach_step* step) {
    return step->keys == 0 && !step->tap && step->until == NULL;
}

// Nothing to press, only something to reach (the focus on a row, with the D-Pad).
static bool coach_wait_step(const coach_step* step) {
    return step->keys == 0 && !step->tap && step->until != NULL;
}

// ---- anchors ----

void coach_anchor(const char* id, float x, float y, float width, float height, bool selected) {
    if(phase == COACH_OFF || !recording || id == NULL || curCount >= COACH_ANCHORS_MAX) {
        return;
    }

    coach_anchor_rec* a = &cur[curCount++];
    string_copy(a->id, id, sizeof(a->id));
    a->x = x;
    a->y = y;
    a->w = width;
    a->h = height;
    a->selected = selected;
    a->screen = recordScreen;
}

static const coach_anchor_rec* coach_find_in(const coach_anchor_rec* list, u32 count, const char* id) {
    if(id == NULL) {
        return NULL;
    }

    const char* translated = i18n_translate_text(id);
    const coach_anchor_rec* found = NULL;
    for(u32 i = 0; i < count; i++) {
        if(strcmp(list[i].id, id) == 0 || strcmp(list[i].id, translated) == 0) {
            found = &list[i];   // The last one drawn is on top.
        }
    }

    return found;
}

static bool coach_inside(const coach_anchor_rec* a, const touchPosition* pos) {
    return a != NULL && a->screen == GFX_BOTTOM && pos->px >= a->x && pos->px < a->x + a->w && pos->py >= a->y && pos->py < a->y + a->h;
}

// ---- steps ----

static const coach_step* coach_current() {
    return phase == COACH_STEP && stepIndex < stepCount ? &steps[stepIndex] : NULL;
}

static void coach_finish(bool finished) {
    void (*onEnd)(bool) = endCallback;
    swallowKeys = rawHeld;
    phase = COACH_OFF;
    free(steps);
    steps = NULL;
    stepCount = 0;
    curCount = 0;
    lastCount = 0;
    if(onEnd != NULL) {
        onEnd(finished);
    }
}

static void coach_begin_step() {
    acted = false;
    actedTime = 0;
    missingFrames = 0;
    touchInside = false;
    freshKeys = 0;
    onlyChecked = false;
    phaseStart = osGetTime();
    phase = steps[stepIndex].home && ui_depth() > 1 ? COACH_REWIND : COACH_STEP;
    rewindFrames = 0;
}

static void coach_next() {
    u32 tag = steps[stepIndex].tag;
    bool lastOfTag = stepIndex + 1 >= stepCount || steps[stepIndex + 1].tag != tag;
    if(stepCallback != NULL) {
        stepCallback(tag, lastOfTag);
    }

    stepIndex++;
    if(stepIndex >= stepCount) {
        coach_finish(true);
        return;
    }

    coach_begin_step();
}

void coach_start(const coach_step* list, u32 count, void (*onStep)(u32 tag, bool last), void (*onEnd)(bool finished)) {
    if(phase != COACH_OFF || count == 0) {
        if(onEnd != NULL) {
            onEnd(false);
        }

        return;
    }

    if(count > COACH_STEPS_MAX) {
        count = COACH_STEPS_MAX;
    }

    steps = (coach_step*) malloc(count * sizeof(coach_step));
    if(steps == NULL) {
        if(onEnd != NULL) {
            onEnd(false);
        }

        return;
    }

    memcpy(steps, list, count * sizeof(coach_step));
    stepCount = count;
    stepIndex = 0;
    stepCallback = onStep;
    endCallback = onEnd;
    curCount = 0;
    lastCount = 0;
    rawPrev = 0xFFFFFFFF;   // Whatever is held now does not count as pressed.
    coach_begin_step();
}

bool coach_active() {
    return phase != COACH_OFF;
}

// "v:<view>", "<anchor>", "!<anchor>" or "@<anchor>".
static bool coach_until_one(const char* until) {
    if(strncmp(until, "v:", 2) == 0) {
        ui_view* top = ui_top();
        const char* want = until + 2;
        return top != NULL && ((top->id != NULL && strcmp(top->id, want) == 0) || (top->name != NULL && strcmp(top->name, i18n_translate_text(want)) == 0));
    }

    if(until[0] == '!') {
        return coach_find_in(last, lastCount, until + 1) == NULL;
    }

    if(until[0] == '@') {
        const coach_anchor_rec* a = coach_find_in(last, lastCount, until + 1);
        return a != NULL && a->selected;
    }

    return coach_find_in(last, lastCount, until) != NULL;
}

// Any of the conditions separated by "|".
static bool coach_until_met(const char* until) {
    if(until == NULL) {
        return true;
    }

    char buf[128];
    string_copy(buf, until, sizeof(buf));
    char* rest = buf;
    while(rest != NULL) {
        char* bar = strchr(rest, '|');
        if(bar != NULL) {
            *bar = '\0';
        }

        if(coach_until_one(rest)) {
            return true;
        }

        rest = bar != NULL ? bar + 1 : NULL;
    }

    return false;
}

static bool coach_tap_on_bubble() {
    return (rawDown & KEY_TOUCH) && bubbleOnBottom && rawTouch.px >= bubbleRect[0] && rawTouch.px < bubbleRect[0] + bubbleRect[2]
           && rawTouch.py >= bubbleRect[1] && rawTouch.py < bubbleRect[1] + bubbleRect[3];
}

// What the step lets through now (before the touch is looked at).
static u32 coach_allowed_keys(const coach_step* step, const coach_anchor_rec* a) {
    if(coach_info_step(step) || (step->anchor != NULL && a == NULL && !acted && !coach_wait_step(step))) {
        return 0;
    }

    u32 allowed = step->pass | (step->keys & ~KEY_A);
    if((step->keys & KEY_A) && (step->anchor == NULL || (a != NULL && a->selected) || acted)) {
        allowed |= KEY_A;
    }

    return allowed & ~KEY_START;
}

u32 coach_filter(u32 held, const touchPosition* touch) {
    if(phase == COACH_OFF) {
        swallowKeys &= held;
        return held & ~swallowKeys;
    }

    rawDown = rawPrev == 0xFFFFFFFF ? 0 : held & ~rawPrev;
    rawPrev = held;
    rawHeld = held;
    rawTouch = *touch;
    freshKeys = (freshKeys | rawDown) & held;

    if(phase == COACH_REWIND) {
        // Only the B that takes it back (coach_update presses it).
        return held & KEY_B;
    }

    const coach_step* step = coach_current();
    if(step == NULL) {
        return 0;
    }

    const coach_anchor_rec* a = coach_find_in(last, lastCount, step->anchor);
    u32 allowed = coach_allowed_keys(step, a);

    // A finger counts when it went down on the anchor, until it lifts.
    if(rawDown & KEY_TOUCH) {
        touchInside = step->tap && coach_inside(a, touch);
    } else if(!(held & KEY_TOUCH)) {
        touchInside = false;
    }

    if(touchInside) {
        allowed |= KEY_TOUCH;
    }

    return held & allowed & freshKeys;
}

void coach_update() {
    if(phase == COACH_OFF) {
        return;
    }

    ui_wake();

    if(rawDown & KEY_START) {
        coach_finish(false);
        return;
    }

    if(phase == COACH_REWIND) {
        if(ui_depth() <= 1 || osGetTime() - phaseStart > COACH_REWIND_LIMIT) {
            phase = COACH_STEP;
            phaseStart = osGetTime();
        } else if(rewindFrames++ % 8 == 0) {
            input_inject_keys(KEY_B, 2);
        }

        return;
    }

    const coach_step* step = coach_current();
    if(step == NULL) {
        return;
    }

    // A step for a branch not taken is skipped.
    if(!onlyChecked) {
        onlyChecked = true;
        if(step->only != NULL && coach_find_in(last, lastCount, step->only) == NULL) {
            coach_next();
            return;
        }
    }

    const coach_anchor_rec* a = coach_find_in(last, lastCount, step->anchor);
    // A row to reach may be off the screen until the D-Pad brings it: that is not missing.
    missingFrames = step->anchor != NULL && a == NULL && !acted ? missingFrames + 1 : 0;
    bool stuck = missingFrames > COACH_MISSING_FRAMES || (acted && !coach_wait_step(step) && osGetTime() - actedTime > COACH_WAIT_LIMIT);

    if(coach_info_step(step) || stuck) {
        if((rawDown & KEY_A) || coach_tap_on_bubble()) {
            coach_next();
        }

        return;
    }

    if(!acted && coach_wait_step(step)) {
        acted = true;
        actedTime = osGetTime();
    }

    if(!acted) {
        u32 did = rawDown & step->keys & coach_allowed_keys(step, a);
        if(did != 0 || ((rawDown & KEY_TOUCH) && touchInside)) {
            acted = true;
            actedTime = osGetTime();
        }

        return;
    }

    if(step->until != NULL) {
        if(coach_until_met(step->until)) {
            coach_next();
        }

        return;
    }

    // Done when what did it is let go (views act on release).
    if(!(rawHeld & (step->keys | KEY_TOUCH))) {
        coach_next();
    }
}

// ---- drawing ----

void coach_frame_begin() {
    curCount = 0;
    bubbleOnBottom = false;
}

void coach_screen_begin(gfxScreen_t screen, bool record) {
    recordScreen = screen;
    recording = record;
}

// A triangle pointing up or down from the bubble's edge at (x, y), in rows of pixels.
static void coach_draw_arrow(float x, float y, bool up, u32 color) {
    for(int i = 0; i < BUBBLE_ARROW; i++) {
        float w = (float) (2 * (BUBBLE_ARROW - i) - 1);
        float row = up ? y - 1 - i : y + i;
        screen_fill_rect(x - w / 2, row, w, 1, color);
    }
}

// The bubble's contents and size for one screen.
typedef struct {
    char text[512];
    char footer[96];
    char counter[16];
    float innerW;
    float textH;
    float smallH;
    float bw;
    float bh;
} coach_bubble;

static void coach_bubble_measure(const coach_step* step, gfxScreen_t screen, coach_bubble* b) {
    bool stuck = missingFrames > COACH_MISSING_FRAMES;
    snprintf(b->counter, sizeof(b->counter), "%lu/%lu", stepIndex + 1, stepCount);
    snprintf(b->footer, sizeof(b->footer), "%s", coach_info_step(step) || stuck ? _("\uE000 go on   START leave") : _("START leave the guide"));
    snprintf(b->text, sizeof(b->text), "%s%s%s", i18n_translate_text(step->text), stuck ? "\n\n" : "", stuck ? _("It is not on the screen now.") : "");

    float maxW = (screen == GFX_TOP ? 300 : 264) - 2 * BUBBLE_PAD;
    float textW = 0;
    screen_get_string_size_wrap(&textW, &b->textH, b->text, BUBBLE_SCALE, BUBBLE_SCALE, maxW);
    float footerW = widget_text_width(b->footer, BUBBLE_SMALL) + widget_text_width(b->counter, BUBBLE_SMALL) + 16;
    float titleW = step->title != NULL ? widget_text_width(step->title, BUBBLE_SMALL) : 0;
    b->innerW = textW;
    if(footerW > b->innerW) b->innerW = footerW;
    if(titleW > b->innerW) b->innerW = titleW;
    if(b->innerW > maxW) b->innerW = maxW;

    // Measured again at the width it is drawn with: the widest line can wrap differently at its own width.
    screen_get_string_size_wrap(NULL, &b->textH, b->text, BUBBLE_SCALE, BUBBLE_SCALE, b->innerW + 1);

    b->smallH = widget_text_height(BUBBLE_SMALL);
    b->bw = b->innerW + 2 * BUBBLE_PAD;
    b->bh = BUBBLE_PAD + (step->title != NULL ? b->smallH + 4 : 0) + b->textH + 6 + b->smallH + BUBBLE_PAD;
}

// arrow: 0 none, 1 out of the top edge (pointing up), 2 out of the bottom edge (pointing down), at arrowX.
static void coach_bubble_draw(gfxScreen_t screen, const coach_step* step, const coach_bubble* b, float bx, float by, int arrow, float arrowX) {
    const theme_current* t = theme();
    screen_set_depth(screen == GFX_TOP ? COACH_DEPTH_BUBBLE : 0);
    if(screen == GFX_BOTTOM) {
        bubbleRect[0] = bx;
        bubbleRect[1] = by;
        bubbleRect[2] = b->bw;
        bubbleRect[3] = b->bh;
        bubbleOnBottom = true;
    }
    screen_fill_round_rect(bx + 1, by + 3, b->bw, b->bh, t->def->radius, THEME_ALPHA(0x00000000, 0x40));
    screen_frame_round_rect(bx, by, b->bw, b->bh, t->def->radius, 2, t->accent, t->palette->surface);
    if(arrow != 0) {
        if(arrowX < bx + 14) arrowX = bx + 14;
        if(arrowX > bx + b->bw - 14) arrowX = bx + b->bw - 14;
        coach_draw_arrow(arrowX, arrow == 1 ? by : by + b->bh, arrow == 1, t->accent);
    }

    float x = bx + BUBBLE_PAD;
    float y = by + BUBBLE_PAD;
    if(step->title != NULL) {
        widget_text_fit(step->title, x, y, BUBBLE_SMALL, t->accent, b->innerW);
        y += b->smallH + 4;
    }

    screen_draw_text_wrap(b->text, x, y, BUBBLE_SCALE, t->palette->text, x + b->innerW + 1);
    y += b->textH + 6;
    screen_draw_text(b->counter, x, y, BUBBLE_SMALL, t->palette->textMuted);
    float fw = widget_text_width(b->footer, BUBBLE_SMALL);
    screen_draw_text(b->footer, x + b->innerW - fw, y, BUBBLE_SMALL, t->palette->textMuted);
}

static float coach_screen_width(gfxScreen_t screen) {
    return screen == GFX_TOP ? TOP_SCREEN_WIDTH : BOTTOM_SCREEN_WIDTH;
}

static float coach_screen_height(gfxScreen_t screen) {
    return screen == GFX_TOP ? TOP_SCREEN_HEIGHT : BOTTOM_SCREEN_HEIGHT;
}

// Next to the anchor on its own screen: under it, or over it. False when neither has room.
static bool coach_place_beside(const coach_anchor_rec* a, const coach_bubble* b, float* by, bool* below) {
    float screenH = coach_screen_height(a->screen);
    float top = a->y - HOLE_MARGIN;
    float bottom = a->y + a->h + HOLE_MARGIN;
    float minY = a->screen == GFX_TOP ? WIDGET_TOP_BAR_HEIGHT + 2 : 4;
    if(bottom + BUBBLE_ARROW + b->bh + 4 <= screenH) {
        *by = bottom + BUBBLE_ARROW;
        *below = true;
        return true;
    }

    if(top - BUBBLE_ARROW - b->bh >= minY) {
        *by = top - BUBBLE_ARROW - b->bh;
        *below = false;
        return true;
    }

    return false;
}

void coach_draw(gfxScreen_t screen) {
    bool lastScreen = screen == GFX_BOTTOM;
    const coach_step* step = coach_current();
    if(step != NULL && !(acted && step->until != NULL && !coach_wait_step(step))) {
        const theme_current* t = theme();
        float screenW = coach_screen_width(screen);
        float screenH = coach_screen_height(screen);
        u32 dim = THEME_ALPHA(0x00000000, 0x8C);
        screen_set_offset(0, 0);
        screen_clear_clip();
        screen_set_depth(screen == GFX_TOP ? COACH_DEPTH_DIM : 0);

        // The anchor: as drawn now on this screen, else as last drawn (the top screen is drawn before the bottom one).
        const coach_anchor_rec* now = coach_find_in(cur, curCount, step->anchor);
        const coach_anchor_rec* before = coach_find_in(last, lastCount, step->anchor);
        const coach_anchor_rec* a = now != NULL && now->screen == screen ? now : before;
        if(a == NULL && now != NULL) {
            a = now;
        }

        // Everything on the anchor's screen dimmed but the anchor, framed in the accent; with no anchor, the bottom screen.
        if(a != NULL && a->screen == screen) {
            float hx = a->x - HOLE_MARGIN;
            float hy = a->y - HOLE_MARGIN;
            float hw = a->w + 2 * HOLE_MARGIN;
            float hh = a->h + 2 * HOLE_MARGIN;
            screen_fill_rect(0, 0, screenW, hy > 0 ? hy : 0, dim);
            screen_fill_rect(0, hy + hh, screenW, screenH - (hy + hh), dim);
            screen_fill_rect(0, hy, hx > 0 ? hx : 0, hh, dim);
            screen_fill_rect(hx + hw, hy, screenW - (hx + hw), hh, dim);

            float pulse = 0.5f + 0.5f * sinf((float) (osGetTime() % 100000) / 1000.0f * 5.0f);
            u32 ring = THEME_ALPHA(t->accent, (u32) (0x90 + pulse * 0x6F));
            screen_fill_rect(hx, hy, hw, 2, ring);
            screen_fill_rect(hx, hy + hh - 2, hw, 2, ring);
            screen_fill_rect(hx, hy + 2, 2, hh - 4, ring);
            screen_fill_rect(hx + hw - 2, hy + 2, 2, hh - 4, ring);
        } else if(a == NULL && screen == GFX_BOTTOM && !coach_wait_step(step)) {
            screen_fill_rect(0, 0, screenW, screenH, dim);
        }

        // The bubble: next to the anchor when there is room on its screen; else on the other screen, at the edge nearest
        // it, pointing at it; with no anchor, in the middle of the bottom screen.
        coach_bubble b;
        if(a != NULL) {
            coach_bubble_measure(step, a->screen, &b);
            float by = 0;
            bool below = true;
            if(coach_place_beside(a, &b, &by, &below)) {
                if(screen == a->screen) {
                    float bx = a->x + a->w / 2 - b.bw / 2;
                    if(bx < 6) bx = 6;
                    if(bx + b.bw > screenW - 6) bx = screenW - 6 - b.bw;
                    coach_bubble_draw(screen, step, &b, bx, by, below ? 1 : 2, a->x + a->w / 2);
                }
            } else if(screen != a->screen) {
                coach_bubble_measure(step, screen, &b);
                float bx = (screenW - b.bw) / 2;
                // The bottom screen is under the top one: from the top screen the arrow points down, and back up.
                bool pointDown = screen == GFX_TOP;
                float y = pointDown ? screenH - b.bh - BUBBLE_ARROW - 2 : BUBBLE_ARROW + 2;
                float ax = (a->x + a->w / 2) * screenW / coach_screen_width(a->screen);
                coach_bubble_draw(screen, step, &b, bx, y, pointDown ? 2 : 1, ax);
            }
        } else if(screen == GFX_BOTTOM) {
            coach_bubble_measure(step, screen, &b);
            float bx = (screenW - b.bw) / 2;
            // What a step waits for may not be on the screen yet: out of the way at the top (going down, the focus is at the bottom).
            float by = coach_wait_step(step) ? 30 : (screenH - b.bh) / 2;
            coach_bubble_draw(screen, step, &b, bx, by, 0, 0);
        }
    }

    screen_set_depth(0);

    if(lastScreen) {
        memcpy(last, cur, curCount * sizeof(coach_anchor_rec));
        lastCount = curCount;
        recording = false;
    }
}
