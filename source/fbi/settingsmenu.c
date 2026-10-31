#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include <3ds.h>

#include "ftpserver.h"
#include "section.h"
#include "settings.h"
#include "../core/bgmusic.h"
#include "update.h"
#include "../core/core.h"

// Settings: one list. Options change in place with Left/Right (or A, or a
// tap on a selected row); the top screen explains and previews the selection.

typedef enum {
    OPTION_THEME,
    OPTION_ACCENT,
    OPTION_NIGHT,
    OPTION_LANGUAGE,
    OPTION_SOUNDS,
    OPTION_MUSIC,
    OPTION_CHANNEL,
    OPTION_AUTO_UPDATE,
    OPTION_FTP,
    OPTION_CHECK_UPDATES,
    OPTION_CHANGELOG,
    OPTION_COUNT
} settings_option;

#define ROW_PITCH (WIDGET_ROW_HEIGHT + WIDGET_ROW_GAP)
#define PAD 8
#define VOLUME_STEP 5

// Sounds and Music carry a volume slider under their selector, in the same box.
static u32* settingsmenu_slider_value(settings_option option) {
    settings_data* settings = settings_get();
    return option == OPTION_SOUNDS ? &settings->soundVolume : option == OPTION_MUSIC ? &settings->musicVolume : NULL;
}

static float settingsmenu_row_height(u32 index) {
    return settingsmenu_slider_value((settings_option) index) != NULL ? widget_row_slider_height() : WIDGET_ROW_HEIGHT;
}

// Where a row starts, from the top of the list.
static float settingsmenu_row_top(u32 index) {
    float top = 0;
    for(u32 i = 0; i < index; i++) {
        top += settingsmenu_row_height(i) + WIDGET_ROW_GAP;
    }

    return top;
}

// The volume applies as it moves; saving waits for the finger to lift.
static void settingsmenu_apply_volumes() {
    settings_data* settings = settings_get();
    sound_set((sound_mode) settings->sounds, settings->soundVolume);
    bgmusic_set((bgmusic_style) settings->music, settings->musicVolume);
}

typedef struct {
    u32 selected;
    float scroll;
    u64 nextMove;

    bool touching;
    bool dragged;
    int sliding;         // The row whose slider a finger is on, or -1.
    bool barDrag;        // A finger holds the scrollbar.
    widget_press press;
    u16 touchStartY;
    u16 lastTouchY;
} settingsmenu_data;

static icon_id settingsmenu_icon(settings_option option) {
    static const icon_id icons[OPTION_COUNT] = {
            ICON_PALETTE, ICON_COLOR_SWATCH, ICON_MOON, ICON_LANGUAGE, ICON_VOLUME, ICON_MUSIC, ICON_GIT_BRANCH,
            ICON_REFRESH, ICON_SERVER, ICON_WORLD_DOWNLOAD, ICON_LIST_DETAILS
    };

    return icons[option];
}

static const char* settingsmenu_label(settings_option option) {
    switch(option) {
        case OPTION_THEME: return _("Theme");
        case OPTION_ACCENT: return _("Accent colour");
        case OPTION_NIGHT: return _("Night mode");
        case OPTION_LANGUAGE: return _("Language");
        case OPTION_SOUNDS: return _("Sounds");
        case OPTION_MUSIC: return _("Background music");
        case OPTION_CHANNEL: return _("Update channel");
        case OPTION_AUTO_UPDATE: return _("Check at start");
        case OPTION_FTP: return _("FTP server");
        case OPTION_CHECK_UPDATES: return _("Check for updates");
        case OPTION_CHANGELOG: return _("Changelog");
        default: return "";
    }
}

static const char* settingsmenu_on_off(bool on) {
    return on ? _("On") : _("Off");
}

// The value shown between ‹ ›, or NULL for an action.
static const char* settingsmenu_value(settings_option option) {
    settings_data* settings = settings_get();
    switch(option) {
        case OPTION_THEME: return _(theme_get(settings->theme)->name);
        case OPTION_ACCENT: return _(theme_accent_get(settings->accent)->name);
        case OPTION_NIGHT:
            return settings->night == THEME_NIGHT_ON ? _("On") : settings->night == THEME_NIGHT_AUTO ? _("Automatic") : _("Off");
        case OPTION_LANGUAGE: return i18n_language_name(settings->language);
        case OPTION_SOUNDS: return settings->sounds == SOUNDS_ALL ? _("All") : settings->sounds == SOUNDS_ALERTS ? _("Alerts") : _("Off");
        case OPTION_MUSIC: return settings->music == BGMUSIC_LIFT ? _("Lift") : settings->music == BGMUSIC_CALM ? _("Calm") : settings->music == BGMUSIC_SHOP ? _("Shop") : _("Off");
        case OPTION_CHANNEL: return settings->channel == UPDATE_CHANNEL_BETA ? _("Beta") : _("Stable");
        case OPTION_AUTO_UPDATE: return settingsmenu_on_off(settings->autoUpdate);
        case OPTION_FTP: return settingsmenu_on_off(ftpserver_running());
        default: return NULL;
    }
}

