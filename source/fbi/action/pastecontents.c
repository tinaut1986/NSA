#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

typedef struct {
    linked_list* items;

    list_item* targetItem;
    file_info* target;

    linked_list contents;

    data_op_data pasteInfo;

    // Cut and pasted onto another medium: the source goes once the copy is whole.
    bool moveSource;
} paste_contents_data;

static void action_paste_contents_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    u32 curr = pasteData->pasteInfo.processed;
    if(curr < pasteData->pasteInfo.total) {
        task_draw_file_info(view, ((list_item*) linked_list_get(&pasteData->contents, curr))->data, x1, y1, x2, y2);
    } else {
        task_draw_file_info(view, pasteData->target, x1, y1, x2, y2);
    }
}

static void action_paste_contents_get_dst_path(paste_contents_data* data, u32 index, char* dstPath) {
    char baseSrcPath[FILE_PATH_MAX];
    if(clipboard_is_contents_only()) {
        string_copy(baseSrcPath, clipboard_get_path(), FILE_PATH_MAX);
    } else {
        string_get_parent_path(baseSrcPath, clipboard_get_path(), FILE_PATH_MAX);
    }

    char baseDstPath[FILE_PATH_MAX];
    if(data->target->attributes & FS_ATTRIBUTE_DIRECTORY) {
        string_copy(baseDstPath, data->target->path, FILE_PATH_MAX);
    } else {
        string_get_parent_path(baseDstPath, data->target->path, FILE_PATH_MAX);
    }

    snprintf(dstPath, FILE_PATH_MAX, "%s%s", baseDstPath, ((file_info*) ((list_item*) linked_list_get(&data->contents, index))->data)->path + strlen(baseSrcPath));
}

static Result action_paste_contents_is_src_directory(void* data, u32 index, bool* isDirectory) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    *isDirectory = (bool) (((file_info*) ((list_item*) linked_list_get(&pasteData->contents, index))->data)->attributes & FS_ATTRIBUTE_DIRECTORY);
    return 0;
}

static Result action_paste_contents_make_dst_directory(void* data, u32 index) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    Result res = 0;

    u32 attributes = ((file_info*) ((list_item*) linked_list_get(&pasteData->contents, index))->data)->attributes;

    char dstPath[FILE_PATH_MAX];
    action_paste_contents_get_dst_path(pasteData, index, dstPath);

    FS_Path* fsPath = fs_make_path_utf8(dstPath);
    if(fsPath != NULL) {
        Handle dirHandle = 0;
        if(R_SUCCEEDED(FSUSER_OpenDirectory(&dirHandle, pasteData->target->archive, *fsPath))) {
            FSDIR_Close(dirHandle);
        } else if(R_SUCCEEDED(res = FSUSER_CreateDirectory(pasteData->target->archive, *fsPath, attributes))) {
            char parentPath[FILE_PATH_MAX];
            string_get_parent_path(parentPath, dstPath, FILE_PATH_MAX);

            char baseDstPath[FILE_PATH_MAX];
            if(pasteData->target->attributes & FS_ATTRIBUTE_DIRECTORY) {
                string_copy(baseDstPath, pasteData->target->path, FILE_PATH_MAX);
            } else {
                string_get_parent_path(baseDstPath, pasteData->target->path, FILE_PATH_MAX);
            }

            if(strncmp(parentPath, baseDstPath, FILE_PATH_MAX) == 0) {
                list_item* dstItem = NULL;
                if(R_SUCCEEDED(res) && R_SUCCEEDED(task_create_file_item(&dstItem, pasteData->target->archive, dstPath, attributes, true))) {
                    linked_list_add(pasteData->items, dstItem);
                }
            }
        }

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static Result action_paste_contents_open_src(void* data, u32 index, u32* handle) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(((file_info*) ((list_item*) linked_list_get(&pasteData->contents, index))->data)->path);
    if(fsPath != NULL) {
        res = FSUSER_OpenFile(handle, clipboard_get_archive(), *fsPath, FS_OPEN_READ, 0);

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static Result action_paste_contents_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    return FSFILE_Close(handle);
}

static Result action_paste_contents_get_src_size(void* data, u32 handle, u64* size) {
    return FSFILE_GetSize(handle, size);
}

static Result action_paste_contents_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    return FSFILE_Read(handle, bytesRead, offset, buffer, size);
}

