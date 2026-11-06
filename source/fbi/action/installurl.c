#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

typedef enum content_type_e {
    CONTENT_CIA,
    CONTENT_TICKET,
    CONTENT_3DSX_SMDH,
    CONTENT_SAVED      // "Save": the file goes to INSTALL_URL_SAVE_DIR as it is, nothing is installed.
} content_type;

// Where "Save" keeps the downloads, to install later from Files: with what arrives from another 3DS, as the SD
// card's root is the owner's.
#define INSTALL_URL_SAVE_DIR APP_DATA_DIR "received/"


// What became of each URL, for the list on the install screen.
typedef enum {
    URL_QUEUED,
    URL_INSTALLED,
    URL_FAILED,
    URL_SKIPPED,
    URL_SAVED
} url_state;

typedef struct {
    char urls[INSTALL_URLS_MAX][DOWNLOAD_URL_MAX];
    u8 states[INSTALL_URLS_MAX];

    char paths[INSTALL_URLS_MAX][FILE_PATH_MAX];

    void* userData;
    void (*finishedURL)(void* data, u32 index);
    void (*finishedAll)(void* data);
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index);

    bool saveOnly;   // A "Save" choice: the files go to INSTALL_URL_SAVE_DIR (and may be installed from there).
    u32 mode;        // INSTALL_URL_CHOICE_*.
    bool currIsCia;  // The file being saved is a CIA.
    char* toInstall; // The CIAs saved, one a line, to install once the downloads end ("Save and install").
    char confirm[256];
    content_type contentType;
    u64 currTitleId;
    volatile bool n3dsContinue;
    ticket_info ticketInfo;
    char currPath[FILE_PATH_MAX];

    data_op_data installInfo;
} install_url_data;

// The confirmation's choices. Install: downloads and installs at once. The others save the file to
// INSTALL_URL_SAVE_DIR first (long pieces over the connections that pay off, http_download_file), and
// install it from there once the downloads end, deleting it after the third.
#define INSTALL_URL_CHOICE_INSTALL 0
#define INSTALL_URL_CHOICE_SAVE_INSTALL 1
#define INSTALL_URL_CHOICE_SAVE_INSTALL_DELETE 2
#define INSTALL_URL_CHOICE_SAVE 3
#define INSTALL_URL_CHOICES 4
#define INSTALL_URL_CHOICE_CANCEL 4

static const char* action_install_url_choice_names[INSTALL_URL_CHOICES] = {"Install", "Save and install", "Save, install and delete", "Save"};
static const char* action_install_url_choice_infos[INSTALL_URL_CHOICES] = {
        "Downloads and installs at the same time, with no extra space on the SD card. With links good for one request (Dropbox) it comes over one connection. If it fails, it starts again from the beginning.",
        "Downloads the file to /nsa/received/ first (it needs room on the SD card for all of it), over the connections that make it faster; then installs it and keeps the file.",
        "Downloads the file to /nsa/received/ first (it needs room on the SD card for all of it), over the connections that make it faster; then installs it and deletes the file.",
        "Only downloads the file to /nsa/received/ (it needs room on the SD card for all of it), over the connections that make it faster, to install it later from Files."
};

static void action_install_url_free_data(install_url_data* data) {
    if(data->finishedAll != NULL) {
        data->finishedAll(data->userData);
    }

    free(data->toInstall);
    free(data);
}

static void action_install_url_queue_install(install_url_data* data, const char* path) {
    size_t have = data->toInstall != NULL ? strlen(data->toInstall) : 0;
    size_t len = strlen(path);
    char* grown = (char*) realloc(data->toInstall, have + len + 2);
    if(grown != NULL) {
        memcpy(grown + have, path, len);
        grown[have + len] = '\n';
        grown[have + len + 1] = '\0';
        data->toInstall = grown;
    }
}

static void action_install_url_install_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2);

static void action_install_url_n3ds_onresponse(ui_view* view, void* data, u32 response) {
    ((install_url_data*) data)->n3dsContinue = response == PROMPT_YES;
}

static Result action_install_url_get_src_url(void* data, u32 index, char* url, size_t maxSize) {
    install_url_data* installData = (install_url_data*) data;

    strncpy(url, installData->urls[index], maxSize);
    return 0;
}

