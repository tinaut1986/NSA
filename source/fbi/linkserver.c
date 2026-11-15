#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <3ds.h>

#include "linkserver.h"
#include "action/action.h"
#include "../core/core.h"
#include "../core/netpoll.h"

// See linkserver.h. One thread serves the HTTP port and answers mDNS; what came is asked about on the main thread.

#define LINK_LINKS_MAX 20
#define LINK_URLS_SIZE 4096
#define LINK_HEAD_MAX 8192
#define LINK_BODY_MAX 16384
#define LINK_IDLE_MS 5000

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define MDNS_NAMES 2
#define MDNS_NAME_MAX 32
#define MDNS_TTL 120

static volatile bool link_run = false;
static int link_listen = -1;
static int link_mdns = -1;
static int link_client = -1;
static Thread link_thread = NULL;
static u32 link_ip = 0;   // Network order.
static char link_status[64] = "";

static bool link_retry = false;
static bool link_paused = false;
static u64 link_retry_next = 0;
static u64 link_retry_end = 0;
static u64 link_watch_next = 0;

// The names this console answers for ("3ds.local", "nsa.local"), found free when the server started.
static char link_names[MDNS_NAMES][MDNS_NAME_MAX];
static u32 link_name_count = 0;

// Links for the main thread: written by the server thread while `state` is 0, then 1; 2 while the console asks about
// them and gets them (one question at a time); 0 again once that is over.
static struct {
    volatile u32 state;
    char urls[LINK_URLS_SIZE];   // One a line.
    u32 count;
    char from[16];               // The device that sent them.
    char host[64];               // The site of the first one.
} link_job;

// DEBUG builds: what the mDNS side sees, in sdmc:/nsa/link-log.txt (for testing on a console).
static void link_log(const char* format, ...) {
    if(!NSA_DEBUG) {
        return;
    }

    FILE* f = fopen("sdmc:" APP_DATA_DIR "link-log.txt", "ab");
    if(f == NULL) {
        return;
    }

    char line[200];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fprintf(f, "[%llu] %s\n", (unsigned long long) osGetTime(), line);
    fclose(f);
}

static void link_close_socket(int* fd) {
    if(*fd >= 0) {
        shutdown(*fd, SHUT_RDWR);
        close(*fd);
        *fd = -1;
    }
}

// Napping until `fd` is ready (no thread waits inside poll(): netpoll.h). False after `timeoutMs` (0: none) or a stop.
static bool link_wait_fd(int fd, short events, int timeoutMs) {
    int waited = 0;
    while(link_run) {
        struct pollfd p = {.fd = fd, .events = events, .revents = 0};
        int r = poll(&p, 1, 0);
        if(r > 0) {
            return (p.revents & (events | POLLHUP | POLLERR)) != 0;
        }

        if(r < 0) {
            return false;
        }

        int nap = (int) net_nap_ms((u32) waited);
        svcSleepThread((s64) nap * 1000000);
        waited += nap;
        if(timeoutMs > 0 && waited >= timeoutMs) {
            return false;
        }
    }

    return false;
}

static bool link_send_all(int fd, const char* data, size_t len) {
    while(len > 0) {
        if(!link_wait_fd(fd, POLLOUT, LINK_IDLE_MS)) {
            return false;
        }

        ssize_t n = send(fd, data, len, 0);
        if(n < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }

            return false;
        }

        data += n;
        len -= (size_t) n;
    }

    return true;
}

static void link_respond(int fd, int code, const char* status, const char* type, const char* extra, const char* body) {
    char head[512];
    size_t len = body != NULL ? strlen(body) : 0;
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\n%sConnection: close\r\n\r\n",
                     code, status, type, (unsigned) len, extra != NULL ? extra : "");
    if(n > 0 && link_send_all(fd, head, (size_t) n) && len > 0) {
        link_send_all(fd, body, len);
    }
}

// ---- forms ----

static int link_hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// %XX and '+' decoded, in place.
static void link_url_decode(char* s) {
    char* out = s;
    for(char* in = s; *in != '\0'; in++) {
        if(*in == '+') {
            *out++ = ' ';
        } else if(*in == '%' && link_hex(in[1]) >= 0 && link_hex(in[2]) >= 0) {
            *out++ = (char) (link_hex(in[1]) * 16 + link_hex(in[2]));
            in += 2;
        } else {
            *out++ = *in;
        }
    }

    *out = '\0';
}

