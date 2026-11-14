#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <3ds.h>
#include <jansson.h>

#include "storescript.h"
#include "dlqueue.h"
#include "ftpserver.h"
#include "action/action.h"
#include "../core/core.h"
#include "../core/minire.h"
#include "../core/speedlog.h"
#include "../core/unarchive.h"

#define SCRIPT_STEPS_MAX 64
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
    STEP_SKIP,       // Skips `count` steps (after a prompt's Yes, the steps for its No).
    STEP_COPY,       // a: from, b: to.
    STEP_EXIT,       // Ends the script there, as if it had reached its end.
    STEP_NONE        // A step NSA takes as done without doing anything (bootTitle: the title is not started).
} script_step_type;

typedef struct {
    u8 type;
    bool prerelease;   // STEP_RELEASE: the newest release, even a pre-release.
    bool sensitive;    // It writes where the user is warned first (script_sensitive).
    bool insecure;     // STEP_DOWNLOAD: over plain http (the user is warned first).
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

    bool sensitive;     // A step writes where the user is warned first; after the warning an extraction may too.
    bool insecure;      // A download is over plain http.
    bool warnedSensitive;
    bool warnedInsecure;

    // In the download queue (issue #41): asked beforehand, then run with no screen and no question.
    char name[96];
    bool queued;
    u8 answers[SCRIPT_STEPS_MAX];   // Each prompt's and rmdir's answer: 1 yes, 2 no.
    u32 askPc;                      // Where the questions asked beforehand are.
    void* dl;                       // The download under way (installurl.c).
    bool releasing;                 // The release's file is being looked for (a thread).
    bool filesRunning;              // The file steps run (a thread).
    bool installing;                // The CIAs install (a thread).
    volatile bool installDone;
    bool installOk;
    bool stopRequested;
    bool stopPause;
    bool userCancelled;
    void (*queueDone)(void* item, u32 ok, u32 failed, bool paused, bool cancelled, const char* error);
    void* queueItem;
    char error[160];
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

// Where an extraction goes: a folder as above, or the card's root itself ("/" or "sdmc:/", as the packs made "for SD Card
// root" ask). What comes out is checked file by file (script_extract_allowed).
static bool script_resolve_folder(const char* in, char* out, size_t size) {
    if(in != NULL && (strcmp(in, "/") == 0 || strcasecmp(in, "sdmc:/") == 0) && size > 1) {
        string_copy(out, "/", size);
        return true;
    }

    return script_resolve(in, out, size);
}

static bool script_under(const char* path, const char* folder) {
    size_t len = strlen(folder);
    return strncasecmp(path, folder, len) == 0 && (path[len] == '\0' || path[len] == '/');
}

// Where a script may never write: the console's own data (/Nintendo 3DS) and NSA's (/nsa).
static bool script_forbidden(const char* path) {
    return script_under(path, "/Nintendo 3DS") || script_under(path, "/nsa");
}

// Where a script may write only once the user has been warned (script_warn): what the console starts from (boot.firm,
// the payloads: every .firm), Luma's folders (its system modules and patches), GodMode9's and /private. Of /luma, a
// game's own folder in /luma/titles (its mods, which Luma loads into that game alone) and /luma/plugins (the .3gx plugins
// Luma's plugin loader offers, such as Vapecord's) need no warning.
static bool script_sensitive(const char* path) {
    if(script_under(path, "/luma")) {
        return !((script_under(path, "/luma/titles") && strlen(path) > strlen("/luma/titles/")) || script_under(path, "/luma/plugins"));
    }

    size_t len = strlen(path);
    return (len >= 5 && strcasecmp(path + len - 5, ".firm") == 0) || script_under(path, "/gm9") || script_under(path, "/private");
}

// A folder a script may delete with everything in it, as in Universal-Updater (the user is asked first, and warned in a
// protected place): any but where a script may never write.
static bool script_removable(const char* path) {
    return !script_forbidden(path);
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
            if(url == NULL || (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) || strlen(url) >= sizeof(s->a)) {
                return false;
            }

            s->type = STEP_DOWNLOAD;
            s->insecure = strncmp(url, "http://", 7) == 0;
            string_copy(s->a, url, sizeof(s->a));
            if(!script_resolve(script_str(step, "output"), s->b, sizeof(s->b)) || script_forbidden(s->b)) {
                return false;
            }

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
            if(!script_resolve(script_str(step, "output"), s->b, sizeof(s->b)) || script_forbidden(s->b)) {
                return false;
            }

        } else if(strcmp(type, "extractFile") == 0) {
            const char* input = script_str(step, "input");
            if(input == NULL || strlen(input) >= sizeof(s->b) || !script_resolve(script_str(step, "file"), s->a, sizeof(s->a))
               || !script_resolve_folder(script_str(step, "output"), s->c, sizeof(s->c)) || script_forbidden(s->c)) {
                return false;
            }

            s->type = STEP_EXTRACT;
            string_copy(s->b, input, sizeof(s->b));
        } else if(strcmp(type, "installCia") == 0) {
            if(!script_resolve(script_str(step, "file"), s->a, sizeof(s->a))) {
                return false;
            }

            s->type = STEP_INSTALL;
        } else if(strcmp(type, "deleteFile") == 0) {
            if(!script_resolve(script_str(step, "file"), s->a, sizeof(s->a)) || script_forbidden(s->a)) {
                return false;
            }

            s->type = STEP_DELETE;
        } else if(strcmp(type, "move") == 0) {
            if(!script_resolve(script_str(step, "old"), s->a, sizeof(s->a)) || script_forbidden(s->a)
               || !script_resolve(script_str(step, "new"), s->b, sizeof(s->b)) || script_forbidden(s->b)) {
                return false;
            }

            s->type = STEP_MOVE;
        } else if(strcmp(type, "mkdir") == 0) {
            if(!script_resolve(script_str(step, "directory"), s->b, sizeof(s->b)) || script_forbidden(s->b)) {
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
        } else if(strcmp(type, "copy") == 0) {
            // Universal-Updater's names, or move's.
            const char* from = script_str(step, "source") != NULL ? script_str(step, "source") : script_str(step, "old");
            const char* to = script_str(step, "destination") != NULL ? script_str(step, "destination") : script_str(step, "new");
            if(!script_resolve(from, s->a, sizeof(s->a)) || !script_resolve(to, s->b, sizeof(s->b)) || script_forbidden(s->b)) {
                return false;
            }

            s->type = STEP_COPY;
        } else if(strcmp(type, "exit") == 0) {
            s->type = STEP_EXIT;
        } else if(strcmp(type, "bootTitle") == 0) {
            s->type = STEP_NONE;
        } else {
            return false;
        }

        if(s->type == STEP_EXTRACT) {
            s->sensitive = script_sensitive(s->c);
        } else if(s->type == STEP_DELETE) {
            s->sensitive = script_sensitive(s->a);
        } else if(s->type == STEP_MOVE) {
            s->sensitive = script_sensitive(s->a) || script_sensitive(s->b);
        } else if(s->type == STEP_DOWNLOAD || s->type == STEP_RELEASE || s->type == STEP_COPY || s->type == STEP_MKDIR
                  || s->type == STEP_RMDIR) {
            s->sensitive = script_sensitive(s->b);
        }

        (*count)++;
    }

    return true;
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
    // One that needs a warning (http, a protected place) goes through storescript_run, which gives it.
    if(!script_parse(script, script_scratch, &count) || script_scratch[0].type != STEP_DOWNLOAD || !script_has_ext(script_scratch[0].b, ".cia")
       || script_scratch[0].insecure || script_scratch[0].sensitive) {
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

bool storescript_resolve_path(const char* in, char* out, size_t size) {
    return script_resolve(in, out, size);
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

// Each file an archive would put on the card goes only where a script may write ("sdmc:/Nintendo 3DS/..." from a pack
// extracted to the root is refused, and the extraction with it); where script_sensitive says, only in a run the user was
// warned about.
static bool script_extract_allowed(void* userData, const char* sdPath) {
    const char* path = strncmp(sdPath, "sdmc:", 5) == 0 ? sdPath + 5 : sdPath;
    return !script_forbidden(path) && (((script_run*) userData)->sensitive || !script_sensitive(path));
}

static void script_finish(script_run* run, bool ok) {
    if(run->queued && run->queueDone != NULL) {
        if(!ok && run->error[0] == '\0' && !run->userCancelled) {
            string_copy(run->error, _("A step of the script failed."), sizeof(run->error));
        }

        run->queueDone(run->queueItem, ok ? 1 : 0, ok ? 0 : 1, false, run->userCancelled, run->error);
    }

    if(run->done != NULL) {
        run->done(run->userData, ok);
    }

    free(run);
}

// A queued run that keeps an error to show on its item (or, on screen, says it as before).
static void script_fail(script_run* run, Result res, const char* what) {
    if(run->queued) {
        snprintf(run->error, sizeof(run->error), "%s (%08lX)", what, (u32) res);
    } else {
        error_display_res(NULL, NULL, res, what);
    }
}

static bool script_installing(script_run* run, const char* path) {
    for(u32 i = 0; i < run->count; i++) {
        if(run->steps[i].type == STEP_INSTALL && strcasecmp(run->steps[i].a, path) == 0) {
            return true;
        }
    }

    return false;
}

// A file the script deletes (`any`), or one it leaves behind when it stops half way (only what this run wrote).
static void script_delete(script_run* run, const char* path, bool any) {
    if(!any && !script_created(run, path)) {
        return;
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
            script_delete(run, run->steps[i].a, false);
        }
    }

    run->installs[0] = '\0';
    script_finish(run, false);
}

static char script_kept[SCRIPT_PATH_MAX];

const char* storescript_kept_file(void) {
    return script_kept;
}

static bool script_start_thread(script_run* run, void (*entry)(void* arg));

// In the queue: the CIAs the script installs, one after the other, as SITE INSTALL does (no screen, no question); those
// the script deletes go once they are in.
static void script_install_thread(void* arg) {
    script_run* run = (script_run*) arg;
    run->installOk = true;
    const char* line = run->installs;
    while(*line != '\0') {
        const char* end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t) (end - line) : strlen(line);
        char path[SCRIPT_PATH_MAX];
        string_copy(path, line, len + 1 < sizeof(path) ? len + 1 : sizeof(path));
        char error[128] = "";
        if(!ftpserver_install(path, error, sizeof(error))) {
            run->installOk = false;
            snprintf(run->error, sizeof(run->error), "%s: %s", path, error);
        } else if(run->deleteInstalled) {
            char sd[SCRIPT_PATH_MAX + 8];
            script_sd(sd, sizeof(sd), path);
            remove(sd);
        }

        line += len;
        if(*line == '\n') {
            line++;
        }
    }

    run->installDone = true;
}

// The end: the CIAs install (their screens say how it went), or it says where the files went.
static void script_end(script_run* run) {
    // What it leaves, for the Store to look for later.
    script_kept[0] = '\0';
    for(u32 i = run->createdCount; i > 0 && script_kept[0] == '\0'; i--) {
        const char* path = run->created[i - 1];
        char sd[SCRIPT_PATH_MAX + 8];
        script_sd(sd, sizeof(sd), path);
        struct stat st;
        if(!script_has_ext(path, ".cia") && stat(sd, &st) == 0) {
            string_copy(script_kept, path, sizeof(script_kept));
        }
    }

    if(run->queued) {
        if(run->installs[0] != '\0') {
            run->installing = true;
            run->installDone = false;
            if(!script_start_thread(run, script_install_thread)) {
                run->installing = false;
                script_fail(run, R_APP_THREAD_CREATE_FAILED, _("Failed to install the CIAs."));
                script_finish(run, false);
            }

            return;
        }

        script_finish(run, true);
        return;
    }

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
    return type == STEP_EXTRACT || type == STEP_INSTALL || type == STEP_DELETE || type == STEP_MOVE || type == STEP_MKDIR
           || type == STEP_COPY;
}

static void script_make_dirs(char* sdPath) {
    for(char* c = strchr(sdPath + 6, '/'); c != NULL; c = strchr(c + 1, '/')) {
        *c = '\0';
        mkdir(sdPath, 0777);
        *c = '/';
    }
}

// A file copied whole; false if it could not be read or written.
static bool script_copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if(in == NULL) {
        return false;
    }

    FILE* out = fopen(to, "wb");
    if(out == NULL) {
        fclose(in);
        return false;
    }

    static u8 buffer[64 * 1024];
    bool ok = true;
    size_t got;
    while(ok && (got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        ok = fwrite(buffer, 1, got, out) == got;
    }

    ok = ok && !ferror(in);
    fclose(in);
    ok = fclose(out) == 0 && ok;
    if(!ok) {
        remove(to);
    }

    return ok;
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
            // Nothing matched is not an error, as in Universal-Updater: Universal-DB has scripts with an extraction that finds
            // nothing (Vapecord-ACHHD-Plugin's "^VapecordACHHD", its zip holding only luma/plugins). What a later step needs
            // and did not come out fails that step (installs and extractions check that this run wrote their file).
            (void) extracted;
        } else if(s->type == STEP_MOVE) {
            script_sd(from, sizeof(from), s->a);
            script_sd(to, sizeof(to), s->b);
            remove(to);
            script_make_dirs(to);
            if(rename(from, to) != 0) {
                res = R_APP_IO_ERROR;
            } else {
                script_note_created(run, s->b);
            }
        } else if(s->type == STEP_COPY) {
            script_sd(from, sizeof(from), s->a);
            script_sd(to, sizeof(to), s->b);
            script_make_dirs(to);
            if(!script_copy_file(from, to)) {
                res = R_APP_IO_ERROR;
            } else {
                script_note_created(run, s->b);
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
                script_delete(run, s->a, true);
            }
        }

        run->pc++;
    }

    run->res = res;
    run->finished = true;
}

