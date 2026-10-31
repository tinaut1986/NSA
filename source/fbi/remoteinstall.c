#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <3ds.h>
#include <jansson.h>

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

// ---- the history: the requests made, newest first ----

#define HISTORY_PATH "sdmc:" APP_DATA_DIR "history.json"
#define HISTORY_MAX 100

typedef struct {
    char* urls;   // One per line.
    s64 time;     // When it was made (0: before the history existed).
} history_entry;

static history_entry history[HISTORY_MAX];
static u32 history_count = 0;
static bool history_loaded = false;
static u32 history_version = 0;   // Changes with the history, for the screens that show it.

static void history_save() {
    json_t* list = json_array();
    if(list == NULL) {
        return;
    }

    for(u32 i = 0; i < history_count; i++) {
        json_t* entry = json_pack("{s:s, s:I}", "urls", history[i].urls, "time", (json_int_t) history[i].time);
        if(entry != NULL) {
            json_array_append_new(list, entry);
        }
    }

    json_dump_file(list, HISTORY_PATH, JSON_INDENT(2));
    json_decref(list);
}

static void history_load() {
    if(history_loaded) {
        return;
    }

    history_loaded = true;

    json_error_t err;
    json_t* list = json_load_file(HISTORY_PATH, 0, &err);
    if(json_is_array(list)) {
        for(size_t i = 0; i < json_array_size(list) && history_count < HISTORY_MAX; i++) {
            json_t* entry = json_array_get(list, i);
            const char* urls = json_string_value(json_object_get(entry, "urls"));
            if(urls != NULL && urls[0] != '\0' && (history[history_count].urls = strdup(urls)) != NULL) {
                history[history_count].time = (s64) json_integer_value(json_object_get(entry, "time"));
                history_count++;
            }
        }
    } else {
        // Before the history there was only the last request, in its own file.
        char* last = (char*) calloc(1, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX);
        if(last != NULL) {
            if(remoteinstall_get_last_urls(last, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX) && (history[0].urls = strdup(last)) != NULL) {
                history[0].time = 0;
                history_count = 1;
                history_save();
            }

            free(last);
        }
    }

    if(list != NULL) {
        json_decref(list);
    }

    // The old file is not needed any more (the history holds its request).
    FS_Archive sdmc = 0;
    if(R_SUCCEEDED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, APP_DATA_DIR "lasturls"));
        FSUSER_CloseArchive(sdmc);
    }
}

static void history_remove(u32 index) {
    if(index >= history_count) {
        return;
    }

    free(history[index].urls);
    memmove(&history[index], &history[index + 1], (history_count - index - 1) * sizeof(history_entry));
    history_count--;
    history_version++;
}

// A request goes first; made before, it moves up instead of repeating.
static void history_add(const char* urls) {
    history_load();
    if(urls == NULL || urls[0] == '\0') {
        return;
    }

    for(u32 i = 0; i < history_count; i++) {
        if(strcmp(history[i].urls, urls) == 0) {
            history_remove(i);
            break;
        }
    }

    char* copy = strdup(urls);
    if(copy == NULL) {
        return;
    }

    if(history_count == HISTORY_MAX) {
        history_remove(HISTORY_MAX - 1);
    }

    memmove(&history[1], &history[0], history_count * sizeof(history_entry));
    history[0].urls = copy;
    history[0].time = (s64) time(NULL);
    history_count++;
    history_version++;
    history_save();
}

// How many links, from where, and the first few of them.
static u32 history_describe(const history_entry* entry, char* host, size_t hostSize, char shown[][DOWNLOAD_URL_MAX], u32 shownMax) {
    u32 count = 0;
    host[0] = '\0';
    for(const char* line = entry->urls; *line != '\0';) {
        size_t len = strcspn(line, "\n");
        if(len > 0) {
            if(count < shownMax) {
                size_t n = len < DOWNLOAD_URL_MAX - 1 ? len : DOWNLOAD_URL_MAX - 1;
                memcpy(shown[count], line, n);
                shown[count][n] = '\0';
            }

            if(count == 0) {
                const char* start = strstr(line, "://");
                start = start != NULL && (size_t) (start - line) < len ? start + 3 : line;
                size_t n = strcspn(start, ":/?#\n");
                if(n >= hostSize) {
                    n = hostSize - 1;
                }

                memcpy(host, start, n);
                host[n] = '\0';
            }

            count++;
        }

        line += len;
        if(*line == '\n') {
            line++;
        }
    }

    return count;
}

