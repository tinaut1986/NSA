#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "ftpserver.h"
#include "transfer.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "../libs/quirc/quirc_internal.h"

static bool remoteinstall_get_last_urls(char* out, size_t size) {
    if(out == NULL || size == 0) {
        return false;
    }

    Handle file = 0;
    if(R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, APP_DATA_DIR "lasturls"), FS_OPEN_READ, 0))) {
        return false;
    }

    u32 bytesRead = 0;
    FSFILE_Read(file, &bytesRead, 0, out, size - 1);
    out[bytesRead] = '\0';

    FSFILE_Close(file);

    return bytesRead != 0;
}

static Result remoteinstall_set_last_urls(const char* urls) {
    Result res = 0;

    FS_Archive sdmcArchive = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        FS_Path path = fsMakePath(PATH_ASCII, APP_DATA_DIR "lasturls");

        Handle file = 0;
        if(R_SUCCEEDED(FSUSER_OpenFile(&file, sdmcArchive, path, FS_OPEN_READ, 0))) {
            FSFILE_Close(file);

            res = FSUSER_DeleteFile(sdmcArchive, path);
        }

        if(urls != NULL && strlen(urls) != 0
           && R_SUCCEEDED(res)
           && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, APP_DATA_DIR))
           && R_SUCCEEDED(res = FSUSER_OpenFile(&file, sdmcArchive, path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
            u32 bytesWritten = 0;
            res = FSFILE_Write(file, &bytesWritten, 0, urls, strlen(urls), FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME);

            Result closeRes = FSFILE_Close(file);
            if(R_SUCCEEDED(res)) {
                res = closeRes;
            }
        }

        Result closeRes = FSUSER_CloseArchive(sdmcArchive);
        if(R_SUCCEEDED(res)) {
            res = closeRes;
        }
    }

    return res;
}

typedef struct {
    int serverSocket;
    int clientSocket;
} remoteinstall_network_data;

static int remoteinstall_network_recvwait(int sockfd, void* buf, size_t len, int flags) {
    errno = 0;

    int ret = 0;
    size_t read = 0;
    while((((ret = recv(sockfd, buf + read, len - read, flags)) > 0 && (read += ret) < len) || errno == EAGAIN) && !(hidKeysDown() & KEY_B)) {
        errno = 0;
    }

    return ret < 0 ? ret : (int) read;
}

static int remoteinstall_network_sendwait(int sockfd, void* buf, size_t len, int flags) {
    errno = 0;

    int ret = 0;
    size_t written = 0;
    while((((ret = send(sockfd, buf + written, len - written, flags)) > 0 && (written += ret) < len) || errno == EAGAIN) && !(hidKeysDown() & KEY_B)) {
        errno = 0;
    }

    return ret < 0 ? ret : (int) written;
}

static void remoteinstall_network_close_client(void* data) {
    remoteinstall_network_data* networkData = (remoteinstall_network_data*) data;

    if(networkData->clientSocket != 0) {
        u8 ack = 0;
        remoteinstall_network_sendwait(networkData->clientSocket, &ack, sizeof(ack), 0);

        close(networkData->clientSocket);
        networkData->clientSocket = 0;
    }
}


static void remoteinstall_network_free_data(remoteinstall_network_data* data) {
    remoteinstall_network_close_client(data);

    if(data->serverSocket != 0) {
        close(data->serverSocket);
        data->serverSocket = 0;
    }

    free(data);
}