static u32 settingsmenu_step(u32 value, u32 count, int direction) {
    return (value + count + (direction < 0 ? count - 1 : 1)) % count;
}

static void settingsmenu_save() {
    Result res = settings_save();
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to save settings."));
    }
}

// Changes an option by one step, or runs an action (direction 0: A or a tap).
static void settingsmenu_change(settings_option option, int direction) {
    settings_data* settings = settings_get();
    int step = direction != 0 ? direction : 1;

    switch(option) {
        case OPTION_THEME:
            settings->theme = settingsmenu_step(settings->theme, theme_count(), step);
            break;
        case OPTION_ACCENT:
            settings->accent = settingsmenu_step(settings->accent, theme_accent_count(), step);
            break;
        case OPTION_NIGHT:
            settings->night = (theme_night_mode) settingsmenu_step(settings->night, 3, step);
            break;
        case OPTION_LANGUAGE:
            settings->language = (language_id) settingsmenu_step(settings->language, LANGUAGE_COUNT, step);
            break;
        case OPTION_SOUNDS:
            settings->sounds = settingsmenu_step(settings->sounds, 3, step);
            break;
        case OPTION_MUSIC:
            settings->music = settingsmenu_step(settings->music, BGMUSIC_COUNT, step);
            bgmusic_set((bgmusic_style) settings->music, settings->musicVolume);
            break;
        case OPTION_CHANNEL:
            settings->channel = settings->channel == UPDATE_CHANNEL_BETA ? UPDATE_CHANNEL_STABLE : UPDATE_CHANNEL_BETA;
            break;
        case OPTION_AUTO_UPDATE:
            settings->autoUpdate = !settings->autoUpdate;
            break;
        case OPTION_FTP:
            if(ftpserver_running()) {
                ftpserver_stop();
                settings->ftpServer = false;
            } else {
                // Saved on even when it cannot start now (no Wi-Fi): it starts next time.
                ftpserver_start();
                settings->ftpServer = true;
            }
            break;
        case OPTION_CHECK_UPDATES:
            if(direction == 0) {
                update_check_start(false);
            }
            return;
        case OPTION_CHANGELOG:
            if(direction == 0) {
                update_show_history();
            }
            return;
        default:
            return;
    }

    settings_apply();
    settingsmenu_save();
}

static float settingsmenu_total_height() {
    return settingsmenu_row_top(OPTION_COUNT) - WIDGET_ROW_GAP;
}

static void settingsmenu_clamp_scroll(settingsmenu_data* menu, float viewHeight) {
    float max = settingsmenu_total_height() - viewHeight;
    if(menu->scroll > max) {
        menu->scroll = max;
    }

    if(menu->scroll < 0) {
        menu->scroll = 0;
    }
}

static void settingsmenu_reveal(settingsmenu_data* menu, float viewHeight) {
    float top = settingsmenu_row_top(menu->selected);
    float height = settingsmenu_row_height(menu->selected);
    if(top < menu->scroll) {
        menu->scroll = top;
    } else if(top + height > menu->scroll + viewHeight) {
        menu->scroll = top + height - viewHeight;
    }

    settingsmenu_clamp_scroll(menu, viewHeight);
}

static bool settingsmenu_repeat(settingsmenu_data* menu, u32 key) {
    if(hidKeysDown() & key) {
        menu->nextMove = osGetTime() + 400;
        return true;
    }

    if((hidKeysHeld() & key) && osGetTime() >= menu->nextMove) {
        menu->nextMove = osGetTime() + 120;
        return true;
    }

    return false;
}

