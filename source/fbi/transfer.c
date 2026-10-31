#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <3ds.h>

#include "resources.h"
#include "selftest.h"
#include "transfer.h"
#include "transferconn.h"
#include "udslink.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"

#define TRANSFER_DISCOVERY_PORT 5001

#define TRANSFER_NAME_MAX 32
#define TRANSFER_RECEIVED_DIR APP_DATA_DIR "received/"

// A transfer cut off in the middle can go on from the last place the receiver
// wrote down; this much before it is sent again, in case it had not reached
// the card.
#define TRANSFER_RESUME_MARGIN (1024 * 1024)
// The receiver writes that place down after this many more bytes.
#define TRANSFER_POSITION_EVERY (4 * 1024 * 1024)
#define TRANSFER_CHECK_BYTES 4096
#define TRANSFER_PROTOCOL 5

// What the bytes after the hello are.
#define TRANSFER_KIND_FILE 0
#define TRANSFER_KIND_TITLE 1   // A title package (exportcia.c): the receiver builds a CIA.

// The answer: a status (1 go ahead, 0 refused), the byte to go on from and the
// transfer's number, which the extra connections quote.
#define TRANSFER_ANSWER_SIZE 13

// How long a search on the Wi-Fi may find nobody before looking nearby.
#define TRANSFER_NEARBY_AFTER 7000

// How long the Receive screen waits on the Wi-Fi before it suggests the other way.
#define TRANSFER_RECEIVE_HINT_AFTER 15000

// Changing between the Wi-Fi and the local wireless takes the radio over, and the console
// does not answer for some seconds. So the change is asked for first, the screen says
// "Switching..." and the change is made a couple of frames later.
#define TRANSFER_MODE_NONE 0
#define TRANSFER_MODE_NEARBY 1
#define TRANSFER_MODE_WIFI 2

#define TRANSFER_PROBE_INTERVAL 2000
#define TRANSFER_HELLO_TIMEOUT 5000

static const char TRANSFER_MAGIC[4] = {'N', 'S', 'A', 'T'};
static const char TRANSFER_PROBE[4] = {'N', 'S', 'A', '?'};
static const char TRANSFER_ANSWER[4] = {'N', 'S', 'A', '!'};

// What a sender says first. Numbers are big endian; 316 bytes, no padding.
// `check` is the CRC-32 of the file's first 4 KB (of a title package's header):
// it tells the receiver that a partial file it has is the same one. `head` is the
// size of that header, which is sent again when a transfer goes on.
typedef struct {
    char magic[4];
    u32 version;
    char sender[TRANSFER_NAME_MAX];
    char file[FILE_NAME_MAX];
    u32 sizeHigh;
    u32 sizeLow;
    u32 check;
    u32 kind;
    u32 head;
} transfer_hello;

// ---- shared ----

static const char* transfer_model_name(u8 model) {
    switch(model) {
        case CFG_MODEL_3DS: return "3DS";
        case CFG_MODEL_3DSXL: return "3DS XL";
        case CFG_MODEL_N3DS: return "New 3DS";
        case CFG_MODEL_2DS: return "2DS";
        case CFG_MODEL_N3DSXL: return "New 3DS XL";
        case CFG_MODEL_N2DSXL: return "New 2DS XL";
        default: return "3DS";
    }
}

// "New 3DS of Marta": the model and the user name set in System Settings.
void transfer_local_name(char* out, size_t size) {
    u8 model = 0;
    const char* modelName = R_SUCCEEDED(CFGU_GetSystemModel(&model)) ? transfer_model_name(model) : "3DS";

    char user[32] = "";
    u16 name[0x1C / 2 + 1] = {0};
    if(R_SUCCEEDED(CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, name))) {
        name[10] = 0;
        ssize_t len = utf16_to_utf8((uint8_t*) user, name, sizeof(user) - 1);
        user[len > 0 ? len : 0] = '\0';
    }

    if(user[0] != '\0') {
        snprintf(out, size, "%s - %s", modelName, user);
    } else {
        snprintf(out, size, "%s", modelName);
    }
}

static void transfer_set_nonblocking(int sock) {
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
}

static void transfer_set_buffers(int sock) {
    transfer_conn_set_buffers(sock);
}

static void transfer_close(int* sock) {
    if(*sock >= 0) {
        close(*sock);
        *sock = -1;
    }
}

// Sends or receives exactly `len` bytes on a non-blocking socket, giving up
// when `cancel` is signalled or nothing moves for a while.
static Result transfer_io(int sock, bool write, void* buf, size_t len, Handle cancel) {
    return transfer_conn_io(sock, write, buf, len, cancel, NULL);
}

static void transfer_ip_text(char* out, size_t size) {
    struct in_addr addr = {(in_addr_t) gethostid()};
    snprintf(out, size, "%s", inet_ntoa(addr));
}

#define transfer_log udslink_log

static void transfer_size_text(char* out, size_t size, u64 bytes) {
    snprintf(out, size, "%.2f %s", ui_get_display_size(bytes), ui_get_display_size_units(bytes));
}

static u32 transfer_crc32(u32 crc, const u8* data, size_t size) {
    crc = ~crc;
    for(size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for(int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
        }
    }

    return ~crc;
}

static void transfer_put64(u8* out, u64 value) {
    for(int i = 0; i < 8; i++) {
        out[i] = (u8) (value >> (56 - 8 * i));
    }
}

static u64 transfer_get64(const u8* in) {
    u64 value = 0;
    for(int i = 0; i < 8; i++) {
        value = (value << 8) | in[i];
    }

    return value;
}

// A card on the top screen: an icon, a line and a smaller one.
static void transfer_draw_card(float x1, float y1, float x2, float y2, u32 icon, const char* line1, const char* line2) {
    const theme_current* t = theme();

    float w = x2 - x1;
    float h = y2 - y1;
    float cx = x1 + w / 2;
    float cy = y1 + h / 2;

    screen_draw_icon(icon, cx - 16, cy - 50, 32, t->accent);
    widget_text_centered(line1, x1 + 10, x2 - 10, cy - 4, WIDGET_TITLE_SCALE, t->palette->text);
    if(line2 != NULL && line2[0] != '\0') {
        widget_text_centered(line2, x1 + 10, x2 - 10, cy + 20, WIDGET_TEXT_SCALE, t->palette->textMuted);
    }
}

// ---- sending ----

static FS_Archive transfer_sd_archive();

typedef struct {
    char name[TRANSFER_NAME_MAX + 1];
    struct in_addr addr;
    bool uds;                 // Found over the local wireless (no router).
    bool hint;                // Not a console: the row that offers to look nearby.
    udslink_peer udsPeer;
} transfer_peer;

typedef struct {
    FS_Archive archive;
    char path[FILE_PATH_MAX];
    char name[FILE_NAME_MAX];
    u64 size;
    export_cia_data* stream;   // A title to send as a package, instead of a file.

    int udp;
    u64 lastProbe;
    u64 openedAt;
    bool udsMode;             // Looking over the local wireless instead of the Wi-Fi.
    bool autoDone;            // Tests: the first console found has been sent to.
    bool hintShown;           // The "look nearby" row is in the list.
    bool toggleRequested;     // The tool (or X) asked to change between the Wi-Fi and nearby.
    bool waitingWifi;         // Left the local wireless: the Wi-Fi is coming back.
    u32 pendingMode;          // A change that has been asked for and is about to be made (see below).
    u32 pendingFrames;
    char udsNames[UDSLINK_PEERS_MAX][UDSLINK_NAME_MAX + 1];
    u32 udsCount;
    struct in_addr known[8];   // Consoles listed in /nsa/peers.txt, asked directly.
    u32 knownCount;
} transfer_send_data;

typedef enum {
    SEND_WAITING,
    SEND_SENDING,
    SEND_FINISHING
} transfer_send_state;

typedef struct {
    FS_Archive archive;
    char path[FILE_PATH_MAX];
    char name[FILE_NAME_MAX];
    u64 size;
    export_cia_data* stream;   // Owned by the list.

    char peerName[TRANSFER_NAME_MAX + 1];
    struct in_addr addr;
    bool uds;
    udslink_peer udsPeer;
    transfer_link link;
    transfer_send_state state;

    u32 check;
    u32 head;           // Bytes of a title package that are its header.
    u8 answer[TRANSFER_ANSWER_SIZE];
    u32 answerGot;
    u64 start;          // The payload is sent from here on.

    transfer_sender* engine;
    u64 sent;
    u64 speedTime;
    u64 speedSent;
    u32 speed;
    u64 engineStart;
} transfer_send_session;

// Fills the chunk from the file, or from the package: its header is in memory
// and each content is read from the system.
static Result transfer_send_read(void* user, const transfer_chunk* chunk, void* buffer) {
    transfer_send_session* session = (transfer_send_session*) user;

    if(selftest_fault("send.read")) {
        return R_APP_IO_ERROR;
    }

    if(session->stream != NULL) {
        if(chunk->segment == 0) {
            u32 headerSize = 0;
            const u8* header = action_export_cia_stream_header(session->stream, &headerSize);
            memcpy(buffer, header + chunk->segmentOffset, chunk->len);
            return 0;
        }

        return action_export_cia_stream_read(session->stream, chunk->segment - 1, chunk->segmentOffset, buffer, chunk->len);
    }

    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(session->path);
    if(fsPath != NULL) {
        Handle file = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenFile(&file, session->archive, *fsPath, FS_OPEN_READ, 0))) {
            u32 bytesRead = 0;
            res = FSFILE_Read(file, &bytesRead, chunk->segmentOffset, buffer, chunk->len);
            if(R_SUCCEEDED(res) && bytesRead != chunk->len) {
                res = R_APP_IO_ERROR;
            }

            FSFILE_Close(file);
        }

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static void transfer_send_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    transfer_send_session* session = (transfer_send_session*) data;

    char size[32];
    transfer_size_text(size, sizeof(size), session->size);
    transfer_draw_card(x1, y1, x2, y2, ICON_3DS, session->name, size);
}

static void transfer_send_free(transfer_send_session* session) {
    transfer_sender_free(session->engine);
    transfer_link_close(&session->link);
    free(session);
}

