#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <jansson.h>

#include "changelog.h"
#include "resources.h"
#include "section.h"
#include "relaunch.h"
#include "settings.h"
#include "update.h"
#include "udslink.h"
#include "update_manifest.h"
#include "action/action.h"
#include "../core/core.h"

// Builds up to v0.9.9 also read their changelog from here: the release bodies keep only English notes, so what
// they download stays as it was. The changelog of later builds comes a page at a time, in its language, from
// GitHub Pages (tools/make_changelog.py).
#define UPDATE_RELEASES_URL "https://api.github.com/repos/" APP_REPOSITORY "/releases?per_page=100"
#define UPDATE_HISTORY_URL APP_PAGES_URL "changelog/%s/%u.json"
#define UPDATE_RESPONSE_MAX (1024 * 1024)
#define UPDATE_HISTORY_NOTES_MAX (64 * 1024)
#define UPDATE_HISTORY_INDEX_MAX 512

typedef enum {
    CHECK_UPDATE,         // Offer the newest release of the channel.
    CHECK_UPDATE_SILENT,  // The same, at start-up: quiet unless one is found.
    CHECK_HISTORY         // Show the changelog, a page at a time.
} check_mode;

typedef struct {
    check_mode mode;   // B on the wait view turns CHECK_HISTORY into CHECK_UPDATE_SILENT: nothing is shown.
    bool betas;
    bool homebrew;
    char language[8];  // Of the notes, as i18n_language_code.

    // The changelog: the page to ask for (then the one after the last read), whether it is a later page for the
    // changelog already open, and what was read.
    bool history;
    bool more;
    unsigned page;
    bool last;
    char* notes;
    update_history_entry* index;   // Every release, from page 1.
    unsigned indexCount;

    volatile bool done;
    Result res;
    int found;
    update_release release;

    ui_view* waitView;
    u64 started;   // When it was asked for, for the transfer log's timings.
} update_check;

// One check at a time; update_poll() hands its result to the UI.
static update_check* current = NULL;
static Thread currentThread = NULL;

// Pages of the changelog from check->page on, until one has a release for this build (every release in a page can be
// newer than the installed one) or the list ends.
static void update_history_pages(update_check* check, char* response) {
    check->notes = (char*) malloc(UPDATE_HISTORY_NOTES_MAX);
    if(check->notes == NULL) {
        check->res = R_APP_OUT_OF_MEMORY;
        return;
    }

    check->notes[0] = '\0';
    check->found = 0;
    while(!check->last) {
        char url[192];
        snprintf(url, sizeof(url), UPDATE_HISTORY_URL, check->language, check->page);

        u32 size = 0;
        if(R_FAILED(check->res = http_download_buffer(url, &size, response, UPDATE_RESPONSE_MAX))) {
            // A language the pages do not have yet (added after the last release): English.
            if(check->res != (Result) (R_APP_HTTP_ERROR_BASE + 404) || strcmp(check->language, "en") == 0) {
                return;
            }

            udslink_log("update: %s failed (%08lX), trying English\n", url, check->res);
            string_copy(check->language, "en", sizeof(check->language));
            continue;
        }

        if(size >= UPDATE_RESPONSE_MAX) {
            check->res = R_APP_OUT_OF_RANGE;
            return;
        }

        unsigned pages = 0;
        check->found = update_parse_history(response, size, VERSION_STRING, check->notes, UPDATE_HISTORY_NOTES_MAX, &pages);
        if(check->found < 0) {
            check->res = R_APP_PARSE_FAILED;
            return;
        }

        if(check->page == 1 && check->index == NULL) {
            check->index = (update_history_entry*) malloc(UPDATE_HISTORY_INDEX_MAX * sizeof(update_history_entry));
            if(check->index != NULL) {
                check->indexCount = update_parse_history_index(response, size, VERSION_STRING, check->index, UPDATE_HISTORY_INDEX_MAX);
            }
        }

        udslink_log("update: changelog page %u of %u (%s): %d shown (%llu ms after asking)\n", check->page, pages, check->language,
                    check->found, osGetTime() - check->started);
        check->page++;
        check->last = check->page > pages;
        if(check->found > 0) {
            return;
        }
    }
}