static void settingsmenu_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    settingsmenu_data* menu = (settingsmenu_data*) data;
    float viewHeight = by2 - by1 - 2 * PAD;

    view->name = _("Settings");
    view->info = settingsmenu_slider_value((settings_option) menu->selected) != NULL ? _(" change     volume    back") : _(" change    back");

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        free(menu);
        ui_destroy(view);
        return;
    }

    u32 last = menu->selected;
    if(settingsmenu_repeat(menu, KEY_DOWN)) {
        menu->selected = (menu->selected + 1) % OPTION_COUNT;
    } else if(settingsmenu_repeat(menu, KEY_UP)) {
        menu->selected = (menu->selected + OPTION_COUNT - 1) % OPTION_COUNT;
    } else if(hidKeysDown() & (KEY_R | KEY_L)) {
        // Left and Right change the option, so L and R turn a page: the first row
        // that was not shown (or the one a page above) becomes the top one.
        u32 page = (u32) ((viewHeight - WIDGET_ROW_HEIGHT) / ROW_PITCH) + 1;
        u32 first = 0;
        while(first + 1 < OPTION_COUNT && settingsmenu_row_top(first) < menu->scroll) {
            first++;
        }
        if(hidKeysDown() & KEY_R) {
            menu->selected = first + page < OPTION_COUNT ? first + page : OPTION_COUNT - 1;
        } else {
            menu->selected = first > page ? first - page : 0;
        }

        menu->scroll = settingsmenu_row_top(menu->selected);
        settingsmenu_clamp_scroll(menu, viewHeight);
    } else if(settingsmenu_slider_value((settings_option) menu->selected) != NULL && (settingsmenu_repeat(menu, KEY_X) || settingsmenu_repeat(menu, KEY_Y))) {
        u32* volume = settingsmenu_slider_value((settings_option) menu->selected);
        if(hidKeysDown() & KEY_X || hidKeysHeld() & KEY_X) {
            *volume = *volume + VOLUME_STEP > 100 ? 100 : *volume + VOLUME_STEP;
        } else {
            *volume = *volume < VOLUME_STEP ? 0 : *volume - VOLUME_STEP;
        }

        settingsmenu_apply_volumes();
        settings_apply();
        settingsmenu_save();
    } else if(hidKeysDown() & KEY_RIGHT) {
        settingsmenu_change((settings_option) menu->selected, 1);
    } else if(hidKeysDown() & KEY_LEFT) {
        settingsmenu_change((settings_option) menu->selected, -1);
    } else if(hidKeysDown() & KEY_A) {
        settingsmenu_change((settings_option) menu->selected, 0);
        return;
    }

    if(menu->selected != last) {
        settingsmenu_reveal(menu, viewHeight);
    }

    // Touch: a tap selects a row, a tap on the selected row changes it; a drag scrolls.
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        menu->touching = true;
        menu->dragged = false;
        menu->touchStartY = menu->lastTouchY = pos.py;
        widget_press_end(&menu->press);
        menu->sliding = -1;
        menu->barDrag = widget_scrollbar_touched(&pos, bx2 - PAD - WIDGET_SCROLLBAR_WIDTH, by1 + PAD, viewHeight, viewHeight, settingsmenu_total_height());
        if(menu->barDrag) {
            menu->scroll = widget_scrollbar_offset(by1 + PAD, viewHeight, viewHeight, settingsmenu_total_height(), pos.py);
            settingsmenu_clamp_scroll(menu, viewHeight);
        } else if(pos.py >= by1 + PAD && pos.py < by2 - PAD) {
            float offset = pos.py - by1 - PAD + menu->scroll;
            for(u32 index = 0; index < OPTION_COUNT; index++) {
                float top = settingsmenu_row_top(index);
                if(offset < top || offset >= top + settingsmenu_row_height(index)) {
                    continue;
                }

                if(settingsmenu_slider_value((settings_option) index) != NULL && offset - top >= WIDGET_ROW_HEIGHT - 2) {
                    // A finger on a slider takes it, wherever along the bar.
                    menu->sliding = (int) index;
                    menu->selected = index;
                } else if(offset - top < WIDGET_ROW_HEIGHT) {
                    widget_press_begin(&menu->press, (int) index, true, pos.px, pos.py);
                }

                break;
            }
        }
    }

    if(menu->touching && menu->barDrag && (hidKeysHeld() & KEY_TOUCH)) {
        menu->scroll = widget_scrollbar_offset(by1 + PAD, viewHeight, viewHeight, settingsmenu_total_height(), pos.py);
        settingsmenu_clamp_scroll(menu, viewHeight);
    } else if(menu->touching && menu->sliding >= 0 && (hidKeysHeld() & KEY_TOUCH)) {
        float trackX, trackWidth, centerY;
        widget_row_slider_track(bx1 + PAD, 0, bx2 - bx1 - 2 * PAD - WIDGET_SCROLLBAR_WIDTH - 4, &trackX, &trackWidth, &centerY);
        float fraction = ((float) pos.px - trackX) / trackWidth;
        fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
        *settingsmenu_slider_value((settings_option) menu->sliding) = ((u32) (fraction * 100 / VOLUME_STEP + 0.5f)) * VOLUME_STEP;
        settingsmenu_apply_volumes();
    } else if(menu->touching && (hidKeysHeld() & KEY_TOUCH) && !(hidKeysDown() & KEY_TOUCH)) {
        if(abs((int) pos.py - (int) menu->touchStartY) > 6) {
            menu->dragged = true;
            menu->press.cancelled = true;
        }

        if(menu->dragged) {
            menu->scroll -= (int) pos.py - (int) menu->lastTouchY;
            settingsmenu_clamp_scroll(menu, viewHeight);
        }

        menu->lastTouchY = pos.py;
    } else if(menu->touching && (hidKeysUp() & KEY_TOUCH)) {
        menu->touching = false;
        if(menu->barDrag) {
            menu->barDrag = false;
            return;
        }

        if(menu->sliding >= 0) {
            menu->sliding = -1;
            settings_apply();
            settingsmenu_save();
            sound_play(SOUND_TICK);
            return;
        }

        int index = menu->press.index;
        bool released = widget_press_on(&menu->press, index);
        widget_press_end(&menu->press);
        if(released) {
            if((u32) index == menu->selected) {
                settingsmenu_change((settings_option) menu->selected, 0);
            } else {
                menu->selected = (u32) index;
            }
        }
    }
}

