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
#include "../core/unarchive.h"

#define SCRIPT_STEPS_MAX 16
#define SCRIPT_PATH_MAX 256
#define SCRIPT_CREATED_MAX 64

typedef enum {
    STEP_DOWNLOAD,   // a: the URL, b: where it goes.
    STEP_EXTRACT,    // a: the archive, b: what to take (a pattern), c: where it goes.
    STEP_INSTALL,    // a: the CIA.
    STEP_DELETE,     // a: the file.
    STEP_MOVE        // a: from, b: to.
} script_step_type;

typedef struct {
    script_step_type type;
    char a[DOWNLOAD_URL_MAX];
    char b[SCRIPT_PATH_MAX];
    char c[SCRIPT_PATH_MAX];
} script_step;

typedef struct {
    script_step steps[SCRIPT_STEPS_MAX];
    u32 count;
    u32 downloads;      // The first `downloads` steps are the downloads.
    u32 downloaded;     // How many came.

    void* userData;
    void (*done)(void* userData, bool ok);

    // The rest, on a thread.
    volatile bool cancel;
    volatile bool finished;
    Result res;
    u32 current;        // The step under way (for the screen).
    unarchive_progress progress;
    char installs[SCRIPT_STEPS_MAX * (SCRIPT_PATH_MAX + 1)];   // The CIAs to install, one a line.
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

// Where a script may write: not where the console starts from, not its system data, not NSA's own.
static bool script_writable(const char* path) {
    return !script_under(path, "/luma") && strcasecmp(path, "/boot.firm") != 0 && !script_under(path, "/Nintendo 3DS")
           && !script_under(path, "/nsa") && !script_under(path, "/gm9") && !script_under(path, "/private");
}

// Whether `path` is something the script wrote before step `upTo`.
static bool script_made_before(const script_step* steps, u32 upTo, const char* path) {
    for(u32 i = 0; i < upTo; i++) {
        const script_step* s = &steps[i];
        if(s->type == STEP_DOWNLOAD && strcasecmp(s->b, path) == 0) {
            return true;
        }

        if(s->type == STEP_MOVE && strcasecmp(s->b, path) == 0) {
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

static bool script_parse(json_t* script, script_step* steps, u32* count, u32* downloads) {
    if(!json_is_array(script) || json_array_size(script) == 0 || json_array_size(script) > SCRIPT_STEPS_MAX) {
        return false;
    }

    *count = 0;
    *downloads = 0;
    bool past = false;
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
            if(past || url == NULL || strncmp(url, "https://", 8) != 0 || strlen(url) >= sizeof(s->a)) {
                return false;
            }

            s->type = STEP_DOWNLOAD;
            string_copy(s->a, url, sizeof(s->a));
            if(!script_resolve(script_str(step, "output"), s->b, sizeof(s->b)) || !script_writable(s->b)) {
                return false;
            }

            (*downloads)++;
        } else if(strcmp(type, "extractFile") == 0) {
            const char* input = script_str(step, "input");
            if(input == NULL || strlen(input) >= sizeof(s->b) || !script_resolve(script_str(step, "file"), s->a, sizeof(s->a))
               || !script_resolve(script_str(step, "output"), s->c, sizeof(s->c)) || !script_writable(s->c)
               || !script_made_before(steps, *count, s->a)) {
                return false;
            }

            s->type = STEP_EXTRACT;
            string_copy(s->b, input, sizeof(s->b));
            past = true;
        } else if(strcmp(type, "installCia") == 0) {
            if(!script_resolve(script_str(step, "file"), s->a, sizeof(s->a)) || !script_made_before(steps, *count, s->a)) {
                return false;
            }

            s->type = STEP_INSTALL;
            past = true;
        } else if(strcmp(type, "deleteFile") == 0) {
            if(!script_resolve(script_str(step, "file"), s->a, sizeof(s->a)) || !script_made_before(steps, *count, s->a)) {
                return false;
            }

            s->type = STEP_DELETE;
            past = true;
        } else if(strcmp(type, "move") == 0) {
            if(!script_resolve(script_str(step, "old"), s->a, sizeof(s->a)) || !script_made_before(steps, *count, s->a)
               || !script_resolve(script_str(step, "new"), s->b, sizeof(s->b)) || !script_writable(s->b)) {
                return false;
            }

            s->type = STEP_MOVE;
            past = true;
        } else {
            return false;
        }

        (*count)++;
    }

    return *downloads > 0;
}

bool storescript_supported(json_t* script) {
    static script_step steps[SCRIPT_STEPS_MAX];   // Large: kept off the stack (the catalogue is read on one thread).
    u32 count = 0;
    u32 downloads = 0;
    return script_parse(script, steps, &count, &downloads);
}

static bool script_has_ext(const char* path, const char* ext) {
    size_t len = strlen(path);
    size_t extLen = strlen(ext);
    return len >= extLen && strcasecmp(path + len - extLen, ext) == 0;
}

bool storescript_plain_cia(json_t* script) {
    static script_step steps[SCRIPT_STEPS_MAX];
    u32 count = 0;
    u32 downloads = 0;
    if(!script_parse(script, steps, &count, &downloads) || downloads != 1 || !script_has_ext(steps[0].b, ".cia")) {
        return false;
    }

    for(u32 i = 1; i < count; i++) {
        if((steps[i].type != STEP_INSTALL && steps[i].type != STEP_DELETE) || strcasecmp(steps[i].a, steps[0].b) != 0) {
            return false;
        }
    }

    return true;
}

const char* storescript_first_url(json_t* script) {
    json_t* first = json_array_get(script, 0);
    const char* url = first != NULL ? script_str(first, "file") : NULL;
    return url != NULL ? url : "";
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

static void script_thread(void* arg) {
    script_run* run = (script_run*) arg;
    Result res = 0;

    for(u32 i = run->downloads; i < run->count && R_SUCCEEDED(res); i++) {
        if(run->cancel) {
            res = R_APP_CANCELLED;
            break;
        }

        run->current = i;
        script_step* s = &run->steps[i];
        char from[SCRIPT_PATH_MAX + 8];
        char to[SCRIPT_PATH_MAX + 8];
        if(s->type == STEP_EXTRACT) {
            script_sd(from, sizeof(from), s->a);
            script_sd(to, sizeof(to), s->c);
            run->progress.done = 0;
            run->progress.total = 0;
            u32 extracted = 0;
            res = unarchive_extract(from, s->b, to, &run->progress, &extracted);
            // Nothing matched: the archive is not what the catalogue expects, and what should be installed is not there.
            if(R_SUCCEEDED(res) && extracted == 0) {
                res = R_APP_BAD_DATA;
            }
        } else if(s->type == STEP_MOVE) {
            script_sd(from, sizeof(from), s->a);
            script_sd(to, sizeof(to), s->b);
            remove(to);
            for(char* c = strchr(to + 6, '/'); c != NULL; c = strchr(c + 1, '/')) {
                *c = '\0';
                mkdir(to, 0777);
                *c = '/';
            }

            if(rename(from, to) != 0) {
                res = R_APP_IO_ERROR;
            } else {
                script_note_created(run, s->b);
            }
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
    }

    // On a failure, what the script would have deleted goes anyway (its downloads and what it took out of them).
    if(R_FAILED(res)) {
        for(u32 i = run->downloads; i < run->count; i++) {
            if(run->steps[i].type == STEP_DELETE) {
                script_delete(run, run->steps[i].a);
            }
        }

        run->installs[0] = '\0';
    }

    run->res = res;
    run->finished = true;
}

static void script_progress_update(ui_view* view, void* data, float* progress, char* text) {
    script_run* run = (script_run*) data;

    if(run->finished) {
        ui_pop();
        info_destroy(view);

        if(R_FAILED(run->res)) {
            if(run->res != R_APP_CANCELLED) {
                error_display_res(NULL, NULL, run->res, _("Failed to take the files out of the archive."));
            }

            script_finish(run, false);
            return;
        }

        if(run->installs[0] != '\0') {
            // The install screens take over and say how it went.
            action_install_cia_paths_now(run->installs, run->deleteInstalled);
        } else {
            char message[SCRIPT_PATH_MAX + 64];
            snprintf(message, sizeof(message), _("Saved in %s"), run->last);
            prompt_display_notify("Success", message, COLOR_TEXT, NULL, NULL, NULL);
        }

        script_finish(run, true);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        run->cancel = true;
    }

    u64 total = run->progress.total;
    *progress = total > 0 ? (float) ((double) run->progress.done / (double) total) : 0;

    const script_step* s = &run->steps[run->current];
    const char* path = s->a;
    const char* name = strrchr(path, '/');
    snprintf(text, PROGRESS_TEXT_MAX, "%s\n%s", _("Extracting..."), name != NULL ? name + 1 : path);
}

static void script_downloaded(void* data, u32 index) {
    script_run* run = (script_run*) data;
    run->downloaded++;
    if(index < run->downloads) {
        script_note_created(run, run->steps[index].b);
    }
}

static void script_downloads_done(void* data) {
    script_run* run = (script_run*) data;
    if(run->downloaded < run->downloads) {
        // The download screens said why (or it was cancelled): nothing half done is left.
        for(u32 i = 0; i < run->downloads; i++) {
            script_delete(run, run->steps[i].b);
        }

        script_finish(run, false);
        return;
    }

    // Only downloads (a .3dsx): done.
    if(run->count == run->downloads) {
        char message[SCRIPT_PATH_MAX + 64];
        snprintf(message, sizeof(message), _("Saved in %s"), run->last);
        prompt_display_notify("Success", message, COLOR_TEXT, NULL, NULL, NULL);
        script_finish(run, true);
        return;
    }

    // Decoding never waits: below the UI's priority, so the screen keeps moving, and on a New 3DS on its third core.
    bool isNew = false;
    Thread thread = NULL;
    if(info_display(_("Store"), _("Press B to cancel."), true, run, script_progress_update, NULL) != NULL) {
        if(R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
            thread = threadCreate(script_thread, run, 0x8000, 0x38, 2, true);
        }

        if(thread == NULL) {
            thread = threadCreate(script_thread, run, 0x8000, 0x38, -2, true);
        }
    }

    if(thread == NULL) {
        error_display_res(NULL, NULL, R_APP_THREAD_CREATE_FAILED, _("Failed to take the files out of the archive."));
        for(u32 i = 0; i < run->downloads; i++) {
            script_delete(run, run->steps[i].b);
        }

        // The progress screen, if it opened, closes once `finished` is seen; with no thread it never would.
        run->res = R_APP_CANCELLED;
        run->finished = true;
    }
}

static void script_start_downloads(script_run* run) {
    char* urls = (char*) calloc(1, run->downloads * (DOWNLOAD_URL_MAX + 1) + 1);
    char* paths = (char*) calloc(1, run->downloads * (SCRIPT_PATH_MAX + 1) + 1);
    if(urls == NULL || paths == NULL) {
        free(urls);
        free(paths);
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("Failed to open the Store."));
        script_finish(run, false);
        return;
    }

    for(u32 i = 0; i < run->downloads; i++) {
        strcat(urls, run->steps[i].a);
        strcat(paths, run->steps[i].b);
        if(i + 1 < run->downloads) {
            strcat(urls, "\n");
            strcat(paths, "\n");
        }
    }

    action_download_urls_to(urls, paths, run, script_downloaded, script_downloads_done);
    free(urls);
    free(paths);
}

static void script_question_answered(ui_view* view, void* data, u32 response) {
    script_run* run = (script_run*) data;
    if(response != PROMPT_YES) {
        script_finish(run, false);
        return;
    }

    script_start_downloads(run);
}

void storescript_run(const char* scriptText, const char* question, void* userData, void (*done)(void* userData, bool ok)) {
    script_run* run = (script_run*) calloc(1, sizeof(script_run));
    json_error_t error;
    json_t* script = scriptText != NULL ? json_loads(scriptText, 0, &error) : NULL;
    if(run == NULL || !script_parse(script, run->steps, &run->count, &run->downloads)) {
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
