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
#include "transferfolder.h"
#include "foldermanifest.h"
#include "udslink.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "../core/xferturn.h"

#define TRANSFER_DISCOVERY_PORT 5001

#define TRANSFER_NAME_MAX 32
#define TRANSFER_RECEIVED_DIR APP_DATA_DIR "received/"
// What arrives (a file, a title package or a folder) is built here, as <name>.part with
// <name>.part.pos beside it, and moved to the chosen folder when it is whole: only NSA touches
// this one, so a transfer never leaves half a file among the user's.
#define TRANSFER_TMP_DIR APP_DATA_DIR "tmp/"

// A transfer cut off in the middle can go on from the last place the receiver
// wrote down; this much before it is sent again, in case it had not reached
// the card.
#define TRANSFER_RESUME_MARGIN (1024 * 1024)
// The receiver writes that place down after this many more bytes.
#define TRANSFER_POSITION_EVERY (4 * 1024 * 1024)
#define TRANSFER_CHECK_BYTES 4096
#define TRANSFER_PROTOCOL 9

// What the bytes after the hello are.
#define TRANSFER_KIND_FILE 0
#define TRANSFER_KIND_TITLE 1   // A title package (exportcia.c): the receiver builds a CIA.
#define TRANSFER_KIND_FOLDER 2  // A folder (transferfolder.c): its manifest, then each file; `head` is the manifest's size.
#define TRANSFER_KIND_BATCH 3   // Titles sent together (issue #53): their list (`head` bytes) follows; the receiver's
                                // user answers once for all, the answer's number is the batch's, and each title comes
                                // then as a transfer of its own (TRANSFER_KIND_TITLE) that quotes it.
#define TRANSFER_KIND_BATCH_END 4   // The sender has nothing more for the batch it quotes: no payload, no answer.
#define TRANSFER_KIND_CIA_ORDERED 5 // A batch's title as a CIA in order (exportcia.c), installed as it arrives: no resume.

// A batch's list: "NSAB", the count (4, big endian), then for each title its package's size (8), its title ID (8), its
// version (4), 4 bytes kept at zero and its CIA's name (FILE_NAME_MAX bytes, zero padded). The receiver's answer is
// followed by one byte per title: how it is to come (TRANSFER_WAY_*), chosen from its free space.
#define TRANSFER_BATCH_MAX 64
#define TRANSFER_BATCH_HEAD 8
#define TRANSFER_BATCH_ENTRY (24 + FILE_NAME_MAX)

#define TRANSFER_WAY_SKIP 0      // No room for it: not sent.
#define TRANSFER_WAY_PACKAGE 1   // A title package: its CIA built on the receiver, installed afterwards (twice its size).
#define TRANSFER_WAY_ORDERED 2   // A CIA in order, installed as it arrives (its size once; slower, and a cut starts over).

// Kept free on the receiver's SD card when a batch's ways are chosen.
#define TRANSFER_BATCH_MARGIN (16 * 1024 * 1024)

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

// What to do with files that already exist where the original path puts them.
#define TRANSFER_OVERWRITE_ASK 0
#define TRANSFER_OVERWRITE_ALL 1
#define TRANSFER_OVERWRITE_SKIP 2

static const char TRANSFER_MAGIC[4] = {'N', 'S', 'A', 'T'};
static const char TRANSFER_PROBE[4] = {'N', 'S', 'A', '?'};
static const char TRANSFER_ANSWER[4] = {'N', 'S', 'A', '!'};

// What a sender says first. Numbers are big endian; 836 bytes, no padding. For a folder, the
// manifest (`head` bytes) follows at once, before the receiver answers, so that it can say what
// would be overwritten when the user chooses the original path.
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
    char origin[FILE_PATH_MAX];   // Where it was on the sender's SD card ("" when it was not there): the file's path, or the folder's.
    u32 batch;                    // The batch this title belongs to (the receiver's number for it), 0 when none.
    u32 batchIndex;               // Its place in the batch's list.
} transfer_hello;

// ---- shared ----

u32 transfer_protocol() {
    return TRANSFER_PROTOCOL;
}

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

// How many bytes of `text` fit in `max` without cutting a character in two (UTF-8), for "%.*s".
static int transfer_utf8_cut(const char* text, size_t max) {
    size_t len = strlen(text);
    if(len <= max) {
        return (int) len;
    }

    while(max > 0 && ((u8) text[max] & 0xC0) == 0x80) {
        max--;
    }

    return (int) max;
}

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

// A line centered, or as much of it as fits when it is too wide.
static void transfer_draw_centered_fit(const char* text, float x1, float x2, float y, float scale, u32 color) {
    if(widget_text_width(text, scale) <= x2 - x1) {
        widget_text_centered(text, x1, x2, y, scale, color);
    } else {
        widget_text_fit(text, x1, y, scale, color, x2 - x1);
    }
}

