// Export and import of a title's save data and extra data as plain folders on the SD card:
//
//   sdmc:/nsa/saves/<TITLEID>/<YYYYMMDD-HHMMSS>[-before-import]/{manifest.txt, save/ | ext/}
//
// Importing replaces the whole archive, and first exports what is there (the "-before-import" folder).

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

#define MAX_FILES 1024
#define SAVES_DIR APP_DATA_DIR "saves/"
#define BACKUP_SUFFIX "-before-import"
#define PICK_MAX 16

static const char* save_kind_dir(save_kind kind) {
    return kind == SAVE_KIND_EXT ? "ext" : "save";
}

// ---- tree copy: every file under one root of an archive into another root of another archive ----

typedef struct {
    title_info* title;

    FS_Archive srcArchive;
    FS_Archive dstArchive;
    char srcRoot[FILE_PATH_MAX];  // With the trailing slash.
    char dstRoot[FILE_PATH_MAX];
    bool commit;                  // The destination is a save archive.

    linked_list contents;
    data_op_data op;

    void (*done)(void* ctx, Result res);
    void* ctx;
} tree_copy;

static void tree_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    task_draw_title_info(view, ((tree_copy*) data)->title, x1, y1, x2, y2);
}

static void tree_dst_path(tree_copy* job, u32 index, char* out) {
    file_info* src = (file_info*) ((list_item*) linked_list_get(&job->contents, index))->data;
    snprintf(out, FILE_PATH_MAX, "%s%s", job->dstRoot, src->path + strlen(job->srcRoot));
}

static Result tree_is_src_directory(void* data, u32 index, bool* isDirectory) {
    tree_copy* job = (tree_copy*) data;
    *isDirectory = (bool) (((file_info*) ((list_item*) linked_list_get(&job->contents, index))->data)->attributes & FS_ATTRIBUTE_DIRECTORY);
    return 0;
}

static Result tree_make_dst_directory(void* data, u32 index) {
    tree_copy* job = (tree_copy*) data;

    char path[FILE_PATH_MAX];
    tree_dst_path(job, index, path);

    return fs_ensure_dir(job->dstArchive, path);
}

static Result tree_open_src(void* data, u32 index, u32* handle) {
    tree_copy* job = (tree_copy*) data;

    Result res = 0;
    FS_Path* fsPath = fs_make_path_utf8(((file_info*) ((list_item*) linked_list_get(&job->contents, index))->data)->path);
    if(fsPath != NULL) {
        res = FSUSER_OpenFile(handle, job->srcArchive, *fsPath, FS_OPEN_READ, 0);
        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static Result tree_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    return FSFILE_Close(handle);
}

static Result tree_get_src_size(void* data, u32 handle, u64* size) {
    return FSFILE_GetSize(handle, size);
}

static Result tree_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    return FSFILE_Read(handle, bytesRead, offset, buffer, size);
}

static Result tree_open_dst(void* data, u32 index, void* initialReadBlock, u64 size, u32* handle) {
    tree_copy* job = (tree_copy*) data;

    char path[FILE_PATH_MAX];
    tree_dst_path(job, index, path);

    Result res = 0;
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath != NULL) {
        // A file that is already there goes first (a save archive has fixed-size files).
        FSUSER_DeleteFile(job->dstArchive, *fsPath);
        if(R_SUCCEEDED(res = FSUSER_CreateFile(job->dstArchive, *fsPath, 0, size))) {
            res = FSUSER_OpenFile(handle, job->dstArchive, *fsPath, FS_OPEN_WRITE, 0);
        }

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static Result tree_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    return FSFILE_Close(handle);
}

static Result tree_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

static Result tree_suspend(void* data, u32 index) {
    return 0;
}

static Result tree_restore(void* data, u32 index) {
    return 0;
}

static bool tree_error(void* data, u32 index, Result res, ui_view** errorView) {
    *errorView = error_display_res(data, tree_draw_top, res, "Failed to copy the files.");
    return true;
}