// "Game Name.cia" from ".../Game%20Name.cia?x=1": the URL's last part, decoded, without the characters FAT refuses.
static void action_install_url_save_name(const char* url, const void* initialReadBlock, char* out, size_t size) {
    // The name the server gave (Google Drive: "EUR - Paper Mario.cia" for .../file/d/ID/view), else the URL's.
    char given[FILE_NAME_MAX];
    if(http_file_name(given, sizeof(given))) {
        string_escape_file_name(out, given, size);
        out[size - 1] = '\0';
        return;
    }

    const char* start = strstr(url, "://");
    start = start != NULL ? start + 3 : url;
    const char* slash = strrchr(start, '/');
    const char* name = slash != NULL && slash[1] != '\0' ? slash + 1 : "";

    char decoded[FILE_NAME_MAX];
    size_t len = 0;
    for(const char* c = name; *c != '\0' && *c != '?' && *c != '#' && len < sizeof(decoded) - 1; c++) {
        unsigned hex = 0;
        if(c[0] == '%' && c[1] != '\0' && c[2] != '\0' && sscanf(c + 1, "%2x", &hex) == 1 && hex >= 0x20) {
            decoded[len++] = (char) hex;
            c += 2;
        } else {
            decoded[len++] = *c == '+' ? ' ' : *c;
        }
    }

    decoded[len] = '\0';
    if(len == 0) {
        string_copy(decoded, "download", sizeof(decoded));
    }

    // No extension: the one its content says.
    if(strchr(decoded, '.') == NULL) {
        u32 magic = *(const u32*) initialReadBlock;
        const char* ext = *(const u16*) initialReadBlock == 0x2020 ? ".cia"
                          : *(const u16*) initialReadBlock == 0x0100 ? ".tik"
                          : magic == 0x58534433 ? ".3dsx" : "";
        size_t used = strlen(decoded);
        string_copy(decoded + used, ext, sizeof(decoded) - used);
    }

    string_escape_file_name(out, decoded, size);
    out[size - 1] = '\0';
}

static bool action_install_url_exists(FS_Archive archive, const char* path) {
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return false;
    }

    Handle file = 0;
    bool exists = R_SUCCEEDED(FSUSER_OpenFile(&file, archive, *fsPath, FS_OPEN_READ, 0));
    if(exists) {
        FSFILE_Close(file);
    }

    fs_free_path_utf8(fsPath);
    return exists || fs_is_dir(archive, path);
}

// "Save": opens INSTALL_URL_SAVE_DIR's file for the download; a name in use gets " (2)", " (3)"...
static Result action_install_url_open_save(install_url_data* installData, u32 index, void* initialReadBlock, u64 size, u32* handle) {
    installData->contentType = CONTENT_SAVED;
    installData->currIsCia = initialReadBlock != NULL && *(u16*) initialReadBlock == 0x2020;

    char name[FILE_NAME_MAX];
    action_install_url_save_name(installData->urls[index], initialReadBlock, name, sizeof(name));

    FS_Archive sdmcArchive = 0;
    Result res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if(R_FAILED(res)) {
        return res;
    }

    if(R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, APP_DATA_DIR)) && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, INSTALL_URL_SAVE_DIR))) {
        char base[FILE_NAME_MAX];
        string_get_file_name(base, name, sizeof(base));
        const char* ext = strlen(base) < strlen(name) ? name + strlen(base) : "";

        snprintf(installData->currPath, FILE_PATH_MAX, INSTALL_URL_SAVE_DIR "%s", name);
        for(u32 n = 2; n < 1000 && action_install_url_exists(sdmcArchive, installData->currPath); n++) {
            snprintf(installData->currPath, FILE_PATH_MAX, INSTALL_URL_SAVE_DIR "%s (%lu)%s", base, n, ext);
        }

        FS_Path* path = fs_make_path_utf8(installData->currPath);
        if(path != NULL) {
            res = FSUSER_OpenFile(handle, sdmcArchive, *path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);

            // Its whole size at once: the pieces are written where they go, and a write far past the end would
            // first fill the gap. A card without room for it gets no file.
            if(R_SUCCEEDED(res) && size > 0 && R_FAILED(res = FSFILE_SetSize(*handle, size))) {
                FSFILE_Close(*handle);
                *handle = 0;
                FSUSER_DeleteFile(sdmcArchive, *path);
            }

            fs_free_path_utf8(path);
        } else {
            res = R_APP_OUT_OF_MEMORY;
        }
    }

    FSUSER_CloseArchive(sdmcArchive);
    return res;
}