static Result action_paste_contents_open_dst(void* data, u32 index, void* initialReadBlock, u64 size, u32* handle) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    Result res = 0;

    char dstPath[FILE_PATH_MAX];
    action_paste_contents_get_dst_path(pasteData, index, dstPath);

    FS_Path* fsPath = fs_make_path_utf8(dstPath);
    if(fsPath != NULL) {
        Handle currHandle;
        if(R_SUCCEEDED(FSUSER_OpenFile(&currHandle, pasteData->target->archive, *fsPath, FS_OPEN_READ, 0))) {
            FSFILE_Close(currHandle);
            if(R_SUCCEEDED(res = FSUSER_DeleteFile(pasteData->target->archive, *fsPath))) {
                linked_list_iter iter;
                linked_list_iterate(pasteData->items, &iter);

                while(linked_list_iter_has_next(&iter)) {
                    list_item* item = (list_item*) linked_list_iter_next(&iter);
                    file_info* currInfo = (file_info*) item->data;

                    if(strncmp(currInfo->path, dstPath, FILE_PATH_MAX) == 0) {
                        linked_list_iter_remove(&iter);
                        task_free_file(item);
                    }
                }
            }
        }

        if(R_SUCCEEDED(res) && R_SUCCEEDED(res = FSUSER_CreateFile(pasteData->target->archive, *fsPath, ((file_info*) ((list_item*) linked_list_get(&pasteData->contents, index))->data)->attributes & ~FS_ATTRIBUTE_READ_ONLY, size))) {
            res = FSUSER_OpenFile(handle, pasteData->target->archive, *fsPath, FS_OPEN_WRITE, 0);
        }

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static Result action_paste_contents_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    Result res = 0;

    // Stopped or failed half way: the file has its full size (the copy sets it first) but not its data. It goes,
    // so that nothing looks whole that is not.
    if(!succeeded) {
        res = FSFILE_Close(handle);

        char dstPath[FILE_PATH_MAX];
        action_paste_contents_get_dst_path(pasteData, index, dstPath);
        FS_Path* fsPath = fs_make_path_utf8(dstPath);
        if(fsPath != NULL) {
            FSUSER_DeleteFile(pasteData->target->archive, *fsPath);
            fs_free_path_utf8(fsPath);
        }

        return res;
    }

    if(R_SUCCEEDED(res = FSFILE_Close(handle))) {
        char dstPath[FILE_PATH_MAX];
        action_paste_contents_get_dst_path(pasteData, index, dstPath);

        char parentPath[FILE_PATH_MAX];
        string_get_parent_path(parentPath, dstPath, FILE_PATH_MAX);

        char baseDstPath[FILE_PATH_MAX];
        if(pasteData->target->attributes & FS_ATTRIBUTE_DIRECTORY) {
            string_copy(baseDstPath, pasteData->target->path, FILE_PATH_MAX);
        } else {
            string_get_parent_path(baseDstPath, pasteData->target->path, FILE_PATH_MAX);
        }

        if(strncmp(parentPath, baseDstPath, FILE_PATH_MAX) == 0) {
            list_item* dstItem = NULL;
            if(R_SUCCEEDED(task_create_file_item(&dstItem, pasteData->target->archive, dstPath, ((file_info*) ((list_item*) linked_list_get(&pasteData->contents, index))->data)->attributes & ~FS_ATTRIBUTE_READ_ONLY, true))) {
                linked_list_add(pasteData->items, dstItem);
            }
        }
    }

    return res;
}

static Result action_paste_contents_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

static bool action_paste_contents_error(void* data, u32 index, Result res, ui_view** errorView) {
    *errorView = error_display_res(data, action_paste_contents_draw_top, res, "Failed to paste content.");
    return true;
}

static void action_paste_contents_free_data(paste_contents_data* data) {
    task_clear_files(&data->contents);
    linked_list_destroy(&data->contents);

    if(data->targetItem != NULL) {
        task_free_file(data->targetItem);
        data->targetItem = NULL;
        data->target = NULL;
    }

    free(data);
}

static Result action_paste_contents_delete_path(const char* path);

// Cut and pasted onto another medium: what was cut goes once the copy is whole. With names (a selection), only those.
static Result action_paste_contents_delete_source() {
    u32 count = clipboard_get_name_count();
    if(count == 0) {
        return action_paste_contents_delete_path(clipboard_get_path());
    }

    Result res = 0;
    for(u32 i = 0; i < count; i++) {
        char path[FILE_PATH_MAX];
        snprintf(path, sizeof(path), "%s%s", clipboard_get_path(), clipboard_get_name(i));
        Result one = action_paste_contents_delete_path(path);
        if(R_FAILED(one) && R_SUCCEEDED(res)) {
            res = one;
        }
    }

    return res;
}