// The end of a job, whatever came of it: commit, release, hand the result on.
static void tree_finish(tree_copy* job, Result res) {
    if(R_SUCCEEDED(res) && job->commit) {
        res = FSUSER_ControlArchive(job->dstArchive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
        if(R_FAILED(res)) {
            error_display_res(job->title, tree_draw_top, res, "Failed to commit the changes.");
        }
    }

    fs_close_archive(job->srcArchive);
    fs_close_archive(job->dstArchive);

    task_clear_files(&job->contents);
    linked_list_destroy(&job->contents);

    void (*done)(void*, Result) = job->done;
    void* ctx = job->ctx;
    free(job);

    done(ctx, res);
}

static void tree_copy_update(ui_view* view, void* data, float* progress, char* text) {
    tree_copy* job = (tree_copy*) data;

    if(job->op.finished) {
        ui_pop();
        info_destroy(view);

        tree_finish(job, job->op.result);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        svcSignalEvent(job->op.cancelEvent);
    }

    *progress = job->op.currTotal != 0 ? (float) ((double) job->op.currProcessed / (double) job->op.currTotal) : 0;
    snprintf(text, PROGRESS_TEXT_MAX, _("%lu / %lu\n%.2f %s / %.2f %s\n%.2f %s/s, ETA %s"), job->op.processed, job->op.total,
             ui_get_display_size(job->op.currProcessed),
             ui_get_display_size_units(job->op.currProcessed),
             ui_get_display_size(job->op.currTotal),
             ui_get_display_size_units(job->op.currTotal),
             ui_get_display_size(job->op.bytesPerSecond),
             ui_get_display_size_units(job->op.bytesPerSecond),
             ui_get_display_eta(job->op.estimatedRemainingSeconds));
}

typedef struct {
    tree_copy* job;
    populate_files_data pop;
} tree_loading;

static void tree_loading_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    tree_draw_top(view, ((tree_loading*) data)->job, x1, y1, x2, y2);
}

static void tree_loading_update(ui_view* view, void* data, float* progress, char* text) {
    tree_loading* loading = (tree_loading*) data;

    if(loading->pop.finished) {
        ui_pop();
        info_destroy(view);

        tree_copy* job = loading->job;
        Result res = loading->pop.result;
        free(loading);

        u32 total = linked_list_size(&job->contents);
        if(R_SUCCEEDED(res) && total > 0) {
            job->op.total = total;
            job->op.processed = total;

            res = task_data_op(&job->op);
            if(R_SUCCEEDED(res)) {
                info_display("Copying", "Press B to cancel.", true, job, tree_copy_update, tree_draw_top);
                return;
            }

            error_display_res(job->title, tree_draw_top, res, "Failed to start the copy.");
        } else if(R_FAILED(res)) {
            error_display_res(job->title, tree_draw_top, res, "Failed to list the files.");
        }

        // Nothing to copy (an empty archive) is a success of its own.
        tree_finish(job, res);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        svcSignalEvent(loading->pop.cancelEvent);
    }

    snprintf(text, PROGRESS_TEXT_MAX, "%s", _("Listing the files..."));
}

// Takes over the archives (closed at the end) and `job`; `done` is called once, with the result.
static void tree_copy_start(tree_copy* job) {
    linked_list_init(&job->contents);

    job->op.data = job;
    job->op.op = DATAOP_COPY;
    job->op.bufferSize = 256 * 1024;
    job->op.copyEmpty = true;
    job->op.noRetry = true;
    job->op.isSrcDirectory = tree_is_src_directory;
    job->op.makeDstDirectory = tree_make_dst_directory;
    job->op.openSrc = tree_open_src;
    job->op.closeSrc = tree_close_src;
    job->op.getSrcSize = tree_get_src_size;
    job->op.readSrc = tree_read_src;
    job->op.openDst = tree_open_dst;
    job->op.closeDst = tree_close_dst;
    job->op.writeDst = tree_write_dst;
    job->op.suspend = tree_suspend;
    job->op.restore = tree_restore;
    job->op.error = tree_error;
    job->op.finished = true;

    tree_loading* loading = (tree_loading*) calloc(1, sizeof(tree_loading));
    if(loading == NULL) {
        error_display(NULL, NULL, "Failed to allocate the copy.");
        tree_finish(job, R_APP_OUT_OF_MEMORY);
        return;
    }

    loading->job = job;
    loading->pop.items = &job->contents;
    loading->pop.archive = job->srcArchive;
    string_copy(loading->pop.path, job->srcRoot, FILE_PATH_MAX);
    loading->pop.recursive = true;
    loading->pop.includeBase = false;
    loading->pop.meta = false;

    Result res = task_populate_files(&loading->pop);
    if(R_FAILED(res)) {
        error_display_res(job->title, tree_draw_top, res, "Failed to list the files.");
        free(loading);
        tree_finish(job, res);
        return;
    }

    info_display("Loading", "Press B to cancel.", false, loading, tree_loading_update, tree_loading_draw_top);
}

