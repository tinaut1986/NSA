#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <3ds.h>
#include <jansson.h>

#include "ftpserver.h"
#include "settings.h"
#include "section.h"
#include "relaunch.h"
#include "transfer.h"
#include "webserver.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/bgmusic.h"
#include "../core/core.h"
#include "../core/speedlog.h"
#include "../core/xferturn.h"
#include "../core/input.h"
#include "../core/ui/ui.h"
#include "../core/data/smdh.h"

#define WEB_PATH_MAX 512
#define WEB_HEAD_MAX 8192
#define WEB_IO_SIZE (64 * 1024)
#define WEB_FILE_BUFFER (256 * 1024)
#define WEB_NAP_MS 10
#define WEB_SEND_CHUNK (64 * 1024)
// SO_RCVBUF / SO_SNDBUF of the listening socket (0: the system's); see webserver_start.
#define WEB_SOCKET_BUFFER (32 * 1024)
#define WEB_IDLE_MS 30000
// A file transfer stalled this long gives up when another request is waiting: the page is served one
// request at a time, and the 3DS does not always see a client's reset.
#define WEB_STALL_MS 10000
#define WEB_PIN_DIGITS 6
#define WEB_FAILS_MAX 10
#define WEB_LOCK_MS 60000

static Thread web_thread = NULL;
static volatile bool web_run = false;
static bool web_paused = false;
static bool web_retry = false;
static u64 web_retry_next = 0;
static u64 web_retry_end = 0;
static int web_listen = -1;
static volatile int web_client = -1;
static u32 web_ip = 0;
static char web_status[48] = "Off";
static char web_pin[WEB_PIN_DIGITS + 1] = "";
static u32 web_fails = 0;
static u64 web_locked_until = 0;
static char* web_io = NULL;
// The transfer turn (xferturn.h) held by the file going up or down, and its socket; 0 / -1 when none.
static u32 web_turn = 0;
static int web_turn_fd = -1;

typedef struct {
    int fd;
    char head[WEB_HEAD_MAX + 1];
    u32 headLen;            // Bytes of the request's head.
    u32 have;               // Bytes read in total (the head and the start of the body).
    char method[8];
    char path[WEB_PATH_MAX];
    char query[1024];
    u64 contentLength;
    u64 rangeFrom;
    bool hasRange;
    char pin[16];           // From the cookie or the X-Pin header.
    bool pinHeader;         // ...it came in the header.
} web_request;

// ---- sockets (as the FTP server's: libctru has one SOC session, so never block in poll) ----

static void web_close_socket(int* fd) {
    if(*fd >= 0) {
        shutdown(*fd, SHUT_RDWR);
        close(*fd);
        *fd = -1;
    }
}

// The socket of a file transfer that holds the turn waits inside poll(), a few ms at a time, as the
// FTP server's does (ftp_wait_fd).
static bool web_wait_fd(int fd, short events, int timeoutMs) {
    bool owner = web_turn != 0 && fd == web_turn_fd;
    int waited = 0;
    while(web_run) {
        struct pollfd p = {.fd = fd, .events = events, .revents = 0};
        bool polls = owner && net_owner_polls((u32) waited);
        u64 start = polls ? osGetTime() : 0;
        int r = polls ? net_poll_owner(&p, 1, NET_OWNER_POLL_MS) : poll(&p, 1, 0);
        if(r > 0) {
            return (p.revents & (events | POLLHUP | POLLERR)) != 0;
        }

        if(r < 0) {
            return false;
        }

        // Short naps while data flows, longer once idle (see ftp_wait_fd).
        if(polls) {
            u64 took = osGetTime() - start;
            waited += took > 0 ? (int) took : 1;
        } else {
            int nap = (int) net_nap_ms((u32) waited);
            svcSleepThread((s64) nap * 1000000);
            waited += nap;
        }
        if(timeoutMs > 0 && waited >= timeoutMs) {
            return false;
        }
    }

    return false;
}

// A file transfer's wait, up to WEB_IDLE_MS; after WEB_STALL_MS with nothing, it gives up if another
// request is waiting (see WEB_STALL_MS).
static bool web_wait_transfer(int fd, short events) {
    for(int waited = 0; waited < WEB_IDLE_MS; waited += WEB_STALL_MS) {
        if(web_wait_fd(fd, events, WEB_STALL_MS)) {
            return true;
        }

        struct pollfd p = {.fd = web_listen, .events = POLLIN, .revents = 0};
        if(!web_run || (poll(&p, 1, 0) > 0 && (p.revents & POLLIN) != 0)) {
            return false;
        }
    }

    return false;
}

// `log` (may be NULL) gets the time spent waiting for room and inside send().
static bool web_send_logged(int fd, const char* data, size_t n, speedlog* log) {
    while(n > 0) {
        u64 start = speedlog_ticks();
        bool ready = web_wait_transfer(fd, POLLOUT);
        speedlog_wait(log, speedlog_ticks() - start);
        if(!ready) {
            return false;
        }

        // The sockets do not block (net_set_nonblocking): what does not fit now waits for room.
        start = speedlog_ticks();
        ssize_t w = send(fd, data, n > WEB_SEND_CHUNK ? WEB_SEND_CHUNK : n, 0);
        speedlog_net(log, w > 0 ? (u64) w : 0, speedlog_ticks() - start);
        if(w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }

        if(w <= 0) {
            return false;
        }

        data += w;
        n -= (size_t) w;
    }

    return true;
}

static bool web_send_all(int fd, const char* data, size_t n) {
    return web_send_logged(fd, data, n, NULL);
}

// ---- responses ----

static const char* web_status_text(int status) {
    switch(status) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        default: return "Error";
    }
}

// The head of a response. `length` < 0 sends none (the connection closing ends the body).
static bool web_send_head(web_request* r, int status, const char* type, s64 length, const char* extra) {
    char head[512];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\n", status, web_status_text(status), type);
    if(extra == NULL || strstr(extra, "Cache-Control") == NULL) {
        n += snprintf(head + n, sizeof(head) - n, "Cache-Control: no-store\r\n");
    }

    if(length >= 0) {
        n += snprintf(head + n, sizeof(head) - n, "Content-Length: %lld\r\n", (long long) length);
    }

    if(extra != NULL) {
        n += snprintf(head + n, sizeof(head) - n, "%s", extra);
    }

    n += snprintf(head + n, sizeof(head) - n, "\r\n");
    return web_send_all(r->fd, head, (size_t) n);
}

static void web_send_text(web_request* r, int status, const char* type, const char* body) {
    size_t len = strlen(body);
    if(web_send_head(r, status, type, (s64) len, NULL)) {
        web_send_all(r->fd, body, len);
    }
}

static void web_send_json(web_request* r, int status, json_t* json) {
    char* text = json_dumps(json, JSON_COMPACT);
    json_decref(json);
    if(text == NULL) {
        web_send_text(r, 500, "text/plain", "out of memory");
        return;
    }

    web_send_text(r, status, "application/json", text);
    free(text);
}

static void web_send_error(web_request* r, int status, const char* message) {
    web_send_json(r, status, json_pack("{s:b,s:s}", "ok", 0, "error", message));
}

static void web_send_ok(web_request* r) {
    web_send_json(r, 200, json_pack("{s:b}", "ok", 1));
}

// ---- one transfer at a time ----

// What moves data now, for the page: null when nothing does.
static json_t* web_transfer_json() {
    xferturn_status status;
    xferturn_get(&status);
    if(!status.busy) {
        return json_null();
    }

    return json_pack("{s:s,s:s,s:I,s:I,s:i,s:i}", "kind", xferturn_kind_name(status.kind), "name", status.name, "done", (json_int_t) status.done,
                     "total", (json_int_t) status.total, "kbs", (int) status.kbs, "seconds", (int) status.seconds);
}

// 503 with what holds the turn: the page says so and lets the user try again when it ends.
static void web_send_busy(web_request* r) {
    char text[128];
    if(!xferturn_describe(text, sizeof(text))) {
        string_copy(text, "Busy", sizeof(text));
    }

    web_send_json(r, 503, json_pack("{s:b,s:s,s:o}", "ok", 0, "error", text, "busy", web_transfer_json()));
}

// Files from this size up take the turn to go down; smaller ones (a text, a picture) do not.
#define WEB_TURN_MIN_GET (1024 * 1024)

static bool web_take_turn(web_request* r, const char* vpath, u64 total, bool incoming) {
    web_turn = xferturn_take(XFER_WEB, vpath, total);
    if(web_turn == 0) {
        web_send_busy(r);
        return false;
    }

    xferturn_incoming(web_turn, incoming);

    web_turn_fd = r->fd;
    return true;
}

static void web_release_turn() {
    xferturn_release(web_turn);
    web_turn = 0;
    web_turn_fd = -1;
}

// ---- request ----

static int web_hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// The value of `key` in the query, decoded; false when it is not there.
static bool web_query_get(const char* query, const char* key, char* out, size_t cap) {
    size_t keyLen = strlen(key);
    for(const char* p = query; *p != '\0'; ) {
        const char* end = strchr(p, '&');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);
        if(len > keyLen && strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
            size_t n = 0;
            for(const char* c = p + keyLen + 1; c < p + len && n + 1 < cap; c++) {
                if(*c == '%' && c + 2 < p + len && web_hex(c[1]) >= 0 && web_hex(c[2]) >= 0) {
                    out[n++] = (char) (web_hex(c[1]) * 16 + web_hex(c[2]));
                    c += 2;
                } else if(*c == '+') {
                    out[n++] = ' ';
                } else {
                    out[n++] = *c;
                }
            }

            out[n] = '\0';
            return true;
        }

        p = end != NULL ? end + 1 : p + len;
    }

    return false;
}

