#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <jansson.h>

#include "changelog.h"
#include "resources.h"
#include "section.h"
#include "settings.h"
#include "update.h"
#include "update_manifest.h"
#include "action/action.h"
#include "../core/core.h"

#define UPDATE_RELEASES_URL "https://api.github.com/repos/" APP_REPOSITORY "/releases?per_page=100"
#define UPDATE_RESPONSE_MAX (1024 * 1024)

typedef enum {
    CHECK_UPDATE,         // Offer the newest release of the channel.
    CHECK_UPDATE_SILENT,  // The same, at start-up: quiet unless one is found.
    CHECK_HISTORY         // Show the channel's whole changelog.
} check_mode;

typedef struct {
    check_mode mode;
    bool betas;
    bool homebrew;

    volatile bool done;
    Result res;
    int found;
    update_release release;

    ui_view* waitView;
} update_check;

// One check at a time; update_poll() hands its result to the UI.
static update_check* current = NULL;
static Thread currentThread = NULL;

static void update_check_thread(void* arg) {
    update_check* check = (update_check*) arg;

    char* response = (char*) malloc(UPDATE_RESPONSE_MAX);
    if(response != NULL) {
        u32 size = 0;
        if(R_SUCCEEDED(check->res = http_download_buffer(UPDATE_RELEASES_URL, &size, response, UPDATE_RESPONSE_MAX))) {
            // A full buffer means the response was cut short.
            if(size >= UPDATE_RESPONSE_MAX) {
                check->res = R_APP_OUT_OF_RANGE;
            } else {
                const char* installed = check->mode == CHECK_HISTORY ? "0.0.0" : VERSION_STRING;
                check->found = update_parse_releases(response, size, check->betas, check->homebrew, installed, &check->release);
                if(check->found < 0) {
                    check->res = R_APP_PARSE_FAILED;
                }
            }
        }

        free(response);
    } else {
        check->res = R_APP_OUT_OF_MEMORY;
    }

    check->done = true;
}

static bool update_is_available(update_check* check) {
    if(check->found != 1) {
        return false;
    }

    char candidate[64];
    snprintf(candidate, sizeof(candidate), "%s%s", check->release.version, check->release.prerelease ? "-beta" : "");
    return update_is_newer(candidate, VERSION_STRING);
}

typedef struct {
    changelog_text* text;
    char url[512];
    char message[128];
} update_offer;

static void update_offer_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    changelog_text_draw(((update_offer*) data)->text, x1, y1, x2, y2);
}

static void update_offer_onresponse(ui_view* view, void* data, u32 response) {
    update_offer* offer = (update_offer*) data;

    if(response == PROMPT_YES) {
        action_install_url(NULL, offer->url, fs_get_3dsx_path(), NULL, NULL, NULL, NULL);
    }

    changelog_text_free(offer->text);
    free(offer);
}

static void update_offer_display(update_release* release) {
    update_offer* offer = (update_offer*) calloc(1, sizeof(update_offer));
    if(offer == NULL || !update_allowed_download_url(release->url)) {
        free(offer);
        error_display_res(NULL, NULL, R_APP_BAD_DATA, "Failed to read the update.");
        return;
    }

    offer->text = changelog_text_create(release->notes);
    string_copy(offer->url, release->url, sizeof(offer->url));
    snprintf(offer->message, sizeof(offer->message), APP_NAME " %s%s is available.\nInstalled: %s\n\nUpdate now?",
             release->version, release->prerelease ? " BETA" : "", VERSION_STRING);

    prompt_display_yes_no("Update Available", offer->message, COLOR_TEXT, offer, update_offer_draw_top, update_offer_onresponse);
}

static void update_check_finish(update_check* check) {
    bool available = update_is_available(check);

    if(check->mode == CHECK_HISTORY) {
        if(R_FAILED(check->res)) {
            error_display_res(NULL, NULL, check->res, "Failed to download the changelog.");
        } else if(check->found == 0) {
            prompt_display_notify("Changelog", "No releases in this channel yet.", COLOR_TEXT, NULL, NULL, NULL);
        } else {
            changelog_display("Changelog", changelog_text_create(check->release.notes));
        }
    } else if(available) {
        update_offer_display(&check->release);
    } else if(check->mode == CHECK_UPDATE) {
        if(R_FAILED(check->res)) {
            error_display_res(NULL, NULL, check->res, "Failed to check for updates.");
        } else {
            prompt_display_notify("Updates", "No updates available.", COLOR_TEXT, NULL, NULL, NULL);
        }
    }
}

static void update_wait_update(ui_view* view, void* data, float* progress, char* text) {
    // update_poll() closes this view when the check is done.
}

static void update_start(check_mode mode) {
    if(current != NULL) {
        // The start-up check is still running: let it answer this request.
        if(current->mode == CHECK_UPDATE_SILENT && mode == CHECK_UPDATE) {
            current->mode = CHECK_UPDATE;
            current->waitView = info_display("Updates", "", false, NULL, update_wait_update, NULL);
        } else {
            prompt_display_notify("Updates", "Already checking. Try again in a moment.", COLOR_TEXT, NULL, NULL, NULL);
        }

        return;
    }

    update_check* check = (update_check*) calloc(1, sizeof(update_check));
    if(check == NULL) {
        if(mode != CHECK_UPDATE_SILENT) {
            error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, "Failed to check for updates.");
        }

        return;
    }

    check->mode = mode;
    check->betas = settings_get()->channel == UPDATE_CHANNEL_BETA;
    check->homebrew = fs_get_3dsx_path() != NULL;

    currentThread = threadCreate(update_check_thread, check, 0x10000, 0x18, 1, false);
    if(currentThread == NULL) {
        if(mode != CHECK_UPDATE_SILENT) {
            error_display_res(NULL, NULL, R_APP_THREAD_CREATE_FAILED, "Failed to check for updates.");
        }

        free(check);
        return;
    }

    current = check;
    if(mode != CHECK_UPDATE_SILENT) {
        check->waitView = info_display(mode == CHECK_HISTORY ? "Changelog" : "Updates", "",
                                       false, NULL, update_wait_update, NULL);
    }
}

void update_check_start(bool silent) {
    if(silent) {
        // No Wi-Fi: nothing to check, and nothing to say about it.
        u32 wifi = 0;
        if(R_FAILED(ACU_GetWifiStatus(&wifi)) || wifi == 0) {
            return;
        }
    }

    update_start(silent ? CHECK_UPDATE_SILENT : CHECK_UPDATE);
}

void update_show_history() {
    update_start(CHECK_HISTORY);
}

void update_poll() {
    if(current == NULL || !current->done) {
        return;
    }

    update_check* check = current;
    current = NULL;

    threadJoin(currentThread, U64_MAX);
    threadFree(currentThread);
    currentThread = NULL;

    // The wait view takes no input, so it is still on top; if anything ever
    // covers it, leaking it beats destroying a view that is still stacked.
    if(check->waitView != NULL && ui_top() == check->waitView) {
        ui_pop();
        info_destroy(check->waitView);
    }

    update_check_finish(check);
    free(check);
}
