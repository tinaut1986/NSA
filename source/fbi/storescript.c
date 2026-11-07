#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <3ds.h>
#include <jansson.h>

#include "storescript.h"
#include "action/action.h"
#include "../core/core.h"
#include "../core/minire.h"
#include "../core/speedlog.h"
#include "../core/unarchive.h"

#define SCRIPT_STEPS_MAX 40
#define SCRIPT_PATH_MAX 256
#define SCRIPT_CREATED_MAX 64
#define SCRIPT_RELEASES_MAX (512 * 1024)

typedef enum {
    STEP_DOWNLOAD,   // a: the URL, b: where it goes.
    STEP_RELEASE,    // a: the GitHub repository ("user/repo"), c: which file of its latest release (a pattern), b: where it goes.
    STEP_EXTRACT,    // a: the archive, b: what to take (a pattern), c: where it goes.
    STEP_INSTALL,    // a: the CIA.
    STEP_DELETE,     // a: the file.
    STEP_MOVE,       // a: from, b: to.
    STEP_MKDIR,      // b: the folder.
    STEP_RMDIR,      // b: the folder (deleted with everything in it, once the user says yes).
    STEP_PROMPT,     // a: the message; a No skips `count` steps (0: stops the script).
    STEP_SKIP        // Skips `count` steps (after a prompt's Yes, the steps for its No).
} script_step_type;

typedef struct {
    u8 type;
    bool prerelease;   // STEP_RELEASE: the newest release, even a pre-release.
    u16 count;
    char a[DOWNLOAD_URL_MAX];
    char b[SCRIPT_PATH_MAX];
    char c[SCRIPT_PATH_MAX];
} script_step;

typedef struct {
    script_step steps[SCRIPT_STEPS_MAX];
    u32 count;
    u32 pc;             // The next step.

    void* userData;
    void (*done)(void* userData, bool ok);

    // The file steps run on a thread, a run of them at a time; a release is looked for on one too.
    volatile bool cancel;
    volatile bool finished;
    Result res;
    u32 current;        // The step under way (for the screen).
    unarchive_progress progress;
    char releaseUrl[DOWNLOAD_URL_MAX];

    bool downloaded;    // The download under way came.
    char installs[SCRIPT_STEPS_MAX * (SCRIPT_PATH_MAX + 1)];   // The CIAs to install at the end, one a line.
    bool deleteInstalled;   // The script deletes them afterwards.
    char last[SCRIPT_PATH_MAX];   // The last file written (said at the end when nothing is installed).

    // What it wrote, the only things it may delete or move.
    char created[SCRIPT_CREATED_MAX][SCRIPT_PATH_MAX];
    u32 createdCount;
} script_run;

// ---- reading a script ----

static const char* script_str(json_t* step, const char* key) {
    json_t* value = json_object_get(step, key);
    return json_is_string(value) ? json_string_value(value) : NULL;
}

// "%3DSX%/app.3dsx" or "sdmc:/app.cia" as "/3ds/app.3dsx" or "/app.cia": Universal-Updater's default folders.
static bool script_resolve(const char* in, char* out, size_t size) {
    if(in == NULL) {
        return false;
    }

    char tmp[SCRIPT_PATH_MAX];
    size_t o = 0;
    if(strncasecmp(in, "sdmc:", 5) == 0) {
        in += 5;
    }

    while(*in != '\0' && o < sizeof(tmp) - 1) {
        const char* value = NULL;
        size_t skip = 0;
        if(strncmp(in, "%3DSX%", 6) == 0) {
            value = "/3ds";
            skip = 6;
        } else if(strncmp(in, "%NDS%", 5) == 0) {
            value = "";
            skip = 5;
        } else if(strncmp(in, "%FIRM%", 6) == 0) {
            value = "/luma/payloads";
            skip = 6;
        } else if(*in == '%') {
            return false;   // A folder NSA does not know.
        }

        if(value != NULL) {
            size_t len = strlen(value);
            if(o + len >= sizeof(tmp)) {
                return false;
            }

            memcpy(tmp + o, value, len);
            o += len;
            in += skip;
            continue;
        }

        tmp[o++] = *in++;
    }

    tmp[o] = '\0';
    if(*in != '\0') {
        return false;
    }

    // One "/" in front and none doubled.
    size_t n = 0;
    if(tmp[0] != '/' && n < size - 1) {
        out[n++] = '/';
    }

    for(size_t i = 0; tmp[i] != '\0' && n < size - 1; i++) {
        if(tmp[i] == '/' && n > 0 && out[n - 1] == '/') {
            continue;
        }

        out[n++] = tmp[i];
    }

    out[n] = '\0';
    return n > 1 && strstr(out, "..") == NULL;
}

