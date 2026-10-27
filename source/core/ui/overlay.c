#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "error.h"
#include "overlay.h"
#include "ui.h"
#include "../i18n.h"
#include "../icons.h"
#include "../screen.h"
#include "../stringutil.h"
#include "../theme.h"

// The view underneath keeps drawing its top screen, content and title bar.
static void overlay_draw_parent_top(ui_view* parent, float x1, float y1, float x2, float y2) {
    if(parent != NULL && parent->drawTop != NULL) {
        parent->drawTop(parent, parent->data, x1, y1, x2, y2);
    }
}

static void overlay_draw_parent_bottom(ui_view* parent, float x1, float y1, float x2, float y2) {
    if(parent != NULL && parent->drawBottom != NULL) {
        parent->drawBottom(parent, parent->data, x1, y1, x2, y2);
    }
}

static void overlay_draw_parent_title(ui_view* parent, float x1, float y1, float x2, float y2) {
    if(parent != NULL && parent->drawTitleBar != NULL) {
        parent->drawTitleBar(parent, parent->data, x1, y1, x2, y2);
    }
}

// ---- popover ----

#define POPOVER_WIDTH 236
#define POPOVER_PAD 10
#define POPOVER_OPTION_HEIGHT 24
#define POPOVER_CHIP_GAP 5
#define POPOVER_GROUP_GAP 8

typedef struct {
    ui_view* parent;
    popover_def def;
    void* data;
    u32 group;
    u32 option;

    // Layout, relative to the card's content top (before scrolling).
    float optionX[POPOVER_GROUPS_MAX][POPOVER_OPTIONS_MAX];
    float optionY[POPOVER_GROUPS_MAX][POPOVER_OPTIONS_MAX];
    float optionW[POPOVER_GROUPS_MAX][POPOVER_OPTIONS_MAX];
    float labelY[POPOVER_GROUPS_MAX];
    float contentHeight;

    float scroll;
    bool touching;
    bool dragged;
    u16 touchStartY;
    u16 lastTouchY;
    widget_press press;   // Index: group * POPOVER_OPTIONS_MAX + option.
    u64 opened;           // For the card's entrance.
} popover_data;

static float popover_chip_width(popover_group* group, u32 o) {
    return widget_text_width(group->options[o], WIDGET_SMALL_SCALE) + (group->icons[o] >= 0 ? 14 : 0) + 20;
}

// Chips flow onto more lines when a group has many; a segmented choice
// shares one line.
static void popover_layout(popover_data* p) {
    float inner = POPOVER_WIDTH - 2 * POPOVER_PAD - WIDGET_SCROLLBAR_WIDTH - 2;
    float labelH = widget_text_height(WIDGET_SMALL_SCALE) + 4;
    float y = 0;

    for(u32 g = 0; g < p->def.groupCount; g++) {
        popover_group* group = &p->def.groups[g];
        p->labelY[g] = y;
        y += labelH;

        if(group->multiple) {
            float x = 0;
            for(u32 o = 0; o < group->count; o++) {
                float w = popover_chip_width(group, o);
                if(w > inner) {
                    w = inner;
                }

                if(x > 0 && x + w > inner) {
                    x = 0;
                    y += POPOVER_OPTION_HEIGHT + POPOVER_CHIP_GAP;
                }

                p->optionX[g][o] = x;
                p->optionY[g][o] = y;
                p->optionW[g][o] = w;
                x += w + POPOVER_CHIP_GAP;
            }
        } else {
            float w = inner / group->count;
            for(u32 o = 0; o < group->count; o++) {
                p->optionX[g][o] = o * w;
                p->optionY[g][o] = y;
                p->optionW[g][o] = w;
            }
        }

        y += POPOVER_OPTION_HEIGHT + POPOVER_GROUP_GAP;
    }

    p->contentHeight = y - POPOVER_GROUP_GAP;
}

static void popover_card(popover_data* p, float x1, float y1, float x2, float y2, float* cardX, float* cardY, float* cardH) {
    *cardX = x2 - 8 - POPOVER_WIDTH;
    *cardY = y1 + 4;
    float max = y2 - y1 - 8;
    *cardH = p->contentHeight + 2 * POPOVER_PAD;
    if(*cardH > max) {
        *cardH = max;
    }
}