static void update_check_thread(void* arg) {
    update_check* check = (update_check*) arg;

    char* response = (char*) malloc(UPDATE_RESPONSE_MAX);
    if(response != NULL && check->history) {
        update_history_pages(check, response);
        free(response);
    } else if(response != NULL) {
        u32 size = 0;
        udslink_log("update: check thread running after %llu ms\n", osGetTime() - check->started);
        if(R_SUCCEEDED(check->res = http_download_buffer(UPDATE_RELEASES_URL, &size, response, UPDATE_RESPONSE_MAX))) {
            u64 parseStart = osGetTime();
            // A full buffer means the response was cut short.
            if(size >= UPDATE_RESPONSE_MAX) {
                check->res = R_APP_OUT_OF_RANGE;
            } else {
                check->found = update_parse_releases(response, size, check->betas, check->homebrew, VERSION_STRING, &check->release);
                if(check->found < 0) {
                    check->res = R_APP_PARSE_FAILED;
                }

                udslink_log("update: %lu bytes of releases read in %llu ms (%llu ms after asking)\n", size, osGetTime() - parseStart,
                            osGetTime() - check->started);
            }
        }

        free(response);
    } else {
        check->res = R_APP_OUT_OF_MEMORY;
    }

    check->done = true;
}

static bool update_is_available(update_check* check) {
    if(check->history || check->found != 1) {
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

static bool update_installed = false;

static void update_install_finished_url(void* data, u32 index) {
    update_installed = true;
}

// After the install (or its failure or cancel): restart into a new build.
static void update_install_finished_all(void* data) {
    if(update_installed) {
        update_installed = false;
        relaunch_request();
    }
}

static void update_offer_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    changelog_text_draw(((update_offer*) data)->text, x1, y1, x2, y2);
}

static void update_offer_onresponse(ui_view* view, void* data, u32 response) {
    update_offer* offer = (update_offer*) data;

    if(response == PROMPT_YES) {
        udslink_log("update: yes, downloading %s\n", offer->url);
        update_installed = false;
        action_install_url(NULL, offer->url, fs_get_3dsx_path(), NULL, update_install_finished_url, update_install_finished_all, NULL);
    }

    changelog_text_free(offer->text);
    free(offer);
}

static void update_offer_display(update_release* release) {
    update_offer* offer = (update_offer*) calloc(1, sizeof(update_offer));
    if(offer == NULL || !update_allowed_download_url(release->url)) {
        free(offer);
        error_display_res(NULL, NULL, R_APP_BAD_DATA, _("Failed to read the update."));
        return;
    }

    offer->text = changelog_text_create(release->notes);
    string_copy(offer->url, release->url, sizeof(offer->url));
    snprintf(offer->message, sizeof(offer->message), _(APP_NAME " %s%s is available.\nInstalled: %s\n\nUpdate now?"),
             release->version, release->prerelease ? " BETA" : "", VERSION_STRING);

    prompt_display_yes_no(_("Update Available"), offer->message, COLOR_TEXT, offer, update_offer_draw_top, update_offer_onresponse);
}

// The changelog on screen, while it is (text is NULL otherwise), and where its next page comes from.
static struct {
    changelog_text* text;
    char language[8];
    unsigned nextPage;
    bool loading;
    bool failed;
    bool end;
    update_history_entry index[UPDATE_HISTORY_INDEX_MAX];
    u32 indexCount;
} history;

static bool update_spawn(update_check* check);

static void update_history_more(void* data, bool retry) {
    if(history.text == NULL || history.loading || history.end || (history.failed && !retry) || current != NULL) {
        return;
    }

    update_check* check = (update_check*) calloc(1, sizeof(update_check));
    if(check == NULL) {
        history.failed = true;
        return;
    }

    check->mode = CHECK_HISTORY;
    check->history = true;
    check->more = true;
    check->page = history.nextPage;
    check->started = osGetTime();
    check->homebrew = fs_get_3dsx_path() != NULL;
    string_copy(check->language, history.language, sizeof(check->language));
    if(!update_spawn(check)) {
        free(check);
        history.failed = true;
        return;
    }

    current = check;
    history.loading = true;
    history.failed = false;
}

static const char* update_history_status(void* data) {
    if(history.loading) {
        return _("Loading older releases...");
    }

    return history.failed ? _("Could not load older releases. \uE000 retry") : NULL;
}

static void update_history_closed(void* data) {
    history.text = NULL;
}

static bool update_history_finished(void* data) {
    return history.end || history.failed;
}

static void update_history_finish(update_check* check) {
    if(check->more) {
        history.loading = false;
        if(history.text == NULL) {
            return;
        }

        if(R_FAILED(check->res) || (check->found > 0 && !changelog_text_append(history.text, check->notes))) {
            history.failed = true;
            return;
        }

        history.nextPage = check->page;
        history.end = check->last;
        return;
    }

    if(R_FAILED(check->res)) {
        error_display_res(NULL, NULL, check->res, _("Failed to download the changelog."));
        return;
    }

    if(check->found == 0) {
        prompt_display_notify(_("Changelog"), _("No releases yet."), COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    history.text = changelog_text_create(check->notes);
    string_copy(history.language, check->language, sizeof(history.language));
    history.nextPage = check->page;
    history.loading = false;
    history.failed = false;
    history.end = check->last;
    history.indexCount = 0;
    if(check->index != NULL) {
        memcpy(history.index, check->index, check->indexCount * sizeof(update_history_entry));
        history.indexCount = check->indexCount;
    }

    changelog_source source = {.data = NULL,
                               .more = update_history_more,
                               .status = update_history_status,
                               .closed = update_history_closed,
                               .finished = update_history_finished,
                               .catalog = history.index,
                               .catalogCount = history.indexCount};
    changelog_display_paged(_("Changelog"), history.text, &source);
}

static void update_check_finish(update_check* check) {
    bool available = update_is_available(check);

    if(check->history) {
        // Given up on with B (mode is no longer CHECK_HISTORY): nothing to show.
        if(check->mode == CHECK_HISTORY) {
            update_history_finish(check);
        }
    } else if(available) {
        update_offer_display(&check->release);
    } else if(check->mode == CHECK_UPDATE) {
        if(R_FAILED(check->res)) {
            error_display_res(NULL, NULL, check->res, _("Failed to check for updates."));
        } else {
            prompt_display_notify(_("Updates"), _("No updates available."), COLOR_TEXT, NULL, NULL, NULL);
        }
    }
}

static void update_wait_update(ui_view* view, void* data, float* progress, char* text) {
    // update_poll() closes this view when the check is done. B gives up
    // waiting: the check finishes in the background and says nothing.
    if((hidKeysDown() & KEY_B) && current != NULL && current->waitView == view) {
        current->waitView = NULL;
        current->mode = CHECK_UPDATE_SILENT;
        ui_pop();
        info_destroy(view);
    }
}

static void update_start(check_mode mode) {
    if(current != NULL) {
        // The start-up check is still running: let it answer this request.
        if(current->mode == CHECK_UPDATE_SILENT && mode == CHECK_UPDATE) {
            current->mode = CHECK_UPDATE;
            current->waitView = info_display(_("Updates"), _("\uE001 cancel"), false, NULL, update_wait_update, NULL);
        } else {
            prompt_display_notify(_("Updates"), _("Already checking. Try again in a moment."), COLOR_TEXT, NULL, NULL, NULL);
        }

        return;
    }

    update_check* check = (update_check*) calloc(1, sizeof(update_check));
    if(check == NULL) {
        if(mode != CHECK_UPDATE_SILENT) {
            error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("Failed to check for updates."));
        }

        return;
    }

    check->mode = mode;
    check->started = osGetTime();
    check->betas = settings_get()->channel == UPDATE_CHANNEL_BETA;
    check->homebrew = fs_get_3dsx_path() != NULL;
    check->history = mode == CHECK_HISTORY;
    check->page = 1;
    string_copy(check->language, i18n_language_code(i18n_effective_language()), sizeof(check->language));

    if(!update_spawn(check)) {
        if(mode != CHECK_UPDATE_SILENT) {
            error_display_res(NULL, NULL, R_APP_THREAD_CREATE_FAILED, _("Failed to check for updates."));
        }

        free(check);
        return;
    }

    current = check;
    if(mode != CHECK_UPDATE_SILENT) {
        check->waitView = info_display(mode == CHECK_HISTORY ? _("Changelog") : _("Updates"), _("\uE001 cancel"),
                                       false, NULL, update_wait_update, NULL);
    }
}

static bool update_spawn(update_check* check) {
    // On the app's core (core 2 on a New 3DS), below the UI: on the system core an Old 3DS gives the app only 30% of the
    // time, and reading the releases (200 KB of JSON) took seconds there.
    bool isNew = false;
    currentThread = NULL;
    if(R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
        currentThread = threadCreate(update_check_thread, check, 0x10000, 0x31, 2, false);
    }

    if(currentThread == NULL) {
        currentThread = threadCreate(update_check_thread, check, 0x10000, 0x31, -2, false);
    }

    return currentThread != NULL;
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
    if(history.text != NULL) {
        return;
    }

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

    u64 shown = osGetTime();
    update_check_finish(check);
    udslink_log("update: answer shown %llu ms after asking (%llu ms to show it)\n", osGetTime() - check->started, osGetTime() - shown);
    free(check->notes);
    free(check->index);
    free(check);
}
