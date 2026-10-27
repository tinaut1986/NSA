#include <malloc.h>
#include <stdio.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "settings.h"
#include "update.h"
#include "../core/core.h"

// The Updates screen: one list, options change in place with A.

static void updates_toggle_channel();
static void updates_toggle_auto();

static list_item channel = {"", COLOR_TEXT, updates_toggle_channel};
static list_item autoUpdate = {"", COLOR_TEXT, updates_toggle_auto};
static list_item checkNow = {"Check For Updates", COLOR_TEXT, NULL};
static list_item history = {"Changelog", COLOR_TEXT, NULL};

static void updates_refresh_names() {
    settings_data* settings = settings_get();
    snprintf(channel.name, LIST_ITEM_NAME_MAX, "Channel: %s", settings->channel == UPDATE_CHANNEL_BETA ? "Beta" : "Stable");
    snprintf(autoUpdate.name, LIST_ITEM_NAME_MAX, "Automatic Updates: %s", settings->autoUpdate ? "On" : "Off");
}

static void updates_save() {
    Result res = settings_save();
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to save settings.");
    }
}

static void updates_toggle_channel() {
    settings_data* settings = settings_get();
    settings->channel = settings->channel == UPDATE_CHANNEL_BETA ? UPDATE_CHANNEL_STABLE : UPDATE_CHANNEL_BETA;
    updates_refresh_names();
    updates_save();
}

static void updates_toggle_auto() {
    settings_data* settings = settings_get();
    settings->autoUpdate = !settings->autoUpdate;
    updates_refresh_names();
    updates_save();
}

static void updates_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    settings_data* settings = settings_get();

    const char* help = "";
    if(selected == &channel) {
        help = settings->channel == UPDATE_CHANNEL_BETA
               ? "Beta: the newest release, beta or stable.\nThe changelog lists every release since yours."
               : "Stable: stable releases only.\nThe changelog lists everything since yours,\nincluding the betas in between.";
    } else if(selected == &autoUpdate) {
        help = settings->autoUpdate
               ? "On: check when " APP_NAME " starts and ask\nbefore installing an update."
               : "Off: only check from this screen.";
    } else if(selected == &checkNow) {
        help = "Look for a newer release in your channel now.";
    } else if(selected == &history) {
        help = "Every release of your channel, newest first.";
    }

    char text[512];
    snprintf(text, sizeof(text), "Installed: %s\nBased on " APP_UPSTREAM_VERSION "\n\n%s", VERSION_STRING, help);

    float textWidth;
    float textHeight;
    screen_get_string_size(&textWidth, &textHeight, text, 0.5f, 0.5f);
    screen_draw_string(text, x1 + (x2 - x1 - textWidth) / 2, y1 + (y2 - y1 - textHeight) / 2, 0.5f, 0.5f, COLOR_TEXT, true);
}

static void updates_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);

        return;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        if(selected == &checkNow) {
            update_check_start(false);
        } else if(selected == &history) {
            update_show_history();
        } else if(selected->data != NULL) {
            ((void (*)()) selected->data)();
        }

        return;
    }

    if(linked_list_size(items) == 0) {
        updates_refresh_names();
        linked_list_add(items, &channel);
        linked_list_add(items, &autoUpdate);
        linked_list_add(items, &checkNow);
        linked_list_add(items, &history);
    }
}

void updates_open() {
    list_display("Updates", "A: Select, B: Return", NULL, updates_update, updates_draw_top);
}