// The value of `name` in a form ("a=1&links=..."), decoded into `out`; false when it is not there.
static bool link_form_value(const char* form, const char* name, char* out, size_t size) {
    size_t nameLen = strlen(name);
    const char* p = form;
    while(p != NULL && *p != '\0') {
        const char* end = strchr(p, '&');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);
        if(len > nameLen && strncmp(p, name, nameLen) == 0 && p[nameLen] == '=') {
            size_t valueLen = len - nameLen - 1;
            if(valueLen >= size) {
                valueLen = size - 1;
            }

            memcpy(out, p + nameLen + 1, valueLen);
            out[valueLen] = '\0';
            link_url_decode(out);
            return true;
        }

        p = end != NULL ? end + 1 : NULL;
    }

    return false;
}

// The http(s) links in `text`, in order and without repeats, one a line into `out`. How many.
static u32 link_extract(const char* text, char* out, size_t size) {
    u32 count = 0;
    size_t used = 0;
    out[0] = '\0';
    const char* p = text;
    while(*p != '\0' && count < LINK_LINKS_MAX) {
        const char* http = strstr(p, "http");
        if(http == NULL) {
            break;
        }

        bool scheme = strncasecmp(http, "http://", 7) == 0 || strncasecmp(http, "https://", 8) == 0;
        // Only where a link can start: the beginning, or after a space, a quote or a bracket.
        bool start = http == text || isspace((unsigned char) http[-1]) || strchr("\"'<([,;", http[-1]) != NULL;
        if(!scheme || !start) {
            p = http + 4;
            continue;
        }

        size_t len = 0;
        while(http[len] != '\0' && !isspace((unsigned char) http[len]) && strchr("\"'<>", http[len]) == NULL) {
            len++;
        }

        // Ends of sentences are not part of it.
        while(len > 0 && strchr(".,;)]", http[len - 1]) != NULL) {
            len--;
        }

        p = http + len;
        if(len < 10 || used + len + 2 > size) {
            continue;
        }

        // Not twice.
        bool seen = false;
        for(const char* line = out; *line != '\0' && !seen; ) {
            const char* end = strchr(line, '\n');
            size_t lineLen = end != NULL ? (size_t) (end - line) : strlen(line);
            seen = lineLen == len && strncmp(line, http, len) == 0;
            line = end != NULL ? end + 1 : line + lineLen;
        }

        if(seen) {
            continue;
        }

        memcpy(out + used, http, len);
        used += len;
        out[used++] = '\n';
        out[used] = '\0';
        count++;
    }

    if(used > 0) {
        out[used - 1] = '\0';
    }

    return count;
}

// The site of a link: "store.brewology.com".
static void link_host_of(const char* url, char* out, size_t size) {
    const char* start = strstr(url, "://");
    start = start != NULL ? start + 3 : url;
    const char* at = start;
    size_t len = 0;
    while(at[len] != '\0' && at[len] != '/' && at[len] != '?' && at[len] != '#' && at[len] != '\n') {
        len++;
    }

    // user:password@ is not the site.
    for(size_t i = 0; i < len; i++) {
        if(at[i] == '@') {
            start = at + i + 1;
            len -= i + 1;
            at = start;
            i = 0;
        }
    }

    if(len >= size) {
        len = size - 1;
    }

    memcpy(out, start, len);
    out[len] = '\0';
}

// ---- HTTP ----

// The page, in the console's language (i18n_translate_as only reads the tables: fine on this thread).
// Three states: the form; "sent", on a page of its own so that it can't be taken for the form; "busy".

static const char* link_page_style =
        "<style>body{font-family:system-ui,sans-serif;max-width:32rem;margin:0 auto;padding:2.5rem 1.2rem;background:#16181d;"
        "color:#e8e8ec;line-height:1.45}h1{font-size:1.45rem;margin:.2rem 0 .6rem}p{margin:.4rem 0 1rem}"
        "textarea{width:100%;height:9rem;box-sizing:border-box;background:#22252c;color:#e8e8ec;border:1px solid #3a3e48;"
        "border-radius:10px;padding:.7rem;font-size:1rem}.btn{display:inline-block;margin-top:.8rem;padding:.75rem 1.6rem;"
        "border:0;border-radius:10px;background:#7b74e0;color:#fff;font-size:1.05rem;text-decoration:none;cursor:pointer}"
        ".btn.alt{background:#2c3039;color:#e8e8ec}.err{background:#3a2328;color:#ffb4b4;border-radius:10px;padding:.7rem 1rem}"
        ".mark{width:4.5rem;height:4.5rem;border-radius:50%;display:flex;align-items:center;justify-content:center;"
        "font-size:2.6rem;margin:1.5rem 0 1rem}.ok{background:#2f8f5b}.wait{background:#a7792a}.center{text-align:center}"
        ".center .mark{margin-left:auto;margin-right:auto}.brand{color:#9a9ca6;font-size:.85rem;letter-spacing:.04em}</style>";