static void popover_clamp_scroll(popover_data* p, float cardH) {
    float max = p->contentHeight - (cardH - 2 * POPOVER_PAD);
    if(p->scroll > max) {
        p->scroll = max;
    }

    if(p->scroll < 0) {
        p->scroll = 0;
    }
}

// Scrolls so the focused option, and its group's label, are in view.
static void popover_reveal(popover_data* p, float cardH) {
    float view = cardH - 2 * POPOVER_PAD;
    float top = p->option == 0 ? p->labelY[p->group] : p->optionY[p->group][p->option];
    float bottom = p->optionY[p->group][p->option] + POPOVER_OPTION_HEIGHT;
    if(top < p->scroll) {
        p->scroll = top;
    } else if(bottom > p->scroll + view) {
        p->scroll = bottom - view;
    }

    popover_clamp_scroll(p, cardH);
}

static void popover_close(ui_view* view, popover_data* p) {
    ui_pop();
    free(p);
    ui_destroy(view);
}

static void popover_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    popover_data* p = (popover_data*) data;

    view->name = p->parent->name;
    view->info = _(" choose    close");

    float cardX, cardY, cardH;
    popover_card(p, bx1, by1, bx2, by2, &cardX, &cardY, &cardH);

    u32 down = hidKeysDown();
    if(down & KEY_B) {
        popover_close(view, p);
        return;
    }

    bool moved = true;
    if(down & KEY_DOWN) {
        p->group = (p->group + 1) % p->def.groupCount;
        p->option = 0;
    } else if(down & KEY_UP) {
        p->group = (p->group + p->def.groupCount - 1) % p->def.groupCount;
        p->option = 0;
    } else if(down & KEY_RIGHT) {
        p->option = (p->option + 1) % p->def.groups[p->group].count;
    } else if(down & KEY_LEFT) {
        u32 count = p->def.groups[p->group].count;
        p->option = (p->option + count - 1) % count;
    } else {
        moved = false;
    }

    if(moved) {
        popover_reveal(p, cardH);
    }

    if(down & KEY_A) {
        p->def.select(p->data, p->group, p->option);
    }

    // Touch: a tap toggles an option, a drag scrolls, outside the card closes it.
    touchPosition pos;
    hidTouchRead(&pos);
    float contentTop = cardY + POPOVER_PAD - p->scroll;
    if(down & KEY_TOUCH) {
        if(!widget_touched(&pos, cardX, cardY, POPOVER_WIDTH, cardH)) {
            popover_close(view, p);
            return;
        }

        p->touching = true;
        p->dragged = false;
        p->touchStartY = p->lastTouchY = pos.py;
        widget_press_end(&p->press);
        for(u32 g = 0; g < p->def.groupCount; g++) {
            for(u32 o = 0; o < p->def.groups[g].count; o++) {
                float x = cardX + POPOVER_PAD + p->optionX[g][o];
                float y = contentTop + p->optionY[g][o];
                if(widget_touched(&pos, x, y, p->optionW[g][o], POPOVER_OPTION_HEIGHT)) {
                    widget_press_begin(&p->press, (int) (g * POPOVER_OPTIONS_MAX + o), true, pos.px, pos.py);
                }
            }
        }
    } else if(p->touching && (hidKeysHeld() & KEY_TOUCH)) {
        if(abs((int) pos.py - (int) p->touchStartY) > 6) {
            p->dragged = true;
            p->press.cancelled = true;
        }

        if(p->press.index >= 0) {
            u32 g = (u32) p->press.index / POPOVER_OPTIONS_MAX;
            u32 o = (u32) p->press.index % POPOVER_OPTIONS_MAX;
            if(!widget_touched(&pos, cardX + POPOVER_PAD + p->optionX[g][o], contentTop + p->optionY[g][o], p->optionW[g][o], POPOVER_OPTION_HEIGHT)) {
                p->press.cancelled = true;
            }
        }

        if(p->dragged) {
            p->scroll -= (int) pos.py - (int) p->lastTouchY;
            popover_clamp_scroll(p, cardH);
        }

        p->lastTouchY = pos.py;
    } else if(p->touching && (hidKeysUp() & KEY_TOUCH)) {
        p->touching = false;
        if(widget_press_on(&p->press, p->press.index)) {
            u32 g = (u32) p->press.index / POPOVER_OPTIONS_MAX;
            u32 o = (u32) p->press.index % POPOVER_OPTIONS_MAX;
            p->group = g;
            p->option = o;
            p->def.select(p->data, g, o);
        }

        widget_press_end(&p->press);
    }
}