// Reads the request's head. False when the connection should just be dropped.
static bool web_read_head(web_request* r) {
    r->have = 0;
    for(;;) {
        if(r->have >= WEB_HEAD_MAX || !web_wait_fd(r->fd, POLLIN, WEB_IDLE_MS)) {
            return false;
        }

        ssize_t n = net_recv(r->fd, r->head + r->have, WEB_HEAD_MAX - r->have, WEB_IDLE_MS);
        if(n <= 0) {
            return false;
        }

        r->have += (u32) n;
        r->head[r->have] = '\0';
        char* end = strstr(r->head, "\r\n\r\n");
        if(end != NULL) {
            r->headLen = (u32) (end - r->head) + 4;
            *end = '\0';
            return true;
        }
    }
}

static void web_parse_head(web_request* r) {
    char* line = r->head;
    char* next = strstr(line, "\r\n");
    if(next != NULL) {
        *next = '\0';
        next += 2;
    }

    // METHOD target HTTP/1.1
    char target[WEB_PATH_MAX + 1024];
    target[0] = '\0';
    sscanf(line, "%7s %1535s", r->method, target);
    char* q = strchr(target, '?');
    if(q != NULL) {
        *q = '\0';
        string_copy(r->query, q + 1, sizeof(r->query));
    } else {
        r->query[0] = '\0';
    }

    string_copy(r->path, target, sizeof(r->path));

    r->contentLength = 0;
    r->hasRange = false;
    r->rangeFrom = 0;
    r->pin[0] = '\0';
    r->pinHeader = false;

    while(next != NULL && *next != '\0') {
        char* eol = strstr(next, "\r\n");
        if(eol != NULL) {
            *eol = '\0';
        }

        if(strncasecmp(next, "Content-Length:", 15) == 0) {
            r->contentLength = strtoull(next + 15, NULL, 10);
        } else if(strncasecmp(next, "Range:", 6) == 0) {
            const char* eq = strstr(next, "bytes=");
            if(eq != NULL) {
                r->hasRange = true;
                r->rangeFrom = strtoull(eq + 6, NULL, 10);
            }
        } else if(strncasecmp(next, "X-Pin:", 6) == 0) {
            const char* v = next + 6;
            while(*v == ' ') {
                v++;
            }

            string_copy(r->pin, v, sizeof(r->pin));
            r->pinHeader = true;
        } else if(strncasecmp(next, "Cookie:", 7) == 0 && !r->pinHeader) {
            const char* c = strstr(next + 7, "pin=");
            if(c != NULL) {
                c += 4;
                size_t n = strcspn(c, "; ");
                if(n < sizeof(r->pin)) {
                    memcpy(r->pin, c, n);
                    r->pin[n] = '\0';
                }
            }
        }

        next = eol != NULL ? eol + 2 : NULL;
    }
}

// ---- the PIN ----

static bool web_check_pin(web_request* r) {
    u64 now = osGetTime();
    if(web_locked_until > now) {
        return false;
    }

    // Anything that changes something needs the header: a page of another site cannot send it.
    bool reads = strcmp(r->method, "GET") == 0;
    if(r->pin[0] != '\0' && strcmp(r->pin, web_pin) == 0 && (reads || r->pinHeader)) {
        web_fails = 0;
        return true;
    }

    if(r->pin[0] != '\0' && ++web_fails >= WEB_FAILS_MAX) {
        web_fails = 0;
        web_locked_until = now + WEB_LOCK_MS;
    }

    return false;
}

// ---- paths ----

// "/dir/file" as "sdmc:/dir/file"; false for anything that is not a plain path inside the SD card.
static bool web_real_path(const char* vpath, char* out, size_t cap) {
    if(vpath[0] != '/' || strlen(vpath) + 6 >= cap) {
        return false;
    }

    for(const char* c = vpath; *c != '\0'; c++) {
        if((unsigned char) *c < 0x20 || *c == '\\' || *c == ':') {
            return false;
        }
    }

    for(const char* part = vpath + 1; part != NULL && *part != '\0'; ) {
        const char* slash = strchr(part, '/');
        size_t len = slash != NULL ? (size_t) (slash - part) : strlen(part);
        if((len == 2 && part[0] == '.' && part[1] == '.') || (len == 1 && part[0] == '.')) {
            return false;
        }

        part = slash != NULL ? slash + 1 : NULL;
    }

    snprintf(out, cap, "sdmc:%s", vpath);
    return true;
}

static bool web_path_param(web_request* r, const char* key, char* vpath, size_t vcap, char* rpath, size_t rcap) {
    if(!web_query_get(r->query, key, vpath, vcap) || !web_real_path(vpath, rpath, rcap)) {
        web_send_error(r, 400, "bad path");
        return false;
    }

    return true;
}

// ---- API ----

static void web_api_info(web_request* r) {
    char name[64] = "";
    transfer_local_name(name, sizeof(name));

    FS_ArchiveResource resource = {0};
    u64 total = 0;
    u64 free_ = 0;
    if(R_SUCCEEDED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD))) {
        total = (u64) resource.totalClusters * resource.clusterSize;
        free_ = (u64) resource.freeClusters * resource.clusterSize;
    }

    u8 battery = 0;
    u8 charging = 0;
    PTMU_GetBatteryLevel(&battery);
    PTMU_GetBatteryChargeState(&charging);

    web_send_json(r, 200, json_pack("{s:b,s:s,s:s,s:I,s:I,s:i,s:b,s:o}", "ok", 1, "console", name, "version", VERSION_STRING,
                                    "free", (json_int_t) free_, "total", (json_int_t) total, "battery", (int) battery, "charging", charging != 0,
                                    "transfer", web_transfer_json()));
}