// `text` with & < > " made safe for HTML, appended to `out`.
static void link_html_add(char* out, size_t size, const char* text) {
    size_t used = strlen(out);
    for(const char* c = text; *c != '\0' && used + 7 < size; c++) {
        const char* entity = *c == '&' ? "&amp;" : *c == '<' ? "&lt;" : *c == '>' ? "&gt;" : *c == '"' ? "&quot;" : NULL;
        if(entity != NULL) {
            size_t len = strlen(entity);
            memcpy(out + used, entity, len);
            used += len;
        } else {
            out[used++] = *c;
        }
    }

    out[used] = '\0';
}

static void link_raw_add(char* out, size_t size, const char* text) {
    size_t used = strlen(out);
    snprintf(out + used, size - used, "%s", text);
}

static void link_send_page(int fd, const char* query) {
    language_id lang = i18n_effective_language();
    #define T(text) i18n_translate_as(lang, text)

    char sent[8] = "";
    char busy[4] = "";
    link_form_value(query, "sent", sent, sizeof(sent));
    link_form_value(query, "busy", busy, sizeof(busy));

    size_t size = 8192;
    char* page = (char*) malloc(size);
    if(page == NULL) {
        link_respond(fd, 500, "Internal Server Error", "text/plain", NULL, "Out of memory\n");
        return;
    }

    const char* code = i18n_language_code(lang);
    snprintf(page, size, "<!doctype html><html lang=\"%s\"><head><meta charset=\"utf-8\">"
                         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>NSA</title>%s</head><body>",
             strcmp(code, "tw") == 0 ? "zh-Hant" : strcmp(code, "zh") == 0 ? "zh-Hans" : strcmp(code, "system") == 0 ? "en" : code,
             link_page_style);

    int count = atoi(sent);
    if(busy[0] != '\0') {
        link_raw_add(page, size, "<div class=\"center\"><div class=\"mark wait\">&#8987;</div><h1>");
        link_html_add(page, size, T("The 3DS is busy"));
        link_raw_add(page, size, "</h1><p>");
        link_html_add(page, size, T("It is still with the last links. Send these again once it has finished."));
        link_raw_add(page, size, "</p><a class=\"btn alt\" href=\"/\">");
        link_html_add(page, size, T("Back"));
        link_raw_add(page, size, "</a></div>");
    } else if(count > 0) {
        char line[256];
        if(count == 1) {
            snprintf(line, sizeof(line), "%s", T("Answer on the console to download it."));
        } else {
            snprintf(line, sizeof(line), T("%lu links. Answer on the console to download them."), (unsigned long) count);
        }

        link_raw_add(page, size, "<div class=\"center\"><div class=\"mark ok\">&#10003;</div><h1>");
        link_html_add(page, size, T("Sent to your 3DS"));
        link_raw_add(page, size, "</h1><p>");
        link_html_add(page, size, line);
        link_raw_add(page, size, "</p><a class=\"btn alt\" href=\"/\">");
        link_html_add(page, size, T("Send other links"));
        link_raw_add(page, size, "</a></div>");
    } else {
        link_raw_add(page, size, "<div class=\"brand\">NSA</div><h1>");
        link_html_add(page, size, T("Send to this 3DS"));
        link_raw_add(page, size, "</h1>");
        if(sent[0] != '\0') {
            link_raw_add(page, size, "<p class=\"err\">");
            link_html_add(page, size, T("No links were found in what you sent."));
            link_raw_add(page, size, "</p>");
        }

        link_raw_add(page, size, "<form method=\"post\" action=\"/send\"><p>");
        link_html_add(page, size, T("Paste links to files, one a line (at most 20):"));
        link_raw_add(page, size, "</p><textarea name=\"links\" placeholder=\"https://example.com/app.cia\"></textarea><br><button class=\"btn\">");
        link_html_add(page, size, T("Send"));
        link_raw_add(page, size, "</button></form>");
    }

    link_raw_add(page, size, "</body></html>");
    link_respond(fd, 200, "OK", "text/html; charset=utf-8", NULL, page);
    free(page);
    #undef T
}

