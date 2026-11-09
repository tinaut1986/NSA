#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "datepicker.h"
#include "resources.h"
#include "../core/core.h"

// The top screen's preview ticks with the clock until a wheel is turned (see timeTouched).
// Two pages on the touch screen, switched with the tabs at the top (or A / B): a month's calendar (the D-Pad moves the
// day, a week up or down; L / R or the arrows change the month; a tap picks a day), and wheels for the hours, minutes
// and seconds, plus AM / PM on a 12-hour clock (a finger drags a wheel, a tap above or below its middle moves it one;
// Left / Right pick a wheel, Up / Down turn it). The top screen shows the choice.

#define TAB_Y 30.0f
#define TAB_H 22.0f
#define HEAD_Y 56.0f
#define HEAD_H 20.0f
#define WEEK_Y 79.0f
#define GRID_Y 92.0f
#define CELL_H 20.0f
#define GRID_X 16.0f
#define WHEEL_Y 58.0f
#define WHEEL_H 120.0f
#define WHEEL_PITCH 24.0f
#define OK_Y 186.0f
#define OK_H 26.0f
#define OK_W 120.0f

typedef struct {
    int year;
    int month;    // 1-12.
    int day;
    int hour;     // 0-23.
    int minute;
    int second;

    int page;     // 0 the date, 1 the time.
    int wheel;    // The wheel with the focus.
    float offset[4];
    bool dragging;
    bool moved;
    int dragWheel;
    float lastY;
    float startY;
    bool okPressed;
    // Until a wheel is turned the time follows the console's clock: picking only the day (or taking a while) does not
    // leave the clock behind.
    bool timeTouched;
    widget_repeat repeat;

    char title[64];
    void* data;
    void (*done)(void* data, const struct tm* chosen);
} picker_data;

// ---- dates ----

static bool picker_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int picker_days_in(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && picker_leap(year) ? 29 : days[month - 1];
}

// 0 = Sunday.
static int picker_weekday(int year, int month, int day) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if(month < 3) {
        year--;
    }

    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

// The console's clock goes from 2000 to 2099.
static void picker_clamp(picker_data* p) {
    if(p->year < 2000) {
        p->year = 2000;
        p->month = 1;
        p->day = 1;
    } else if(p->year > 2099) {
        p->year = 2099;
        p->month = 12;
        p->day = 31;
    }

    int days = picker_days_in(p->year, p->month);
    if(p->day > days) {
        p->day = days;
    }
}

static void picker_add_months(picker_data* p, int months) {
    int index = p->year * 12 + (p->month - 1) + months;
    p->year = index / 12;
    p->month = index % 12 + 1;
    picker_clamp(p);
}

static void picker_add_days(picker_data* p, int days) {
    p->day += days;
    while(p->day < 1) {
        if(p->year == 2000 && p->month == 1) {
            p->day = 1;
            break;
        }

        picker_add_months(p, -1);
        p->day += picker_days_in(p->year, p->month);
    }

    while(p->day > picker_days_in(p->year, p->month)) {
        if(p->year == 2099 && p->month == 12) {
            p->day = 31;
            break;
        }

        p->day -= picker_days_in(p->year, p->month);
        p->month++;
        if(p->month > 12) {
            p->month = 1;
            p->year++;
        }
    }
}

static const char* picker_month_name(int month) {
    switch(month) {
        case 1: return _("January");
        case 2: return _("February");
        case 3: return _("March");
        case 4: return _("April");
        case 5: return _("May");
        case 6: return _("June");
        case 7: return _("July");
        case 8: return _("August");
        case 9: return _("September");
        case 10: return _("October");
        case 11: return _("November");
        default: return _("December");
    }
}

static const char* picker_weekday_name(int weekday) {
    switch(weekday) {
        case 0: return _("Su");
        case 1: return _("Mo");
        case 2: return _("Tu");
        case 3: return _("We");
        case 4: return _("Th");
        case 5: return _("Fr");
        default: return _("Sa");
    }
}