static bool script_under(const char* path, const char* folder) {
    size_t len = strlen(folder);
    return strncasecmp(path, folder, len) == 0 && (path[len] == '\0' || path[len] == '/');
}

// Where a script may write: not where the console starts from, not its system data, not NSA's own. Of /luma, only a
// game's own folder in /luma/titles (its mods, which Luma loads into that game alone).
static bool script_writable(const char* path) {
    if(script_under(path, "/luma")) {
        return script_under(path, "/luma/titles") && strlen(path) > strlen("/luma/titles/");
    }

    size_t len = strlen(path);
    bool firm = len >= 5 && strcasecmp(path + len - 5, ".firm") == 0;
    return !firm && !script_under(path, "/Nintendo 3DS")
           && !script_under(path, "/nsa") && !script_under(path, "/gm9") && !script_under(path, "/private");
}

// A folder a script may delete with everything in it (the user is asked first): one it may write to, and not a folder of
// the card's root such as /3ds or /roms, nor /luma/titles itself.
static bool script_removable(const char* path) {
    if(!script_writable(path)) {
        return false;
    }

    const char* second = strchr(path + 1, '/');
    return second != NULL && second[1] != '\0';
}

// Whether `path` is something the script wrote before step `upTo`.
static bool script_made_before(const script_step* steps, u32 upTo, const char* path) {
    for(u32 i = 0; i < upTo; i++) {
        const script_step* s = &steps[i];
        if((s->type == STEP_DOWNLOAD || s->type == STEP_RELEASE || s->type == STEP_MOVE) && strcasecmp(s->b, path) == 0) {
            return true;
        }

        // An extract writes its output, or a whole folder under it.
        if(s->type == STEP_EXTRACT && (strcasecmp(s->c, path) == 0 || (s->c[strlen(s->c) - 1] == '/' && strncasecmp(s->c, path, strlen(s->c)) == 0)
                                       || script_under(path, s->c))) {
            return true;
        }
    }

    return false;
}

static bool script_count(json_t* step, u16* out) {
    json_t* count = json_object_get(step, "count");
    if(count == NULL) {
        *out = 0;
        return true;
    }

    if(!json_is_integer(count) || json_integer_value(count) < 0 || json_integer_value(count) > SCRIPT_STEPS_MAX) {
        return false;
    }

    *out = (u16) json_integer_value(count);
    return true;
}