// A header's value ("" when it is not there), from the request's head (its lines end with \r\n).
static void link_header(const char* head, const char* name, char* out, size_t size) {
    out[0] = '\0';
    size_t nameLen = strlen(name);
    const char* line = strstr(head, "\r\n");
    while(line != NULL) {
        line += 2;
        if(strncasecmp(line, name, nameLen) == 0 && line[nameLen] == ':') {
            const char* v = line + nameLen + 1;
            while(*v == ' ' || *v == '\t') {
                v++;
            }

            const char* end = strstr(v, "\r\n");
            size_t len = end != NULL ? (size_t) (end - v) : strlen(v);
            if(len >= size) {
                len = size - 1;
            }

            memcpy(out, v, len);
            out[len] = '\0';
            return;
        }

        line = strstr(line, "\r\n");
    }
}

static void link_take(int fd, const char* form, bool plain, const char* from, bool page) {
    char* text = (char*) malloc(LINK_BODY_MAX);
    char* urls = (char*) malloc(LINK_URLS_SIZE);
    if(text == NULL || urls == NULL) {
        free(text);
        free(urls);
        link_respond(fd, 500, "Internal Server Error", "text/plain", NULL, "Out of memory\n");
        return;
    }

    // A form's "links", or a plain body that is the links themselves.
    if(!link_form_value(form, "links", text, LINK_BODY_MAX)) {
        snprintf(text, LINK_BODY_MAX, "%s", plain ? form : "");
    }

    u32 count = link_extract(text, urls, LINK_URLS_SIZE);
    free(text);

    if(count == 0) {
        free(urls);
        if(page) {
            link_respond(fd, 400, "Bad Request", "application/json", "Access-Control-Allow-Origin: *\r\n", "{\"error\":\"No http(s) links found.\"}");
        } else {
            link_respond(fd, 303, "See Other", "text/plain", "Location: /?sent=0\r\n", NULL);
        }
        return;
    }

    // The last ones still asked about or downloading: one at a time.
    if(link_job.state != 0) {
        free(urls);
        if(page) {
            link_respond(fd, 409, "Conflict", "application/json", "Access-Control-Allow-Origin: *\r\n",
                         "{\"error\":\"The 3DS is still busy with the last links.\"}");
        } else {
            link_respond(fd, 303, "See Other", "text/plain", "Location: /?busy=1\r\n", NULL);
        }
        return;
    }

    snprintf(link_job.urls, sizeof(link_job.urls), "%s", urls);
    link_job.count = count;
    snprintf(link_job.from, sizeof(link_job.from), "%s", from);
    link_host_of(urls, link_job.host, sizeof(link_job.host));
    free(urls);
    __dsb();
    link_job.state = 1;

    if(page) {
        char body[32];
        snprintf(body, sizeof(body), "{\"count\":%lu}", (unsigned long) count);
        link_respond(fd, 200, "OK", "application/json", "Access-Control-Allow-Origin: *\r\n", body);
    } else {
        // Back to the page, so that reloading it does not send again.
        char location[48];
        snprintf(location, sizeof(location), "Location: /?sent=%lu\r\n", (unsigned long) count);
        link_respond(fd, 303, "See Other", "text/plain", location, NULL);
    }
}

