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
    CONTENT_3DSX_SMDH
} content_type;

// What became of each URL, for the list on the install screen.
typedef enum {
    URL_QUEUED,
    URL_INSTALLED,
    URL_FAILED,
    URL_SKIPPED
} url_state;

typedef struct {
    char urls[INSTALL_URLS_MAX][DOWNLOAD_URL_MAX];
    u8 states[INSTALL_URLS_MAX];

    char paths[INSTALL_URLS_MAX][FILE_PATH_MAX];

    void* userData;
    void (*finishedURL)(void* data, u32 index);
    void (*finishedAll)(void* data);
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index);

    content_type contentType;
    u64 currTitleId;
    volatile bool n3dsContinue;
    ticket_info ticketInfo;
    char currPath[FILE_PATH_MAX];

    data_op_data installInfo;
} install_url_data;

static void action_install_url_free_data(install_url_data* data) {
    if(data->finishedAll != NULL) {
        data->finishedAll(data->userData);
    }

    free(data);
}

static void action_install_url_install_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2);

static void action_install_url_n3ds_onresponse(ui_view* view, void* data, u32 response) {
    ((install_url_data*) data)->n3dsContinue = response == PROMPT_YES;
}

static void action_install_url_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    install_url_data* installData = (install_url_data*) data;

    if(installData->drawTop != NULL) {
        installData->drawTop(view, installData->userData, x1, y1, x2, y2, installData->installInfo.processed);
    } else if(installData->installInfo.processed == installData->installInfo.total) {
        float urlY = y1 + 5;
        u32 index = 0;
        while(urlY < y2 && index < installData->installInfo.total) {
            float urlWidth = 0;
            float urlHeight = 0;
            screen_get_string_size_wrap(&urlWidth, &urlHeight, installData->urls[index], 0.5f, 0.5f, x2 - x1 - 10);

            float urlX = x1 + (x2 - x1 - urlWidth) / 2;
            screen_draw_string_wrap(installData->urls[index], urlX, urlY, 0.5f, 0.5f, COLOR_TEXT, true, urlX + urlWidth + 1);

            urlY += urlHeight;
            index++;
        }
    } else {
        float urlWidth = 0;
        float urlHeight = 0;
        screen_get_string_size_wrap(&urlWidth, &urlHeight, installData->urls[installData->installInfo.processed], 0.5f, 0.5f, x2 - x1 - 10);

        float urlX = x1 + (x2 - x1 - urlWidth) / 2;
        float urlY = y1 + (y2 - y1 - urlHeight) / 2;
        screen_draw_string_wrap(installData->urls[installData->installInfo.processed], urlX, urlY, 0.5f, 0.5f, COLOR_TEXT, true, urlX + urlWidth + 1);
    }
}

static Result action_install_url_get_src_url(void* data, u32 index, char* url, size_t maxSize) {
    install_url_data* installData = (install_url_data*) data;

    strncpy(url, installData->urls[index], maxSize);
    return 0;
}

static Result action_install_url_open_dst(void* data, u32 index, void* initialReadBlock, u64 size, u32* handle) {
    install_url_data* installData = (install_url_data*) data;

    Result res = 0;

    installData->contentType = CONTENT_CIA;
    installData->currTitleId = 0;
    installData->n3dsContinue = false;
    memset(&installData->ticketInfo, 0, sizeof(installData->ticketInfo));
    memset(&installData->currPath, 0, sizeof(installData->currPath));

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
        } else if(installData->contentType == CONTENT_3DSX_SMDH) {
            res = FSFILE_Close(handle);
        }
    } else {
        if(installData->contentType == CONTENT_CIA) {
            res = AM_CancelCIAInstall(handle);
        } else if(installData->contentType == CONTENT_TICKET) {
            res = AMNET_InstallTicketAbort(handle);
        } else if(installData->contentType == CONTENT_3DSX_SMDH) {
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
        installData->states[index] = URL_INSTALLED;
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
    if(strlen(url) > 38) {
        *errorView = error_display_res(data, action_install_url_install_draw_top, res, "Failed to install from URL.\n%.35s...", url);
    } else {
        *errorView = error_display_res(data, action_install_url_install_draw_top, res, "Failed to install from URL.\n%.38s", url);
    }

    return true;
}

// ---- the install screen: the current URL's progress on top, every URL below ----

// "game.cia" from "http://host/dir/game.cia?x=1".
static void action_install_url_file_name(const char* url, char* out, size_t size) {
    const char* start = strstr(url, "://");
    start = start != NULL ? start + 3 : url;

    const char* slash = strrchr(start, '/');
    const char* name = slash != NULL && slash[1] != '\0' ? slash + 1 : start;
    size_t len = strcspn(name, "?#");
    if(len >= size) {
        len = size - 1;
    }

    memcpy(out, name, len);
    out[len] = '\0';
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
        u32 failed = action_install_url_count(installData, URL_FAILED);
        u32 skipped = action_install_url_count(installData, URL_SKIPPED);
        if(installData->installInfo.result != R_APP_CANCELLED) {
            if(failed == 0 && skipped == 0) {
                if(installed > 0) {
                    prompt_display_notify("Success", "Install finished.", COLOR_TEXT, NULL, NULL, NULL);
                }
            } else if(installData->installInfo.total > 1) {
                char text[128];
                snprintf(text, sizeof(text), _("Installed: %lu\nFailed: %lu\nSkipped: %lu"), installed, failed, skipped);
                prompt_display_notify(failed > 0 ? "Failure" : "Success", text, COLOR_TEXT, NULL, NULL, NULL);
            }
        }

        action_install_url_free_data(installData);
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
    snprintf(line, sizeof(line), _("From %s"), host);
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
            snprintf(status, sizeof(status), "%d %%", info->currTotal != 0 ? (int) (info->currProcessed * 100 / info->currTotal) : 0);
        } else if(installData->states[i] == URL_INSTALLED) {
            icon = ICON_CHECK;
            iconColor = t->palette->success;
            string_copy(status, _("Installed"), sizeof(status));
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
    view->name = _("Installing");
    view->id = "Installing";
    view->info = _(" cancel");
    view->data = data;
    view->update = action_install_url_install_update;
    view->drawTop = action_install_url_install_draw_top;
    view->drawBottom = action_install_url_install_draw_bottom;
    ui_push(view);
}

static void action_install_url_confirm_onresponse(ui_view* view, void* data, u32 response) {
    install_url_data* installData = (install_url_data*) data;

    if(response == PROMPT_YES) {
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

void action_install_url(const char* confirmMessage, const char* urls, const char* paths, void* userData,
                        void (*finishedURL)(void* data, u32 index),
                        void (*finishedAll)(void* data),
                        void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index)) {
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

    // No message: the caller already asked (the updater shows its own prompt).
    if(confirmMessage == NULL) {
        action_install_url_confirm_onresponse(NULL, data, PROMPT_YES);
        return;
    }

    prompt_display_yes_no("Confirmation", confirmMessage, COLOR_TEXT, data, action_install_url_draw_top, action_install_url_confirm_onresponse);
}