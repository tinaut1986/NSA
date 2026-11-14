#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <3ds.h>
#include <jansson.h>

#include "resources.h"
#include "section.h"
#include "settings.h"
#include "ftpserver.h"
#include "webserver.h"
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

void remoteinstall_note_urls(const char* urls) {
    history_add(urls);
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
        datefmt_day_month_time(out, size, t);
    }
}

typedef struct {
    int serverSocket;
    int clientSocket;
} remoteinstall_network_data;

// These run on the UI thread, where the keys are not read again: a client that connects and sends nothing (FileZilla
// on port 5000 waits for an FTP welcome, #35) used to wait here forever and freeze NSA. Give up after
// REMOTEINSTALL_NETWORK_IDLE_MS without a byte moved.
#define REMOTEINSTALL_NETWORK_IDLE_MS 5000

static int remoteinstall_network_recvwait(int sockfd, void* buf, size_t len, int flags) {
    errno = 0;

    int ret = 0;
    size_t read = 0;
    u64 last = osGetTime();
    while((((ret = recv(sockfd, buf + read, len - read, flags)) > 0 && (read += ret) < len) || errno == EAGAIN) && !(hidKeysDown() & KEY_B)) {
        if(ret > 0) {
            last = osGetTime();
        } else if(osGetTime() - last >= REMOTEINSTALL_NETWORK_IDLE_MS) {
            errno = ETIMEDOUT;
            return -1;
        } else {
            svcSleepThread(1000000);
        }

        errno = 0;
    }

    return ret < 0 ? ret : (int) read;
}

static int remoteinstall_network_sendwait(int sockfd, void* buf, size_t len, int flags) {
    errno = 0;

    int ret = 0;
    size_t written = 0;
    u64 last = osGetTime();
    while((((ret = send(sockfd, buf + written, len - written, flags)) > 0 && (written += ret) < len) || errno == EAGAIN) && !(hidKeysDown() & KEY_B)) {
        if(ret > 0) {
            last = osGetTime();
        } else if(osGetTime() - last >= REMOTEINSTALL_NETWORK_IDLE_MS) {
            errno = ETIMEDOUT;
            return -1;
        } else {
            svcSleepThread(1000000);
        }

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
        action_install_url_remote("Install from the received URL(s)?", urls, data, remoteinstall_network_close_client);

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

    // qr_scan: the first code read goes to `found` (NULL: Remote install's own, which installs the links).
    const char* hint;
    void* userData;
    void (*found)(void* data, const char* text);
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

        if(err == 0 && installData->found != NULL) {
            static char text[QUIRC_MAX_PAYLOAD + 1];
            size_t len = qrData.payload_len < QUIRC_MAX_PAYLOAD ? (size_t) qrData.payload_len : QUIRC_MAX_PAYLOAD;
            memcpy(text, qrData.payload, len);
            text[len] = '\0';

            void* userData = installData->userData;
            void (*found)(void* data, const char* text) = installData->found;
            ui_pop();
            info_destroy(view);
            remoteinstall_qr_free_data(installData);
            found(userData, text);
            return;
        }

        if(err == 0) {
            remoteinstall_qr_stop_capture(installData);

            history_add((const char*) qrData.payload);

            action_install_url_remote("Install from the scanned QR code?", (const char*) qrData.payload, NULL, NULL);
            return;
        }
    }

    snprintf(text, PROGRESS_TEXT_MAX, "%s", installData->hint != NULL ? i18n_translate_text(installData->hint) : _("Point the camera at the code.\nA QR code with one or more links to CIAs."));
}