// Weeks start on Monday where the day goes first, on Sunday where the month does.
static int picker_first_weekday() {
    return datefmt_month_first() ? 0 : 1;
}

static float picker_cell_w() {
    return (BOTTOM_SCREEN_WIDTH - 2 * GRID_X) / 7;
}

// The cell of a day of the month shown (row 0-5, column 0-6).
static void picker_cell_of(picker_data* p, int day, int* row, int* column) {
    int lead = (picker_weekday(p->year, p->month, 1) - picker_first_weekday() + 7) % 7;
    int index = lead + day - 1;
    *row = index / 7;
    *column = index % 7;
}

// ---- wheels ----

static int picker_wheels() {
    return datefmt_clock24() ? 3 : 4;
}

static int picker_wheel_range(int wheel) {
    switch(wheel) {
        case 0: return datefmt_clock24() ? 24 : 12;
        case 3: return 2;
        default: return 60;
    }
}

static int picker_wheel_get(picker_data* p, int wheel) {
    switch(wheel) {
        case 0: return datefmt_clock24() ? p->hour : p->hour % 12;
        case 1: return p->minute;
        case 2: return p->second;
        default: return p->hour / 12;
    }
}

static void picker_wheel_set(picker_data* p, int wheel, int value) {
    p->timeTouched = true;
    int range = picker_wheel_range(wheel);
    value = ((value % range) + range) % range;
    switch(wheel) {
        case 0: p->hour = datefmt_clock24() ? value : value + (p->hour >= 12 ? 12 : 0); break;
        case 1: p->minute = value; break;
        case 2: p->second = value; break;
        default: p->hour = p->hour % 12 + value * 12; break;
    }
}

static void picker_wheel_text(int wheel, int value, char* out, size_t size) {
    switch(wheel) {
        case 0:
            if(datefmt_clock24()) {
                snprintf(out, size, "%02d", value);
            } else {
                snprintf(out, size, "%d", value == 0 ? 12 : value);
            }

            break;
        case 3: snprintf(out, size, "%s", value == 0 ? "AM" : "PM"); break;
        default: snprintf(out, size, "%02d", value); break;
    }
}

static void picker_wheel_rect(int wheel, float* x, float* w) {
    int count = picker_wheels();
    float width = count == 3 ? 70 : 60;
    float gap = count == 3 ? 10 : 8;
    float total = count * width + (count - 1) * gap;
    *x = (BOTTOM_SCREEN_WIDTH - total) / 2 + wheel * (width + gap);
    *w = width;
}

static const char* picker_wheel_label(int wheel) {
    switch(wheel) {
        case 0: return _("Hours");
        case 1: return _("Minutes");
        case 2: return _("Seconds");
        default: return "AM/PM";
    }
}

// ---- drawing ----

static void picker_time_text(picker_data* p, char* out, size_t size) {
    if(datefmt_clock24()) {
        snprintf(out, size, "%02d:%02d:%02d", p->hour, p->minute, p->second);
    } else {
        int hour = p->hour % 12;
        snprintf(out, size, "%d:%02d:%02d %s", hour == 0 ? 12 : hour, p->minute, p->second, p->hour < 12 ? "AM" : "PM");
    }
}