// ---- helpers ----

// Creates `path` (ending in a slash) and its parents on the SD card.
static Result save_make_dirs(FS_Archive sd, const char* path) {
    char partial[FILE_PATH_MAX];
    string_copy(partial, path, sizeof(partial));

    for(char* slash = strchr(partial + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        Result res = fs_ensure_dir(sd, partial);
        *slash = '/';
        if(R_FAILED(res)) {
            return res;
        }
    }

    return 0;
}

static Result save_open_sd(FS_Archive* sd) {
    return fs_open_archive(sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
}

// The archive with the data: the save, or the extra data of `extId`.
static Result save_open_target(FS_Archive* archive, title_info* title, save_kind kind, u32 extId) {
    if(kind == SAVE_KIND_EXT) {
        u32 path[3] = {MEDIATYPE_SD, extId, 0};
        return fs_open_archive(archive, ARCHIVE_EXTDATA, fs_make_path_binary(path, sizeof(path)));
    }

    u32 path[3] = {title->mediaType, (u32) (title->titleId & 0xFFFFFFFF), (u32) ((title->titleId >> 32) & 0xFFFFFFFF)};
    return fs_open_archive(archive, ARCHIVE_USER_SAVEDATA, fs_make_path_binary(path, sizeof(path)));
}

static void save_timestamp(char* out, size_t size) {
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    if(tm == NULL || strftime(out, size, "%Y%m%d-%H%M%S", tm) == 0) {
        snprintf(out, size, "%llu", (unsigned long long) osGetTime());
    }
}

static void save_write_manifest(const char* dir, title_info* title, const char* name, save_kind kind, const char* stamp) {
    char path[FILE_PATH_MAX];
    snprintf(path, sizeof(path), "sdmc:%smanifest.txt", dir);

    FILE* f = fopen(path, "wb");
    if(f != NULL) {
        fprintf(f, "kind=%s\ntitleid=%016llX\nversion=%u\nname=%s\ndate=%s\n", save_kind_dir(kind),
                (unsigned long long) title->titleId, (unsigned) title->version, name, stamp);
        fclose(f);
    }
}

// The value of `key` in a folder's manifest, or an empty string.
static void save_read_manifest(const char* dir, const char* key, char* out, size_t size) {
    out[0] = '\0';

    char path[FILE_PATH_MAX];
    snprintf(path, sizeof(path), "sdmc:%smanifest.txt", dir);

    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        return;
    }

    size_t keyLen = strlen(key);
    char line[FILE_NAME_MAX + 16];
    while(fgets(line, sizeof(line), f) != NULL) {
        if(strncmp(line, key, keyLen) == 0 && line[keyLen] == '=') {
            string_copy(out, line + keyLen + 1, size);
            out[strcspn(out, "\r\n")] = '\0';
            break;
        }
    }

    fclose(f);
}

// ---- export ----

typedef struct {
    title_info* title;
    save_kind kind;
    char dir[FILE_PATH_MAX];   // The export folder, with the trailing slash.

    void (*then)(void* ctx, bool ok);   // NULL: tell the owner where it went.
    void* thenCtx;
} export_ctx;

static void export_done(void* ctx, Result res) {
    export_ctx* exp = (export_ctx*) ctx;

    if(exp->then != NULL) {
        exp->then(exp->thenCtx, R_SUCCEEDED(res));
    } else if(R_SUCCEEDED(res)) {
        char text[FILE_PATH_MAX + 64];
        snprintf(text, sizeof(text), _("Saved in sdmc:%s"), exp->dir);
        prompt_display_notify("Success", text, COLOR_TEXT, exp->title, task_draw_title_info, NULL);
    }

    free(exp);
}

static void save_export(title_info* title, const char* name, save_kind kind, u32 extId, bool backup, void (*then)(void* ctx, bool ok), void* thenCtx) {
    export_ctx* exp = (export_ctx*) calloc(1, sizeof(export_ctx));
    tree_copy* job = (tree_copy*) calloc(1, sizeof(tree_copy));
    if(exp == NULL || job == NULL) {
        free(exp);
        free(job);
        error_display(NULL, NULL, "Failed to allocate the export.");
        if(then != NULL) {
            then(thenCtx, false);
        }

        return;
    }

    exp->title = title;
    exp->kind = kind;
    exp->then = then;
    exp->thenCtx = thenCtx;

    job->title = title;
    job->done = export_done;
    job->ctx = exp;
    string_copy(job->srcRoot, "/", sizeof(job->srcRoot));

    char stamp[32];
    save_timestamp(stamp, sizeof(stamp));
    snprintf(exp->dir, sizeof(exp->dir), SAVES_DIR "%016llX/%s%s/", (unsigned long long) title->titleId, stamp, backup ? BACKUP_SUFFIX : "");
    snprintf(job->dstRoot, sizeof(job->dstRoot), "%s%s/", exp->dir, save_kind_dir(kind));

    Result res = save_open_target(&job->srcArchive, title, kind, extId);
    if(R_FAILED(res)) {
        error_display_res(title, task_draw_title_info, res, kind == SAVE_KIND_EXT ? "Failed to open the extra data." : "Failed to open the save data. Has the game been started once?");
        free(job);
        export_done(exp, res);
        return;
    }

    if(R_FAILED(res = save_open_sd(&job->dstArchive)) || R_FAILED(res = save_make_dirs(job->dstArchive, job->dstRoot))) {
        error_display_res(title, task_draw_title_info, res, "Failed to prepare the folder on the SD card.");
        fs_close_archive(job->srcArchive);
        if(job->dstArchive != 0) {
            fs_close_archive(job->dstArchive);
        }

        free(job);
        export_done(exp, res);
        return;
    }

    save_write_manifest(exp->dir, title, name, kind, stamp);

    tree_copy_start(job);
}

void action_export_save_files(title_info* title, const char* name, save_kind kind, u32 extId) {
    save_export(title, name, kind, extId, false, NULL, NULL);
}

// ---- import ----

typedef struct {
    title_info* title;
    char name[LIST_ITEM_NAME_MAX];
    save_kind kind;
    u32 extId;
    char from[FILE_PATH_MAX];   // The export folder to import, with the trailing slash.
} import_ctx;

static void import_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    task_draw_title_info(view, ((import_ctx*) data)->title, x1, y1, x2, y2);
}