void qr_scan(const char* title, const char* hint, void* userData, void (*found)(void* data, const char* text)) {
    remoteinstall_qr_data* data = (remoteinstall_qr_data*) calloc(1, sizeof(remoteinstall_qr_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate QR install data.");

        return;
    }

    data->tex = 0;
    data->hint = hint;
    data->userData = userData;
    data->found = found;

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

    info_display(title, "\uE002 switch camera   \uE001 back", false, data, remoteinstall_qr_update, remoteinstall_qr_draw_top);
}

static void remoteinstall_scan_qr_code() {
    qr_scan("Scan QR code", NULL, NULL, NULL);
}

static void remoteinstall_manually_enter_urls_onresponse(ui_view* view, void* data, SwkbdButton button, const char* response) {
    if(button == SWKBD_BUTTON_CONFIRM) {
        history_add(response);

        action_install_url_remote("Install from the entered URL(s)?", response, NULL, NULL);
    }
}

static void remoteinstall_manually_enter_urls() {
    osk_display("Enter URL(s)", "", DOWNLOAD_URL_MAX * INSTALL_URLS_MAX, OSK_NOT_BLANK | OSK_MULTILINE, NULL, remoteinstall_manually_enter_urls_onresponse);
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
    action_install_url_remote("Install from the selected request?", urls, NULL, NULL);
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
    string_url_decode(name, name, sizeof(name));

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

    // Each link whole, over as many lines as it needs, while the card has room.
    float lineH = widget_text_height(WIDGET_SMALL_SCALE) + 3;
    float bottom = cardY + cardH - 10;
    for(u32 i = 0; i < count && i < 8; i++) {
        char decoded[DOWNLOAD_URL_MAX];
        string_url_decode(decoded, shown[i], sizeof(decoded));

        float textW = 0;
        float textH = 0;
        screen_get_string_size_wrap(&textW, &textH, decoded, WIDGET_SMALL_SCALE, WIDGET_SMALL_SCALE, right - textX);

        // Room for this one and for the "and N more" line after it, unless it is the last.
        bool last = i + 1 == count;
        if(y + textH + (last ? 0 : lineH) > bottom) {
            if(i == 0) {
                // Not even one fits whole: as much of it as there is.
                widget_text_fit(decoded, textX, y, WIDGET_SMALL_SCALE, t->palette->text, right - textX);
                y += lineH;
                i++;
            }

            if(i < count) {
                snprintf(line, sizeof(line), _("and %lu more"), count - i);
                screen_draw_text(line, textX, y, WIDGET_SMALL_SCALE, t->palette->textMuted);
            }

            break;
        }

        screen_draw_text_wrap(decoded, textX, y, WIDGET_SMALL_SCALE, t->palette->text, right);
        y += textH + 3;
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

// Ids for the order kept in settings.json, in the order of entries[].
static const char* const entry_ids[] = {"3ds", "pc", "qr", "url"};
_Static_assert(sizeof(entry_ids) / sizeof(*entry_ids) == ENTRY_COUNT, "an id for every tile");

// The tiles' order: position p (0 to 3, two by two) holds entries[order[p]]. The history row stays below them.
static u8 order[ENTRY_COUNT] = {0, 1, 2, 3};
static bool editing = false;

static void remoteinstall_order_load() {
    for(u32 i = 0; i < ENTRY_COUNT; i++) {
        order[i] = (u8) i;
    }

    const char* c = settings_get()->remoteMenu;
    bool used[ENTRY_COUNT] = {false};
    u8 loaded[ENTRY_COUNT];
    u32 count = 0;
    while(*c != '\0') {
        char id[8];
        size_t len = 0;
        while(*c != '\0' && *c != ',') {
            if(len < sizeof(id) - 1) {
                id[len++] = *c;
            }

            c++;
        }

        id[len] = '\0';
        if(*c == ',') {
            c++;
        }

        for(u32 e = 0; e < ENTRY_COUNT; e++) {
            if(!used[e] && strcmp(id, entry_ids[e]) == 0) {
                used[e] = true;
                loaded[count++] = (u8) e;
                break;
            }
        }
    }

    // A saved order that does not name every tile is not used.
    if(count == ENTRY_COUNT) {
        memcpy(order, loaded, sizeof(order));
    }
}

static void remoteinstall_order_save() {
    char text[sizeof(settings_get()->remoteMenu)];
    size_t len = 0;
    bool isDefault = true;
    for(u32 p = 0; p < ENTRY_COUNT; p++) {
        isDefault = isDefault && order[p] == p;
        len += snprintf(text + len, sizeof(text) - len, "%s%s", p > 0 ? "," : "", entry_ids[order[p]]);
    }

    string_copy(settings_get()->remoteMenu, isDefault ? "" : text, sizeof(settings_get()->remoteMenu));
    Result res = settings_save();
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to save settings."));
    }
}
#define HISTORY_INDEX ENTRY_COUNT   // The last request's row, when there is one.
// Two small buttons at the right of the history row: they turn the FTP server and the web server on or off (as Settings does).
#define FTP_BUTTON (ENTRY_COUNT + 1)
#define WEB_BUTTON (ENTRY_COUNT + 2)
#define TARGET_COUNT (ENTRY_COUNT + 3)
#define BUTTON_WIDTH 46
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
    char lastName[FILE_NAME_MAX];   // The file the newest request names (its first link).
    u32 lastVersion;

    u32 shown;
    u32 leaving;
    u64 shownTime;

    // Editing the order of the tiles.
    bool grabbed;       // The selected tile is picked up: the D-Pad moves it.
    int dragFrom;       // A finger holds the tile at this position, or -1.
    bool dragMoved;
    float dragStartX, dragStartY, dragX, dragY;
} remoteinstall_data;

static void remoteinstall_read_last(remoteinstall_data* data) {
    history_load();
    data->lastVersion = history_version;
    data->haveLast = history_count > 0;
    if(data->haveLast) {
        char shown[1][DOWNLOAD_URL_MAX];
        data->lastCount = history_describe(&history[0], data->lastHost, sizeof(data->lastHost), shown, 1);

        // The last part of the link's path, without its query: the file's name.
        char path[DOWNLOAD_URL_MAX];
        string_copy(path, shown[0], sizeof(path));
        path[strcspn(path, "?#")] = '\0';
        const char* slash = strrchr(path, '/');
        string_url_decode(data->lastName, slash != NULL && slash[1] != '\0' ? slash + 1 : data->lastHost, sizeof(data->lastName));
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
        *h = HISTORY_HEIGHT;
        *y = y1 + PAD + 2 * (TILE_HEIGHT + GAP);
        if(index == HISTORY_INDEX) {
            *w = area - 2 * (BUTTON_WIDTH + GAP);
            *x = x1 + PAD;
        } else {
            *w = BUTTON_WIDTH;
            *x = x1 + PAD + area - (index == WEB_BUTTON ? BUTTON_WIDTH : 2 * BUTTON_WIDTH + GAP);
        }
    }
}

static u32 remoteinstall_targets(remoteinstall_data* data) {
    return TARGET_COUNT;
}

// The history row is only there when there is a request; the tiles and the two buttons always are.
static bool remoteinstall_has(remoteinstall_data* data, u32 index) {
    return index != HISTORY_INDEX || data->haveLast;
}

// Two by two, the history row under both columns.
static u32 remoteinstall_move(remoteinstall_data* data, u32 selected, u32 key) {
    bool history = data->haveLast;
    // The bottom row: the history (when there is one), then the FTP and the web buttons.
    if(selected >= ENTRY_COUNT) {
        switch(key) {
            case KEY_UP: return selected == HISTORY_INDEX ? 2 : 3;
            case KEY_DOWN: return selected == WEB_BUTTON ? 1 : selected == FTP_BUTTON ? 1 : 0;
            case KEY_LEFT: return selected == WEB_BUTTON ? FTP_BUTTON : selected == FTP_BUTTON && history ? HISTORY_INDEX : selected;
            case KEY_RIGHT: return selected == HISTORY_INDEX ? FTP_BUTTON : selected == FTP_BUTTON ? WEB_BUTTON : selected;
            default: return selected;
        }
    }

    switch(key) {
        case KEY_LEFT: return selected % 2 == 1 ? selected - 1 : selected;
        case KEY_RIGHT: return selected % 2 == 0 ? selected + 1 : selected;
        case KEY_UP: return selected >= 2 ? selected - 2 : (selected == 0 ? (history ? HISTORY_INDEX : FTP_BUTTON) : WEB_BUTTON);
        case KEY_DOWN: return selected < 2 ? selected + 2 : (selected == 2 ? (history ? HISTORY_INDEX : FTP_BUTTON) : WEB_BUTTON);
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

static void remoteinstall_save_settings() {
    Result res = settings_save();
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to save settings."));
    }
}

// As Settings does: on or waiting for Wi-Fi turns it off; off turns it on now, or once there is Wi-Fi.
static void remoteinstall_toggle_ftp() {
    settings_data* settings = settings_get();
    if(ftpserver_running() || ftpserver_waiting()) {
        ftpserver_stop();
        settings->ftpServer = false;
    } else {
        ftpserver_start_when_online();
        settings->ftpServer = true;
    }

    remoteinstall_save_settings();
}

static void remoteinstall_toggle_web() {
    settings_data* settings = settings_get();
    if(webserver_running() || webserver_waiting()) {
        webserver_stop();
        settings->webServer = false;
    } else {
        webserver_start_when_online();
        settings->webServer = true;
    }

    remoteinstall_save_settings();
}

static void remoteinstall_open_target(remoteinstall_data* data, u32 index) {
    if(index < ENTRY_COUNT) {
        entries[order[index]].open();
    } else if(index == FTP_BUTTON) {
        remoteinstall_toggle_ftp();
    } else if(index == WEB_BUTTON) {
        remoteinstall_toggle_web();
    } else if(data->haveLast) {
        remoteinstall_history_open();
    }
}

static void remoteinstall_edit_exit(remoteinstall_data* menu) {
    editing = false;
    menu->grabbed = false;
    menu->dragFrom = -1;
    remoteinstall_order_save();
}

static void remoteinstall_reset_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        remoteinstall_data* menu = (remoteinstall_data*) data;
        for(u32 i = 0; i < ENTRY_COUNT; i++) {
            order[i] = (u8) i;
        }

        menu->grabbed = false;
        menu->dragFrom = -1;
        remoteinstall_order_save();
    }
}