// "28/10 21:40", or "" for a request older than the history.
static void history_date(const history_entry* entry, char* out, size_t size) {
    out[0] = '\0';
    if(entry->time > 0) {
        time_t when = (time_t) entry->time;
        struct tm* t = gmtime(&when);
        snprintf(out, size, "%02d/%02d %02d:%02d", t->tm_mday, t->tm_mon + 1, t->tm_hour, t->tm_min);
    }
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

        history_add(urls);
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

            history_add((const char*) qrData.payload);

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
        history_add(response);

        action_install_url("Install from the entered URL(s)?", response, NULL, NULL, NULL, NULL, NULL);
    }
}

static void remoteinstall_manually_enter_urls() {
    kbd_display("Enter URL(s)", "", SWKBD_TYPE_NORMAL, SWKBD_MULTILINE, SWKBD_NOTEMPTY_NOTBLANK, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX, NULL, remoteinstall_manually_enter_urls_onresponse);
}

static void remoteinstall_install_entry(u32 index) {
    if(index >= history_count) {
        return;
    }

    // A copy: installing moves the request to the top of the history.
    char* urls = strdup(history[index].urls);
    if(urls == NULL) {
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, "Failed to allocate URL text buffer.");
        return;
    }

    history_add(urls);
    action_install_url("Install from the selected request?", urls, NULL, NULL, NULL, NULL, NULL);
    free(urls);
}