static Result action_install_url_open_dst(void* data, u32 index, void* initialReadBlock, u64 size, u32* handle) {
    install_url_data* installData = (install_url_data*) data;

    Result res = 0;

    installData->contentType = CONTENT_CIA;
    installData->currTitleId = 0;
    installData->n3dsContinue = false;
    memset(&installData->ticketInfo, 0, sizeof(installData->ticketInfo));
    memset(&installData->currPath, 0, sizeof(installData->currPath));

    if(installData->saveOnly) {
        return action_install_url_open_save(installData, index, initialReadBlock, size, handle);
    }

    if(*(u16*) initialReadBlock == 0x2020) {
        installData->contentType = CONTENT_CIA;

        u64 titleId = 0;
        if(R_SUCCEEDED(res = cia_get_title_id(&titleId, (u8*) initialReadBlock, installData->installInfo.bufferSize))) {
            FS_MediaType dest = fs_get_title_destination(titleId);

            bool n3ds = false;
            if(R_SUCCEEDED(APT_CheckNew3DS(&n3ds)) && !n3ds && ((titleId >> 28) & 0xF) == 2) {
                ui_view* view = prompt_display_yes_no("Confirmation", "Title is intended for New 3DS systems.\nContinue?", COLOR_TEXT, data, action_install_url_install_draw_top, action_install_url_n3ds_onresponse);
                if(view != NULL) {
                    svcWaitSynchronization(view->active, U64_MAX);
                }

                if(!installData->n3dsContinue) {
                    installData->states[index] = URL_SKIPPED;
                    return R_APP_SKIPPED;
                }
            }

            // Deleting FBI before it reinstalls itself causes issues.
            u64 currTitleId = 0;
            FS_MediaType currMediaType = MEDIATYPE_NAND;

            if(envIsHomebrew() || R_FAILED(APT_GetAppletInfo((NS_APPID) envGetAptAppId(), &currTitleId, (u8*) &currMediaType, NULL, NULL, NULL)) || titleId != currTitleId || dest != currMediaType) {
                AM_DeleteTitle(dest, titleId);
                AM_DeleteTicket(titleId);

                if(dest == MEDIATYPE_SD) {
                    AM_QueryAvailableExternalTitleDatabase(NULL);
                }
            }

            if(R_SUCCEEDED(res = AM_StartCiaInstall(dest, handle))) {
                installData->currTitleId = titleId;
            }
        }
    } else if(*(u16*) initialReadBlock == 0x0100) {
        if(R_SUCCEEDED(res = ticket_get_title_id(&installData->ticketInfo.titleId, (u8*) initialReadBlock, installData->installInfo.bufferSize))) {
            installData->contentType = CONTENT_TICKET;

            installData->ticketInfo.inUse = false;
            installData->ticketInfo.loaded = true;

            AM_DeleteTicket(installData->ticketInfo.titleId);
            res = AMNET_InstallTicketBegin(handle);
        }
    } else if(*(u32*) initialReadBlock == 0x58534433 /* 3DSX */ || *(u32*) initialReadBlock == 0x48444D53 /* SMDH */) {
        installData->contentType = CONTENT_3DSX_SMDH;

        FS_Archive sdmcArchive = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
            char dir[FILE_PATH_MAX];
            if(strlen(installData->paths[index]) > 0) {
                string_get_parent_path(dir, installData->paths[index], FILE_PATH_MAX);
                string_copy(installData->currPath, installData->paths[index], FILE_PATH_MAX);
            } else {
                char filename[FILE_NAME_MAX];
                string_get_path_file(filename, installData->urls[index], FILE_NAME_MAX);

                char name[FILE_NAME_MAX];
                string_get_file_name(name, filename, FILE_NAME_MAX);

                snprintf(dir, FILE_PATH_MAX, "/3ds/%s/", name);
                snprintf(installData->currPath, FILE_PATH_MAX, "/3ds/%s/%s.%s", name, name, *(u32*) initialReadBlock == 0x58534433 ? "3dsx" : "smdh");
            }

            if(R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, "/3ds/")) && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, dir))) {
                FS_Path* path = fs_make_path_utf8(installData->currPath);
                if(path != NULL) {
                    res = FSUSER_OpenFileDirectly(handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);

                    fs_free_path_utf8(path);
                } else {
                    res = R_APP_OUT_OF_MEMORY;
                }
            }

            FSUSER_CloseArchive(sdmcArchive);
        }
    } else {
        res = R_APP_BAD_DATA;
    }

    return res;
}

static Result action_install_url_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    install_url_data* installData = (install_url_data*) data;

    Result res = 0;

    if(succeeded) {
        if(installData->contentType == CONTENT_CIA) {
            if(R_SUCCEEDED(res = AM_FinishCiaInstall(handle))) {
                http_download_seed(installData->currTitleId);

                if(installData->currTitleId == 0x0004013800000002 || installData->currTitleId == 0x0004013820000002) {
                    res = AM_InstallFirm(installData->currTitleId);
                }
            }
        } else if(installData->contentType == CONTENT_TICKET) {
            res = AMNET_InstallTicketFinish(handle);
        } else if(installData->contentType == CONTENT_3DSX_SMDH || installData->contentType == CONTENT_SAVED) {
            res = FSFILE_Close(handle);
        }
    } else {
        if(installData->contentType == CONTENT_CIA) {
            res = AM_CancelCIAInstall(handle);
        } else if(installData->contentType == CONTENT_TICKET) {
            res = AMNET_InstallTicketAbort(handle);
        } else if(installData->contentType == CONTENT_3DSX_SMDH || installData->contentType == CONTENT_SAVED) {
            res = FSFILE_Close(handle);

            FS_Archive sdmcArchive = 0;
            if(R_SUCCEEDED(FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
                FS_Path* path = fs_make_path_utf8(installData->currPath);
                if(path != NULL) {
                    FSUSER_DeleteFile(sdmcArchive, *path);

                    fs_free_path_utf8(path);
                }

                FSUSER_CloseArchive(sdmcArchive);
            }
        }
    }

    if(succeeded && R_SUCCEEDED(res)) {
        installData->states[index] = installData->contentType == CONTENT_SAVED ? URL_SAVED : URL_INSTALLED;

        // "Save and install": installed from the SD card once the downloads end (what is not a CIA stays saved).
        if(installData->contentType == CONTENT_SAVED && installData->currIsCia && installData->mode != INSTALL_URL_CHOICE_SAVE) {
            action_install_url_queue_install(installData, installData->currPath);
        }
    }

    if(R_SUCCEEDED(res) && installData->finishedURL != NULL) {
        installData->finishedURL(installData->userData, index);
    }

    return res;
}

