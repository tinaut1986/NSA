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
#include "../transfer.h"

#define MAX_FILES 1024
#define SAVES_DIR APP_DATA_DIR "saves/"
#define BACKUP_SUFFIX "-before-import"
#define PICK_MAX 24
#define CHECKPOINT_DIR "/3ds/Checkpoint/"

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
        fprintf(f, "source=NSA\nkind=%s\ntitleid=%016llX\nversion=%u\nname=%s\ndate=%s\n", save_kind_dir(kind),
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
    title_info* title;          // Points at titleCopy.
    title_info titleCopy;
    char name[LIST_ITEM_NAME_MAX];
    save_kind kind;
    u32 extId;
    char from[FILE_PATH_MAX];   // The folder whose tree is copied in, with the trailing slash.
    char manifest[FILE_PATH_MAX];   // The export folder with the manifest, or empty (Checkpoint's have none).
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
    string_copy(job->srcRoot, imp->from, sizeof(job->srcRoot));
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

// Asks, in red, whether to replace the current data with the tree in `from`. `manifest` is the export
// folder with the manifest (the version it was taken at), or empty.
static void import_ask(title_info* title, const char* name, save_kind kind, u32 extId, const char* manifest, const char* from, const char* label) {
    import_ctx* imp = (import_ctx*) calloc(1, sizeof(import_ctx));
    if(imp == NULL) {
        error_display(NULL, NULL, "Failed to allocate the import.");
        return;
    }

    imp->titleCopy = *title;
    imp->title = &imp->titleCopy;
    string_copy(imp->name, name, sizeof(imp->name));
    imp->kind = kind;
    imp->extId = extId;
    string_copy(imp->from, from, sizeof(imp->from));
    string_copy(imp->manifest, manifest, sizeof(imp->manifest));

    char saved[16] = {'\0'};
    if(manifest[0] != '\0') {
        save_read_manifest(manifest, "version", saved, sizeof(saved));
    }

    char text[512];
    char versions[160] = {'\0'};
    char current[16];
    snprintf(current, sizeof(current), "%u", (unsigned) title->version);
    if(saved[0] != '\0' && strcmp(saved, current) != 0) {
        snprintf(versions, sizeof(versions), _("\nThe export is from version %s of the game; the installed one is %s."), saved, current);
    }

    snprintf(text, sizeof(text), _("Replace the current %s with the one from %s? What is there now is exported first.%s"),
             kind == SAVE_KIND_EXT ? _("extra data") : _("save"), label, versions);

    prompt_display_warning("Confirmation", text, COLOR_TEXT, imp, import_draw_top, import_onresponse);
}

// One choice per backup, NSA's first (newest first), then Checkpoint's: the sheet's actions carry no index,
// so each row has its own function.
typedef struct {
    title_info* title;
    char name[LIST_ITEM_NAME_MAX];
    save_kind kind;
    u32 extId;

    u32 count;
    char path[PICK_MAX][FILE_PATH_MAX];   // The backup's folder, with the trailing slash.
    bool checkpoint[PICK_MAX];
    char label[PICK_MAX][96];
} import_picks;

static import_picks picks;

static void import_pick(u32 index) {
    if(picks.checkpoint[index]) {
        // Checkpoint keeps the archive's tree right in the folder, and no manifest.
        import_ask(picks.title, picks.name, picks.kind, picks.extId, "", picks.path[index], picks.label[index]);
    } else {
        char root[FILE_PATH_MAX];
        snprintf(root, sizeof(root), "%s%s/", picks.path[index], save_kind_dir(picks.kind));
        import_ask(picks.title, picks.name, picks.kind, picks.extId, picks.path[index], root, picks.label[index]);
    }
}

#define PICK_FN(n) static void import_pick_##n(void* data) { import_pick(n); }
PICK_FN(0) PICK_FN(1) PICK_FN(2) PICK_FN(3) PICK_FN(4) PICK_FN(5) PICK_FN(6) PICK_FN(7)
PICK_FN(8) PICK_FN(9) PICK_FN(10) PICK_FN(11) PICK_FN(12) PICK_FN(13) PICK_FN(14) PICK_FN(15)
PICK_FN(16) PICK_FN(17) PICK_FN(18) PICK_FN(19) PICK_FN(20) PICK_FN(21) PICK_FN(22) PICK_FN(23)

static void (*const pick_fns[PICK_MAX])(void*) = {
    import_pick_0, import_pick_1, import_pick_2, import_pick_3, import_pick_4, import_pick_5, import_pick_6, import_pick_7,
    import_pick_8, import_pick_9, import_pick_10, import_pick_11, import_pick_12, import_pick_13, import_pick_14, import_pick_15,
    import_pick_16, import_pick_17, import_pick_18, import_pick_19, import_pick_20, import_pick_21, import_pick_22, import_pick_23,
};

// The names of the folders inside `dir` (with the trailing slash), at most `max`; `prefix` (or NULL) keeps
// only those that start with it.
static u32 save_list_dirs(FS_Archive sd, const char* dir, const char* prefix, char names[][FILE_NAME_MAX], u32 max) {
    u32 found = 0;
    FS_Path* fsDir = fs_make_path_utf8(dir);
    Handle handle = 0;
    if(fsDir != NULL && R_SUCCEEDED(FSUSER_OpenDirectory(&handle, sd, *fsDir))) {
        FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(MAX_FILES, sizeof(FS_DirectoryEntry));
        u32 count = 0;
        if(entries != NULL && R_SUCCEEDED(FSDIR_Read(handle, &count, MAX_FILES, entries))) {
            for(u32 i = 0; i < count && found < max; i++) {
                char entry[FILE_NAME_MAX] = {'\0'};
                utf16_to_utf8((uint8_t*) entry, entries[i].name, sizeof(entry) - 1);
                if((entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) && (prefix == NULL || strncmp(entry, prefix, strlen(prefix)) == 0)) {
                    string_copy(names[found++], entry, FILE_NAME_MAX);
                }
            }
        }

        free(entries);
        FSDIR_Close(handle);
    }

    if(fsDir != NULL) {
        fs_free_path_utf8(fsDir);
    }

    return found;
}

static int import_compare_names(const void* a, const void* b) {
    // Newest first (the NSA names start with the date; Checkpoint's usually do too).
    return strcmp((const char*) b, (const char*) a);
}

static void import_add_pick(const char* path, bool checkpoint, const char* label) {
    if(picks.count >= PICK_MAX) {
        return;
    }

    string_copy(picks.path[picks.count], path, FILE_PATH_MAX);
    picks.checkpoint[picks.count] = checkpoint;
    string_copy(picks.label[picks.count], label, sizeof(picks.label[0]));
    picks.count++;
}

void action_import_save_files(title_info* title, const char* name, save_kind kind, u32 extId) {
    FS_Archive sd = 0;
    Result res = save_open_sd(&sd);
    if(R_FAILED(res)) {
        error_display_res(title, task_draw_title_info, res, "Failed to open the SD card.");
        return;
    }

    picks.title = title;
    string_copy(picks.name, name, sizeof(picks.name));
    picks.kind = kind;
    picks.extId = extId;
    picks.count = 0;

    static char names[48][FILE_NAME_MAX];

    // NSA's own exports.
    char base[FILE_PATH_MAX];
    snprintf(base, sizeof(base), SAVES_DIR "%016llX/", (unsigned long long) title->titleId);
    u32 listed = save_list_dirs(sd, base, NULL, names, 48);
    u32 found = 0;
    for(u32 i = 0; i < listed; i++) {
        char sub[FILE_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s%s/%s", base, names[i], save_kind_dir(kind));
        if(fs_is_dir(sd, sub)) {
            string_copy(names[found++], names[i], FILE_NAME_MAX);
        }
    }

    qsort(names, found, FILE_NAME_MAX, import_compare_names);
    for(u32 i = 0; i < found; i++) {
        // 20261031-142530 -> 2026-10-31 14:25:30
        const char* n = names[i];
        bool backup = strstr(n, BACKUP_SUFFIX) != NULL;
        char label[96];
        if(strlen(n) >= 15 && n[8] == '-') {
            snprintf(label, sizeof(label), "[NSA] %.4s-%.2s-%.2s %.2s:%.2s:%.2s%s", n, n + 4, n + 6, n + 9, n + 11, n + 13, backup ? _("  (before an import)") : "");
        } else {
            snprintf(label, sizeof(label), "[NSA] %s", n);
        }

        char path[FILE_PATH_MAX];
        snprintf(path, sizeof(path), "%s%s/", base, n);
        import_add_pick(path, false, label);
    }

    // Checkpoint's: <root>/saves|extdata/0x<unique ID, 5 digits> <game>/<backup>/ with the tree inside.
    char idPrefix[16];
    snprintf(idPrefix, sizeof(idPrefix), "0x%05X", (unsigned) ((u32) title->titleId >> 8));
    char cpBase[FILE_PATH_MAX];
    snprintf(cpBase, sizeof(cpBase), CHECKPOINT_DIR "%s/", kind == SAVE_KIND_EXT ? "extdata" : "saves");
    u32 games = save_list_dirs(sd, cpBase, idPrefix, names, 8);
    for(u32 g = 0; g < games; g++) {
        static char backups[32][FILE_NAME_MAX];
        char gameDir[FILE_PATH_MAX];
        snprintf(gameDir, sizeof(gameDir), "%s%s/", cpBase, names[g]);

        u32 n = save_list_dirs(sd, gameDir, NULL, backups, 32);
        qsort(backups, n, FILE_NAME_MAX, import_compare_names);
        for(u32 i = 0; i < n; i++) {
            char label[96];
            char path[FILE_PATH_MAX];
            snprintf(label, sizeof(label), "[Checkpoint] %s", backups[i]);
            snprintf(path, sizeof(path), "%s%s/", gameDir, backups[i]);
            import_add_pick(path, true, label);
        }
    }

    fs_close_archive(sd);

    if(picks.count == 0) {
        char text[FILE_PATH_MAX + 64];
        snprintf(text, sizeof(text), _("Nothing to import: there are no exports of this title in sdmc:%s, nor Checkpoint backups"), base);
        prompt_display_notify("Failure", text, COLOR_TEXT, title, task_draw_title_info, NULL);
        return;
    }

    sheet_action actions[PICK_MAX];
    for(u32 i = 0; i < picks.count; i++) {
        actions[i] = (sheet_action) {ICON_DOWNLOAD, picks.label[i], true, pick_fns[i], false};
    }

    sheet_display_list(NULL, 0, actions, picks.count, NULL);
}

// ---- extra data of a game ----

// The ID of the extra data a game has on the SD card, if it has any: under its own ID, or under it
// without the last byte (Mario Kart 7, 00030700, keeps its data as 307).
bool action_find_ext_data(title_info* info, u32* extId) {
    title_kind kind = title_kind_of(info->titleId);
    if(info->twl || (kind != TITLE_KIND_GAME && kind != TITLE_KIND_DEMO)) {
        return false;
    }

    u32 candidates[2] = {(u32) info->titleId, (u32) info->titleId >> 8};
    for(u32 i = 0; i < 2; i++) {
        u32 path[3] = {MEDIATYPE_SD, candidates[i], 0};
        FS_Archive archive = 0;
        if(R_SUCCEEDED(fs_open_archive(&archive, ARCHIVE_EXTDATA, fs_make_path_binary(path, sizeof(path))))) {
            fs_close_archive(archive);
            *extId = candidates[i];
            return true;
        }
    }

    return false;
}

// ---- a save as one file, to send it ----
//
//   "NSASAVE1", u32 kind, u64 title ID, u32 version, u32 name length + name, u32 entries,
//   then per entry: u8 type (0 folder, 1 file), u16 path length + path (relative, no leading slash),
//   u64 size, and the bytes of a file.
// It travels as a plain file (resumable, like any other) named <TITLEID>-<save|ext>-<name>.nsasave.

#define PACK_MAGIC "NSASAVE1"
#define PACK_EXT ".nsasave"
#define PACK_DIR SAVES_DIR "send/"
#define PACK_NAME_MAX 64

typedef struct worker_s worker;

struct worker_s {
    Thread thread;
    volatile bool done;
    volatile bool cancel;
    volatile Result result;
    volatile u64 processed;
    volatile u64 total;
    const char* status;     // Translated.

    title_info* title;      // For the top screen, or NULL.
    Result (*work)(worker* w);
    void (*finished)(worker* w, Result res);
    void* arg;
};

static void worker_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    worker* w = (worker*) data;
    if(w->title != NULL) {
        task_draw_title_info(view, w->title, x1, y1, x2, y2);
    }
}

static void worker_thread(void* arg) {
    worker* w = (worker*) arg;
    w->result = w->work(w);
    w->done = true;
}

static void worker_update(ui_view* view, void* data, float* progress, char* text) {
    worker* w = (worker*) data;

    if(w->done) {
        ui_pop();
        info_destroy(view);

        threadJoin(w->thread, U64_MAX);
        threadFree(w->thread);

        Result res = w->result;
        w->finished(w, res);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        w->cancel = true;
    }

    u64 total = w->total;
    *progress = total != 0 ? (float) ((double) w->processed / (double) total) : 0;
    snprintf(text, PROGRESS_TEXT_MAX, "%s\n%.2f %s / %.2f %s", w->status,
             ui_get_display_size(w->processed), ui_get_display_size_units(w->processed),
             ui_get_display_size(total), ui_get_display_size_units(total));
}

// `finished` frees the worker and whatever it carries.
static void worker_start(worker* w, const char* title) {
    w->thread = threadCreate(worker_thread, w, 0x10000, 0x19, -2, false);
    if(w->thread == NULL) {
        error_display(NULL, NULL, "Failed to start the task.");
        w->finished(w, R_APP_THREAD_CREATE_FAILED);
        return;
    }

    info_display(title, "Press B to cancel.", true, w, worker_update, worker_draw_top);
}

// Is `rel` a path that stays inside the folder it is unpacked in?
static bool pack_path_ok(const char* rel) {
    if(rel[0] == '\0' || rel[0] == '/' || strchr(rel, '\\') != NULL || strchr(rel, ':') != NULL) {
        return false;
    }

    for(const char* part = rel; part != NULL; ) {
        const char* slash = strchr(part, '/');
        size_t len = slash != NULL ? (size_t) (slash - part) : strlen(part);
        if(len == 0 || (len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.')) {
            return false;
        }

        part = slash != NULL ? slash + 1 : NULL;
    }

    return true;
}

typedef struct {
    title_info title;
    char name[LIST_ITEM_NAME_MAX];
    save_kind kind;
    u32 extId;
    char path[FILE_PATH_MAX];       // The package, with the sdmc: prefix.
    char sdPath[FILE_PATH_MAX];     // The same inside the SD archive.
    char fileName[FILE_NAME_MAX];
    FS_Archive src;
    FILE* out;
    u32 entries;
    u64 bytes;
    u8* buffer;
} pack_job;

#define PACK_BUFFER (64 * 1024)

static bool pack_write(pack_job* job, const void* data, size_t size) {
    return job->out == NULL || fwrite(data, 1, size, job->out) == size;
}

// Walks `dir` (ends with a slash) of the archive: counts, or also writes when job->out is set.
static Result pack_walk(worker* w, pack_job* job, const char* dir, const char* rel) {
    FS_Path* dirPath = fs_make_path_utf8(dir);
    if(dirPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Handle handle = 0;
    Result res = FSUSER_OpenDirectory(&handle, job->src, *dirPath);
    fs_free_path_utf8(dirPath);
    if(R_FAILED(res)) {
        return res;
    }

    FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(MAX_FILES, sizeof(FS_DirectoryEntry));
    u32 count = 0;
    if(entries == NULL) {
        FSDIR_Close(handle);
        return R_APP_OUT_OF_MEMORY;
    }

    res = FSDIR_Read(handle, &count, MAX_FILES, entries);
    FSDIR_Close(handle);

    for(u32 i = 0; R_SUCCEEDED(res) && i < count; i++) {
        if(w->cancel) {
            res = R_APP_CANCELLED;
            break;
        }

        char name[FILE_NAME_MAX] = {'\0'};
        utf16_to_utf8((uint8_t*) name, entries[i].name, sizeof(name) - 1);

        char entryRel[FILE_PATH_MAX];
        char entryPath[FILE_PATH_MAX];
        if((size_t) snprintf(entryRel, sizeof(entryRel), "%s%s", rel, name) >= sizeof(entryRel)
           || (size_t) snprintf(entryPath, sizeof(entryPath), "%s%s", dir, name) >= sizeof(entryPath)) {
            res = R_APP_BAD_DATA;
            break;
        }

        u16 pathLen = (u16) strlen(entryRel);
        bool isDir = (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
        u8 type = isDir ? 0 : 1;
        u64 size = 0;

        Handle file = 0;
        if(!isDir) {
            FS_Path* filePath = fs_make_path_utf8(entryPath);
            if(filePath == NULL) {
                res = R_APP_OUT_OF_MEMORY;
                break;
            }

            res = FSUSER_OpenFile(&file, job->src, *filePath, FS_OPEN_READ, 0);
            fs_free_path_utf8(filePath);
            if(R_SUCCEEDED(res)) {
                res = FSFILE_GetSize(file, &size);
            }

            if(R_FAILED(res)) {
                if(file != 0) {
                    FSFILE_Close(file);
                }

                break;
            }
        }

        job->entries++;
        job->bytes += size;

        if(job->out != NULL) {
            if(!pack_write(job, &type, 1) || !pack_write(job, &pathLen, 2) || !pack_write(job, entryRel, pathLen) || !pack_write(job, &size, 8)) {
                res = R_APP_BAD_DATA;
            }

            for(u64 at = 0; R_SUCCEEDED(res) && !isDir && at < size; ) {
                u32 want = size - at < PACK_BUFFER ? (u32) (size - at) : PACK_BUFFER;
                u32 got = 0;
                if(w->cancel) {
                    res = R_APP_CANCELLED;
                } else if(R_SUCCEEDED(res = FSFILE_Read(file, &got, at, job->buffer, want)) && got != want) {
                    res = R_APP_BAD_DATA;
                } else if(R_SUCCEEDED(res) && !pack_write(job, job->buffer, got)) {
                    res = R_APP_BAD_DATA;
                }

                at += got;
                w->processed += got;
            }
        }

        if(file != 0) {
            FSFILE_Close(file);
        }

        if(R_SUCCEEDED(res) && isDir) {
            char subDir[FILE_PATH_MAX];
            char subRel[FILE_PATH_MAX];
            if((size_t) snprintf(subDir, sizeof(subDir), "%s/", entryPath) >= sizeof(subDir)
               || (size_t) snprintf(subRel, sizeof(subRel), "%s/", entryRel) >= sizeof(subRel)) {
                res = R_APP_BAD_DATA;
            } else {
                res = pack_walk(w, job, subDir, subRel);
            }
        }
    }

    free(entries);
    return res;
}

static Result pack_work(worker* w) {
    pack_job* job = (pack_job*) w->arg;
    Result res = 0;

    w->status = _("Counting...");
    job->entries = 0;
    job->bytes = 0;
    job->out = NULL;
    if(R_FAILED(res = pack_walk(w, job, "/", ""))) {
        return res;
    }

    w->total = job->bytes;
    w->processed = 0;
    w->status = _("Packing...");

    u32 entries = job->entries;
    job->entries = 0;
    job->bytes = 0;

    job->out = fopen(job->path, "wb");
    if(job->out == NULL) {
        return R_APP_BAD_DATA;
    }

    u32 kind = job->kind;
    u32 version = job->title.version;
    u32 nameLen = (u32) strlen(job->name);
    bool ok = pack_write(job, PACK_MAGIC, 8) && pack_write(job, &kind, 4) && pack_write(job, &job->title.titleId, 8)
              && pack_write(job, &version, 4) && pack_write(job, &nameLen, 4) && pack_write(job, job->name, nameLen)
              && pack_write(job, &entries, 4);

    res = ok ? pack_walk(w, job, "/", "") : R_APP_BAD_DATA;
    if(R_SUCCEEDED(res) && job->entries != entries) {
        res = R_APP_BAD_DATA;   // The save changed while it was read.
    }

    if(fclose(job->out) != 0 && R_SUCCEEDED(res)) {
        res = R_APP_BAD_DATA;
    }

    job->out = NULL;
    if(R_FAILED(res)) {
        remove(job->path);
    }

    return res;
}

static FS_Archive pack_sd_kept = 0;

static void pack_finished(worker* w, Result res) {
    pack_job* job = (pack_job*) w->arg;

    fs_close_archive(job->src);
    free(job->buffer);

    if(R_SUCCEEDED(res)) {
        FILE* f = fopen(job->path, "rb");
        u64 size = 0;
        if(f != NULL) {
            fseek(f, 0, SEEK_END);
            size = (u64) ftell(f);
            fclose(f);
        }

        if(pack_sd_kept == 0) {
            save_open_sd(&pack_sd_kept);
        }

        if(size > 0 && pack_sd_kept != 0) {
            // The Send screen keeps the archive open: it stays open for good (one handle).
            transfer_send_file(pack_sd_kept, job->sdPath, job->fileName, size);
        } else {
            error_display(NULL, NULL, "Failed to read the package.");
        }
    } else if(res != R_APP_CANCELLED) {
        error_display_res(&job->title, task_draw_title_info, res, "Failed to pack the data.");
    } else {
        prompt_display_notify("Failure", "Operation cancelled.", COLOR_TEXT, NULL, NULL, NULL);
    }

    free(job);
    free(w);
}

void action_send_save_files(title_info* title, const char* name, save_kind kind, u32 extId) {
    worker* w = (worker*) calloc(1, sizeof(worker));
    pack_job* job = (pack_job*) calloc(1, sizeof(pack_job));
    if(w == NULL || job == NULL) {
        free(w);
        free(job);
        error_display(NULL, NULL, "Failed to allocate the package.");
        return;
    }

    job->title = *title;
    string_copy(job->name, name, sizeof(job->name));
    job->kind = kind;
    job->extId = extId;
    job->buffer = (u8*) malloc(PACK_BUFFER);

    Result res = job->buffer != NULL ? 0 : R_APP_OUT_OF_MEMORY;
    if(R_SUCCEEDED(res)) {
        res = save_open_target(&job->src, title, kind, extId);
        if(R_FAILED(res)) {
            error_display_res(title, task_draw_title_info, res, kind == SAVE_KIND_EXT ? "Failed to open the extra data." : "Failed to open the save data. Has the game been started once?");
            free(job->buffer);
            free(job);
            free(w);
            return;
        }
    }

    if(R_FAILED(res)) {
        error_display(NULL, NULL, "Failed to allocate the package.");
        free(job->buffer);
        free(job);
        free(w);
        return;
    }

    // Readable by the receiver, which sees only the name: what it is and whose.
    char clean[PACK_NAME_MAX];
    size_t n = 0;
    for(const char* c = name; *c != '\0' && n + 1 < sizeof(clean); c++) {
        bool plain = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == ' ' || *c == '.' || *c == '(' || *c == ')';
        clean[n++] = plain ? *c : '_';
    }

    clean[n] = '\0';
    snprintf(job->fileName, sizeof(job->fileName), "%016llX-%s-%s" PACK_EXT, (unsigned long long) title->titleId, save_kind_dir(kind), clean);
    snprintf(job->sdPath, sizeof(job->sdPath), PACK_DIR "%s", job->fileName);
    snprintf(job->path, sizeof(job->path), "sdmc:%s", job->sdPath);

    FS_Archive sd = 0;
    if(R_SUCCEEDED(save_open_sd(&sd))) {
        save_make_dirs(sd, PACK_DIR);
        fs_close_archive(sd);
    }

    w->title = &job->title;
    w->arg = job;
    w->work = pack_work;
    w->finished = pack_finished;
    w->status = _("Counting...");
    worker_start(w, "Packing");
}

// ---- receiving one ----

// Reads "<TITLEID>-<save|ext>-<name>.nsasave"; false for any other name.
bool action_save_package_name(const char* fileName, u64* titleId, save_kind* kind, char* name, size_t nameSize) {
    size_t len = strlen(fileName);
    size_t extLen = strlen(PACK_EXT);
    if(len < 16 + 1 + 3 + 1 || len < extLen || strcasecmp(fileName + len - extLen, PACK_EXT) != 0) {
        return false;
    }

    u64 id = 0;
    for(u32 i = 0; i < 16; i++) {
        char c = fileName[i];
        u32 v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;
        if(v > 15) {
            return false;
        }

        id = (id << 4) | v;
    }

    const char* rest = fileName + 17;
    if(fileName[16] != '-') {
        return false;
    }

    if(strncmp(rest, "save-", 5) == 0) {
        *kind = SAVE_KIND_SAVE;
    } else if(strncmp(rest, "ext-", 4) == 0) {
        *kind = SAVE_KIND_EXT;
    } else {
        return false;
    }

    rest += *kind == SAVE_KIND_EXT ? 4 : 5;
    size_t nameLen = (size_t) (fileName + len - extLen - rest);
    if(nameLen >= nameSize) {
        nameLen = nameSize - 1;
    }

    memcpy(name, rest, nameLen);
    name[nameLen] = '\0';
    *titleId = id;
    return true;
}

// The title as installed here (SD, then NAND), if it is.
static bool save_installed_title(u64 titleId, title_info* out) {
    static const FS_MediaType media[2] = {MEDIATYPE_SD, MEDIATYPE_NAND};
    for(u32 i = 0; i < 2; i++) {
        AM_TitleEntry entry;
        if(R_SUCCEEDED(AM_GetTitleInfo(media[i], 1, &titleId, &entry))) {
            memset(out, 0, sizeof(*out));
            out->mediaType = media[i];
            out->titleId = titleId;
            out->version = entry.version;
            out->installedSize = entry.size;
            return true;
        }
    }

    return false;
}

bool action_save_package_can_import(u64 titleId, save_kind kind) {
    title_info title;
    if(!save_installed_title(titleId, &title)) {
        return false;
    }

    u32 extId = 0;
    if(kind == SAVE_KIND_EXT) {
        return action_find_ext_data(&title, &extId);
    }

    FS_Archive archive = 0;
    if(R_FAILED(save_open_target(&archive, &title, kind, 0))) {
        return false;
    }

    fs_close_archive(archive);
    return true;
}

typedef struct {
    char package[FILE_PATH_MAX];    // With the sdmc: prefix.
    char dir[FILE_PATH_MAX];        // The folder it is unpacked in (inside the SD archive), with the trailing slash.
    bool importNow;
    u64 titleId;
    u32 version;
    save_kind kind;
    char name[LIST_ITEM_NAME_MAX];
    char stamp[32];
    u8* buffer;
} unpack_job;

static bool unpack_read(FILE* f, void* out, size_t size) {
    return fread(out, 1, size, f) == size;
}

static Result unpack_work(worker* w) {
    unpack_job* job = (unpack_job*) w->arg;

    FILE* f = fopen(job->package, "rb");
    if(f == NULL) {
        return R_APP_BAD_DATA;
    }

    fseek(f, 0, SEEK_END);
    w->total = (u64) ftell(f);
    fseek(f, 0, SEEK_SET);
    w->processed = 0;
    w->status = _("Unpacking...");

    char magic[8];
    u32 kind = 0;
    u32 nameLen = 0;
    u32 entries = 0;
    Result res = 0;
    if(!unpack_read(f, magic, 8) || memcmp(magic, PACK_MAGIC, 8) != 0 || !unpack_read(f, &kind, 4) || kind > 1
       || !unpack_read(f, &job->titleId, 8) || !unpack_read(f, &job->version, 4) || !unpack_read(f, &nameLen, 4)
       || nameLen >= sizeof(job->name) || !unpack_read(f, job->name, nameLen) || !unpack_read(f, &entries, 4)) {
        fclose(f);
        return R_APP_BAD_DATA;
    }

    job->name[nameLen] = '\0';
    job->kind = kind == 1 ? SAVE_KIND_EXT : SAVE_KIND_SAVE;

    FS_Archive sd = 0;
    char root[FILE_PATH_MAX];
    snprintf(root, sizeof(root), "%s%s/", job->dir, save_kind_dir(job->kind));
    if(R_FAILED(res = save_open_sd(&sd)) || R_FAILED(res = save_make_dirs(sd, root))) {
        if(sd != 0) {
            fs_close_archive(sd);
        }

        fclose(f);
        return res;
    }

    for(u32 i = 0; R_SUCCEEDED(res) && i < entries; i++) {
        u8 type = 0;
        u16 pathLen = 0;
        u64 size = 0;
        char rel[FILE_PATH_MAX];
        if(w->cancel) {
            res = R_APP_CANCELLED;
            break;
        }

        if(!unpack_read(f, &type, 1) || type > 1 || !unpack_read(f, &pathLen, 2) || pathLen == 0 || pathLen >= sizeof(rel)
           || !unpack_read(f, rel, pathLen) || !unpack_read(f, &size, 8)) {
            res = R_APP_BAD_DATA;
            break;
        }

        rel[pathLen] = '\0';
        char path[FILE_PATH_MAX];
        if(!pack_path_ok(rel) || (size_t) snprintf(path, sizeof(path), "%s%s", root, rel) >= sizeof(path)) {
            res = R_APP_BAD_DATA;
            break;
        }

        if(type == 0) {
            res = fs_ensure_dir(sd, path);
            continue;
        }

        char full[FILE_PATH_MAX + 8];
        snprintf(full, sizeof(full), "sdmc:%s", path);
        FILE* out = fopen(full, "wb");
        if(out == NULL) {
            res = R_APP_BAD_DATA;
            break;
        }

        for(u64 at = 0; at < size; ) {
            u32 want = size - at < PACK_BUFFER ? (u32) (size - at) : PACK_BUFFER;
            if(!unpack_read(f, job->buffer, want) || fwrite(job->buffer, 1, want, out) != want) {
                res = R_APP_BAD_DATA;
                break;
            }

            at += want;
            w->processed += want;
        }

        if(fclose(out) != 0 && R_SUCCEEDED(res)) {
            res = R_APP_BAD_DATA;
        }
    }

    fclose(f);
    fs_close_archive(sd);
    return res;
}

static void unpack_finished(worker* w, Result res) {
    unpack_job* job = (unpack_job*) w->arg;

    if(R_SUCCEEDED(res)) {
        // The folder is a normal export now: what to know about it is in its manifest.
        title_info title;
        memset(&title, 0, sizeof(title));
        title.titleId = job->titleId;
        title.version = (u16) job->version;
        save_write_manifest(job->dir, &title, job->name, job->kind, job->stamp);
        remove(job->package);

        title_info installed;
        u32 extId = 0;
        if(job->importNow && save_installed_title(job->titleId, &installed) && (job->kind == SAVE_KIND_SAVE || action_find_ext_data(&installed, &extId))) {
            char root[FILE_PATH_MAX];
            snprintf(root, sizeof(root), "%s%s/", job->dir, save_kind_dir(job->kind));
            import_ask(&installed, job->name, job->kind, extId, job->dir, root, _("the other console"));
        } else {
            char text[FILE_PATH_MAX + 64];
            snprintf(text, sizeof(text), _("Saved in sdmc:%s"), job->dir);
            prompt_display_notify("Success", text, COLOR_TEXT, NULL, NULL, NULL);
        }
    } else if(res != R_APP_CANCELLED) {
        error_display_res(NULL, NULL, res, "Failed to unpack the data.");
    }

    free(job->buffer);
    free(job);
    free(w);
}

// A package arrived at `path` (inside the SD archive): unpack it into a folder of saves, then import
// it if asked to. The package is deleted once unpacked.
void action_save_package_received(const char* path, bool importNow) {
    worker* w = (worker*) calloc(1, sizeof(worker));
    unpack_job* job = (unpack_job*) calloc(1, sizeof(unpack_job));
    if(w == NULL || job == NULL) {
        free(w);
        free(job);
        error_display(NULL, NULL, "Failed to allocate the package.");
        return;
    }

    job->buffer = (u8*) malloc(PACK_BUFFER);
    if(job->buffer == NULL) {
        free(w);
        free(job);
        error_display(NULL, NULL, "Failed to allocate the package.");
        return;
    }

    snprintf(job->package, sizeof(job->package), "sdmc:%s", path);
    job->importNow = importNow;
    save_timestamp(job->stamp, sizeof(job->stamp));

    // The title is in the file's name too; the folder is known before reading it.
    u64 id = 0;
    save_kind kind = SAVE_KIND_SAVE;
    char name[LIST_ITEM_NAME_MAX];
    const char* base = strrchr(path, '/');
    if(!action_save_package_name(base != NULL ? base + 1 : path, &id, &kind, name, sizeof(name))) {
        error_display(NULL, NULL, "Failed to unpack the data.");
        free(job->buffer);
        free(job);
        free(w);
        return;
    }

    snprintf(job->dir, sizeof(job->dir), SAVES_DIR "%016llX/%s/", (unsigned long long) id, job->stamp);

    w->arg = job;
    w->work = unpack_work;
    w->finished = unpack_finished;
    w->status = _("Unpacking...");
    worker_start(w, "Unpacking");
}