static void web_api_list(web_request* r) {
    char vpath[WEB_PATH_MAX];
    char rpath[WEB_PATH_MAX + 8];
    if(!web_path_param(r, "path", vpath, sizeof(vpath), rpath, sizeof(rpath))) {
        return;
    }

    DIR* dir = opendir(rpath);
    if(dir == NULL) {
        web_send_error(r, 404, "no such folder");
        return;
    }

    json_t* list = json_array();
    struct dirent* entry;
    while((entry = readdir(dir)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full[WEB_PATH_MAX * 2];
        snprintf(full, sizeof(full), "%s%s%s", rpath, rpath[strlen(rpath) - 1] == '/' ? "" : "/", entry->d_name);

        struct stat st;
        if(stat(full, &st) != 0) {
            continue;
        }

        u64 mtime = 0;
        archive_getmtime(full, &mtime);   // libctru's stat gives 0.
        json_array_append_new(list, json_pack("{s:s,s:b,s:I,s:I}", "name", entry->d_name, "dir", S_ISDIR(st.st_mode), "size", (json_int_t) st.st_size, "mtime", (json_int_t) mtime));
    }

    closedir(dir);
    web_send_json(r, 200, json_pack("{s:b,s:o}", "ok", 1, "files", list));
}

static void web_api_get_file(web_request* r) {
    char vpath[WEB_PATH_MAX];
    char rpath[WEB_PATH_MAX + 8];
    if(!web_path_param(r, "path", vpath, sizeof(vpath), rpath, sizeof(rpath))) {
        return;
    }

    struct stat st;
    FILE* f = stat(rpath, &st) == 0 && !S_ISDIR(st.st_mode) ? fopen(rpath, "rb") : NULL;
    if(f == NULL) {
        web_send_error(r, 404, "no such file");
        return;
    }

    u64 size = (u64) st.st_size;
    u64 from = r->hasRange && r->rangeFrom < size ? r->rangeFrom : 0;
    bool partial = r->hasRange && from > 0;

    if(size - from >= WEB_TURN_MIN_GET && !web_take_turn(r, vpath, size - from, false)) {
        fclose(f);
        return;
    }

    const char* name = strrchr(vpath, '/');
    name = name != NULL ? name + 1 : vpath;
    char extra[400];
    int n = snprintf(extra, sizeof(extra), "Accept-Ranges: bytes\r\nContent-Disposition: attachment; filename=\"%.200s\"\r\n", name);
    if(partial) {
        snprintf(extra + n, sizeof(extra) - n, "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long) from, (unsigned long long) (size - 1), (unsigned long long) size);
    }

    if(!web_send_head(r, partial ? 206 : 200, "application/octet-stream", (s64) (size - from), extra)) {
        fclose(f);
        web_release_turn();
        return;
    }

    fseek(f, (long) from, SEEK_SET);
    speedlog* log = speedlog_begin("web-down", vpath, size - from);
    u64 sent = 0;
    bool ok = true;
    while(ok) {
        u64 start = speedlog_ticks();
        size_t got = fread(web_io, 1, WEB_IO_SIZE, f);
        speedlog_disk(log, got, speedlog_ticks() - start);
        if(got == 0) {
            break;
        }

        ok = web_send_logged(r->fd, web_io, got, log);
        sent += got;
        xferturn_progress(web_turn, sent);
    }

    fclose(f);
    web_release_turn();
    speedlog_end(log, ok);
}

static void web_api_put_file(web_request* r) {
    char vpath[WEB_PATH_MAX];
    char rpath[WEB_PATH_MAX + 8];
    if(!web_path_param(r, "path", vpath, sizeof(vpath), rpath, sizeof(rpath))) {
        return;
    }

    if(!web_take_turn(r, vpath, r->contentLength, true)) {
        return;
    }

    char part[WEB_PATH_MAX + 16];
    snprintf(part, sizeof(part), "%s.part", rpath);
    FILE* f = fopen(part, "wb");
    if(f == NULL) {
        web_release_turn();
        web_send_error(r, 500, "cannot create the file");
        return;
    }

    // Big writes to the SD card, not one per piece the network hands over (see the FTP server's STOR).
    setvbuf(f, NULL, _IOFBF, WEB_FILE_BUFFER);

    // Its whole size first: the card writes faster so (issue #26), and a card without room says so at once.
    if(r->contentLength > 0 && ftruncate(fileno(f), (off_t) r->contentLength) != 0) {
        fclose(f);
        remove(part);
        web_release_turn();
        web_send_error(r, 507, "not enough free space on the SD card");
        return;
    }

    // What came with the head, then the rest.
    u64 left = r->contentLength;
    u32 early = r->have - r->headLen;
    if(early > left) {
        early = (u32) left;
    }

    speedlog* log = speedlog_begin("web-up", vpath, r->contentLength);
    bool ok = early == 0 || fwrite(r->head + r->headLen, 1, early, f) == early;
    bool gone = false;   // The client went away: nobody to answer.
    left -= early;
    while(ok && left > 0) {
        u64 start = speedlog_ticks();
        bool ready = web_wait_transfer(r->fd, POLLIN);
        speedlog_wait(log, speedlog_ticks() - start);
        if(!ready) {
            ok = false;
            gone = true;
            break;
        }

        start = speedlog_ticks();
        // The socket said it is readable: nothing within a second means a client that reset (the 3DS flags
        // the socket but recv() keeps saying "not yet").
        ssize_t n = net_recv(r->fd, web_io, left > WEB_IO_SIZE ? WEB_IO_SIZE : (size_t) left, 1000);
        speedlog_net(log, n > 0 ? (u64) n : 0, speedlog_ticks() - start);
        if(n <= 0) {
            ok = false;
            gone = true;
            break;
        }

        start = speedlog_ticks();
        bool written = fwrite(web_io, 1, (size_t) n, f) == (size_t) n;
        speedlog_disk(log, (u64) n, speedlog_ticks() - start);
        if(!written) {
            ok = false;
            break;
        }

        left -= (u64) n;
        xferturn_progress(web_turn, r->contentLength - left);
    }

    web_release_turn();
    u64 closing = speedlog_ticks();
    if(fclose(f) != 0) {
        ok = false;
    }

    speedlog_disk(log, 0, speedlog_ticks() - closing);
    speedlog_end(log, ok);

    if(ok) {
        remove(rpath);
        ok = rename(part, rpath) == 0;
    }

    if(!ok) {
        remove(part);
        if(!gone) {
            web_send_error(r, 500, "the upload failed");
        }

        return;
    }

    web_send_ok(r);
}

static void web_api_delete_file(web_request* r) {
    char vpath[WEB_PATH_MAX];
    char rpath[WEB_PATH_MAX + 8];
    if(!web_path_param(r, "path", vpath, sizeof(vpath), rpath, sizeof(rpath))) {
        return;
    }

    if(strcmp(vpath, "/") == 0) {
        web_send_error(r, 403, "not the root of the SD card");
        return;
    }

    struct stat st;
    if(stat(rpath, &st) != 0) {
        web_send_error(r, 404, "no such file");
        return;
    }

    Result res = 0;
    FS_Archive sd = 0;
    FS_Path* fsPath = fs_make_path_utf8(vpath);
    if(fsPath == NULL) {
        web_send_error(r, 500, "out of memory");
        return;
    }

    if(R_SUCCEEDED(res = fs_open_archive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        res = S_ISDIR(st.st_mode) ? fs_delete_dir_recursively(sd, vpath) : FSUSER_DeleteFile(sd, *fsPath);
        fs_close_archive(sd);
    }

    fs_free_path_utf8(fsPath);
    if(R_FAILED(res)) {
        char text[48];
        snprintf(text, sizeof(text), "delete failed (%08lX)", res);
        web_send_error(r, 500, text);
        return;
    }

    web_send_ok(r);
}

static void web_api_mkdir(web_request* r) {
    char vpath[WEB_PATH_MAX];
    char rpath[WEB_PATH_MAX + 8];
    if(!web_path_param(r, "path", vpath, sizeof(vpath), rpath, sizeof(rpath))) {
        return;
    }

    if(mkdir(rpath, 0777) != 0 && errno != EEXIST) {
        web_send_error(r, 500, "cannot create the folder");
        return;
    }

    web_send_ok(r);
}

static void web_api_rename(web_request* r) {
    char vpath[WEB_PATH_MAX];
    char rpath[WEB_PATH_MAX + 8];
    char vto[WEB_PATH_MAX];
    char rto[WEB_PATH_MAX + 8];
    if(!web_path_param(r, "path", vpath, sizeof(vpath), rpath, sizeof(rpath)) || !web_path_param(r, "to", vto, sizeof(vto), rto, sizeof(rto))) {
        return;
    }

    struct stat st;
    if(stat(rto, &st) == 0) {
        web_send_error(r, 409, "that name is taken");
        return;
    }

    if(rename(rpath, rto) != 0) {
        web_send_error(r, 500, "cannot rename");
        return;
    }

    web_send_ok(r);
}

// Copies a file or a folder tree (stdio, on the web thread). `depth` stops a runaway tree.
static bool web_copy_tree(const char* from, const char* to, int depth) {
    struct stat st;
    if(depth > 24 || stat(from, &st) != 0) {
        return false;
    }

    if(S_ISDIR(st.st_mode)) {
        if(mkdir(to, 0777) != 0 && errno != EEXIST) {
            return false;
        }

        DIR* dir = opendir(from);
        if(dir == NULL) {
            return false;
        }

        bool ok = true;
        struct dirent* ent;
        while(ok && (ent = readdir(dir)) != NULL) {
            if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }

            char a[WEB_PATH_MAX + 8];
            char b[WEB_PATH_MAX + 8];
            if(snprintf(a, sizeof(a), "%s/%s", from, ent->d_name) >= (int) sizeof(a) || snprintf(b, sizeof(b), "%s/%s", to, ent->d_name) >= (int) sizeof(b)) {
                ok = false;
            } else {
                ok = web_copy_tree(a, b, depth + 1);
            }
        }

        closedir(dir);
        return ok;
    }

    FILE* in = fopen(from, "rb");
    if(in == NULL) {
        return false;
    }

    FILE* out = fopen(to, "wb");
    if(out == NULL) {
        fclose(in);
        return false;
    }

    u8* buf = (u8*) malloc(64 * 1024);
    bool ok = buf != NULL;
    size_t n;
    while(ok && (n = fread(buf, 1, 64 * 1024, in)) > 0) {
        ok = fwrite(buf, 1, n, out) == n;
    }

    free(buf);
    fclose(in);
    ok = (fclose(out) == 0) && ok;
    if(!ok) {
        remove(to);
    }

    return ok;
}

static void web_api_copy(web_request* r) {
    char vpath[WEB_PATH_MAX];
    char rpath[WEB_PATH_MAX + 8];
    char vto[WEB_PATH_MAX];
    char rto[WEB_PATH_MAX + 8];
    if(!web_path_param(r, "path", vpath, sizeof(vpath), rpath, sizeof(rpath)) || !web_path_param(r, "to", vto, sizeof(vto), rto, sizeof(rto))) {
        return;
    }

    struct stat st;
    if(stat(rpath, &st) != 0) {
        web_send_error(r, 404, "no such file");
        return;
    }

    if(stat(rto, &st) == 0) {
        web_send_error(r, 409, "that name is taken");
        return;
    }

    // A folder cannot be copied into itself.
    size_t len = strlen(vpath);
    if(strncmp(vto, vpath, len) == 0 && (vto[len] == '/' || vto[len] == '\0')) {
        web_send_error(r, 400, "cannot copy a folder into itself");
        return;
    }

    if(!web_copy_tree(rpath, rto, 0)) {
        web_send_error(r, 500, "the copy failed");
        return;
    }

    web_send_ok(r);
}

static void web_api_install(web_request* r) {
    char vpath[WEB_PATH_MAX];
    char rpath[WEB_PATH_MAX + 8];
    if(!web_path_param(r, "path", vpath, sizeof(vpath), rpath, sizeof(rpath))) {
        return;
    }

    if(relaunch_pending()) {
        web_send_error(r, 409, "NSA is restarting into the build just installed");
        return;
    }

    char error[128] = "";
    if(!ftpserver_install(vpath, error, sizeof(error))) {
        web_send_error(r, 500, error);
        return;
    }

    web_send_json(r, 200, json_pack("{s:b,s:b}", "ok", 1, "restarting", relaunch_pending()));
}

// ---- the page ----

// `cache`: the browser may keep it for an hour (icons and the font only change with a new build).
static void web_send_romfs(web_request* r, const char* path, const char* type, bool cache) {
    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        web_send_text(r, 404, "text/plain", "missing from this build");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(size < 0 || !web_send_head(r, 200, type, size, cache ? "Cache-Control: max-age=3600\r\n" : NULL)) {
        fclose(f);
        return;
    }

    size_t got;
    bool ok = true;
    while(ok && (got = fread(web_io, 1, WEB_IO_SIZE, f)) > 0) {
        ok = web_send_all(r->fd, web_io, got);
    }

    fclose(f);
}

// ---- the console's theme, for the page to look the same ----

static void web_color(char* out, u32 color) {
    snprintf(out, 8, "#%02x%02x%02x", (unsigned) (color & 0xFF), (unsigned) ((color >> 8) & 0xFF), (unsigned) ((color >> 16) & 0xFF));
}

// Public: only colours and shapes, and the login page wears them too.
static void web_send_theme(web_request* r) {
    const theme_current* t = theme();
    char background[8], surface[8], border[8], text[8], muted[8], success[8], danger[8], warning[8], accent[8], onAccent[8], soft[8];
    web_color(background, t->palette->background);
    web_color(surface, t->palette->surface);
    web_color(border, t->palette->border);
    web_color(text, t->palette->text);
    web_color(muted, t->palette->textMuted);
    web_color(success, t->palette->success);
    web_color(danger, t->palette->danger);
    web_color(warning, t->palette->warning);
    web_color(accent, t->accent);
    web_color(onAccent, t->onAccent);
    web_color(soft, t->accentSoft);

    web_send_json(r, 200, json_pack("{s:s,s:b,s:b,s:f,s:f,s:f,s:{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s}}",
                                    "theme", t->def->id, "night", t->night, "pixel", t->def->pixelFont,
                                    "radius", (double) t->def->radius, "radiusSmall", (double) t->def->radiusSmall, "border", (double) t->def->border,
                                    "colors", "background", background, "surface", surface, "border", border, "text", text, "muted", muted,
                                    "success", success, "danger", danger, "warning", warning, "accent", accent, "onAccent", onAccent, "accentSoft", soft));
}

// ---- titles ----

static bool web_parse_title(web_request* r, title_info* title) {
    char id[32] = "";
    char media[8] = "";
    if(!web_query_get(r->query, "id", id, sizeof(id)) || !web_query_get(r->query, "media", media, sizeof(media)) || strlen(id) != 16) {
        web_send_error(r, 400, "bad title");
        return false;
    }

    memset(title, 0, sizeof(*title));
    title->titleId = strtoull(id, NULL, 16);
    title->mediaType = strcmp(media, "nand") == 0 ? MEDIATYPE_NAND : strcmp(media, "card") == 0 ? MEDIATYPE_GAME_CARD : MEDIATYPE_SD;

    AM_TitleEntry entry;
    if(R_FAILED(AM_GetTitleInfo(title->mediaType, 1, &title->titleId, &entry))) {
        web_send_error(r, 404, "no such title");
        return false;
    }

    title->version = entry.version;
    title->installedSize = entry.size;
    return true;
}

static const char* web_kind_name(u64 id) {
    switch(title_kind_of(id)) {
        case TITLE_KIND_GAME: return "game";
        case TITLE_KIND_UPDATE: return "update";
        case TITLE_KIND_DLC: return "dlc";
        case TITLE_KIND_DEMO: return "demo";
        case TITLE_KIND_DSIWARE: return "dsiware";
        default: return "system";
    }
}

static const char* web_media_name(FS_MediaType media) {
    return media == MEDIATYPE_NAND ? "nand" : media == MEDIATYPE_GAME_CARD ? "card" : "sd";
}

static int web_compare_ids(const void* a, const void* b) {
    u64 x = *(const u64*) a;
    u64 y = *(const u64*) b;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Fast: ids, kind, version and size only. The names and icons come title by title.
static void web_api_titles(web_request* r) {
    json_t* list = json_array();
    static const FS_MediaType media[2] = {MEDIATYPE_SD, MEDIATYPE_NAND};
    for(u32 m = 0; m < 2; m++) {
        u32 count = 0;
        if(R_FAILED(AM_GetTitleCount(media[m], &count)) || count == 0) {
            continue;
        }

        u64* ids = (u64*) calloc(count, sizeof(u64));
        if(ids == NULL) {
            continue;
        }

        if(R_SUCCEEDED(AM_GetTitleList(&count, media[m], count, ids))) {
            qsort(ids, count, sizeof(u64), web_compare_ids);
            for(u32 i = 0; i < count; i++) {
                if(((ids[i] >> 32) & 0x8000) != 0) {
                    continue;   // DSiWare: its own screens.
                }

                AM_TitleEntry entry;
                if(R_FAILED(AM_GetTitleInfo(media[m], 1, &ids[i], &entry))) {
                    continue;
                }

                char id[17];
                snprintf(id, sizeof(id), "%016llX", (unsigned long long) ids[i]);
                json_array_append_new(list, json_pack("{s:s,s:s,s:s,s:i,s:I}", "id", id, "media", web_media_name(media[m]), "kind", web_kind_name(ids[i]),
                                                      "version", (int) entry.version, "size", (json_int_t) entry.size));
            }
        }

        free(ids);
    }

    web_send_json(r, 200, json_pack("{s:b,s:o}", "ok", 1, "titles", list));
}

// The title's SMDH, or NULL.
static SMDH* web_read_smdh(title_info* title) {
    static const u32 filePath[5] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000};
    u32 archivePath[4] = {(u32) (title->titleId & 0xFFFFFFFF), (u32) ((title->titleId >> 32) & 0xFFFFFFFF), title->mediaType, 0x00000000};

    SMDH* smdh = (SMDH*) calloc(1, sizeof(SMDH));
    if(smdh == NULL) {
        return NULL;
    }

    Handle file = 0;
    bool ok = false;
    if(R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SAVEDATA_AND_CONTENT, fs_make_path_binary(archivePath, sizeof(archivePath)),
                                           fs_make_path_binary(filePath, sizeof(filePath)), FS_OPEN_READ, 0))) {
        u32 got = 0;
        ok = R_SUCCEEDED(FSFILE_Read(file, &got, 0, smdh, sizeof(SMDH))) && got == sizeof(SMDH)
             && smdh->magic[0] == 'S' && smdh->magic[1] == 'M' && smdh->magic[2] == 'D' && smdh->magic[3] == 'H';
        FSFILE_Close(file);
    }

    if(!ok) {
        free(smdh);
        return NULL;
    }

    return smdh;
}

