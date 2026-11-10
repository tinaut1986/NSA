#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

// Copies a title's ExeFS files (.code, icon, banner, logo) to sdmc:/nsa/exefs/<title ID>/, as the title's content
// archive hands them: decrypted, the code as the title ships it. For mods, patches and translations, which work on
// these files, without GodMode9.

#define EXEFS_DIR APP_DATA_DIR "exefs/"

static const char* exefs_names[] = {".code", "icon", "banner", "logo"};

// One file of the ExeFS onto the SD. *found is false when the title has no such file (not an error: a DLC has no
// code, a system title often no icon).
static Result action_extract_exefs_file(u64 titleId, FS_MediaType mediaType, FS_Archive sdmc, const char* dir, const char* name, bool* found) {
    *found = false;

    // The content archive's file path: content 0, ExeFS (2), then the file's 8-byte name.
    u32 filePath[5] = {0x00000000, 0x00000000, 0x00000002, 0x00000000, 0x00000000};
    memcpy(&filePath[3], name, strlen(name));
    u32 archivePath[4] = {(u32) (titleId & 0xFFFFFFFF), (u32) ((titleId >> 32) & 0xFFFFFFFF), mediaType, 0x00000000};

    Handle in = 0;
    if(R_FAILED(FSUSER_OpenFileDirectly(&in, ARCHIVE_SAVEDATA_AND_CONTENT, fs_make_path_binary(archivePath, sizeof(archivePath)),
                                        fs_make_path_binary(filePath, sizeof(filePath)), FS_OPEN_READ, 0))) {
        return 0;
    }

    *found = true;

    // An ExeFS file is read whole, in one read: one of a part of it, or from an offset, is "not supported"
    // (0xE0C046F9). The code of a game is a few MB at most.
    Result res = 0;
    u64 size = 0;
    if(R_SUCCEEDED(res = FSFILE_GetSize(in, &size))) {
        u8* buf = (u8*) malloc(size > 0 ? (size_t) size : 1);
        if(buf != NULL) {
            u32 bytesRead = 0;
            if(R_SUCCEEDED(res = FSFILE_Read(in, &bytesRead, 0, buf, (u32) size)) && bytesRead != size) {
                res = R_APP_BAD_DATA;
            }

            if(R_SUCCEEDED(res)) {
                char path[96];
                snprintf(path, sizeof(path), "%s%s", dir, name);

                FS_Path* fsPath = fs_make_path_utf8(path);
                if(fsPath != NULL) {
                    FSUSER_DeleteFile(sdmc, *fsPath);   // A shorter file over an old dump must not keep its tail.

                    Handle out = 0;
                    if(R_SUCCEEDED(res = FSUSER_OpenFile(&out, sdmc, *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
                        u32 bytesWritten = 0;
                        res = FSFILE_Write(out, &bytesWritten, 0, buf, bytesRead, FS_WRITE_FLUSH);
                        FSFILE_Close(out);
                    }

                    fs_free_path_utf8(fsPath);
                } else {
                    res = R_APP_OUT_OF_MEMORY;
                }
            }

            free(buf);
        } else {
            res = R_APP_OUT_OF_MEMORY;
        }
    }

    FSFILE_Close(in);
    return res;
}

Result action_extract_exefs_to_sd(u64 titleId, FS_MediaType mediaType, u32* extracted) {
    *extracted = 0;

    char dir[64];
    snprintf(dir, sizeof(dir), EXEFS_DIR "%016llX/", titleId);

    Result res = 0;
    FS_Archive sdmc = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        if(R_SUCCEEDED(res = fs_ensure_dir(sdmc, APP_DATA_DIR)) && R_SUCCEEDED(res = fs_ensure_dir(sdmc, EXEFS_DIR))
           && R_SUCCEEDED(res = fs_ensure_dir(sdmc, dir))) {
            for(u32 i = 0; i < sizeof(exefs_names) / sizeof(exefs_names[0]) && R_SUCCEEDED(res); i++) {
                bool found = false;
                res = action_extract_exefs_file(titleId, mediaType, sdmc, dir, exefs_names[i], &found);
                if(found && R_SUCCEEDED(res)) {
                    (*extracted)++;
                }
            }
        }

        FSUSER_CloseArchive(sdmc);
    }

    // Not even one file: the title's content could not be opened at all.
    if(R_SUCCEEDED(res) && *extracted == 0) {
        res = R_APP_BAD_DATA;
    }

    return res;
}

static void action_extract_exefs_update(ui_view* view, void* data, float* progress, char* text) {
    title_info* info = (title_info*) data;

    u32 extracted = 0;
    Result res = action_extract_exefs_to_sd(info->titleId, info->mediaType, &extracted);

    ui_pop();
    info_destroy(view);

    if(R_SUCCEEDED(res)) {
        prompt_display_notify("Success", "ExeFS extracted to sdmc:" EXEFS_DIR ".", COLOR_TEXT, info, task_draw_title_info, NULL);
    } else {
        error_display_res(info, task_draw_title_info, res, "Failed to extract ExeFS.");
    }
}

static void action_extract_exefs_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        info_display("Extracting ExeFS", "", false, data, action_extract_exefs_update, task_draw_title_info);
    }
}

void action_extract_exefs(linked_list* items, list_item* selected) {
    prompt_display_yes_no("Confirmation", "Extract the ExeFS of the selected title (code, icon, banner and logo)?", COLOR_TEXT, selected->data,
                          task_draw_title_info, action_extract_exefs_onresponse);
}