static void remoteinstall_network_update(ui_view* view, void* data, float* progress, char* text) {
    remoteinstall_network_data* networkData = (remoteinstall_network_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        info_destroy(view);

        remoteinstall_network_free_data(networkData);

        return;
    }

    struct sockaddr_in client;
    socklen_t clientLen = sizeof(client);

    int sock = accept(networkData->serverSocket, (struct sockaddr*) &client, &clientLen);
    if(sock >= 0) {
        networkData->clientSocket = sock;

        u32 size = 0;
        if(remoteinstall_network_recvwait(networkData->clientSocket, &size, sizeof(size), 0) != sizeof(size)) {
            error_display_errno(NULL, NULL, errno, "Failed to read payload length.");

            remoteinstall_network_close_client(data);
            return;
        }

        size = ntohl(size);
        if(size >= DOWNLOAD_URL_MAX * INSTALL_URLS_MAX) {
            error_display(NULL, NULL, "Payload too large.");

            remoteinstall_network_close_client(data);
            return;
        }

        char* urls = (char*) calloc(size + 1, sizeof(char));
        if(urls == NULL) {
            error_display(NULL, NULL, "Failed to allocate URL buffer.");

            remoteinstall_network_close_client(data);
            return;
        }

        if(remoteinstall_network_recvwait(networkData->clientSocket, urls, size, 0) != size) {
            error_display_errno(NULL, NULL, errno, "Failed to read URL(s).");

            free(urls);
            remoteinstall_network_close_client(data);
            return;
        }

        remoteinstall_set_last_urls(urls);
        action_install_url("Install from the received URL(s)?", urls, NULL, data, NULL, remoteinstall_network_close_client, NULL);

        free(urls);
    } else if(errno != EAGAIN) {
        if(errno == 22 || errno == 115) {
            ui_pop();
            info_destroy(view);
        }

        error_display_errno(NULL, NULL, errno, "Failed to open socket.");

        if(errno == 22 || errno == 115) {
            remoteinstall_network_free_data(networkData);

            return;
        }
    }

    snprintf(text, PROGRESS_TEXT_MAX, "%s", _("Waiting for the computer...\nWhat it sends installs from here."));
}

// The address to send to, large, and how.
static void remoteinstall_network_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    const theme_current* t = theme();

    float cardX = x1 + 12;
    float cardW = x2 - x1 - 24;
    float cardY = y1 + 14;
    float cardH = 100;
    widget_card(cardX, cardY, cardW, cardH);

    widget_text_centered(_("Send from your computer to"), cardX, cardX + cardW, cardY + 12, WIDGET_TEXT_SCALE, t->palette->textMuted);

    struct in_addr addr = {(in_addr_t) gethostid()};
    char ip[32];
    snprintf(ip, sizeof(ip), "%s", inet_ntoa(addr));
    const char* port = ":5000";
    float scale = 1.1f;
    float ipW = widget_text_width(ip, scale);
    float width = ipW + widget_text_width(port, scale);
    float textX = cardX + (cardW - width) / 2;
    float textY = cardY + 12 + widget_text_height(WIDGET_TEXT_SCALE) + 8;
    screen_draw_text(ip, textX, textY, scale, t->palette->text);
    screen_draw_text(port, textX + ipW, textY, scale, t->accent);

    if(ftpserver_running()) {
        widget_text_centered(_("The FTP server keeps running on port 5021."), cardX, cardX + cardW, cardY + cardH - 12 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, t->palette->textMuted);
    }

    float howY = cardY + cardH + 10;
    float howH = 52;
    widget_card(cardX, howY, cardW, howH);
    screen_draw_text_wrap(_("With servefiles.py, or any tool made for FBI:"), cardX + 14, howY + 10, WIDGET_SMALL_SCALE, t->palette->textMuted, cardX + cardW - 14);

    char command[96];
    snprintf(command, sizeof(command), "python servefiles.py %s game.cia", ip);
    widget_text_fit(command, cardX + 14, howY + 12 + widget_text_height(WIDGET_SMALL_SCALE) + 4, WIDGET_TEXT_SCALE, t->palette->text, cardW - 28);
}