// Which kinds of data a title has (opening each archive is the slow part, so the page asks for it apart).
static void web_title_data_flags(title_info* title, bool* save, bool* ext, bool* boss) {
    bool game = title_kind_of(title->titleId) == TITLE_KIND_GAME || title_kind_of(title->titleId) == TITLE_KIND_DEMO;
    u32 extId = 0;
    *save = game && action_save_data_exists(title, SAVE_KIND_SAVE, &extId);
    *ext = game && action_save_data_exists(title, SAVE_KIND_EXT, &extId);
    *boss = game && action_save_data_exists(title, SAVE_KIND_BOSS, &extId);
}

static void web_api_title_data(web_request* r) {
    title_info title;
    if(!web_parse_title(r, &title)) {
        return;
    }

    bool save, ext, boss;
    web_title_data_flags(&title, &save, &ext, &boss);
    web_send_json(r, 200, json_pack("{s:b,s:b,s:b,s:b}", "ok", 1, "save", save, "ext", ext, "boss", boss));
}

// One title's details: name, publisher, product code and, unless data=0, which kinds of data it has.
static void web_api_title(web_request* r) {
    title_info title;
    if(!web_parse_title(r, &title)) {
        return;
    }

    char name[0x100] = "";
    char publisher[0x100] = "";
    char description[0x200] = "";
    SMDH* smdh = web_read_smdh(&title);
    if(smdh != NULL) {
        SMDH_title* names = smdh_select_title(smdh);
        utf16_to_utf8((uint8_t*) name, names->shortDescription, sizeof(name) - 1);
        utf16_to_utf8((uint8_t*) publisher, names->publisher, sizeof(publisher) - 1);
        utf16_to_utf8((uint8_t*) description, names->longDescription, sizeof(description) - 1);
        free(smdh);
    }

    char code[0x10] = "";
    AM_GetTitleProductCode(title.mediaType, title.titleId, code);

    char skip[4] = "";
    if(web_query_get(r->query, "data", skip, sizeof(skip)) && strcmp(skip, "0") == 0) {
        web_send_json(r, 200, json_pack("{s:b,s:s,s:s,s:s,s:s}", "ok", 1, "name", name, "publisher", publisher, "code", code, "description", description));
        return;
    }

    bool save, ext, boss;
    web_title_data_flags(&title, &save, &ext, &boss);
    web_send_json(r, 200, json_pack("{s:b,s:s,s:s,s:s,s:s,s:b,s:b,s:b}", "ok", 1, "name", name, "publisher", publisher, "code", code, "description", description, "save", save, "ext", ext, "boss", boss));
}

// Sends an SMDH's large icon as a 48x48 BMP (frees the SMDH).
static void web_send_smdh_icon(web_request* r, SMDH* smdh) {
    enum { SIZE = 48, ROW = SIZE * 3, DATA = ROW * SIZE, HEAD = 54 };
    u8* bmp = (u8*) calloc(1, HEAD + DATA);
    if(bmp == NULL) {
        free(smdh);
        web_send_error(r, 500, "out of memory");
        return;
    }

    bmp[0] = 'B';
    bmp[1] = 'M';
    u32 fileSize = HEAD + DATA;
    memcpy(bmp + 2, &fileSize, 4);
    u32 offset = HEAD;
    memcpy(bmp + 10, &offset, 4);
    u32 header = 40;
    memcpy(bmp + 14, &header, 4);
    s32 width = SIZE;
    s32 height = SIZE;   // Positive: rows from the bottom.
    memcpy(bmp + 18, &width, 4);
    memcpy(bmp + 22, &height, 4);
    u16 planes = 1;
    u16 bits = 24;
    memcpy(bmp + 26, &planes, 2);
    memcpy(bmp + 28, &bits, 2);
    u32 dataSize = DATA;
    memcpy(bmp + 34, &dataSize, 4);

    // The icon is RGB565 in 8x8 tiles, each in Morton order.
    const u16* pixels = (const u16*) smdh->largeIcon;
    for(u32 tile = 0; tile < (SIZE / 8) * (SIZE / 8); tile++) {
        u32 tx = (tile % (SIZE / 8)) * 8;
        u32 ty = (tile / (SIZE / 8)) * 8;
        for(u32 i = 0; i < 64; i++) {
            u32 x = tx + ((i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4));
            u32 y = ty + (((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4));
            u16 c = pixels[tile * 64 + i];
            u8* out = bmp + HEAD + (SIZE - 1 - y) * ROW + x * 3;
            out[0] = (u8) (((c & 0x1F) * 255) / 31);
            out[1] = (u8) ((((c >> 5) & 0x3F) * 255) / 63);
            out[2] = (u8) ((((c >> 11) & 0x1F) * 255) / 31);
        }
    }

    free(smdh);
    if(web_send_head(r, 200, "image/bmp", HEAD + DATA, "Cache-Control: max-age=3600\r\n")) {
        web_send_all(r->fd, (const char*) bmp, HEAD + DATA);
    }

    free(bmp);
}


// The large icon as a 48x48 BMP.
static void web_api_icon(web_request* r) {
    title_info title;
    if(!web_parse_title(r, &title)) {
        return;
    }

    SMDH* smdh = web_read_smdh(&title);
    if(smdh == NULL) {
        web_send_error(r, 404, "no icon");
        return;
    }

    web_send_smdh_icon(r, smdh);
}

// ---- tickets and extra data (read only: deleting stays a question on the console) ----

// Every ticket's title ID, and where that title is installed ("" when it is not).
static void web_api_tickets(web_request* r) {
    u32 count = 0;
    if(R_FAILED(AM_GetTicketCount(&count))) {
        web_send_error(r, 500, "cannot read the tickets");
        return;
    }

    json_t* list = json_array();
    u64* ids = (u64*) calloc(count == 0 ? 1 : count, sizeof(u64));
    if(ids == NULL) {
        json_decref(list);
        web_send_error(r, 500, "out of memory");
        return;
    }

    u32 got = 0;
    if(R_SUCCEEDED(AM_GetTicketList(&got, count, 0, ids))) {
        qsort(ids, got, sizeof(u64), web_compare_ids);
        for(u32 i = 0; i < got; i++) {
            if(i > 0 && ids[i] == ids[i - 1]) {
                continue;   // A title can hold several tickets (Azahar repeats them).
            }

            const char* where = "";
            AM_TitleEntry entry;
            if(R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &ids[i], &entry))) {
                where = "sd";
            } else if(R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_NAND, 1, &ids[i], &entry))) {
                where = "nand";
            }

            char id[17];
            snprintf(id, sizeof(id), "%016llX", (unsigned long long) ids[i]);
            json_array_append_new(list, json_pack("{s:s,s:s,s:s}", "id", id, "media", where, "kind", web_kind_name(ids[i])));
        }
    }

    free(ids);
    web_send_json(r, 200, json_pack("{s:b,s:o}", "ok", 1, "tickets", list));
}