static bool script_parse(json_t* script, script_step* steps, u32* count) {
    if(!json_is_array(script) || json_array_size(script) == 0 || json_array_size(script) > SCRIPT_STEPS_MAX) {
        return false;
    }

    *count = 0;
    u32 downloads = 0;
    size_t index;
    json_t* step;
    json_array_foreach(script, index, step) {
        const char* type = script_str(step, "type");
        if(type == NULL) {
            return false;
        }

        script_step* s = &steps[*count];
        memset(s, 0, sizeof(*s));
        if(strcmp(type, "downloadFile") == 0) {
            const char* url = script_str(step, "file");
            if(url == NULL || strncmp(url, "https://", 8) != 0 || strlen(url) >= sizeof(s->a)) {
                return false;
            }

            s->type = STEP_DOWNLOAD;
            string_copy(s->a, url, sizeof(s->a));
            if(!script_resolve(script_str(step, "output"), s->b, sizeof(s->b)) || !script_writable(s->b)) {
                return false;
            }

            downloads++;
        } else if(strcmp(type, "downloadRelease") == 0) {
            const char* repo = script_str(step, "repo");
            const char* file = script_str(step, "file");
            if(repo == NULL || file == NULL || strlen(repo) >= sizeof(s->a) || strlen(file) >= sizeof(s->c) || strchr(repo, '/') == NULL
               || strstr(repo, "..") != NULL || strchr(repo, '?') != NULL || strchr(repo, '#') != NULL) {
                return false;
            }

            s->type = STEP_RELEASE;
            string_copy(s->a, repo, sizeof(s->a));
            string_copy(s->c, file, sizeof(s->c));
            s->prerelease = json_is_true(json_object_get(step, "includePrereleases"));
            if(!script_resolve(script_str(step, "output"), s->b, sizeof(s->b)) || !script_writable(s->b)) {
                return false;
            }

            downloads++;
        } else if(strcmp(type, "extractFile") == 0) {
            const char* input = script_str(step, "input");
            if(input == NULL || strlen(input) >= sizeof(s->b) || !script_resolve(script_str(step, "file"), s->a, sizeof(s->a))
               || !script_resolve(script_str(step, "output"), s->c, sizeof(s->c)) || !script_writable(s->c)
               || !script_made_before(steps, *count, s->a)) {
                return false;
            }

            s->type = STEP_EXTRACT;
            string_copy(s->b, input, sizeof(s->b));
        } else if(strcmp(type, "installCia") == 0) {
            if(!script_resolve(script_str(step, "file"), s->a, sizeof(s->a)) || !script_made_before(steps, *count, s->a)) {
                return false;
            }

            s->type = STEP_INSTALL;
        } else if(strcmp(type, "deleteFile") == 0) {
            if(!script_resolve(script_str(step, "file"), s->a, sizeof(s->a)) || !script_made_before(steps, *count, s->a)) {
                return false;
            }

            s->type = STEP_DELETE;
        } else if(strcmp(type, "move") == 0) {
            if(!script_resolve(script_str(step, "old"), s->a, sizeof(s->a)) || !script_made_before(steps, *count, s->a)
               || !script_resolve(script_str(step, "new"), s->b, sizeof(s->b)) || !script_writable(s->b)) {
                return false;
            }

            s->type = STEP_MOVE;
        } else if(strcmp(type, "mkdir") == 0) {
            if(!script_resolve(script_str(step, "directory"), s->b, sizeof(s->b)) || !script_writable(s->b)) {
                return false;
            }

            s->type = STEP_MKDIR;
        } else if(strcmp(type, "rmdir") == 0) {
            if(!script_resolve(script_str(step, "directory"), s->b, sizeof(s->b)) || !script_removable(s->b)) {
                return false;
            }

            s->type = STEP_RMDIR;
        } else if(strcmp(type, "promptMessage") == 0 || strcmp(type, "promptMsg") == 0) {
            const char* message = script_str(step, "message");
            if(message == NULL || !script_count(step, &s->count)) {
                return false;
            }

            s->type = STEP_PROMPT;
            string_copy(s->a, message, sizeof(s->a));
        } else if(strcmp(type, "skip") == 0) {
            if(!script_count(step, &s->count) || s->count == 0) {
                return false;
            }

            s->type = STEP_SKIP;
        } else {
            return false;
        }

        (*count)++;
    }

    return downloads > 0;
}

// Large: kept off the stack (the catalogue is read on one thread).
static script_step script_scratch[SCRIPT_STEPS_MAX];

bool storescript_supported(json_t* script) {
    u32 count = 0;
    return script_parse(script, script_scratch, &count);
}

