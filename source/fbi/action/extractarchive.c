#include <malloc.h>
#include <stdio.h>
#include <string.h>
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

    volatile bool cancel;
    unarchive_progress progress;
    u32 count;
    Result res;
    volatile bool finished;

    void (*done)(void* userData);
    void* userData;
} extract_data;

static void action_extract_thread(void* arg) {
    extract_data* data = (extract_data*) arg;
    // "^" matches at the start of every path: each entry goes to the destination plus its whole path.
    data->res = unarchive_extract(data->archive, "^", data->output, &data->progress, &data->count);
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