// A card on the top screen: an icon, a line, a smaller one and, when given, a third (a path, say)
// that goes over as many lines as it needs.
static void transfer_draw_card(float x1, float y1, float x2, float y2, u32 icon, const char* line1, const char* line2, const char* line3) {
    const theme_current* t = theme();

    float w = x2 - x1;
    float h = y2 - y1;
    float cx = x1 + w / 2;
    float cy = y1 + h / 2;

    screen_draw_icon(icon, cx - 16, cy - 56, 32, t->accent);
    transfer_draw_centered_fit(line1, x1 + 10, x2 - 10, cy - 10, WIDGET_TITLE_SCALE, t->palette->text);
    if(line2 != NULL && line2[0] != '\0') {
        transfer_draw_centered_fit(line2, x1 + 10, x2 - 10, cy + 14, WIDGET_TEXT_SCALE, t->palette->textMuted);
    }

    if(line3 != NULL && line3[0] != '\0') {
        // The block is centered as a whole (the lines of a wrapped text are centered on its widest one).
        float blockW = 0;
        float blockH = 0;
        screen_get_string_size_wrap(&blockW, &blockH, line3, WIDGET_SMALL_SCALE, WIDGET_SMALL_SCALE, w - 20);
        screen_draw_text_wrap_centered(line3, cx - blockW / 2, cy + 36, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - 10);
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
    transfer_folder_send* folder;   // A folder to send, instead of a file.
    char origin[FILE_PATH_MAX];   // Where it is on the SD card ("" when it is not there).

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

    struct transfer_batch_send_s* batch;   // Titles sent together, instead of one thing (owned).
    ui_view* listView;
} transfer_send_data;

typedef enum {
    SEND_WAITING,
    SEND_SENDING,
    SEND_FINISHING,
    SEND_PAUSE,         // A batch between two tries: the next connection waits until `retryAt`.
    SEND_HASHING        // A batch's title to send in order: its contents are read once first (a thread).
} transfer_send_state;

// ---- a batch of titles (issue #53) ----
//
// The receiver is asked once, with the list (TRANSFER_KIND_BATCH); its answer gives the batch a number. Each title then
// goes as a transfer of its own that quotes it, which the receiver takes without asking. One that fails is tried again
// (from where it was) a few times and then given up; the batch goes on with the next. TRANSFER_KIND_BATCH_END closes it.

#define TRANSFER_BATCH_TRIES 3
#define TRANSFER_BATCH_PAUSE 3000
#define TRANSFER_BATCH_ERROR_MAX 96

typedef enum {
    BATCH_ITEM_WAITING,
    BATCH_ITEM_SENT,
    BATCH_ITEM_FAILED
} transfer_batch_item_state;

typedef struct {
    char name[FILE_NAME_MAX];   // The CIA's name (the title's, from export).
    u64 size;                   // As a package.
    transfer_batch_item_state state;
    char error[TRANSFER_BATCH_ERROR_MAX];
} transfer_batch_item;

typedef struct transfer_batch_send_s {
    title_info* titles;         // Owned (copies).
    transfer_batch_item* items;
    u32 count;
    u64 total;
    u8* list;                   // The batch's list, as it is sent.
    u32 listSize;

    bool asking;                // The question for all of them is out.
    u32 id;                     // The receiver's number for the batch.
    u32 current;                // The title being sent.
    u32 tries;                  // Of that one.
    u64 retryAt;
    bool stopped;               // Cancelled here or on the other console: what is left is not sent.
    u8 ways[TRANSFER_BATCH_MAX];   // How each one goes (TRANSFER_WAY_*), as the receiver chose.
} transfer_batch_send;

static void transfer_batch_free(transfer_batch_send* batch);

typedef struct {
    FS_Archive archive;
    char path[FILE_PATH_MAX];
    char name[FILE_NAME_MAX];
    u64 size;
    export_cia_data* stream;   // Owned by the list.
    transfer_folder_send* folder;   // Owned by the list.
    char origin[FILE_PATH_MAX];

    char peerName[TRANSFER_NAME_MAX + 1];
    struct in_addr addr;
    bool uds;
    udslink_peer udsPeer;
    transfer_link link;
    transfer_send_state state;

    u32 check;
    u32 head;           // Bytes of a title package that are its header (of a folder, its manifest).
    u8 answer[TRANSFER_ANSWER_SIZE];
    u32 answerGot;
    u64 start;          // The payload is sent from here on.

    transfer_sender* engine;
    u64 sent;
    u64 speedTime;
    u64 speedSent;
    u32 speed;
    u64 engineStart;
    u32 turn;           // The transfer turn (xferturn.h), from connecting (sending) or accepting (receiving) to the end.

    u32 kind;           // TRANSFER_KIND_*: what the hello says.
    transfer_batch_send* batch;   // Owned by the list (`list`), which the end of the batch closes.
    transfer_send_data* list;
    char error[TRANSFER_BATCH_ERROR_MAX];   // In a batch, why the connection failed (nothing is shown).

    // A batch's title sent as a CIA in order (TRANSFER_WAY_ORDERED): `stream` read once first on `hasher`.
    bool ordered;
    Thread hasher;
    volatile bool hashDone;
    volatile bool hashCancel;
    volatile u64 hashed;
    u64 hashTotal;
    Result hashResult;
} transfer_send_session;

// Fills the chunk from the file, or from the package: its header is in memory
// and each content is read from the system.
static Result transfer_send_read(void* user, const transfer_chunk* chunk, void* buffer) {
    transfer_send_session* session = (transfer_send_session*) user;

    if(selftest_fault("send.read")) {
        return R_APP_IO_ERROR;
    }

    if(session->folder != NULL) {
        return transfer_folder_send_read(session->folder, session->archive, chunk, buffer);
    }

    if(session->stream != NULL && session->ordered) {
        return action_export_cia_ordered_read(session->stream, chunk->segment, chunk->segmentOffset, buffer, chunk->len);
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

    char size[64];
    transfer_size_text(size, sizeof(size), session->size);
    if(session->batch != NULL && !session->batch->asking && session->batch->current < session->batch->count) {
        // Which of the batch's titles this is.
        size_t used = strlen(size);
        snprintf(size + used, sizeof(size) - used, "  (%lu/%lu)", (unsigned long) session->batch->current + 1, (unsigned long) session->batch->count);
    }

    char line[TRANSFER_NAME_MAX + 32];
    snprintf(line, sizeof(line), _("To %s"), session->peerName);
    transfer_draw_card(x1, y1, x2, y2, ICON_3DS, session->name, line, size);
}

static void transfer_send_free(transfer_send_session* session) {
    xferturn_release(session->turn);
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
static void transfer_send_list_exit(ui_view* view, transfer_send_data* data, linked_list* items);

// ---- a batch's steps (each takes the session: after one, the caller only returns) ----

// A title read once before it is sent in order: stopped (if it still runs) and let go.
static void transfer_batch_stop_hasher(transfer_send_session* session) {
    if(session->hasher != NULL) {
        session->hashCancel = true;
        threadJoin(session->hasher, U64_MAX);
        threadFree(session->hasher);
        session->hasher = NULL;
    }
}

static void transfer_batch_hash_thread(void* arg) {
    transfer_send_session* session = (transfer_send_session*) arg;
    session->hashResult = action_export_cia_ordered_hash(session->stream, &session->hashed, &session->hashCancel);
    session->hashDone = true;
}

// The batch is over: everything tried, stopped, or not accepted. The receiver is told (it shows its own summary),
// the sending screen and the list of consoles close, and a summary says what went. `why` is said of the titles left.
static void transfer_batch_end(ui_view* view, transfer_send_session* session, const char* why) {
    transfer_batch_send* batch = session->batch;
    transfer_send_data* list = session->list;

    transfer_batch_stop_hasher(session);
    if(session->stream != NULL) {
        action_export_cia_stream_free(session->stream);
        session->stream = NULL;
    }

    transfer_sender_free(session->engine);
    session->engine = NULL;

    bool accepted = batch->id != 0;
    if(accepted) {
        batch->asking = false;
        session->kind = TRANSFER_KIND_BATCH_END;
        session->size = 0;
        session->head = 0;
        session->check = 0;
        string_copy(session->name, "end", sizeof(session->name));
        if(transfer_send_connect(session)) {
            // It closes once it has read the hello; closing first could lose it.
            transfer_link_wait_closed(&session->link, 1500);
        }
    }

    u32 sent = 0;
    u32 failed = 0;
    for(u32 i = 0; i < batch->count; i++) {
        transfer_batch_item* item = &batch->items[i];
        if(item->state == BATCH_ITEM_WAITING) {
            item->state = BATCH_ITEM_FAILED;
            string_copy(item->error, why != NULL ? why : _("Not sent."), sizeof(item->error));
        }

        if(item->state == BATCH_ITEM_SENT) {
            sent++;
        } else {
            failed++;
        }
    }

    // Kept short: the dialog holds 512 bytes.
    char text[512];
    if(!accepted) {
        string_copy(text, why != NULL ? why : _("The other console declined the titles."), sizeof(text));
    } else {
        snprintf(text, sizeof(text), _("Sent %lu of %lu titles to %s."), (unsigned long) sent, (unsigned long) batch->count, session->peerName);
        u32 listed = 0;
        for(u32 i = 0; i < batch->count; i++) {
            transfer_batch_item* item = &batch->items[i];
            if(item->state != BATCH_ITEM_FAILED) {
                continue;
            }

            size_t used = strlen(text);
            if(listed == 3) {
                snprintf(text + used, sizeof(text) - used, "\n%s", _("And more."));
                break;
            }

            snprintf(text + used, sizeof(text) - used, "\n%.*s: %.*s", transfer_utf8_cut(item->name, 40), item->name,
                     transfer_utf8_cut(item->error, 90), item->error);
            listed++;
        }
    }

    transfer_log("send batch: %lu sent, %lu not\n", (unsigned long) sent, (unsigned long) failed);

    ui_pop();
    info_destroy(view);
    transfer_send_free(session);

    ui_view* listView = list->listView;
    transfer_send_list_exit(listView, list, list_get_items(listView));

    led_blink(accepted && failed == 0);
    prompt_display_notify(accepted && failed == 0 ? "Success" : "Failure", text, COLOR_TEXT, NULL, NULL, NULL);
}

// The next title that has not been tried: read from the system, then its own connection. The end when none is left.
static void transfer_batch_start_next(ui_view* view, transfer_send_session* session) {
    transfer_batch_send* batch = session->batch;

    transfer_batch_stop_hasher(session);
    if(session->stream != NULL) {
        action_export_cia_stream_free(session->stream);
        session->stream = NULL;
    }

    transfer_sender_free(session->engine);
    session->engine = NULL;
    transfer_link_close(&session->link);
    session->ordered = false;

    for(; batch->current < batch->count; batch->current++) {
        transfer_batch_item* item = &batch->items[batch->current];
        if(item->state != BATCH_ITEM_WAITING) {
            continue;
        }

        u8 way = batch->ways[batch->current];
        if(way == TRANSFER_WAY_SKIP) {
            item->state = BATCH_ITEM_FAILED;
            string_copy(item->error, _("No room for it on the other console."), sizeof(item->error));
            transfer_log("send batch: %s skipped, no room there\n", item->name);
            continue;
        }

        export_cia_data* stream = NULL;
        u64 size = 0;
        char name[FILE_NAME_MAX];
        Result res = action_export_cia_stream_open(&batch->titles[batch->current], &stream, &size, name, sizeof(name));
        if(R_FAILED(res)) {
            item->state = BATCH_ITEM_FAILED;
            snprintf(item->error, sizeof(item->error), "%s (%08lX)", _("Could not read the title."), res);
            transfer_log("send batch: %s could not be read (%08lX)\n", item->name, res);
            continue;
        }

        u32 headerSize = 0;
        const u8* header = action_export_cia_stream_header(stream, &headerSize);
        session->stream = stream;
        string_copy(session->name, name, sizeof(session->name));
        session->size = size;
        session->head = headerSize;
        session->check = transfer_crc32(0, header, headerSize < TRANSFER_CHECK_BYTES ? headerSize : TRANSFER_CHECK_BYTES);
        session->kind = TRANSFER_KIND_TITLE;
        session->start = 0;
        session->sent = 0;
        session->error[0] = '\0';
        batch->tries = 0;

        if(way == TRANSFER_WAY_ORDERED) {
            // Installed as it arrives: its TMD goes first, so every content is read once now to hash it.
            session->ordered = true;
            session->hashDone = false;
            session->hashCancel = false;
            session->hashed = 0;
            session->hashTotal = size - headerSize;
            session->hashResult = 0;
            session->hasher = threadCreate(transfer_batch_hash_thread, session, 0x8000, 0x30, -2, false);
            if(session->hasher == NULL) {
                item->state = BATCH_ITEM_FAILED;
                string_copy(item->error, _("Could not read the title."), sizeof(item->error));
                action_export_cia_stream_free(session->stream);
                session->stream = NULL;
                session->ordered = false;
                continue;
            }

            transfer_log("send batch: %s goes in order, reading it first\n", item->name);
            session->state = SEND_HASHING;
            return;
        }

        if(!transfer_send_connect(session)) {
            batch->tries = 1;
            session->state = SEND_PAUSE;
            batch->retryAt = osGetTime() + TRANSFER_BATCH_PAUSE;
        }

        return;
    }

    transfer_batch_end(view, session, NULL);
}

// The title being sent is done with, well or not; then the next one.
static void transfer_batch_item_over(ui_view* view, transfer_send_session* session, bool ok, const char* error) {
    transfer_batch_send* batch = session->batch;
    transfer_batch_item* item = &batch->items[batch->current];
    item->state = ok ? BATCH_ITEM_SENT : BATCH_ITEM_FAILED;
    string_copy(item->error, ok || error == NULL ? "" : error, sizeof(item->error));
    transfer_log("send batch: %s %s%s\n", item->name, ok ? "sent" : "given up: ", item->error);

    batch->current++;
    transfer_batch_start_next(view, session);
}

// This try failed: again in a moment (the receiver goes on from where it was), or given up after a few.
static void transfer_batch_retry(ui_view* view, transfer_send_session* session, const char* error) {
    transfer_batch_send* batch = session->batch;

    transfer_sender_free(session->engine);
    session->engine = NULL;
    transfer_link_close(&session->link);

    if(++batch->tries >= TRANSFER_BATCH_TRIES) {
        transfer_batch_item_over(view, session, false, error);
        return;
    }

    if(error != NULL && error != session->error) {
        string_copy(session->error, error, sizeof(session->error));
    }

    session->state = SEND_PAUSE;
    batch->retryAt = osGetTime() + TRANSFER_BATCH_PAUSE;
}

// Cancelled, here or there: the rest is not sent.
static void transfer_batch_stop(ui_view* view, transfer_send_session* session, const char* error) {
    transfer_batch_send* batch = session->batch;
    batch->stopped = true;
    if(!batch->asking && batch->current < batch->count) {
        batch->items[batch->current].state = BATCH_ITEM_FAILED;
        string_copy(batch->items[batch->current].error, error, sizeof(batch->items[batch->current].error));
    }

    transfer_batch_end(view, session, batch->asking ? error : _("Not sent: the batch was stopped."));
}

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
    if(session->batch != NULL) {
        // A batch asks nothing: the title is tried again by itself, unless the other console cancelled.
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
            transfer_batch_stop(view, session, _("The other console cancelled the transfer."));
            return;
        }

        char error[TRANSFER_BATCH_ERROR_MAX];
        snprintf(error, sizeof(error), "%s (%08lX)", _("Failed to send the file."), res);
        transfer_batch_retry(view, session, error);
        return;
    }

    ui_pop();
    info_destroy(view);

    transfer_sender_free(session->engine);
    session->engine = NULL;
    // Not held while the user is asked whether to try again: connecting takes it back.
    xferturn_release(session->turn);
    session->turn = 0;

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
    led_blink(false);
    error_display_res(NULL, NULL, res, "Failed to send the file.");
}

// The receiver agreed: cuts the payload in chunks from where it says to go on
// and starts the connections.
static Result transfer_send_begin_engine(transfer_send_session* session, u32 id) {
    u32 segmentCount = 1;
    u64* segments = NULL;
    if(session->folder != NULL) {
        segments = transfer_folder_send_segments(session->folder, &segmentCount);
    } else if(session->stream != NULL && session->ordered) {
        // A CIA in order: head, each content, tail.
        segments = (u64*) calloc(action_export_cia_stream_content_count(session->stream) + 2, sizeof(u64));
        if(segments != NULL) {
            segmentCount = action_export_cia_ordered_segments(session->stream, segments);
        }
    } else {
        if(session->stream != NULL) {
            segmentCount = 1 + action_export_cia_stream_content_count(session->stream);
        }

        segments = (u64*) calloc(segmentCount, sizeof(u64));
    }

    if(segments == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    if(session->folder != NULL || (session->stream != NULL && session->ordered)) {
        // Filled in already.
    } else if(session->stream != NULL) {
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
    transfer_chunk* chunks = transfer_chunks_build(segments, segmentCount, session->start, session->head > 0 && session->start > 0, &count);
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
    // Every frame, also while the other console's user decides: the turn stays alive.
    xferturn_progress(session->turn, session->sent);

    transfer_batch_send* batch = session->batch;
    if(session->state == SEND_HASHING) {
        // A batch's title read once before it goes in order.
        if(hidKeysDown() & KEY_B) {
            transfer_batch_stop(view, session, _("Cancelled."));
            return;
        }

        if(!session->hashDone) {
            u64 total = session->hashTotal;
            *progress = total != 0 ? (float) ((double) session->hashed / (double) total) : 0;
            snprintf(text, PROGRESS_TEXT_MAX, _("Title %lu of %lu\nReading it first: the other console\ninstalls it as it arrives."),
                     (unsigned long) batch->current + 1, (unsigned long) batch->count);
            return;
        }

        threadJoin(session->hasher, U64_MAX);
        threadFree(session->hasher);
        session->hasher = NULL;

        u64 size = 0;
        Result res = session->hashResult;
        if(R_SUCCEEDED(res)) {
            res = action_export_cia_ordered_make(session->stream, &size);
        }

        if(R_FAILED(res)) {
            char error[TRANSFER_BATCH_ERROR_MAX];
            snprintf(error, sizeof(error), "%s (%08lX)", _("Could not read the title."), res);
            transfer_batch_item_over(view, session, false, error);
            return;
        }

        session->size = size;
        session->kind = TRANSFER_KIND_CIA_ORDERED;
        session->head = 0;
        session->check = 0;
        if(!transfer_send_connect(session)) {
            batch->tries = 1;
            session->state = SEND_PAUSE;
            batch->retryAt = osGetTime() + TRANSFER_BATCH_PAUSE;
        }

        return;
    }

    if(session->state == SEND_PAUSE) {
        // A batch's title between two tries.
        if(hidKeysDown() & KEY_B) {
            transfer_batch_stop(view, session, _("Cancelled."));
            return;
        }

        if(osGetTime() >= batch->retryAt) {
            if(!transfer_send_connect(session)) {
                transfer_batch_retry(view, session, session->error);
            }

            return;
        }

        *progress = 0;
        snprintf(text, PROGRESS_TEXT_MAX, _("Title %lu of %lu\nTrying again in a moment..."), (unsigned long) batch->current + 1, (unsigned long) batch->count);
        return;
    }

    if(session->state == SEND_SENDING) {
        Result result = 0;
        bool done = transfer_sender_progress(session->engine, &session->sent, &result);

        if(done) {
            transfer_log("send %s link=%s sent=%llu secs=%.1f result=%08lX\n", session->name, session->uds ? "uds" : "ip",
                         session->sent, (osGetTime() - session->engineStart) / 1000.0, result);

            if(result == R_APP_CANCELLED && batch != NULL) {
                transfer_batch_stop(view, session, _("Cancelled."));
            } else if(result == R_APP_CANCELLED) {
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
            // An average over the last seconds (chunks and the card's writes come in bursts), as downloads do.
            u32 last = (u32) ((session->sent - session->speedSent) * 1000 / (now - session->speedTime));
            session->speed = session->speed == 0 ? last : (u32) (session->speed * 0.75f + last * 0.25f);
            session->speedSent = session->sent;
            session->speedTime = now;
        }

        // Where the file is, not what this try sent: a resume starts part way and
        // sends a title package's header again first.
        u64 again = session->start > 0 ? session->head : 0;
        u64 at = session->start + (session->sent > again ? session->sent - again : 0);
        if(at > session->size) {
            at = session->size;
        }

        u64 total = session->size;
        *progress = total != 0 ? (float) ((double) at / (double) total) : 0;
        if(batch != NULL) {
            snprintf(text, PROGRESS_TEXT_MAX, _("Title %lu of %lu\n%.2f %s / %.2f %s\n%.2f %s/s"), (unsigned long) batch->current + 1, (unsigned long) batch->count,
                     ui_get_display_size(at), ui_get_display_size_units(at),
                     ui_get_display_size(total), ui_get_display_size_units(total),
                     ui_get_display_size(session->speed), ui_get_display_size_units(session->speed));
            return;
        }

        snprintf(text, PROGRESS_TEXT_MAX, _("To %s\n%.2f %s / %.2f %s\n%.2f %s/s"), session->peerName,
                 ui_get_display_size(at), ui_get_display_size_units(at),
                 ui_get_display_size(total), ui_get_display_size_units(total),
                 ui_get_display_size(session->speed), ui_get_display_size_units(session->speed));
        return;
    }

    if(hidKeysDown() & KEY_B) {
        if(batch != NULL) {
            transfer_batch_stop(view, session, _("Cancelled."));
        } else {
            transfer_send_finish(view, session);
        }

        return;
    }

    if(session->state == SEND_FINISHING) {
        // One byte once the other console has the file whole (a title takes a
        // while to check).
        u8 ack = 0;
        int n = transfer_link_recv_some(&session->link, &ack, 1);
        if(n == 1 || n == 0 || n == -2) {
            bool ok = n == 1 && ack == 1;
            if(n == 1 && ack == 2 && batch != NULL) {
                transfer_log("send: the other console cancelled\n");
                transfer_batch_stop(view, session, _("The other console cancelled the transfer."));
                return;
            }

            if(n == 1 && ack == 2) {
                transfer_log("send: the other console cancelled\n");
                transfer_send_finish(view, session);

                prompt_display_notify("Failure", "The other console cancelled the transfer.", COLOR_TEXT, NULL, NULL, NULL);
                return;
            }

            transfer_log("send: final answer %s (n=%d)\n", ok ? "whole" : "NOT whole", n);

            if(ok && batch != NULL) {
                transfer_batch_item_over(view, session, true, NULL);
            } else if(ok) {
                transfer_send_finish(view, session);

                led_blink(true);
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

            if(batch != NULL && batch->asking) {
                // Yes for all of them: the number is the batch's, which each title quotes.
                transfer_log("send batch: accepted (%08lX), %lu titles\n", id, (unsigned long) batch->count);

                // How each one is to come, one byte each, right after the answer.
                u32 got = 0;
                for(u64 begin = osGetTime(); got < batch->count && osGetTime() - begin < 3000; ) {
                    int n = transfer_link_recv_some(&session->link, batch->ways + got, batch->count - got);
                    if(n > 0) {
                        got += (u32) n;
                    } else if(n == 0 || n == -2) {
                        break;
                    } else {
                        svcSleepThread(10000000);
                    }
                }

                if(got < batch->count) {
                    transfer_batch_end(view, session, _("The other console closed the connection."));
                    return;
                }

                batch->id = id != 0 ? id : 1;
                batch->asking = false;
                batch->current = 0;
                transfer_batch_start_next(view, session);
                return;
            }

            // A CIA in order always starts at its beginning: the install that a cut stopped starts again.
            session->start = !session->ordered && start < session->size && (session->head == 0 || start >= session->head) ? start : 0;

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
        if(batch != NULL && batch->asking) {
            transfer_batch_end(view, session, answered ? _("The other console declined the titles.") : _("The other console closed the connection."));
            return;
        } else if(batch != NULL && answered) {
            transfer_batch_item_over(view, session, false, _("The other console refused it."));
            return;
        } else if(batch != NULL) {
            transfer_batch_retry(view, session, _("The other console closed the connection."));
            return;
        }

        transfer_send_finish(view, session);

        prompt_display_notify("Failure", answered ? "The other console declined the file." : "The other console closed the connection.", COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    if(batch != NULL && !batch->asking) {
        // The receiver may still be installing the last one.
        snprintf(text, PROGRESS_TEXT_MAX, _("Title %lu of %lu\nWaiting for %s..."), (unsigned long) batch->current + 1, (unsigned long) batch->count, session->peerName);
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

// A batch's titles say nothing on the screen when they can not connect: the reason is kept for the summary.
static bool transfer_send_quiet(transfer_send_session* session) {
    return session->batch != NULL && !session->batch->asking;
}

static void transfer_send_connect_failed(transfer_send_session* session, Result res, int err, const char* text) {
    if(transfer_send_quiet(session)) {
        if(err != 0) {
            snprintf(session->error, sizeof(session->error), "%s (errno %d)", _(text), err);
        } else {
            snprintf(session->error, sizeof(session->error), "%s (%08lX)", _(text), res);
        }

        transfer_log("send: %s\n", session->error);
    } else if(err != 0) {
        error_display_errno(NULL, NULL, err, text);
    } else {
        error_display_res(NULL, NULL, res, text);
    }
}

// Opens the connection to the receiver and says what is coming. False (the
// reason shown, or kept in `error` for a batch's title) when it could not.
static bool transfer_send_connect(transfer_send_session* session) {
    transfer_link_close(&session->link);
    session->answerGot = 0;
    session->state = SEND_WAITING;

    // One transfer at a time: an FTP, web page or download under way is let finish.
    if(session->turn == 0 && (session->turn = xferturn_take(XFER_CONSOLE, session->name, session->size)) == 0) {
        transfer_send_connect_failed(session, R_APP_BUSY, 0, "Could not send the file.");
        return false;
    }

    if(session->uds) {
        Result res = udslink_connect(&session->udsPeer, &session->link.uds);
        if(R_FAILED(res)) {
            transfer_send_connect_failed(session, res, 0, "Could not connect to the other console.");
            return false;
        }

        session->link.fd = -1;
    } else {
        session->link.fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if(session->link.fd < 0) {
            transfer_send_connect_failed(session, 0, errno, "Failed to open socket.");
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
            transfer_send_connect_failed(session, 0, errno, "Could not connect to the other console.");
            return false;
        }

        transfer_set_nonblocking(session->link.fd);
    }

    transfer_log("send: %s (%llu bytes, kind %lu) to %s over %s\n", session->name, session->size, session->kind, session->peerName, session->uds ? "nearby" : "Wi-Fi");

    transfer_hello hello;
    memset(&hello, 0, sizeof(hello));
    memcpy(hello.magic, TRANSFER_MAGIC, sizeof(hello.magic));
    hello.version = htonl(TRANSFER_PROTOCOL);
    transfer_local_name(hello.sender, sizeof(hello.sender));
    string_copy(hello.file, session->name, sizeof(hello.file));
    hello.sizeHigh = htonl((u32) (session->size >> 32));
    hello.sizeLow = htonl((u32) (session->size & 0xFFFFFFFF));
    hello.check = htonl(session->check);
    hello.kind = htonl(session->kind);
    hello.head = htonl(session->head);
    string_copy(hello.origin, session->origin, sizeof(hello.origin));
    if(session->batch != NULL) {
        hello.batch = htonl(session->batch->id);
        hello.batchIndex = htonl(session->batch->current);
    }

    Result res = transfer_link_io(&session->link, true, &hello, sizeof(hello), 0, NULL);
    if(R_SUCCEEDED(res) && session->folder != NULL) {
        // The list of files goes first, so that the receiver can say what it would overwrite.
        u32 manifestSize = 0;
        const u8* manifest = transfer_folder_send_manifest(session->folder, &manifestSize);
        res = transfer_link_io(&session->link, true, (void*) manifest, manifestSize, 0, NULL);
    } else if(R_SUCCEEDED(res) && session->kind == TRANSFER_KIND_BATCH) {
        // The titles that come, for the question.
        res = transfer_link_io(&session->link, true, session->batch->list, session->batch->listSize, 0, NULL);
    }

    if(R_FAILED(res)) {
        transfer_send_connect_failed(session, res, 0, "Could not connect to the other console.");
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
    session->folder = source->folder;
    string_copy(session->origin, source->origin, sizeof(session->origin));
    session->kind = source->folder != NULL ? TRANSFER_KIND_FOLDER : source->stream != NULL ? TRANSFER_KIND_TITLE : TRANSFER_KIND_FILE;
    if(source->batch != NULL) {
        // The question for all the titles first; each one is sent once it is answered.
        transfer_batch_send* batch = source->batch;
        batch->asking = true;
        batch->id = 0;
        batch->current = 0;
        batch->tries = 0;
        batch->stopped = false;
        for(u32 i = 0; i < batch->count; i++) {
            batch->items[i].state = BATCH_ITEM_WAITING;
            batch->items[i].error[0] = '\0';
        }

        session->batch = batch;
        session->list = source;
        session->kind = TRANSFER_KIND_BATCH;
        session->head = batch->listSize;
        session->check = transfer_crc32(0, batch->list, batch->listSize < TRANSFER_CHECK_BYTES ? batch->listSize : TRANSFER_CHECK_BYTES);
    } else if(source->folder != NULL) {
        u32 manifestSize = 0;
        const u8* manifest = transfer_folder_send_manifest(source->folder, &manifestSize);
        session->head = manifestSize;
        session->check = transfer_crc32(0, manifest, manifestSize < TRANSFER_CHECK_BYTES ? manifestSize : TRANSFER_CHECK_BYTES);
    } else if(source->stream != NULL) {
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

    u32 ink = 0;
    u32 muted = 0;
    widget_item_frame(x, y, width, height, selected, &ink, &muted);

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

    transfer_draw_card(x1, y1, x2, y2, ICON_3DS, _("Send to another 3DS"), line, NULL);
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

    transfer_folder_send_free(data->folder);
    transfer_batch_free(data->batch);

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

static void transfer_send_begin(FS_Archive archive, const char* path, const char* name, u64 size, export_cia_data* stream, transfer_folder_send* folder, const char* origin, transfer_batch_send* batch) {
    if(size == 0) {
        prompt_display_notify("Failure", "The file is empty.", COLOR_TEXT, NULL, NULL, NULL);
        transfer_folder_send_free(folder);
        transfer_batch_free(batch);
        return;
    }

    transfer_send_data* data = (transfer_send_data*) calloc(1, sizeof(transfer_send_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate transfer data.");

        if(stream != NULL) {
            action_export_cia_stream_free(stream);
        }

        transfer_folder_send_free(folder);
        transfer_batch_free(batch);
        return;
    }

    data->stream = stream;
    data->folder = folder;
    data->batch = batch;
    string_copy(data->origin, origin != NULL ? origin : "", sizeof(data->origin));

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

            transfer_folder_send_free(folder);
            transfer_batch_free(batch);
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

        transfer_folder_send_free(folder);
        transfer_batch_free(batch);
        free(data);
        return;
    }

    data->listView = view;

    list_style style = {
            .rowHeight = 30,
            .drawRow = transfer_send_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
    list_add_tool(view, ICON_WIFI_OFF, transfer_send_tool_active, transfer_send_tool_press);
}

void transfer_send_file(FS_Archive archive, const char* path, const char* name, u64 size, const char* origin) {
    transfer_send_begin(archive, path, name, size, NULL, NULL, origin, NULL);
}

void transfer_send_folder(FS_Archive archive, const char* path, const char* name, const char* origin) {
    transfer_folder_send* folder = NULL;
    Result res = transfer_folder_send_scan(&folder, archive, path);
    if(R_FAILED(res)) {
        if(res == R_APP_OUT_OF_RANGE) {
            prompt_display_notify("Failure", "The folder has too many files or folders inside it to send.", COLOR_TEXT, NULL, NULL, NULL);
        } else {
            error_display_res(NULL, NULL, res, "Failed to read the folder.");
        }

        return;
    }

    if(transfer_folder_send_files(folder) == 0) {
        transfer_folder_send_free(folder);
        prompt_display_notify("Failure", "The folder has no files.", COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    // The folder's own path, with no slash at the end.
    char where[FILE_PATH_MAX];
    string_copy(where, origin != NULL ? origin : "", sizeof(where));
    size_t len = strlen(where);
    while(len > 1 && where[len - 1] == '/') {
        where[--len] = '\0';
    }

    transfer_send_begin(archive, path, name, transfer_folder_send_total(folder), NULL, folder, where, NULL);
}

void transfer_send_stream(export_cia_data* stream, const char* name, u64 size) {
    transfer_send_begin(0, "", name, size, stream, NULL, NULL, NULL);
}

static void transfer_batch_free(transfer_batch_send* batch) {
    if(batch == NULL) {
        return;
    }

    free(batch->titles);
    free(batch->items);
    free(batch->list);
    free(batch);
}

void transfer_send_titles(title_info* titles, u32 count) {
    if(count > TRANSFER_BATCH_MAX) {
        free(titles);
        char text[128];
        snprintf(text, sizeof(text), _("At most %u titles can be sent at once."), TRANSFER_BATCH_MAX);
        prompt_display_notify("Failure", text, COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    transfer_batch_send* batch = (transfer_batch_send*) calloc(1, sizeof(transfer_batch_send));
    if(batch != NULL) {
        batch->titles = titles;
        batch->count = count;
        batch->items = (transfer_batch_item*) calloc(count, sizeof(transfer_batch_item));
        batch->listSize = TRANSFER_BATCH_HEAD + count * TRANSFER_BATCH_ENTRY;
        batch->list = (u8*) calloc(1, batch->listSize);
    }

    if(batch == NULL || batch->items == NULL || batch->list == NULL) {
        if(batch == NULL) {
            free(titles);
        }

        transfer_batch_free(batch);
        error_display(NULL, NULL, "Failed to allocate transfer data.");
        return;
    }

    // Each title's size as a package and its name: read from the system now, then let go (each is opened again when
    // its turn comes).
    u8* list = batch->list;
    memcpy(list, "NSAB", 4);
    list[4] = (u8) (count >> 24);
    list[5] = (u8) (count >> 16);
    list[6] = (u8) (count >> 8);
    list[7] = (u8) count;

    for(u32 i = 0; i < count; i++) {
        export_cia_data* stream = NULL;
        u64 size = 0;
        Result res = action_export_cia_stream_open(&titles[i], &stream, &size, batch->items[i].name, sizeof(batch->items[i].name));
        if(R_FAILED(res)) {
            if(res == R_APP_NOT_IMPLEMENTED) {
                error_display(NULL, NULL, "Exporting a CIA needs Luma3DS.");
            } else {
                error_display_res(&titles[i], task_draw_title_info, res, "Failed to prepare the CIA.");
            }

            transfer_batch_free(batch);
            return;
        }

        action_export_cia_stream_free(stream);
        batch->items[i].size = size;
        batch->total += size;

        u8* entry = list + TRANSFER_BATCH_HEAD + i * TRANSFER_BATCH_ENTRY;
        transfer_put64(entry, size);
        transfer_put64(entry + 8, titles[i].titleId);
        entry[16] = 0;
        entry[17] = 0;
        entry[18] = (u8) (titles[i].version >> 8);
        entry[19] = (u8) titles[i].version;
        string_copy((char*) entry + 24, batch->items[i].name, FILE_NAME_MAX);
    }

    char name[FILE_NAME_MAX];
    snprintf(name, sizeof(name), _("%lu titles"), (unsigned long) count);
    transfer_send_begin(0, "", name, batch->total, NULL, NULL, NULL, batch);
}

// ---- receiving ----

typedef enum {
    RECEIVE_IDLE,
    RECEIVE_HELLO,
    RECEIVE_MANIFEST,
    RECEIVE_ASKING,
    RECEIVE_RECEIVING,
    RECEIVE_INSTALLING   // A batch's title, received, is being installed: no sender is taken meanwhile.
} transfer_receive_state;

// A batch the user here said yes to (issue #53): its titles come one by one, taken without asking.
typedef enum {
    BATCH_ENTRY_WAITING,
    BATCH_ENTRY_SAVED,
    BATCH_ENTRY_INSTALLED,
    BATCH_ENTRY_FAILED
} transfer_batch_entry_state;

typedef struct {
    char name[FILE_NAME_MAX];
    u64 size;
    u64 titleId;
    u16 version;
    u8 way;                  // How it comes (TRANSFER_WAY_*), chosen when the batch was accepted.
    transfer_batch_entry_state state;
    char error[TRANSFER_BATCH_ERROR_MAX];
} transfer_batch_entry;

typedef struct {
    bool active;
    u32 id;
    char sender[TRANSFER_NAME_MAX + 1];
    bool install;
    u32 count;
    u64 total;
    transfer_batch_entry entries[TRANSFER_BATCH_MAX];
} transfer_batch_recv;

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
    char message[FILE_NAME_MAX + 384];

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
    bool lastKeepPath;
    u32 lastOverwrite;

    // The sender's original path (see transfer_hello): offered as a place to save when it is safe.
    char origin[FILE_PATH_MAX];
    bool offerOrigin;
    bool keepPath;           // Chosen: the files go to `origin`, not to `dir`.
    u32 overwrite;           // What to do with files that exist there: TRANSFER_OVERWRITE_*.
    u8* preManifest;         // A folder's manifest as it arrives, before the question.
    u32 preGot;
    u64 preStart;
    folder_layout layout;    // ... and read, to count what exists and to place the files.
    bool haveLayout;
    u32 existing;
    u32 fileCount;

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
    transfer_folder_recv* folder;    // Writes the files of a folder.
    LightLock fileLock;
    u32 head;                        // Header bytes of a title package, or a folder's manifest (sent again on a resume).
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
    u32 turn;           // The transfer turn (xferturn.h), from connecting (sending) or accepting (receiving) to the end.

    // Batches (issue #53): the one accepted, the one being asked about, and what the hello said of this transfer.
    transfer_batch_recv batch;
    transfer_batch_recv* offer;
    char offerNote[384];     // What a short SD card changes, on the top screen while the batch is asked about.
    u32 helloBatch;
    u32 helloIndex;
    bool batchItem;          // This transfer is a title of `batch`, at `batchIndex`.
    u32 batchIndex;

    // A batch's title being installed (RECEIVE_INSTALLING), on its own thread.
    Thread installer;
    volatile bool installDone;
    bool installOk;
    volatile u64 installAt;
    volatile u64 installTotal;
    const char* installReason;
    Result installResult;
    char installPath[FILE_PATH_MAX];

    // A batch's title that comes as a CIA in order (TRANSFER_KIND_CIA_ORDERED): written into AM as it arrives, each
    // chunk waiting for the ones before it (they come in order on each connection, not across them).
    Handle amHandle;
    u64 orderedNext;
    volatile bool orderedFailed;
    volatile bool orderedAbort;
    cia_seed_watch orderedSeed;
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

        if(data->kind != TRANSFER_KIND_CIA_ORDERED && data->prefix >= data->positionSaved + TRANSFER_POSITION_EVERY) {
            transfer_receive_save_position(data);
        }
    } else if(data->pendingCount < sizeof(data->pending) / sizeof(*data->pending)) {
        data->pending[data->pendingCount].offset = offset;
        data->pending[data->pendingCount].len = len;
        data->pendingCount++;
    }
}

static Result transfer_ordered_write(transfer_receive_data* data, u64 offset, const void* buffer, u32 size);
static void transfer_ordered_cancel(transfer_receive_data* data);
static void transfer_ordered_finish_thread(void* arg);

// The bytes at `offset` of the payload, from any connection.
static Result transfer_receive_write(void* user, u64 offset, const void* buffer, u32 size) {
    transfer_receive_data* data = (transfer_receive_data*) user;

    if(selftest_fault("recv.write")) {
        return R_APP_IO_ERROR;
    }

    Result res = 0;
    if(data->kind == TRANSFER_KIND_CIA_ORDERED) {
        res = transfer_ordered_write(data, offset, buffer, size);
    } else if(data->folder != NULL) {
        res = transfer_folder_recv_write(data->folder, offset, buffer, size);
    } else if(data->sink != NULL) {
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

    char from[TRANSFER_NAME_MAX + 32];
    snprintf(from, sizeof(from), _("From %s"), receive->sender);
    char line[sizeof(from) + 64];
    snprintf(line, sizeof(line), "%s  (%s)", from, size);
    if(receive->batchItem) {
        size_t used = strlen(line);
        snprintf(line + used, sizeof(line) - used, "  %lu/%lu", (unsigned long) receive->batchIndex + 1, (unsigned long) receive->batch.count);
    }

    // Where it is going, once the user has chosen.
    char where[FILE_PATH_MAX + 32];
    where[0] = '\0';
    if(receive->state == RECEIVE_RECEIVING) {
        if(receive->install && receive->isSave) {
            string_copy(where, _("Importing it into the title"), sizeof(where));
        } else if(receive->install) {
            string_copy(where, _("Installing it on this console"), sizeof(where));
        } else {
            snprintf(where, sizeof(where), _("Saving in %s"), receive->keepPath ? receive->origin : receive->dir);
        }
    }

    transfer_draw_card(x1, y1, x2, y2, ICON_DOWNLOAD, receive->file, line, where);
}

static void transfer_receive_reset(transfer_receive_data* data) {
    xferturn_release(data->turn);
    data->turn = 0;

    if(data->client.uds != NULL) {
        data->ignoreUntil = osGetTime() + 4000;
    }

    transfer_link_close(&data->client);
    data->state = RECEIVE_IDLE;
    data->got = 0;

    free(data->preManifest);
    data->preManifest = NULL;
    if(data->haveLayout) {
        folder_layout_free(&data->layout);
        data->haveLayout = false;
    }

    free(data->offer);
    data->offer = NULL;
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

// Renames the finished <name>.part folder to its final name (not over a folder already there).
static Result transfer_receive_finish_folder(transfer_receive_data* data, const char* name) {
    snprintf(data->dst, sizeof(data->dst), "%s%s", data->dir, name);
    for(int i = 2; i < 100 && fs_is_dir(transfer_sd_archive(), data->dst); i++) {
        snprintf(data->dst, sizeof(data->dst), "%s%s (%d)", data->dir, name, i);
    }

    // The part is "<name>.part"; both are folders on the card.
    char partDir[FILE_PATH_MAX];
    string_copy(partDir, data->part, sizeof(partDir));

    Result res = 0;

    FS_Path* from = fs_make_path_utf8(partDir);
    FS_Path* to = fs_make_path_utf8(data->dst);
    if(from != NULL && to != NULL) {
        res = FSUSER_RenameDirectory(transfer_sd_archive(), *from, transfer_sd_archive(), *to);
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
               && data->kind == TRANSFER_KIND_FOLDER) {
                // The files are in the <name>.part folder; the manifest is sent again first.
                if(fs_is_dir(transfer_sd_archive(), data->part)) {
                    start = position - TRANSFER_RESUME_MARGIN;
                    if(start < data->head) {
                        start = 0;
                    }
                }
            } else if(position > TRANSFER_RESUME_MARGIN && position <= data->size && size == data->size && check == data->check
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

    // A chunk waiting for its turn (a CIA in order) lets its connection go.
    data->orderedAbort = true;
    transfer_receiver_free(data->engine);
    data->engine = NULL;
    xferturn_release(data->turn);
    data->turn = 0;

    if(data->finalizer != NULL) {
        threadJoin(data->finalizer, U64_MAX);
        threadFree(data->finalizer);
        data->finalizer = NULL;
    }

    if(data->kind == TRANSFER_KIND_CIA_ORDERED) {
        // Nothing to go on from: a CIA in order that stopped is installed from its start next time.
        if(!succeeded || data->amHandle != 0) {
            transfer_ordered_cancel(data);
        }

        return succeeded && data->amHandle == 0 ? 0 : res;
    }

    if(!succeeded && data->prefix > 0) {
        transfer_receive_save_position(data);
    }

    if(data->folder != NULL) {
        if(succeeded) {
            res = transfer_folder_recv_finish(data->folder);
        }

        transfer_folder_recv_free(data->folder);
        data->folder = NULL;
    } else if(data->sink != NULL) {
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

// ---- the sender's original path ----

// An original path that is safe to write in: inside the SD card, with no ".." or odd characters,
// and not inside NSA's own working folder.
static bool transfer_origin_ok(const char* origin) {
    size_t len = strlen(origin);
    if(len < 2 || origin[0] != '/' || origin[len - 1] == '/' || !folder_path_ok(origin + 1)) {
        return false;
    }

    static const char work[] = "/nsa/tmp";
    return !(strncmp(origin, work, sizeof(work) - 1) == 0 && (origin[sizeof(work) - 1] == '/' || origin[sizeof(work) - 1] == '\0'));
}

static bool transfer_origin_join(char* out, size_t size, const char* origin, const char* rel) {
    return (size_t) snprintf(out, size, "%s/%s", origin, rel) < size;
}

static bool transfer_path_exists(const char* path) {
    return transfer_sd_exists(path) || fs_is_dir(transfer_sd_archive(), path);
}

// How many of the files would be overwritten at the original path.
static void transfer_receive_count_existing(transfer_receive_data* data) {
    data->existing = 0;
    data->fileCount = 0;

    if(data->kind == TRANSFER_KIND_FILE) {
        data->fileCount = 1;
        data->existing = transfer_path_exists(data->origin) ? 1 : 0;
        return;
    }

    if(!data->haveLayout) {
        return;
    }

    for(u32 i = 0; i < data->layout.fileCount; i++) {
        const folder_entry* entry = &data->layout.entries[data->layout.files[i]];

        char path[FILE_PATH_MAX];
        data->fileCount++;
        if(transfer_origin_join(path, sizeof(path), data->origin, entry->path) && transfer_path_exists(path)) {
            data->existing++;
        }
    }
}

// One folder; a file in its place is an error, never deleted for it.
static Result transfer_origin_make_dir(const char* path) {
    if(fs_is_dir(transfer_sd_archive(), path)) {
        return 0;
    }

    if(transfer_sd_exists(path)) {
        return R_APP_BAD_DATA;
    }

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = FSUSER_CreateDirectory(transfer_sd_archive(), *fsPath, 0);
    fs_free_path_utf8(fsPath);
    return res;
}

// The folders above `path`.
static Result transfer_origin_make_parents(const char* path) {
    char copy[FILE_PATH_MAX];
    string_copy(copy, path, sizeof(copy));

    for(char* slash = strchr(copy + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        Result res = transfer_origin_make_dir(copy);
        *slash = '/';

        if(R_FAILED(res)) {
            return res;
        }
    }

    return 0;
}

// Moves a finished file into place. A file that is already there is replaced, or left as it is
// when the user chose to skip the existing ones.
static Result transfer_origin_move_file(const char* from, const char* to, u32 overwrite) {
    Result res = transfer_origin_make_parents(to);
    if(R_FAILED(res)) {
        return res;
    }

    if(transfer_path_exists(to)) {
        if(overwrite == TRANSFER_OVERWRITE_SKIP) {
            return 0;
        }

        FS_Path* old = fs_make_path_utf8(to);
        if(old == NULL) {
            return R_APP_OUT_OF_MEMORY;
        }

        res = FSUSER_DeleteFile(transfer_sd_archive(), *old);   // Fails on a folder: an error, not a silent loss.
        fs_free_path_utf8(old);
        if(R_FAILED(res)) {
            return res;
        }
    }

    FS_Path* source = fs_make_path_utf8(from);
    FS_Path* target = fs_make_path_utf8(to);
    if(source != NULL && target != NULL) {
        res = FSUSER_RenameFile(transfer_sd_archive(), *source, transfer_sd_archive(), *target);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    if(source != NULL) {
        fs_free_path_utf8(source);
    }

    if(target != NULL) {
        fs_free_path_utf8(target);
    }

    return res;
}

// The transfer is whole in nsa/tmp: puts the file, or each file of the folder, at the sender's path.
static Result transfer_receive_place_original(transfer_receive_data* data) {
    string_copy(data->dst, data->origin, sizeof(data->dst));

    if(data->kind == TRANSFER_KIND_FILE) {
        return transfer_origin_move_file(data->part, data->origin, data->overwrite);
    }

    if(!data->haveLayout) {
        return R_APP_BAD_DATA;
    }

    char path[FILE_PATH_MAX];
    if(!transfer_origin_join(path, sizeof(path), data->origin, "x")) {
        return R_APP_OUT_OF_RANGE;
    }

    Result res = transfer_origin_make_parents(path);   // The folder itself and what is above it.

    for(u32 i = 0; R_SUCCEEDED(res) && i < data->layout.count; i++) {
        const folder_entry* entry = &data->layout.entries[i];

        char dest[FILE_PATH_MAX];
        if(!transfer_origin_join(dest, sizeof(dest), data->origin, entry->path)) {
            res = R_APP_OUT_OF_RANGE;
        } else if(entry->isDir) {
            res = transfer_origin_make_dir(dest);
        } else {
            char source[FILE_PATH_MAX];
            if((size_t) snprintf(source, sizeof(source), "%s/%s", data->part, entry->path) >= sizeof(source)) {
                res = R_APP_OUT_OF_RANGE;
            } else {
                res = transfer_origin_move_file(source, dest, data->overwrite);
            }
        }
    }

    if(R_SUCCEEDED(res)) {
        // What is left (the files that were skipped) is of no use.
        FS_Path* left = fs_make_path_utf8(data->part);
        if(left != NULL) {
            FSUSER_DeleteDirectoryRecursively(transfer_sd_archive(), *left);
            fs_free_path_utf8(left);
        }
    }

    return res;
}

// ---- a batch, received (issue #53) ----

static u64 transfer_sd_free() {
    // Tests (DEBUG builds): /nsa/batch-free.txt says how many bytes to take as free, to try the ways a small card takes.
    if(NSA_DEBUG) {
        Handle file = 0;
        if(R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, APP_DATA_DIR "batch-free.txt"), FS_OPEN_READ, 0))) {
            char text[24] = {0};
            u32 read = 0;
            FSFILE_Read(file, &read, 0, text, sizeof(text) - 1);
            FSFILE_Close(file);
            return strtoull(text, NULL, 10);
        }
    }

    FS_ArchiveResource resource = {0};
    if(R_SUCCEEDED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD))) {
        return (u64) resource.freeClusters * resource.clusterSize;
    }

    return 0;
}

// The batch's list (`head` bytes, see TRANSFER_BATCH_HEAD), read after the hello: into `offer`. False when it is not one.
static bool transfer_batch_parse(transfer_receive_data* data) {
    const u8* list = data->preManifest;
    u32 count = list != NULL && data->head >= TRANSFER_BATCH_HEAD && memcmp(list, "NSAB", 4) == 0
                ? ((u32) list[4] << 24) | ((u32) list[5] << 16) | ((u32) list[6] << 8) | list[7] : 0;
    if(count == 0 || count > TRANSFER_BATCH_MAX || data->head != TRANSFER_BATCH_HEAD + count * TRANSFER_BATCH_ENTRY) {
        return false;
    }

    transfer_batch_recv* offer = (transfer_batch_recv*) calloc(1, sizeof(transfer_batch_recv));
    if(offer == NULL) {
        return false;
    }

    offer->count = count;
    for(u32 i = 0; i < count; i++) {
        const u8* entry = list + TRANSFER_BATCH_HEAD + i * TRANSFER_BATCH_ENTRY;
        offer->entries[i].size = transfer_get64(entry);
        offer->entries[i].titleId = transfer_get64(entry + 8);
        offer->entries[i].version = (u16) (((u16) entry[18] << 8) | entry[19]);
        memcpy(offer->entries[i].name, entry + 24, FILE_NAME_MAX);
        offer->entries[i].name[FILE_NAME_MAX - 1] = '\0';
        offer->total += offer->entries[i].size;
    }

    if(offer->total != data->size) {
        free(offer);
        return false;
    }

    free(data->offer);
    data->offer = offer;
    return true;
}

// The top screen while the batch is asked about: how many, from whom, and their names.
static void transfer_batch_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    transfer_receive_data* receive = (transfer_receive_data*) data;
    transfer_batch_recv* offer = receive->offer;
    if(offer == NULL) {
        return;
    }

    char size[32];
    transfer_size_text(size, sizeof(size), offer->total);
    char title[64];
    snprintf(title, sizeof(title), _("%lu titles"), (unsigned long) offer->count);
    char from[TRANSFER_NAME_MAX + 64];
    snprintf(from, sizeof(from), _("From %s"), receive->sender);
    size_t used = strlen(from);
    snprintf(from + used, sizeof(from) - used, "  (%s)", size);

    char names[400];
    names[0] = '\0';
    for(u32 i = 0; i < offer->count; i++) {
        used = strlen(names);
        if(used + 4 >= sizeof(names)) {
            break;
        }

        snprintf(names + used, sizeof(names) - used, "%s%s", i > 0 ? ", " : "", offer->entries[i].name);
    }

    // What a short SD card changes, first.
    char text[sizeof(names) + sizeof(receive->offerNote) + 4];
    snprintf(text, sizeof(text), "%s%s%s", receive->offerNote, receive->offerNote[0] != 0 ? "\n\n" : "", names);
    transfer_draw_card(x1, y1, x2, y2, ICON_DOWNLOAD, title, from, text);
}

// How each title is to come, from the free space here, in the list's order (`ways`, TRANSFER_WAY_*; `counts` by way):
// saved, a title needs its size; installed, twice its size for today's way (its CIA, then the title, and the CIA is
// deleted), or its size once installed as it arrives. What does not fit at all is skipped.
static void transfer_batch_plan(const transfer_batch_recv* offer, bool install, u64 freeSpace, u8* ways, u32* counts) {
    u64 left = freeSpace > TRANSFER_BATCH_MARGIN ? freeSpace - TRANSFER_BATCH_MARGIN : 0;
    counts[TRANSFER_WAY_SKIP] = counts[TRANSFER_WAY_PACKAGE] = counts[TRANSFER_WAY_ORDERED] = 0;

    for(u32 i = 0; i < offer->count; i++) {
        u64 size = offer->entries[i].size;
        u8 way = TRANSFER_WAY_SKIP;
        if(left >= size * 2 || (!install && left >= size)) {
            way = TRANSFER_WAY_PACKAGE;
        } else if(install && left >= size) {
            way = TRANSFER_WAY_ORDERED;
        }

        if(way != TRANSFER_WAY_SKIP) {
            left -= size;
        }

        ways[i] = way;
        counts[way]++;
    }
}

static void transfer_batch_accept(transfer_receive_data* data, bool install) {
    if(data->offer == NULL) {
        transfer_receive_reset(data);
        return;
    }

    data->batch = *data->offer;
    data->batch.active = true;
    data->batch.install = install;
    string_copy(data->batch.sender, data->sender, sizeof(data->batch.sender));
    do {
        data->batch.id = (u32) osGetTime() ^ (u32) svcGetSystemTick();
    } while(data->batch.id == 0);

    u8 ways[TRANSFER_BATCH_MAX];
    u32 counts[3];
    transfer_batch_plan(&data->batch, install, transfer_sd_free(), ways, counts);
    for(u32 i = 0; i < data->batch.count; i++) {
        data->batch.entries[i].way = ways[i];
        if(ways[i] == TRANSFER_WAY_SKIP) {
            data->batch.entries[i].state = BATCH_ENTRY_FAILED;
            string_copy(data->batch.entries[i].error, _("No room for it on the SD card."), sizeof(data->batch.entries[i].error));
        }
    }

    transfer_log("receive batch: %lu titles from %s accepted (%08lX), %s: %lu as usual, %lu in order, %lu skipped\n",
                 (unsigned long) data->batch.count, data->sender, data->batch.id, install ? "to install" : "to save",
                 (unsigned long) counts[TRANSFER_WAY_PACKAGE], (unsigned long) counts[TRANSFER_WAY_ORDERED], (unsigned long) counts[TRANSFER_WAY_SKIP]);

    // The answer, then one byte per title: how it is to come.
    transfer_receive_answer(data, 1, 0, data->batch.id);
    transfer_link_io(&data->client, true, ways, data->batch.count, 0, NULL);
    // The sender closes once it has the answer: closing first could lose it.
    transfer_link_wait_closed(&data->client, 1500);
    transfer_receive_reset(data);
}

static void transfer_batch_accept_install(void* data) {
    transfer_batch_accept((transfer_receive_data*) data, true);
}

static void transfer_batch_accept_save(void* data) {
    transfer_batch_accept((transfer_receive_data*) data, false);
}

static void transfer_batch_reject(void* data) {
    transfer_receive_data* receive = (transfer_receive_data*) data;
    transfer_receive_answer(receive, 0, 0, 0);
    transfer_receive_reset(receive);
}

// A line more for the note over the names.
static void transfer_batch_note(transfer_receive_data* data, const char* line) {
    size_t used = strlen(data->offerNote);
    snprintf(data->offerNote + used, sizeof(data->offerNote) - used, "%s%s", used > 0 ? "\n" : "", line);
}

// One question for all the titles: install them, keep their CIAs, or say no. When the SD card is short of room it says
// what that changes: titles installed as they arrive, or left out.
static void transfer_batch_ask(transfer_receive_data* data) {
    transfer_batch_recv* offer = data->offer;

    char size[32];
    transfer_size_text(size, sizeof(size), offer->total);
    snprintf(data->message, sizeof(data->message), _("%s wants to send you %lu titles (%s)."), data->sender, (unsigned long) offer->count, size);

    u64 freeSpace = transfer_sd_free();
    u8 ways[TRANSFER_BATCH_MAX];
    u32 installCounts[3];
    u32 saveCounts[3];
    transfer_batch_plan(offer, true, freeSpace, ways, installCounts);
    transfer_batch_plan(offer, false, freeSpace, ways, saveCounts);

    // The sheet's header holds one line: this goes on the top screen, over the names.
    data->offerNote[0] = 0;
    char line[160];
    if(installCounts[TRANSFER_WAY_ORDERED] > 0) {
        snprintf(line, sizeof(line), _("Little room: %lu will install as they arrive (slower)."), (unsigned long) installCounts[TRANSFER_WAY_ORDERED]);
        transfer_batch_note(data, line);
    }

    if(installCounts[TRANSFER_WAY_SKIP] > 0) {
        snprintf(line, sizeof(line), _("%lu do not fit and will be skipped."), (unsigned long) installCounts[TRANSFER_WAY_SKIP]);
        transfer_batch_note(data, line);
    }

    if(saveCounts[TRANSFER_WAY_SKIP] > installCounts[TRANSFER_WAY_SKIP]) {
        snprintf(line, sizeof(line), _("Saved instead, %lu would not fit."), (unsigned long) saveCounts[TRANSFER_WAY_SKIP]);
        transfer_batch_note(data, line);
    }

    transfer_log("receive batch: %s offers %lu titles (%llu bytes), %llu free\n", data->sender, (unsigned long) offer->count, offer->total, freeSpace);

    sheet_action actions[3];
    u32 count = 0;
    actions[count++] = (sheet_action) {.icon = ICON_PACKAGE, .label = _("Install all"), .run = transfer_batch_accept_install,
                                       .detail = _("Each one is installed as it arrives, and its CIA deleted")};
    actions[count++] = (sheet_action) {.icon = ICON_FOLDER, .label = _("Save all"), .run = transfer_batch_accept_save,
                                       .detail = _("Their CIAs are kept in /nsa/received/")};
    actions[count++] = (sheet_action) {.icon = ICON_X, .label = _("Reject"), .run = transfer_batch_reject,
                                       .detail = _("Tells the other console no")};

    data->state = RECEIVE_ASKING;
    sheet_display_choice(data->message, actions, count, data, transfer_batch_draw_top, transfer_batch_reject);
    sheet_set_id("Incoming titles");
}

// The sender said the batch is over (or the user here cancelled it): what arrived, what did not, and why.
static void transfer_batch_summary(transfer_receive_data* data) {
    transfer_batch_recv* batch = &data->batch;
    batch->active = false;

    u32 done = 0;
    for(u32 i = 0; i < batch->count; i++) {
        transfer_batch_entry* entry = &batch->entries[i];
        if(entry->state == BATCH_ENTRY_WAITING) {
            entry->state = BATCH_ENTRY_FAILED;
            string_copy(entry->error, _("Not received."), sizeof(entry->error));
        }

        if(entry->state != BATCH_ENTRY_FAILED) {
            done++;
        }
    }

    char text[512];
    if(batch->install) {
        snprintf(text, sizeof(text), _("Installed %lu of %lu titles from %s."), (unsigned long) done, (unsigned long) batch->count, batch->sender);
    } else {
        snprintf(text, sizeof(text), _("Received %lu of %lu titles from %s, in %s."), (unsigned long) done, (unsigned long) batch->count, batch->sender, TRANSFER_RECEIVED_DIR);
    }

    u32 listed = 0;
    for(u32 i = 0; i < batch->count; i++) {
        transfer_batch_entry* entry = &batch->entries[i];
        if(entry->state != BATCH_ENTRY_FAILED) {
            continue;
        }

        size_t used = strlen(text);
        if(listed == 3) {
            snprintf(text + used, sizeof(text) - used, "\n%s", _("And more."));
            break;
        }

        snprintf(text + used, sizeof(text) - used, "\n%.*s: %.*s", transfer_utf8_cut(entry->name, 40), entry->name,
                 transfer_utf8_cut(entry->error, 90), entry->error);
        listed++;
    }

    transfer_log("receive batch: over, %lu of %lu\n", (unsigned long) done, (unsigned long) batch->count);

    bool ok = done == batch->count;
    led_blink(ok);
    prompt_display_notify(ok ? "Success" : "Failure", text, COLOR_TEXT, NULL, NULL, NULL);
}

// Every title is here: the summary now, without waiting for the sender's end (which a lost connection could keep away).
static void transfer_batch_check_all(transfer_receive_data* data) {
    for(u32 i = 0; i < data->batch.count; i++) {
        if(data->batch.entries[i].state != BATCH_ENTRY_SAVED && data->batch.entries[i].state != BATCH_ENTRY_INSTALLED) {
            return;
        }
    }

    transfer_batch_summary(data);
}

static void transfer_batch_install_thread(void* arg) {
    transfer_receive_data* data = (transfer_receive_data*) arg;
    data->installOk = action_install_cia_quietly(data->installPath, &data->installAt, &data->installTotal, &data->installReason, &data->installResult);
    data->installDone = true;
}

// A batch's title is over here: kept, or installed next (on a thread, while the waiting screen says so).
static void transfer_batch_received(transfer_receive_data* data, bool ok, Result res) {
    transfer_batch_entry* entry = &data->batch.entries[data->batchIndex];
    data->batchItem = false;

    if(!ok) {
        // The sender tries it again by itself, unless it was cancelled; a later success replaces this.
        entry->state = BATCH_ENTRY_FAILED;
        snprintf(entry->error, sizeof(entry->error), "%s (%08lX)", res == R_APP_CANCELLED ? _("Cancelled.") : _("Failed to receive the file."), res);
        return;
    }

    if(data->kind == TRANSFER_KIND_CIA_ORDERED) {
        // Installed as it arrived.
        entry->state = BATCH_ENTRY_INSTALLED;
        entry->error[0] = '\0';
        transfer_batch_check_all(data);
        return;
    }

    if(!data->batch.install) {
        entry->state = BATCH_ENTRY_SAVED;
        entry->error[0] = '\0';
        transfer_batch_check_all(data);
        return;
    }

    string_copy(data->installPath, data->dst, sizeof(data->installPath));
    data->installDone = false;
    data->installOk = false;
    data->installAt = 0;
    data->installTotal = entry->size;
    data->installReason = NULL;
    data->installResult = 0;
    data->installer = threadCreate(transfer_batch_install_thread, data, 0x8000, 0x30, -2, false);
    if(data->installer == NULL) {
        entry->state = BATCH_ENTRY_FAILED;
        string_copy(entry->error, _("Could not start the install."), sizeof(entry->error));
        return;
    }

    transfer_log("receive batch: installing %s\n", data->dst);
    data->state = RECEIVE_INSTALLING;
}

// RECEIVE_INSTALLING, each frame: true while the install goes on (its line in `text`).
static bool transfer_batch_installing(transfer_receive_data* data, char* text) {
    transfer_batch_entry* entry = &data->batch.entries[data->batchIndex];

    if(!data->installDone) {
        u64 total = data->installTotal;
        u32 percent = total != 0 ? (u32) (data->installAt * 100 / total) : 0;
        // The name on a line of its own, cut to what the box holds (between two characters, not inside one).
        size_t cut = strlen(entry->name);
        if(cut > 34) {
            cut = 34;
            while(cut > 0 && ((u8) entry->name[cut] & 0xC0) == 0x80) {
                cut--;
            }
        }

        snprintf(text, PROGRESS_TEXT_MAX, _("Installing title %lu of %lu...\n%.*s\n%lu %%"), (unsigned long) data->batchIndex + 1,
                 (unsigned long) data->batch.count, (int) cut, entry->name, (unsigned long) percent);
        return true;
    }

    threadJoin(data->installer, U64_MAX);
    threadFree(data->installer);
    data->installer = NULL;

    if(data->installOk) {
        entry->state = BATCH_ENTRY_INSTALLED;
        entry->error[0] = '\0';
        transfer_receive_delete(data->installPath);
    } else {
        // The CIA stays, to install by hand.
        entry->state = BATCH_ENTRY_FAILED;
        const char* reason = data->installReason != NULL ? _(data->installReason) : _("The install failed.");
        if(data->installResult != 0) {
            snprintf(entry->error, sizeof(entry->error), "%s (%08lX)", reason, data->installResult);
        } else {
            string_copy(entry->error, reason, sizeof(entry->error));
        }
    }

    transfer_log("receive batch: %s %s\n", data->installPath, data->installOk ? "installed" : "not installed");
    data->state = RECEIVE_IDLE;
    transfer_batch_check_all(data);
    return false;
}

static void transfer_receive_progress_update(ui_view* view, void* data, float* progress, char* text) {
    transfer_receive_data* receive = (transfer_receive_data*) data;

    Result res = 0;
    bool done = false;

    if(!receive->finalizing) {
        transfer_receiver_accept(receive->engine);

        done = transfer_receiver_progress(receive->engine, &receive->received, &res);
        xferturn_progress(receive->turn, receive->received);

        if(!done && (hidKeysDown() & KEY_B)) {
            transfer_receiver_cancel(receive->engine);
        }

        if(done && R_SUCCEEDED(res)) {
            // All here. A title package is checked and its TMD written first.
            receive->finalizing = true;
            if(receive->kind == TRANSFER_KIND_CIA_ORDERED) {
                receive->finalizer = threadCreate(transfer_ordered_finish_thread, receive, 0x8000, 0x28, -2, false);
                if(receive->finalizer == NULL) {
                    receive->finalResult = R_APP_THREAD_CREATE_FAILED;
                    receive->finalDone = true;
                }
            } else if(receive->sink != NULL) {
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
            // An average over the last seconds (see the sender's).
            u32 last = (u32) ((receive->received - receive->speedReceived) * 1000 / (now - receive->speedTime));
            receive->speed = receive->speed == 0 ? last : (u32) (receive->speed * 0.75f + last * 0.25f);
            receive->speedReceived = receive->received;
            receive->speedTime = now;
        }

        // As on the sender: where the file is, without the header sent again.
        u64 again = receive->kind != TRANSFER_KIND_FILE && receive->start > 0 ? receive->head : 0;
        u64 at = receive->start + (receive->received > again ? receive->received - again : 0);
        if(at > receive->size) {
            at = receive->size;
        }

        u64 total = receive->size;
        *progress = total != 0 ? (float) ((double) at / (double) total) : 0;
        snprintf(text, PROGRESS_TEXT_MAX, _("From %s\n%.2f %s / %.2f %s\n%.2f %s/s"), receive->sender,
                 ui_get_display_size(at), ui_get_display_size_units(at),
                 ui_get_display_size(total), ui_get_display_size_units(total),
                 ui_get_display_size(receive->speed), ui_get_display_size_units(receive->speed));
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

    if(ok && receive->kind != TRANSFER_KIND_CIA_ORDERED) {
        Result renameRes = receive->keepPath ? transfer_receive_place_original(receive)
                                             : receive->kind == TRANSFER_KIND_FOLDER ? transfer_receive_finish_folder(receive, name) : transfer_receive_finish_file(receive, name);
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

    // A batch's title: nothing to answer here, the summary comes at the end.
    if(receive->batchItem) {
        transfer_batch_received(receive, ok, res);
        return;
    }

    // A cancelled or failed transfer keeps its choices for a retry.
    if(ok || res == R_APP_CANCELLED) {
        receive->haveLast = false;
    }

    // A title to install lights it once installed (installcias.c).
    if(ok ? receive->isSave || !receive->install : res != R_APP_CANCELLED) {
        led_blink(ok);
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

// ---- a batch's title as a CIA in order: installed as it arrives ----

// Gives up the install that is under way (a failure or a cancel): the title stays as it was.
static void transfer_ordered_cancel(transfer_receive_data* data) {
    if(data->amHandle != 0) {
        AM_CancelCIAInstall(data->amHandle);
        data->amHandle = 0;
    }
}

// The bytes at `offset`, from any connection. Each connection brings its chunks in order but they are spread over
// the connections, so a chunk waits here for the ones before it (its connection stops reading meanwhile).
static Result transfer_ordered_write(transfer_receive_data* data, u64 offset, const void* buffer, u32 size) {
    u64 began = osGetTime();
    while(true) {
        LightLock_Lock(&data->fileLock);
        if(data->orderedFailed || data->orderedAbort) {
            LightLock_Unlock(&data->fileLock);
            return R_APP_CANCELLED;
        }

        if(offset + size <= data->orderedNext) {
            LightLock_Unlock(&data->fileLock);
            return 0;
        }

        if(offset == data->orderedNext) {
            if(offset == 0) {
                cia_seed_watch_start(&data->orderedSeed, (const u8*) buffer, size);
            }

            cia_seed_watch_feed(&data->orderedSeed, buffer, offset, size);

            u32 written = 0;
            Result res = FSFILE_Write(data->amHandle, &written, offset, buffer, size, 0);
            if(R_SUCCEEDED(res) && written != size) {
                res = R_APP_IO_ERROR;
            }

            if(R_FAILED(res)) {
                data->orderedFailed = true;
                transfer_log("receive: the install refused the bytes at %llu: %08lX\n", offset, res);
            } else {
                data->orderedNext += size;
            }

            LightLock_Unlock(&data->fileLock);
            return res;
        }

        LightLock_Unlock(&data->fileLock);

        // Well under the sender's TRANSFER_IO_TIMEOUT: a chunk before this one that does not come means a lost connection.
        if(osGetTime() - began > 12000) {
            transfer_log("receive: the chunk at %llu waited too long for %llu\n", offset, data->orderedNext);
            return R_APP_IO_ERROR;
        }

        svcSleepThread(2000000);
    }
}

// Every byte is in: AM checks and adds the title (a while for a big one), on the finalizer thread.
static void transfer_ordered_finish_thread(void* arg) {
    transfer_receive_data* data = (transfer_receive_data*) arg;
    transfer_batch_entry* entry = &data->batch.entries[data->batchIndex];

    Result res = data->orderedNext == data->size ? AM_FinishCiaInstall(data->amHandle) : R_APP_IO_ERROR;
    if(R_SUCCEEDED(res)) {
        data->amHandle = 0;
        if(cia_seed_watch_needed(&data->orderedSeed)) {
            http_download_seed(entry->titleId);
        }
    }

    data->finalResult = res;
    data->finalDone = true;
}

static void transfer_receive_start_ordered(transfer_receive_data* data) {
    transfer_batch_entry* entry = &data->batch.entries[data->batchIndex];
    Result res = 0;

    // One transfer at a time.
    if((data->turn = xferturn_take(XFER_CONSOLE, entry->name, data->size)) == 0) {
        res = R_APP_BUSY;
    }

    // As action_install_cia_quietly: never over a newer version; over an installed one as an update goes, a new one
    // the usual way.
    AM_TitleInfo installed;
    u64 titleId = entry->titleId;
    bool isInstalled = R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &titleId, &installed)) && installed.titleID == titleId;
    bool newer = isInstalled && entry->version < installed.version;
    if(R_SUCCEEDED(res) && newer) {
        res = R_APP_SKIPPED;
    }

    data->amHandle = 0;
    if(R_SUCCEEDED(res)) {
        res = isInstalled ? AM_StartCiaInstallOverwrite(&data->amHandle, MEDIATYPE_SD) : AM_StartCiaInstall(MEDIATYPE_SD, &data->amHandle);
    }

    u32 id = (u32) osGetTime() ^ (u32) svcGetSystemTick();
    if(R_SUCCEEDED(res)) {
        transfer_log("receive: %s installed as it arrives (%llu bytes)\n", entry->name, data->size);

        LightLock_Init(&data->fileLock);
        data->part[0] = '\0';
        data->position[0] = '\0';
        data->sink = NULL;
        data->folder = NULL;
        data->file_ = 0;
        data->start = 0;
        data->prefix = 0;
        data->pendingCount = 0;
        data->positionSaved = 0;
        data->orderedNext = 0;
        data->orderedFailed = false;
        data->orderedAbort = false;
        memset(&data->orderedSeed, 0, sizeof(data->orderedSeed));
        data->finalDone = false;
        data->finalizing = false;
        data->finalizer = NULL;
        data->received = 0;
        data->speed = 0;
        data->speedReceived = 0;
        data->speedTime = osGetTime();
        data->engineStart = data->speedTime;

        transfer_receive_answer(data, 1, 0, id);
        if(R_SUCCEEDED(res = transfer_receiver_start(&data->engine, &data->client, data->tcp, id, 0, data->size, 0, transfer_receive_write, data))) {
            info_display("Receiving", " cancel", true, data, transfer_receive_progress_update, transfer_receive_draw_top);
            data->state = RECEIVE_RECEIVING;
            return;
        }
    }

    transfer_ordered_cancel(data);
    xferturn_release(data->turn);
    data->turn = 0;
    transfer_receive_answer(data, 0, 0, 0);
    transfer_receive_reset(data);

    // The sender gives it up (refused); the summary says why.
    entry->state = BATCH_ENTRY_FAILED;
    if(newer) {
        string_copy(entry->error, _("A newer version is installed."), sizeof(entry->error));
    } else {
        snprintf(entry->error, sizeof(entry->error), "%s (%08lX)", _("The install could not start."), res);
    }

    data->batchItem = false;
}

static void transfer_receive_start(transfer_receive_data* data) {
    if(data->kind == TRANSFER_KIND_CIA_ORDERED) {
        transfer_receive_start_ordered(data);
        return;
    }

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

    // Built in NSA's own tmp folder, wherever it is going to end up.
    const char* partDir = TRANSFER_TMP_DIR;
    if(R_SUCCEEDED(res)) {
        res = fs_ensure_dir(sd, APP_DATA_DIR);
        if(R_SUCCEEDED(res)) {
            res = fs_ensure_dir(sd, TRANSFER_TMP_DIR);
        }
    }

    // One transfer at a time: the sender is refused (and told) while another holds the turn.
    if(R_SUCCEEDED(res) && (data->turn = xferturn_take(XFER_CONSOLE, name, data->size)) == 0) {
        res = R_APP_BUSY;
    }

    u32 id = (u32) osGetTime() ^ (u32) svcGetSystemTick();

    // A path cut short could make <name>.part.pos the .part itself, or the .part
    // a file already there.
    if(R_SUCCEEDED(res)
       && ((size_t) snprintf(data->part, sizeof(data->part), "%s%s.part", partDir, name) >= sizeof(data->part)
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
        data->lastKeepPath = data->keepPath;
        data->lastOverwrite = data->overwrite;
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

        if(data->kind == TRANSFER_KIND_FOLDER) {
            if(data->start == 0) {
                // Whatever an earlier try left there is no use; the files of this one replace it.
                FS_Path* old = fs_make_path_utf8(data->part);
                if(old != NULL) {
                    FSUSER_DeleteDirectoryRecursively(sd, *old);
                    fs_free_path_utf8(old);
                }
            }

            char root[FILE_PATH_MAX];
            snprintf(root, sizeof(root), "%s/", data->part);
            res = transfer_folder_recv_open(&data->folder, sd, root, data->head);
        } else if(data->kind == TRANSFER_KIND_TITLE) {
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

            if(R_SUCCEEDED(res = transfer_receiver_start(&data->engine, &data->client, data->tcp, id, data->start, data->size, data->kind != TRANSFER_KIND_FILE ? data->head : 0, transfer_receive_write, data))) {
                info_display("Receiving", "\uE001 cancel", true, data, transfer_receive_progress_update, transfer_receive_draw_top);
                data->state = RECEIVE_RECEIVING;
                return;
            }

            transfer_receive_close_transfer(data, false);
        }
    }

    transfer_receive_answer(data, 0, 0, 0);
    transfer_receive_reset(data);

    if(data->batchItem) {
        transfer_batch_received(data, false, res);
        return;
    }

    led_blink(false);
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
    widget_row(x, y, width, height, item->data == NULL ? ICON_CHECK : ICON_FOLDER, item->data == NULL ? _("Save here") : item->name, theme()->palette->text, NULL, selected);
}

static void transfer_folder_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    transfer_folder_data* folder = (transfer_folder_data*) data;

    transfer_draw_card(x1, y1, x2, y2, ICON_FOLDER, _("Choose where to save"), folder->path, NULL);
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

// Answers of the "Existing files" question (see transfer_receive_keep_path).
static void transfer_receive_overwrite_all(void* data) {
    transfer_receive_data* receive = (transfer_receive_data*) data;
    receive->overwrite = TRANSFER_OVERWRITE_ALL;
    transfer_receive_start(receive);
}

static void transfer_receive_overwrite_skip(void* data) {
    transfer_receive_data* receive = (transfer_receive_data*) data;
    receive->overwrite = TRANSFER_OVERWRITE_SKIP;
    transfer_receive_start(receive);
}

// Rejecting, or backing out of a question: the sender is told no.
static void transfer_receive_reject(void* data) {
    transfer_receive_data* receive = (transfer_receive_data*) data;
    transfer_receive_answer(receive, 0, 0, 0);
    transfer_receive_reset(receive);
}

// "Original path" was chosen: it goes ahead at once, or asks first when files would be overwritten.
static void transfer_receive_keep_path(void* data) {
    transfer_receive_data* receive = (transfer_receive_data*) data;
    receive->keepPath = true;
    receive->install = false;
    string_copy(receive->dir, TRANSFER_RECEIVED_DIR, sizeof(receive->dir));

    transfer_receive_count_existing(receive);
    if(receive->existing == 0) {
        receive->overwrite = TRANSFER_OVERWRITE_ALL;
        transfer_receive_start(receive);
        return;
    }

    bool folder = receive->kind == TRANSFER_KIND_FOLDER;
    if(folder) {
        snprintf(receive->message, sizeof(receive->message), _("%lu of %lu files already exist"),
                 (unsigned long) receive->existing, (unsigned long) receive->fileCount);
    } else {
        snprintf(receive->message, sizeof(receive->message), _("%s already exists"), receive->file);
    }

    sheet_action actions[3];
    u32 count = 0;
    actions[count++] = (sheet_action) {.icon = ICON_CHECK, .label = _("Overwrite"), .run = transfer_receive_overwrite_all,
                                       .detail = folder ? _("Replaces the files that already exist") : _("Replaces the existing file")};
    if(folder) {
        actions[count++] = (sheet_action) {.icon = ICON_PLAYER_TRACK_NEXT, .label = _("Skip existing"), .run = transfer_receive_overwrite_skip,
                                           .detail = _("Only receives the files that are new")};
    }

    actions[count++] = (sheet_action) {.icon = ICON_X, .label = _("Cancel"), .run = transfer_receive_reject,
                                       .detail = _("Nothing is received")};
    sheet_display_choice(receive->message, actions, count, receive, transfer_receive_draw_top, transfer_receive_reject);
    sheet_set_id("Existing files");
}

// Answers of the "Incoming file" question (see transfer_receive_ask_action).
static void transfer_receive_accept_install(void* data) {
    transfer_receive_data* receive = (transfer_receive_data*) data;
    receive->install = true;
    // A CIA that installs right away, or a save that is imported, goes to the default folder (and is deleted after).
    string_copy(receive->dir, TRANSFER_RECEIVED_DIR, sizeof(receive->dir));
    transfer_receive_start(receive);
}

static void transfer_receive_save(void* data) {
    transfer_receive_data* receive = (transfer_receive_data*) data;
    receive->install = false;
    if(receive->isSave) {
        // Whatever the choice, a save arrives in the default folder and is unpacked into a folder of saves.
        string_copy(receive->dir, TRANSFER_RECEIVED_DIR, sizeof(receive->dir));
        transfer_receive_start(receive);
    } else {
        transfer_receive_choose_folder(receive);
    }
}

static void transfer_receive_save_here(void* data) {
    ((transfer_receive_data*) data)->keepPath = false;
    transfer_receive_save(data);
}

static void transfer_receive_ask_action(transfer_receive_data* receive) {
    char size[32];
    transfer_size_text(size, sizeof(size), receive->size);
    snprintf(receive->message, sizeof(receive->message), "%s (%s)", receive->file, size);

    sheet_action actions[3];
    u32 count = 0;

    if(receive->offerOrigin) {
        actions[count++] = (sheet_action) {.icon = ICON_FOLDER, .label = _("Save"), .run = transfer_receive_save_here,
                                           .detail = _("Choose a folder on the SD card")};
        actions[count++] = (sheet_action) {.icon = ICON_FOLDER_UP, .label = _("Original path"), .run = transfer_receive_keep_path,
                                           .detail = receive->origin};
    } else if(receive->isSave) {
        // Importing needs the title (and for extra data, its archive) here: otherwise it is only kept.
        if(receive->canImport) {
            actions[count++] = (sheet_action) {.icon = ICON_DOWNLOAD, .label = _("Import now"), .run = transfer_receive_accept_install,
                                               .detail = _("Puts it into the title on this console")};
            actions[count++] = (sheet_action) {.icon = ICON_FOLDER, .label = _("Save in a folder"), .run = transfer_receive_save,
                                               .detail = _("Keeps it to import later")};
        } else {
            actions[count++] = (sheet_action) {.icon = ICON_FOLDER, .label = _("Save in a folder"), .run = transfer_receive_save,
                                               .detail = _("The title or its data is not here: it can only be kept")};
        }

        u64 id = 0;
        save_kind kind = SAVE_KIND_SAVE;
        char what[LIST_ITEM_NAME_MAX];
        action_save_package_name(receive->file, &id, &kind, what, sizeof(what));
        snprintf(receive->message, sizeof(receive->message), "%s: %s (%s)",
                 kind == SAVE_KIND_EXT ? _("Extra data") : kind == SAVE_KIND_BOSS ? _("SpotPass data") : _("Save data"), what, size);
    } else {
        if(receive->isCia) {
            actions[count++] = (sheet_action) {.icon = ICON_PACKAGE, .label = _("Install now"), .run = transfer_receive_accept_install,
                                               .detail = _("Installs it and deletes the CIA")};
        }

        actions[count++] = (sheet_action) {.icon = ICON_FOLDER, .label = _("Save"), .run = transfer_receive_save,
                                           .detail = _("Choose a folder on the SD card")};
    }

    actions[count++] = (sheet_action) {.icon = ICON_X, .label = _("Reject"), .run = transfer_receive_reject,
                                       .detail = _("Tells the other console no")};
    sheet_display_choice(receive->message, actions, count, receive, transfer_receive_draw_top, transfer_receive_reject);
    sheet_set_id("Incoming file");
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

static void transfer_receive_hello_ready(transfer_receive_data* data);

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
    data->helloBatch = ntohl(hello->batch);
    data->helloIndex = ntohl(hello->batchIndex);
    data->batchItem = false;

    // The end of a batch: its summary, when it is the one accepted here.
    if(data->kind == TRANSFER_KIND_BATCH_END) {
        bool ours = data->batch.active && data->helloBatch == data->batch.id && strcmp(data->sender, data->batch.sender) == 0;
        transfer_receive_reset(data);
        if(ours) {
            transfer_batch_summary(data);
        }

        return;
    }

    if(data->kind > TRANSFER_KIND_CIA_ORDERED || data->kind == TRANSFER_KIND_BATCH_END || (data->kind == TRANSFER_KIND_FOLDER && (data->head < 8 || data->head >= data->size))
       || (data->kind == TRANSFER_KIND_BATCH && (data->head < TRANSFER_BATCH_HEAD || data->head > TRANSFER_BATCH_HEAD + TRANSFER_BATCH_MAX * TRANSFER_BATCH_ENTRY))) {
        transfer_receive_reset(data);
        return;
    }

    const char* ext = strrchr(data->file, '.');
    data->isCia = data->kind == TRANSFER_KIND_TITLE || (data->kind == TRANSFER_KIND_FILE && ext != NULL && strcasecmp(ext, ".cia") == 0);

    u64 saveId = 0;
    save_kind saveKind = SAVE_KIND_SAVE;
    char saveName[LIST_ITEM_NAME_MAX];
    data->isSave = data->kind == TRANSFER_KIND_FILE && action_save_package_name(data->file, &saveId, &saveKind, saveName, sizeof(saveName));
    data->canImport = data->isSave && action_save_package_can_import(saveId, saveKind);

    if(data->size == 0) {
        transfer_receive_reset(data);
        return;
    }

    hello->origin[FILE_PATH_MAX - 1] = '\0';
    string_copy(data->origin, hello->origin, sizeof(data->origin));
    data->offerOrigin = (data->kind == TRANSFER_KIND_FILE || data->kind == TRANSFER_KIND_FOLDER) && !data->isCia && !data->isSave && transfer_origin_ok(data->origin);
    data->keepPath = false;
    data->overwrite = TRANSFER_OVERWRITE_ASK;
    data->existing = 0;
    data->fileCount = 0;

    if(data->kind == TRANSFER_KIND_FOLDER || data->kind == TRANSFER_KIND_BATCH) {
        // The manifest (a batch's list) follows the hello: read it before asking anything.
        free(data->preManifest);
        data->preManifest = data->kind == TRANSFER_KIND_BATCH || data->head <= FOLDER_MANIFEST_MAX_SIZE ? (u8*) malloc(data->head) : NULL;
        if(data->preManifest == NULL) {
            transfer_receive_reset(data);
            return;
        }

        data->preGot = 0;
        data->preStart = osGetTime();
        data->state = RECEIVE_MANIFEST;
        return;
    }

    transfer_receive_hello_ready(data);
}

// Everything the hello (and a folder's manifest) said is known: asks the user, or goes ahead.
static void transfer_receive_hello_ready(transfer_receive_data* data) {
    // A title of a batch: taken as the batch was (saved in the default folder, installed afterwards when so chosen).
    // One of a batch not accepted here (an old one, or another console's) is refused without a question.
    if(data->helloBatch != 0) {
        if(data->batch.active && data->helloBatch == data->batch.id && strcmp(data->sender, data->batch.sender) == 0
           && data->helloIndex < data->batch.count
           && ((data->kind == TRANSFER_KIND_TITLE && data->batch.entries[data->helloIndex].way == TRANSFER_WAY_PACKAGE)
               || (data->kind == TRANSFER_KIND_CIA_ORDERED && data->batch.entries[data->helloIndex].way == TRANSFER_WAY_ORDERED))) {
            data->batchItem = true;
            data->batchIndex = data->helloIndex;
            // A CIA in order is installed as it comes (the screen says so); a package is saved, installed afterwards.
            data->install = data->kind == TRANSFER_KIND_CIA_ORDERED;
            data->keepPath = false;
            data->overwrite = TRANSFER_OVERWRITE_ASK;
            string_copy(data->dir, TRANSFER_RECEIVED_DIR, sizeof(data->dir));
            transfer_log("receive batch: title %lu of %lu, %s\n", (unsigned long) data->helloIndex + 1, (unsigned long) data->batch.count, data->file);

            data->state = RECEIVE_ASKING;
            transfer_receive_start(data);
            return;
        }

        transfer_log("receive: a title of a batch not accepted here, refused\n");
        transfer_receive_answer(data, 0, 0, 0);
        transfer_receive_reset(data);
        return;
    }

    // A CIA in order is only ever a batch's title.
    if(data->kind == TRANSFER_KIND_CIA_ORDERED) {
        transfer_receive_answer(data, 0, 0, 0);
        transfer_receive_reset(data);
        return;
    }

    if(data->kind == TRANSFER_KIND_BATCH) {
        // Tests: saved, with no question.
        if(udslink_auto()) {
            transfer_batch_accept(data, false);
            return;
        }

        transfer_batch_ask(data);
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
        data->keepPath = data->lastKeepPath;
        data->overwrite = data->lastOverwrite;
        string_copy(data->dir, data->lastDir, sizeof(data->dir));
        data->state = RECEIVE_ASKING;
        transfer_receive_start(data);
        return;
    }

    transfer_log("receive: %s wants to send %s (%llu bytes, %s)\n", data->sender, data->file, data->size, data->kind == TRANSFER_KIND_FOLDER ? "folder" : data->kind == TRANSFER_KIND_TITLE ? "title" : "file");

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

    free(data->preManifest);
    if(data->haveLayout) {
        folder_layout_free(&data->layout);
    }

    free(data->offer);
    free(data);
}

static bool transfer_receive_start_wifi(transfer_receive_data* data);
static bool transfer_receive_start_nearby(transfer_receive_data* data);
static void transfer_receive_set_button(ui_view* view, transfer_receive_data* data);

static void transfer_receive_update(ui_view* view, void* data, float* progress, char* text) {
    transfer_receive_data* receive = (transfer_receive_data*) data;

    if(receive->state == RECEIVE_IDLE || receive->state == RECEIVE_HELLO || receive->state == RECEIVE_MANIFEST) {
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

    // A batch's title being installed: the next one waits (its connection is taken once this is done).
    if(receive->state == RECEIVE_INSTALLING && transfer_batch_installing(receive, text)) {
        return;
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

    if(receive->state == RECEIVE_MANIFEST) {
        int n = transfer_link_recv_some(&receive->client, receive->preManifest + receive->preGot, receive->head - receive->preGot);
        if(n > 0) {
            receive->preGot += (u32) n;
            receive->preStart = osGetTime();
        }

        if(n == 0 || n == -2 || osGetTime() - receive->preStart > 3 * TRANSFER_HELLO_TIMEOUT) {
            transfer_receive_reset(receive);
        } else if(receive->preGot == receive->head && receive->kind == TRANSFER_KIND_BATCH) {
            bool ok = transfer_batch_parse(receive);
            free(receive->preManifest);
            receive->preManifest = NULL;

            if(ok) {
                transfer_receive_hello_ready(receive);
            } else {
                transfer_receive_reset(receive);
            }
        } else if(receive->preGot == receive->head) {
            folder_layout layout;
            bool ok = folder_manifest_parse(receive->preManifest, receive->head, &layout);
            free(receive->preManifest);
            receive->preManifest = NULL;

            if(ok && layout.total == receive->size) {
                receive->layout = layout;
                receive->haveLayout = true;
                transfer_receive_hello_ready(receive);
            } else {
                if(ok) {
                    folder_layout_free(&layout);
                }

                transfer_receive_reset(receive);
            }
        }
    }

    if(receive->batch.active) {
        u32 done = 0;
        for(u32 i = 0; i < receive->batch.count; i++) {
            done += receive->batch.entries[i].state == BATCH_ENTRY_SAVED || receive->batch.entries[i].state == BATCH_ENTRY_INSTALLED ? 1 : 0;
        }

        snprintf(text, PROGRESS_TEXT_MAX, _("Titles from %s: %lu of %lu here.\nWaiting for the next one..."), receive->batch.sender,
                 (unsigned long) done, (unsigned long) receive->batch.count);
        return;
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

    transfer_draw_card(x1, y1, x2, y2, ICON_3DS, receive->localName, ip, NULL);
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