static void remoteinstall_repeat_last_request() {
    history_load();
    if(history_count == 0) {
        prompt_display_notify("Failure", "No previously requested URL(s) could be found.", COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    remoteinstall_install_entry(0);
}

// ---- the history screen: every request, to install again or forget ----

typedef struct {
    u32 version;
} remoteinstall_history_data;

static void remoteinstall_history_clear(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static u32 remoteinstall_history_index(list_item* item) {
    return (u32) (uintptr_t) item->data;
}

static void remoteinstall_history_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    u32 index = remoteinstall_history_index(item);
    if(index >= history_count) {
        return;
    }

    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, selected ? t->accent : t->palette->border, selected ? t->accent : t->palette->surface);
    screen_draw_icon(ICON_WORLD_DOWNLOAD, x + 9, y + (height - 16) / 2, 16, selected ? t->onAccent : t->accent);

    char host[64];
    char shown[1][DOWNLOAD_URL_MAX];
    u32 count = history_describe(&history[index], host, sizeof(host), shown, 1);

    char name[FILE_NAME_MAX];
    const char* slash = strrchr(shown[0], '/');
    string_copy(name, slash != NULL && slash[1] != '\0' ? slash + 1 : shown[0], sizeof(name));
    name[strcspn(name, "?#")] = '\0';

    char date[32];
    history_date(&history[index], date, sizeof(date));
    float dateW = widget_text_width(date, WIDGET_SMALL_SCALE);

    float tx = x + 33;
    float right = x + width - 9;
    float lineH = widget_text_height(WIDGET_TEXT_SCALE);
    float smallH = widget_text_height(WIDGET_SMALL_SCALE);
    float top = y + (height - lineH - smallH - 1) / 2;
    widget_text_fit(name, tx, top, WIDGET_TEXT_SCALE, ink, right - tx);

    char line[96];
    snprintf(line, sizeof(line), count == 1 ? _("%lu link from %s") : _("%lu links from %s"), count, host);
    widget_text_fit(line, tx, top + lineH + 1, WIDGET_SMALL_SCALE, muted, right - tx - dateW - 8);
    screen_draw_text(date, right - dateW, top + lineH + 1, WIDGET_SMALL_SCALE, muted);
}

// Every link of the request, on the top screen.
static void remoteinstall_history_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL || remoteinstall_history_index(selected) >= history_count) {
        return;
    }

    const theme_current* t = theme();
    const history_entry* entry = &history[remoteinstall_history_index(selected)];

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = y2 - y1 - 28;
    widget_card(cardX, cardY, cardW, cardH);

    char host[64];
    static char shown[8][DOWNLOAD_URL_MAX];
    u32 count = history_describe(entry, host, sizeof(host), shown, 8);

    char date[32];
    history_date(entry, date, sizeof(date));
    char line[128];
    snprintf(line, sizeof(line), count == 1 ? _("%lu link from %s") : _("%lu links from %s"), count, host);
    float textX = cardX + 14;
    float right = cardX + cardW - 14;
    float y = cardY + 12;
    widget_text_fit(line, textX, y, WIDGET_TITLE_SCALE, t->palette->text, right - textX);
    y += widget_text_height(WIDGET_TITLE_SCALE) + 2;
    if(date[0] != '\0') {
        screen_draw_text(date, textX, y, WIDGET_SMALL_SCALE, t->palette->textMuted);
        y += widget_text_height(WIDGET_SMALL_SCALE) + 6;
    }

    float lineH = widget_text_height(WIDGET_SMALL_SCALE) + 3;
    for(u32 i = 0; i < count && i < 8 && y + lineH <= cardY + cardH - 10; i++) {
        bool more = i == 7 || y + 2 * lineH > cardY + cardH - 10;
        if(more && count > i + 1) {
            snprintf(line, sizeof(line), _("and %lu more"), count - i);
            screen_draw_text(line, textX, y, WIDGET_SMALL_SCALE, t->palette->textMuted);
            break;
        }

        widget_text_fit(shown[i], textX, y, WIDGET_SMALL_SCALE, t->palette->text, right - textX);
        y += lineH;
    }
}

static void remoteinstall_history_sheet_install(void* data) {
    remoteinstall_install_entry(remoteinstall_history_index((list_item*) data));
}

static void remoteinstall_history_sheet_forget(void* data) {
    history_remove(remoteinstall_history_index((list_item*) data));
    history_save();
}

static void remoteinstall_history_long_press(ui_view* view, void* data, linked_list* items, list_item* item) {
    static const sheet_action actions[] = {
            {ICON_DOWNLOAD, "Install", false, remoteinstall_history_sheet_install, false},
            {ICON_TRASH, "Forget", true, remoteinstall_history_sheet_forget, false},
    };

    sheet_action translated[2];
    for(u32 i = 0; i < 2; i++) {
        translated[i] = actions[i];
        translated[i].label = _(actions[i].label);
    }

    sheet_display(NULL, 0, translated, 2, item);
}

static void remoteinstall_history_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    remoteinstall_history_data* historyData = (remoteinstall_history_data*) data;

    view->name = _("History");
    view->info = _("\uE000 install   \uE001 back");

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        remoteinstall_history_clear(items);
        list_destroy(view);
        free(historyData);
        return;
    }

    // Built again when the history changes (installed, forgotten).
    if(historyData->version != history_version || linked_list_size(items) != history_count) {
        historyData->version = history_version;
        remoteinstall_history_clear(items);
        for(u32 i = 0; i < history_count; i++) {
            list_item* item = (list_item*) calloc(1, sizeof(list_item));
            if(item != NULL) {
                char host[64];
                char shown[1][DOWNLOAD_URL_MAX];
                history_describe(&history[i], host, sizeof(host), shown, 1);
                string_copy(item->name, shown[0], sizeof(item->name));
                item->color = COLOR_TEXT;
                item->data = (void*) (uintptr_t) i;
                linked_list_add(items, item);
            }
        }

        return;
    }

    if(selected != NULL && selectedTouched) {
        remoteinstall_install_entry(remoteinstall_history_index(selected));
    }
}