static const char* settingsmenu_help(settings_option option) {
    settings_data* settings = settings_get();
    switch(option) {
        case OPTION_THEME: return _("How the menus look.");
        case OPTION_ACCENT: return _("The colour of buttons, selections and bars.");
        case OPTION_NIGHT: return _("Dark colours. Automatic: from 20:00 to 8:00.");
        case OPTION_LANGUAGE: return _("System: the console's language when NSA has it, else English.");
        case OPTION_SOUNDS:
            return settings->sounds == SOUNDS_ALL ? _("All: a tick for buttons and touches, and a tune when something ends or fails.")
                   : settings->sounds == SOUNDS_ALERTS ? _("Alerts: only when something ends well or fails (a transfer, an install).")
                                                       : _("Off: no sounds. They need the DSP firmware (dspfirm.cdc).");
        case OPTION_MUSIC:
            return settings->music == BGMUSIC_LIFT ? _("Lift: soft lift music, made by the console as it plays, in a loop of half a minute.")
                   : settings->music == BGMUSIC_CALM ? _("Calm: slower and sparser, in a loop of about forty seconds.")
                   : settings->music == BGMUSIC_SHOP ? _("Shop: a soft bossa with marimba, electric piano and bass, in a loop of about forty seconds.")
                                                     : _("Off: no music. It needs the DSP firmware (dspfirm.cdc) and takes a moment to start.");
        case OPTION_CHANNEL:
            return settings->channel == UPDATE_CHANNEL_BETA
                   ? _("Beta: the newest release, beta or stable. The changelog lists every release since yours.")
                   : _("Stable: stable releases only. The changelog lists everything since yours, betas included.");
        case OPTION_AUTO_UPDATE:
            return settings->autoUpdate ? _("On: look for an update when NSA starts and ask before installing it.")
                                        : _("Off: only look for updates from this screen.");
        case OPTION_FTP: return _("Only the SD card, no password. Driving the console and SITE INSTALL need the file sdmc:/nsa/remote.flag.");
        case OPTION_CHECK_UPDATES: return _("Look for a newer release in your channel now.");
        case OPTION_CHANGELOG: return _("Every release of your channel, newest first.");
        default: return "";
    }
}