static void remoteinstall_receive_urls_network() {
    remoteinstall_network_data* data = (remoteinstall_network_data*) calloc(1, sizeof(remoteinstall_network_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate network install data.");
        return;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(sock < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to open server socket.");

        remoteinstall_network_free_data(data);
        return;
    }

    data->serverSocket = sock;

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(5000);
    server.sin_addr.s_addr = (in_addr_t) gethostid();

    if(bind(data->serverSocket, (struct sockaddr*) &server, sizeof(server)) < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to bind server socket.");

        remoteinstall_network_free_data(data);
        return;
    }

    fcntl(data->serverSocket, F_SETFL, fcntl(data->serverSocket, F_GETFL, 0) | O_NONBLOCK);

    if(listen(data->serverSocket, 5) < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to listen on server socket.");

        remoteinstall_network_free_data(data);
        return;
    }

    info_display("Receive from a computer", "\uE001 back", false, data, remoteinstall_network_update, remoteinstall_network_draw_top);
}

#define QR_IMAGE_WIDTH 400
#define QR_IMAGE_HEIGHT 240

typedef struct {
    struct quirc* qrContext;
    u32 tex;

    bool capturing;
    capture_cam_data captureInfo;
} remoteinstall_qr_data;

static void remoteinstall_qr_stop_capture(remoteinstall_qr_data* data) {
    if(!data->captureInfo.finished) {
        svcSignalEvent(data->captureInfo.cancelEvent);
        while(!data->captureInfo.finished) {
            svcSleepThread(1000000);
        }
    }

    data->capturing = false;

    if(data->captureInfo.buffer != NULL) {
        memset(data->captureInfo.buffer, 0, QR_IMAGE_WIDTH * QR_IMAGE_HEIGHT * sizeof(u16));
    }
}

static void remoteinstall_qr_free_data(remoteinstall_qr_data* data) {
    remoteinstall_qr_stop_capture(data);

    if(data->captureInfo.buffer != NULL) {
        free(data->captureInfo.buffer);
        data->captureInfo.buffer = NULL;
    }

    if(data->tex != 0) {
        screen_unload_texture(data->tex);
        data->tex = 0;
    }

    if(data->qrContext != NULL) {
        quirc_destroy(data->qrContext);
        data->qrContext = NULL;
    }

    free(data);
}

static void remoteinstall_qr_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    remoteinstall_qr_data* installData = (remoteinstall_qr_data*) data;

    if(installData->tex != 0) {
        svcWaitSynchronization(installData->captureInfo.mutex, U64_MAX);
        screen_load_texture_untiled(installData->tex, installData->captureInfo.buffer, QR_IMAGE_WIDTH * QR_IMAGE_HEIGHT * sizeof(u16), QR_IMAGE_WIDTH, QR_IMAGE_HEIGHT, GPU_RGB565, false);
        svcReleaseMutex(installData->captureInfo.mutex);

        screen_draw_texture(installData->tex, 0, 0, QR_IMAGE_WIDTH, QR_IMAGE_HEIGHT);
    }

    // Where to hold the code: its four corners.
    const theme_current* t = theme();
    float size = 150;
    float arm = 24;
    float line = 3;
    float fx = (QR_IMAGE_WIDTH - size) / 2;
    float fy = (QR_IMAGE_HEIGHT - size) / 2;
    float depth = screen_get_depth();
    screen_set_depth(depth + 1);
    for(u32 corner = 0; corner < 4; corner++) {
        float cx = corner % 2 == 0 ? fx : fx + size - arm;
        float cy = corner / 2 == 0 ? fy : fy + size - line;
        float vx = corner % 2 == 0 ? fx : fx + size - line;
        float vy = corner / 2 == 0 ? fy : fy + size - arm;
        screen_fill_round_rect(cx, cy, arm, line, line / 2, t->accent);
        screen_fill_round_rect(vx, vy, line, arm, line / 2, t->accent);
    }
    screen_set_depth(depth);
}

static void remoteinstall_qr_update(ui_view* view, void* data, float* progress, char* text) {
    remoteinstall_qr_data* installData = (remoteinstall_qr_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        info_destroy(view);

        remoteinstall_qr_free_data(installData);

        return;
    }

    if(hidKeysDown() & KEY_X) {
        remoteinstall_qr_stop_capture(installData);

        installData->captureInfo.camera = installData->captureInfo.camera == CAMERA_OUTER ? CAMERA_INNER : CAMERA_OUTER;

        return;
    }

    if(!installData->capturing) {
        Result capRes = task_capture_cam(&installData->captureInfo);
        if(R_FAILED(capRes)) {
            ui_pop();
            info_destroy(view);

            error_display_res(NULL, NULL, capRes, "Failed to start camera capture.");

            remoteinstall_qr_free_data(installData);
            return;
        } else {
            installData->capturing = true;
        }
    }

    if(installData->captureInfo.finished) {
        ui_pop();
        info_destroy(view);

        if(R_FAILED(installData->captureInfo.result)) {
            error_display_res(NULL, NULL, installData->captureInfo.result, "Error while capturing camera frames.");
        }

        remoteinstall_qr_free_data(installData);

        return;
    }

    int w = 0;
    int h = 0;
    uint8_t* qrBuf = quirc_begin(installData->qrContext, &w, &h);

    svcWaitSynchronization(installData->captureInfo.mutex, U64_MAX);

    for(int x = 0; x < w; x++) {
        for(int y = 0; y < h; y++) {
            u16 px = installData->captureInfo.buffer[y * QR_IMAGE_WIDTH + x];
            qrBuf[y * w + x] = (u8) (((((px >> 11) & 0x1F) << 3) + (((px >> 5) & 0x3F) << 2) + ((px & 0x1F) << 3)) / 3);
        }
    }

    svcReleaseMutex(installData->captureInfo.mutex);

    quirc_end(installData->qrContext);

    int qrCount = quirc_count(installData->qrContext);
    for(int i = 0; i < qrCount; i++) {
        struct quirc_code qrCode;
        quirc_extract(installData->qrContext, i, &qrCode);

        struct quirc_data qrData;
        quirc_decode_error_t err = quirc_decode(&qrCode, &qrData);

        if(err == 0) {
            remoteinstall_qr_stop_capture(installData);

            remoteinstall_set_last_urls((const char*) qrData.payload);

            action_install_url("Install from the scanned QR code?", (const char*) qrData.payload, NULL, NULL, NULL, NULL, NULL);
            return;
        }
    }

    snprintf(text, PROGRESS_TEXT_MAX, "%s", _("Point the camera at the code.\nA QR code with one or more links to CIAs."));
}

static void remoteinstall_scan_qr_code() {
    remoteinstall_qr_data* data = (remoteinstall_qr_data*) calloc(1, sizeof(remoteinstall_qr_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate QR install data.");

        return;
    }

    data->tex = 0;

    data->capturing = false;

    data->captureInfo.width = QR_IMAGE_WIDTH;
    data->captureInfo.height = QR_IMAGE_HEIGHT;

    data->captureInfo.camera = CAMERA_OUTER;

    data->captureInfo.finished = true;

    data->qrContext = quirc_new();
    if(data->qrContext == NULL) {
        error_display(NULL, NULL, "Failed to create QR context.");

        remoteinstall_qr_free_data(data);
        return;
    }

    if(quirc_resize(data->qrContext, QR_IMAGE_WIDTH, QR_IMAGE_HEIGHT) != 0) {
        error_display(NULL, NULL, "Failed to resize QR context.");

        remoteinstall_qr_free_data(data);
        return;
    }

    data->captureInfo.buffer = (u16*) calloc(1, QR_IMAGE_WIDTH * QR_IMAGE_HEIGHT * sizeof(u16));
    if(data->captureInfo.buffer == NULL) {
        error_display(NULL, NULL, "Failed to create image buffer.");

        remoteinstall_qr_free_data(data);
        return;
    }

    data->tex = screen_allocate_free_texture();

    info_display("Scan QR code", "\uE002 switch camera   \uE001 back", false, data, remoteinstall_qr_update, remoteinstall_qr_draw_top);
}

static void remoteinstall_manually_enter_urls_onresponse(ui_view* view, void* data, SwkbdButton button, const char* response) {
    if(button == SWKBD_BUTTON_CONFIRM) {
        remoteinstall_set_last_urls(response);

        action_install_url("Install from the entered URL(s)?", response, NULL, NULL, NULL, NULL, NULL);
    }
}

static void remoteinstall_manually_enter_urls() {
    kbd_display("Enter URL(s)", "", SWKBD_TYPE_NORMAL, SWKBD_MULTILINE, SWKBD_NOTEMPTY_NOTBLANK, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX, NULL, remoteinstall_manually_enter_urls_onresponse);
}

static void remoteinstall_repeat_last_request() {
    char* textBuf = (char*) calloc(1, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX);
    if(textBuf != NULL) {
        if(remoteinstall_get_last_urls(textBuf, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX)) {
            action_install_url("Install from the last requested URL(s)?", textBuf, NULL, NULL, NULL, NULL, NULL);
        } else {
            prompt_display_notify("Failure", "No previously requested URL(s) could be found.", COLOR_TEXT, NULL, NULL, NULL);
        }

        free(textBuf);
    } else {
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, "Failed to allocate URL text buffer.");
    }
}

static void remoteinstall_forget_last_request() {
    Result forgetRes = remoteinstall_set_last_urls(NULL);
    if(R_FAILED(forgetRes)) {
        error_display_res(NULL, NULL, forgetRes, "Failed to forget last requested URL(s).");
    }
}

// ---- the home screen: four ways in as tiles, the last request as a row ----

typedef struct {
    u32 icon;
    const char* name;
    const char* description;
    void (*open)();
} remoteinstall_entry;

static const remoteinstall_entry entries[] = {
        {ICON_3DS, "Receive from another 3DS", "Another console running NSA sends you a game or a file, over your Wi-Fi or with no router. Install it or keep it.", transfer_receive_open},
        {ICON_DEVICE_DESKTOP, "Receive from a computer", "Send CIAs from a computer with servefiles.py or any tool made for FBI: they install as they arrive.", remoteinstall_receive_urls_network},
        {ICON_QRCODE, "Scan QR code", "Point the camera at a QR code that holds one or more links to CIAs.", remoteinstall_scan_qr_code},
        {ICON_KEYBOARD, "Enter URLs", "Type one or more links to CIAs, one per line, and install from them.", remoteinstall_manually_enter_urls},
};

#define ENTRY_COUNT (sizeof(entries) / sizeof(*entries))
#define HISTORY_INDEX ENTRY_COUNT   // The last request's row, when there is one.
#define TILE_HEIGHT 60
#define HISTORY_HEIGHT 36
#define GAP 6
#define PAD 8

typedef struct {
    u32 selected;
    u64 nextMove;
    widget_press press;

    // The last request, read again every second (a screen opened from here may change it).
    bool haveLast;
    u32 lastCount;
    char lastHost[64];
    char lastFirst[DOWNLOAD_URL_MAX];
    char lastShown[3][DOWNLOAD_URL_MAX];   // The first few, for the top card.
    u64 lastRead;

    u32 shown;
    u32 leaving;
    u64 shownTime;
} remoteinstall_data;

static void remoteinstall_read_last(remoteinstall_data* data) {
    data->lastRead = osGetTime();
    data->haveLast = false;

    char* text = (char*) calloc(1, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX);
    if(text == NULL) {
        return;
    }

    if(remoteinstall_get_last_urls(text, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX)) {
        data->haveLast = true;
        data->lastCount = 0;
        for(const char* line = text; *line != '\0';) {
            size_t len = strcspn(line, "\n");
            if(len > 0) {
                if(data->lastCount < 3) {
                    char* shown = data->lastShown[data->lastCount];
                    size_t n = len < DOWNLOAD_URL_MAX - 1 ? len : DOWNLOAD_URL_MAX - 1;
                    memcpy(shown, line, n);
                    shown[n] = '\0';
                    if(data->lastCount == 0) {
                        string_copy(data->lastFirst, shown, sizeof(data->lastFirst));
                    }
                }

                data->lastCount++;
            }

            line += len;
            if(*line == '\n') {
                line++;
            }
        }

        const char* host = strstr(data->lastFirst, "://");
        host = host != NULL ? host + 3 : data->lastFirst;
        size_t len = strcspn(host, ":/?#");
        if(len >= sizeof(data->lastHost)) {
            len = sizeof(data->lastHost) - 1;
        }

        memcpy(data->lastHost, host, len);
        data->lastHost[len] = '\0';
    }

    free(text);
}

static void remoteinstall_rect(u32 index, float x1, float y1, float x2, float* x, float* y, float* w, float* h) {
    float area = x2 - x1 - 2 * PAD;
    if(index < ENTRY_COUNT) {
        *w = (area - GAP) / 2;
        *h = TILE_HEIGHT;
        *x = x1 + PAD + (index % 2) * (*w + GAP);
        *y = y1 + PAD + (index / 2) * (TILE_HEIGHT + GAP);
    } else {
        *w = area;
        *h = HISTORY_HEIGHT;
        *x = x1 + PAD;
        *y = y1 + PAD + 2 * (TILE_HEIGHT + GAP);
    }
}

static u32 remoteinstall_targets(remoteinstall_data* data) {
    return ENTRY_COUNT + (data->haveLast ? 1 : 0);
}

// Two by two, the history row under both columns.
static u32 remoteinstall_move(remoteinstall_data* data, u32 selected, u32 key) {
    bool history = data->haveLast;
    if(selected == HISTORY_INDEX) {
        if(key == KEY_UP) {
            return 2;
        } else if(key == KEY_DOWN) {
            return 0;
        }

        return selected;
    }

    switch(key) {
        case KEY_LEFT: return selected % 2 == 1 ? selected - 1 : selected;
        case KEY_RIGHT: return selected % 2 == 0 ? selected + 1 : selected;
        case KEY_UP: return selected >= 2 ? selected - 2 : (history ? HISTORY_INDEX : selected + 2);
        case KEY_DOWN: return selected < 2 ? selected + 2 : (history ? HISTORY_INDEX : selected - 2);
        default: return selected;
    }
}

static bool remoteinstall_repeat(remoteinstall_data* data, u32 key) {
    if(hidKeysDown() & key) {
        data->nextMove = osGetTime() + 400;
        return true;
    }

    if((hidKeysHeld() & key) && osGetTime() >= data->nextMove) {
        data->nextMove = osGetTime() + 120;
        return true;
    }

    return false;
}

static void remoteinstall_sheet_repeat(void* data) {
    remoteinstall_repeat_last_request();
}

static void remoteinstall_sheet_forget(void* data) {
    remoteinstall_forget_last_request();
    ((remoteinstall_data*) data)->lastRead = 0;
}

static void remoteinstall_history_actions(remoteinstall_data* data) {
    static const sheet_action actions[] = {
            {ICON_REFRESH, "Install again", false, remoteinstall_sheet_repeat, false},
            {ICON_TRASH, "Forget", true, remoteinstall_sheet_forget, false},
    };

    sheet_action translated[2];
    for(u32 i = 0; i < 2; i++) {
        translated[i] = actions[i];
        translated[i].label = _(actions[i].label);
    }

    sheet_display(_("Last request"), 0, translated, 2, data);
}

static void remoteinstall_open_target(remoteinstall_data* data, u32 index) {
    if(index < ENTRY_COUNT) {
        entries[index].open();
    } else if(data->haveLast) {
        remoteinstall_repeat_last_request();
    }
}

static void remoteinstall_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    remoteinstall_data* menu = (remoteinstall_data*) data;

    view->name = _("Remote install");
    view->info = menu->haveLast ? _(" open    install the last again    back") : _(" open    back");

    if(menu->lastRead == 0 || osGetTime() - menu->lastRead > 1000) {
        remoteinstall_read_last(menu);
        if(!menu->haveLast && menu->selected == HISTORY_INDEX) {
            menu->selected = 2;
        }
    }

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        free(menu);
        ui_destroy(view);
        return;
    }

    if((hidKeysDown() & KEY_Y) && menu->haveLast) {
        remoteinstall_repeat_last_request();
        return;
    }

    static const u32 keys[] = {KEY_DOWN, KEY_UP, KEY_RIGHT, KEY_LEFT};
    if(menu->press.index < 0) {
        for(u32 i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
            if(remoteinstall_repeat(menu, keys[i])) {
                menu->selected = remoteinstall_move(menu, menu->selected, keys[i]);
                break;
            }
        }
    }

    // A, or a finger: acts on release; a long hold on the last request offers its actions.
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        widget_press_end(&menu->press);
        for(u32 i = 0; i < remoteinstall_targets(menu); i++) {
            float x, y, w, h;
            remoteinstall_rect(i, bx1, by1, bx2, &x, &y, &w, &h);
            if(widget_touched(&pos, x, y, w, h)) {
                widget_press_begin(&menu->press, (int) i, true, pos.px, pos.py);
                menu->selected = i;
            }
        }
    } else if(hidKeysDown() & KEY_A) {
        float x, y, w, h;
        remoteinstall_rect(menu->selected, bx1, by1, bx2, &x, &y, &w, &h);
        widget_press_begin(&menu->press, (int) menu->selected, false, x + w / 2, y + h / 2);
    }

    if(menu->press.index >= 0) {
        bool touch = menu->press.touch;
        bool held = touch ? (hidKeysHeld() & KEY_TOUCH) != 0 : (hidKeysHeld() & KEY_A) != 0;
        if(held && touch && !menu->press.cancelled) {
            float x, y, w, h;
            remoteinstall_rect((u32) menu->press.index, bx1, by1, bx2, &x, &y, &w, &h);
            if(!widget_touched(&pos, x, y, w, h)) {
                menu->press.cancelled = true;
            }
        }

        if(held && menu->press.index == HISTORY_INDEX && widget_press_long_due(&menu->press)) {
            remoteinstall_history_actions(menu);
        } else if(!held) {
            int index = menu->press.index;
            bool open = widget_press_on(&menu->press, index) && !menu->press.fired;
            widget_press_end(&menu->press);
            if(open) {
                remoteinstall_open_target(menu, (u32) index);
            }
        }
    }
}