static void remoteinstall_history_open() {
    history_load();

    remoteinstall_history_data* data = (remoteinstall_history_data*) calloc(1, sizeof(remoteinstall_history_data));
    if(data == NULL) {
        return;
    }

    data->version = history_version - 1;
    ui_view* view = list_display(_("History"), _("\uE000 install   \uE001 back"), data, remoteinstall_history_update, remoteinstall_history_draw_top);
    if(view == NULL) {
        free(data);
        return;
    }

    view->id = "History";
    list_style style = {
            .rowHeight = 36,
            .drawRow = remoteinstall_history_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
    list_set_long_press(view, remoteinstall_history_long_press);
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

    // The newest request, from the history (a screen opened from here may change it).
    bool haveLast;
    u32 lastCount;
    char lastHost[64];
    u32 lastVersion;

    u32 shown;
    u32 leaving;
    u64 shownTime;
} remoteinstall_data;

static void remoteinstall_read_last(remoteinstall_data* data) {
    history_load();
    data->lastVersion = history_version;
    data->haveLast = history_count > 0;
    if(data->haveLast) {
        char shown[1][DOWNLOAD_URL_MAX];
        data->lastCount = history_describe(&history[0], data->lastHost, sizeof(data->lastHost), shown, 1);
    }
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

static void remoteinstall_open_target(remoteinstall_data* data, u32 index) {
    if(index < ENTRY_COUNT) {
        entries[index].open();
    } else if(data->haveLast) {
        remoteinstall_history_open();
    }
}

static void remoteinstall_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    remoteinstall_data* menu = (remoteinstall_data*) data;

    view->name = _("Remote install");
    view->info = menu->haveLast ? _("\uE000 open   \uE003 install the last again   \uE001 back") : _("\uE000 open   \uE001 back");

    if(menu->lastVersion != history_version) {
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

        if(!held) {
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
    const char* name = index < ENTRY_COUNT ? _(entries[index].name) : _("History");

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
        // The newest requests, one line each.
        char line[128];
        snprintf(line, sizeof(line), history_count == 1 ? _("%lu request") : _("%lu requests"), history_count);
        widget_text_fit(line, textX, textY, WIDGET_TEXT_SCALE, t->palette->textMuted, wrap - textX);
        textY += widget_text_height(WIDGET_TEXT_SCALE) + 4;
        for(u32 i = 0; i < history_count && i < 3; i++) {
            char host[64];
            char shown[1][DOWNLOAD_URL_MAX];
            history_describe(&history[i], host, sizeof(host), shown, 1);
            const char* slash = strrchr(shown[0], '/');
            widget_text_fit(slash != NULL && slash[1] != '\0' ? slash + 1 : shown[0], textX, textY, WIDGET_SMALL_SCALE, t->palette->text, wrap - textX);
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

    char badge[16];
    snprintf(badge, sizeof(badge), "%lu", history_count);
    float badgeW = widget_text_width(badge, WIDGET_SMALL_SCALE) + 12;
    float textX = x + 33;
    float textW = x + w - 9 - badgeW - 8 - textX;
    float lineH = widget_text_height(WIDGET_TEXT_SCALE);
    float smallH = widget_text_height(WIDGET_SMALL_SCALE);
    float top = y + (h - lineH - smallH - 1) / 2;
    widget_text_fit(_("History"), textX, top, WIDGET_TEXT_SCALE, ink, textW);

    char line[96];
    snprintf(line, sizeof(line), menu->lastCount == 1 ? _("Last: %lu link from %s") : _("Last: %lu links from %s"), menu->lastCount, menu->lastHost);
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
    view->info = _("\uE000 open   \uE001 back");
    view->data = data;
    view->update = remoteinstall_update;
    view->drawTop = remoteinstall_draw_top;
    view->drawBottom = remoteinstall_draw_bottom;
    ui_push(view);
}