static void popover_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    overlay_draw_parent_top(((popover_data*) data)->parent, x1, y1, x2, y2);
}

static void popover_draw_title(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    overlay_draw_parent_title(((popover_data*) data)->parent, x1, y1, x2, y2);
}

static void popover_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    popover_data* p = (popover_data*) data;
    const theme_current* t = theme();

    overlay_draw_parent_bottom(p->parent, x1, y1, x2, y2);

    // The card drops a little from the title bar and fades in.
    float e = widget_ease(p->opened, 160);
    screen_fill_rect(x1, y1, x2 - x1, y2 - y1, THEME_ALPHA(t->palette->background, (u32) (0xA0 * e)));

    float cardX, cardY, cardH;
    popover_card(p, x1, y1, x2, y2, &cardX, &cardY, &cardH);
    popover_clamp_scroll(p, cardH);

    screen_set_offset(0, (e - 1) * 10);
    screen_set_base_alpha((u8) (0x30 + e * 0xCF));

    screen_fill_round_rect(cardX + 1, cardY + 3, POPOVER_WIDTH, cardH, t->def->radius, THEME_ALPHA(0, 0x30));
    screen_frame_round_rect(cardX, cardY, POPOVER_WIDTH, cardH, t->def->radius, t->def->border, t->palette->border, t->palette->surface);

    float contentTop = cardY + POPOVER_PAD - p->scroll;
    screen_set_clip(cardX, cardY + 3, POPOVER_WIDTH, cardH - 6);

    for(u32 g = 0; g < p->def.groupCount; g++) {
        popover_group* group = &p->def.groups[g];
        float baseX = cardX + POPOVER_PAD;
        screen_draw_text(group->label, baseX, contentTop + p->labelY[g], WIDGET_SMALL_SCALE, t->palette->textMuted);

        if(!group->multiple) {
            // A segmented control: one frame around all the options.
            float width = p->optionX[g][group->count - 1] + p->optionW[g][group->count - 1];
            screen_frame_round_rect(baseX, contentTop + p->optionY[g][0], width, POPOVER_OPTION_HEIGHT, t->def->radiusSmall, t->def->border, t->palette->border, t->palette->surface);
        }

        for(u32 o = 0; o < group->count; o++) {
            float x = baseX + p->optionX[g][o];
            float y = contentTop + p->optionY[g][o];
            float w = p->optionW[g][o];
            float radius = group->multiple ? POPOVER_OPTION_HEIGHT / 2 : t->def->radiusSmall;
            bool on = p->def.isOn(p->data, g, o);
            bool focused = g == p->group && o == p->option;
            u32 ink = on ? t->onAccent : t->palette->text;

            if(focused) {
                // The D-Pad cursor: a ring around the option.
                screen_fill_round_rect(x - 2, y - 2, w + 4, POPOVER_OPTION_HEIGHT + 4, radius + 2, t->palette->text);
                screen_fill_round_rect(x, y, w, POPOVER_OPTION_HEIGHT, radius, t->palette->surface);
            }

            if(on) {
                screen_fill_round_rect(x, y, w, POPOVER_OPTION_HEIGHT, radius, t->accent);
            } else if(group->multiple && !focused) {
                screen_frame_round_rect(x, y, w, POPOVER_OPTION_HEIGHT, radius, t->def->border, t->palette->border, t->palette->surface);
            }

            float textW = widget_text_width(group->options[o], WIDGET_SMALL_SCALE);
            float iconW = group->icons[o] >= 0 ? 14 : 0;
            float cx = x + (w - textW - iconW) / 2;
            float ty = y + (POPOVER_OPTION_HEIGHT - widget_text_height(WIDGET_SMALL_SCALE)) / 2;
            if(iconW > 0) {
                screen_draw_icon((u32) group->icons[o], cx - 2, y + (POPOVER_OPTION_HEIGHT - 12) / 2, 12, ink);
            }

            widget_text_fit(group->options[o], cx + iconW, ty, WIDGET_SMALL_SCALE, ink, w - iconW - 8);
            widget_press_draw(&p->press, (int) (g * POPOVER_OPTIONS_MAX + o), x, y, w, POPOVER_OPTION_HEIGHT, radius, ink, false);
        }
    }

    screen_clear_clip();

    float visible = cardH - 2 * POPOVER_PAD;
    widget_scrollbar(cardX + POPOVER_WIDTH - POPOVER_PAD / 2 - WIDGET_SCROLLBAR_WIDTH, cardY + POPOVER_PAD, visible, p->scroll, visible, p->contentHeight);

    screen_set_offset(0, 0);
    screen_set_base_alpha(0xFF);
}