static void link_handle(int fd, const struct sockaddr_in* peer) {
    char* buf = (char*) malloc(LINK_HEAD_MAX + LINK_BODY_MAX + 1);
    if(buf == NULL) {
        return;
    }

    // The head, and what came after it.
    size_t have = 0;
    char* headEnd = NULL;
    while(headEnd == NULL && have < LINK_HEAD_MAX) {
        if(!link_wait_fd(fd, POLLIN, LINK_IDLE_MS)) {
            free(buf);
            return;
        }

        ssize_t n = recv(fd, buf + have, LINK_HEAD_MAX - have, 0);
        if(n <= 0) {
            if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }

            free(buf);
            return;
        }

        have += (size_t) n;
        buf[have] = '\0';
        headEnd = strstr(buf, "\r\n\r\n");
    }

    if(headEnd == NULL) {
        link_respond(fd, 431, "Request Header Fields Too Large", "text/plain", NULL, "Too large\n");
        free(buf);
        return;
    }

    *headEnd = '\0';
    char* body = headEnd + 4;
    size_t bodyHave = have - (size_t) (body - buf);

    // "GET /send?links=... HTTP/1.1"
    char method[8] = "";
    char target[LINK_HEAD_MAX / 2];
    if(sscanf(buf, "%7s %4095s", method, target) != 2) {
        link_respond(fd, 400, "Bad Request", "text/plain", NULL, "Bad request\n");
        free(buf);
        return;
    }

    char* query = strchr(target, '?');
    if(query != NULL) {
        *query++ = '\0';
    } else {
        query = target + strlen(target);
    }

    char value[64];
    link_header(buf, "Content-Length", value, sizeof(value));
    size_t length = value[0] != '\0' ? (size_t) strtoul(value, NULL, 10) : 0;
    link_header(buf, "X-Requested-With", value, sizeof(value));
    bool page = value[0] != '\0';
    link_header(buf, "Content-Type", value, sizeof(value));
    bool plain = strncasecmp(value, "text/plain", 10) == 0 || value[0] == '\0';

    char from[16];
    snprintf(from, sizeof(from), "%s", inet_ntoa(peer->sin_addr));

    bool get = strcmp(method, "GET") == 0;
    bool post = strcmp(method, "POST") == 0;
    if(strcmp(target, "/send") == 0 && (get || post)) {
        if(post) {
            if(length > LINK_BODY_MAX) {
                link_respond(fd, 413, "Payload Too Large", "text/plain", NULL, "Too large\n");
                free(buf);
                return;
            }

            // The rest of the body.
            memmove(buf, body, bodyHave);
            while(bodyHave < length) {
                if(!link_wait_fd(fd, POLLIN, LINK_IDLE_MS)) {
                    free(buf);
                    return;
                }

                ssize_t n = recv(fd, buf + bodyHave, length - bodyHave, 0);
                if(n <= 0) {
                    if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }

                    free(buf);
                    return;
                }

                bodyHave += (size_t) n;
            }

            buf[length] = '\0';
            link_take(fd, buf, plain, from, page);
        } else {
            link_take(fd, query, false, from, page);
        }
    } else if(strcmp(target, "/") == 0 && get) {
        link_send_page(fd, query);
    } else if(strcmp(method, "OPTIONS") == 0) {
        link_respond(fd, 204, "No Content", "text/plain",
                     "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST\r\nAccess-Control-Allow-Headers: X-Requested-With, Content-Type\r\n", NULL);
    } else if(!get && !post) {
        link_respond(fd, 405, "Method Not Allowed", "text/plain", NULL, "Method not allowed\n");
    } else {
        link_respond(fd, 404, "Not Found", "text/plain", NULL, "Not found\n");
    }

    free(buf);
}

// ---- mDNS (RFC 6762): answering for our names ----

// The name at `at` in a packet, dotted and in lower case; where the name ends (0 when it is broken).
static size_t mdns_read_name(const u8* packet, size_t size, size_t at, char* out, size_t outSize) {
    size_t used = 0;
    size_t end = 0;
    u32 jumps = 0;
    out[0] = '\0';
    while(at < size) {
        u8 len = packet[at];
        if(len == 0) {
            return end != 0 ? end : at + 1;
        }

        if((len & 0xC0) == 0xC0) {
            if(at + 1 >= size || ++jumps > 8) {
                return 0;
            }

            if(end == 0) {
                end = at + 2;
            }

            at = (size_t) (((len & 0x3F) << 8) | packet[at + 1]);
            continue;
        }

        if(at + 1 + len > size || used + len + 2 > outSize) {
            return 0;
        }

        if(used > 0) {
            out[used++] = '.';
        }

        for(u8 i = 0; i < len; i++) {
            out[used++] = (char) tolower(packet[at + 1 + i]);
        }

        out[used] = '\0';
        at += 1 + len;
    }

    return 0;
}

static size_t mdns_write_name(u8* out, const char* name) {
    size_t used = 0;
    const char* p = name;
    while(*p != '\0') {
        const char* dot = strchr(p, '.');
        size_t len = dot != NULL ? (size_t) (dot - p) : strlen(p);
        out[used++] = (u8) len;
        memcpy(out + used, p, len);
        used += len;
        p += len;
        if(*p == '.') {
            p++;
        }
    }

    out[used++] = 0;
    return used;
}

static void mdns_put16(u8* p, u16 v) {
    p[0] = (u8) (v >> 8);
    p[1] = (u8) v;
}

