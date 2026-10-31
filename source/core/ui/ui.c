#include <stdio.h>
#include <string.h>
#include <time.h>

#include <3ds.h>
#include <malloc.h>

#include "ui.h"
#include "../error.h"
#include "../screen.h"
#include "../sound.h"
#include "../appinfo.h"
#include "../i18n.h"
#include "../data/smdh.h"
#include "../icons.h"
#include "../theme.h"
#include "widgets.h"

#define MAX_UI_VIEWS 16

static ui_view* ui_stack[MAX_UI_VIEWS];
static int ui_stack_top = -1;

static Handle ui_stack_mutex = 0;


static void (*ui_status_corner)(char* out, size_t size) = NULL;
static bool (*ui_status_notice)(char* out, size_t size, float* progress) = NULL;

static u64 ui_fade_begin_time = 0;
static u8 ui_fade_alpha = 0;

void ui_init() {
    if(ui_stack_mutex == 0) {
        svcCreateMutex(&ui_stack_mutex, false);
    }

    ui_fade_begin_time = osGetTime();
}

void ui_exit() {
    if(ui_stack_mutex != 0) {
        svcCloseHandle(ui_stack_mutex);
        ui_stack_mutex = 0;
    }
}

ui_view* ui_create() {
    ui_view* view = (ui_view*) calloc(1, sizeof(ui_view));
    if(view == NULL) {
        error_panic("Failed to allocate UI view.");
        return NULL;
    }

    Result res = 0;
    if(R_FAILED(res = svcCreateEvent(&view->active, RESET_STICKY))) {
        error_panic("Failed to create view active event: 0x%08lX", res);

        free(view);
        return NULL;
    }

    return view;
}

void ui_destroy(ui_view* view) {
    if(view != NULL) {
        svcCloseHandle(view->active);
        free(view);
    }
}

u32 ui_depth() {
    svcWaitSynchronization(ui_stack_mutex, U64_MAX);
    u32 depth = (u32) (ui_stack_top + 1);
    svcReleaseMutex(ui_stack_mutex);

    return depth;
}

ui_view* ui_top() {
    svcWaitSynchronization(ui_stack_mutex, U64_MAX);

    ui_view* ui = NULL;
    if(ui_stack_top >= 0) {
        ui = ui_stack[ui_stack_top];
    }

    svcReleaseMutex(ui_stack_mutex);

    return ui;
}

bool ui_push(ui_view* view) {
    if(view == NULL) {
        return false;
    }

    svcWaitSynchronization(ui_stack_mutex, U64_MAX);

    bool space = ui_stack_top < MAX_UI_VIEWS - 1;
    if(space) {
        ui_stack[++ui_stack_top] = view;

        svcClearEvent(view->active);
    }

    svcReleaseMutex(ui_stack_mutex);

    return space;
}

void ui_pop() {
    svcWaitSynchronization(ui_stack_mutex, U64_MAX);

    if(ui_stack_top >= 0) {
        svcSignalEvent(ui_stack[ui_stack_top]->active);

        ui_stack[ui_stack_top--] = NULL;
    }

    svcReleaseMutex(ui_stack_mutex);
}

static void ui_draw_top_bar() {
    const theme_current* t = theme();
    float height = WIDGET_TOP_BAR_HEIGHT;
    float textY = (height - widget_text_height(WIDGET_SMALL_SCALE)) / 2;

    screen_fill_rect(0, 0, TOP_SCREEN_WIDTH, height, t->palette->surface);
    screen_fill_rect(0, height - 1, TOP_SCREEN_WIDTH, 1, t->palette->border);

    // Left: the app and its version.
    char version[64];
    snprintf(version, sizeof(version), APP_NAME " %s%s", VERSION_STRING, NSA_DEBUG ? " DEBUG" : "");
    float left = 8 + widget_text_width(version, WIDGET_SMALL_SCALE);
    screen_draw_text(version, 8, textY, WIDGET_SMALL_SCALE, t->palette->text);

    // Right, from the edge: battery, Wi-Fi and, when it runs, the FTP server
    // with the end of the console's address (the rest is the network's).
    float x = TOP_SCREEN_WIDTH - 8;
    x -= widget_battery(x, 5, height - 10);

    x -= 8 + 13;
    widget_wifi(x, 6, height - 12);

    if(ui_status_corner != NULL) {
        char corner[64] = "";
        ui_status_corner(corner, sizeof(corner));
        if(corner[0] != '\0') {
            // "FTP 192.168.1.133:5021" -> ".133"
            char tail[16] = "";
            char* dot = strrchr(corner, '.');
            if(dot != NULL) {
                snprintf(tail, sizeof(tail), "%s", dot);
                char* colon = strchr(tail, ':');
                if(colon != NULL) {
                    *colon = '\0';
                }
            }

            float tailW = widget_text_width(tail, WIDGET_SMALL_SCALE);
            x -= 8 + tailW;
            screen_draw_text(tail, x, textY, WIDGET_SMALL_SCALE, t->accent);
            x -= 15;
            screen_draw_icon(ICON_SERVER, x, (height - 14) / 2, 14, t->accent);
        }
    }

    // Centre: date and time, or only the time when the date does not fit.
    time_t now = time(NULL);
    struct tm* local = gmtime(&now);
    char clock[32];
    strftime(clock, sizeof(clock), "%d/%m  %H:%M", local);
    float clockWidth = widget_text_width(clock, WIDGET_SMALL_SCALE);
    float clockX = (TOP_SCREEN_WIDTH - clockWidth) / 2;
    if(clockX < left + 8 || clockX + clockWidth > x - 8) {
        strftime(clock, sizeof(clock), "%H:%M", local);
        clockWidth = widget_text_width(clock, WIDGET_SMALL_SCALE);
        clockX = left + (x - left - clockWidth) / 2;
    }

    if(clockX >= left + 4 && clockX + clockWidth <= x - 4) {
        screen_draw_text(clock, clockX, textY, WIDGET_SMALL_SCALE, t->palette->text);
    }
}