static bool script_has_ext(const char* path, const char* ext) {
    size_t len = strlen(path);
    size_t extLen = strlen(ext);
    return len >= extLen && strcasecmp(path + len - extLen, ext) == 0;
}

bool storescript_plain_cia(json_t* script) {
    u32 count = 0;
    if(!script_parse(script, script_scratch, &count) || script_scratch[0].type != STEP_DOWNLOAD || !script_has_ext(script_scratch[0].b, ".cia")) {
        return false;
    }

    for(u32 i = 1; i < count; i++) {
        if((script_scratch[i].type != STEP_INSTALL && script_scratch[i].type != STEP_DELETE) || strcasecmp(script_scratch[i].a, script_scratch[0].b) != 0) {
            return false;
        }
    }

    return true;
}

const char* storescript_first_url(json_t* script) {
    size_t index;
    json_t* step;
    json_array_foreach(script, index, step) {
        const char* type = script_str(step, "type");
        if(type != NULL && strcmp(type, "downloadFile") == 0) {
            const char* url = script_str(step, "file");
            return url != NULL ? url : "";
        }
    }

    return "";
}

void storescript_dsx_name(json_t* script, char* out, size_t size) {
    out[0] = '\0';
    size_t index;
    json_t* step;
    json_array_foreach(script, index, step) {
        const char* path = script_str(step, "output");
        if(path == NULL) {
            path = script_str(step, "new");
        }

        if(path == NULL || !script_has_ext(path, ".3dsx")) {
            continue;
        }

        const char* name = strrchr(path, '/');
        name = name != NULL ? name + 1 : path;
        size_t len = strlen(name) - 5;
        if(len >= size) {
            len = size - 1;
        }

        memcpy(out, name, len);
        out[len] = '\0';
    }
}

// ---- running one ----

static void script_next(script_run* run);

static void script_sd(char* out, size_t size, const char* path) {
    snprintf(out, size, "sdmc:%s", path);
}

static bool script_created(script_run* run, const char* path) {
    for(u32 i = 0; i < run->createdCount; i++) {
        if(strcasecmp(run->created[i], path) == 0 || script_under(path, run->created[i])) {
            return true;
        }
    }

    return false;
}

static void script_note_created(script_run* run, const char* path) {
    if(run->createdCount < SCRIPT_CREATED_MAX && !script_created(run, path)) {
        string_copy(run->created[run->createdCount++], path, SCRIPT_PATH_MAX);
    }

    string_copy(run->last, path, sizeof(run->last));
}

static void script_extracted(void* userData, const char* sdPath) {
    // "sdmc:/3ds/app.3dsx" as "/3ds/app.3dsx".
    script_note_created((script_run*) userData, strncmp(sdPath, "sdmc:", 5) == 0 ? sdPath + 5 : sdPath);
}

static void script_finish(script_run* run, bool ok) {
    if(run->done != NULL) {
        run->done(run->userData, ok);
    }

    free(run);
}

static bool script_installing(script_run* run, const char* path) {
    for(u32 i = 0; i < run->count; i++) {
        if(run->steps[i].type == STEP_INSTALL && strcasecmp(run->steps[i].a, path) == 0) {
            return true;
        }
    }

    return false;
}

static void script_delete(script_run* run, const char* path) {
    if(!script_created(run, path)) {
        return;   // Only what this run wrote.
    }

    char sd[SCRIPT_PATH_MAX + 8];
    script_sd(sd, sizeof(sd), path);
    remove(sd);
}

// A script that stops half way (a failure, a cancel, a No that ends it): what it would have deleted goes anyway (its
// downloads and what it took out of them), and nothing is installed.
static void script_abandon(script_run* run) {
    for(u32 i = 0; i < run->count; i++) {
        if(run->steps[i].type == STEP_DELETE) {
            script_delete(run, run->steps[i].a);
        }
    }

    run->installs[0] = '\0';
    script_finish(run, false);
}