static u16 mdns_get16(const u8* p) {
    return (u16) ((p[0] << 8) | p[1]);
}

// Our A record for `name`. `legacy`: a plain DNS client (asked from another port), which wants its question back.
static size_t mdns_answer(u8* out, const char* name, u16 id, bool legacy) {
    memset(out, 0, 12);
    mdns_put16(out, legacy ? id : 0);
    mdns_put16(out + 2, 0x8400);   // A response, authoritative.
    mdns_put16(out + 4, legacy ? 1 : 0);
    mdns_put16(out + 6, 1);
    size_t used = 12;
    if(legacy) {
        used += mdns_write_name(out + used, name);
        mdns_put16(out + used, 1);
        mdns_put16(out + used + 2, 1);
        used += 4;
    }

    used += mdns_write_name(out + used, name);
    mdns_put16(out + used, 1);                         // A
    mdns_put16(out + used + 2, legacy ? 1 : 0x8001);   // IN, and the cache flush bit: the name is ours alone.
    u32 ttl = legacy ? 10 : MDNS_TTL;
    out[used + 4] = (u8) (ttl >> 24);
    out[used + 5] = (u8) (ttl >> 16);
    out[used + 6] = (u8) (ttl >> 8);
    out[used + 7] = (u8) ttl;
    mdns_put16(out + used + 8, 4);
    memcpy(out + used + 10, &link_ip, 4);
    return used + 14;
}

static void mdns_send(const u8* data, size_t len, const struct sockaddr_in* to) {
    sendto(link_mdns, data, len, 0, (const struct sockaddr*) to, sizeof(*to));
}

static struct sockaddr_in mdns_group_address() {
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(MDNS_PORT);
    to.sin_addr.s_addr = inet_addr(MDNS_GROUP);
    return to;
}

// One packet that came: its questions about our names answered; whether it answers `probing` for another device.
static bool mdns_packet(const u8* packet, size_t size, const struct sockaddr_in* from, const char* probing) {
    if(size < 12) {
        return false;
    }

    u16 id = mdns_get16(packet);
    u16 flags = mdns_get16(packet + 2);
    u16 questions = mdns_get16(packet + 4);
    u16 answers = mdns_get16(packet + 6);
    size_t at = 12;
    char name[128];

    if(flags & 0x8000) {
        // A response: does someone else hold the name being probed?
        if(probing == NULL || from->sin_addr.s_addr == link_ip) {
            return false;
        }

        for(u16 q = 0; q < questions && at != 0; q++) {
            at = mdns_read_name(packet, size, at, name, sizeof(name));
            at = at != 0 && at + 4 <= size ? at + 4 : 0;
        }

        for(u16 a = 0; a < answers && at != 0; a++) {
            at = mdns_read_name(packet, size, at, name, sizeof(name));
            if(at == 0 || at + 10 > size) {
                return false;
            }

            if(strcmp(name, probing) == 0) {
                return true;
            }

            at += 10 + mdns_get16(packet + at + 8);
        }

        return false;
    }

    bool legacy = ntohs(from->sin_port) != MDNS_PORT;
    for(u16 q = 0; q < questions; q++) {
        at = mdns_read_name(packet, size, at, name, sizeof(name));
        if(at == 0 || at + 4 > size) {
            return false;
        }

        u16 type = mdns_get16(packet + at);
        u16 klass = mdns_get16(packet + at + 2);
        at += 4;
        if((type != 1 && type != 255) || (klass & 0x7FFF) != 1) {
            continue;
        }

        for(u32 i = 0; i < link_name_count; i++) {
            if(strcmp(name, link_names[i]) != 0) {
                continue;
            }

            u8 reply[256];
            size_t len = mdns_answer(reply, link_names[i], id, legacy);
            if(legacy || (klass & 0x8000) != 0) {
                // A plain DNS client, or one that asked for a unicast answer.
                mdns_send(reply, len, from);
            } else {
                struct sockaddr_in group = mdns_group_address();
                mdns_send(reply, len, &group);
            }
        }
    }

    return false;
}

static void mdns_read_all(const char* probing, bool* taken) {
    u8 packet[1500];
    while(true) {
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(link_mdns, packet, sizeof(packet), 0, (struct sockaddr*) &from, &fromLen);
        if(n <= 0) {
            return;
        }

        if(mdns_packet(packet, (size_t) n, &from, probing) && taken != NULL) {
            *taken = true;
        }
    }
}

