#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"
#include "../../core/unarchive.h"

// Files -> a .zip or .7z on the SD card -> Extract here / Extract to a folder (issue #34): everything in it, on a
// thread below the UI's priority, with a progress bar B cancels (what was written by then stays).

typedef struct {
    char archive[FILE_PATH_MAX + 8];   // "sdmc:/path/name.zip"
    char output[FILE_PATH_MAX + 8];    // "sdmc:/path/" or "sdmc:/path/name/"
    char shown[FILE_PATH_MAX];         // The destination as the user knows it: "/path/name/"
    char name[FILE_NAME_MAX];          // The archive's own name, for the progress screen.
    char pattern[FILE_PATH_MAX + 8];   // What to take (minire.h): "^" for everything, "^Folder/" for one folder's.
    bool guarded;                      // To the SD card's own folders: never where the console starts from, or NSA's.
    bool deleteAfter;                  // The archive goes once all of it is out.
    bool sdLayout;                     // Only what goes in the SD card's own folders (action_extract_sd_place)...
    char top[FILE_NAME_MAX];           // ... under this top folder of the archive ("": at its root).

    volatile bool cancel;
    unarchive_progress progress;
    u32 count;
    Result res;
    volatile bool finished;

    void (*done)(void* userData);
    void* userData;
} extract_data;

static bool action_extract_under(const char* path, const char* folder) {
    size_t len = strlen(folder);
    return strncasecmp(path, folder, len) == 0 && (path[len] == '\0' || path[len] == '/');
}

// An archive taken out onto the SD card's own folders (a remote install, issue #51): what the console starts from
// (any .firm, /boot.3dsx), Luma's folders but a game's mods and the plugins, GodMode9's, /private, the console's data and
// NSA's are refused, and the whole extraction with them.
static bool action_extract_allowed(void* userData, const char* path) {
    if(strncmp(path, "sdmc:", 5) == 0) {
        path += 5;
    }

    size_t len = strlen(path);
    if((len >= 5 && strcasecmp(path + len - 5, ".firm") == 0) || strcasecmp(path, "/boot.3dsx") == 0) {
        return false;
    }

    // The folders on the way to the allowed ones (an archive lists "luma/" and "luma/titles/" as entries too).
    if(strcasecmp(path, "/luma/") == 0 || strcasecmp(path, "/luma/titles/") == 0) {
        return true;
    }

    if(action_extract_under(path, "/luma")) {
        return (action_extract_under(path, "/luma/titles") && len > strlen("/luma/titles/")) || action_extract_under(path, "/luma/plugins");
    }

    return !action_extract_under(path, "/gm9") && !action_extract_under(path, "/private") && !action_extract_under(path, "/Nintendo 3DS")
           && !action_extract_under(path, "/nsa");
}

bool action_extract_sd_place(const char* path) {
    return strncasecmp(path, "3ds/", 4) == 0 || strncasecmp(path, "cias/", 5) == 0 || strncasecmp(path, "luma/plugins/", 13) == 0
           || (strncasecmp(path, "luma/titles/", 12) == 0 && strchr(path + 12, '/') != NULL);
}

// The loose files beside them (a README, a LICENSE) stay in the archive.
static bool action_extract_wanted(void* userData, const char* name) {
    extract_data* data = (extract_data*) userData;
    while(name[0] == '/' || (name[0] == '.' && name[1] == '/')) {
        name += name[0] == '/' ? 1 : 2;
    }

    size_t topLen = strlen(data->top);
    if(topLen > 0) {
        if(strncmp(name, data->top, topLen) != 0 || name[topLen] != '/') {
            return false;
        }

        name += topLen + 1;
    }

    return action_extract_sd_place(name);
}

static void action_extract_thread(void* arg) {
    extract_data* data = (extract_data*) arg;
    // "^" matches at the start of every path: each entry goes to the destination plus its whole path.
    data->res = unarchive_extract(data->archive, data->pattern[0] != '\0' ? data->pattern : "^", data->output, &data->progress, &data->count);
    if(R_SUCCEEDED(data->res) && data->deleteAfter) {
        remove(data->archive);
    }

    data->finished = true;
}

static void action_extract_update(ui_view* view, void* data, float* progress, char* text) {
    extract_data* extractData = (extract_data*) data;

    if(extractData->finished) {
        ui_pop();
        info_destroy(view);

        if(extractData->done != NULL) {
            extractData->done(extractData->userData);
        }

        if(R_FAILED(extractData->res)) {
            if(extractData->res != R_APP_CANCELLED) {
                error_display_res(NULL, NULL, extractData->res, _("Failed to take the files out of the archive."));
            }
        } else if(extractData->count == 0) {
            prompt_display_notify("Success", _("The archive had no files."), COLOR_TEXT, NULL, NULL, NULL);
        } else {
            char message[FILE_PATH_MAX + 64];
            snprintf(message, sizeof(message), extractData->count == 1 ? _("%lu file extracted to %s") : _("%lu files extracted to %s"),
                     extractData->count, extractData->shown);
            prompt_display_notify("Success", message, COLOR_TEXT, NULL, NULL, NULL);
        }

        free(extractData);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        extractData->cancel = true;
    }

    u64 total = extractData->progress.total;
    *progress = total > 0 ? (float) ((double) extractData->progress.done / (double) total) : 0;
    snprintf(text, PROGRESS_TEXT_MAX, "%s\n%s", _("Extracting..."), extractData->name);
}