// The end: the CIAs install (their screens say how it went), or it says where the files went.
static void script_end(script_run* run) {
    if(run->installs[0] != '\0') {
        action_install_cia_paths_now(run->installs, run->deleteInstalled);
    } else if(run->last[0] != '\0') {
        char message[SCRIPT_PATH_MAX + 64];
        snprintf(message, sizeof(message), _("Saved in %s"), run->last);
        prompt_display_notify("Success", message, COLOR_TEXT, NULL, NULL, NULL);
    } else {
        prompt_display_notify("Success", _("Done"), COLOR_TEXT, NULL, NULL, NULL);
    }

    script_finish(run, true);
}

// Starts a thread below the UI's priority (on a New 3DS on its third core), so the screen keeps moving.
static bool script_start_thread(script_run* run, void (*entry)(void* arg)) {
    bool isNew = false;
    Thread thread = NULL;
    if(R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
        thread = threadCreate(entry, run, 0x8000, 0x38, 2, true);
    }

    if(thread == NULL) {
        thread = threadCreate(entry, run, 0x8000, 0x38, -2, true);
    }

    return thread != NULL;
}

// ---- the steps that only touch the SD card, on a thread ----

static bool script_file_step(u8 type) {
    return type == STEP_EXTRACT || type == STEP_INSTALL || type == STEP_DELETE || type == STEP_MOVE || type == STEP_MKDIR;
}

static void script_make_dirs(char* sdPath) {
    for(char* c = strchr(sdPath + 6, '/'); c != NULL; c = strchr(c + 1, '/')) {
        *c = '\0';
        mkdir(sdPath, 0777);
        *c = '/';
    }
}

static void script_files_thread(void* arg) {
    script_run* run = (script_run*) arg;
    Result res = 0;

    while(run->pc < run->count && script_file_step(run->steps[run->pc].type) && R_SUCCEEDED(res)) {
        if(run->cancel) {
            res = R_APP_CANCELLED;
            break;
        }

        run->current = run->pc;
        script_step* s = &run->steps[run->pc];
        char from[SCRIPT_PATH_MAX + 8];
        char to[SCRIPT_PATH_MAX + 8];
        if(s->type == STEP_EXTRACT) {
            script_sd(from, sizeof(from), s->a);
            script_sd(to, sizeof(to), s->c);
            run->progress.done = 0;
            run->progress.total = 0;
            u32 extracted = 0;
            // speed-log.txt: how long an extraction takes (its line says the time; no bytes are counted).
            speedlog* trace = speedlog_begin("extract", s->c, 0);
            res = unarchive_extract(from, s->b, to, &run->progress, &extracted);
            speedlog_end(trace, R_SUCCEEDED(res));
            // Nothing matched: the archive is not what the catalogue expects, and what should be installed is not there.
            if(R_SUCCEEDED(res) && extracted == 0) {
                res = R_APP_BAD_DATA;
            }
        } else if(s->type == STEP_MOVE) {
            if(!script_created(run, s->a)) {
                res = R_APP_BAD_DATA;   // Not written by this run (a step for it was skipped).
            } else {
                script_sd(from, sizeof(from), s->a);
                script_sd(to, sizeof(to), s->b);
                remove(to);
                script_make_dirs(to);
                if(rename(from, to) != 0) {
                    res = R_APP_IO_ERROR;
                } else {
                    script_note_created(run, s->b);
                }
            }
        } else if(s->type == STEP_MKDIR) {
            snprintf(to, sizeof(to), "sdmc:%s/", s->b);
            script_make_dirs(to);
        } else if(s->type == STEP_INSTALL) {
            size_t used = strlen(run->installs);
            snprintf(run->installs + used, sizeof(run->installs) - used, "%s%s", used > 0 ? "\n" : "", s->a);
        } else if(s->type == STEP_DELETE) {
            // A CIA still to install is deleted by the install itself, once it is in.
            if(script_installing(run, s->a)) {
                run->deleteInstalled = true;
            } else {
                script_delete(run, s->a);
            }
        }

        run->pc++;
    }

    run->res = res;
    run->finished = true;
}