// Whether another device answers for `name`: three probes, 250 ms apart (RFC 6762, 8.1).
static bool mdns_taken(const char* name) {
    u8 probe[128];
    memset(probe, 0, 12);
    mdns_put16(probe + 4, 1);
    size_t len = 12 + mdns_write_name(probe + 12, name);
    mdns_put16(probe + len, 255);      // ANY
    mdns_put16(probe + len + 2, 0x8001);   // IN, a unicast answer welcome.
    len += 4;

    struct sockaddr_in group = mdns_group_address();
    bool taken = false;
    for(u32 round = 0; round < 3 && !taken && link_run; round++) {
        mdns_send(probe, len, &group);
        for(u32 waited = 0; waited < 250 && !taken; waited += 25) {
            svcSleepThread(25 * 1000000LL);
            mdns_read_all(name, &taken);
        }
    }

    return taken;
}

// The names, each the first free of "x.local", "x-2.local", "x-3.local"; then said to the network.
static void mdns_claim_names() {
    static const char* const bases[MDNS_NAMES] = {"3ds", "nsa"};
    link_name_count = 0;
    for(u32 b = 0; b < MDNS_NAMES && link_run; b++) {
        for(u32 n = 1; n <= 3 && link_run; n++) {
            char name[MDNS_NAME_MAX];
            if(n == 1) {
                snprintf(name, sizeof(name), "%s.local", bases[b]);
            } else {
                snprintf(name, sizeof(name), "%s-%lu.local", bases[b], (unsigned long) n);
            }

            if(!mdns_taken(name)) {
                snprintf(link_names[link_name_count++], MDNS_NAME_MAX, "%s", name);
                link_log("claimed %s", name);
                break;
            }
        }
    }

    // Announced (twice, a second apart), so that caches holding another device's address for a name drop it.
    struct sockaddr_in group = mdns_group_address();
    for(u32 round = 0; round < 2 && link_run; round++) {
        for(u32 i = 0; i < link_name_count; i++) {
            u8 reply[256];
            mdns_send(reply, mdns_answer(reply, link_names[i], 0, false), &group);
        }

        if(round == 0) {
            for(u32 waited = 0; waited < 1000 && link_run; waited += 50) {
                svcSleepThread(50 * 1000000LL);
                mdns_read_all(NULL, NULL);
            }
        }
    }
}