void popover_display(const popover_def* def, void* data) {
    popover_data* p = (popover_data*) calloc(1, sizeof(popover_data));
    if(p == NULL) {
        error_display(NULL, NULL, "Failed to allocate popover.");
        return;
    }

    p->parent = ui_top();
    p->press.index = -1;
    p->opened = osGetTime();
    p->def = *def;
    p->data = data;
    popover_layout(p);

    ui_view* view = ui_create();
    view->name = p->parent != NULL ? p->parent->name : "";
    view->info = _(" choose    close");
    view->data = p;
    view->update = popover_update;
    view->drawTop = popover_draw_top;
    view->drawBottom = popover_draw_bottom;
    view->drawTitleBar = popover_draw_title;
    ui_push(view);
}

// ---- sheet ----

#define SHEET_COLUMNS 4
#define SHEET_BUTTON_HEIGHT 50
#define SHEET_GAP 6
#define SHEET_PAD 10
#define SHEET_HEADER 34

// The header (icon and title) only when the caller gives a title.
#define SHEET_HEADER_OF(s) ((s)->title[0] != '\0' ? SHEET_HEADER : 0)

typedef struct {
    ui_view* parent;
    char title[128];
    u32 texture;
    sheet_action actions[SHEET_ACTIONS_MAX];
    u32 count;
    void* data;
    u32 selected;
    widget_press press;   // Touched action, run on release over it.
    bool dragged;         // The touch turned into a scroll (list).
    u16 touchStartY;
    u16 lastTouchY;
    u64 opened;           // For the slide up.
    bool list;     // One row per action instead of a grid of buttons.
    float scroll;  // List only, when the rows do not all fit.
} sheet_data;

static u32 sheet_columns(sheet_data* s) {
    return s->list ? 1 : SHEET_COLUMNS;
}

static float sheet_room(sheet_data* s) {
    return BOTTOM_SCREEN_HEIGHT - WIDGET_TITLE_BAR_HEIGHT - WIDGET_HINT_BAR_HEIGHT - 8 - 2 * SHEET_PAD - SHEET_HEADER_OF(s);
}

// Buttons shrink when there are many rows, so the sheet fits under the title bar.
// Grid buttons shrink when there are many rows, so the sheet fits under the
// title bar; list rows keep their height and scroll instead.
static float sheet_button_height(sheet_data* s) {
    if(s->list) {
        return WIDGET_ROW_HEIGHT;
    }

    u32 rows = (s->count + SHEET_COLUMNS - 1) / SHEET_COLUMNS;
    float h = (sheet_room(s) + SHEET_GAP) / rows - SHEET_GAP;
    return h < SHEET_BUTTON_HEIGHT ? h : SHEET_BUTTON_HEIGHT;
}

static float sheet_gap(sheet_data* s) {
    return s->list ? WIDGET_ROW_GAP : SHEET_GAP;
}

static float sheet_content_height(sheet_data* s) {
    u32 rows = (s->count + sheet_columns(s) - 1) / sheet_columns(s);
    return rows * (sheet_button_height(s) + sheet_gap(s)) - sheet_gap(s);
}