static void transfer_send_finish(ui_view* view, transfer_send_session* session) {
    ui_pop();
    info_destroy(view);

    transfer_send_free(session);
}

static bool transfer_send_connect(transfer_send_session* session);
static void transfer_send_update(ui_view* view, void* data, float* progress, char* text);

static void transfer_send_retry_onresponse(ui_view* view, void* data, u32 response) {
    transfer_send_session* session = (transfer_send_session*) data;

    if(response == PROMPT_YES && transfer_send_connect(session)) {
        info_display("Sending", "\uE001 cancel", true, session, transfer_send_update, transfer_send_draw_top);
        return;
    }

    transfer_send_free(session);
}

// The send failed: the error is shown, and then the question of trying again,
// with the same console and the same file. Unless the user of the other console
// cancelled: then it is said, and that is all (sending again is a new request).
static void transfer_send_failed(ui_view* view, transfer_send_session* session, Result res) {
    ui_pop();
    info_destroy(view);

    transfer_sender_free(session->engine);
    session->engine = NULL;

    u8 status = 0;
    int got = -1;
    for(u64 begin = osGetTime(); osGetTime() - begin < 1500; svcSleepThread(20000000)) {
        got = transfer_link_recv_some(&session->link, &status, 1);
        if(got != -1) {
            break;
        }
    }

    if(got == 1 && status == 2) {
        transfer_log("send: the other console cancelled\n");
        transfer_send_free(session);

        prompt_display_notify("Failure", "The other console cancelled the transfer.", COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    transfer_link_close(&session->link);

    // The second one pushed is the one on top.
    prompt_display_yes_no("Confirmation", "Retry?", COLOR_TEXT, session, NULL, transfer_send_retry_onresponse);
    error_display_res(NULL, NULL, res, "Failed to send the file.");
}

// The receiver agreed: cuts the payload in chunks from where it says to go on
// and starts the connections.
static Result transfer_send_begin_engine(transfer_send_session* session, u32 id) {
    u32 segmentCount = 1;
    if(session->stream != NULL) {
        segmentCount = 1 + action_export_cia_stream_content_count(session->stream);
    }

    u64* segments = (u64*) calloc(segmentCount, sizeof(u64));
    if(segments == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    if(session->stream != NULL) {
        u32 headerSize = 0;
        action_export_cia_stream_header(session->stream, &headerSize);
        segments[0] = headerSize;
        for(u32 i = 1; i < segmentCount; i++) {
            segments[i] = action_export_cia_stream_content_size(session->stream, i - 1);
        }
    } else {
        segments[0] = session->size;
    }

    u32 count = 0;
    transfer_chunk* chunks = transfer_chunks_build(segments, segmentCount, session->start, session->stream != NULL && session->start > 0, &count);
    free(segments);
    if(chunks == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = transfer_sender_start(&session->engine, &session->link, session->addr, id, chunks, count, transfer_send_read, session);
    if(R_SUCCEEDED(res)) {
        session->sent = 0;
        session->speed = 0;
        session->speedSent = 0;
        session->speedTime = osGetTime();
        session->engineStart = session->speedTime;
    }

    return res;
}

static void transfer_send_update(ui_view* view, void* data, float* progress, char* text) {
    transfer_send_session* session = (transfer_send_session*) data;

    if(session->state == SEND_SENDING) {
        Result result = 0;
        bool done = transfer_sender_progress(session->engine, &session->sent, &result);

        if(done) {
            transfer_log("send %s link=%s sent=%llu secs=%.1f result=%08lX\n", session->name, session->uds ? "uds" : "ip",
                         session->sent, (osGetTime() - session->engineStart) / 1000.0, result);

            if(result == R_APP_CANCELLED) {
                transfer_send_finish(view, session);
            } else if(R_FAILED(result)) {
                transfer_send_failed(view, session, result);
            } else {
                // Everything is sent; the other console may still be putting it together.
                transfer_sender_free(session->engine);
                session->engine = NULL;
                session->state = SEND_FINISHING;
            }

            return;
        }

        if(hidKeysDown() & KEY_B) {
            transfer_sender_cancel(session->engine);
        }

        u64 now = osGetTime();
        if(now - session->speedTime >= 1000) {
            session->speed = (u32) ((session->sent - session->speedSent) * 1000 / (now - session->speedTime));
            session->speedSent = session->sent;
            session->speedTime = now;
        }

        // Where the file is, not what this try sent: a resume starts part way and
        // sends a title package's header again first.
        u64 again = session->stream != NULL && session->start > 0 ? session->head : 0;
        u64 at = session->start + (session->sent > again ? session->sent - again : 0);
        if(at > session->size) {
            at = session->size;
        }

        u64 total = session->size;
        u32 eta = session->speed > 0 ? (u32) ((total - at) / session->speed) : 0;

        *progress = total != 0 ? (float) ((double) at / (double) total) : 0;
        snprintf(text, PROGRESS_TEXT_MAX, _("To %s\n%.2f %s / %.2f %s\n%.2f %s/s, ETA %s"), session->peerName,
                 ui_get_display_size(at), ui_get_display_size_units(at),
                 ui_get_display_size(total), ui_get_display_size_units(total),
                 ui_get_display_size(session->speed), ui_get_display_size_units(session->speed),
                 ui_get_display_eta(eta));
        return;
    }

    if(hidKeysDown() & KEY_B) {
        transfer_send_finish(view, session);
        return;
    }

    if(session->state == SEND_FINISHING) {
        // One byte once the other console has the file whole (a title takes a
        // while to check).
        u8 ack = 0;
        int n = transfer_link_recv_some(&session->link, &ack, 1);
        if(n == 1 || n == 0 || n == -2) {
            bool ok = n == 1 && ack == 1;
            if(n == 1 && ack == 2) {
                transfer_log("send: the other console cancelled\n");
                transfer_send_finish(view, session);

                prompt_display_notify("Failure", "The other console cancelled the transfer.", COLOR_TEXT, NULL, NULL, NULL);
                return;
            }

            transfer_log("send: final answer %s (n=%d)\n", ok ? "whole" : "NOT whole", n);

            if(ok) {
                transfer_send_finish(view, session);

                prompt_display_notify("Success", "File sent.", COLOR_TEXT, NULL, NULL, NULL);
            } else {
                transfer_send_failed(view, session, R_APP_IO_ERROR);
            }

            return;
        }

        *progress = 1;
        snprintf(text, PROGRESS_TEXT_MAX, _("Waiting for %s to finish..."), session->peerName);
        return;
    }

    // SEND_WAITING: the answer, once the other user has decided.
    int n = transfer_link_recv_some(&session->link, session->answer + session->answerGot, sizeof(session->answer) - session->answerGot);
    if(n > 0) {
        session->answerGot += (u32) n;
    }

    if(n == 0 || n == -2 || session->answerGot == sizeof(session->answer)) {
        bool answered = session->answerGot == sizeof(session->answer);

        if(answered && session->answer[0] == 1) {
            u64 start = transfer_get64(session->answer + 1);
            u32 id = ((u32) session->answer[9] << 24) | ((u32) session->answer[10] << 16) | ((u32) session->answer[11] << 8) | session->answer[12];
            session->start = start < session->size && (session->stream == NULL || start >= session->head) ? start : 0;

            transfer_log("send: accepted, going on from byte %llu\n", session->start);
            Result res = transfer_send_begin_engine(session, id);
            if(R_SUCCEEDED(res)) {
                session->state = SEND_SENDING;
                *progress = 0;
                return;
            }

            transfer_send_failed(view, session, res);
            return;
        }

        transfer_log("send: %s\n", answered ? "declined by the other console" : "the other console closed the connection");
        transfer_send_finish(view, session);

        prompt_display_notify("Failure", answered ? "The other console declined the file." : "The other console closed the connection.", COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    snprintf(text, PROGRESS_TEXT_MAX, _("Waiting for %s to accept..."), session->peerName);
}

// The CRC-32 of the file's first bytes, which says "this is the same file".
static u32 transfer_file_check(FS_Archive archive, const char* path, u64 size) {
    u32 crc = 0;

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath != NULL) {
        Handle file = 0;
        if(R_SUCCEEDED(FSUSER_OpenFile(&file, archive, *fsPath, FS_OPEN_READ, 0))) {
            u8 buffer[TRANSFER_CHECK_BYTES];
            u32 want = size < sizeof(buffer) ? (u32) size : sizeof(buffer);
            u32 read = 0;
            if(R_SUCCEEDED(FSFILE_Read(file, &read, 0, buffer, want))) {
                crc = transfer_crc32(0, buffer, read);
            }

            FSFILE_Close(file);
        }

        fs_free_path_utf8(fsPath);
    }

    return crc;
}

// Opens the connection to the receiver and says what is coming. False (the
// reason shown) when it could not.
static bool transfer_send_connect(transfer_send_session* session) {
    transfer_link_close(&session->link);
    session->answerGot = 0;
    session->state = SEND_WAITING;

    if(session->uds) {
        Result res = udslink_connect(&session->udsPeer, &session->link.uds);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Could not connect to the other console.");
            return false;
        }

        session->link.fd = -1;
    } else {
        session->link.fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if(session->link.fd < 0) {
            error_display_errno(NULL, NULL, errno, "Failed to open socket.");
            return false;
        }

        transfer_set_buffers(session->link.fd);

        struct sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(TRANSFER_PORT);
        to.sin_addr = session->addr;

        // Blocking: the console answered a probe a moment ago, and a non-blocking
        // connect does not report its outcome reliably on the 3DS.
        if(connect(session->link.fd, (struct sockaddr*) &to, sizeof(to)) < 0) {
            error_display_errno(NULL, NULL, errno, "Could not connect to the other console.");
            return false;
        }

        transfer_set_nonblocking(session->link.fd);
    }

    transfer_log("send: %s (%llu bytes, %s) to %s over %s\n", session->name, session->size, session->stream != NULL ? "title" : "file", session->peerName, session->uds ? "nearby" : "Wi-Fi");

    transfer_hello hello;
    memset(&hello, 0, sizeof(hello));
    memcpy(hello.magic, TRANSFER_MAGIC, sizeof(hello.magic));
    hello.version = htonl(TRANSFER_PROTOCOL);
    transfer_local_name(hello.sender, sizeof(hello.sender));
    string_copy(hello.file, session->name, sizeof(hello.file));
    hello.sizeHigh = htonl((u32) (session->size >> 32));
    hello.sizeLow = htonl((u32) (session->size & 0xFFFFFFFF));
    hello.check = htonl(session->check);
    hello.kind = htonl(session->stream != NULL ? TRANSFER_KIND_TITLE : TRANSFER_KIND_FILE);
    hello.head = htonl(session->head);

    Result res = transfer_link_io(&session->link, true, &hello, sizeof(hello), 0, NULL);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Could not connect to the other console.");
        return false;
    }

    return true;
}

static void transfer_send_to(transfer_send_data* source, const transfer_peer* peer) {
    transfer_send_session* session = (transfer_send_session*) calloc(1, sizeof(transfer_send_session));
    if(session == NULL) {
        error_display(NULL, NULL, "Failed to allocate transfer data.");
        return;
    }

    session->link.fd = -1;
    session->archive = source->archive;
    string_copy(session->path, source->path, sizeof(session->path));
    string_copy(session->name, source->name, sizeof(session->name));
    session->size = source->size;
    session->stream = source->stream;
    if(source->stream != NULL) {
        u32 headerSize = 0;
        const u8* header = action_export_cia_stream_header(source->stream, &headerSize);
        session->head = headerSize;
        session->check = transfer_crc32(0, header, headerSize < TRANSFER_CHECK_BYTES ? headerSize : TRANSFER_CHECK_BYTES);
    } else {
        session->check = transfer_file_check(source->archive, source->path, source->size);
    }

    string_copy(session->peerName, peer->name, sizeof(session->peerName));
    session->addr = peer->addr;
    session->uds = peer->uds;
    session->udsPeer = peer->udsPeer;

    if(!transfer_send_connect(session)) {
        transfer_send_free(session);
        return;
    }

    info_display("Sending", "\uE001 cancel", true, session, transfer_send_update, transfer_send_draw_top);
}

// /nsa/peers.txt lists consoles to ask directly, one address per line: for a
// network that does not pass broadcasts (and for tests).
static void transfer_send_load_known(transfer_send_data* data) {
    Handle file = 0;
    if(R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, APP_DATA_DIR "peers.txt"), FS_OPEN_READ, 0))) {
        return;
    }

    char text[512];
    u32 read = 0;
    if(R_SUCCEEDED(FSFILE_Read(file, &read, 0, text, sizeof(text) - 1))) {
        text[read] = '\0';

        for(const char* c = text; *c != '\0' && data->knownCount < 8;) {
            unsigned a, b, d, e;
            if(sscanf(c, "%u.%u.%u.%u", &a, &b, &d, &e) == 4 && a < 256 && b < 256 && d < 256 && e < 256) {
                data->known[data->knownCount++].s_addr = htonl((a << 24) | (b << 16) | (d << 8) | e);
            }

            while(*c != '\0' && *c != '\n') {
                c++;
            }

            if(*c == '\n') {
                c++;
            }
        }
    }

    FSFILE_Close(file);
}