static bool mdns_open() {
    link_mdns = socket(AF_INET, SOCK_DGRAM, 0);
    if(link_mdns < 0) {
        return false;
    }

    int yes = 1;
    setsockopt(link_mdns, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if(bind(link_mdns, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
        link_log("mdns bind failed: %d", errno);
        link_close_socket(&link_mdns);
        return false;
    }

    struct ip_mreq group;
    group.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    group.imr_interface.s_addr = htonl(INADDR_ANY);   // As 3dsDL: the console's own address there is not taken.
    if(setsockopt(link_mdns, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) != 0) {
        link_log("mdns membership failed: %d", errno);
        link_close_socket(&link_mdns);
        return false;
    }

    u8 ttl = 255;
    int ttlRes = setsockopt(link_mdns, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    link_log("mdns open, ttl %d", ttlRes);
    net_set_nonblocking(link_mdns);
    return true;
}

// ---- the thread ----

static void link_main(void* arg) {
    // Without mDNS the page still works at the console's address.
    if(link_mdns >= 0) {
        mdns_claim_names();
    }

    while(link_run) {
        struct pollfd fds[2] = {{.fd = link_listen, .events = POLLIN, .revents = 0}, {.fd = link_mdns, .events = POLLIN, .revents = 0}};
        int r = poll(fds, link_mdns >= 0 ? 2 : 1, 0);
        if(r <= 0) {
            svcSleepThread((s64) net_nap_ms(1000) * 1000000);
            continue;
        }

        if(link_mdns >= 0 && (fds[1].revents & POLLIN)) {
            mdns_read_all(NULL, NULL);
        }

        if(fds[0].revents & POLLIN) {
            struct sockaddr_in peer;
            socklen_t len = sizeof(peer);
            int fd = accept(link_listen, (struct sockaddr*) &peer, &len);
            if(fd >= 0) {
                net_set_nonblocking(fd);
                link_client = fd;
                link_handle(fd, &peer);
                link_client = -1;
                net_close_gently(fd);
            }
        }
    }
}

static void link_shutdown() {
    link_run = false;
    int fd = link_client;
    if(fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }

    if(link_thread != NULL) {
        threadJoin(link_thread, U64_MAX);
        threadFree(link_thread);
        link_thread = NULL;
    }

    link_close_socket(&link_listen);
    link_close_socket(&link_mdns);
    link_name_count = 0;
    snprintf(link_status, sizeof(link_status), "%s", _("Off"));
}

bool linkserver_start() {
    if(link_run) {
        return true;
    }

    link_paused = false;
    u32 wifi = 0;
    if(R_FAILED(ACU_GetWifiStatus(&wifi)) || wifi == 0) {
        snprintf(link_status, sizeof(link_status), "%s", _("No Wi-Fi"));
        return false;
    }

    link_ip = (u32) gethostid();
    link_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(link_listen >= 0) {
        int yes = 1;
        setsockopt(link_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        net_set_nonblocking(link_listen);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LINK_SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if(link_listen < 0 || bind(link_listen, (struct sockaddr*) &addr, sizeof(addr)) != 0 || listen(link_listen, 4) != 0) {
        snprintf(link_status, sizeof(link_status), _("Port %d busy"), LINK_SERVER_PORT);
        link_close_socket(&link_listen);
        return false;
    }

    link_run = true;
    mdns_open();
    link_thread = net_server_thread(link_main, 0x8000);
    if(link_thread == NULL) {
        link_shutdown();
        snprintf(link_status, sizeof(link_status), "%s", _("No thread"));
        return false;
    }

    return true;
}

void linkserver_start_when_online() {
    if(linkserver_start()) {
        return;
    }

    link_retry = true;
    link_retry_next = osGetTime() + 2000;
    link_retry_end = U64_MAX;
}

bool linkserver_waiting() {
    return link_retry && !link_run;
}

void linkserver_stop() {
    link_retry = false;
    link_paused = false;
    link_shutdown();
}

bool linkserver_running() {
    return link_run;
}

void linkserver_pause() {
    if(link_run || link_retry) {
        link_retry = false;
        link_shutdown();
        link_paused = true;
    }
}

void linkserver_resume_later() {
    if(link_paused) {
        link_paused = false;
        link_retry = true;
        link_retry_next = osGetTime() + 3000;
        link_retry_end = osGetTime() + 120000;
    }
}

// The links' question answered and, if they were wanted, got (or stopped): the next ones may come.
static void link_finished(void* data) {
    link_job.state = 0;
}

void linkserver_poll_main() {
    // The Wi-Fi gone: stopped, and started again once it is back (as the web server).
    if(link_run && !link_paused && osGetTime() >= link_watch_next) {
        link_watch_next = osGetTime() + 2000;
        u32 wifi = 0;
        if(R_FAILED(ACU_GetWifiStatus(&wifi)) || wifi == 0) {
            link_shutdown();
            link_retry = true;
            link_retry_next = osGetTime() + 2000;
            link_retry_end = U64_MAX;
        }
    }

    if(link_retry && osGetTime() >= link_retry_next) {
        if(linkserver_start() || osGetTime() > link_retry_end) {
            link_retry = false;
        } else {
            link_retry_next = osGetTime() + 2000;
        }
    }

    // Links that came: Remote install's question, which says what they are and where they come from.
    if(link_job.state == 1) {
        __dsb();
        static char question[512];
        if(link_job.count == 1) {
            snprintf(question, sizeof(question), _("A device on your network (%s) sent a link from %s. Download it?"), link_job.from, link_job.host);
        } else {
            snprintf(question, sizeof(question), _("A device on your network (%s) sent %lu links, the first from %s. Download them?"), link_job.from,
                     (unsigned long) link_job.count, link_job.host);
        }

        static char urls[LINK_URLS_SIZE];
        snprintf(urls, sizeof(urls), "%s", link_job.urls);
        link_job.state = 2;
        action_install_url_remote(question, urls, NULL, link_finished);
    }
}

void linkserver_address(char* out, size_t size) {
    if(!link_run) {
        snprintf(out, size, "%s", link_status[0] != '\0' ? link_status : _("Off"));
        return;
    }

    struct in_addr addr = {.s_addr = link_ip};
    size_t used = 0;
    out[0] = '\0';
    for(u32 i = 0; i < link_name_count && used < size; i++) {
        used += (size_t) snprintf(out + used, size - used, "%s, ", link_names[i]);
    }

    if(used < size) {
        snprintf(out + used, size - used, "%s", inet_ntoa(addr));
    }
}