static void picker_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    picker_data* p = (picker_data*) data;
    const theme_current* t = theme();
    float x = x1 + 10;
    float w = x2 - x1 - 20;

    widget_card(x, y1 + 10, w, 92);
    screen_fill_round_rect(x + 8, y1 + 18, 36, 36, t->def->radius, t->accentSoft);
    screen_draw_icon(p->page == 0 ? ICON_CALENDAR : ICON_CLOCK, x + 10, y1 + 20, 32, t->accent);
    widget_text_fit(p->title, x + 54, y1 + 18, WIDGET_SMALL_SCALE, t->palette->textMuted, w - 64);

    char time[24];
    picker_time_text(p, time, sizeof(time));
    float big = WIDGET_TITLE_SCALE * 1.6f;
    screen_draw_text(time, x + 54, y1 + 32, big, p->page == 1 ? t->accent : t->palette->text);

    struct tm tm = {0};
    tm.tm_year = p->year - 1900;
    tm.tm_mon = p->month - 1;
    tm.tm_mday = p->day;
    char date[24];
    datefmt_date(date, sizeof(date), &tm);
    char line[96];
    snprintf(line, sizeof(line), "%s, %s", picker_weekday_name(picker_weekday(p->year, p->month, p->day)), date);
    widget_text_fit(line, x + 54, y1 + 38 + widget_text_height(big), WIDGET_TEXT_SCALE, p->page == 0 ? t->accent : t->palette->text, w - 64);

    const char* hint = p->page == 0 ? _("Pick the day, then A for the time. L / R change the month.")
                                    : _("Drag a wheel or turn it with Up / Down; Left / Right pick another. A saves.");
    screen_draw_text_wrap(hint, x + 2, y1 + 112, WIDGET_SMALL_SCALE + 0.04f, t->palette->textMuted, x + w - 2);
}

