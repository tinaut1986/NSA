#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "ftpserver.h"
#include "wifislots.h"
#include "guide.h"
#include "webserver.h"
#include "settings.h"
#include "update.h"
#include "../core/core.h"

// Start-up: offer once to import upstream FBI's data folder, then run the
// automatic update check.

#define LEGACY_DIR "sdmc:" APP_LEGACY_DATA_DIR
#define DATA_DIR "sdmc:" APP_DATA_DIR

// The same folders without the trailing slash, for building paths.
#define LEGACY_ROOT "sdmc:/fbi"
#define DATA_ROOT "sdmc:/nsa"

// NAND dumps are large backups NSA never reads back: they stay in /fbi/.
#define SKIPPED_DIR "nand"

#define COPY_BUFFER_SIZE (128 * 1024)

typedef struct {
    volatile bool done;
    volatile u64 copied;
    u64 total;
    int err;
    char failedPath[256];
} import_data;

static bool startup_is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool startup_skip(const char* name, int depth) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || (depth == 0 && strcmp(name, SKIPPED_DIR) == 0);
}

static u64 startup_measure(const char* dir, int depth) {
    u64 total = 0;

    DIR* d = opendir(dir);
    if(d == NULL) {
        return 0;
    }

    struct dirent* ent;
    while((ent = readdir(d)) != NULL) {
        if(startup_skip(ent->d_name, depth)) {
            continue;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if(stat(path, &st) == 0) {
            total += S_ISDIR(st.st_mode) ? startup_measure(path, depth + 1) : (u64) st.st_size;
        }
    }

    closedir(d);
    return total;
}

static bool startup_copy_file(import_data* data, const char* src, const char* dst, u8* buffer) {
    FILE* in = fopen(src, "rb");
    if(in == NULL) {
        return false;
    }

    FILE* out = fopen(dst, "wb");
    if(out == NULL) {
        fclose(in);
        return false;
    }

    bool ok = true;
    size_t n;
    while((n = fread(buffer, 1, COPY_BUFFER_SIZE, in)) > 0) {
        if(fwrite(buffer, 1, n, out) != n) {
            ok = false;
            break;
        }

        data->copied += n;
    }

    ok = ok && !ferror(in);
    fclose(in);
    ok = fclose(out) == 0 && ok;
    return ok;
}

static bool startup_copy_dir(import_data* data, const char* src, const char* dst, int depth, u8* buffer) {
    if(mkdir(dst, 0777) != 0 && errno != EEXIST) {
        data->err = errno;
        string_copy(data->failedPath, dst, sizeof(data->failedPath));
        return false;
    }

    DIR* d = opendir(src);
    if(d == NULL) {
        data->err = errno;
        string_copy(data->failedPath, src, sizeof(data->failedPath));
        return false;
    }

    bool ok = true;
    struct dirent* ent;
    while(ok && (ent = readdir(d)) != NULL) {
        if(startup_skip(ent->d_name, depth)) {
            continue;
        }

        char srcPath[512];
        char dstPath[512];
        snprintf(srcPath, sizeof(srcPath), "%s/%s", src, ent->d_name);
        snprintf(dstPath, sizeof(dstPath), "%s/%s", dst, ent->d_name);

        if(startup_is_dir(srcPath)) {
            ok = startup_copy_dir(data, srcPath, dstPath, depth + 1, buffer);
        } else if(!startup_copy_file(data, srcPath, dstPath, buffer)) {
            data->err = errno;
            string_copy(data->failedPath, srcPath, sizeof(data->failedPath));
            ok = false;
        }
    }

    closedir(d);
    return ok;
}

static void startup_import_thread(void* arg) {
    import_data* data = (import_data*) arg;

    u8* buffer = (u8*) malloc(COPY_BUFFER_SIZE);
    if(buffer != NULL) {
        startup_copy_dir(data, LEGACY_ROOT, DATA_ROOT, 0, buffer);
        free(buffer);
    } else {
        data->err = ENOMEM;
    }

    data->done = true;
}

static void startup_check_updates() {
    if(settings_get()->autoUpdate) {
        update_check_start(true);
    }
}

// The guide's question (only when it has chapters this console was not offered), then the update check: after the
// guide closes, so its question does not land in the middle of it.
static void startup_guide_then_updates() {
    guide_startup(startup_check_updates);
}

static void startup_music_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        settings_get()->ftpPausesMusic = false;
    }

    settings_save();
    startup_guide_then_updates();
}