static void script_next(script_run* run);

// The file steps done (or failed): on with the script.
static void script_files_after(script_run* run) {
    if(R_FAILED(run->res)) {
        if(run->res != R_APP_CANCELLED) {
            script_fail(run, run->res, _("Failed to take the files out of the archive."));
        }

        script_abandon(run);
        return;
    }

    script_next(run);
}

static void script_files_update(ui_view* view, void* data, float* progress, char* text) {
    script_run* run = (script_run*) data;

    if(run->finished) {
        ui_pop();
        info_destroy(view);
        script_files_after(run);
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
    if(run->queued) {
        run->filesRunning = true;
        if(!script_start_thread(run, script_files_thread)) {
            run->filesRunning = false;
            script_fail(run, R_APP_THREAD_CREATE_FAILED, _("Failed to take the files out of the archive."));
            script_abandon(run);
        }

        return;
    }

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

static void script_stopped(script_run* run);

static void script_download_done(void* data) {
    script_run* run = (script_run*) data;
    run->dl = NULL;
    // Stopped on purpose (the queue pauses or cancels it): the step starts again on resume.
    if(run->queued && run->stopRequested) {
        script_stopped(run);
        return;
    }

    if(run->queued && !run->downloaded) {
        string_copy(run->error, action_install_url_last_error(), sizeof(run->error));
    }

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
    if(run->queued) {
        run->dl = action_download_urls_to_queued(url, run->steps[run->pc].b, run, script_downloaded, script_download_done);
        return;
    }

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

// The release's file found (or not): downloaded next.
static void script_release_after(script_run* run) {
    if(R_FAILED(run->res)) {
        script_fail(run, run->res, _("Failed to find the release's file."));
        script_abandon(run);
        return;
    }

    script_download(run, run->releaseUrl);
}

static void script_release_update(ui_view* view, void* data, float* progress, char* text) {
    script_run* run = (script_run*) data;
    snprintf(text, PROGRESS_TEXT_MAX, "%s", _("Looking for the latest release..."));
    if(!run->finished) {
        return;
    }

    ui_pop();
    info_destroy(view);
    script_release_after(run);
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
        // The queue asked it to stop: here, between two steps.
        if(run->queued && run->stopRequested) {
            script_stopped(run);
            return;
        }

        script_step* s = &run->steps[run->pc];
        if(run->queued && s->type == STEP_PROMPT) {
            // Answered when it was added.
            if(run->answers[run->pc] == 1) {
                run->pc++;
            } else if(s->count > 0) {
                run->pc += 1 + s->count;
            } else {
                script_abandon(run);
                return;
            }

            continue;
        }

        if(run->queued && s->type == STEP_RMDIR) {
            char sd[SCRIPT_PATH_MAX + 8];
            script_sd(sd, sizeof(sd), s->b);
            struct stat st;
            if(run->answers[run->pc] == 1 && stat(sd, &st) == 0) {
                FS_Archive archive = 0;
                if(R_SUCCEEDED(fs_open_archive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
                    fs_delete_dir_recursively(archive, s->b);
                    fs_close_archive(archive);
                }
            }

            run->pc++;
            continue;
        }

        switch(s->type) {
            case STEP_SKIP:
                run->pc += 1 + s->count;
                continue;
            case STEP_EXIT:
                run->pc = run->count;
                continue;
            case STEP_NONE:
                run->pc++;
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
                if(run->queued) {
                    run->releasing = true;
                    if(!script_start_thread(run, script_release_thread)) {
                        run->releasing = false;
                        script_fail(run, R_APP_THREAD_CREATE_FAILED, _("Failed to find the release's file."));
                        script_abandon(run);
                    }

                    return;
                }

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

static void script_warnings(script_run* run);

// ---- the download queue: what the script will ask, asked now ----

static void script_ask_next(script_run* run);

static void script_ask_prompt_answered(ui_view* view, void* data, u32 response) {
    script_run* run = (script_run*) data;
    const script_step* s = &run->steps[run->askPc];
    run->answers[run->askPc] = response == PROMPT_YES ? 1 : 2;
    if(response == PROMPT_YES) {
        run->askPc++;
    } else if(s->count > 0) {
        run->askPc += 1 + s->count;
    } else {
        // A No that ends the script: nothing goes to the queue.
        script_finish(run, false);
        return;
    }

    script_ask_next(run);
}

static void script_ask_rmdir_answered(ui_view* view, void* data, u32 response) {
    script_run* run = (script_run*) data;
    run->answers[run->askPc] = response == PROMPT_YES ? 1 : 2;
    run->askPc++;
    script_ask_next(run);
}

// The script's path, as its answers make it: each question on it asked now, in order; then it waits in the queue.
static void script_ask_next(script_run* run) {
    while(run->askPc < run->count) {
        const script_step* s = &run->steps[run->askPc];
        if(s->type == STEP_SKIP) {
            run->askPc += 1 + s->count;
        } else if(s->type == STEP_EXIT) {
            run->askPc = run->count;
        } else if(s->type == STEP_PROMPT) {
            prompt_display_yes_no("Confirmation", s->a, COLOR_TEXT, run, NULL, script_ask_prompt_answered);
            return;
        } else if(s->type == STEP_RMDIR) {
            static char question[SCRIPT_PATH_MAX + 160];
            snprintf(question, sizeof(question), _("When it runs, delete this folder and everything in it (if it is there)?\n\n%s"), s->b);
            prompt_display_warning("Confirmation", question, COLOR_TEXT, run, NULL, script_ask_rmdir_answered);
            return;
        } else {
            run->askPc++;
        }
    }

    run->pc = 0;
    if(!dlqueue_add_script(run->name, run)) {
        script_finish(run, false);
    }
}

static void script_warning_answered(ui_view* view, void* data, u32 response) {
    script_run* run = (script_run*) data;
    if(response != PROMPT_YES) {
        script_finish(run, false);
        return;
    }

    script_warnings(run);
}

// The protected place a step touches.
static const char* script_step_target(const script_step* s) {
    if(s->type == STEP_EXTRACT) {
        return s->c;
    }

    if(s->type == STEP_DELETE || (s->type == STEP_MOVE && !script_sensitive(s->b))) {
        return s->a;
    }

    return s->b;
}

// Before a script that writes where the console starts from or where Luma keeps its system files: where (a few of the
// places, once each), and what a bad file there can do.
static void script_warn(script_run* run) {
    char list[640] = "";
    size_t used = 0;
    u32 more = 0;
    for(u32 i = 0; i < run->count; i++) {
        const script_step* s = &run->steps[i];
        if(!s->sensitive) {
            continue;
        }

        const char* path = script_step_target(s);
        bool seen = false;
        for(u32 j = 0; j < i && !seen; j++) {
            seen = run->steps[j].sensitive && strcasecmp(script_step_target(&run->steps[j]), path) == 0;
        }

        if(seen) {
            continue;
        }

        if(used < 400 && used + strlen(path) + 2 < sizeof(list)) {
            used += snprintf(list + used, sizeof(list) - used, "%s%s", used > 0 ? "\n" : "", path);
        } else {
            more++;
        }
    }

    char extra[64] = "";
    if(more > 0) {
        extra[0] = '\n';
        snprintf(extra + 1, sizeof(extra) - 1, _("and %lu more"), (unsigned long) more);
    }

    static char text[1024];
    snprintf(text, sizeof(text),
             _("Careful: this writes to protected places on the SD card, where the console starts from or where Luma keeps its system files:\n\n%s%s\n\nA wrong or broken file there can stop the console from starting. Go on only if you trust it.\n\nContinue?"),
             list, extra);
    prompt_display_warning("Confirmation", text, COLOR_TEXT, run, NULL, script_warning_answered);
}

// Before a script that downloads over plain http: from where (each server once).
static void script_warn_http(script_run* run) {
    char list[512] = "";
    size_t used = 0;
    for(u32 i = 0; i < run->count; i++) {
        const script_step* s = &run->steps[i];
        if(!s->insecure) {
            continue;
        }

        // "http://host/path" as "host".
        char host[128];
        const char* start = s->a + 7;
        size_t len = strcspn(start, "/?#");
        if(len >= sizeof(host)) {
            len = sizeof(host) - 1;
        }

        memcpy(host, start, len);
        host[len] = '\0';
        bool seen = false;
        for(const char* line = list; *line != '\0' && !seen; line = strchr(line, '\n') != NULL ? strchr(line, '\n') + 1 : line + strlen(line)) {
            seen = strncasecmp(line, host, len) == 0 && (line[len] == '\n' || line[len] == '\0');
        }

        if(!seen && used + len + 2 < sizeof(list)) {
            used += snprintf(list + used, sizeof(list) - used, "%s%s", used > 0 ? "\n" : "", host);
        }
    }

    static char text[1024];
    snprintf(text, sizeof(text),
             _("This downloads over plain http, without encryption, from:\n\n%s\n\nSomeone on the same network could change the file on its way. Go on only if you trust the network.\n\nContinue?"),
             list);
    prompt_display_warning("Confirmation", text, COLOR_TEXT, run, NULL, script_warning_answered);
}

// The warnings a script needs, one after the other, then its steps.
static void script_warnings(script_run* run) {
    if(run->sensitive && !run->warnedSensitive) {
        run->warnedSensitive = true;
        script_warn(run);
    } else if(run->insecure && !run->warnedInsecure) {
        run->warnedInsecure = true;
        script_warn_http(run);
    } else if(run->queued) {
        script_ask_next(run);
    } else {
        script_next(run);
    }
}

static void script_question_answered(ui_view* view, void* data, u32 response) {
    script_run* run = (script_run*) data;
    // 0: now; 1: the download queue; else: no.
    if(response != 0 && response != 1) {
        script_finish(run, false);
        return;
    }

    run->queued = response == 1;
    script_warnings(run);
}

void storescript_run(const char* scriptText, const char* question, const char* name, void* userData, void (*done)(void* userData, bool ok)) {
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
    string_copy(run->name, name != NULL ? name : "", sizeof(run->name));
    for(u32 i = 0; i < run->count; i++) {
        run->sensitive |= run->steps[i].sensitive;
        run->insecure |= run->steps[i].insecure;
    }

    run->progress.cancel = &run->cancel;
    run->progress.userData = run;
    run->progress.created = script_extracted;
    run->progress.allowed = script_extract_allowed;

    static const char* options[] = {"Yes", "To the queue", "No"};   // A button: short.
    static u32 buttons[] = {KEY_A, KEY_X, KEY_B};
    static const char* translated[3];
    for(u32 i = 0; i < 3; i++) {
        translated[i] = _(options[i]);
    }

    prompt_display_multi_choice("Confirmation", question, COLOR_TEXT, translated, buttons, 3, run, NULL, script_question_answered);
}

// ---- the download queue's side (dlqueue.c) ----

// Stopped between two steps: paused (it waits where it is) or cancelled (what it wrote for nothing goes).
static void script_stopped(script_run* run) {
    run->stopRequested = false;
    if(run->stopPause) {
        if(run->queueDone != NULL) {
            run->queueDone(run->queueItem, 0, 0, true, false, "");
        }

        return;
    }

    run->userCancelled = true;
    script_abandon(run);
}

bool storescript_queued_start(void* data, void (*done)(void* item, u32 ok, u32 failed, bool paused, bool cancelled, const char* error), void* item) {
    script_run* run = (script_run*) data;
    run->queueDone = done;
    run->queueItem = item;
    run->stopRequested = false;
    run->error[0] = '\0';
    script_next(run);
    return true;
}

void storescript_queued_poll(void* data) {
    script_run* run = (script_run*) data;
    // Each of these may end the run (and free it): nothing after.
    if(run->dl != NULL) {
        action_install_url_queued_poll(run->dl);
    } else if(run->releasing && run->finished) {
        run->releasing = false;
        script_release_after(run);
    } else if(run->filesRunning && run->finished) {
        run->filesRunning = false;
        script_files_after(run);
    } else if(run->installing && run->installDone) {
        run->installing = false;
        script_finish(run, run->installOk);
    }
}

void storescript_queued_stop(void* data, bool pause) {
    script_run* run = (script_run*) data;
    run->stopRequested = true;
    run->stopPause = pause;
    if(run->dl != NULL) {
        action_install_url_queued_stop(run->dl, false);
    }
}

void storescript_queued_discard(void* data) {
    script_run* run = (script_run*) data;
    run->queueDone = NULL;
    run->userCancelled = true;
    script_abandon(run);
}

void storescript_queued_open(void* data) {
    script_run* run = (script_run*) data;
    if(run->dl != NULL) {
        action_install_url_queued_open(run->dl);
    }
}

bool storescript_queued_on_screen(void* data) {
    script_run* run = (script_run*) data;
    return run->dl != NULL && action_install_url_queued_on_screen(run->dl);
}

void storescript_queued_progress(void* data, u64* done, u64* total, char* name, size_t size) {
    script_run* run = (script_run*) data;
    if(run->dl != NULL) {
        action_install_url_queued_progress(run->dl, done, total, name, size);
        return;
    }

    *done = run->pc;
    *total = run->count;
    snprintf(name, size, run->installing ? _("Installing...") : _("Step %lu of %lu"), run->pc + 1, run->count);
}