static void remoteinstall_swap(u32 a, u32 b) {
    u8 tmp = order[a];
    order[a] = order[b];
    order[b] = tmp;
}

// Editing the tiles: A picks one up (and the D-Pad moves it), or drag it by touch; Y puts the order back; SELECT or B finish.
static void remoteinstall_edit_update(remoteinstall_data* menu, float bx1, float by1, float bx2) {
    u32 down = hidKeysDown();
    if((down & KEY_SELECT) || ((down & KEY_B) && !menu->grabbed)) {
        remoteinstall_edit_exit(menu);
        return;
    }

    if(down & KEY_B) {
        menu->grabbed = false;
    }

    if(menu->selected >= ENTRY_COUNT) {
        menu->selected = 0;
    }

    static const u32 keys[] = {KEY_DOWN, KEY_UP, KEY_RIGHT, KEY_LEFT};
    for(u32 i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
        if(remoteinstall_repeat(menu, keys[i])) {
            // Two by two: only the tiles take part.
            u32 key = keys[i];
            u32 target = menu->selected;
            switch(key) {
                case KEY_LEFT: target = menu->selected % 2 == 1 ? menu->selected - 1 : menu->selected; break;
                case KEY_RIGHT: target = menu->selected % 2 == 0 ? menu->selected + 1 : menu->selected; break;
                case KEY_UP: target = menu->selected >= 2 ? menu->selected - 2 : menu->selected + 2; break;
                case KEY_DOWN: target = menu->selected < 2 ? menu->selected + 2 : menu->selected - 2; break;
            }

            if(menu->grabbed && target != menu->selected) {
                remoteinstall_swap(menu->selected, target);
            }

            menu->selected = target;
            break;
        }
    }

    if(down & KEY_A) {
        menu->grabbed = !menu->grabbed;
    }

    if(down & KEY_Y) {
        prompt_display_yes_no(_("Edit the menu"), _("Put the menu back as it came?"), COLOR_TEXT, menu, NULL, remoteinstall_reset_onresponse);
        return;
    }

    touchPosition pos;
    hidTouchRead(&pos);
    if(down & KEY_TOUCH) {
        menu->dragFrom = -1;
        menu->dragMoved = false;
        for(u32 i = 0; i < ENTRY_COUNT; i++) {
            float x, y, w, h;
            remoteinstall_rect(i, bx1, by1, bx2, &x, &y, &w, &h);
            if(widget_touched(&pos, x, y, w, h)) {
                menu->dragFrom = (int) i;
                menu->selected = i;
                menu->grabbed = false;
                menu->dragStartX = menu->dragX = pos.px;
                menu->dragStartY = menu->dragY = pos.py;
                break;
            }
        }
    } else if(menu->dragFrom >= 0 && (hidKeysHeld() & KEY_TOUCH)) {
        menu->dragX = pos.px;
        menu->dragY = pos.py;
        float dx = menu->dragX - menu->dragStartX;
        float dy = menu->dragY - menu->dragStartY;
        if(dx * dx + dy * dy > 64) {
            menu->dragMoved = true;
        }

        // The tile in the hand takes the place of the one under the finger at once; that one makes way.
        if(menu->dragMoved) {
            for(u32 i = 0; i < ENTRY_COUNT; i++) {
                float x, y, w, h;
                remoteinstall_rect(i, bx1, by1, bx2, &x, &y, &w, &h);
                if(i != (u32) menu->dragFrom && widget_touched(&pos, x, y, w, h)) {
                    remoteinstall_swap((u32) menu->dragFrom, i);
                    menu->dragFrom = (int) i;
                    menu->selected = i;
                    break;
                }
            }
        }
    } else if(menu->dragFrom >= 0 && (hidKeysUp() & KEY_TOUCH)) {
        // A drag was applied as it went.
        menu->dragFrom = -1;
    }
}