static void action_extract_start(extract_data* data) {
    data->progress.cancel = &data->cancel;
    if(data->guarded) {
        data->progress.allowed = action_extract_allowed;
    }

    if(data->sdLayout) {
        data->progress.userData = data;
        data->progress.wanted = action_extract_wanted;
    }

    ui_view* view = info_display(_("Extract"), _("Press B to cancel."), true, data, action_extract_update, NULL);
    if(view == NULL) {
        free(data);
        return;
    }

    // Below the UI's priority; on a New 3DS on its third core, so the screen keeps moving.
    bool isNew = false;
    Thread thread = NULL;
    if(R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
        thread = threadCreate(action_extract_thread, data, 0x8000, 0x38, 2, true);
    }

    if(thread == NULL) {
        thread = threadCreate(action_extract_thread, data, 0x8000, 0x38, -2, true);
    }

    if(thread == NULL) {
        // The progress screen closes once `finished` is seen.
        data->res = R_APP_THREAD_CREATE_FAILED;
        data->finished = true;
    }
}

static void action_extract_here_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        action_extract_start((extract_data*) data);
    } else {
        free(data);
    }
}

void action_extract_archive_to(const char* path, const char* pattern, const char* output, const char* shown, bool deleteAfter,
                               const char* sdLayoutTop, void (*done)(void* userData), void* userData) {
    extract_data* data = (extract_data*) calloc(1, sizeof(extract_data));
    if(data == NULL) {
        error_display(NULL, NULL, _("Failed to take the files out of the archive."));
        if(done != NULL) {
            done(userData);
        }

        return;
    }

    data->done = done;
    data->userData = userData;
    data->guarded = true;
    data->deleteAfter = deleteAfter;
    data->sdLayout = sdLayoutTop != NULL;
    string_copy(data->top, sdLayoutTop != NULL ? sdLayoutTop : "", sizeof(data->top));
    snprintf(data->archive, sizeof(data->archive), "sdmc:%s", path);
    string_copy(data->pattern, pattern, sizeof(data->pattern));
    string_copy(data->output, output, sizeof(data->output));
    string_copy(data->shown, shown, sizeof(data->shown));
    const char* slash = strrchr(path, '/');
    string_copy(data->name, slash != NULL ? slash + 1 : path, sizeof(data->name));
    action_extract_start(data);
}

void action_extract_archive(const char* path, bool intoFolder, void (*done)(void* userData), void* userData) {
    extract_data* data = (extract_data*) calloc(1, sizeof(extract_data));
    if(data == NULL) {
        error_display(NULL, NULL, _("Failed to take the files out of the archive."));
        return;
    }

    data->done = done;
    data->userData = userData;
    snprintf(data->archive, sizeof(data->archive), "sdmc:%s", path);

    const char* slash = strrchr(path, '/');
    const char* name = slash != NULL ? slash + 1 : path;
    string_copy(data->name, name, sizeof(data->name));

    char dir[FILE_PATH_MAX];
    size_t dirLen = slash != NULL ? (size_t) (slash - path) + 1 : 0;
    if(dirLen >= sizeof(dir)) {
        dirLen = sizeof(dir) - 1;
    }

    memcpy(dir, path, dirLen);
    dir[dirLen] = '\0';

    if(!intoFolder) {
        string_copy(data->shown, dir, sizeof(data->shown));
        snprintf(data->output, sizeof(data->output), "sdmc:%s", data->shown);
        prompt_display_yes_no(_("Extract here"), _("Files with the same names in this folder are replaced."), COLOR_TEXT, data, NULL,
                              action_extract_here_onresponse);
        return;
    }

    // A folder named after the archive, without its extension; "name (2)" and on when it is taken.
    char base[FILE_NAME_MAX];
    string_get_file_name(base, name, sizeof(base));
    struct stat st;
    char folder[FILE_PATH_MAX + 8];
    snprintf(folder, sizeof(folder), "sdmc:%s%s", dir, base);
    for(u32 n = 2; n < 1000 && stat(folder, &st) == 0; n++) {
        snprintf(folder, sizeof(folder), "sdmc:%s%s (%lu)", dir, base, n);
    }

    snprintf(data->output, sizeof(data->output), "%s/", folder);
    string_copy(data->shown, data->output + 5, sizeof(data->shown));

    action_extract_start(data);
}