// Pixels each eye moves per unit of depth with the 3D slider all the way up.
#define UI_STEREO_SHIFT 1.6f

// The top screen's layers in 3D: the background on the screen's plane, the
// bar a little in front and the view's content (cards) in front of it.
#define UI_DEPTH_BAR 1.0f
#define UI_DEPTH_CONTENT 2.0f

static void ui_draw_top_eye(ui_view* ui, gfx3dSide_t side, float shift) {
    screen_select_eye(GFX_TOP, side, shift);
    widget_background(TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT);

    screen_set_base_alpha(ui_fade_alpha);

    if(ui->drawTop != NULL) {
        screen_set_depth(UI_DEPTH_CONTENT);
        ui->drawTop(ui, ui->data, 0, WIDGET_TOP_BAR_HEIGHT, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT);
    }

    screen_set_offset(0, 0);
    screen_set_depth(UI_DEPTH_BAR);
    ui_draw_top_bar();
    screen_set_depth(0);

    screen_set_base_alpha(0xFF);
}

static void ui_draw_top(ui_view* ui) {
    // In front of the screen: the left eye sees things moved right.
    float slider = osGet3DSliderState();
    bool stereo = slider > 0.01f;
    if(stereo != gfxIs3D()) {
        gfxSet3D(stereo);
    }

    ui_draw_top_eye(ui, GFX_LEFT, stereo ? slider * UI_STEREO_SHIFT : 0);
    if(stereo) {
        ui_draw_top_eye(ui, GFX_RIGHT, -slider * UI_STEREO_SHIFT);
    }
}

void ui_set_status(void (*corner)(char* out, size_t size), bool (*notice)(char* out, size_t size, float* progress)) {
    ui_status_corner = corner;
    ui_status_notice = notice;
}