static float sheet_visible_height(sheet_data* s) {
    float content = sheet_content_height(s);
    return content < sheet_room(s) ? content : sheet_room(s);
}

static float sheet_height(sheet_data* s) {
    return SHEET_PAD + SHEET_HEADER_OF(s) + sheet_visible_height(s) + SHEET_PAD;
}

static void sheet_button_rect(sheet_data* s, u32 i, float x1, float x2, float top, float* x, float* y, float* w) {
    u32 cols = sheet_columns(s);
    float scrollbar = s->list && sheet_content_height(s) > sheet_room(s) ? WIDGET_SCROLLBAR_WIDTH + 6 : 0;
    *w = (x2 - x1 - 2 * SHEET_PAD - scrollbar - (cols - 1) * SHEET_GAP) / cols;
    *x = x1 + SHEET_PAD + (i % cols) * (*w + SHEET_GAP);
    *y = top + SHEET_PAD + SHEET_HEADER_OF(s) + (i / cols) * (sheet_button_height(s) + sheet_gap(s)) - s->scroll;
}

// Keeps the selected row in view (list).
static void sheet_reveal(sheet_data* s) {
    float pitch = sheet_button_height(s) + sheet_gap(s);
    float y = (s->selected / sheet_columns(s)) * pitch;
    float view = sheet_visible_height(s);
    if(y < s->scroll) {
        s->scroll = y;
    } else if(y + sheet_button_height(s) > s->scroll + view) {
        s->scroll = y + sheet_button_height(s) - view;
    }
}

static void sheet_close(ui_view* view, sheet_data* s) {
    ui_pop();
    free(s);
    ui_destroy(view);
}

static void sheet_run(ui_view* view, sheet_data* s, u32 index) {
    void (*run)(void*) = s->actions[index].run;
    void* data = s->data;
    sheet_close(view, s);
    if(run != NULL) {
        run(data);
    }
}

static void sheet_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    sheet_data* s = (sheet_data*) data;
    u32 down = hidKeysDown();

    if(down & KEY_B) {
        sheet_close(view, s);
        return;
    }

    u32 cols = sheet_columns(s);
    u32 last = s->selected;
    if((down & KEY_RIGHT) && cols > 1) {
        s->selected = (s->selected + 1) % s->count;
    } else if((down & KEY_LEFT) && cols > 1) {
        s->selected = (s->selected + s->count - 1) % s->count;
    } else if(down & KEY_DOWN) {
        s->selected = s->selected + cols < s->count ? s->selected + cols : s->selected % cols;
    } else if(down & KEY_UP) {
        if(s->selected >= cols) {
            s->selected -= cols;
        } else {
            u32 lastRow = ((s->count - 1) / cols) * cols;
            s->selected = lastRow + s->selected < s->count ? lastRow + s->selected : s->count - 1;
        }
    } else if(down & KEY_A) {
        sheet_run(view, s, s->selected);
        return;
    }

    if(s->selected != last) {
        sheet_reveal(s);
    }

    touchPosition pos;
    hidTouchRead(&pos);
    float top = by2 - sheet_height(s);
    float listTop = top + SHEET_PAD + SHEET_HEADER_OF(s);
    if(down & KEY_TOUCH) {
        widget_press_end(&s->press);
        s->dragged = false;
        s->lastTouchY = pos.py;
        if(pos.py < top) {
            sheet_close(view, s);
            return;
        }

        for(u32 i = 0; i < s->count; i++) {
            float x, y, w;
            sheet_button_rect(s, i, bx1, bx2, top, &x, &y, &w);
            if(pos.py >= listTop && widget_touched(&pos, x, y, w, sheet_button_height(s))) {
                s->selected = i;
                widget_press_begin(&s->press, (int) i, true, pos.px, pos.py);
            }
        }

        s->touchStartY = pos.py;
    } else if(hidKeysHeld() & KEY_TOUCH) {
        // A list scrolls under the finger; moving off an action cancels it.
        if(s->list && sheet_content_height(s) > sheet_room(s) && (s->dragged || abs((int) pos.py - (int) s->touchStartY) > 6)) {
            s->dragged = true;
            s->press.cancelled = true;
            s->scroll -= (int) pos.py - (int) s->lastTouchY;
            float max = sheet_content_height(s) - sheet_visible_height(s);
            if(s->scroll > max) s->scroll = max;
            if(s->scroll < 0) s->scroll = 0;
        }

        if(s->press.index >= 0) {
            float x, y, w;
            sheet_button_rect(s, (u32) s->press.index, bx1, bx2, top, &x, &y, &w);
            if(!widget_touched(&pos, x, y, w, sheet_button_height(s))) {
                s->press.cancelled = true;
            }
        }

        s->lastTouchY = pos.py;
    } else if(hidKeysUp() & KEY_TOUCH) {
        int index = s->press.index;
        bool run = widget_press_on(&s->press, index);
        widget_press_end(&s->press);
        if(run) {
            sheet_run(view, s, (u32) index);
            return;
        }
    }
}