static void import_copied(void* ctx, Result res) {
    import_ctx* imp = (import_ctx*) ctx;

    if(R_SUCCEEDED(res)) {
        prompt_display_notify("Success", imp->kind == SAVE_KIND_EXT ? "Extra data imported." : "Save imported.", COLOR_TEXT, imp->title, task_draw_title_info, NULL);
    } else {
        // The archive was emptied before: the copy of what was there is in the -before-import folder.
        prompt_display_notify("Failure", "The import did not finish. What was there before is in the folder ending in -before-import.", COLOR_TEXT, imp->title, task_draw_title_info, NULL);
    }

    free(imp);
}

// Empties the archive's root: the files and folders of the old save.
static Result import_wipe(FS_Archive archive) {
    FS_Path rootPath = fsMakePath(PATH_ASCII, "/");

    Handle dir = 0;
    Result res = FSUSER_OpenDirectory(&dir, archive, rootPath);
    if(R_FAILED(res)) {
        return res;
    }

    u32 count = 0;
    FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(MAX_FILES, sizeof(FS_DirectoryEntry));
    if(entries == NULL) {
        FSDIR_Close(dir);
        return R_APP_OUT_OF_MEMORY;
    }

    res = FSDIR_Read(dir, &count, MAX_FILES, entries);
    FSDIR_Close(dir);

    for(u32 i = 0; R_SUCCEEDED(res) && i < count; i++) {
        char name[FILE_NAME_MAX] = {'\0'};
        utf16_to_utf8((uint8_t*) name, entries[i].name, sizeof(name) - 1);

        char path[FILE_PATH_MAX];
        snprintf(path, sizeof(path), "/%s", name);

        FS_Path* fsPath = fs_make_path_utf8(path);
        if(fsPath == NULL) {
            res = R_APP_OUT_OF_MEMORY;
            break;
        }

        if(entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) {
            res = FSUSER_DeleteDirectoryRecursively(archive, *fsPath);
        } else {
            res = FSUSER_DeleteFile(archive, *fsPath);
        }

        fs_free_path_utf8(fsPath);
    }

    free(entries);
    return res;
}