// A cell under the top card: a label over its value.
static void remoteinstall_draw_cell(float x, float y, float w, const char* label, const char* value) {
    const theme_current* t = theme();
    float h = 30;
    screen_fill_round_rect(x, y, w, h, t->def->radiusSmall, t->palette->surface);
    widget_text_fit(label, x + 8, y + 3, WIDGET_SMALL_SCALE, t->palette->textMuted, w - 16);
    widget_text_fit(value, x + 8, y + 4 + widget_text_height(WIDGET_SMALL_SCALE), WIDGET_TEXT_SCALE, t->palette->text, w - 16);
}

static void remoteinstall_draw_card(remoteinstall_data* menu, u32 index, float x1, float y1, float x2, float y2) {
    const theme_current* t = theme();

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = 96;
    widget_card(cardX, cardY, cardW, cardH);

    u32 icon = index < ENTRY_COUNT ? entries[index].icon : ICON_HISTORY;
    const char* name = index < ENTRY_COUNT ? _(entries[index].name) : _("Last request");

    float tile = 64;
    float tileX = cardX + 16;
    float tileY = cardY + (cardH - tile) / 2;
    float depth = screen_get_depth();
    screen_set_depth(depth + 1);
    screen_fill_round_rect(tileX, tileY, tile, tile, t->def->radius, t->accentSoft);
    screen_draw_icon(icon, tileX + 14, tileY + 14, 36, t->accent);
    screen_set_depth(depth);

    float textX = tileX + tile + 14;
    float wrap = cardX + cardW - 14;
    float textY = cardY + 14;
    screen_draw_text(name, textX, textY, WIDGET_TITLE_SCALE, t->palette->text);
    textY += widget_text_height(WIDGET_TITLE_SCALE) + 4;

    if(index < ENTRY_COUNT) {
        screen_draw_text_wrap(_(entries[index].description), textX, textY, WIDGET_TEXT_SCALE, t->palette->textMuted, wrap);
    } else {
        char line[96];
        snprintf(line, sizeof(line), menu->lastCount == 1 ? _("%lu link from %s") : _("%lu links from %s"), menu->lastCount, menu->lastHost);
        widget_text_fit(line, textX, textY, WIDGET_TEXT_SCALE, t->palette->textMuted, wrap - textX);
        textY += widget_text_height(WIDGET_TEXT_SCALE) + 4;
        for(u32 i = 0; i < menu->lastCount && i < 3; i++) {
            widget_text_fit(menu->lastShown[i], textX, textY, WIDGET_SMALL_SCALE, t->palette->text, wrap - textX);
            textY += widget_text_height(WIDGET_SMALL_SCALE) + 2;
        }
    }

    // Under the card, what is useful to know for this way in.
    float cellY = cardY + cardH + 8;
    float cellW = (cardW - 6) / 2;
    struct in_addr addr = {(in_addr_t) gethostid()};
    bool wifi = addr.s_addr != 0;
    char value[64];
    if(index == 0) {
        transfer_local_name(value, sizeof(value));
        remoteinstall_draw_cell(cardX, cellY, cellW, _("This console"), value);
        remoteinstall_draw_cell(cardX + cellW + 6, cellY, cellW, _("Saved to"), "/nsa/received/");
    } else if(index == 1) {
        if(wifi) {
            snprintf(value, sizeof(value), "%s:5000", inet_ntoa(addr));
        } else {
            string_copy(value, _("No Wi-Fi"), sizeof(value));
        }

        remoteinstall_draw_cell(cardX, cellY, cellW, _("Send to"), value);
        remoteinstall_draw_cell(cardX + cellW + 6, cellY, cellW, _("FTP server"), ftpserver_running() ? _("Keeps running") : _("Off"));
    }
}