static Result action_install_url_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

static Result action_install_url_suspend(void* data, u32 index) {
    return 0;
}

static Result action_install_url_restore(void* data, u32 index) {
    return 0;
}

static bool action_install_url_error(void* data, u32 index, Result res, ui_view** errorView) {
    install_url_data* installData = (install_url_data*) data;
    installData->states[index] = URL_FAILED;

    char* url = installData->urls[index];
    if(installData->saveOnly) {
        *errorView = error_display_res(data, action_install_url_install_draw_top, res, strlen(url) > 38 ? "Failed to download from URL.\n%.35s..." : "Failed to download from URL.\n%.38s", url);
    } else if(strlen(url) > 38) {
        *errorView = error_display_res(data, action_install_url_install_draw_top, res, "Failed to install from URL.\n%.35s...", url);
    } else {
        *errorView = error_display_res(data, action_install_url_install_draw_top, res, "Failed to install from URL.\n%.38s", url);
    }

    return true;
}

// ---- the install screen: the current URL's progress on top, every URL below ----

// "Game Name.cia" from ".../Game%20Name.cia?x=1": the part after the last slash, decoded as the saved name is.
static void action_install_url_file_name(const char* url, char* out, size_t size) {
    const char* start = strstr(url, "://");
    start = start != NULL ? start + 3 : url;

    const char* slash = strrchr(start, '/');
    const char* name = slash != NULL && slash[1] != '\0' ? slash + 1 : start;
    size_t len = strcspn(name, "?#");

    size_t used = 0;
    for(size_t i = 0; i < len && used < size - 1; i++) {
        unsigned hex = 0;
        if(name[i] == '%' && i + 2 < len && sscanf(name + i + 1, "%2x", &hex) == 1 && hex >= 0x20) {
            out[used++] = (char) hex;
            i += 2;
        } else {
            out[used++] = name[i] == '+' ? ' ' : name[i];
        }
    }

    out[used] = '\0';
}

// "192.168.1.143" from "http://192.168.1.143:8080/game.cia".
static void action_install_url_host(const char* url, char* out, size_t size) {
    const char* start = strstr(url, "://");
    start = start != NULL ? start + 3 : url;

    size_t len = strcspn(start, ":/?#");
    if(len >= size) {
        len = size - 1;
    }

    memcpy(out, start, len);
    out[len] = '\0';
}

static u32 action_install_url_count(install_url_data* data, url_state state) {
    u32 count = 0;
    for(u32 i = 0; i < data->installInfo.total; i++) {
        if(data->states[i] == state) {
            count++;
        }
    }

    return count;
}

static void action_install_url_install_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    install_url_data* installData = (install_url_data*) data;

    if(installData->installInfo.finished) {
        ui_pop();
        ui_destroy(view);

        // A cancel has said so already, and so has a single URL that failed.
        u32 installed = action_install_url_count(installData, URL_INSTALLED);
        u32 saved = action_install_url_count(installData, URL_SAVED);
        u32 failed = action_install_url_count(installData, URL_FAILED);
        u32 skipped = action_install_url_count(installData, URL_SKIPPED);
        bool installAfter = installData->toInstall != NULL && installData->installInfo.result != R_APP_CANCELLED;
        if(installData->installInfo.result != R_APP_CANCELLED) {
            if(failed == 0 && skipped == 0) {
                if(saved > 0 && installAfter) {
                    // The install says when it is done.
                } else if(saved > 0) {
                    char text[FILE_PATH_MAX + 64];
                    if(saved == 1) {
                        snprintf(text, sizeof(text), _("Saved in %s"), installData->currPath);
                    } else {
                        snprintf(text, sizeof(text), _("%lu files saved in %s"), saved, INSTALL_URL_SAVE_DIR);
                    }

                    prompt_display_notify("Success", text, COLOR_TEXT, NULL, NULL, NULL);
                } else if(installed > 0) {
                    prompt_display_notify("Success", "Install finished.", COLOR_TEXT, NULL, NULL, NULL);
                }
            } else if(installData->installInfo.total > 1) {
                char text[128];
                if(installData->saveOnly) {
                    snprintf(text, sizeof(text), _("Saved: %lu\nFailed: %lu\nSkipped: %lu"), saved, failed, skipped);
                } else {
                    snprintf(text, sizeof(text), _("Installed: %lu\nFailed: %lu\nSkipped: %lu"), installed, failed, skipped);
                }
                prompt_display_notify(failed > 0 ? "Failure" : "Success", text, COLOR_TEXT, NULL, NULL, NULL);
            }
        }

        char* paths = installAfter ? installData->toInstall : NULL;
        bool delete = installData->mode == INSTALL_URL_CHOICE_SAVE_INSTALL_DELETE;
        if(paths != NULL) {
            installData->toInstall = NULL;
        }

        action_install_url_free_data(installData);

        if(paths != NULL) {
            action_install_cia_paths_now(paths, delete);
            free(paths);
        }

        return;
    }

    if(hidKeysDown() & KEY_B) {
        svcSignalEvent(installData->installInfo.cancelEvent);
    }
}