#define WEB_MAX_EXT 512

static bool web_parse_ext(web_request* r, FS_ExtSaveDataInfo* info) {
    char id[32] = "";
    char media[8] = "";
    if(!web_query_get(r->query, "id", id, sizeof(id)) || !web_query_get(r->query, "media", media, sizeof(media)) || strlen(id) != 16
       || (strcmp(media, "sd") != 0 && strcmp(media, "nand") != 0)) {
        web_send_error(r, 400, "bad extra data");
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->mediaType = strcmp(media, "nand") == 0 ? MEDIATYPE_NAND : MEDIATYPE_SD;
    info->saveId = strtoull(id, NULL, 16);
    return true;
}

// Every extra data archive (SD and NAND): {id, media, shared}.
static void web_api_extdata(web_request* r) {
    json_t* list = json_array();
    static const FS_MediaType media[2] = {MEDIATYPE_SD, MEDIATYPE_NAND};
    u64* ids = (u64*) calloc(WEB_MAX_EXT, sizeof(u64));
    if(ids == NULL) {
        json_decref(list);
        web_send_error(r, 500, "out of memory");
        return;
    }

    for(u32 m = 0; m < 2; m++) {
        u32 count = 0;
        if(R_FAILED(FSUSER_EnumerateExtSaveData(&count, WEB_MAX_EXT, media[m], 8, media[m] == MEDIATYPE_NAND, (u8*) ids))) {
            continue;
        }

        qsort(ids, count, sizeof(u64), web_compare_ids);
        for(u32 i = 0; i < count; i++) {
            char id[17];
            snprintf(id, sizeof(id), "%016llX", (unsigned long long) ids[i]);
            json_array_append_new(list, json_pack("{s:s,s:s}", "id", id, "media", web_media_name(media[m])));
        }
    }

    free(ids);
    web_send_json(r, 200, json_pack("{s:b,s:o}", "ok", 1, "extdata", list));
}

// The SMDH an extra data archive keeps, or NULL.
static SMDH* web_read_ext_smdh(const FS_ExtSaveDataInfo* info) {
    SMDH* smdh = (SMDH*) calloc(1, sizeof(SMDH));
    if(smdh == NULL) {
        return NULL;
    }

    u32 got = 0;
    if(R_FAILED(FSUSER_ReadExtSaveDataIcon(&got, *info, sizeof(SMDH), (u8*) smdh)) || got != sizeof(SMDH)
       || smdh->magic[0] != 'S' || smdh->magic[1] != 'M' || smdh->magic[2] != 'D' || smdh->magic[3] != 'H') {
        free(smdh);
        return NULL;
    }

    return smdh;
}

static void web_api_extdata_info(web_request* r) {
    FS_ExtSaveDataInfo info;
    if(!web_parse_ext(r, &info)) {
        return;
    }

    char name[0x100] = "";
    char publisher[0x100] = "";
    SMDH* smdh = web_read_ext_smdh(&info);
    if(smdh != NULL) {
        SMDH_title* names = smdh_select_title(smdh);
        utf16_to_utf8((uint8_t*) name, names->shortDescription, sizeof(name) - 1);
        utf16_to_utf8((uint8_t*) publisher, names->publisher, sizeof(publisher) - 1);
        free(smdh);
    }

    web_send_json(r, 200, json_pack("{s:b,s:s,s:s}", "ok", 1, "name", name, "publisher", publisher));
}

static void web_api_extdata_icon(web_request* r) {
    FS_ExtSaveDataInfo info;
    if(!web_parse_ext(r, &info)) {
        return;
    }

    SMDH* smdh = web_read_ext_smdh(&info);
    if(smdh == NULL) {
        web_send_error(r, 404, "no icon");
        return;
    }

    web_send_smdh_icon(r, smdh);
}

// ---- saves ----

static bool web_parse_kind(web_request* r, save_kind* kind) {
    char text[8] = "";
    web_query_get(r->query, "kind", text, sizeof(text));
    if(strcmp(text, "save") == 0) {
        *kind = SAVE_KIND_SAVE;
    } else if(strcmp(text, "ext") == 0) {
        *kind = SAVE_KIND_EXT;
    } else if(strcmp(text, "boss") == 0) {
        *kind = SAVE_KIND_BOSS;
    } else {
        web_send_error(r, 400, "bad kind");
        return false;
    }

    return true;
}

// Makes the package of a title's save, extra or SpotPass data: the page then downloads it as a file.
// ---- the screens, live, and the buttons ----

#define WEB_TOP_W 400
#define WEB_BOTTOM_W 320
#define WEB_SCREEN_H 240
#define WEB_FRAME_BYTES ((WEB_TOP_W + WEB_BOTTOM_W) * WEB_SCREEN_H * 2)

static u16* web_frame = NULL;
static volatile int web_frame_state = 0;   // 0 nothing asked, 1 asked, 2 made, 3 failed

// Both screens as RGB565 (the top's 400x240, then the bottom's 320x240), as the last frame drawn. The
// main thread reads the framebuffers (webserver_poll_main).
static void web_api_screens(web_request* r) {
    // Not while something transfers: a frame is ~340 KB, and the owner does not need the live view then.
    xferturn_status status;
    xferturn_get(&status);
    if(status.busy) {
        web_send_busy(r);
        return;
    }

    if(web_frame == NULL) {
        web_frame = (u16*) malloc(WEB_FRAME_BYTES);
        if(web_frame == NULL) {
            web_send_error(r, 500, "out of memory");
            return;
        }
    }

    web_frame_state = 1;
    for(int waited = 0; web_frame_state == 1 && web_run; waited += WEB_NAP_MS) {
        if(waited > 1500) {
            web_frame_state = 0;
            web_send_error(r, 503, "the console did not answer in time");
            return;
        }

        svcSleepThread((s64) WEB_NAP_MS * 1000000);
    }

    if(web_frame_state != 2) {
        web_send_error(r, 503, "no frame");
        return;
    }

    if(web_send_head(r, 200, "application/octet-stream", WEB_FRAME_BYTES, NULL)) {
        web_send_all(r->fd, (const char*) web_frame, WEB_FRAME_BYTES);
    }
}

static u32 web_key_from_name(const char* name) {
    static const struct {
        const char* name;
        u32 key;
    } names[] = {
            {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R},
            {"ZL", KEY_ZL}, {"ZR", KEY_ZR}, {"START", KEY_START}, {"SELECT", KEY_SELECT},
            {"UP", KEY_DUP}, {"DOWN", KEY_DDOWN}, {"LEFT", KEY_DLEFT}, {"RIGHT", KEY_DRIGHT}
    };

    for(u32 i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if(strcasecmp(names[i].name, name) == 0) {
            return names[i].key;
        }
    }

    return 0;
}

static u32 web_number(web_request* r, const char* key, u32 fallback) {
    char text[16];
    return web_query_get(r->query, key, text, sizeof(text)) ? (u32) strtoul(text, NULL, 10) : fallback;
}

// Holds buttons ("A", "A+R", "UP") for some frames: the same as the FTP server's SITE PRESS.
static void web_api_press(web_request* r) {
    char text[64] = "";
    web_query_get(r->query, "keys", text, sizeof(text));

    u32 keys = 0;
    char* save = NULL;
    for(char* k = strtok_r(text, "+ ", &save); k != NULL; k = strtok_r(NULL, "+ ", &save)) {
        u32 key = web_key_from_name(k);
        if(key == 0) {
            web_send_error(r, 400, "unknown button");
            return;
        }

        keys |= key;
    }

    u32 frames = web_number(r, "frames", 4);
    if(keys == 0 || frames == 0 || frames > 600) {
        web_send_error(r, 400, "keys and frames (1 to 600)");
        return;
    }

    input_inject_keys(keys, frames);
    web_send_ok(r);
}

static void web_api_tap(web_request* r) {
    u32 x = web_number(r, "x", 999);
    u32 y = web_number(r, "y", 999);
    u32 frames = web_number(r, "frames", 4);
    if(x >= WEB_BOTTOM_W || y >= WEB_SCREEN_H || frames == 0 || frames > 600) {
        web_send_error(r, 400, "x 0-319, y 0-239");
        return;
    }

    input_inject_tap((u16) x, (u16) y, frames);
    web_send_ok(r);
}

static void web_api_drag(web_request* r) {
    u32 x = web_number(r, "x", 999);
    u32 y1 = web_number(r, "y1", 999);
    u32 y2 = web_number(r, "y2", 999);
    u32 frames = web_number(r, "frames", 20);
    if(x >= WEB_BOTTOM_W || y1 >= WEB_SCREEN_H || y2 >= WEB_SCREEN_H || frames == 0 || frames > 600) {
        web_send_error(r, 400, "x 0-319, y1 and y2 0-239");
        return;
    }

    input_inject_drag((u16) x, (u16) y1, (u16) y2, frames);
    web_send_ok(r);
}

// ---- what has to happen on the main thread ----
//
// Anything that opens a question or a screen on the console is done by the main thread (webserver_poll_main),
// one request at a time. The page never changes anything this way on its own: the console asks first.

typedef enum {
    REQ_NONE,
    REQ_RECEIVE,       // A package the page uploaded: unpacked, and imported if asked.
    REQ_IMPORT,        // A backup already on the SD card.
    REQ_LAUNCH,
    REQ_DELETE,
    REQ_INSTALL_URL,
    REQ_PACK           // A save package for the page to download.
} web_request_kind;

static struct {
    volatile bool pending;
    web_request_kind kind;
    title_info title;
    char name[0x100];
    save_kind saveKind;
    u32 extId;
    bool flag;                 // REQ_RECEIVE: import; REQ_DELETE: with the ticket.
    char path[WEB_PATH_MAX];
    char urls[4096];
} web_job;

// Claims the slot; false (and the answer sent) when the console is still on the last request.
static bool web_job_begin(web_request* r, web_request_kind kind) {
    if(web_job.pending) {
        web_send_error(r, 409, "the console is still asking about the last request");
        return false;
    }

    memset(&web_job, 0, sizeof(web_job));
    web_job.kind = kind;
    return true;
}

static void web_job_run() {
    switch(web_job.kind) {
        case REQ_RECEIVE:
            action_save_package_received(web_job.path, web_job.flag);
            break;
        case REQ_IMPORT:
            action_save_import_backup(&web_job.title, web_job.name, web_job.saveKind, web_job.extId, web_job.path);
            break;
        case REQ_LAUNCH:
        case REQ_DELETE: {
            // The actions keep the item while their question is open: it is never freed here.
            list_item* item = task_title_item(web_job.title.mediaType, web_job.title.titleId);
            if(item == NULL) {
                break;
            }

            if(web_job.kind == REQ_LAUNCH) {
                action_launch_title(NULL, item);
            } else {
                static linked_list deleting;
                static bool ready = false;
                if(!ready) {
                    linked_list_init(&deleting);
                    ready = true;
                }

                linked_list_add(&deleting, item);
                if(web_job.flag) {
                    action_delete_title_ticket(&deleting, item);
                } else {
                    action_delete_title(&deleting, item);
                }
            }

            break;
        }
        case REQ_PACK:
            action_save_pack_web_start(&web_job.title, web_job.name, web_job.saveKind, web_job.extId);
            break;
        case REQ_INSTALL_URL:
            action_install_url("Install from the URL(s) sent from the web page?", web_job.urls, NULL, NULL, NULL, NULL, NULL);
            break;
        default:
            break;
    }

    web_job.kind = REQ_NONE;
    web_job.pending = false;
}

static void web_api_save_pack(web_request* r) {
    title_info title;
    save_kind kind;
    if(!web_parse_title(r, &title) || !web_parse_kind(r, &kind)) {
        return;
    }

    char name[0x100] = "";
    web_query_get(r->query, "name", name, sizeof(name));
    if(name[0] == '\0') {
        string_copy(name, "title", sizeof(name));
    }

    u32 extId = 0;
    if(!action_save_data_exists(&title, kind, &extId)) {
        web_send_error(r, 404, "this title has no such data (or it was never started)");
        return;
    }

    // Made on the console, which shows its progress there; the page follows it with save/pack/status.
    if(!action_save_pack_web_claim()) {
        web_send_error(r, 409, "the console is still packing the last one");
        return;
    }

    if(!web_job_begin(r, REQ_PACK)) {
        action_save_pack_web_release();
        return;
    }

    web_job.title = title;
    string_copy(web_job.name, name, sizeof(web_job.name));
    web_job.saveKind = kind;
    web_job.extId = extId;
    web_job.pending = true;
    web_send_ok(r);
}

static void web_api_save_pack_status(web_request* r) {
    save_pack_status st;
    action_save_pack_web_status(&st);

    char error[48] = "";
    if(st.finished && R_FAILED(st.res)) {
        if(st.res == R_APP_CANCELLED) {
            string_copy(error, "cancelled", sizeof(error));
        } else {
            snprintf(error, sizeof(error), "packing failed (%08lX)", st.res);
        }
    }

    web_send_json(r, 200, json_pack("{s:b,s:b,s:b,s:b,s:I,s:I,s:s,s:I,s:s}", "ok", 1, "running", st.running, "finished", st.finished,
                                    "counting", st.counting, "processed", (json_int_t) st.processed, "total", (json_int_t) st.total,
                                    "path", st.path, "size", (json_int_t) st.size, "error", error));
}

static void web_api_save_pack_cancel(web_request* r) {
    action_save_pack_web_cancel();
    web_send_ok(r);
}

static void web_api_save_receive(web_request* r) {
    char vpath[WEB_PATH_MAX];
    char rpath[WEB_PATH_MAX + 8];
    if(!web_path_param(r, "path", vpath, sizeof(vpath), rpath, sizeof(rpath))) {
        return;
    }

    u64 id = 0;
    save_kind kind = SAVE_KIND_SAVE;
    char name[64];
    const char* file = strrchr(vpath, '/');
    if(strncmp(vpath, "/nsa/received/", 14) != 0 || !action_save_package_name(file + 1, &id, &kind, name, sizeof(name))) {
        web_send_error(r, 400, "not a save package in /nsa/received/");
        return;
    }

    char want[8] = "";
    web_query_get(r->query, "import", want, sizeof(want));
    if(!web_job_begin(r, REQ_RECEIVE)) {
        return;
    }

    string_copy(web_job.path, vpath, sizeof(web_job.path));
    web_job.flag = strcmp(want, "1") == 0 && action_save_package_can_import(id, kind);
    bool import = web_job.flag;
    web_job.pending = true;
    web_send_json(r, 200, json_pack("{s:b,s:b}", "ok", 1, "import", import));
}

// The backups of a title's data that are on the SD card: NSA's exports and Checkpoint's.
static void web_api_save_backups(web_request* r) {
    title_info title;
    save_kind kind;
    if(!web_parse_title(r, &title) || !web_parse_kind(r, &kind)) {
        return;
    }

    save_backup* backups = NULL;
    u32 count = action_save_backups(&title, kind, &backups);
    json_t* list = json_array();
    for(u32 i = 0; i < count; i++) {
        json_array_append_new(list, json_pack("{s:s,s:s,s:b,s:b,s:b}", "path", backups[i].path, "label", backups[i].label, "checkpoint", backups[i].checkpoint, "jksm", backups[i].jksm, "pksm", backups[i].pksm));
    }

    free(backups);
    web_send_json(r, 200, json_pack("{s:b,s:o}", "ok", 1, "backups", list));
}

// Only a backup the console lists for that title (NSA's exports, Checkpoint's) can be deleted.
static void web_api_save_backup_delete(web_request* r) {
    title_info title;
    save_kind kind;
    if(!web_parse_title(r, &title) || !web_parse_kind(r, &kind)) {
        return;
    }

    char path[WEB_PATH_MAX];
    if(!web_query_get(r->query, "path", path, sizeof(path))) {
        web_send_error(r, 400, "path missing");
        return;
    }

    Result res = action_save_delete_backup(&title, kind, path);
    if(res == R_APP_INVALID_ARGUMENT) {
        web_send_error(r, 404, "no such backup of this title");
    } else if(R_FAILED(res)) {
        char text[48];
        snprintf(text, sizeof(text), "delete failed (%08lX)", res);
        web_send_error(r, 500, text);
    } else {
        web_send_ok(r);
    }
}

static void web_api_save_import(web_request* r) {
    title_info title;
    save_kind kind;
    char path[WEB_PATH_MAX] = "";
    if(!web_parse_title(r, &title) || !web_parse_kind(r, &kind)) {
        return;
    }

    u32 extId = 0;
    if(!web_query_get(r->query, "path", path, sizeof(path)) || !action_save_data_exists(&title, kind, &extId)) {
        web_send_error(r, 404, "this title has no such data (or it was never started)");
        return;
    }

    if(!web_job_begin(r, REQ_IMPORT)) {
        return;
    }

    web_job.title = title;
    web_query_get(r->query, "name", web_job.name, sizeof(web_job.name));
    web_job.saveKind = kind;
    web_job.extId = extId;
    string_copy(web_job.path, path, sizeof(web_job.path));
    web_job.pending = true;
    web_send_ok(r);
}

static void web_api_title_launch(web_request* r) {
    title_info title;
    if(!web_parse_title(r, &title) || !web_job_begin(r, REQ_LAUNCH)) {
        return;
    }

    web_job.title = title;
    web_job.pending = true;
    web_send_ok(r);
}

static void web_api_title_delete(web_request* r) {
    title_info title;
    if(!web_parse_title(r, &title)) {
        return;
    }

    char ticket[8] = "";
    web_query_get(r->query, "ticket", ticket, sizeof(ticket));
    if(!web_job_begin(r, REQ_DELETE)) {
        return;
    }

    web_job.title = title;
    web_job.flag = strcmp(ticket, "1") == 0;
    web_job.pending = true;
    web_send_ok(r);
}

// The body is the URLs, one per line: the console asks before installing them.
static void web_api_install_url(web_request* r) {
    xferturn_status status;
    xferturn_get(&status);
    if(status.busy) {
        web_send_busy(r);
        return;
    }

    if(r->contentLength == 0 || r->contentLength >= sizeof(web_job.urls)) {
        web_send_error(r, 400, "send between 1 and 4000 bytes of URLs");
        return;
    }

    char* urls = (char*) calloc(1, sizeof(web_job.urls));
    if(urls == NULL) {
        web_send_error(r, 500, "out of memory");
        return;
    }

    u32 early = r->have - r->headLen;
    if(early > r->contentLength) {
        early = (u32) r->contentLength;
    }

    memcpy(urls, r->head + r->headLen, early);
    u32 got = early;
    bool ok = true;
    while(ok && got < r->contentLength) {
        if(!web_wait_fd(r->fd, POLLIN, WEB_IDLE_MS)) {
            ok = false;
            break;
        }

        ssize_t n = net_recv(r->fd, urls + got, (size_t) (r->contentLength - got), WEB_IDLE_MS);
        if(n <= 0) {
            ok = false;
            break;
        }

        got += (u32) n;
    }

    // Only web addresses, one per line.
    bool valid = ok && got > 0;
    for(char* line = urls; valid && *line != '\0'; ) {
        char* end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t) (end - line) : strlen(line);
        if(len > 0 && !(strncmp(line, "http://", 7) == 0 || strncmp(line, "https://", 8) == 0)) {
            valid = false;
        }

        line = end != NULL ? end + 1 : line + len;
    }

    if(!valid) {
        free(urls);
        web_send_error(r, 400, "only http:// and https:// addresses, one per line");
        return;
    }

    if(!web_job_begin(r, REQ_INSTALL_URL)) {
        free(urls);
        return;
    }

    string_copy(web_job.urls, urls, sizeof(web_job.urls));
    free(urls);
    remoteinstall_note_urls(web_job.urls);
    web_job.pending = true;
    web_send_ok(r);
}