static void settingsmenu_draw_preview(settings_option option, float x, float y, float width) {
    const theme_current* t = theme();
    settings_data* settings = settings_get();

    if(option == OPTION_ACCENT) {
        u32 count = theme_accent_count();
        float step = width / count;
        float r = 12;
        for(u32 i = 0; i < count; i++) {
            const theme_accent* accent = theme_accent_get(i);
            float cx = x + step * i + step / 2;
            float cy = y + r + 2;
            if(i == settings->accent) {
                screen_fill_circle(cx, cy, r + 3, t->palette->text);
                screen_fill_circle(cx, cy, r + 1.5f, t->palette->surface);
            }

            screen_fill_circle(cx, cy, r, t->night ? accent->night : accent->day);
            widget_text_centered(_(accent->name), cx - step / 2, cx + step / 2, cy + r + 4, WIDGET_SMALL_SCALE, t->palette->textMuted);
        }

        return;
    }

    char line[128];
    if(option == OPTION_FTP) {
        char address[64];
        ftpserver_address(address, sizeof(address));
        snprintf(line, sizeof(line), "FTP: %s", address);
    } else if(option == OPTION_NIGHT) {
        snprintf(line, sizeof(line), "%s %s", _("Now:"), t->night ? _("night") : _("day"));
    } else if(option == OPTION_CHANNEL || option == OPTION_AUTO_UPDATE || option == OPTION_CHECK_UPDATES || option == OPTION_CHANGELOG) {
        snprintf(line, sizeof(line), "%s %s", _("Installed:"), VERSION_STRING);
    } else {
        // Theme and language: a sample of the widgets in the current look.
        widget_button(x, y, 120, 28, ICON_CHECK, _("Button"), true);
        widget_progress(x + 132, y + 10, width - 132, 8, 0.64f);
        return;
    }

    screen_draw_text(line, x, y + 6, WIDGET_TEXT_SCALE, t->palette->text);
}

static void settingsmenu_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    settingsmenu_data* menu = (settingsmenu_data*) data;
    const theme_current* t = theme();
    settings_option option = (settings_option) menu->selected;

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = y2 - y1 - 28;
    widget_card(cardX, cardY, cardW, cardH);

    float x = cardX + 16;
    float y = cardY + 14;
    screen_draw_icon(settingsmenu_icon(option), x, y, 22, t->accent);
    screen_draw_text(settingsmenu_label(option), x + 30, y + 1, WIDGET_TITLE_SCALE, t->palette->text);

    y += 32;
    float helpHeight = 0;
    const char* help = settingsmenu_help(option);
    screen_get_string_size_wrap(NULL, &helpHeight, help, WIDGET_TEXT_SCALE, WIDGET_TEXT_SCALE, cardW - 32);
    screen_draw_text_wrap(help, x, y, WIDGET_TEXT_SCALE, t->palette->textMuted, cardX + cardW - 16);

    settingsmenu_draw_preview(option, x, cardY + cardH - 56, cardW - 32);
}

static void settingsmenu_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    settingsmenu_data* menu = (settingsmenu_data*) data;
    float viewHeight = y2 - y1 - 2 * PAD;
    settingsmenu_clamp_scroll(menu, viewHeight);

    float rowX = x1 + PAD;
    float rowW = x2 - x1 - 2 * PAD - WIDGET_SCROLLBAR_WIDTH - 4;

    screen_set_clip(x1, y1 + PAD / 2, x2 - x1, y2 - y1 - PAD);
    for(u32 i = 0; i < OPTION_COUNT; i++) {
        float y = y1 + PAD + settingsmenu_row_top(i) - menu->scroll;
        if(y + settingsmenu_row_height(i) < y1 || y > y2) {
            continue;
        }

        settings_option option = (settings_option) i;
        u32* volume = settingsmenu_slider_value(option);
        if(volume != NULL) {
            char percent[8];
            snprintf(percent, sizeof(percent), "%lu%%", *volume);
            widget_row_with_slider(rowX, y, rowW, settingsmenu_icon(option), settingsmenu_label(option), theme()->palette->text, settingsmenu_value(option), i == menu->selected, (float) *volume / 100.0f, percent);
        } else {
            widget_row(rowX, y, rowW, WIDGET_ROW_HEIGHT, settingsmenu_icon(option), settingsmenu_label(option), theme()->palette->text, settingsmenu_value(option), i == menu->selected);
        }

        widget_press_draw(&menu->press, (int) i, rowX, y, rowW, WIDGET_ROW_HEIGHT, theme()->def->radiusSmall, i == menu->selected ? theme()->onAccent : theme()->palette->text, false);
    }
    screen_clear_clip();

    widget_scrollbar(x2 - PAD - WIDGET_SCROLLBAR_WIDTH, y1 + PAD, viewHeight, menu->scroll, viewHeight, settingsmenu_total_height());
}

void settingsmenu_open() {
    settingsmenu_data* data = (settingsmenu_data*) calloc(1, sizeof(settingsmenu_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate settings data.");
        return;
    }

    data->press.index = -1;
    data->sliding = -1;

    ui_view* view = ui_create();
    view->name = _("Settings");
    view->info = _(" change    back");
    view->data = data;
    view->update = settingsmenu_update;
    view->drawTop = settingsmenu_draw_top;
    view->drawBottom = settingsmenu_draw_bottom;
    ui_push(view);
}