static void action_install_url_install_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    install_url_data* installData = (install_url_data*) data;
    const theme_current* t = theme();
    data_op_data* info = &installData->installInfo;
    if(info->total == 0) {
        return;
    }

    u32 index = info->processed < info->total ? info->processed : info->total - 1;

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = 100;
    widget_card(cardX, cardY, cardW, cardH);

    float cx = cardX + 14;
    float right = cardX + cardW - 14;
    float y = cardY + 12;

    char name[FILE_NAME_MAX];
    action_install_url_file_name(installData->urls[index], name, sizeof(name));
    screen_draw_icon(ICON_PACKAGE, cx, y + 1, 16, t->accent);
    widget_text_fit(name, cx + 22, y, WIDGET_TITLE_SCALE, t->palette->text, right - cx - 22);
    y += widget_text_height(WIDGET_TITLE_SCALE) + 4;

    char host[64];
    action_install_url_host(installData->urls[index], host, sizeof(host));
    char line[160];
    // As 3dsDL does: how many connections the file comes over.
    if(info->connections > 1) {
        snprintf(line, sizeof(line), _("From %s, %lu connections"), host, info->connections);
    } else if(info->connections == 1) {
        snprintf(line, sizeof(line), _("From %s, 1 connection"), host);
    } else {
        snprintf(line, sizeof(line), _("From %s"), host);
    }
    widget_text_fit(line, cx, y, WIDGET_TEXT_SCALE, t->palette->textMuted, right - cx);
    y += widget_text_height(WIDGET_TEXT_SCALE) + 10;

    float progress = info->currTotal != 0 ? (float) ((double) info->currProcessed / (double) info->currTotal) : 0;
    widget_progress(cx, y, right - cx, 8, progress);
    y += 8 + 8;

    snprintf(line, sizeof(line), _("%.2f %s / %.2f %s, %.2f %s/s, ETA %s"),
             ui_get_display_size(info->currProcessed), ui_get_display_size_units(info->currProcessed),
             ui_get_display_size(info->currTotal), ui_get_display_size_units(info->currTotal),
             ui_get_display_size(info->bytesPerSecond), ui_get_display_size_units(info->bytesPerSecond),
             ui_get_display_eta(info->estimatedRemainingSeconds));
    widget_text_fit(line, cx, y, WIDGET_SMALL_SCALE, t->palette->textMuted, right - cx);
    y += widget_text_height(WIDGET_SMALL_SCALE) + 8;

    if(info->total > 1) {
        snprintf(line, sizeof(line), _("%lu of %lu"), index + 1, info->total);
        widget_text_fit(line, cx, y, WIDGET_SMALL_SCALE, t->palette->text, right - cx);
    }

    widget_storage(SYSTEM_MEDIATYPE_SD, cardX + 4, y2 - 52, cardW - 8);
}

#define INSTALL_ROW_HEIGHT 28
#define INSTALL_ROW_GAP 4