static void sheet_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    overlay_draw_parent_top(((sheet_data*) data)->parent, x1, y1, x2, y2);
}

static void sheet_draw_title(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    overlay_draw_parent_title(((sheet_data*) data)->parent, x1, y1, x2, y2);
}

static void sheet_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    sheet_data* s = (sheet_data*) data;
    const theme_current* t = theme();

    overlay_draw_parent_bottom(s->parent, x1, y1, x2, y2);

    // The scrim darkens in while the sheet slides up from the bottom.
    float e = widget_ease(s->opened, 200);
    screen_fill_rect(x1, y1, x2 - x1, y2 - y1, THEME_ALPHA(t->palette->scrim, (u32) (((t->palette->scrim >> 24) & 0xFF) * e)));
    float slide = (1 - e) * (sheet_height(s) + WIDGET_HINT_BAR_HEIGHT);
    screen_set_offset(0, slide);

    // The sheet runs into the hint bar: only its top corners are round.
    float top = y2 - sheet_height(s);
    float r = t->def->radius + 4;
    screen_fill_round_rect(x1, top, x2 - x1, y2 - top + r, r, t->palette->surface);
    screen_fill_rect(x1, top + r, x2 - x1, 1, t->palette->surface);

    if(s->title[0] != '\0') {
        float hx = x1 + SHEET_PAD;
        float hy = top + SHEET_PAD;
        if(s->texture != 0) {
            screen_draw_texture(s->texture, hx, hy, 26, 26);
            hx += 32;
        }

        widget_text_fit(s->title, hx, hy + (26 - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, t->palette->text, x2 - SHEET_PAD - 24 - hx);
        screen_draw_icon(ICON_X, x2 - SHEET_PAD - 16, hy + 5, 16, t->palette->textMuted);
    }

    float listTop = top + SHEET_PAD + SHEET_HEADER_OF(s);
    if(s->list) {
        screen_set_clip(x1, listTop - 2 + slide, x2 - x1, sheet_visible_height(s) + 4);
    }

    for(u32 i = 0; i < s->count; i++) {
        sheet_action* a = &s->actions[i];
        float x, y, w;
        sheet_button_rect(s, i, x1, x2, top, &x, &y, &w);

        float h = sheet_button_height(s);
        if(s->list) {
            if(y + h < listTop || y > listTop + sheet_visible_height(s)) {
                continue;
            }

            // A row: icon and the whole label; the focused one is filled with the accent.
            bool focused = i == s->selected;
            // The night palette's red is light: dark text on it reads better.
            u32 onDanger = t->night ? t->palette->background : t->onAccent;
            u32 ink = focused ? (a->danger ? onDanger : t->onAccent) : a->danger ? t->palette->danger : t->palette->text;
            u32 fill = focused ? (a->danger ? t->palette->danger : t->accent) : a->danger ? theme_mix(t->palette->surface, t->palette->danger, 0x26) : t->palette->surface;
            u32 edge = focused ? fill : a->danger ? t->palette->danger : t->palette->border;
            screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, edge, fill);
            screen_draw_icon(a->icon, x + 9, y + (h - 16) / 2, 16, ink);
            widget_text_fit(a->label, x + 31, y + (h - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink, w - 40);
            widget_press_draw(&s->press, (int) i, x, y, w, h, t->def->radiusSmall, ink, false);
            continue;
        }

        bool focused = i == s->selected;
        u32 ink = a->danger ? t->palette->danger : t->palette->text;
        u32 fill = a->danger ? theme_mix(t->palette->surface, t->palette->danger, 0x26) : t->palette->surface;
        u32 edge = focused ? t->accent : a->danger ? t->palette->danger : t->palette->border;

        screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, focused ? 2 : t->def->border, edge, fill);
        widget_press_draw(&s->press, (int) i, x, y, w, h, t->def->radiusSmall, ink, false);

        // A label too wide for the button goes on two lines, split at the
        // space closest to its middle.
        float scale = WIDGET_SMALL_SCALE;
        float textH = widget_text_height(scale);
        char line1[64];
        char line2[64];
        string_copy(line1, a->label, sizeof(line1));
        line2[0] = '\0';
        if(widget_text_width(a->label, scale) > w - 6) {
            const char* best = NULL;
            size_t len = strlen(a->label);
            for(const char* c = a->label; *c; c++) {
                if(*c == ' ' && (best == NULL || abs((int) (c - a->label) - (int) len / 2) < abs((int) (best - a->label) - (int) len / 2))) {
                    best = c;
                }
            }

            if(best != NULL && (size_t) (best - a->label) < sizeof(line1)) {
                memcpy(line1, a->label, best - a->label);
                line1[best - a->label] = '\0';
                string_copy(line2, best + 1, sizeof(line2));
            }
        }

        u32 lines = line2[0] != '\0' ? 2 : 1;
        float iconY = y + (h - 18 - 2 - lines * textH) / 2;
        screen_draw_icon(a->icon, x + (w - 18) / 2, iconY, 18, ink);
        float labelY = iconY + 20;
        widget_text_fit(line1, x + 3 + (w - 6 - (widget_text_width(line1, scale) < w - 6 ? widget_text_width(line1, scale) : w - 6)) / 2, labelY, scale, ink, w - 6);
        if(lines == 2) {
            widget_text_fit(line2, x + 3 + (w - 6 - (widget_text_width(line2, scale) < w - 6 ? widget_text_width(line2, scale) : w - 6)) / 2, labelY + textH, scale, ink, w - 6);
        }
    }

    if(s->list) {
        screen_clear_clip();
        widget_scrollbar(x2 - SHEET_PAD - WIDGET_SCROLLBAR_WIDTH, listTop, sheet_visible_height(s), s->scroll, sheet_visible_height(s), sheet_content_height(s));
    }

    screen_set_offset(0, 0);
}