// ---- the console's settings: appearance, sounds and music ----
//
// Read here; changed by the main thread (webserver_poll_main), which applies and saves them as the Settings screen does.

static const char* web_night_ids[] = {"off", "on", "auto"};

static struct {
    volatile int state;    // 0 nothing, 1 asked, 2 applied.
    settings_data values;
} web_settings;

// The System details are read on the main thread, like the screens do: the page asks, the main loop answers.
static struct {
    volatile int state;    // 0 nothing, 1 asked, 2 ready.
    json_t* json;
} web_system;

static void web_api_system(web_request* r) {
    if(web_system.state != 0) {
        web_send_error(r, 409, "the console is still answering the last one");
        return;
    }

    web_system.state = 1;
    for(int waited = 0; web_system.state == 1 && web_run; waited += WEB_NAP_MS) {
        if(waited > 2000) {
            break;
        }

        svcSleepThread((s64) WEB_NAP_MS * 1000000);
    }

    if(web_system.state != 2) {
        web_system.state = 0;
        web_send_error(r, 503, "the console did not answer in time");
        return;
    }

    json_t* json = web_system.json;
    web_system.json = NULL;
    web_system.state = 0;
    json_object_set_new(json, "ok", json_true());
    web_send_json(r, 200, json);
}

static json_t* web_settings_json() {
    settings_data* s = settings_get();

    json_t* themes = json_array();
    for(u32 i = 0; i < theme_count(); i++) {
        json_array_append_new(themes, json_pack("{s:s,s:s}", "id", theme_get(i)->id, "name", theme_get(i)->name));
    }

    json_t* accents = json_array();
    for(u32 i = 0; i < theme_accent_count(); i++) {
        const theme_accent* a = theme_accent_get(i);
        char day[8];
        char night[8];
        web_color(day, a->day);
        web_color(night, a->night);
        json_array_append_new(accents, json_pack("{s:s,s:s,s:s,s:s}", "id", a->id, "name", a->name, "day", day, "night", night));
    }

    return json_pack("{s:b,s:s,s:o,s:s,s:o,s:s,s:s,s:i,s:i,s:i,s:i,s:b,s:b}", "ok", 1,
                     "theme", theme_get(s->theme)->id, "themes", themes,
                     "accent", theme_accent_get(s->accent)->id, "accents", accents,
                     "night", web_night_ids[s->night < 3 ? s->night : 0],
                     "language", i18n_language_code(s->language),
                     "sounds", (int) s->sounds, "soundVolume", (int) s->soundVolume,
                     "music", (int) s->music, "musicVolume", (int) s->musicVolume,
                     "clock24", datefmt_clock24(), "monthFirst", datefmt_month_first());
}