static void action_install_url_install_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    install_url_data* installData = (install_url_data*) data;
    const theme_current* t = theme();
    data_op_data* info = &installData->installInfo;

    float x = x1 + 8;
    float w = x2 - x1 - 16;
    u32 fits = (u32) ((y2 - y1 - 8) / (INSTALL_ROW_HEIGHT + INSTALL_ROW_GAP));
    if(fits == 0) {
        return;
    }

    // Keep the current URL in view.
    u32 first = info->processed >= fits ? info->processed - fits + 1 : 0;
    if(first + fits > info->total) {
        first = info->total > fits ? info->total - fits : 0;
    }

    for(u32 i = first; i < info->total && i < first + fits; i++) {
        float y = y1 + 8 + (i - first) * (INSTALL_ROW_HEIGHT + INSTALL_ROW_GAP);
        bool current = i == info->processed && !info->finished;
        screen_frame_round_rect(x, y, w, INSTALL_ROW_HEIGHT, t->def->radiusSmall, t->def->border, current ? t->accent : t->palette->border, t->palette->surface);

        u32 icon = ICON_CLOCK;
        u32 iconColor = t->palette->textMuted;
        char status[32];
        if(current) {
            icon = ICON_DOWNLOAD;
            iconColor = t->accent;
            u32 hundredths = info->currTotal != 0 ? (u32) ((u64) info->currProcessed * 10000 / info->currTotal) : 0;
            snprintf(status, sizeof(status), "%lu.%02lu %%", hundredths / 100, hundredths % 100);
        } else if(installData->states[i] == URL_INSTALLED || installData->states[i] == URL_SAVED) {
            icon = ICON_CHECK;
            iconColor = t->palette->success;
            string_copy(status, installData->states[i] == URL_SAVED ? _("Saved") : _("Installed"), sizeof(status));
        } else if(installData->states[i] == URL_FAILED) {
            icon = ICON_X;
            iconColor = t->palette->danger;
            string_copy(status, _("Failed"), sizeof(status));
        } else if(installData->states[i] == URL_SKIPPED) {
            icon = ICON_X;
            string_copy(status, _("Skipped"), sizeof(status));
        } else {
            string_copy(status, _("Queued"), sizeof(status));
        }

        screen_draw_icon(icon, x + 7, y + (INSTALL_ROW_HEIGHT - 16) / 2, 16, iconColor);

        float statusW = widget_text_width(status, WIDGET_SMALL_SCALE);
        screen_draw_text(status, x + w - 8 - statusW, y + (INSTALL_ROW_HEIGHT - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, iconColor == t->palette->textMuted ? t->palette->textMuted : iconColor);

        char name[FILE_NAME_MAX];
        action_install_url_file_name(installData->urls[i], name, sizeof(name));
        float tx = x + 29;
        widget_text_fit(name, tx, y + (INSTALL_ROW_HEIGHT - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, t->palette->text, x + w - 16 - statusW - tx);
    }
}

static void action_install_url_install_show(install_url_data* data) {
    ui_view* view = ui_create();
    view->name = data->saveOnly ? _("Downloading") : _("Installing");
    view->id = data->saveOnly ? "Downloading" : "Installing";
    view->info = _(" cancel");
    view->data = data;
    view->update = action_install_url_install_update;
    view->drawTop = action_install_url_install_draw_top;
    view->drawBottom = action_install_url_install_draw_bottom;
    ui_push(view);
}

static bool action_install_url_in_order = true;

void action_install_url_save_in_order(bool inOrder) {
    action_install_url_in_order = inOrder;
}

static void action_install_url_confirm_onresponse(ui_view* view, void* data, u32 response) {
    install_url_data* installData = (install_url_data*) data;

    installData->mode = response;
    installData->saveOnly = response != INSTALL_URL_CHOICE_INSTALL;
    // A file is saved in order, over the connections that pay off (as an install), on every model: long pieces written
    // where they go (http_download_file) made the SD card take seconds for each write once its cache was full (2DS: from
    // the start, 3.2-3.9 s for 256 KB; New 3DS XL: after ~400 MB of Google Drive, 54 ms -> 4 s), as cards do with several
    // places written at once. SITE SAVEORDER ANY (DEBUG) still tries the long pieces.
    installData->installInfo.dstAnyOrder = installData->saveOnly && !action_install_url_in_order;
    if(response < INSTALL_URL_CHOICES) {
        Result res = task_data_op(&installData->installInfo);
        if(R_SUCCEEDED(res)) {
            action_install_url_install_show(installData);
        } else {
            error_display_res(NULL, NULL, res, "Failed to initiate installation.");

            action_install_url_free_data(installData);
        }
    } else {
        action_install_url_free_data(installData);
    }
}

static void action_install_url_choose_clear(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static void action_install_url_choose_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    install_url_data* installData = (install_url_data*) data;

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < INSTALL_URL_CHOICES; i++) {
            list_item* item = (list_item*) calloc(1, sizeof(list_item));
            if(item != NULL) {
                string_copy(item->name, _(action_install_url_choice_names[i]), sizeof(item->name));
                item->color = COLOR_TEXT;
                item->data = (void*) (uintptr_t) i;
                linked_list_add(items, item);
            }
        }

        return;
    }

    // The web page can answer too: the same choices, and Cancel last.
    char webText[512];
    int webLen = snprintf(webText, sizeof(webText), "%s", installData->confirm);
    u32 shown = installData->installInfo.total < 5 ? installData->installInfo.total : 5;
    for(u32 i = 0; i < shown && webLen > 0 && (size_t) webLen < sizeof(webText); i++) {
        char name[FILE_NAME_MAX];
        action_install_url_file_name(installData->urls[i], name, sizeof(name));
        webLen += snprintf(webText + webLen, sizeof(webText) - webLen, "\n%s", name);
    }

    if(installData->installInfo.total > shown && webLen > 0 && (size_t) webLen < sizeof(webText)) {
        snprintf(webText + webLen, sizeof(webText) - webLen, "\n");
        webLen = (int) strlen(webText);
        snprintf(webText + webLen, sizeof(webText) - webLen, _("and %lu more"), installData->installInfo.total - shown);
    }

    char webNames[INSTALL_URL_CHOICES + 1][PROMPT_OPTION_TEXT_MAX];
    const char* webOptions[INSTALL_URL_CHOICES + 1];
    for(u32 i = 0; i < INSTALL_URL_CHOICES; i++) {
        string_copy(webNames[i], _(action_install_url_choice_names[i]), sizeof(webNames[i]));
        webOptions[i] = webNames[i];
    }

    string_copy(webNames[INSTALL_URL_CHOICES], _("Cancel"), sizeof(webNames[INSTALL_URL_CHOICES]));
    webOptions[INSTALL_URL_CHOICES] = webNames[INSTALL_URL_CHOICES];
    int webChoice = prompt_web_offer(installData, _("Install from URL"), webText, webOptions, INSTALL_URL_CHOICES + 1, false);

    // X saves at once, as the confirmation did (and the tests press).
    u32 choice = UINT32_MAX;
    u32 down = hidKeysDown();
    if(webChoice >= 0 && webChoice <= INSTALL_URL_CHOICE_CANCEL) {
        choice = (u32) webChoice;
    } else if(down & KEY_B) {
        choice = INSTALL_URL_CHOICE_CANCEL;
    } else if(down & KEY_X) {
        choice = INSTALL_URL_CHOICE_SAVE;
    } else if(selected != NULL && (selectedTouched || (down & KEY_A))) {
        choice = (u32) (uintptr_t) selected->data;
    }

    if(choice == UINT32_MAX) {
        return;
    }

    prompt_web_forget(installData);
    ui_pop();
    action_install_url_choose_clear(items);
    list_destroy(view);
    action_install_url_confirm_onresponse(NULL, installData, choice);
}