static void sheet_open(const char* title, u32 texture, const sheet_action* actions, u32 count, void* data, bool list) {
    if(count == 0) {
        return;
    }

    sheet_data* s = (sheet_data*) calloc(1, sizeof(sheet_data));
    if(s == NULL) {
        error_display(NULL, NULL, "Failed to allocate actions.");
        return;
    }

    s->parent = ui_top();
    string_copy(s->title, title != NULL ? title : "", sizeof(s->title));
    s->texture = texture;
    s->count = count < SHEET_ACTIONS_MAX ? count : SHEET_ACTIONS_MAX;
    memcpy(s->actions, actions, s->count * sizeof(sheet_action));
    s->data = data;
    s->press.index = -1;
    s->opened = osGetTime();
    s->list = list;

    ui_view* view = ui_create();
    view->name = s->parent != NULL ? s->parent->name : "";
    view->info = _(" run    close");
    view->data = s;
    view->update = sheet_update;
    view->drawTop = sheet_draw_top;
    view->drawBottom = sheet_draw_bottom;
    view->drawTitleBar = sheet_draw_title;
    ui_push(view);
}

void sheet_display(const char* title, u32 texture, const sheet_action* actions, u32 count, void* data) {
    sheet_open(title, texture, actions, count, data, false);
}

void sheet_display_list(const char* title, u32 texture, const sheet_action* actions, u32 count, void* data) {
    sheet_open(title, texture, actions, count, data, true);
}