static void picker_draw_tabs(picker_data* p) {
    const theme_current* t = theme();
    float half = (BOTTOM_SCREEN_WIDTH - 2 * GRID_X) / 2;
    const char* labels[2] = {_("Date"), _("Time")};
    u32 icons[2] = {ICON_CALENDAR, ICON_CLOCK};
    screen_fill_round_rect(GRID_X, TAB_Y, 2 * half, TAB_H, t->def->radiusSmall, t->palette->surface);
    for(int i = 0; i < 2; i++) {
        float x = GRID_X + i * half;
        bool on = p->page == i;
        if(on) {
            screen_fill_round_rect(x + 2, TAB_Y + 2, half - 4, TAB_H - 4, t->def->radiusSmall, t->accent);
        }

        u32 ink = on ? t->onAccent : t->palette->textMuted;
        float labelW = widget_text_width(labels[i], WIDGET_TEXT_SCALE);
        float startX = x + (half - labelW - 18) / 2;
        screen_draw_icon(icons[i], startX, TAB_Y + (TAB_H - 14) / 2, 14, ink);
        screen_draw_text(labels[i], startX + 18, TAB_Y + (TAB_H - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink);
    }
}

static void picker_draw_calendar(picker_data* p) {
    const theme_current* t = theme();
    char head[48];
    snprintf(head, sizeof(head), "%s %d", picker_month_name(p->month), p->year);
    widget_text_centered(head, 0, BOTTOM_SCREEN_WIDTH, HEAD_Y + (HEAD_H - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, t->palette->text);
    screen_draw_icon(ICON_CHEVRON_LEFT, GRID_X + 4, HEAD_Y + 2, 16, t->accent);
    screen_draw_icon(ICON_CHEVRON_RIGHT, BOTTOM_SCREEN_WIDTH - GRID_X - 20, HEAD_Y + 2, 16, t->accent);

    float cw = picker_cell_w();
    for(int c = 0; c < 7; c++) {
        int weekday = (picker_first_weekday() + c) % 7;
        widget_text_centered(picker_weekday_name(weekday), GRID_X + c * cw, GRID_X + (c + 1) * cw, WEEK_Y, WIDGET_SMALL_SCALE,
                             weekday == 0 ? t->palette->danger : t->palette->textMuted);
    }

    int days = picker_days_in(p->year, p->month);
    for(int day = 1; day <= days; day++) {
        int row = 0;
        int column = 0;
        picker_cell_of(p, day, &row, &column);
        float x = GRID_X + column * cw;
        float y = GRID_Y + row * CELL_H;
        bool on = day == p->day;
        if(on) {
            screen_fill_round_rect(x + 2, y + 1, cw - 4, CELL_H - 2, t->def->radiusSmall, t->accent);
        }

        char text[4];
        snprintf(text, sizeof(text), "%d", day);
        int weekday = (picker_first_weekday() + column) % 7;
        u32 ink = on ? t->onAccent : weekday == 0 ? t->palette->danger : t->palette->text;
        widget_text_centered(text, x, x + cw, y + (CELL_H - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink);
    }
}

static void picker_draw_wheels(picker_data* p) {
    const theme_current* t = theme();
    float center = WHEEL_Y + WHEEL_H / 2;
    for(int wheel = 0; wheel < picker_wheels(); wheel++) {
        float x = 0;
        float w = 0;
        picker_wheel_rect(wheel, &x, &w);
        bool focused = wheel == p->wheel;
        widget_text_centered(picker_wheel_label(wheel), x, x + w, WHEEL_Y - 1, WIDGET_SMALL_SCALE, focused ? t->accent : t->palette->textMuted);
        float boxY = WHEEL_Y + 12;
        float boxH = WHEEL_H - 12;
        screen_fill_round_rect(x, boxY, w, boxH, t->def->radiusSmall, t->palette->surface);
        if(focused) {
            screen_fill_round_rect(x - 1, boxY - 1, w + 2, 1, 0, t->accent);
        }

        float bandY = center + 6 - WHEEL_PITCH / 2;
        screen_fill_round_rect(x + 3, bandY, w - 6, WHEEL_PITCH, t->def->radiusSmall, focused ? t->accent : t->accentSoft);

        screen_set_clip(x, boxY, w, boxH);
        int value = picker_wheel_get(p, wheel);
        int range = picker_wheel_range(wheel);
        for(int k = -3; k <= 3; k++) {
            float y = center + 6 + k * WHEEL_PITCH + p->offset[wheel];
            float distance = fabsf(y - (center + 6)) / WHEEL_PITCH;
            if(distance > 2.6f) {
                continue;
            }

            int shown = ((value + k) % range + range) % range;
            char text[8];
            picker_wheel_text(wheel, shown, text, sizeof(text));
            float scale = distance < 0.5f ? WIDGET_TITLE_SCALE * 1.15f : WIDGET_TEXT_SCALE;
            u32 ink = distance < 0.5f ? (focused ? t->onAccent : t->palette->text) : THEME_ALPHA(t->palette->textMuted, (u32) (0xFF * (1.0f - distance / 3.0f)));
            widget_text_centered(text, x, x + w, y - widget_text_height(scale) / 2, scale, ink);
        }

        screen_clear_clip();
    }

    float okX = (BOTTOM_SCREEN_WIDTH - OK_W) / 2;
    widget_button(okX, OK_Y, OK_W, OK_H, ICON_CHECK, _("Save"), p->okPressed);
}

static void picker_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    picker_data* p = (picker_data*) data;
    picker_draw_tabs(p);
    if(p->page == 0) {
        picker_draw_calendar(p);
    } else {
        picker_draw_wheels(p);
    }
}

// ---- input ----

static void picker_finish(ui_view* view, picker_data* p, bool chosen) {
    struct tm tm = {0};
    tm.tm_year = p->year - 1900;
    tm.tm_mon = p->month - 1;
    tm.tm_mday = p->day;
    tm.tm_hour = p->hour;
    tm.tm_min = p->minute;
    tm.tm_sec = p->second;
    tm.tm_wday = picker_weekday(p->year, p->month, p->day);
    void* data = p->data;
    void (*done)(void* data, const struct tm* chosen) = p->done;

    ui_pop();
    ui_destroy(view);
    free(p);

    if(chosen && done != NULL) {
        done(data, &tm);
    }
}

static void picker_page(picker_data* p, int page) {
    if(p->page != page) {
        p->page = page;
        sound_play(SOUND_TICK);
    }
}

static void picker_update_calendar(picker_data* p, const touchPosition* pos, u32 down) {
    bool fresh = false;
    if(widget_repeat_step(&p->repeat, KEY_LEFT, &fresh)) picker_add_days(p, -1);
    if(widget_repeat_step(&p->repeat, KEY_RIGHT, &fresh)) picker_add_days(p, 1);
    if(widget_repeat_step(&p->repeat, KEY_UP, &fresh)) picker_add_days(p, -7);
    if(widget_repeat_step(&p->repeat, KEY_DOWN, &fresh)) picker_add_days(p, 7);
    if(down & KEY_L) picker_add_months(p, -1);
    if(down & KEY_R) picker_add_months(p, 1);

    if(down & KEY_TOUCH) {
        if(pos->py >= HEAD_Y && pos->py < HEAD_Y + HEAD_H) {
            if(pos->px < GRID_X + 40) {
                picker_add_months(p, -1);
                sound_play(SOUND_TICK);
            } else if(pos->px > BOTTOM_SCREEN_WIDTH - GRID_X - 40) {
                picker_add_months(p, 1);
                sound_play(SOUND_TICK);
            }
        } else if(pos->py >= GRID_Y && pos->py < GRID_Y + 6 * CELL_H && pos->px >= GRID_X && pos->px < BOTTOM_SCREEN_WIDTH - GRID_X) {
            int row = (int) ((pos->py - GRID_Y) / CELL_H);
            int column = (int) ((pos->px - GRID_X) / picker_cell_w());
            int lead = (picker_weekday(p->year, p->month, 1) - picker_first_weekday() + 7) % 7;
            int day = row * 7 + column - lead + 1;
            if(day >= 1 && day <= picker_days_in(p->year, p->month)) {
                p->day = day;
                sound_play(SOUND_TICK);
            }
        }
    }
}

static int picker_wheel_at(float px, float py) {
    if(py < WHEEL_Y || py >= WHEEL_Y + WHEEL_H) {
        return -1;
    }

    for(int wheel = 0; wheel < picker_wheels(); wheel++) {
        float x = 0;
        float w = 0;
        picker_wheel_rect(wheel, &x, &w);
        if(px >= x - 4 && px < x + w + 4) {
            return wheel;
        }
    }

    return -1;
}

static bool picker_update_wheels(ui_view* view, picker_data* p, const touchPosition* pos, u32 down, u32 held) {
    bool fresh = false;
    if(widget_repeat_step(&p->repeat, KEY_LEFT, &fresh)) p->wheel = (p->wheel + picker_wheels() - 1) % picker_wheels();
    if(widget_repeat_step(&p->repeat, KEY_RIGHT, &fresh)) p->wheel = (p->wheel + 1) % picker_wheels();
    if(widget_repeat_step(&p->repeat, KEY_UP, &fresh)) picker_wheel_set(p, p->wheel, picker_wheel_get(p, p->wheel) - 1);
    if(widget_repeat_step(&p->repeat, KEY_DOWN, &fresh)) picker_wheel_set(p, p->wheel, picker_wheel_get(p, p->wheel) + 1);

    float okX = (BOTTOM_SCREEN_WIDTH - OK_W) / 2;
    if(down & KEY_TOUCH) {
        if(widget_touched(pos, okX, OK_Y, OK_W, OK_H)) {
            p->okPressed = true;
        } else {
            int wheel = picker_wheel_at(pos->px, pos->py);
            if(wheel >= 0) {
                p->dragging = true;
                p->moved = false;
                p->dragWheel = wheel;
                p->wheel = wheel;
                p->lastY = pos->py;
                p->startY = pos->py;
            }
        }
    } else if(held & KEY_TOUCH) {
        if(p->okPressed && !widget_touched(pos, okX, OK_Y, OK_W, OK_H)) {
            p->okPressed = false;
        }

        if(p->dragging) {
            int wheel = p->dragWheel;
            p->offset[wheel] += pos->py - p->lastY;
            p->lastY = pos->py;
            if(fabsf(pos->py - p->startY) > 4) {
                p->moved = true;
            }

            // The finger pulls the numbers: down shows the ones before.
            while(p->offset[wheel] > WHEEL_PITCH / 2) {
                p->offset[wheel] -= WHEEL_PITCH;
                picker_wheel_set(p, wheel, picker_wheel_get(p, wheel) - 1);
                sound_play(SOUND_TICK);
            }

            while(p->offset[wheel] < -WHEEL_PITCH / 2) {
                p->offset[wheel] += WHEEL_PITCH;
                picker_wheel_set(p, wheel, picker_wheel_get(p, wheel) + 1);
                sound_play(SOUND_TICK);
            }
        }
    } else if(hidKeysUp() & KEY_TOUCH) {
        if(p->okPressed) {
            p->okPressed = false;
            picker_finish(view, p, true);
            return false;
        }

        if(p->dragging && !p->moved) {
            // A tap above or below the middle turns it one.
            float middle = WHEEL_Y + WHEEL_H / 2 + 6;
            if(p->lastY < middle - WHEEL_PITCH / 2) {
                picker_wheel_set(p, p->dragWheel, picker_wheel_get(p, p->dragWheel) - 1);
                sound_play(SOUND_TICK);
            } else if(p->lastY > middle + WHEEL_PITCH / 2) {
                picker_wheel_set(p, p->dragWheel, picker_wheel_get(p, p->dragWheel) + 1);
                sound_play(SOUND_TICK);
            }
        }

        p->dragging = false;
    }

    // Settles on the number in the middle.
    for(int wheel = 0; wheel < 4; wheel++) {
        if(!(p->dragging && wheel == p->dragWheel) && p->offset[wheel] != 0) {
            p->offset[wheel] *= 0.6f;
            if(fabsf(p->offset[wheel]) < 0.5f) {
                p->offset[wheel] = 0;
            }

            ui_wake();
        }
    }

    return true;
}

static void picker_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    picker_data* p = (picker_data*) data;
    u32 down = hidKeysDown();
    u32 held = hidKeysHeld();

    if(!p->timeTouched) {
        time_t now = time(NULL);
        struct tm* tm = gmtime(&now);
        if(tm != NULL) {
            p->hour = tm->tm_hour;
            p->minute = tm->tm_min;
            p->second = tm->tm_sec;
        }
    }
    view->info = p->page == 0 ? _(" time     month    cancel") : _(" save    date");

    if(down & KEY_B) {
        if(p->page == 1) {
            picker_page(p, 0);
        } else {
            picker_finish(view, p, false);
        }

        return;
    }

    if(down & KEY_START) {
        picker_finish(view, p, true);
        return;
    }

    if(down & KEY_A) {
        if(p->page == 0) {
            picker_page(p, 1);
        } else {
            picker_finish(view, p, true);
        }

        return;
    }

    touchPosition pos;
    hidTouchRead(&pos);
    if((down & KEY_TOUCH) && pos.py >= TAB_Y && pos.py < TAB_Y + TAB_H) {
        picker_page(p, pos.px < BOTTOM_SCREEN_WIDTH / 2 ? 0 : 1);
        return;
    }

    if(p->page == 0) {
        picker_update_calendar(p, &pos, down);
    } else {
        picker_update_wheels(view, p, &pos, down, held);
    }
}

void datepicker_display(const char* title, const struct tm* start, void* data, void (*done)(void* data, const struct tm* chosen)) {
    picker_data* p = (picker_data*) calloc(1, sizeof(picker_data));
    if(p == NULL) {
        error_display(NULL, NULL, "Failed to allocate date and time data.");
        return;
    }

    p->year = start->tm_year + 1900;
    p->month = start->tm_mon + 1;
    p->day = start->tm_mday;
    p->hour = start->tm_hour;
    p->minute = start->tm_min;
    p->second = start->tm_sec;
    picker_clamp(p);
    string_copy(p->title, title, sizeof(p->title));
    p->data = data;
    p->done = done;

    ui_view* view = ui_create();
    view->name = p->title;
    view->info = "";
    view->data = p;
    view->update = picker_update;
    view->drawTop = picker_draw_top;
    view->drawBottom = picker_draw_bottom;
    view->backArrow = true;
    ui_push(view);
}