static void script_files_update(ui_view* view, void* data, float* progress, char* text) {
    script_run* run = (script_run*) data;

    if(run->finished) {
        ui_pop();
        info_destroy(view);

        if(R_FAILED(run->res)) {
            if(run->res != R_APP_CANCELLED) {
                error_display_res(NULL, NULL, run->res, _("Failed to take the files out of the archive."));
            }

            script_abandon(run);
            return;
        }

        script_next(run);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        run->cancel = true;
    }

    u64 total = run->progress.total;
    *progress = total > 0 ? (float) ((double) run->progress.done / (double) total) : 0;

    const script_step* s = &run->steps[run->current];
    const char* path = s->type == STEP_MKDIR ? s->b : s->a;
    const char* name = strrchr(path, '/');
    snprintf(text, PROGRESS_TEXT_MAX, "%s\n%s", _("Extracting..."), name != NULL ? name + 1 : path);
}

static void script_run_files(script_run* run) {
    run->finished = false;
    run->res = 0;
    run->current = run->pc;
    ui_view* view = info_display(_("Store"), _("Press B to cancel."), true, run, script_files_update, NULL);
    if(view == NULL || !script_start_thread(run, script_files_thread)) {
        if(view != NULL) {
            // The progress screen closes once `finished` is seen; with no thread it never would.
            run->res = R_APP_THREAD_CREATE_FAILED;
            run->finished = true;
        } else {
            error_display_res(NULL, NULL, R_APP_THREAD_CREATE_FAILED, _("Failed to take the files out of the archive."));
            script_abandon(run);
        }
    }
}

// ---- downloads ----

static void script_downloaded(void* data, u32 index) {
    ((script_run*) data)->downloaded = true;
}

static void script_download_done(void* data) {
    script_run* run = (script_run*) data;
    if(!run->downloaded) {
        // The download screens said why (or it was cancelled).
        script_abandon(run);
        return;
    }

    script_note_created(run, run->steps[run->pc].b);
    run->pc++;
    script_next(run);
}

static void script_download(script_run* run, const char* url) {
    run->downloaded = false;
    action_download_urls_to(url, run->steps[run->pc].b, run, script_downloaded, script_download_done);
}

// A file of a repository's latest release on GitHub (or the newest, pre-releases too): its link, found by its name.
static void script_release_thread(void* arg) {
    script_run* run = (script_run*) arg;
    const script_step* s = &run->steps[run->pc];
    char url[DOWNLOAD_URL_MAX];
    snprintf(url, sizeof(url), s->prerelease ? "https://api.github.com/repos/%s/releases?per_page=5" : "https://api.github.com/repos/%s/releases/latest", s->a);

    json_t* json = NULL;
    run->releaseUrl[0] = '\0';
    run->res = http_download_json(url, &json, SCRIPT_RELEASES_MAX);
    if(R_SUCCEEDED(run->res)) {
        json_t* release = json_is_array(json) ? json_array_get(json, 0) : json;
        json_t* assets = json_object_get(release, "assets");
        size_t index;
        json_t* asset;
        json_array_foreach(assets, index, asset) {
            const char* name = script_str(asset, "name");
            const char* link = script_str(asset, "browser_download_url");
            size_t start = 0;
            size_t end = 0;
            if(name != NULL && link != NULL && strncmp(link, "https://", 8) == 0 && minire_search(s->c, name, &start, &end) && start == 0
               && end == strlen(name)) {
                string_copy(run->releaseUrl, link, sizeof(run->releaseUrl));
                break;
            }
        }

        if(run->releaseUrl[0] == '\0') {
            run->res = R_APP_BAD_DATA;
        }
    }

    json_decref(json);
    run->finished = true;
}