static void web_api_settings(web_request* r) {
    web_send_json(r, 200, web_settings_json());
}

// Only what the query names changes; an unknown value is refused before anything changes.
static void web_api_settings_set(web_request* r) {
    if(web_settings.state == 1) {
        web_send_error(r, 409, "the console is still applying the last change");
        return;
    }

    settings_data values = *settings_get();
    char text[32];

    if(web_query_get(r->query, "theme", text, sizeof(text))) {
        u32 i = 0;
        while(i < theme_count() && strcmp(theme_get(i)->id, text) != 0) {
            i++;
        }

        if(i == theme_count()) {
            web_send_error(r, 400, "unknown theme");
            return;
        }

        values.theme = i;
    }

    if(web_query_get(r->query, "accent", text, sizeof(text))) {
        u32 i = 0;
        while(i < theme_accent_count() && strcmp(theme_accent_get(i)->id, text) != 0) {
            i++;
        }

        if(i == theme_accent_count()) {
            web_send_error(r, 400, "unknown accent");
            return;
        }

        values.accent = i;
    }

    if(web_query_get(r->query, "night", text, sizeof(text))) {
        u32 i = 0;
        while(i < 3 && strcmp(web_night_ids[i], text) != 0) {
            i++;
        }

        if(i == 3) {
            web_send_error(r, 400, "night is off, on or auto");
            return;
        }

        values.night = (theme_night_mode) i;
    }

    if(web_query_get(r->query, "language", text, sizeof(text))) {
        language_id language = i18n_language_from_code(text);
        if(language == LANGUAGE_SYSTEM && strcmp(text, i18n_language_code(LANGUAGE_SYSTEM)) != 0) {
            web_send_error(r, 400, "unknown language");
            return;
        }

        values.language = language;
    }

    values.sounds = web_number(r, "sounds", values.sounds);
    values.soundVolume = web_number(r, "soundVolume", values.soundVolume);
    values.music = web_number(r, "music", values.music);
    values.musicVolume = web_number(r, "musicVolume", values.musicVolume);
    if(values.sounds > SOUNDS_ALL || values.music >= BGMUSIC_COUNT || values.soundVolume > 100 || values.musicVolume > 100) {
        web_send_error(r, 400, "sounds 0-2, music 0-3, volumes 0-100");
        return;
    }

    web_settings.values = values;
    web_settings.state = 1;
    for(int waited = 0; web_settings.state == 1 && web_run; waited += WEB_NAP_MS) {
        if(waited > 2000) {
            break;
        }

        svcSleepThread((s64) WEB_NAP_MS * 1000000);
    }

    web_send_json(r, 200, web_settings_json());
}

// Main thread.
static void web_settings_apply() {
    settings_data* s = settings_get();
    s->theme = web_settings.values.theme;
    s->accent = web_settings.values.accent;
    s->night = web_settings.values.night;
    s->language = web_settings.values.language;
    s->sounds = web_settings.values.sounds;
    s->soundVolume = web_settings.values.soundVolume;
    s->music = web_settings.values.music;
    s->musicVolume = web_settings.values.musicVolume;
    settings_apply();
    settings_save();
    web_settings.state = 2;
}

// ---- serving ----

// The question the console shows now, so the page can show it too (and answer it).
static void web_api_prompt(web_request* r) {
    prompt_web_info info;
    if(!prompt_web_get(&info)) {
        web_send_json(r, 200, json_pack("{s:b,s:n}", "ok", 1, "prompt"));
        return;
    }

    json_t* options = json_array();
    for(u32 i = 0; i < info.numOptions; i++) {
        json_array_append_new(options, json_string(info.options[i]));
    }

    web_send_json(r, 200, json_pack("{s:b,s:{s:I,s:s,s:s,s:o,s:b}}", "ok", 1, "prompt", "id", (json_int_t) info.id, "title", info.title, "text", info.text,
                                    "options", options, "danger", info.danger));
}

// Answers the question the console shows: the same as pressing that button there.
static void web_api_prompt_answer(web_request* r) {
    char text[16];
    if(!web_query_get(r->query, "id", text, sizeof(text))) {
        web_send_error(r, 400, "id and option");
        return;
    }

    u32 id = (u32) strtoul(text, NULL, 10);
    if(!web_query_get(r->query, "option", text, sizeof(text))) {
        web_send_error(r, 400, "id and option");
        return;
    }

    if(!prompt_web_answer(id, (u32) strtoul(text, NULL, 10))) {
        web_send_error(r, 409, "that question is not on the console any more");
        return;
    }

    web_send_ok(r);
}

