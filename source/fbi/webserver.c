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
#include "relaunch.h"
#include "transfer.h"
#include "webserver.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "../core/data/smdh.h"

#define WEB_PATH_MAX 512
#define WEB_HEAD_MAX 8192
#define WEB_IO_SIZE (64 * 1024)
#define WEB_NAP_MS 10
#define WEB_SEND_CHUNK (8 * 1024)
#define WEB_IDLE_MS 30000
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

static bool web_wait_fd(int fd, short events, int timeoutMs) {
    int waited = 0;
    while(web_run) {
        struct pollfd p = {.fd = fd, .events = events, .revents = 0};
        int r = poll(&p, 1, 0);
        if(r > 0) {
            return (p.revents & (events | POLLHUP | POLLERR)) != 0;
        }

        if(r < 0) {
            return false;
        }

        svcSleepThread((s64) WEB_NAP_MS * 1000000);
        waited += WEB_NAP_MS;
        if(timeoutMs > 0 && waited >= timeoutMs) {
            return false;
        }
    }

    return false;
}

static bool web_send_all(int fd, const char* data, size_t n) {
    while(n > 0) {
        if(!web_wait_fd(fd, POLLOUT, WEB_IDLE_MS)) {
            return false;
        }

        ssize_t w = send(fd, data, n > WEB_SEND_CHUNK ? WEB_SEND_CHUNK : n, 0);
        if(w <= 0) {
            return false;
        }

        data += w;
        n -= (size_t) w;
    }

    return true;
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

        ssize_t n = recv(r->fd, r->head + r->have, WEB_HEAD_MAX - r->have, 0);
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

    web_send_json(r, 200, json_pack("{s:b,s:s,s:s,s:I,s:I}", "ok", 1, "console", name, "version", VERSION_STRING, "free", (json_int_t) free_, "total", (json_int_t) total));
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

    const char* name = strrchr(vpath, '/');
    name = name != NULL ? name + 1 : vpath;
    char extra[400];
    int n = snprintf(extra, sizeof(extra), "Accept-Ranges: bytes\r\nContent-Disposition: attachment; filename=\"%.200s\"\r\n", name);
    if(partial) {
        snprintf(extra + n, sizeof(extra) - n, "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long) from, (unsigned long long) (size - 1), (unsigned long long) size);
    }

    if(!web_send_head(r, partial ? 206 : 200, "application/octet-stream", (s64) (size - from), extra)) {
        fclose(f);
        return;
    }

    fseek(f, (long) from, SEEK_SET);
    size_t got;
    bool ok = true;
    while(ok && (got = fread(web_io, 1, WEB_IO_SIZE, f)) > 0) {
        ok = web_send_all(r->fd, web_io, got);
    }

    fclose(f);
}

static void web_api_put_file(web_request* r) {
    char vpath[WEB_PATH_MAX];
    char rpath[WEB_PATH_MAX + 8];
    if(!web_path_param(r, "path", vpath, sizeof(vpath), rpath, sizeof(rpath))) {
        return;
    }

    char part[WEB_PATH_MAX + 16];
    snprintf(part, sizeof(part), "%s.part", rpath);
    FILE* f = fopen(part, "wb");
    if(f == NULL) {
        web_send_error(r, 500, "cannot create the file");
        return;
    }

    // What came with the head, then the rest.
    u64 left = r->contentLength;
    u32 early = r->have - r->headLen;
    if(early > left) {
        early = (u32) left;
    }

    bool ok = early == 0 || fwrite(r->head + r->headLen, 1, early, f) == early;
    left -= early;
    while(ok && left > 0) {
        if(!web_wait_fd(r->fd, POLLIN, WEB_IDLE_MS)) {
            ok = false;
            break;
        }

        ssize_t n = recv(r->fd, web_io, left > WEB_IO_SIZE ? WEB_IO_SIZE : (size_t) left, 0);
        if(n <= 0 || fwrite(web_io, 1, (size_t) n, f) != (size_t) n) {
            ok = false;
            break;
        }

        left -= (u64) n;
    }

    if(fclose(f) != 0) {
        ok = false;
    }

    if(ok) {
        remove(rpath);
        ok = rename(part, rpath) == 0;
    }

    if(!ok) {
        remove(part);
        web_send_error(r, 500, "the upload failed");
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
        res = S_ISDIR(st.st_mode) ? FSUSER_DeleteDirectoryRecursively(sd, *fsPath) : FSUSER_DeleteFile(sd, *fsPath);
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

static void web_send_romfs(web_request* r, const char* path, const char* type) {
    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        web_send_text(r, 404, "text/plain", "missing from this build");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(size < 0 || !web_send_head(r, 200, type, size, NULL)) {
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

// One title's details: name, publisher, product code and which kinds of data it has.
static void web_api_title(web_request* r) {
    title_info title;
    if(!web_parse_title(r, &title)) {
        return;
    }

    char name[0x100] = "";
    char publisher[0x100] = "";
    SMDH* smdh = web_read_smdh(&title);
    if(smdh != NULL) {
        SMDH_title* names = smdh_select_title(smdh);
        utf16_to_utf8((uint8_t*) name, names->shortDescription, sizeof(name) - 1);
        utf16_to_utf8((uint8_t*) publisher, names->publisher, sizeof(publisher) - 1);
        free(smdh);
    }

    char code[0x10] = "";
    AM_GetTitleProductCode(title.mediaType, title.titleId, code);

    bool game = title_kind_of(title.titleId) == TITLE_KIND_GAME || title_kind_of(title.titleId) == TITLE_KIND_DEMO;
    u32 extId = 0;
    bool save = game && action_save_data_exists(&title, SAVE_KIND_SAVE, &extId);
    bool ext = game && action_save_data_exists(&title, SAVE_KIND_EXT, &extId);
    bool boss = game && action_save_data_exists(&title, SAVE_KIND_BOSS, &extId);

    web_send_json(r, 200, json_pack("{s:b,s:s,s:s,s:s,s:b,s:b,s:b}", "ok", 1, "name", name, "publisher", publisher, "code", code, "save", save, "ext", ext, "boss", boss));
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

    char path[WEB_PATH_MAX];
    u64 size = 0;
    Result res = action_save_pack_sync(&title, name, kind, extId, path, sizeof(path), &size);
    if(R_FAILED(res)) {
        char text[48];
        snprintf(text, sizeof(text), "packing failed (%08lX)", res);
        web_send_error(r, 500, text);
        return;
    }

    web_send_json(r, 200, json_pack("{s:b,s:s,s:I}", "ok", 1, "path", path, "size", (json_int_t) size));
}

// A package the page uploaded: the console takes it as if it had been sent, and asks there what to do.
static char web_receive_path[WEB_PATH_MAX];
static volatile bool web_receive_pending = false;
static bool web_receive_import = false;

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

    if(web_receive_pending) {
        web_send_error(r, 409, "the console is still asking about the last one");
        return;
    }

    string_copy(web_receive_path, vpath, sizeof(web_receive_path));
    char want[8] = "";
    web_query_get(r->query, "import", want, sizeof(want));
    web_receive_import = strcmp(want, "1") == 0 && action_save_package_can_import(id, kind);
    web_receive_pending = true;
    web_send_json(r, 200, json_pack("{s:b,s:b}", "ok", 1, "import", web_receive_import));
}

// ---- serving ----

static void web_handle(web_request* r) {
    if(!web_read_head(r)) {
        return;
    }

    web_parse_head(r);

    if(strcmp(r->path, "/") == 0 || strcmp(r->path, "/index.html") == 0 || strcmp(r->path, "/theme.json") == 0 || strcmp(r->path, "/font.ttf") == 0) {
        if(strcmp(r->method, "GET") == 0) {
            if(strcmp(r->path, "/theme.json") == 0) {
                web_send_theme(r);
            } else if(strcmp(r->path, "/font.ttf") == 0) {
                web_send_romfs(r, "romfs:/web/PixelifySans.ttf", "font/ttf");
            } else {
                web_send_romfs(r, "romfs:/web/index.html", "text/html; charset=utf-8");
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
    } else if(post && strcmp(api, "install") == 0) {
        web_api_install(r);
    } else if(get && strcmp(api, "titles") == 0) {
        web_api_titles(r);
    } else if(get && strcmp(api, "title") == 0) {
        web_api_title(r);
    } else if(get && strcmp(api, "icon") == 0) {
        web_api_icon(r);
    } else if(post && strcmp(api, "save/pack") == 0) {
        web_api_save_pack(r);
    } else if(post && strcmp(api, "save/receive") == 0) {
        web_api_save_receive(r);
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

        web_client = fd;
        r->fd = fd;
        web_handle(r);
        web_close_socket(&fd);
        web_client = -1;
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

    web_thread = threadCreate(web_main, NULL, 0x14000, 0x19, 1, false);
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
    web_pin[0] = '\0';
    snprintf(web_status, sizeof(web_status), "%s", _("Off"));
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

void webserver_poll_main() {
    // A package uploaded by the page: unpacked (and imported, behind the red question) like one that was sent.
    if(web_receive_pending) {
        action_save_package_received(web_receive_path, web_receive_import);
        web_receive_pending = false;
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