typedef struct {
    remoteinstall_data* menu;
    float x1, y1, x2, y2;
} remoteinstall_top_context;

static void remoteinstall_draw_card_fade(void* data, bool leaving) {
    remoteinstall_top_context* c = (remoteinstall_top_context*) data;
    remoteinstall_draw_card(c->menu, leaving ? c->menu->leaving : c->menu->selected, c->x1, c->y1, c->x2, c->y2);
}

static void remoteinstall_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    remoteinstall_data* menu = (remoteinstall_data*) data;

    if(menu->shown != menu->selected) {
        menu->leaving = menu->shown;
        menu->shown = menu->selected;
        menu->shownTime = osGetTime();
    }

    remoteinstall_top_context context = {menu, x1, y1, x2, y2};
    widget_crossfade(widget_ease(menu->shownTime, WIDGET_CROSSFADE_TIME), remoteinstall_draw_card_fade, &context, menu->leaving != menu->shown);

    widget_storage(SYSTEM_MEDIATYPE_SD, x1 + 16, y2 - 52, x2 - x1 - 32);
}

// A big tile: the icon over the label, on two lines when it must.
static void remoteinstall_draw_tile(float x, float y, float w, float h, u32 icon, const char* label, bool selected) {
    const theme_current* t = theme();
    u32 ink = selected ? t->onAccent : t->palette->text;
    screen_frame_round_rect(x, y, w, h, t->def->radius, t->def->border, selected ? t->accent : t->palette->border, selected ? t->accent : t->palette->surface);

    float iconSize = 26;
    float textH = widget_text_height(WIDGET_TEXT_SCALE);
    float top = y + (h - iconSize - 4 - textH) / 2;
    screen_draw_icon(icon, x + (w - iconSize) / 2, top, iconSize, selected ? t->onAccent : t->accent);
    float labelW = widget_text_width(label, WIDGET_TEXT_SCALE);
    if(labelW <= w - 8) {
        widget_text_centered(label, x, x + w, top + iconSize + 4, WIDGET_TEXT_SCALE, ink);
    } else {
        widget_text_fit(label, x + 4, top + iconSize + 4, WIDGET_TEXT_SCALE, ink, w - 8);
    }
}