static void remoteinstall_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    remoteinstall_data* menu = (remoteinstall_data*) data;

    view->name = _("Remote install");
    if(editing) {
        view->info = _("\uE000 move   \uE003 reset   SELECT done");
    } else if(menu->selected == FTP_BUTTON || menu->selected == WEB_BUTTON) {
        view->info = _("\uE000 turn on or off   \uE001 back");
    } else {
        view->info = menu->haveLast ? _("\uE000 open   \uE003 install the last again   \uE001 back") : _("\uE000 open   \uE001 back");
    }

    if(menu->lastVersion != history_version) {
        remoteinstall_read_last(menu);
        if(!menu->haveLast && menu->selected == HISTORY_INDEX) {
            menu->selected = 2;
        }
    }

    if(editing) {
        ui_wake();   // The tiles jiggle: keep drawing.
        remoteinstall_edit_update(menu, bx1, by1, bx2);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        free(menu);
        ui_destroy(view);
        return;
    }

    if(hidKeysDown() & KEY_SELECT) {
        editing = true;
        menu->grabbed = false;
        if(menu->selected >= ENTRY_COUNT) {
            menu->selected = 0;
        }

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

    // A, or a finger: acts on release; held long on a tile, it starts editing the order; a finger sliding over the tiles
    // moves the focus to the one under it.
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        widget_press_end(&menu->press);
        for(u32 i = 0; i < remoteinstall_targets(menu); i++) {
            float x, y, w, h;
            remoteinstall_rect(i, bx1, by1, bx2, &x, &y, &w, &h);
            if(remoteinstall_has(menu, i) && widget_touched(&pos, x, y, w, h)) {
                widget_press_begin(&menu->press, (int) i, true, pos.px, pos.py);
                menu->selected = i;
            }
        }
    } else if(hidKeysDown() & KEY_A) {
        float x, y, w, h;
        remoteinstall_rect(menu->selected, bx1, by1, bx2, &x, &y, &w, &h);
        widget_press_begin(&menu->press, (int) menu->selected, false, x + w / 2, y + h / 2);
    } else if(menu->press.touch && menu->press.index >= 0 && (hidKeysHeld() & KEY_TOUCH)) {
        for(u32 i = 0; i < remoteinstall_targets(menu); i++) {
            float x, y, w, h;
            remoteinstall_rect(i, bx1, by1, bx2, &x, &y, &w, &h);
            if(i != menu->selected && remoteinstall_has(menu, i) && widget_touched(&pos, x, y, w, h)) {
                menu->selected = i;
                menu->press.cancelled = true;
                break;
            }
        }

        if(!menu->press.cancelled && (u32) menu->press.index < ENTRY_COUNT && widget_press_long_due(&menu->press)) {
            menu->press.cancelled = true;
            editing = true;
            menu->grabbed = false;
            menu->dragFrom = -1;
            return;
        }
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

// `text` wrapped up to `right`: the lines above `belowY` start at `x` (beside the icon), the rest at `leftX` (under it).
static void remoteinstall_draw_flow(const char* text, float x, float y, float scale, u32 color, float right, float belowY, float leftX) {
    float lineHeight = widget_text_height(scale) + 2;
    char line[192] = "";
    const char* p = text;
    while(*p != '\0') {
        const char* end = p;
        while(*end != '\0' && *end != ' ') {
            end++;
        }

        char word[64];
        size_t n = (size_t) (end - p) < sizeof(word) - 1 ? (size_t) (end - p) : sizeof(word) - 1;
        memcpy(word, p, n);
        word[n] = '\0';

        char joined[192];
        snprintf(joined, sizeof(joined), "%s%s%s", line, line[0] != '\0' ? " " : "", word);
        float lineX = y < belowY ? x : leftX;
        if(line[0] != '\0' && widget_text_width(joined, scale) > right - lineX) {
            screen_draw_text(line, lineX, y, scale, color);
            y += lineHeight;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", joined);
        }

        p = end;
        while(*p == ' ') {
            p++;
        }
    }

    if(line[0] != '\0') {
        screen_draw_text(line, y < belowY ? x : leftX, y, scale, color);
    }
}

static void remoteinstall_draw_card(remoteinstall_data* menu, u32 index, float x1, float y1, float x2, float y2) {
    const theme_current* t = theme();

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    // The web button's card is taller while the server runs: the QR code needs room to be read.
    bool server = index == FTP_BUTTON || index == WEB_BUTTON;
    float cardH = index == WEB_BUTTON && webserver_running() ? 132 : 96;
    widget_card(cardX, cardY, cardW, cardH);

    // The entry shown at position `index` (the history and the two buttons are past the tiles).
    u32 entry = index < ENTRY_COUNT ? order[index] : index;
    u32 icon = index < ENTRY_COUNT ? entries[entry].icon : index == FTP_BUTTON ? ICON_SERVER : index == WEB_BUTTON ? ICON_DEVICE_DESKTOP : ICON_HISTORY;
    const char* name = index < ENTRY_COUNT ? _(entries[entry].name) : index == FTP_BUTTON ? _("FTP server") : index == WEB_BUTTON ? _("Web server") : _("History");

    float tile = 64;
    float tileX = cardX + 16;
    float tileY = cardY + (96 - tile) / 2;   // As in the other cards, also when this one is taller.
    float depth = screen_get_depth();
    screen_set_depth(depth + 1);
    screen_fill_round_rect(tileX, tileY, tile, tile, t->def->radius, t->accentSoft);
    screen_draw_icon(icon, tileX + 14, tileY + 14, 36, t->accent);
    screen_set_depth(depth);

    float textX = tileX + tile + 14;
    float wrap = cardX + cardW - 14;
    if(index == WEB_BUTTON && webserver_running()) {
        // The page's address and PIN as a QR code, to open it from a phone's camera.
        float qrSide = cardH - 12;
        // Raised over the card by a whole step of depth (a fractional one blurs the code in 3D).
        float baseDepth = screen_get_depth();
        screen_set_depth(roundf(baseDepth) + 1);
        settingsmenu_draw_web_qr(cardX + cardW - 6 - qrSide, cardY + 6, qrSide);
        screen_set_depth(baseDepth);
        wrap -= qrSide - 4;
    }

    float textY = cardY + 14;
    screen_draw_text(name, textX, textY, WIDGET_TITLE_SCALE, t->palette->text);
    textY += widget_text_height(WIDGET_TITLE_SCALE) + 4;

    if(index < ENTRY_COUNT) {
        screen_draw_text_wrap(_(entries[entry].description), textX, textY, WIDGET_TEXT_SCALE, t->palette->textMuted, wrap);
    } else if(server) {
        // Beside the icon, then under it.
        remoteinstall_draw_flow(index == FTP_BUTTON ? _("Copy files to and from the SD card with an FTP program. Press A to turn it on or off.")
                                                    : _("Manage the console from a computer's browser. Press A to turn it on or off."),
                                textX, textY, WIDGET_TEXT_SCALE, t->palette->textMuted, wrap, tileY + tile + 4, tileX);
    } else {
        // The newest requests, one line each.
        char line[128];
        snprintf(line, sizeof(line), history_count == 1 ? _("%lu request") : _("%lu requests"), history_count);
        widget_text_fit(line, textX, textY, WIDGET_TEXT_SCALE, t->palette->textMuted, wrap - textX);
        textY += widget_text_height(WIDGET_TEXT_SCALE) + 4;
        // Only the lines that fit inside the card.
        for(u32 i = 0; i < history_count && i < 3 && textY + widget_text_height(WIDGET_SMALL_SCALE) <= cardY + cardH - 8; i++) {
            char host[64];
            char shown[1][DOWNLOAD_URL_MAX];
            history_describe(&history[i], host, sizeof(host), shown, 1);
            const char* slash = strrchr(shown[0], '/');
            char recent[FILE_NAME_MAX];
            string_url_decode(recent, slash != NULL && slash[1] != '\0' ? slash + 1 : shown[0], sizeof(recent));
            widget_text_fit(recent, textX, textY, WIDGET_SMALL_SCALE, t->palette->text, wrap - textX);
            textY += widget_text_height(WIDGET_SMALL_SCALE) + 2;
        }
    }

    // Under the card, what is useful to know for this way in.
    float cellY = cardY + cardH + 8;
    float cellW = (cardW - 6) / 2;
    struct in_addr addr = {(in_addr_t) gethostid()};
    bool wifi = addr.s_addr != 0;
    char value[64];
    if(index < ENTRY_COUNT && entry == 0) {
        transfer_local_name(value, sizeof(value));
        remoteinstall_draw_cell(cardX, cellY, cellW, _("This console"), value);
        remoteinstall_draw_cell(cardX + cellW + 6, cellY, cellW, _("Saved to"), "/nsa/received/");
    } else if(index < ENTRY_COUNT && entry == 1) {
        if(wifi) {
            snprintf(value, sizeof(value), "%s:5000", inet_ntoa(addr));
        } else {
            string_copy(value, _("No Wi-Fi"), sizeof(value));
        }

        remoteinstall_draw_cell(cardX, cellY, cellW, _("Send to"), value);
        remoteinstall_draw_cell(cardX + cellW + 6, cellY, cellW, _("FTP server"), ftpserver_running() ? _("Keeps running") : _("Off"));
    } else if(index == FTP_BUTTON) {
        ftpserver_address(value, sizeof(value));
        remoteinstall_draw_cell(cardX, cellY, cellW, _("Address"), value);
        remoteinstall_draw_cell(cardX + cellW + 6, cellY, cellW, _("Status"), ftpserver_running() ? _("On") : ftpserver_waiting() ? _("Waiting for Wi-Fi") : _("Off"));
    } else if(index == WEB_BUTTON) {
        char pin[16];
        webserver_address(value, sizeof(value));
        webserver_pin(pin, sizeof(pin));
        // The address is the long one: the PIN's cell is narrow.
        float pinW = 96;
        remoteinstall_draw_cell(cardX, cellY, cardW - pinW - 6, _("Address"), value);
        remoteinstall_draw_cell(cardX + cardW - pinW, cellY, pinW, _("PIN"), pin[0] != '\0' ? pin : webserver_running() ? _("None") : "-");
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

    if(editing) {
        const theme_current* t = theme();
        float cardX = x1 + 12;
        float cardY = y1 + 14;
        float cardW = x2 - x1 - 24;
        widget_card(cardX, cardY, cardW, 96);
        float textX = cardX + 16;
        screen_draw_text(_("Edit the menu"), textX, cardY + 12, WIDGET_TITLE_SCALE, t->palette->text);
        screen_draw_text_wrap(_("Drag a tile, or pick it up with A and use the D-Pad, to put it where you want. The history row stays at the bottom."),
                              textX, cardY + 12 + widget_text_height(WIDGET_TITLE_SCALE) + 8, WIDGET_TEXT_SCALE, t->palette->textMuted, cardX + cardW - 14);
        return;
    }

    if(menu->shown != menu->selected) {
        menu->leaving = menu->shown;
        menu->shown = menu->selected;
        menu->shownTime = osGetTime();
    }

    remoteinstall_top_context context = {menu, x1, y1, x2, y2};
    widget_crossfade(widget_ease(menu->shownTime, WIDGET_CROSSFADE_TIME), remoteinstall_draw_card_fade, &context, menu->leaving != menu->shown);

    // The server buttons' cards need the room.
    if(menu->selected < FTP_BUTTON) {
        widget_storage(SYSTEM_MEDIATYPE_SD, x1 + 16, y2 - 52, x2 - x1 - 32);
    }
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

    // The file's name; with more links in the request, how many others come with it.
    char line[FILE_NAME_MAX + 16];
    if(menu->lastCount > 1) {
        snprintf(line, sizeof(line), "%s (+%lu)", menu->lastName, menu->lastCount - 1);
    } else {
        snprintf(line, sizeof(line), "%s", menu->lastName);
    }

    widget_text_fit(line, textX, top + lineH + 1, WIDGET_SMALL_SCALE, muted, textW);

    widget_badge(x + w - 9 - badgeW, y + (h - smallH - 6) / 2, badge, selected ? THEME_ALPHA(t->onAccent, 0x30) : t->accentSoft, selected ? t->onAccent : t->accent);
}

// A small button that turns a server on or off: lit while it runs.
static void remoteinstall_draw_server_button(float x, float y, float w, float h, u32 icon, const char* label, bool on, bool selected) {
    const theme_current* t = theme();
    u32 fill = selected ? t->accent : on ? t->accentSoft : t->palette->surface;
    u32 edge = selected || on ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : on ? t->accent : t->palette->textMuted;
    screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, edge, fill);
    screen_draw_icon(icon, x + (w - 14) / 2, y + 3, 14, ink);
    widget_text_centered(label, x, x + w, y + 3 + 14 + 1, WIDGET_SMALL_SCALE, ink);
    if(on) {
        screen_fill_rect(x + w - 9, y + 4, 4, 4, ink);
    }
}

// Where each tile (by entry) is drawn: it eases towards its place, so while one is dragged the others make way.
static float remoteinstall_anim[ENTRY_COUNT][4];
static bool remoteinstall_anim_valid[ENTRY_COUNT];

static bool remoteinstall_anim_step(u32 e, const float target[4], float out[4]) {
    if(!remoteinstall_anim_valid[e]) {
        memcpy(remoteinstall_anim[e], target, sizeof(remoteinstall_anim[e]));
        remoteinstall_anim_valid[e] = true;
    }

    bool moving = false;
    for(int k = 0; k < 4; k++) {
        float d = target[k] - remoteinstall_anim[e][k];
        if(d > -0.5f && d < 0.5f) {
            remoteinstall_anim[e][k] = target[k];
        } else {
            remoteinstall_anim[e][k] += d * 0.35f;
            moving = true;
        }
    }

    memcpy(out, remoteinstall_anim[e], sizeof(remoteinstall_anim[e]));
    return moving;
}

static void remoteinstall_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    remoteinstall_data* menu = (remoteinstall_data*) data;
    const theme_current* t = theme();
    bool dragging = menu->dragFrom >= 0 && menu->dragMoved;
    bool moving = false;

    // The tile in the hand is drawn last, over the rest.
    for(int pass = 0; pass < 2; pass++) {
        for(u32 i = 0; i < remoteinstall_targets(menu); i++) {
            if(!remoteinstall_has(menu, i)) {
                continue;
            }

            bool isDragged = dragging && i == (u32) menu->dragFrom;
            if(isDragged != (pass == 1)) {
                continue;
            }

            float slot[4];
            remoteinstall_rect(i, x1, y1, x2, &slot[0], &slot[1], &slot[2], &slot[3]);
            float cur[4] = {slot[0], slot[1], slot[2], slot[3]};
            if(i < ENTRY_COUNT) {
                float target[4] = {slot[0], slot[1], slot[2], slot[3]};
                if(isDragged) {
                    target[0] = menu->dragX - slot[2] / 2;
                    target[1] = menu->dragY - slot[3] / 2 - 8;
                    screen_fill_round_rect(slot[0], slot[1], slot[2], slot[3], t->def->radius, THEME_ALPHA(t->palette->border, 0x70));
                }

                moving |= remoteinstall_anim_step(order[i], target, cur);
            }

            float x = cur[0], y = cur[1], w = cur[2], h = cur[3];
            bool selected = i == menu->selected;
            if(i < ENTRY_COUNT && editing && !isDragged) {
                // The tile picked up with the D-Pad is lifted; the others jiggle, each out of step.
                if(selected && menu->grabbed) {
                    y -= 4;
                } else {
                    float phase = (float) (osGetTime() % 100000) * 0.022f + (float) i * 1.9f;
                    x += roundf(sinf(phase) * 1.5f);
                    y += roundf(cosf(phase * 1.3f));
                }
            }

            if(i < ENTRY_COUNT) {
                remoteinstall_draw_tile(x, y, w, h, entries[order[i]].icon, _(entries[order[i]].name), selected);
            } else if(i == FTP_BUTTON) {
                remoteinstall_draw_server_button(x, y, w, h, ICON_SERVER, "FTP", ftpserver_running() || ftpserver_waiting(), selected);
            } else if(i == WEB_BUTTON) {
                remoteinstall_draw_server_button(x, y, w, h, ICON_DEVICE_DESKTOP, "Web", webserver_running() || webserver_waiting(), selected);
            } else {
                remoteinstall_draw_history(menu, x, y, w, h, selected);
            }

            widget_press_draw(&menu->press, (int) i, x, y, w, h, i < ENTRY_COUNT ? t->def->radius : t->def->radiusSmall, selected ? t->onAccent : t->palette->text, !editing);

            // Where a guided tour finds it (coach.h): "remote.qr"..., "remote.history".
            char anchor[32];
            snprintf(anchor, sizeof(anchor), "remote.%s", i < ENTRY_COUNT ? entry_ids[order[i]] : i == FTP_BUTTON ? "ftp" : i == WEB_BUTTON ? "web" : "history");
            coach_anchor(anchor, slot[0], slot[1], slot[2], slot[3], selected);
        }
    }

    // Keep drawing while the tiles are still gliding into place.
    if(moving) {
        ui_wake();
    }
}

void remoteinstall_open() {
    remoteinstall_data* data = (remoteinstall_data*) calloc(1, sizeof(remoteinstall_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate remote install data.");
        return;
    }

    data->press.index = -1;
    data->dragFrom = -1;
    editing = false;
    remoteinstall_order_load();
    remoteinstall_read_last(data);

    ui_view* view = ui_create();
    view->name = _("Remote install");
    view->id = "Remote install";
    view->backArrow = true;
    view->info = _("\uE000 open   \uE001 back");
    view->data = data;
    view->update = remoteinstall_update;
    view->drawTop = remoteinstall_draw_top;
    view->drawBottom = remoteinstall_draw_bottom;
    ui_push(view);
}