static Result action_paste_contents_delete_path(const char* path) {
    if(fs_is_dir(clipboard_get_archive(), path)) {
        return fs_delete_dir_recursively(clipboard_get_archive(), path);
    }

    Result res = 0;
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath != NULL) {
        res = FSUSER_DeleteFile(clipboard_get_archive(), *fsPath);
        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static void action_paste_contents_update(ui_view* view, void* data, float* progress, char* text) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    if(pasteData->pasteInfo.finished) {
        FSUSER_ControlArchive(pasteData->target->archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);

        linked_list_sort(pasteData->items, NULL, task_compare_files);

        ui_pop();
        info_destroy(view);

        if(R_SUCCEEDED(pasteData->pasteInfo.result)) {
            if(pasteData->moveSource) {
                Result delRes = action_paste_contents_delete_source();
                clipboard_clear();
                if(R_SUCCEEDED(delRes)) {
                    prompt_display_notify("Success", "Moved.", COLOR_TEXT, NULL, NULL, NULL);
                } else {
                    error_display_res(NULL, NULL, delRes, "Copied, but failed to delete the original.");
                }
            } else {
                prompt_display_notify("Success", "Contents pasted.", COLOR_TEXT, NULL, NULL, NULL);
            }
        }

        action_paste_contents_free_data(pasteData);

        return;
    }

    if((hidKeysDown() & KEY_B) && !pasteData->pasteInfo.finished) {
        svcSignalEvent(pasteData->pasteInfo.cancelEvent);
    }

    data_op_progress_bytes(&pasteData->pasteInfo, progress, text, true);
}

static void action_paste_contents_onresponse(ui_view* view, void* data, u32 response) {
    paste_contents_data* pasteData = (paste_contents_data*) data;
    if(response == PROMPT_YES) {
        Result res = task_data_op(&pasteData->pasteInfo);
        if(R_SUCCEEDED(res)) {
            info_display("Pasting Contents", "Press B to cancel.", true, data, action_paste_contents_update, action_paste_contents_draw_top);
        } else {
            error_display_res(NULL, NULL, res, "Failed to initiate paste operation.");

            action_paste_contents_free_data(pasteData);
        }
    } else {
        action_paste_contents_free_data(pasteData);
    }
}

typedef struct {
    paste_contents_data* pasteData;

    populate_files_data popData;
} paste_contents_loading_data;

static void action_paste_contents_loading_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    action_paste_contents_draw_top(view, ((paste_contents_loading_data*) data)->pasteData, x1, y1, x2, y2);
}

static void action_paste_contents_loading_update(ui_view* view, void* data, float* progress, char* text)  {
    paste_contents_loading_data* loadingData = (paste_contents_loading_data*) data;

    if(info_task_finished(view, &loadingData->popData.finished, loadingData->popData.cancelEvent)) {
        if(loadingData->popData.cancelled) {
            // B while reading: nothing to ask about, the list is not whole.
            action_paste_contents_free_data(loadingData->pasteData);
        } else if(R_SUCCEEDED(loadingData->popData.result)) {
            loadingData->pasteData->pasteInfo.total = linked_list_size(&loadingData->pasteData->contents);
            loadingData->pasteData->pasteInfo.processed = loadingData->pasteData->pasteInfo.total;

            bool names = clipboard_get_name_count() > 0;
            const char* question = loadingData->pasteData->moveSource ? (names ? "Move the cut items to the current directory? They are copied and then deleted from where they were." : "Move the cut item to the current directory? It is copied and then deleted from where it was.")
                                                                       : "Paste clipboard contents to the current directory?";
            prompt_display_yes_no("Confirmation", question, COLOR_TEXT, loadingData->pasteData, action_paste_contents_draw_top, action_paste_contents_onresponse);
        } else {
            error_display_res(NULL, NULL, loadingData->popData.result, "Failed to populate clipboard content list.");

            action_paste_contents_free_data(loadingData->pasteData);
        }

        free(loadingData);
        return;
    }

    snprintf(text, PROGRESS_TEXT_MAX, _("Fetching clipboard content list..."));
}

// Archives whose files can be renamed across folders: the whole medium is one archive.
static bool action_paste_contents_renamable(FS_ArchiveID id) {
    return id == ARCHIVE_SDMC || id == ARCHIVE_NAND_CTR_FS || id == ARCHIVE_NAND_TWL_FS || id == ARCHIVE_TWL_PHOTO || id == ARCHIVE_TWL_SOUND;
}

typedef enum {
    MOVE_DONE,
    MOVE_SAME_FOLDER,
    MOVE_INTO_ITSELF,
    MOVE_FAILED
} move_outcome;