static void ui_draw_bottom(ui_view* ui) {
    const theme_current* t = theme();

    screen_select(GFX_BOTTOM);
    widget_background(BOTTOM_SCREEN_WIDTH, BOTTOM_SCREEN_HEIGHT);

    screen_set_base_alpha(ui_fade_alpha);

    float contentTop = WIDGET_TITLE_BAR_HEIGHT;
    float contentBottom = BOTTOM_SCREEN_HEIGHT - WIDGET_HINT_BAR_HEIGHT;

    if(ui->drawBottom != NULL) {
        ui->drawBottom(ui, ui->data, 0, contentTop, BOTTOM_SCREEN_WIDTH, contentBottom);
    }

    // Title bar.
    screen_fill_rect(0, 0, BOTTOM_SCREEN_WIDTH, contentTop, t->palette->surface);
    screen_fill_rect(0, contentTop - 1, BOTTOM_SCREEN_WIDTH, 1, t->palette->border);
    if(ui->drawTitleBar != NULL) {
        ui->drawTitleBar(ui, ui->data, 0, 0, BOTTOM_SCREEN_WIDTH, contentTop);
    } else if(ui->name != NULL) {
        widget_text_centered(ui->name, 0, BOTTOM_SCREEN_WIDTH, (contentTop - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, t->palette->text);
    }

    // Hint bar: the view's buttons.
    screen_fill_rect(0, contentBottom, BOTTOM_SCREEN_WIDTH, WIDGET_HINT_BAR_HEIGHT, t->palette->surface);
    screen_fill_rect(0, contentBottom, BOTTOM_SCREEN_WIDTH, 1, t->palette->border);
    if(ui->info != NULL) {
        const char* hint = i18n_translate_text(ui->info);
        float scale = WIDGET_SMALL_SCALE;
        float width = widget_text_width(hint, scale);
        if(width > BOTTOM_SCREEN_WIDTH - 12) {
            scale *= (BOTTOM_SCREEN_WIDTH - 12) / width;
        }

        widget_text_centered(hint, 0, BOTTOM_SCREEN_WIDTH, contentBottom + (WIDGET_HINT_BAR_HEIGHT - widget_text_height(scale)) / 2, scale, t->palette->textMuted);
    }

    char notice[128];
    float progress = -1;
    if(ui_status_notice != NULL && ui_status_notice(notice, sizeof(notice), &progress)) {
        widget_toast(notice, progress, contentBottom);
    }

    screen_set_base_alpha(0xFF);
}

// Drawing every frame costs the app core much of its time (on an Old 3DS over
// half, with nothing moving), time a download or an install could use. So a
// frame is drawn while something happens (input, a view opening or closing,
// the 3D slider) and for a while after, for the animations to finish; past
// that, a few times a second, enough for progress and clocks.
#define UI_ACTIVE_TIME 1500
#define UI_IDLE_FRAME 100

static u64 ui_last_active = 0;
static u64 ui_last_draw = 0;
static ui_view* ui_last_view = NULL;
static u32 ui_last_depth = 0;
static float ui_last_slider = 0;

void ui_wake() {
    ui_last_active = osGetTime();
}

static bool ui_should_draw(ui_view* ui) {
    u64 now = osGetTime();
    float slider = osGet3DSliderState();
    if((hidKeysHeld() | hidKeysDown() | hidKeysUp()) != 0 || ui != ui_last_view || ui_depth() != ui_last_depth
       || slider != ui_last_slider || ui_last_active == 0) {
        ui_last_active = now;
        ui_last_view = ui;
        ui_last_depth = ui_depth();
        ui_last_slider = slider;
    }

    if(now - ui_last_active < UI_ACTIVE_TIME || now - ui_last_draw >= UI_IDLE_FRAME) {
        ui_last_draw = now;
        return true;
    }

    return false;
}

bool ui_update() {
    ui_view* ui = NULL;

    input_scan();

    // Feedback for the buttons, wherever they are pressed.
    u32 down = hidKeysDown();
    if((down & KEY_B) && ui_depth() > 1) {
        sound_play(SOUND_BACK);
    } else if(down & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_TOUCH)) {
        sound_play(SOUND_TICK);
    } else if(down & KEY_A) {
        sound_play(SOUND_CLICK);
    }

    sound_update();

    ui = ui_top();
    if(ui != NULL && ui->update != NULL) {
        ui->update(ui, ui->data, 0, WIDGET_TITLE_BAR_HEIGHT, BOTTOM_SCREEN_WIDTH, BOTTOM_SCREEN_HEIGHT - WIDGET_HINT_BAR_HEIGHT);
    }

    u64 time = osGetTime();
    if(!envIsHomebrew() && time - ui_fade_begin_time < 500) {
        ui_fade_alpha = (u8) (((time - ui_fade_begin_time) / 500.0f) * 0xFF);
    } else {
        ui_fade_alpha = 0xFF;
    }

    theme_update();

    ui = ui_top();
    if(ui != NULL) {
        if(ui_should_draw(ui)) {
            screen_begin_frame();
            ui_draw_top(ui);
            ui_draw_bottom(ui);
            screen_end_frame();
        } else {
            // The screens keep the last frame; wait for the next one as drawing would.
            gspWaitForVBlank();
        }
    }

    return ui != NULL;
}

const char* ui_get_display_eta(u32 seconds) {
    static char disp[12];

    u8 hours     = seconds / 3600;
    seconds     -= hours * 3600;
    u8 minutes   = seconds / 60;
    seconds     -= minutes* 60;

    snprintf(disp, 12, "%02u:%02u:%02u", hours, minutes, (u8) seconds);
    return disp;
}

double ui_get_display_size(u64 size) {
    double s = size;
    if(s >= 1024) {
        s /= 1024;
    }

    if(s >= 1024) {
        s /= 1024;
    }

    if(s >= 1024) {
        s /= 1024;
    }

    return s;
}

const char* ui_get_display_size_units(u64 size) {
    if(size >= 1024 * 1024 * 1024) {
        return "GiB";
    }

    if(size >= 1024 * 1024) {
        return "MiB";
    }

    if(size >= 1024) {
        return "KiB";
    }

    return "B";
}