static void transfer_send_probe(transfer_send_data* data) {
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(TRANSFER_DISCOVERY_PORT);

    // The limited broadcast, and the one of the local /24 network.
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(data->udp, TRANSFER_PROBE, sizeof(TRANSFER_PROBE), 0, (struct sockaddr*) &to, sizeof(to));

    to.sin_addr.s_addr = (in_addr_t) gethostid() | 0xFF000000;
    sendto(data->udp, TRANSFER_PROBE, sizeof(TRANSFER_PROBE), 0, (struct sockaddr*) &to, sizeof(to));

    for(u32 i = 0; i < data->knownCount; i++) {
        to.sin_addr = data->known[i];
        sendto(data->udp, TRANSFER_PROBE, sizeof(TRANSFER_PROBE), 0, (struct sockaddr*) &to, sizeof(to));
    }
}

static void transfer_send_collect(transfer_send_data* data, linked_list* items) {
    for(int i = 0; i < 8; i++) {
        u8 packet[sizeof(TRANSFER_ANSWER) + TRANSFER_NAME_MAX];
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);

        ssize_t n = recvfrom(data->udp, packet, sizeof(packet), 0, (struct sockaddr*) &from, &fromLen);
        if(n < (ssize_t) sizeof(TRANSFER_ANSWER) || memcmp(packet, TRANSFER_ANSWER, sizeof(TRANSFER_ANSWER)) != 0) {
            if(n < 0) {
                break;
            }

            continue;
        }

        bool known = false;
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            transfer_peer* peer = (transfer_peer*) ((list_item*) linked_list_iter_next(&iter))->data;
            if(peer->addr.s_addr == from.sin_addr.s_addr) {
                known = true;
                break;
            }
        }

        if(known) {
            continue;
        }

        list_item* item = (list_item*) calloc(1, sizeof(list_item));
        transfer_peer* peer = (transfer_peer*) calloc(1, sizeof(transfer_peer));
        if(item == NULL || peer == NULL) {
            free(item);
            free(peer);
            continue;
        }

        size_t nameLen = (size_t) n - sizeof(TRANSFER_ANSWER);
        memcpy(peer->name, packet + sizeof(TRANSFER_ANSWER), nameLen);
        peer->name[nameLen] = '\0';
        peer->addr = from.sin_addr;

        string_copy(item->name, peer->name, sizeof(item->name));
        item->color = COLOR_TEXT;
        item->data = peer;
        linked_list_add(items, item);
    }
}

static void transfer_send_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    const transfer_peer* peer = (const transfer_peer*) item->data;

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    if(peer->hint) {
        screen_draw_icon(ICON_WIFI_OFF, x + 9, y + (height - 16) / 2, 16, selected ? t->onAccent : t->accent);
        widget_text_fit(_("Search nearby (no Wi-Fi)"), x + 33, y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink, x + width - 9 - (x + 33));
        return;
    }

    screen_draw_icon(ICON_3DS, x + 9, y + (height - 16) / 2, 16, selected ? t->onAccent : t->accent);

    char ip[24];
    snprintf(ip, sizeof(ip), "%s", peer->uds ? _("Nearby") : inet_ntoa(peer->addr));
    float ipW = widget_text_width(ip, WIDGET_SMALL_SCALE);
    float right = x + width - 9;
    screen_draw_text(ip, right - ipW, y + (height - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, muted);

    float tx = x + 33;
    widget_text_fit(peer->name, tx, y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink, right - ipW - 8 - tx);
}

static void transfer_send_list_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    transfer_send_data* sendData = (transfer_send_data*) data;

    char size[32];
    transfer_size_text(size, sizeof(size), sendData->size);

    char line[FILE_NAME_MAX + 48];
    snprintf(line, sizeof(line), "%s  (%s)", sendData->name, size);
    if(sendData->pendingMode == TRANSFER_MODE_NEARBY) {
        snprintf(line, sizeof(line), "%s", _("Switching to nearby mode..."));
    } else if(sendData->pendingMode == TRANSFER_MODE_WIFI) {
        snprintf(line, sizeof(line), "%s", _("Switching to the Wi-Fi..."));
    } else if(sendData->waitingWifi) {
        snprintf(line, sizeof(line), "%s", _("Waiting for the Wi-Fi..."));
    }

    transfer_draw_card(x1, y1, x2, y2, ICON_3DS, _("Send to another 3DS"), line);
}

static void transfer_send_list_exit(ui_view* view, transfer_send_data* data, linked_list* items) {
    ui_pop();

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        free(item->data);
        free(item);
        linked_list_iter_remove(&iter);
    }

    transfer_close(&data->udp);
    if(data->udsMode) {
        udslink_leave();
    }

    if(data->stream != NULL) {
        action_export_cia_stream_free(data->stream);
    }

    list_destroy(view);
    free(data);
}

// The consoles found over the local wireless, as list rows.
static void transfer_send_sync_nearby(transfer_send_data* data, linked_list* items) {
    udslink_peer found[UDSLINK_PEERS_MAX];
    u32 count = udslink_scan_results(found, UDSLINK_PEERS_MAX);

    bool same = count == data->udsCount;
    for(u32 i = 0; same && i < count; i++) {
        same = strcmp(found[i].name, data->udsNames[i]) == 0;
    }

    if(same) {
        return;
    }

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        free(item->data);
        free(item);
        linked_list_iter_remove(&iter);
    }

    data->udsCount = count;
    for(u32 i = 0; i < count; i++) {
        string_copy(data->udsNames[i], found[i].name, sizeof(data->udsNames[i]));

        list_item* item = (list_item*) calloc(1, sizeof(list_item));
        transfer_peer* peer = (transfer_peer*) calloc(1, sizeof(transfer_peer));
        if(item == NULL || peer == NULL) {
            free(item);
            free(peer);
            continue;
        }

        string_copy(peer->name, found[i].name, sizeof(peer->name));
        peer->uds = true;
        peer->udsPeer = found[i];

        string_copy(item->name, peer->name, sizeof(item->name));
        item->color = COLOR_TEXT;
        item->data = peer;
        linked_list_add(items, item);
    }
}

static bool transfer_send_open_udp(transfer_send_data* data);

static void transfer_send_clear_items(transfer_send_data* data, linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        free(item->data);
        free(item);
        linked_list_iter_remove(&iter);
    }
}

static void transfer_send_remove_hint(transfer_send_data* data, linked_list* items) {
    if(!data->hintShown) {
        return;
    }

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        if(((transfer_peer*) item->data)->hint) {
            free(item->data);
            free(item);
            linked_list_iter_remove(&iter);
        }
    }

    data->hintShown = false;
}

// Nobody answered on the Wi-Fi for a while: a row offers to look nearby over the
// local wireless. It is a choice because it takes the console off the Wi-Fi.
static void transfer_send_add_hint(transfer_send_data* data, linked_list* items) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    transfer_peer* peer = (transfer_peer*) calloc(1, sizeof(transfer_peer));
    if(item == NULL || peer == NULL) {
        free(item);
        free(peer);
        return;
    }

    peer->hint = true;
    string_copy(item->name, "Search nearby (no Wi-Fi)", sizeof(item->name));
    item->color = COLOR_TEXT;
    item->data = peer;
    linked_list_add(items, item);
    data->hintShown = true;
}