static void action_install_url_choose_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    install_url_data* installData = (install_url_data*) data;
    const theme_current* t = theme();

    // The question and what comes.
    float x = x1 + 12;
    float width = x2 - x1 - 24;
    float y = y1 + 10;
    float w = 0;
    float h = 0;
    screen_get_string_size_wrap(&w, &h, installData->confirm, 0.5f, 0.5f, width);
    screen_draw_string_wrap(installData->confirm, x, y, 0.5f, 0.5f, COLOR_TEXT, false, x + width);
    y += h + 6;

    // Each name whole, over as many lines as it needs; one address is shown whole too (decoded).
    u32 shown = installData->installInfo.total < 3 ? installData->installInfo.total : 3;
    for(u32 i = 0; i < shown; i++) {
        char name[FILE_NAME_MAX];
        action_install_url_file_name(installData->urls[i], name, sizeof(name));
        screen_draw_icon(ICON_PACKAGE, x, y + 1, 14, t->accent);
        float nameW = 0;
        float nameH = 0;
        screen_get_string_size_wrap(&nameW, &nameH, name, 0.5f, 0.5f, width - 20);
        screen_draw_text_wrap(name, x + 20, y, 0.5f, t->palette->text, x + width);
        y += nameH + 3;
    }

    if(installData->installInfo.total > shown) {
        char more[64];
        snprintf(more, sizeof(more), _("and %lu more"), installData->installInfo.total - shown);
        widget_text_fit(more, x + 20, y, 0.5f, t->palette->textMuted, width - 20);
        y += widget_text_height(0.5f) + 3;
    } else if(installData->installInfo.total == 1) {
        char address[DOWNLOAD_URL_MAX];
        string_url_decode(address, installData->urls[0], sizeof(address));
        float addressW = 0;
        float addressH = 0;
        screen_get_string_size_wrap(&addressW, &addressH, address, 0.4f, 0.4f, width);
        // Only while the card under it keeps room for its text.
        if(y + addressH + 4 + 70 <= y2) {
            screen_draw_text_wrap(address, x, y, 0.4f, t->palette->textMuted, x + width);
            y += addressH + 4;
        }
    }

    // What the chosen way does.
    if(selected == NULL) {
        return;
    }

    u32 choice = (u32) (uintptr_t) selected->data;
    if(choice >= INSTALL_URL_CHOICES) {
        return;
    }

    float cardY = y + 6;
    float cardH = y2 - 8 - cardY;
    widget_card(x1 + 8, cardY, x2 - x1 - 16, cardH);
    widget_text_fit(_(action_install_url_choice_names[choice]), x + 4, cardY + 8, WIDGET_TITLE_SCALE, t->accent, width - 8);
    screen_draw_string_wrap(_(action_install_url_choice_infos[choice]), x + 4, cardY + 10 + widget_text_height(WIDGET_TITLE_SCALE) + 4, 0.5f, 0.5f,
                            COLOR_TEXT, false, x + width - 4);
}