static void remoteinstall_draw_history(remoteinstall_data* menu, float x, float y, float w, float h, bool selected) {
    const theme_current* t = theme();
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, selected ? t->accent : t->palette->border, selected ? t->accent : t->palette->surface);

    screen_draw_icon(ICON_HISTORY, x + 9, y + (h - 16) / 2, 16, selected ? t->onAccent : t->accent);

    const char* badge = _("Install again");
    float badgeW = widget_text_width(badge, WIDGET_SMALL_SCALE) + 12;
    float textX = x + 33;
    float textW = x + w - 9 - badgeW - 8 - textX;
    float lineH = widget_text_height(WIDGET_TEXT_SCALE);
    float smallH = widget_text_height(WIDGET_SMALL_SCALE);
    float top = y + (h - lineH - smallH - 1) / 2;
    widget_text_fit(_("Last request"), textX, top, WIDGET_TEXT_SCALE, ink, textW);

    char line[96];
    snprintf(line, sizeof(line), menu->lastCount == 1 ? _("%lu link from %s") : _("%lu links from %s"), menu->lastCount, menu->lastHost);
    widget_text_fit(line, textX, top + lineH + 1, WIDGET_SMALL_SCALE, muted, textW);

    widget_badge(x + w - 9 - badgeW, y + (h - smallH - 6) / 2, badge, selected ? THEME_ALPHA(t->onAccent, 0x30) : t->accentSoft, selected ? t->onAccent : t->accent);
}