// Renames one cut file or folder (`srcPath`, a folder's without its last '/') into `baseDstPath`.
static move_outcome action_paste_contents_move_one(linked_list* items, file_info* target, const char* baseDstPath, const char* srcPath, Result* resOut) {
    bool isDir = fs_is_dir(clipboard_get_archive(), srcPath);

    char name[FILE_NAME_MAX] = {'\0'};
    string_get_path_file(name, srcPath, sizeof(name));

    char srcDirPath[FILE_PATH_MAX] = {'\0'};
    snprintf(srcDirPath, FILE_PATH_MAX, isDir && srcPath[strlen(srcPath) - 1] != '/' ? "%s/" : "%s", srcPath);

    char dstPath[FILE_PATH_MAX] = {'\0'};
    snprintf(dstPath, FILE_PATH_MAX, isDir ? "%s%s/" : "%s%s", baseDstPath, name);

    if(strncmp(srcDirPath, dstPath, FILE_PATH_MAX) == 0) {
        return MOVE_SAME_FOLDER;
    }

    if(isDir && strncmp(dstPath, srcDirPath, strlen(srcDirPath)) == 0) {
        return MOVE_INTO_ITSELF;
    }

    Result res = 0;
    FS_Path* srcFsPath = fs_make_path_utf8(srcPath);
    if(srcFsPath != NULL) {
        FS_Path* dstFsPath = fs_make_path_utf8(dstPath);
        if(dstFsPath != NULL) {
            if(isDir) {
                res = FSUSER_RenameDirectory(clipboard_get_archive(), *srcFsPath, clipboard_get_archive(), *dstFsPath);
            } else {
                res = FSUSER_RenameFile(clipboard_get_archive(), *srcFsPath, clipboard_get_archive(), *dstFsPath);
            }

            fs_free_path_utf8(dstFsPath);
        } else {
            res = R_APP_OUT_OF_MEMORY;
        }

        fs_free_path_utf8(srcFsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    if(R_FAILED(res)) {
        *resOut = res;
        return MOVE_FAILED;
    }

    list_item* dstItem = NULL;
    if(R_SUCCEEDED(task_create_file_item(&dstItem, target->archive, dstPath, isDir ? FS_ATTRIBUTE_DIRECTORY : 0, true))) {
        linked_list_add(items, dstItem);
    }

    return MOVE_DONE;
}

// A cut item (or the items of a selection) on the same medium is renamed into the folder: no copy.
static void action_paste_contents_move_here(linked_list* items, list_item* selected) {
    file_info* target = (file_info*) selected->data;

    char baseDstPath[FILE_PATH_MAX] = {'\0'};
    if(target->attributes & FS_ATTRIBUTE_DIRECTORY) {
        string_copy(baseDstPath, target->path, FILE_PATH_MAX);
    } else {
        string_get_parent_path(baseDstPath, target->path, FILE_PATH_MAX);
    }

    u32 count = clipboard_get_name_count();
    u32 total = count > 0 ? count : 1;
    u32 moved = 0;
    move_outcome last = MOVE_DONE;
    Result failure = 0;
    for(u32 i = 0; i < total; i++) {
        char srcPath[FILE_PATH_MAX];
        if(count > 0) {
            snprintf(srcPath, sizeof(srcPath), "%s%s", clipboard_get_path(), clipboard_get_name(i));
        } else {
            string_copy(srcPath, clipboard_get_path(), sizeof(srcPath));
        }

        Result res = 0;
        move_outcome outcome = action_paste_contents_move_one(items, target, baseDstPath, srcPath, &res);
        if(outcome == MOVE_DONE) {
            moved++;
        } else {
            last = outcome;
            if(outcome == MOVE_FAILED && R_SUCCEEDED(failure)) {
                failure = res;
            }
        }
    }

    if(moved > 0) {
        FSUSER_ControlArchive(target->archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
        linked_list_sort(items, NULL, task_compare_files);
    }

    if(moved == total) {
        clipboard_clear();
        prompt_display_notify("Success", "Moved.", COLOR_TEXT, NULL, NULL, NULL);
    } else if(last == MOVE_FAILED) {
        error_display_res(NULL, NULL, failure, moved > 0 ? "Some items were moved, but not all." : "Failed to move the item.");
    } else if(last == MOVE_SAME_FOLDER) {
        prompt_display_notify("Failure", moved > 0 ? "Some items were moved; the rest are already in this folder." : "It is already in this folder.", COLOR_TEXT, NULL, NULL, NULL);
    } else {
        prompt_display_notify("Failure", "A folder cannot be moved into itself.", COLOR_TEXT, NULL, NULL, NULL);
    }
}

void action_paste_contents_in(linked_list* items, list_item* selected, FS_ArchiveID archiveId) {
    // A selection copied into the folder it came from would overwrite each item with itself (deleting it first).
    if(clipboard_has_contents() && !clipboard_is_cut() && clipboard_get_name_count() > 0 && archiveId != 0 && archiveId == clipboard_get_archive_id()) {
        file_info* target = (file_info*) selected->data;
        char dir[FILE_PATH_MAX] = {'\0'};
        if(target->attributes & FS_ATTRIBUTE_DIRECTORY) {
            string_copy(dir, target->path, FILE_PATH_MAX);
        } else {
            string_get_parent_path(dir, target->path, FILE_PATH_MAX);
        }

        if(strncmp(dir, clipboard_get_path(), FILE_PATH_MAX) == 0) {
            prompt_display_notify("Failure", "The copied items are already in this folder.", COLOR_TEXT, NULL, NULL, NULL);
            return;
        }
    }

    if(clipboard_has_contents() && clipboard_is_cut() && archiveId != 0 && archiveId == clipboard_get_archive_id() && action_paste_contents_renamable(archiveId)) {
        action_paste_contents_move_here(items, selected);
        return;
    }

    action_paste_contents(items, selected);
}

void action_paste_contents(linked_list* items, list_item* selected) {
    if(!clipboard_has_contents()) {
        prompt_display_notify("Failure", "Clipboard empty.", COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    paste_contents_data* data = (paste_contents_data*) calloc(1, sizeof(paste_contents_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate paste contents data.");

        return;
    }

    data->moveSource = clipboard_is_cut();

    data->items = items;

    file_info* targetInfo = (file_info*) selected->data;
    Result targetCreateRes = task_create_file_item(&data->targetItem, targetInfo->archive, targetInfo->path, targetInfo->attributes, false);
    if(R_FAILED(targetCreateRes)) {
        error_display_res(NULL, NULL, targetCreateRes, "Failed to create target file item.");

        action_paste_contents_free_data(data);
        return;
    }

    data->target = (file_info*) data->targetItem->data;

    data->pasteInfo.data = data;

    data->pasteInfo.op = DATAOP_COPY;

    data->pasteInfo.bufferSize = 256 * 1024;
    data->pasteInfo.copyEmpty = true;

    data->pasteInfo.isSrcDirectory = action_paste_contents_is_src_directory;
    data->pasteInfo.makeDstDirectory = action_paste_contents_make_dst_directory;

    data->pasteInfo.openSrc = action_paste_contents_open_src;
    data->pasteInfo.closeSrc = action_paste_contents_close_src;
    data->pasteInfo.getSrcSize = action_paste_contents_get_src_size;
    data->pasteInfo.readSrc = action_paste_contents_read_src;

    data->pasteInfo.openDst = action_paste_contents_open_dst;
    data->pasteInfo.closeDst = action_paste_contents_close_dst;
    data->pasteInfo.writeDst = action_paste_contents_write_dst;

    data->pasteInfo.error = action_paste_contents_error;

    data->pasteInfo.finished = true;

    linked_list_init(&data->contents);

    paste_contents_loading_data* loadingData = (paste_contents_loading_data*) calloc(1, sizeof(paste_contents_loading_data));
    if(loadingData == NULL) {
        error_display(NULL, NULL, "Failed to allocate loading data.");

        action_paste_contents_free_data(data);
        return;
    }

    loadingData->pasteData = data;

    loadingData->popData.items = &data->contents;
    loadingData->popData.archive = clipboard_get_archive();
    string_copy(loadingData->popData.path, clipboard_get_path(), FILE_PATH_MAX);
    loadingData->popData.recursive = true;
    loadingData->popData.includeBase = !clipboard_is_contents_only() || !fs_is_dir(clipboard_get_archive(), clipboard_get_path());
    loadingData->popData.meta = false;
    // A selection: only its items of that folder, with all they hold.
    bool names = clipboard_get_name_count() > 0;
    loadingData->popData.filter = names ? clipboard_filter_names : NULL;
    loadingData->popData.filterData = NULL;
    loadingData->popData.filterTopOnly = names;

    Result listRes = task_populate_files(&loadingData->popData);
    if(R_FAILED(listRes)) {
        error_display_res(NULL, NULL, listRes, "Failed to initiate clipboard content list population.");

        free(loadingData);
        action_paste_contents_free_data(data);
        return;
    }

    info_display("Loading", "Press B to cancel.", false, loadingData, action_paste_contents_loading_update, action_paste_contents_loading_draw_top);
}