static void transfer_send_list_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    transfer_send_data* sendData = (transfer_send_data*) data;

    if(hidKeysDown() & KEY_B) {
        transfer_send_list_exit(view, sendData, items);
        return;
    }

    if(hidKeysDown() & KEY_X) {
        sendData->toggleRequested = true;
    }

    if(sendData->toggleRequested && !sendData->waitingWifi && sendData->pendingMode == TRANSFER_MODE_NONE) {
        sendData->toggleRequested = false;
        transfer_send_remove_hint(sendData, items);
        transfer_send_clear_items(sendData, items);
        sendData->udsCount = 0;
        sendData->pendingMode = sendData->udsMode ? TRANSFER_MODE_WIFI : TRANSFER_MODE_NEARBY;
        sendData->pendingFrames = 0;
        return;
    }

    sendData->toggleRequested = false;

    if(sendData->pendingMode != TRANSFER_MODE_NONE) {
        // The screen has said "Switching...": now the change, which takes a while.
        if(++sendData->pendingFrames >= 2) {
            if(sendData->pendingMode == TRANSFER_MODE_WIFI) {
                // Back to the Wi-Fi: the sockets wait until it is there.
                udslink_scan_stop();
                udslink_leave();
                sendData->udsMode = false;
                sendData->waitingWifi = true;
            } else if(udslink_enter()) {
                transfer_close(&sendData->udp);
                sendData->udsMode = true;
                udslink_scan_start();
            }

            sendData->pendingMode = TRANSFER_MODE_NONE;
            sendData->openedAt = osGetTime();
        }

        return;
    }

    if(sendData->waitingWifi) {
        if(udslink_wifi_connected() && transfer_send_open_udp(sendData)) {
            sendData->waitingWifi = false;
            sendData->openedAt = osGetTime();
        }

        return;
    }

    u64 now = osGetTime();
    if(sendData->udsMode) {
        // Joining a console stops the scan; back on this list the send is over
        // (its link closed with it), so look again. Nothing if it is running.
        udslink_scan_start();
        transfer_send_sync_nearby(sendData, items);
    } else {
        if(now - sendData->lastProbe >= TRANSFER_PROBE_INTERVAL) {
            sendData->lastProbe = now;
            transfer_send_probe(sendData);
        }

        transfer_send_collect(sendData, items);

        u32 count = linked_list_size(items) - (sendData->hintShown ? 1 : 0);
        if(count > 0) {
            transfer_send_remove_hint(sendData, items);
        } else if(!sendData->hintShown && now - sendData->openedAt > TRANSFER_NEARBY_AFTER) {
            transfer_send_add_hint(sendData, items);
        }
    }

    if(udslink_auto() && sendData->udsMode && selected != NULL && now - sendData->openedAt > 4000 && !sendData->autoDone) {
        sendData->autoDone = true;
        transfer_send_to(sendData, (const transfer_peer*) selected->data);
        return;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        if(((const transfer_peer*) selected->data)->hint) {
            // The other console is waiting without a Wi-Fi network (it makes its
            // own): look for it over the local wireless.
            transfer_log("send list: looking nearby (the row was chosen)\n");
            transfer_send_remove_hint(sendData, items);
            sendData->udsCount = 0;
            sendData->pendingMode = TRANSFER_MODE_NEARBY;
            sendData->pendingFrames = 0;
            return;
        }

        transfer_send_to(sendData, (const transfer_peer*) selected->data);
    }
}