static void remoteinstall_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    remoteinstall_data* menu = (remoteinstall_data*) data;
    const theme_current* t = theme();

    for(u32 i = 0; i < remoteinstall_targets(menu); i++) {
        float x, y, w, h;
        remoteinstall_rect(i, x1, y1, x2, &x, &y, &w, &h);
        bool selected = i == menu->selected;
        if(i < ENTRY_COUNT) {
            remoteinstall_draw_tile(x, y, w, h, entries[i].icon, _(entries[i].name), selected);
        } else {
            remoteinstall_draw_history(menu, x, y, w, h, selected);
        }

        widget_press_draw(&menu->press, (int) i, x, y, w, h, i < ENTRY_COUNT ? t->def->radius : t->def->radiusSmall, selected ? t->onAccent : t->palette->text, i == HISTORY_INDEX);
    }
}

void remoteinstall_open() {
    remoteinstall_data* data = (remoteinstall_data*) calloc(1, sizeof(remoteinstall_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate remote install data.");
        return;
    }

    data->press.index = -1;
    remoteinstall_read_last(data);

    ui_view* view = ui_create();
    view->name = _("Remote install");
    view->id = "Remote install";
    view->info = _(" open    back");
    view->data = data;
    view->update = remoteinstall_update;
    view->drawTop = remoteinstall_draw_top;
    view->drawBottom = remoteinstall_draw_bottom;
    ui_push(view);
}