static void web_handle(web_request* r) {
    if(!web_read_head(r)) {
        return;
    }

    web_parse_head(r);

    if(strcmp(r->path, "/") == 0 || strcmp(r->path, "/index.html") == 0 || strcmp(r->path, "/theme.json") == 0 || strcmp(r->path, "/font.ttf") == 0
       || strcmp(r->path, "/icons.png") == 0 || strcmp(r->path, "/icons.js") == 0) {
        if(strcmp(r->method, "GET") == 0) {
            if(strcmp(r->path, "/theme.json") == 0) {
                web_send_theme(r);
            } else if(strcmp(r->path, "/font.ttf") == 0) {
                web_send_romfs(r, "romfs:/web/PixelifySans.ttf", "font/ttf", true);
            } else if(strcmp(r->path, "/icons.png") == 0) {
                web_send_romfs(r, "romfs:/icons32.png", "image/png", true);
            } else if(strcmp(r->path, "/icons.js") == 0) {
                web_send_romfs(r, "romfs:/web/icons.js", "text/javascript; charset=utf-8", true);
            } else {
                web_send_romfs(r, "romfs:/web/index.html", "text/html; charset=utf-8", false);
            }
        } else {
            web_send_error(r, 405, "method not allowed");
        }

        return;
    }

    if(strncmp(r->path, "/api/", 5) != 0) {
        web_send_error(r, 404, "not found");
        return;
    }

    if(web_locked_until > osGetTime()) {
        web_send_error(r, 429, "too many wrong PINs: wait a minute");
        return;
    }

    if(!web_check_pin(r)) {
        web_send_error(r, 401, "wrong PIN");
        return;
    }

    const char* api = r->path + 5;
    bool get = strcmp(r->method, "GET") == 0;
    bool put = strcmp(r->method, "PUT") == 0;
    bool post = strcmp(r->method, "POST") == 0;
    bool del = strcmp(r->method, "DELETE") == 0;

    if(get && strcmp(api, "info") == 0) {
        web_api_info(r);
    } else if(get && strcmp(api, "list") == 0) {
        web_api_list(r);
    } else if(get && strcmp(api, "file") == 0) {
        web_api_get_file(r);
    } else if(put && strcmp(api, "file") == 0) {
        web_api_put_file(r);
    } else if(del && strcmp(api, "file") == 0) {
        web_api_delete_file(r);
    } else if(post && strcmp(api, "mkdir") == 0) {
        web_api_mkdir(r);
    } else if(post && strcmp(api, "rename") == 0) {
        web_api_rename(r);
    } else if(post && strcmp(api, "copy") == 0) {
        web_api_copy(r);
    } else if(post && strcmp(api, "install") == 0) {
        web_api_install(r);
    } else if(get && strcmp(api, "titles") == 0) {
        web_api_titles(r);
    } else if(get && strcmp(api, "title") == 0) {
        web_api_title(r);
    } else if(get && strcmp(api, "title/data") == 0) {
        web_api_title_data(r);
    } else if(get && strcmp(api, "tickets") == 0) {
        web_api_tickets(r);
    } else if(get && strcmp(api, "extdata") == 0) {
        web_api_extdata(r);
    } else if(get && strcmp(api, "extdata/info") == 0) {
        web_api_extdata_info(r);
    } else if(get && strcmp(api, "extdata/icon") == 0) {
        web_api_extdata_icon(r);
    } else if(get && strcmp(api, "icon") == 0) {
        web_api_icon(r);
    } else if(post && strcmp(api, "save/pack") == 0) {
        web_api_save_pack(r);
    } else if(get && strcmp(api, "save/pack/status") == 0) {
        web_api_save_pack_status(r);
    } else if(post && strcmp(api, "save/pack/cancel") == 0) {
        web_api_save_pack_cancel(r);
    } else if(post && strcmp(api, "save/receive") == 0) {
        web_api_save_receive(r);
    } else if(get && strcmp(api, "save/backups") == 0) {
        web_api_save_backups(r);
    } else if(post && strcmp(api, "save/backup/delete") == 0) {
        web_api_save_backup_delete(r);
    } else if(post && strcmp(api, "save/import") == 0) {
        web_api_save_import(r);
    } else if(post && strcmp(api, "title/launch") == 0) {
        web_api_title_launch(r);
    } else if(post && strcmp(api, "title/delete") == 0) {
        web_api_title_delete(r);
    } else if(get && strcmp(api, "screens") == 0) {
        web_api_screens(r);
    } else if(post && strcmp(api, "input/press") == 0) {
        web_api_press(r);
    } else if(post && strcmp(api, "input/tap") == 0) {
        web_api_tap(r);
    } else if(post && strcmp(api, "input/drag") == 0) {
        web_api_drag(r);
    } else if(get && strcmp(api, "system") == 0) {
        web_api_system(r);
    } else if(get && strcmp(api, "settings") == 0) {
        web_api_settings(r);
    } else if(post && strcmp(api, "settings") == 0) {
        web_api_settings_set(r);
    } else if(post && strcmp(api, "install-url") == 0) {
        web_api_install_url(r);
    } else if(get && strcmp(api, "prompt") == 0) {
        web_api_prompt(r);
    } else if(post && strcmp(api, "prompt/answer") == 0) {
        web_api_prompt_answer(r);
    } else {
        web_send_error(r, 404, "unknown request");
    }
}

static void web_main(void* arg) {
    web_request* r = (web_request*) calloc(1, sizeof(web_request));
    if(r == NULL) {
        return;
    }

    while(web_run) {
        if(!web_wait_fd(web_listen, POLLIN, 0)) {
            continue;
        }

        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int fd = accept(web_listen, (struct sockaddr*) &peer, &len);
        if(fd < 0) {
            continue;
        }

        net_set_nonblocking(fd);
        web_client = fd;
        r->fd = fd;
        web_handle(r);
        web_client = -1;
        net_close_gently(fd);
    }

    free(r);
}

// ---- start and stop ----

static void web_shutdown();

static void web_make_pin() {
    // Not secret-grade randomness, but unpredictable enough for a PIN that locks after ten tries.
    u64 mix = osGetTime() ^ (svcGetSystemTick() * 2654435761ULL);
    mix ^= mix >> 17;
    snprintf(web_pin, sizeof(web_pin), "%0*u", WEB_PIN_DIGITS, (unsigned) (mix % 1000000ULL));
}

bool webserver_start() {
    if(web_run) {
        return true;
    }

    web_paused = false;

    u32 wifi = 0;
    if(R_FAILED(ACU_GetWifiStatus(&wifi)) || wifi == 0) {
        snprintf(web_status, sizeof(web_status), "%s", _("No Wi-Fi"));
        return false;
    }

    web_io = (char*) malloc(WEB_IO_SIZE);
    web_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(web_listen >= 0) {
        int yes = 1;
        setsockopt(web_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        // The listening socket's buffers, which the accepted connections inherit, as the FTP server's passive socket
        // (ftp_pasv_buffer_kb): the system's are small and each send()/recv() moved 5-6 KB.
        if(WEB_SOCKET_BUFFER > 0) {
            int size = WEB_SOCKET_BUFFER;
            setsockopt(web_listen, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
            setsockopt(web_listen, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
        }
        net_set_nonblocking(web_listen);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WEB_SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if(web_io == NULL || web_listen < 0 || bind(web_listen, (struct sockaddr*) &addr, sizeof(addr)) != 0 || listen(web_listen, 4) != 0) {
        snprintf(web_status, sizeof(web_status), _("Port %d busy"), WEB_SERVER_PORT);
        web_close_socket(&web_listen);
        free(web_io);
        web_io = NULL;
        return false;
    }

    web_ip = (u32) gethostid();
    web_make_pin();
    web_fails = 0;
    web_locked_until = 0;
    web_run = true;

    // For tests from a computer (tools/web_tests.py): a DEBUG build leaves the PIN where the FTP server can fetch it.
    if(NSA_DEBUG) {
        FILE* f = fopen("sdmc:" APP_DATA_DIR "web-pin.txt", "wb");
        if(f != NULL) {
            fputs(web_pin, f);
            fclose(f);
        }
    }

    web_thread = net_server_thread(web_main, 0x14000);
    if(web_thread == NULL) {
        web_shutdown();
        snprintf(web_status, sizeof(web_status), "%s", _("No thread"));
        return false;
    }

    return true;
}

static void web_shutdown() {
    web_run = false;

    int fd = web_client;
    if(fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }

    if(web_thread != NULL) {
        threadJoin(web_thread, U64_MAX);
        threadFree(web_thread);
        web_thread = NULL;
    }

    web_close_socket(&web_listen);
    free(web_io);
    web_io = NULL;
    free(web_frame);
    web_frame = NULL;
    web_frame_state = 0;
    web_pin[0] = '\0';
    snprintf(web_status, sizeof(web_status), "%s", _("Off"));
}

// Started now, or as soon as there is Wi-Fi (NSA opened without it): it tries every couple of seconds, with no end.
void webserver_start_when_online() {
    if(webserver_start()) {
        return;
    }

    web_retry = true;
    web_retry_next = osGetTime() + 2000;
    web_retry_end = U64_MAX;
}

bool webserver_waiting() {
    return web_retry && !webserver_running();
}

void webserver_stop() {
    web_retry = false;
    web_paused = false;
    web_shutdown();
}

bool webserver_running() {
    return web_run;
}

void webserver_pause() {
    if(web_run || web_retry) {
        web_retry = false;
        web_shutdown();
        web_paused = true;
    }
}

void webserver_resume_later() {
    if(web_paused) {
        web_paused = false;
        web_retry = true;
        web_retry_next = osGetTime() + 3000;
        web_retry_end = osGetTime() + 120000;
    }
}

// The Wi-Fi gone while serving: the server kept looking on but could not serve (its sockets dead) until HOME. It stops,
// and starts again once the Wi-Fi is back (as webserver_start_when_online).
static u64 web_watch_next = 0;

static void web_watch_wifi() {
    if(!web_run || web_paused || osGetTime() < web_watch_next) {
        return;
    }

    web_watch_next = osGetTime() + 2000;
    u32 wifi = 0;
    if(R_FAILED(ACU_GetWifiStatus(&wifi)) || wifi == 0) {
        web_shutdown();
        web_retry = true;
        web_retry_next = osGetTime() + 2000;
        web_retry_end = U64_MAX;
    }
}

void webserver_poll_main() {
    web_watch_wifi();

    // A frame asked for by the page: both screens, now that this frame is drawn.
    if(web_frame_state == 1 && web_frame != NULL) {
        u32 w = 0;
        u32 h = 0;
        bool ok = screen_capture_rgb565(GFX_TOP, web_frame, &w, &h) && w == WEB_TOP_W && h == WEB_SCREEN_H
                  && screen_capture_rgb565(GFX_BOTTOM, web_frame + WEB_TOP_W * WEB_SCREEN_H, &w, &h) && w == WEB_BOTTOM_W && h == WEB_SCREEN_H;
        web_frame_state = ok ? 2 : 3;
    }

    // A package uploaded by the page: unpacked (and imported, behind the red question) like one that was sent.
    if(web_job.pending) {
        web_job_run();
    }

    if(web_system.state == 1) {
        web_system.json = systeminfo_json();
        web_system.state = 2;
    }

    // Settings changed from the page.
    if(web_settings.state == 1) {
        web_settings_apply();
    }

    if(web_retry && osGetTime() >= web_retry_next) {
        if(webserver_start() || osGetTime() > web_retry_end) {
            web_retry = false;
        } else {
            web_retry_next = osGetTime() + 2000;
        }
    }
}

void webserver_address(char* out, size_t size) {
    if(!web_run) {
        snprintf(out, size, "%s", web_status);
        return;
    }

    struct in_addr addr = {.s_addr = web_ip};
    snprintf(out, size, "http://%s:%d", inet_ntoa(addr), WEB_SERVER_PORT);
}

void webserver_pin(char* out, size_t size) {
    snprintf(out, size, "%s", web_run ? web_pin : "");
}