// The socket that asks the Wi-Fi who is receiving. False (the reason shown) when it can not.
static bool transfer_send_open_udp(transfer_send_data* data) {
    data->udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(data->udp < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to open socket.");
        return false;
    }

    int yes = 1;
    setsockopt(data->udp, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    transfer_set_nonblocking(data->udp);
    transfer_send_load_known(data);
    return true;
}

static bool transfer_send_tool_active(ui_view* view, void* data) {
    return ((transfer_send_data*) data)->udsMode;
}

static void transfer_send_tool_press(ui_view* view, void* data) {
    ((transfer_send_data*) data)->toggleRequested = true;
}

static void transfer_send_begin(FS_Archive archive, const char* path, const char* name, u64 size, export_cia_data* stream) {
    if(size == 0) {
        prompt_display_notify("Failure", "The file is empty.", COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    transfer_send_data* data = (transfer_send_data*) calloc(1, sizeof(transfer_send_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate transfer data.");

        if(stream != NULL) {
            action_export_cia_stream_free(stream);
        }

        return;
    }

    data->stream = stream;

    data->archive = archive;
    string_copy(data->path, path, sizeof(data->path));
    string_copy(data->name, name, sizeof(data->name));
    data->size = size;

    data->openedAt = osGetTime();
    data->udp = -1;

    bool wifi = udslink_wifi_connected();
    transfer_log("send list opened: wifi=%d\n", wifi ? 1 : 0);
    if(!wifi) {
        // No Wi-Fi: the consoles nearby that are waiting (once the screen is up, so it can say so).
        data->pendingMode = TRANSFER_MODE_NEARBY;
    } else {
        if(!transfer_send_open_udp(data)) {
            if(stream != NULL) {
                action_export_cia_stream_free(stream);
            }

            free(data);
            return;
        }
    }

    ui_view* view = list_display("Send to...", "\uE000 send   \uE002 nearby / Wi-Fi   \uE001 back", data, transfer_send_list_update, transfer_send_list_draw_top);
    if(view == NULL) {
        transfer_close(&data->udp);
        if(data->udsMode) {
            udslink_leave();
        }

        if(stream != NULL) {
            action_export_cia_stream_free(stream);
        }

        free(data);
        return;
    }

    list_style style = {
            .rowHeight = 30,
            .drawRow = transfer_send_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
    list_add_tool(view, ICON_WIFI_OFF, transfer_send_tool_active, transfer_send_tool_press);
}

void transfer_send_file(FS_Archive archive, const char* path, const char* name, u64 size) {
    transfer_send_begin(archive, path, name, size, NULL);
}

void transfer_send_stream(export_cia_data* stream, const char* name, u64 size) {
    transfer_send_begin(0, "", name, size, stream);
}

// ---- receiving ----

typedef enum {
    RECEIVE_IDLE,
    RECEIVE_HELLO,
    RECEIVE_ASKING,
    RECEIVE_RECEIVING
} transfer_receive_state;

typedef struct {
    int udp;
    int tcp;
    transfer_link client;
    bool uds;                // Waiting over the local wireless (the Wi-Fi is off).
    bool waitingWifi;        // Left the local wireless: the Wi-Fi is coming back.
    u32 pendingMode;         // A change asked for, about to be made (see TRANSFER_MODE_*).
    u32 pendingFrames;
    u64 modeSince;           // When the mode (or the wait) began.
    u64 ignoreUntil;         // A link has just ended: the console that left may not have yet.
    u64 lastAccept;

    char localName[TRANSFER_NAME_MAX];
    transfer_receive_state state;

    transfer_hello hello;
    u32 got;
    u64 helloStart;

    char sender[TRANSFER_NAME_MAX + 1];
    char file[FILE_NAME_MAX];
    u64 size;
    char message[FILE_NAME_MAX + 128];

    u32 check;
    u32 kind;
    u64 start;              // Already here from an earlier try.
    // What was chosen for a transfer that failed: the same file coming again
    // goes ahead the same way, without asking.
    bool haveLast;
    char lastSender[TRANSFER_NAME_MAX + 1];
    char lastFile[FILE_NAME_MAX];
    u64 lastSize;
    u32 lastKind;
    u32 lastCheck;
    bool lastInstall;
    char lastDir[FILE_PATH_MAX];

    char dir[FILE_PATH_MAX];
    char part[FILE_PATH_MAX];   // <name>.part while it arrives.
    char position[FILE_PATH_MAX];   // <name>.part.pos: how far it is whole.
    char dst[FILE_PATH_MAX];
    bool isCia;
    bool isSave;        // A save package (savetransfer.c): "install" means import.
    bool canImport;
    bool install;

    // The transfer itself.
    transfer_receiver* engine;
    Handle file_;                    // The .part of a file.
    export_cia_data* sink;           // Builds the CIA of a title package.
    LightLock fileLock;
    u32 head;                        // Header bytes of a title package (sent again on a resume).
    u64 prefix;                      // Every payload byte before this one is written.
    struct {
        u64 offset;
        u32 len;
    } pending[16];                   // Written chunks that are past `prefix`, not joined to it yet.
    u32 pendingCount;
    u64 positionSaved;

    Thread finalizer;                // Checks a title package once it is all here.
    volatile bool finalDone;
    Result finalResult;
    bool finalizing;

    u64 received;
    u64 speedTime;
    u64 speedReceived;
    u32 speed;
    u64 engineStart;
} transfer_receive_data;

static FS_Archive transfer_sd_archive() {
    static FS_Archive archive = 0;
    if(archive == 0) {
        FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    }

    return archive;
}

static bool transfer_sd_exists(const char* path) {
    bool exists = false;

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath != NULL) {
        Handle file = 0;
        if(R_SUCCEEDED(FSUSER_OpenFile(&file, transfer_sd_archive(), *fsPath, FS_OPEN_READ, 0))) {
            FSFILE_Close(file);
            exists = true;
        }

        fs_free_path_utf8(fsPath);
    }

    return exists;
}

// The name a sender gave, as a safe file name: no folders, no characters the
// SD card refuses.
static void transfer_clean_name(char* out, size_t size, const char* name) {
    const char* base = name;
    for(const char* c = name; *c != '\0'; c++) {
        if(*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }

    size_t len = 0;
    for(const char* c = base; *c != '\0' && len < size - 1; c++) {
        out[len++] = strchr(":*?\"<>|", *c) != NULL || (u8) *c < 0x20 ? '_' : *c;
    }

    out[len] = '\0';

    if(len == 0 || strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        string_copy(out, "received", size);
    }
}

// <dir><name>, or "name (2).ext" when that exists. `dir` ends with a slash.
static void transfer_unique_path(char* out, size_t size, const char* dir, const char* name) {
    snprintf(out, size, "%s%s", dir, name);

    const char* dot = strrchr(name, '.');
    size_t stem = dot != NULL && dot != name ? (size_t) (dot - name) : strlen(name);
    for(int i = 2; i < 100 && transfer_sd_exists(out); i++) {
        snprintf(out, size, "%s%.*s (%d)%s", dir, (int) stem, name, i, name + stem);
    }
}

// Writes down how far the payload is whole, for a later try: the place, which
// file it is (the check of the hello) and its size.
static void transfer_receive_save_position(transfer_receive_data* data) {
    FS_Path* fsPath = fs_make_path_utf8(data->position);
    if(fsPath != NULL) {
        Handle file = 0;
        if(R_SUCCEEDED(FSUSER_OpenFile(&file, transfer_sd_archive(), *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
            u8 bytes[20];
            transfer_put64(bytes, data->prefix);
            bytes[8] = (u8) (data->check >> 24);
            bytes[9] = (u8) (data->check >> 16);
            bytes[10] = (u8) (data->check >> 8);
            bytes[11] = (u8) data->check;
            transfer_put64(bytes + 12, data->size);

            u32 written = 0;
            FSFILE_Write(file, &written, 0, bytes, sizeof(bytes), FS_WRITE_FLUSH);
            FSFILE_Close(file);
        }

        fs_free_path_utf8(fsPath);
    }

    data->positionSaved = data->prefix;
}

static void transfer_receive_delete(const char* path) {
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath != NULL) {
        FSUSER_DeleteFile(transfer_sd_archive(), *fsPath);

        fs_free_path_utf8(fsPath);
    }
}

// A chunk is written. Chunks are taken in order but finish in any order: `prefix`
// is how far everything is there with no gap. Under the file lock.
static void transfer_receive_track(transfer_receive_data* data, u64 offset, u32 len) {
    if(offset + len <= data->prefix) {
        return;   // Before the place the transfer went on from (a header sent again).
    }

    if(offset == data->prefix) {
        data->prefix += len;

        bool joined = true;
        while(joined) {
            joined = false;
            for(u32 i = 0; i < data->pendingCount; i++) {
                if(data->pending[i].offset == data->prefix) {
                    data->prefix += data->pending[i].len;
                    data->pending[i] = data->pending[--data->pendingCount];
                    joined = true;
                    break;
                }
            }
        }

        if(data->prefix >= data->positionSaved + TRANSFER_POSITION_EVERY) {
            transfer_receive_save_position(data);
        }
    } else if(data->pendingCount < sizeof(data->pending) / sizeof(*data->pending)) {
        data->pending[data->pendingCount].offset = offset;
        data->pending[data->pendingCount].len = len;
        data->pendingCount++;
    }
}

// The bytes at `offset` of the payload, from any connection.
static Result transfer_receive_write(void* user, u64 offset, const void* buffer, u32 size) {
    transfer_receive_data* data = (transfer_receive_data*) user;

    if(selftest_fault("recv.write")) {
        return R_APP_IO_ERROR;
    }

    Result res = 0;
    if(data->sink != NULL) {
        res = action_export_cia_sink_write_at(data->sink, offset, buffer, size);
    } else {
        LightLock_Lock(&data->fileLock);

        u32 written = 0;
        res = FSFILE_Write(data->file_, &written, offset, buffer, size, 0);
        if(R_SUCCEEDED(res) && written != size) {
            res = R_APP_IO_ERROR;
        }

        LightLock_Unlock(&data->fileLock);
    }

    if(R_SUCCEEDED(res)) {
        LightLock_Lock(&data->fileLock);
        transfer_receive_track(data, offset, size);
        LightLock_Unlock(&data->fileLock);
    }

    return res;
}

static void transfer_receive_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    transfer_receive_data* receive = (transfer_receive_data*) data;

    char size[32];
    transfer_size_text(size, sizeof(size), receive->size);

    char line[FILE_NAME_MAX + 48];
    snprintf(line, sizeof(line), "%s  (%s)", receive->file, size);
    transfer_draw_card(x1, y1, x2, y2, ICON_DOWNLOAD, receive->sender, line);
}

static void transfer_receive_reset(transfer_receive_data* data) {
    if(data->client.uds != NULL) {
        data->ignoreUntil = osGetTime() + 4000;
    }

    transfer_link_close(&data->client);
    data->state = RECEIVE_IDLE;
    data->got = 0;
}

// Answers the sender: 1 to go ahead from byte `offset` (and the transfer's
// number), 0 to refuse.
static void transfer_receive_answer(transfer_receive_data* data, u8 status, u64 offset, u32 id) {
    u8 answer[TRANSFER_ANSWER_SIZE];
    answer[0] = status;
    transfer_put64(answer + 1, offset);
    answer[9] = (u8) (id >> 24);
    answer[10] = (u8) (id >> 16);
    answer[11] = (u8) (id >> 8);
    answer[12] = (u8) id;

    transfer_link_io(&data->client, true, answer, sizeof(answer), 0, NULL);
}

// Renames the finished .part to its final name (not over a file already there).
static Result transfer_receive_finish_file(transfer_receive_data* data, const char* name) {
    transfer_unique_path(data->dst, sizeof(data->dst), data->dir, name);

    Result res = 0;

    FS_Path* from = fs_make_path_utf8(data->part);
    FS_Path* to = fs_make_path_utf8(data->dst);
    if(from != NULL && to != NULL) {
        res = FSUSER_RenameFile(transfer_sd_archive(), *from, transfer_sd_archive(), *to);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    if(from != NULL) {
        fs_free_path_utf8(from);
    }

    if(to != NULL) {
        fs_free_path_utf8(to);
    }

    return res;
}

// How far an earlier try got with this file: its .part is there and its .pos
// says where it is whole, for this same file (the check and size of the hello).
// 0 when it is another file or there is nothing worth keeping.
static u64 transfer_receive_resume_point(transfer_receive_data* data) {
    u64 start = 0;

    FS_Path* partPath = fs_make_path_utf8(data->part);
    FS_Path* posPath = fs_make_path_utf8(data->position);
    if(partPath != NULL && posPath != NULL) {
        Handle pos = 0;
        Handle part = 0;
        if(R_SUCCEEDED(FSUSER_OpenFile(&pos, transfer_sd_archive(), *posPath, FS_OPEN_READ, 0))) {
            u8 bytes[20];
            u32 read = 0;
            u64 position = 0;
            u32 check = 0;
            u64 size = 0;
            if(R_SUCCEEDED(FSFILE_Read(pos, &read, 0, bytes, sizeof(bytes))) && read == sizeof(bytes)) {
                position = transfer_get64(bytes);
                check = ((u32) bytes[8] << 24) | ((u32) bytes[9] << 16) | ((u32) bytes[10] << 8) | bytes[11];
                size = transfer_get64(bytes + 12);
            }

            FSFILE_Close(pos);

            if(position > TRANSFER_RESUME_MARGIN && position <= data->size && size == data->size && check == data->check
               && R_SUCCEEDED(FSUSER_OpenFile(&part, transfer_sd_archive(), *partPath, FS_OPEN_READ, 0))) {
                u64 have = 0;

                // A file is as long as it will be; a CIA is longer than its package.
                if(R_SUCCEEDED(FSFILE_GetSize(part, &have)) && have > 0 && (data->kind == TRANSFER_KIND_TITLE || have == data->size)) {
                    start = position - TRANSFER_RESUME_MARGIN;
                    if(data->kind == TRANSFER_KIND_TITLE && start < data->head) {
                        start = 0;
                    }
                }

                FSFILE_Close(part);
            }
        }
    }

    if(partPath != NULL) {
        fs_free_path_utf8(partPath);
    }

    if(posPath != NULL) {
        fs_free_path_utf8(posPath);
    }

    return start;
}

static void transfer_receive_install(const char* path, const char* name, u64 size) {
    static linked_list noItems;
    static bool noItemsReady = false;
    if(!noItemsReady) {
        linked_list_init(&noItems);
        noItemsReady = true;
    }

    file_info info;
    memset(&info, 0, sizeof(info));
    info.archive = transfer_sd_archive();
    string_copy(info.name, name, sizeof(info.name));
    string_copy(info.path, path, sizeof(info.path));
    info.size = size;

    list_item item;
    memset(&item, 0, sizeof(item));
    string_copy(item.name, name, sizeof(item.name));
    item.data = &info;

    action_install_cia_delete_now(&noItems, &item);
}

// Takes everything the transfer holds apart. `succeeded`: the file is whole;
// else what is there is kept, with its place written down, for a retry.
static Result transfer_receive_close_transfer(transfer_receive_data* data, bool succeeded) {
    Result res = 0;

    transfer_receiver_free(data->engine);
    data->engine = NULL;

    if(data->finalizer != NULL) {
        threadJoin(data->finalizer, U64_MAX);
        threadFree(data->finalizer);
        data->finalizer = NULL;
    }

    if(!succeeded && data->prefix > 0) {
        transfer_receive_save_position(data);
    }

    if(data->sink != NULL) {
        res = action_export_cia_sink_close(data->sink, succeeded, true);
        data->sink = NULL;
    } else if(data->file_ != 0) {
        res = FSFILE_Close(data->file_);
        data->file_ = 0;
    }

    return res;
}

static void transfer_receive_finalize_thread(void* arg) {
    transfer_receive_data* data = (transfer_receive_data*) arg;

    data->finalResult = action_export_cia_sink_finish(data->sink);
    data->finalDone = true;
}

static void transfer_receive_progress_update(ui_view* view, void* data, float* progress, char* text) {
    transfer_receive_data* receive = (transfer_receive_data*) data;

    Result res = 0;
    bool done = false;

    if(!receive->finalizing) {
        transfer_receiver_accept(receive->engine);

        done = transfer_receiver_progress(receive->engine, &receive->received, &res);

        if(!done && (hidKeysDown() & KEY_B)) {
            transfer_receiver_cancel(receive->engine);
        }

        if(done && R_SUCCEEDED(res)) {
            // All here. A title package is checked and its TMD written first.
            receive->finalizing = true;
            if(receive->sink != NULL) {
                receive->finalizer = threadCreate(transfer_receive_finalize_thread, receive, 0x8000, 0x28, -2, false);
                if(receive->finalizer == NULL) {
                    receive->finalResult = R_APP_THREAD_CREATE_FAILED;
                    receive->finalDone = true;
                }
            } else {
                receive->finalResult = 0;
                receive->finalDone = true;
            }

            done = false;
        }
    }

    if(receive->finalizing) {
        if(!receive->finalDone) {
            u64 hashed = 0;
            u64 total = 0;
            if(receive->sink != NULL) {
                action_export_cia_sink_progress(receive->sink, &hashed, &total);
            }

            *progress = total != 0 ? (float) ((double) hashed / (double) total) : 1;
            snprintf(text, PROGRESS_TEXT_MAX, "%s", _("Checking the contents..."));
            return;
        }

        res = receive->finalResult;
        done = true;
    }

    if(!done) {
        u64 now = osGetTime();
        if(now - receive->speedTime >= 1000) {
            receive->speed = (u32) ((receive->received - receive->speedReceived) * 1000 / (now - receive->speedTime));
            receive->speedReceived = receive->received;
            receive->speedTime = now;
        }

        // As on the sender: where the file is, without the header sent again.
        u64 again = receive->kind == TRANSFER_KIND_TITLE && receive->start > 0 ? receive->head : 0;
        u64 at = receive->start + (receive->received > again ? receive->received - again : 0);
        if(at > receive->size) {
            at = receive->size;
        }

        u64 total = receive->size;
        u32 eta = receive->speed > 0 ? (u32) ((total - at) / receive->speed) : 0;

        *progress = total != 0 ? (float) ((double) at / (double) total) : 0;
        snprintf(text, PROGRESS_TEXT_MAX, _("From %s\n%.2f %s / %.2f %s\n%.2f %s/s, ETA %s"), receive->sender,
                 ui_get_display_size(at), ui_get_display_size_units(at),
                 ui_get_display_size(total), ui_get_display_size_units(total),
                 ui_get_display_size(receive->speed), ui_get_display_size_units(receive->speed),
                 ui_get_display_eta(eta));
        return;
    }

    // Over: whole, failed or stopped.
    bool ok = R_SUCCEEDED(res);
    transfer_log("receive %s link=%s received=%llu secs=%.1f result=%08lX\n", receive->file, receive->client.uds != NULL ? "uds" : "ip",
                 receive->received, (osGetTime() - receive->engineStart) / 1000.0, res);

    ui_pop();
    info_destroy(view);

    Result closeRes = transfer_receive_close_transfer(receive, ok);
    if(ok && R_FAILED(closeRes)) {
        ok = false;
        res = closeRes;
    }

    char name[FILE_NAME_MAX];
    transfer_clean_name(name, sizeof(name), receive->file);

    if(ok) {
        Result renameRes = transfer_receive_finish_file(receive, name);
        if(R_FAILED(renameRes)) {
            ok = false;
            res = renameRes;
        } else {
            transfer_receive_delete(receive->position);
        }
    }

    // The sender is waiting to hear that it is whole: 1, 0 when it failed, 2 when the user
    // here cancelled (the sender then does not offer to try again). A last wait so the
    // answer is not cut short by closing at once.
    u8 ack = ok ? 1 : res == R_APP_CANCELLED ? 2 : 0;
    transfer_link_io(&receive->client, true, &ack, 1, 0, NULL);
    transfer_link_wait_closed(&receive->client, 1500);

    transfer_receive_reset(receive);

    // A cancelled or failed transfer keeps its choices for a retry.
    if(ok || res == R_APP_CANCELLED) {
        receive->haveLast = false;
    }

    if(ok) {
        if(receive->isSave) {
            action_save_package_received(receive->dst, receive->install && receive->canImport);
        } else if(receive->install) {
            transfer_receive_install(receive->dst, receive->file, receive->size);
        } else {
            snprintf(receive->message, sizeof(receive->message), _("Saved in %s"), receive->dst);
            prompt_display_notify("Success", receive->message, COLOR_TEXT, NULL, NULL, NULL);
        }
    } else if(res != R_APP_CANCELLED) {
        error_display_res(NULL, NULL, res, "Failed to receive the file.");
    }
}

static void transfer_receive_start(transfer_receive_data* data) {
    char name[FILE_NAME_MAX];
    transfer_clean_name(name, sizeof(name), data->file);

    FS_Archive sd = transfer_sd_archive();
    Result res = 0;
    if(strcmp(data->dir, TRANSFER_RECEIVED_DIR) == 0) {
        res = fs_ensure_dir(sd, APP_DATA_DIR);
        if(R_SUCCEEDED(res)) {
            res = fs_ensure_dir(sd, TRANSFER_RECEIVED_DIR);
        }
    }

    u32 id = (u32) osGetTime() ^ (u32) svcGetSystemTick();

    // A path cut short could make <name>.part.pos the .part itself, or the .part
    // a file already there.
    if(R_SUCCEEDED(res)
       && ((size_t) snprintf(data->part, sizeof(data->part), "%s%s.part", data->dir, name) >= sizeof(data->part)
           || (size_t) snprintf(data->position, sizeof(data->position), "%s.pos", data->part) >= sizeof(data->position))) {
        transfer_log("receive: the path for %s in %s is too long\n", name, data->dir);
        res = R_APP_BAD_DATA;
    }

    if(R_SUCCEEDED(res)) {
        data->haveLast = true;
        string_copy(data->lastSender, data->sender, sizeof(data->lastSender));
        string_copy(data->lastFile, data->file, sizeof(data->lastFile));
        data->lastSize = data->size;
        data->lastKind = data->kind;
        data->lastCheck = data->check;
        data->lastInstall = data->install;
        string_copy(data->lastDir, data->dir, sizeof(data->lastDir));

        data->start = transfer_receive_resume_point(data);
        transfer_log("receive: %s -> %s, %s, from byte %llu\n", data->file, data->dir, data->install ? "to install" : "to save", data->start);

        LightLock_Init(&data->fileLock);
        data->prefix = data->start;
        data->pendingCount = 0;
        data->positionSaved = data->start;
        data->finalDone = false;
        data->finalizing = false;
        data->finalizer = NULL;
        data->received = 0;
        data->speed = 0;
        data->speedReceived = 0;
        data->speedTime = osGetTime();
        data->engineStart = data->speedTime;

        if(data->start == 0) {
            // Not a retry of an earlier try (or one that can not be): its place is no use.
            transfer_receive_delete(data->position);
        }

        if(data->kind == TRANSFER_KIND_TITLE) {
            res = action_export_cia_sink_open(data->part, data->size, data->start > 0, &data->sink);
        } else {
            FS_Path* fsPath = fs_make_path_utf8(data->part);
            if(fsPath == NULL) {
                res = R_APP_OUT_OF_MEMORY;
            } else if(R_SUCCEEDED(res = FSUSER_OpenFile(&data->file_, sd, *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
                // The whole size at once: a full card fails now, not at the end, and
                // the chunks, which arrive in any order, have somewhere to go. A
                // resumed file keeps what it has.
                u64 have = 0;
                if(R_FAILED(res = FSFILE_GetSize(data->file_, &have)) || (have != data->size && R_FAILED(res = FSFILE_SetSize(data->file_, data->size)))) {
                    FSFILE_Close(data->file_);
                    data->file_ = 0;
                }
            }

            if(fsPath != NULL) {
                fs_free_path_utf8(fsPath);
            }
        }

        if(R_SUCCEEDED(res)) {
            // The sender starts as soon as it hears this.
            transfer_receive_answer(data, 1, data->start, id);

            if(R_SUCCEEDED(res = transfer_receiver_start(&data->engine, &data->client, data->tcp, id, data->start, data->size, data->kind == TRANSFER_KIND_TITLE ? data->head : 0, transfer_receive_write, data))) {
                info_display("Receiving", "\uE001 cancel", true, data, transfer_receive_progress_update, transfer_receive_draw_top);
                data->state = RECEIVE_RECEIVING;
                return;
            }

            transfer_receive_close_transfer(data, false);
        }
    }

    transfer_receive_answer(data, 0, 0, 0);
    transfer_receive_reset(data);

    error_display_res(NULL, NULL, res, "Failed to receive the file.");
}

// ---- choosing where to save ----

#define TRANSFER_FOLDERS_MAX 512

typedef struct {
    transfer_receive_data* receive;
    char path[FILE_PATH_MAX];   // Ends with a slash.
    bool reload;
} transfer_folder_data;

static void transfer_receive_ask_action(transfer_receive_data* receive);

static void transfer_folder_clear(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static int transfer_folder_compare(const void* a, const void* b) {
    return strcasecmp((const char*) a, (const char*) b);
}

// The first row saves here; the others are the folders inside.
static void transfer_folder_fill(transfer_folder_data* data, linked_list* items) {
    transfer_folder_clear(items);

    list_item* here = (list_item*) calloc(1, sizeof(list_item));
    if(here != NULL) {
        string_copy(here->name, "Save here", sizeof(here->name));
        here->color = COLOR_TEXT;
        linked_list_add(items, here);
    }

    char* names = (char*) calloc(TRANSFER_FOLDERS_MAX, FILE_NAME_MAX);
    if(names == NULL) {
        return;
    }

    u32 count = 0;

    FS_Path* fsPath = fs_make_path_utf8(data->path);
    if(fsPath != NULL) {
        Handle dir = 0;
        if(R_SUCCEEDED(FSUSER_OpenDirectory(&dir, transfer_sd_archive(), *fsPath))) {
            FS_DirectoryEntry entry;
            u32 read = 0;
            while(count < TRANSFER_FOLDERS_MAX && R_SUCCEEDED(FSDIR_Read(dir, &read, 1, &entry)) && read == 1) {
                if(entry.attributes & FS_ATTRIBUTE_DIRECTORY) {
                    ssize_t len = utf16_to_utf8((uint8_t*) names + (size_t) count * FILE_NAME_MAX, entry.name, FILE_NAME_MAX - 1);
                    if(len > 0) {
                        names[(size_t) count * FILE_NAME_MAX + len] = '\0';
                        count++;
                    }
                }
            }

            FSDIR_Close(dir);
        }

        fs_free_path_utf8(fsPath);
    }

    qsort(names, count, FILE_NAME_MAX, transfer_folder_compare);

    for(u32 i = 0; i < count; i++) {
        list_item* item = (list_item*) calloc(1, sizeof(list_item));
        if(item != NULL) {
            string_copy(item->name, names + (size_t) i * FILE_NAME_MAX, sizeof(item->name));
            item->color = COLOR_TEXT;
            item->data = item;   // Marks a folder.
            linked_list_add(items, item);
        }
    }

    free(names);
}

static void transfer_folder_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    widget_row(x, y, width, height, item->data == NULL ? ICON_CHECK : ICON_FOLDER, item->data == NULL ? _("Save here") : item->name, COLOR_TEXT, NULL, selected);
}

static void transfer_folder_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    transfer_folder_data* folder = (transfer_folder_data*) data;

    transfer_draw_card(x1, y1, x2, y2, ICON_FOLDER, _("Choose where to save"), folder->path);
}

static void transfer_folder_exit(ui_view* view, transfer_folder_data* data, linked_list* items) {
    ui_pop();

    transfer_folder_clear(items);
    list_destroy(view);
}

static void transfer_folder_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    transfer_folder_data* folder = (transfer_folder_data*) data;

    if(hidKeysDown() & KEY_B) {
        size_t len = strlen(folder->path);
        if(len <= 1) {
            transfer_receive_data* receive = folder->receive;

            transfer_folder_exit(view, folder, items);
            free(folder);

            transfer_receive_ask_action(receive);
            return;
        }

        // Up one level: drop the last name and its slash.
        folder->path[len - 1] = '\0';
        char* slash = strrchr(folder->path, '/');
        slash[1] = '\0';
        folder->reload = true;
    } else if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        if(selected->data == NULL) {
            transfer_receive_data* receive = folder->receive;
            string_copy(receive->dir, folder->path, sizeof(receive->dir));

            transfer_folder_exit(view, folder, items);
            free(folder);

            transfer_receive_start(receive);
            return;
        }

        size_t len = strlen(folder->path);
        snprintf(folder->path + len, sizeof(folder->path) - len, "%s/", selected->name);
        folder->reload = true;
    }

    if(folder->reload) {
        folder->reload = false;
        transfer_folder_fill(folder, items);
    }

    view->name = folder->path;
}

static void transfer_receive_choose_folder(transfer_receive_data* receive) {
    transfer_folder_data* folder = (transfer_folder_data*) calloc(1, sizeof(transfer_folder_data));
    if(folder == NULL) {
        error_display(NULL, NULL, "Failed to allocate transfer data.");

        transfer_receive_answer(receive, 0, 0, 0);
        transfer_receive_reset(receive);
        return;
    }

    folder->receive = receive;
    folder->reload = true;

    // Where the last file went, or the default folder (made if it is missing).
    if(receive->dir[0] == '\0') {
        string_copy(receive->dir, TRANSFER_RECEIVED_DIR, sizeof(receive->dir));
        fs_ensure_dir(transfer_sd_archive(), APP_DATA_DIR);
        fs_ensure_dir(transfer_sd_archive(), TRANSFER_RECEIVED_DIR);
    }

    string_copy(folder->path, receive->dir, sizeof(folder->path));

    ui_view* view = list_display(folder->path, "\uE000 open   \uE001 up", folder, transfer_folder_update, transfer_folder_draw_top);
    if(view == NULL) {
        free(folder);

        transfer_receive_answer(receive, 0, 0, 0);
        transfer_receive_reset(receive);
        return;
    }

    list_style style = {
            .rowHeight = 30,
            .drawRow = transfer_folder_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
}

static void transfer_receive_action_onresponse(ui_view* view, void* data, u32 response) {
    transfer_receive_data* receive = (transfer_receive_data*) data;

    // Without Install the choices are Save and Reject.
    if(!receive->isCia && !(receive->isSave && receive->canImport)) {
        response++;
    }

    if(response == 2) {
        transfer_receive_answer(receive, 0, 0, 0);
        transfer_receive_reset(receive);
        return;
    }

    receive->install = response == 0;
    if(receive->isSave) {
        // Whatever the choice, it arrives in the default folder and is unpacked into a folder of saves.
        string_copy(receive->dir, TRANSFER_RECEIVED_DIR, sizeof(receive->dir));
        transfer_receive_start(receive);
    } else if(receive->install) {
        // A CIA that installs right away goes to the default folder, and is deleted after.
        string_copy(receive->dir, TRANSFER_RECEIVED_DIR, sizeof(receive->dir));
        transfer_receive_start(receive);
    } else {
        transfer_receive_choose_folder(receive);
    }
}

static void transfer_receive_ask_action(transfer_receive_data* receive) {
    static const char* ciaOptions[3] = {"Install now", "Save", "Reject"};
    static u32 ciaButtons[3] = {KEY_A, KEY_X, KEY_B};
    static const char* fileOptions[2] = {"Save", "Reject"};
    static u32 fileButtons[2] = {KEY_A, KEY_B};

    char size[32];
    transfer_size_text(size, sizeof(size), receive->size);
    snprintf(receive->message, sizeof(receive->message), _("%s (%s)\nWhat do you want to do with it?"), receive->file, size);

    if(receive->isSave) {
        // Importing needs the title (and for extra data, its archive) here: otherwise it is only kept.
        static const char* saveOptions[3] = {"Import now", "Save in a folder", "Reject"};
        static const char* keepOptions[2] = {"Save in a folder", "Reject"};
        u64 id = 0;
        save_kind kind = SAVE_KIND_SAVE;
        char what[LIST_ITEM_NAME_MAX];
        action_save_package_name(receive->file, &id, &kind, what, sizeof(what));
        snprintf(receive->message, sizeof(receive->message), receive->canImport ? _("%s of %s (%s)\nWhat do you want to do with it?") : _("%s of %s (%s)\nThis title or its data is not here: it can only be kept.\nWhat do you want to do with it?"),
                 kind == SAVE_KIND_EXT ? _("Extra data") : kind == SAVE_KIND_BOSS ? _("SpotPass data") : _("Save data"), what, size);
        if(receive->canImport) {
            prompt_display_multi_choice("Incoming file", receive->message, COLOR_TEXT, saveOptions, ciaButtons, 3, receive, transfer_receive_draw_top, transfer_receive_action_onresponse);
        } else {
            prompt_display_multi_choice("Incoming file", receive->message, COLOR_TEXT, keepOptions, fileButtons, 2, receive, transfer_receive_draw_top, transfer_receive_action_onresponse);
        }
    } else if(receive->isCia) {
        prompt_display_multi_choice("Incoming file", receive->message, COLOR_TEXT, ciaOptions, ciaButtons, 3, receive, transfer_receive_draw_top, transfer_receive_action_onresponse);
    } else {
        prompt_display_multi_choice("Incoming file", receive->message, COLOR_TEXT, fileOptions, fileButtons, 2, receive, transfer_receive_draw_top, transfer_receive_action_onresponse);
    }
}

static void transfer_receive_allow_onresponse(ui_view* view, void* data, u32 response) {
    transfer_receive_data* receive = (transfer_receive_data*) data;

    if(response != PROMPT_YES) {
        transfer_receive_answer(receive, 0, 0, 0);
        transfer_receive_reset(receive);
        return;
    }

    transfer_receive_ask_action(receive);
}

static void transfer_receive_hello_done(transfer_receive_data* data) {
    transfer_hello* hello = &data->hello;

    if(memcmp(hello->magic, TRANSFER_MAGIC, sizeof(hello->magic)) != 0 || ntohl(hello->version) != TRANSFER_PROTOCOL) {
        transfer_receive_reset(data);
        return;
    }

    hello->sender[TRANSFER_NAME_MAX - 1] = '\0';
    hello->file[FILE_NAME_MAX - 1] = '\0';

    string_copy(data->sender, hello->sender, sizeof(data->sender));
    string_copy(data->file, hello->file, sizeof(data->file));
    data->size = ((u64) ntohl(hello->sizeHigh) << 32) | ntohl(hello->sizeLow);
    data->check = ntohl(hello->check);
    data->kind = ntohl(hello->kind);
    data->head = ntohl(hello->head);

    const char* ext = strrchr(data->file, '.');
    data->isCia = data->kind == TRANSFER_KIND_TITLE || (ext != NULL && strcasecmp(ext, ".cia") == 0);

    u64 saveId = 0;
    save_kind saveKind = SAVE_KIND_SAVE;
    char saveName[LIST_ITEM_NAME_MAX];
    data->isSave = data->kind == TRANSFER_KIND_FILE && action_save_package_name(data->file, &saveId, &saveKind, saveName, sizeof(saveName));
    data->canImport = data->isSave && action_save_package_can_import(saveId, saveKind);

    if(data->size == 0) {
        transfer_receive_reset(data);
        return;
    }

    // Tests: no questions.
    if(udslink_auto()) {
        data->install = false;
        string_copy(data->dir, TRANSFER_RECEIVED_DIR, sizeof(data->dir));
        data->state = RECEIVE_ASKING;
        transfer_receive_start(data);
        return;
    }

    // The same file again after a failure: as it was chosen then.
    if(data->haveLast && data->lastKind == data->kind && data->lastSize == data->size && data->lastCheck == data->check
       && strcmp(data->lastSender, data->sender) == 0 && strcmp(data->lastFile, data->file) == 0) {
        data->install = data->lastInstall;
        string_copy(data->dir, data->lastDir, sizeof(data->dir));
        data->state = RECEIVE_ASKING;
        transfer_receive_start(data);
        return;
    }

    transfer_log("receive: %s wants to send %s (%llu bytes, %s)\n", data->sender, data->file, data->size, data->kind == TRANSFER_KIND_TITLE ? "title" : "file");

    char size[32];
    transfer_size_text(size, sizeof(size), data->size);
    snprintf(data->message, sizeof(data->message), _("%s wants to send you\n%s (%s).\nAllow it?"), data->sender, data->file, size);

    data->state = RECEIVE_ASKING;
    prompt_display_yes_no("Receive", data->message, COLOR_TEXT, data, transfer_receive_draw_top, transfer_receive_allow_onresponse);
}

static void transfer_receive_answer_probes(transfer_receive_data* data) {
    for(int i = 0; i < 8; i++) {
        u8 packet[sizeof(TRANSFER_PROBE)];
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);

        ssize_t n = recvfrom(data->udp, packet, sizeof(packet), 0, (struct sockaddr*) &from, &fromLen);
        if(n < 0) {
            break;
        }

        if(n == (ssize_t) sizeof(TRANSFER_PROBE) && memcmp(packet, TRANSFER_PROBE, sizeof(TRANSFER_PROBE)) == 0) {
            u8 answer[sizeof(TRANSFER_ANSWER) + TRANSFER_NAME_MAX];
            memcpy(answer, TRANSFER_ANSWER, sizeof(TRANSFER_ANSWER));
            memset(answer + sizeof(TRANSFER_ANSWER), 0, TRANSFER_NAME_MAX);
            memcpy(answer + sizeof(TRANSFER_ANSWER), data->localName, strnlen(data->localName, TRANSFER_NAME_MAX));

            sendto(data->udp, answer, sizeof(answer), 0, (struct sockaddr*) &from, fromLen);
        }
    }
}

static void transfer_receive_free(transfer_receive_data* data) {
    transfer_link_close(&data->client);
    transfer_close(&data->tcp);
    transfer_close(&data->udp);

    if(data->uds) {
        udslink_leave();
    }

    free(data);
}

static bool transfer_receive_start_wifi(transfer_receive_data* data);
static bool transfer_receive_start_nearby(transfer_receive_data* data);
static void transfer_receive_set_button(ui_view* view, transfer_receive_data* data);

static void transfer_receive_update(ui_view* view, void* data, float* progress, char* text) {
    transfer_receive_data* receive = (transfer_receive_data*) data;

    if(receive->state == RECEIVE_IDLE || receive->state == RECEIVE_HELLO) {
        if(hidKeysDown() & KEY_B) {
            ui_pop();
            info_destroy(view);

            transfer_receive_free(receive);
            return;
        }
    }

    if(receive->pendingMode != TRANSFER_MODE_NONE) {
        // The screen says "Switching...", and a couple of frames later the change is made.
        snprintf(text, PROGRESS_TEXT_MAX, "%s", receive->pendingMode == TRANSFER_MODE_NEARBY ? _("Switching to nearby mode...") : _("Switching to the Wi-Fi..."));

        if(++receive->pendingFrames >= 2) {
            u32 mode = receive->pendingMode;
            receive->pendingMode = TRANSFER_MODE_NONE;

            if(mode == TRANSFER_MODE_WIFI) {
                udslink_host_stop();
                udslink_leave();
                receive->uds = false;
                receive->waitingWifi = true;
                receive->modeSince = osGetTime();
            } else {
                transfer_close(&receive->tcp);
                transfer_close(&receive->udp);

                if(transfer_receive_start_nearby(receive)) {
                    receive->modeSince = osGetTime();
                } else {
                    // Back to what it was (or to waiting for the Wi-Fi, if that had not started).
                    if(udslink_wifi_connected()) {
                        transfer_receive_start_wifi(receive);
                    } else {
                        receive->waitingWifi = true;
                    }
                }
            }

            transfer_receive_set_button(view, receive);
        }

        return;
    }

    if(receive->waitingWifi) {
        // Back from the local wireless: the sockets can be made once the Wi-Fi is up.
        if(udslink_wifi_connected()) {
            receive->waitingWifi = false;
            if(transfer_receive_start_wifi(receive)) {
                receive->modeSince = osGetTime();
            }

            transfer_receive_set_button(view, receive);
        } else {
            snprintf(text, PROGRESS_TEXT_MAX, "%s", _("Waiting for the Wi-Fi..."));
        }

        return;
    }

    if(!receive->uds) {
        transfer_receive_answer_probes(receive);
    }

    if(receive->state == RECEIVE_IDLE) {
        if(receive->uds) {
            u64 now = osGetTime();
            if(now >= receive->ignoreUntil && now - receive->lastAccept >= 200) {
                receive->lastAccept = now;

                udslink* link = udslink_host_accept();
                if(link != NULL) {
                    receive->client.fd = -1;
                    receive->client.uds = link;
                    receive->state = RECEIVE_HELLO;
                    receive->got = 0;
                    receive->helloStart = osGetTime();
                }
            }
        } else {
            int sock = accept(receive->tcp, NULL, NULL);
            if(sock >= 0) {
                receive->client.fd = sock;
                receive->client.uds = NULL;
                transfer_set_nonblocking(sock);
                transfer_set_buffers(sock);

                receive->state = RECEIVE_HELLO;
                receive->got = 0;
                receive->helloStart = osGetTime();
            }
        }
    }

    if(receive->state == RECEIVE_HELLO) {
        int n = transfer_link_recv_some(&receive->client, (u8*) &receive->hello + receive->got, sizeof(receive->hello) - receive->got);
        if(n > 0) {
            receive->got += (u32) n;
        }

        if(n == 0 || n == -2 || osGetTime() - receive->helloStart > (receive->uds ? 3 * TRANSFER_HELLO_TIMEOUT : TRANSFER_HELLO_TIMEOUT)) {
            transfer_receive_reset(receive);
        } else if(receive->got == sizeof(receive->hello)) {
            transfer_receive_hello_done(receive);
        }
    }

    if(receive->uds) {
        snprintf(text, PROGRESS_TEXT_MAX, _("Waiting for a sender...\n%s\nNearby, no Wi-Fi needed"), receive->localName);
        return;
    }

    char ip[24];
    transfer_ip_text(ip, sizeof(ip));
    snprintf(text, PROGRESS_TEXT_MAX, _("Waiting for a sender...\n%s\nIP: %s"), receive->localName, ip);

    // Nobody yet: the other console may be without Wi-Fi, which this one can only
    // meet by making a network of its own.
    if(osGetTime() - receive->modeSince > TRANSFER_RECEIVE_HINT_AFTER) {
        size_t used = strlen(text);
        snprintf(text + used, PROGRESS_TEXT_MAX - used, "\n%s", _("Nobody yet? If the other 3DS has no Wi-Fi,\nuse the button."));
    }
}

static void transfer_receive_draw_waiting_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    transfer_receive_data* receive = (transfer_receive_data*) data;

    char ip[24];
    if(receive->uds) {
        snprintf(ip, sizeof(ip), "%s", _("Nearby"));
    } else {
        transfer_ip_text(ip, sizeof(ip));
    }

    transfer_draw_card(x1, y1, x2, y2, ICON_3DS, receive->localName, ip);
}

static void transfer_receive_switch(ui_view* view, void* data);
static void transfer_receive_set_button(ui_view* view, transfer_receive_data* data);

// Opens the sockets for receiving over the Wi-Fi. False (the reason shown) when it can not.
static bool transfer_receive_start_wifi(transfer_receive_data* data) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;

    data->udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    addr.sin_port = htons(TRANSFER_DISCOVERY_PORT);
    if(data->udp < 0 || bind(data->udp, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to bind UDP socket.");

        transfer_close(&data->tcp);
        transfer_close(&data->udp);
        return false;
    }

    transfer_set_nonblocking(data->udp);

    data->tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(data->tcp < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to open server socket.");

        transfer_close(&data->tcp);
        transfer_close(&data->udp);
        return false;
    }

    int reuse = 1;
    setsockopt(data->tcp, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    addr.sin_port = htons(TRANSFER_PORT);
    if(bind(data->tcp, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to bind TCP socket.");

        transfer_close(&data->tcp);
        transfer_close(&data->udp);
        return false;
    }

    if(listen(data->tcp, 8) < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to listen on server socket.");

        transfer_close(&data->tcp);
        transfer_close(&data->udp);
        return false;
    }

    transfer_set_nonblocking(data->tcp);

    return true;
}

// Makes the network of its own and waits for a sender on it.
static bool transfer_receive_start_nearby(transfer_receive_data* data) {
    if(!udslink_enter()) {
        error_display(NULL, NULL, "Failed to open socket.");
        return false;
    }

    Result res = udslink_host_start(data->localName);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to open server socket.");
        udslink_leave();
        return false;
    }

    data->uds = true;
    return true;
}

static void transfer_receive_set_button(ui_view* view, transfer_receive_data* data) {
    info_set_button(view, data->uds ? "Receive over Wi-Fi" : "Receive without Wi-Fi", transfer_receive_switch);
}

// The button: from the Wi-Fi to a network of its own, or back. Only while waiting.
static void transfer_receive_switch(ui_view* view, void* data) {
    transfer_receive_data* receive = (transfer_receive_data*) data;
    if(receive->state != RECEIVE_IDLE || receive->waitingWifi || receive->pendingMode != TRANSFER_MODE_NONE) {
        return;
    }

    if(receive->pendingMode != TRANSFER_MODE_NONE) {
        return;
    }

    // Only asked for: the change is made once the screen has said "Switching...".
    receive->pendingMode = receive->uds ? TRANSFER_MODE_WIFI : TRANSFER_MODE_NEARBY;
    receive->pendingFrames = 0;
    info_set_button(view, NULL, NULL);
}

void transfer_receive_open() {
    transfer_receive_data* data = (transfer_receive_data*) calloc(1, sizeof(transfer_receive_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate transfer data.");
        return;
    }

    data->udp = -1;
    data->tcp = -1;
    data->client.fd = -1;
    data->modeSince = osGetTime();

    transfer_local_name(data->localName, sizeof(data->localName));

    // On a Wi-Fi network the usual way; without one, a network of its own that the
    // sender will find. The button on the screen changes it either way.
    bool wifi = udslink_wifi_connected();
    if(wifi) {
        if(!transfer_receive_start_wifi(data)) {
            free(data);
            return;
        }
    } else {
        // The network of its own takes a while: once the screen is up, so it can say so.
        data->pendingMode = TRANSFER_MODE_NEARBY;
    }

    ui_view* view = info_display("Receive", "\uE001 return   \uE002 other way", false, data, transfer_receive_update, transfer_receive_draw_waiting_top);
    if(view == NULL) {
        transfer_receive_free(data);
        return;
    }

    if(wifi) {
        transfer_receive_set_button(view, data);
    }
}