// After the old data was exported: empty the archive and copy the chosen folder into it.
static void import_backed_up(void* ctx, bool ok) {
    import_ctx* imp = (import_ctx*) ctx;
    if(!ok) {
        free(imp);
        return;
    }

    tree_copy* job = (tree_copy*) calloc(1, sizeof(tree_copy));
    if(job == NULL) {
        error_display(NULL, NULL, "Failed to allocate the import.");
        free(imp);
        return;
    }

    job->title = imp->title;
    job->commit = true;
    job->done = import_copied;
    job->ctx = imp;
    snprintf(job->srcRoot, sizeof(job->srcRoot), "%s%s/", imp->from, save_kind_dir(imp->kind));
    string_copy(job->dstRoot, "/", sizeof(job->dstRoot));

    Result res = save_open_target(&job->dstArchive, imp->title, imp->kind, imp->extId);
    if(R_SUCCEEDED(res)) {
        res = save_open_sd(&job->srcArchive);
    }

    if(R_SUCCEEDED(res)) {
        res = import_wipe(job->dstArchive);
    }

    if(R_FAILED(res)) {
        error_display_res(imp->title, import_draw_top, res, "Failed to empty the current data.");
        if(job->dstArchive != 0) {
            fs_close_archive(job->dstArchive);
        }

        if(job->srcArchive != 0) {
            fs_close_archive(job->srcArchive);
        }

        free(job);
        free(imp);
        return;
    }

    tree_copy_start(job);
}

static void import_onresponse(ui_view* view, void* data, u32 response) {
    import_ctx* imp = (import_ctx*) data;
    if(response == PROMPT_YES) {
        save_export(imp->title, imp->name, imp->kind, imp->extId, true, import_backed_up, imp);
    } else {
        free(imp);
    }
}

// One choice per export folder, newest first: the sheet's actions carry no index, so each row has its own.
typedef struct {
    title_info* title;
    char name[LIST_ITEM_NAME_MAX];
    save_kind kind;
    u32 extId;

    u32 count;
    char folder[PICK_MAX][FILE_NAME_MAX];
    char label[PICK_MAX][96];
} import_picks;

static import_picks picks;

static void import_pick(u32 index) {
    import_ctx* imp = (import_ctx*) calloc(1, sizeof(import_ctx));
    if(imp == NULL) {
        error_display(NULL, NULL, "Failed to allocate the import.");
        return;
    }

    imp->title = picks.title;
    string_copy(imp->name, picks.name, sizeof(imp->name));
    imp->kind = picks.kind;
    imp->extId = picks.extId;
    snprintf(imp->from, sizeof(imp->from), SAVES_DIR "%016llX/%s/", (unsigned long long) picks.title->titleId, picks.folder[index]);

    char saved[16];
    save_read_manifest(imp->from, "version", saved, sizeof(saved));

    char text[512];
    char versions[160] = {'\0'};
    char current[16];
    snprintf(current, sizeof(current), "%u", (unsigned) picks.title->version);
    if(saved[0] != '\0' && strcmp(saved, current) != 0) {
        snprintf(versions, sizeof(versions), _("\nThe export is from version %s of the game; the installed one is %s."), saved, current);
    }

    snprintf(text, sizeof(text), _("Replace the current %s with the one from %s? What is there now is exported first.%s"),
             picks.kind == SAVE_KIND_EXT ? _("extra data") : _("save"), picks.label[index], versions);

    prompt_display_warning("Confirmation", text, COLOR_TEXT, imp, import_draw_top, import_onresponse);
}