void action_install_url_choice(const char* confirmMessage, const char* urls, const char* paths, void* userData,
                               void (*finishedURL)(void* data, u32 index),
                               void (*finishedAll)(void* data),
                               void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index),
                               int choice) {
    install_url_data* data = (install_url_data*) calloc(1, sizeof(install_url_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate URL install data.");

        return;
    }

    data->installInfo.total = 0;

    size_t payloadLen = strlen(urls);
    if(payloadLen > 0) {
        const char* currStart = urls;
        while(data->installInfo.total < INSTALL_URLS_MAX && currStart - urls < payloadLen) {
            const char* currEnd = strchr(currStart, '\n');
            if(currEnd == NULL) {
                currEnd = urls + payloadLen;
            }

            u32 len = currEnd - currStart;

            if((len < 7 || strncmp(currStart, "http://", 7) != 0) && (len < 8 || strncmp(currStart, "https://", 8) != 0)) {
                if(len > DOWNLOAD_URL_MAX - 8) {
                    len = DOWNLOAD_URL_MAX - 8;
                }

                string_copy(data->urls[data->installInfo.total], "http://", 8);
                string_copy(&data->urls[data->installInfo.total][7], currStart, len + 1);
            } else {
                if(len > DOWNLOAD_URL_MAX - 1) {
                    len = DOWNLOAD_URL_MAX - 1;
                }

                string_copy(data->urls[data->installInfo.total], currStart, len + 1);
            }

            data->installInfo.total++;
            currStart = currEnd + 1;
        }
    }

    if(paths != NULL) {
        size_t pathsLen = strlen(paths);
        if(pathsLen > 0) {
            const char* currStart = paths;
            for(u32 i = 0; i < data->installInfo.total && currStart - paths < pathsLen; i++) {
                const char* currEnd = strchr(currStart, '\n');
                if(currEnd == NULL) {
                    currEnd = paths + pathsLen;
                }

                u32 len = currEnd - currStart;
                if(len > FILE_PATH_MAX - 1) {
                    len = FILE_PATH_MAX - 1;
                }

                string_copy(data->paths[i], currStart, len + 1);

                currStart = currEnd + 1;
            }
        }
    }

    data->userData = userData;
    data->finishedURL = finishedURL;
    data->finishedAll = finishedAll;
    data->drawTop = drawTop;

    data->contentType = CONTENT_CIA;
    data->currTitleId = 0;
    data->n3dsContinue = false;
    memset(&data->ticketInfo, 0, sizeof(data->ticketInfo));
    memset(&data->currPath, 0, sizeof(data->currPath));

    data->installInfo.data = data;

    data->installInfo.op = DATAOP_DOWNLOAD;

    data->installInfo.bufferSize = 128 * 1024;

    data->installInfo.processed = data->installInfo.total;

    data->installInfo.getSrcUrl = action_install_url_get_src_url;

    data->installInfo.openDst = action_install_url_open_dst;
    data->installInfo.closeDst = action_install_url_close_dst;
    data->installInfo.writeDst = action_install_url_write_dst;

    data->installInfo.suspend = action_install_url_suspend;
    data->installInfo.restore = action_install_url_restore;

    data->installInfo.error = action_install_url_error;

    data->installInfo.finished = true;

    // A choice made by the caller (a plain download, say): no question.
    if(choice >= 0) {
        action_install_url_confirm_onresponse(NULL, data, (u32) choice);
        return;
    }

    // No message: the caller already asked (the updater shows its own prompt).
    if(confirmMessage == NULL) {
        action_install_url_confirm_onresponse(NULL, data, INSTALL_URL_CHOICE_INSTALL);
        return;
    }

    // Install, save, or both (issues #22 and #29): what each one does on the top screen.
    string_copy(data->confirm, i18n_translate_text(confirmMessage), sizeof(data->confirm));
    ui_view* view = list_display(_("Install from URL"), _(" choose    save    cancel"), data, action_install_url_choose_update,
                                 action_install_url_choose_draw_top);
    if(view == NULL) {
        action_install_url_free_data(data);
        return;
    }

    view->id = "Confirmation";
}

void action_install_url(const char* confirmMessage, const char* urls, const char* paths, void* userData,
                        void (*finishedURL)(void* data, u32 index),
                        void (*finishedAll)(void* data),
                        void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index)) {
    action_install_url_choice(confirmMessage, urls, paths, userData, finishedURL, finishedAll, drawTop, -1);
}

// Only downloads the file(s) to INSTALL_URL_SAVE_DIR, to install later from Files: no question first.
void action_save_url(const char* urls, void* userData, void (*finishedURL)(void* data, u32 index), void (*finishedAll)(void* data)) {
    action_install_url_choice(NULL, urls, NULL, userData, finishedURL, finishedAll, NULL, INSTALL_URL_CHOICE_SAVE);
}