// An Old 3DS that kept "Pause music for FTP" on, its old default, is asked once whether to let the music play: with only
// 5% of the system core during transfers (xferturn.h) the music costs its FTP 2-3% instead of 10-25%. Then the updates.
static void startup_next() {
    settings_data* settings = settings_get();
    bool new3ds = false;
    APT_CheckNew3DS(&new3ds);

    if(!settings->ftpMusicAsked) {
        settings->ftpMusicAsked = true;
        if(!new3ds && settings->ftpPausesMusic) {
            prompt_display_yes_no(_("Music and FTP"),
                                  _("The music no longer slows down FTP\n"
                                    "transfers on this console (2-3%,\n"
                                    "it used to be 10-25%).\n\n"
                                    "Let it play during FTP transfers?\n\n"
                                    "Settings -> Pause music for FTP\n"
                                    "changes it later."),
                                  COLOR_TEXT, NULL, NULL, startup_music_onresponse);
            return;
        }

        settings_save();
    }

    startup_guide_then_updates();
}

static void startup_import_update(ui_view* view, void* data, float* progress, char* text) {
    import_data* importData = (import_data*) data;

    if(importData->done) {
        ui_pop();
        info_destroy(view);

        if(importData->err != 0) {
            error_display_errno(NULL, NULL, importData->err, _("Failed to import %s."), importData->failedPath[0] ? importData->failedPath : _("the FBI folder"));
        } else {
            prompt_display_notify(_("Import"), _("FBI data imported into " DATA_DIR "."), COLOR_TEXT, NULL, NULL, NULL);
        }

        free(importData);
        startup_next();
        return;
    }

    *progress = importData->total > 0 ? (float) ((double) importData->copied / (double) importData->total) : 0;
    snprintf(text, PROGRESS_TEXT_MAX, _("%.2f %s / %.2f %s"),
             ui_get_display_size(importData->copied), ui_get_display_size_units(importData->copied),
             ui_get_display_size(importData->total), ui_get_display_size_units(importData->total));
}

static void startup_import_onresponse(ui_view* view, void* data, u32 response) {
    if(response != PROMPT_YES) {
        // Not asked again: the question only comes up while /nsa/ is missing.
        mkdir(DATA_ROOT, 0777);
        startup_next();
        return;
    }

    import_data* importData = (import_data*) calloc(1, sizeof(import_data));
    if(importData == NULL) {
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("Failed to import the FBI folder."));
        startup_next();
        return;
    }

    importData->total = startup_measure(LEGACY_ROOT, 0);

    Thread thread = threadCreate(startup_import_thread, importData, 0x10000, 0x18, 1, true);
    if(thread == NULL) {
        error_display_res(NULL, NULL, R_APP_THREAD_CREATE_FAILED, _("Failed to import the FBI folder."));
        free(importData);
        startup_next();
        return;
    }

    info_display(_("Importing FBI data"), "", true, importData, startup_import_update, NULL);
}

void startup_run() {
    // Debug builds always serve, whatever the saved setting says.
    // Without Wi-Fi now, they start once it comes (they used to stay off until set again).
    if(settings_get()->ftpServer || NSA_DEBUG) {
        ftpserver_start_when_online();
    }

    if(settings_get()->webServer) {
        webserver_start_when_online();
    }

    // Opened without a connection, the system does not connect while NSA is in front (the HOME Menu does it): NSA asks,
    // as a game does, and the servers start once it is up.
    if(!wifislots_online()) {
        static wifislots_job job = {.index = WIFISLOTS_COUNT, .timeoutMs = 30000};
        wifislots_connect_start(&job);
    }

    if(startup_is_dir(LEGACY_ROOT) && !startup_is_dir(DATA_ROOT)) {
        prompt_display_yes_no(_("Import FBI data"),
                              _("Found data from FBI in " LEGACY_DIR ".\n"
                                "Copy it to " DATA_DIR "?\n\n"
                                "Seeds, secure values, saves and the last URLs\n"
                                "are copied. NAND dumps stay where they are."),
                              COLOR_TEXT, NULL, NULL, startup_import_onresponse);
        return;
    }

    startup_next();
}