#define PICK_FN(n) static void import_pick_##n(void* data) { import_pick(n); }
PICK_FN(0) PICK_FN(1) PICK_FN(2) PICK_FN(3) PICK_FN(4) PICK_FN(5) PICK_FN(6) PICK_FN(7)
PICK_FN(8) PICK_FN(9) PICK_FN(10) PICK_FN(11) PICK_FN(12) PICK_FN(13) PICK_FN(14) PICK_FN(15)

static void (*const pick_fns[PICK_MAX])(void*) = {
    import_pick_0, import_pick_1, import_pick_2, import_pick_3, import_pick_4, import_pick_5, import_pick_6, import_pick_7,
    import_pick_8, import_pick_9, import_pick_10, import_pick_11, import_pick_12, import_pick_13, import_pick_14, import_pick_15,
};

static int import_compare_names(const void* a, const void* b) {
    // The folder names start with the date: newest first.
    return strcmp((const char*) b, (const char*) a);
}

void action_import_save_files(title_info* title, const char* name, save_kind kind, u32 extId) {
    char base[FILE_PATH_MAX];
    snprintf(base, sizeof(base), SAVES_DIR "%016llX/", (unsigned long long) title->titleId);

    FS_Archive sd = 0;
    Result res = save_open_sd(&sd);
    if(R_FAILED(res)) {
        error_display_res(title, task_draw_title_info, res, "Failed to open the SD card.");
        return;
    }

    static char names[64][FILE_NAME_MAX];
    u32 found = 0;

    FS_Path* fsBase = fs_make_path_utf8(base);
    Handle dir = 0;
    if(fsBase != NULL && R_SUCCEEDED(FSUSER_OpenDirectory(&dir, sd, *fsBase))) {
        FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(MAX_FILES, sizeof(FS_DirectoryEntry));
        u32 count = 0;
        if(entries != NULL && R_SUCCEEDED(FSDIR_Read(dir, &count, MAX_FILES, entries))) {
            for(u32 i = 0; i < count && found < 64; i++) {
                if(!(entries[i].attributes & FS_ATTRIBUTE_DIRECTORY)) {
                    continue;
                }

                char entry[FILE_NAME_MAX] = {'\0'};
                utf16_to_utf8((uint8_t*) entry, entries[i].name, sizeof(entry) - 1);

                char sub[FILE_PATH_MAX];
                snprintf(sub, sizeof(sub), "%s%s/%s", base, entry, save_kind_dir(kind));
                if(fs_is_dir(sd, sub)) {
                    string_copy(names[found++], entry, FILE_NAME_MAX);
                }
            }
        }

        free(entries);
        FSDIR_Close(dir);
    }

    if(fsBase != NULL) {
        fs_free_path_utf8(fsBase);
    }

    fs_close_archive(sd);

    if(found == 0) {
        char text[FILE_PATH_MAX + 64];
        snprintf(text, sizeof(text), _("Nothing to import: there are no exports of this title in sdmc:%s"), base);
        prompt_display_notify("Failure", text, COLOR_TEXT, title, task_draw_title_info, NULL);
        return;
    }

    qsort(names, found, FILE_NAME_MAX, import_compare_names);

    picks.title = title;
    string_copy(picks.name, name, sizeof(picks.name));
    picks.kind = kind;
    picks.extId = extId;
    picks.count = found < PICK_MAX ? found : PICK_MAX;

    sheet_action actions[PICK_MAX];
    for(u32 i = 0; i < picks.count; i++) {
        string_copy(picks.folder[i], names[i], FILE_NAME_MAX);

        // 20261031-142530 -> 2026-10-31 14:25:30
        const char* n = names[i];
        bool backup = strstr(n, BACKUP_SUFFIX) != NULL;
        if(strlen(n) >= 15 && n[8] == '-') {
            snprintf(picks.label[i], sizeof(picks.label[i]), "%.4s-%.2s-%.2s %.2s:%.2s:%.2s%s", n, n + 4, n + 6, n + 9, n + 11, n + 13, backup ? _("  (before an import)") : "");
        } else {
            string_copy(picks.label[i], n, sizeof(picks.label[i]));
        }

        actions[i] = (sheet_action) {ICON_DOWNLOAD, picks.label[i], true, pick_fns[i], false};
    }

    sheet_display_list(NULL, 0, actions, picks.count, NULL);
}