static void script_release_update(ui_view* view, void* data, float* progress, char* text) {
    script_run* run = (script_run*) data;
    snprintf(text, PROGRESS_TEXT_MAX, "%s", _("Looking for the latest release..."));
    if(!run->finished) {
        return;
    }

    ui_pop();
    info_destroy(view);
    if(R_FAILED(run->res)) {
        error_display_res(NULL, NULL, run->res, _("Failed to find the release's file."));
        script_abandon(run);
        return;
    }

    script_download(run, run->releaseUrl);
}

// ---- questions ----

static void script_prompt_answered(ui_view* view, void* data, u32 response) {
    script_run* run = (script_run*) data;
    const script_step* s = &run->steps[run->pc];
    if(response == PROMPT_YES) {
        run->pc++;
    } else if(s->count > 0) {
        run->pc += 1 + s->count;
    } else {
        script_abandon(run);
        return;
    }

    script_next(run);
}

static void script_rmdir_answered(ui_view* view, void* data, u32 response) {
    script_run* run = (script_run*) data;
    if(response == PROMPT_YES) {
        FS_Archive sd = 0;
        if(R_SUCCEEDED(fs_open_archive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
            fs_delete_dir_recursively(sd, run->steps[run->pc].b);
            fs_close_archive(sd);
        }
    }

    run->pc++;
    script_next(run);
}

// ---- the steps, in order ----

static void script_next(script_run* run) {
    while(run->pc < run->count) {
        script_step* s = &run->steps[run->pc];
        switch(s->type) {
            case STEP_SKIP:
                run->pc += 1 + s->count;
                continue;
            case STEP_PROMPT:
                // The store's own words, as it wrote them.
                prompt_display_yes_no("Confirmation", s->a, COLOR_TEXT, run, NULL, script_prompt_answered);
                return;
            case STEP_RMDIR: {
                char sd[SCRIPT_PATH_MAX + 8];
                script_sd(sd, sizeof(sd), s->b);
                struct stat st;
                if(stat(sd, &st) != 0) {
                    run->pc++;
                    continue;
                }

                static char question[SCRIPT_PATH_MAX + 128];
                snprintf(question, sizeof(question), _("Delete this folder and everything in it?\n\n%s"), s->b);
                prompt_display_warning("Confirmation", question, COLOR_TEXT, run, NULL, script_rmdir_answered);
                return;
            }
            case STEP_DOWNLOAD:
                script_download(run, s->a);
                return;
            case STEP_RELEASE:
                run->finished = false;
                if(info_display(_("Store"), "", false, run, script_release_update, NULL) == NULL) {
                    error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("Failed to find the release's file."));
                    script_abandon(run);
                } else if(!script_start_thread(run, script_release_thread)) {
                    // The screen closes once `finished` is seen, and says why.
                    run->res = R_APP_THREAD_CREATE_FAILED;
                    run->finished = true;
                }

                return;
            default:
                script_run_files(run);
                return;
        }
    }

    script_end(run);
}

static void script_question_answered(ui_view* view, void* data, u32 response) {
    script_run* run = (script_run*) data;
    if(response != PROMPT_YES) {
        script_finish(run, false);
        return;
    }

    script_next(run);
}

void storescript_run(const char* scriptText, const char* question, void* userData, void (*done)(void* userData, bool ok)) {
    script_run* run = (script_run*) calloc(1, sizeof(script_run));
    json_error_t error;
    json_t* script = scriptText != NULL ? json_loads(scriptText, 0, &error) : NULL;
    if(run == NULL || !script_parse(script, run->steps, &run->count)) {
        json_decref(script);
        free(run);
        error_display_res(NULL, NULL, R_APP_PARSE_FAILED, _("Failed to open the Store."));
        if(done != NULL) {
            done(userData, false);
        }

        return;
    }

    json_decref(script);
    run->userData = userData;
    run->done = done;
    run->progress.cancel = &run->cancel;
    run->progress.userData = run;
    run->progress.created = script_extracted;

    prompt_display_yes_no("Confirmation", question, COLOR_TEXT, run, NULL, script_question_answered);
}
