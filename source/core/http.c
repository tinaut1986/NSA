#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <curl/curl.h>
#include <jansson.h>
#include <zlib.h>

#include "fs.h"
#include "appinfo.h"
#include "error.h"
#include "http.h"
#include "stringutil.h"

#define MAKE_HTTP_USER_AGENT_(major, minor, micro) ("Mozilla/5.0 (Nintendo 3DS; Mobile; rv:10.0) Gecko/20100101 " APP_NAME "/" #major "." #minor "." #micro)
#define MAKE_HTTP_USER_AGENT(major, minor, micro) MAKE_HTTP_USER_AGENT_(major, minor, micro)
#define HTTP_USER_AGENT MAKE_HTTP_USER_AGENT(VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO)

static void (*http_log)(const char* format, ...) = NULL;

void http_set_log(void (*log)(const char* format, ...)) {
    http_log = log;
}

#define HTTP_MAX_REDIRECTS 50
#define HTTP_TIMEOUT_SEC 15
#define HTTP_TIMEOUT_NS ((u64) HTTP_TIMEOUT_SEC * 1000000000)

struct httpc_context_s {
    httpcContext httpc;

    bool compressed;
    z_stream inflate;
    u8 buffer[32 * 1024];
    u32 bufferSize;
};

typedef struct httpc_context_s* httpc_context;

static void httpc_resolve_redirect(char* oldUrl, const char* redirectTo, size_t size) {
    if(size > 0) {
        if(redirectTo[0] == '/') {
            char* baseEnd = oldUrl;

            // Find the third slash to find the end of the URL's base; e.g. https://www.example.com/
            u32 slashCount = 0;
            while(*baseEnd != '\0' && (baseEnd = strchr(baseEnd + 1, '/')) != NULL) {
                slashCount++;
                if(slashCount == 3) {
                    break;
                }
            }

            // If there are less than 3 slashes, assume the base URL ends at the end of the string; e.g. https://www.example.com
            if(slashCount != 3) {
                baseEnd = oldUrl + strlen(oldUrl);
            }

            size_t baseLen = baseEnd - oldUrl;
            if(baseLen < size) {
                string_copy(baseEnd, redirectTo, size - baseLen);
            }
        } else {
            string_copy(oldUrl, redirectTo, size);
        }
    }
}

static Result httpc_open(httpc_context* context, const char* url, bool userAgent) {
    if(url == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    Result res = 0;

    httpc_context ctx = (httpc_context) calloc(1, sizeof(struct httpc_context_s));
    if(ctx != NULL) {
        char currUrl[1024];
        string_copy(currUrl, url, sizeof(currUrl));

        bool resolved = false;
        u32 redirectCount = 0;
        while(R_SUCCEEDED(res) && !resolved && redirectCount < HTTP_MAX_REDIRECTS) {
            if(R_SUCCEEDED(res = httpcOpenContext(&ctx->httpc, HTTPC_METHOD_GET, currUrl, 1))) {
                u32 response = 0;
                if(R_SUCCEEDED(res = httpcSetSSLOpt(&ctx->httpc, SSLCOPT_DisableVerify))
                   && (!userAgent || R_SUCCEEDED(res = httpcAddRequestHeaderField(&ctx->httpc, "User-Agent", HTTP_USER_AGENT)))
                   && R_SUCCEEDED(res = httpcAddRequestHeaderField(&ctx->httpc, "Accept-Encoding", "gzip, deflate"))
                   && R_SUCCEEDED(res = httpcSetKeepAlive(&ctx->httpc, HTTPC_KEEPALIVE_ENABLED))
                   && R_SUCCEEDED(res = httpcBeginRequest(&ctx->httpc))
                   && R_SUCCEEDED(res = httpcGetResponseStatusCodeTimeout(&ctx->httpc, &response, HTTP_TIMEOUT_NS))) {
                    if(response == 301 || response == 302 || response == 303) {
                        redirectCount++;

                        char redirectTo[1024];
                        memset(redirectTo, '\0', sizeof(redirectTo));
                        if(R_SUCCEEDED(res = httpcGetResponseHeader(&ctx->httpc, "Location", redirectTo, sizeof(redirectTo)))) {
                            httpcCloseContext(&ctx->httpc);

                            httpc_resolve_redirect(currUrl, redirectTo, sizeof(currUrl));
                        }
                    } else {
                        resolved = true;

                        if(response == 200) {
                            char encoding[32];
                            if(R_SUCCEEDED(httpcGetResponseHeader(&ctx->httpc, "Content-Encoding", encoding, sizeof(encoding)))) {
                                bool gzip = strncmp(encoding, "gzip", sizeof(encoding)) == 0;
                                bool deflate = strncmp(encoding, "deflate", sizeof(encoding)) == 0;

                                ctx->compressed = gzip || deflate;

                                if(ctx->compressed) {
                                    memset(&ctx->inflate, 0, sizeof(ctx->inflate));
                                    if(deflate) {
                                        inflateInit(&ctx->inflate);
                                    } else if(gzip) {
                                        inflateInit2(&ctx->inflate, MAX_WBITS | 16);
                                    }
                                }
                            }
                        } else {
                            res = R_APP_HTTP_ERROR_BASE + response;
                        }
                    }
                }

                if(R_FAILED(res)) {
                    httpcCloseContext(&ctx->httpc);
                }
            }
        }

        if(R_SUCCEEDED(res) && redirectCount >= 32) {
            res = R_APP_HTTP_TOO_MANY_REDIRECTS;
        }

        if(R_FAILED(res)) {
            free(ctx);
        }
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    if(R_SUCCEEDED(res)) {
        *context = ctx;
    }

    return res;
}

static Result httpc_close(httpc_context context) {
    if(context == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    if(context->compressed) {
        inflateEnd(&context->inflate);
    }

    Result res = httpcCloseContext(&context->httpc);
    free(context);
    return res;
}

static Result httpc_get_size(httpc_context context, u32* size) {
    if(context == NULL || size == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    return httpcGetDownloadSizeState(&context->httpc, NULL, size);
}

static Result httpc_read(httpc_context context, u32* bytesRead, void* buffer, u32 size) {
    if(context == NULL || buffer == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    Result res = 0;

    u32 startPos = 0;
    if(R_SUCCEEDED(res = httpcGetDownloadSizeState(&context->httpc, &startPos, NULL))) {
        res = HTTPC_RESULTCODE_DOWNLOADPENDING;

        u32 outPos = 0;
        if(context->compressed) {
            u32 lastPos = context->bufferSize;
            while(res == HTTPC_RESULTCODE_DOWNLOADPENDING && outPos < size) {
                if((context->bufferSize > 0
                    || R_SUCCEEDED(res = httpcReceiveDataTimeout(&context->httpc, &context->buffer[context->bufferSize], sizeof(context->buffer) - context->bufferSize, HTTP_TIMEOUT_NS))
                    || res == HTTPC_RESULTCODE_DOWNLOADPENDING)) {
                    Result posRes = 0;
                    u32 currPos = 0;
                    if(R_SUCCEEDED(posRes = httpcGetDownloadSizeState(&context->httpc, &currPos, NULL))) {
                        context->bufferSize += currPos - lastPos;

                        context->inflate.next_in = context->buffer;
                        context->inflate.next_out = buffer + outPos;
                        context->inflate.avail_in = context->bufferSize;
                        context->inflate.avail_out = size - outPos;
                        inflate(&context->inflate, Z_SYNC_FLUSH);

                        memcpy(context->buffer, context->buffer + (context->bufferSize - context->inflate.avail_in), context->inflate.avail_in);
                        context->bufferSize = context->inflate.avail_in;

                        lastPos = currPos;
                        outPos = size - context->inflate.avail_out;
                    } else {
                        res = posRes;
                    }
                }
            }
        } else {
            while(res == HTTPC_RESULTCODE_DOWNLOADPENDING && outPos < size) {
                if(R_SUCCEEDED(res = httpcReceiveDataTimeout(&context->httpc, &((u8*) buffer)[outPos], size - outPos, HTTP_TIMEOUT_NS)) || res == HTTPC_RESULTCODE_DOWNLOADPENDING) {
                    Result posRes = 0;
                    u32 currPos = 0;
                    if(R_SUCCEEDED(posRes = httpcGetDownloadSizeState(&context->httpc, &currPos, NULL))) {
                        outPos = currPos - startPos;
                    } else {
                        res = posRes;
                    }
                }
            }
        }

        if(res == HTTPC_RESULTCODE_DOWNLOADPENDING) {
            res = 0;
        }

        if(R_SUCCEEDED(res) && bytesRead != NULL) {
            *bytesRead = outPos;
        }
    }

    return res;
}

// ---- the system's HTTP service: what downloads went through before, kept for when curl can not connect ----

static Result http_httpc_download(const char* url, u32 bufferSize, void* userData, Result (*callback)(void* userData, void* buffer, size_t size),
                                  Result (*checkRunning)(void* userData), Result (*progress)(void* userData, u64 total, u64 curr)) {
    Result res = 0;

    void* buf = malloc(bufferSize);
    if(buf == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    httpc_context context = NULL;
    if(R_SUCCEEDED(res = httpc_open(&context, url, true))) {
        u32 dlSize = 0;
        if(R_SUCCEEDED(res = httpc_get_size(context, &dlSize))) {
            if(progress != NULL) {
                progress(userData, dlSize, 0);
            }

            u32 total = 0;
            u32 currSize = 0;
            while(total < dlSize
                  && (checkRunning == NULL || R_SUCCEEDED(res = checkRunning(userData)))
                  && R_SUCCEEDED(res = httpc_read(context, &currSize, buf, bufferSize))
                  && R_SUCCEEDED(res = callback(userData, buf, currSize))) {
                if(progress != NULL) {
                    progress(userData, dlSize, total);
                }

                total += currSize;
            }

            Result closeRes = httpc_close(context);
            if(R_SUCCEEDED(res)) {
                res = closeRes;
            }
        }
    }

    free(buf);
    return res;
}

// ---- curl, with the network on a thread of its own ----

// curl (and mbedTLS for HTTPS) needs the CPU, and the tasks that download run
// on the system core, of which NSA gets 30%. So the download runs on the app
// core (the New 3DS's third core when there is one), filling a ring of
// buffers, while the caller's thread hands the full ones to `callback` (the SD
// card, AM): the network and the card work at the same time.

#define HTTP_RING 4
#define HTTP_CHUNK_MAX (128 * 1024)
#define HTTP_SOCKET_BUFFER (128 * 1024)
#define HTTP_FETCH_STACK 0x20000
#define HTTP_FETCH_PRIORITY 0x31   // Below the UI thread.

typedef struct {
    CURL* curl;

    u32 chunk;
    u8* bufs[HTTP_RING];
    u32 sizes[HTTP_RING];
    u32 fill;              // Bytes in the buffer being filled (bufs[filled % HTTP_RING]).
    volatile u32 filled;   // Buffers handed over, ever.
    volatile u32 taken;    // Buffers given back by the caller, ever.

    volatile u64 total;
    volatile u64 received;   // Bytes curl has given, for the progress (the ring hands over whole buffers).
    u64 waited;              // Milliseconds the network thread waited for a free buffer (the SD card behind).
    volatile bool abort;   // The caller stopped (cancel, a failed write).
    volatile bool done;
    CURLcode ret;
} http_fetch;

static void http_fetch_hand_over(http_fetch* f) {
    f->sizes[f->filled % HTTP_RING] = f->fill;
    f->fill = 0;
    __sync_synchronize();
    f->filled++;

    // Wait for a free buffer.
    u64 start = osGetTime();
    while(f->filled - f->taken >= HTTP_RING && !f->abort) {
        svcSleepThread(1000000);
    }

    f->waited += osGetTime() - start;
}

static size_t http_fetch_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    http_fetch* f = (http_fetch*) userdata;

    size_t available = size * nmemb;
    f->received += available;
    size_t pos = 0;
    while(available > 0 && !f->abort) {
        u8* buf = f->bufs[f->filled % HTTP_RING];
        size_t copy = f->chunk - f->fill < available ? f->chunk - f->fill : available;
        memcpy(buf + f->fill, ptr + pos, copy);
        f->fill += copy;
        pos += copy;
        available -= copy;

        if(f->fill == f->chunk) {
            http_fetch_hand_over(f);
        }
    }

    return f->abort ? 0 : size * nmemb;
}

static int http_fetch_info(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    http_fetch* f = (http_fetch*) clientp;
    if(dltotal > 0) {
        f->total = (u64) dltotal;
    }

    return f->abort ? 1 : 0;
}

// Bigger socket buffers keep the data coming while curl is busy with a block.
static int http_fetch_sockopt(void* clientp, curl_socket_t fd, curlsocktype purpose) {
    int size = HTTP_SOCKET_BUFFER;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    return CURL_SOCKOPT_OK;
}

static void http_fetch_thread(void* arg) {
    http_fetch* f = (http_fetch*) arg;

    f->ret = curl_easy_perform(f->curl);
    if(f->ret == CURLE_OK && f->fill > 0 && !f->abort) {
        http_fetch_hand_over(f);
    }

    __sync_synchronize();
    f->done = true;
}

static Thread http_fetch_start(http_fetch* f) {
    bool isNew = false;
    Thread thread = NULL;
    if(R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
        thread = threadCreate(http_fetch_thread, f, HTTP_FETCH_STACK, HTTP_FETCH_PRIORITY, 2, false);
    }

    if(thread == NULL) {
        thread = threadCreate(http_fetch_thread, f, HTTP_FETCH_STACK, HTTP_FETCH_PRIORITY, -2, false);
    }

    return thread;
}

// `connected` says whether the server answered at all (else the system's
// service may still get through).
static Result http_curl_download(const char* url, u32 bufferSize, void* userData, Result (*callback)(void* userData, void* buffer, size_t size),
                                 Result (*checkRunning)(void* userData), Result (*progress)(void* userData, u64 total, u64 curr), bool* connected) {
    *connected = false;

    http_fetch* f = (http_fetch*) calloc(1, sizeof(http_fetch));
    if(f == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = 0;
    f->chunk = bufferSize < HTTP_CHUNK_MAX ? bufferSize : HTTP_CHUNK_MAX;
    for(u32 i = 0; i < HTTP_RING && R_SUCCEEDED(res); i++) {
        if((f->bufs[i] = (u8*) malloc(f->chunk)) == NULL) {
            res = R_APP_OUT_OF_MEMORY;
        }
    }

    if(R_SUCCEEDED(res) && (f->curl = curl_easy_init()) == NULL) {
        res = R_APP_CURL_INIT_FAILED;
    }

    if(R_SUCCEEDED(res)) {
        curl_easy_setopt(f->curl, CURLOPT_URL, url);
        curl_easy_setopt(f->curl, CURLOPT_BUFFERSIZE, (long) HTTP_CHUNK_MAX);
        curl_easy_setopt(f->curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(f->curl, CURLOPT_USERAGENT, HTTP_USER_AGENT);
        curl_easy_setopt(f->curl, CURLOPT_CONNECTTIMEOUT, (long) HTTP_TIMEOUT_SEC);
        // A stalled server fails instead of hanging: under 1 KB/s for the timeout.
        curl_easy_setopt(f->curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(f->curl, CURLOPT_LOW_SPEED_TIME, (long) HTTP_TIMEOUT_SEC * 2);
        curl_easy_setopt(f->curl, CURLOPT_MAXREDIRS, (long) HTTP_MAX_REDIRECTS);
        curl_easy_setopt(f->curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(f->curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(f->curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(f->curl, CURLOPT_HTTP_VERSION, (long) CURL_HTTP_VERSION_2TLS);
        curl_easy_setopt(f->curl, CURLOPT_SOCKOPTFUNCTION, http_fetch_sockopt);
        curl_easy_setopt(f->curl, CURLOPT_WRITEFUNCTION, http_fetch_write);
        curl_easy_setopt(f->curl, CURLOPT_WRITEDATA, (void*) f);
        curl_easy_setopt(f->curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(f->curl, CURLOPT_XFERINFOFUNCTION, http_fetch_info);
        curl_easy_setopt(f->curl, CURLOPT_XFERINFODATA, (void*) f);

        u64 started = osGetTime();
        Thread thread = http_fetch_start(f);
        if(thread == NULL) {
            res = R_APP_THREAD_CREATE_FAILED;
        } else {
            u64 delivered = 0;
            u64 lastLog = started;
            u64 lastReceived = 0;
            while(true) {
                // Every 10 s while it lasts, how fast it goes.
                if(http_log != NULL && osGetTime() - lastLog >= 10000) {
                    u64 received = f->received;
                    http_log("http: %llu KB so far, %llu KB/s, network waited %llu ms for the card\n",
                             received / 1024, (received - lastReceived) / 1024 * 1000 / (osGetTime() - lastLog), f->waited);
                    lastReceived = received;
                    lastLog = osGetTime();
                }

                if(checkRunning != NULL && R_FAILED(res = checkRunning(userData))) {
                    break;
                }

                if(f->taken != f->filled) {
                    u32 index = f->taken % HTTP_RING;
                    if(R_FAILED(res = callback(userData, f->bufs[index], f->sizes[index]))) {
                        break;
                    }

                    delivered += f->sizes[index];
                    __sync_synchronize();
                    f->taken++;

                    continue;
                }

                if(f->done) {
                    break;
                }

                if(progress != NULL) {
                    u64 received = f->received;
                    progress(userData, f->total > received ? f->total : received, received);
                }

                svcSleepThread(2000000);
            }

            f->abort = R_FAILED(res);
            threadJoin(thread, U64_MAX);
            threadFree(thread);

            long responseCode = 0;
            curl_easy_getinfo(f->curl, CURLINFO_RESPONSE_CODE, &responseCode);
            *connected = responseCode != 0 || delivered > 0;

            if(R_SUCCEEDED(res) && f->ret != CURLE_OK) {
                res = f->ret == CURLE_HTTP_RETURNED_ERROR ? R_APP_HTTP_ERROR_BASE + responseCode : R_APP_CURL_ERROR_BASE + f->ret;
            }

            if(http_log != NULL) {
                double secs = (osGetTime() - started) / 1000.0;
                curl_off_t speed = 0;
                curl_easy_getinfo(f->curl, CURLINFO_SPEED_DOWNLOAD_T, &speed);
                http_log("http: curl %s: %llu bytes in %.1f s (%lld KB/s), network waited %llu ms for the card, curl %d, http %ld, result %08lX\n",
                         url, delivered, secs, (long long) speed / 1024, f->waited, (int) f->ret, responseCode, res);
            }
        }

        curl_easy_cleanup(f->curl);
    }

    for(u32 i = 0; i < HTTP_RING; i++) {
        free(f->bufs[i]);
    }

    free(f);
    return res;
}

// ---- several connections at once, when the server serves ranges ----

// One connection to a server far away carries only what the 3DS keeps in
// flight (a few KB per round trip: ~50 KB/s at 130 ms). So a big file from a
// server that serves ranges comes in segments over several connections, as
// the console to console transfers do; the caller still gets it in order.

#define HTTP_PARALLEL 8
#define HTTP_PARALLEL_OLD 2
#define HTTP_SEGMENT (512 * 1024)
#define HTTP_PARALLEL_MIN (4 * 1024 * 1024)   // Smaller files: one connection.
#define HTTP_SEGMENT_TRIES 5

typedef struct http_parallel_s http_parallel;

typedef struct {
    http_parallel* p;
    u32 index;
    CURL* curl;
    u8* buf;
    u32 len;
    u32 want;
    volatile u64 received;   // Only this worker writes it; the caller adds them up.
    volatile s64 segment;    // The segment whole in `buf` for the caller, or -1.
    Thread thread;
} http_worker;

struct http_parallel_s {
    char url[1024];
    u64 size;
    u32 segments;
    u32 workerCount;
    http_worker workers[HTTP_PARALLEL];
    volatile bool abort;
    volatile Result failure;
};

static u64 http_parallel_received(http_parallel* p) {
    u64 total = 0;
    for(u32 i = 0; i < p->workerCount; i++) {
        total += p->workers[i].received;
    }

    return total;
}

static void http_curl_common(CURL* curl, const char* url) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long) HTTP_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long) HTTP_TIMEOUT_SEC * 2);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long) HTTP_MAX_REDIRECTS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, http_fetch_sockopt);
}

static size_t http_probe_header(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t len = size * nitems;
    if(len >= 20 && strncasecmp(buffer, "accept-ranges:", 14) == 0) {
        for(size_t i = 14; i + 5 <= len; i++) {
            if(strncasecmp(buffer + i, "bytes", 5) == 0) {
                *(bool*) userdata = true;
                break;
            }
        }
    }

    return len;
}

// Asks for the headers only: where the file is after redirects, how big, and
// whether ranges are served. False when it can not tell.
static bool http_probe(const char* url, char* effective, size_t effectiveSize, u64* size) {
    CURL* curl = curl_easy_init();
    if(curl == NULL) {
        return false;
    }

    bool ranges = false;
    http_curl_common(curl, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, http_probe_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void*) &ranges);

    bool ok = false;
    long code = 0;
    curl_off_t length = -1;
    char* where = NULL;
    if(curl_easy_perform(curl) == CURLE_OK
       && curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK && code == 200
       && curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK && length > 0
       && curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &where) == CURLE_OK && where != NULL
       && strlen(where) < effectiveSize) {
        string_copy(effective, where, effectiveSize);
        *size = (u64) length;
        ok = ranges;
    }

    curl_easy_cleanup(curl);
    return ok;
}

static size_t http_worker_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    http_worker* w = (http_worker*) userdata;
    size_t len = size * nmemb;
    if(w->p->abort || w->len + len > w->want) {
        return 0;
    }

    memcpy(w->buf + w->len, ptr, len);
    w->len += len;
    w->received += len;
    return len;
}

static int http_worker_info(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    return ((http_worker*) clientp)->p->abort ? 1 : 0;
}

static void http_worker_thread(void* arg) {
    http_worker* w = (http_worker*) arg;
    http_parallel* p = w->p;

    for(u32 segment = w->index; segment < p->segments && !p->abort; segment += p->workerCount) {
        // The caller has not taken the last one yet.
        while(w->segment >= 0 && !p->abort) {
            svcSleepThread(1000000);
        }

        u64 start = (u64) segment * HTTP_SEGMENT;
        u64 end = start + HTTP_SEGMENT < p->size ? start + HTTP_SEGMENT : p->size;
        char range[64];
        snprintf(range, sizeof(range), "%llu-%llu", start, end - 1);

        CURLcode ret = CURLE_OK;
        long code = 0;
        w->len = 0;
        for(u32 attempt = 0; attempt < HTTP_SEGMENT_TRIES && !p->abort; attempt++) {
            // Again: what the failed try got does not count.
            w->received -= w->len;
            w->len = 0;
            w->want = (u32) (end - start);
            curl_easy_setopt(w->curl, CURLOPT_RANGE, range);
            ret = curl_easy_perform(w->curl);
            curl_easy_getinfo(w->curl, CURLINFO_RESPONSE_CODE, &code);
            if(ret == CURLE_OK && code == 206 && w->len == w->want) {
                break;
            }

            ret = ret == CURLE_OK ? CURLE_PARTIAL_FILE : ret;
        }

        if(ret != CURLE_OK || code != 206) {
            if(!p->abort) {
                p->failure = ret == CURLE_HTTP_RETURNED_ERROR ? R_APP_HTTP_ERROR_BASE + code : R_APP_CURL_ERROR_BASE + ret;
                if(http_log != NULL) {
                    http_log("http: segment %lu failed: curl %d, http %ld\n", segment, (int) ret, code);
                }
            }

            p->abort = true;
            break;
        }

        __sync_synchronize();
        w->segment = segment;
    }
}

static Result http_parallel_download(const char* url, u64 size, void* userData, Result (*callback)(void* userData, void* buffer, size_t size),
                                     Result (*checkRunning)(void* userData), Result (*progress)(void* userData, u64 total, u64 curr)) {
    http_parallel* p = (http_parallel*) calloc(1, sizeof(http_parallel));
    if(p == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    string_copy(p->url, url, sizeof(p->url));
    p->size = size;
    p->segments = (u32) ((size + HTTP_SEGMENT - 1) / HTTP_SEGMENT);
    // An Old 3DS has not the CPU for many HTTPS connections at once: they starve and time out.
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    u32 connections = isNew ? HTTP_PARALLEL : HTTP_PARALLEL_OLD;
    p->workerCount = p->segments < connections ? p->segments : connections;

    Result res = 0;
    for(u32 i = 0; i < p->workerCount && R_SUCCEEDED(res); i++) {
        http_worker* w = &p->workers[i];
        w->p = p;
        w->index = i;
        w->segment = -1;
        if((w->buf = (u8*) malloc(HTTP_SEGMENT)) == NULL) {
            res = R_APP_OUT_OF_MEMORY;
        } else if((w->curl = curl_easy_init()) == NULL) {
            res = R_APP_CURL_INIT_FAILED;
        } else {
            http_curl_common(w->curl, p->url);
            curl_easy_setopt(w->curl, CURLOPT_BUFFERSIZE, (long) HTTP_CHUNK_MAX);
            curl_easy_setopt(w->curl, CURLOPT_WRITEFUNCTION, http_worker_write);
            curl_easy_setopt(w->curl, CURLOPT_WRITEDATA, (void*) w);
            curl_easy_setopt(w->curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(w->curl, CURLOPT_XFERINFOFUNCTION, http_worker_info);
            curl_easy_setopt(w->curl, CURLOPT_XFERINFODATA, (void*) w);
        }
    }

    for(u32 i = 0; i < p->workerCount && R_SUCCEEDED(res); i++) {
        http_worker* w = &p->workers[i];
        w->thread = isNew ? threadCreate(http_worker_thread, w, HTTP_FETCH_STACK, HTTP_FETCH_PRIORITY, 2, false) : NULL;
        if(w->thread == NULL) {
            w->thread = threadCreate(http_worker_thread, w, HTTP_FETCH_STACK, HTTP_FETCH_PRIORITY, -2, false);
        }

        if(w->thread == NULL) {
            res = R_APP_THREAD_CREATE_FAILED;
        }
    }

    u64 started = osGetTime();
    u64 lastLog = started;
    u64 lastReceived = 0;
    u64 shown = 0;
    u64 delivered = 0;
    for(u32 segment = 0; segment < p->segments && R_SUCCEEDED(res);) {
        if(checkRunning != NULL && R_FAILED(res = checkRunning(userData))) {
            break;
        }

        if(http_log != NULL && osGetTime() - lastLog >= 10000) {
            u64 received = http_parallel_received(p);
            http_log("http: %llu KB so far (%llu KB written), %llu KB/s over %lu connections\n", received / 1024, delivered / 1024,
                     received > lastReceived ? (received - lastReceived) / 1024 * 1000 / (osGetTime() - lastLog) : 0, p->workerCount);
            lastReceived = received;
            lastLog = osGetTime();
        }

        http_worker* w = &p->workers[segment % p->workerCount];
        if(w->segment == (s64) segment) {
            if(R_FAILED(res = callback(userData, w->buf, w->len))) {
                break;
            }

            delivered += w->len;
            __sync_synchronize();
            w->segment = -1;
            segment++;
        } else if(p->abort) {
            res = p->failure != 0 ? p->failure : R_APP_CANCELLED;
            break;
        } else {
            svcSleepThread(2000000);
        }

        // A segment tried again goes back: the bar does not.
        if(progress != NULL) {
            u64 received = http_parallel_received(p);
            shown = received > shown ? received : shown;
            progress(userData, p->size, shown);
        }
    }

    p->abort = true;
    for(u32 i = 0; i < p->workerCount; i++) {
        http_worker* w = &p->workers[i];
        if(w->thread != NULL) {
            threadJoin(w->thread, U64_MAX);
            threadFree(w->thread);
        }

        if(w->curl != NULL) {
            curl_easy_cleanup(w->curl);
        }

        free(w->buf);
    }

    if(http_log != NULL) {
        double secs = (osGetTime() - started) / 1000.0;
        http_log("http: %lu connections %s: %llu of %llu bytes in %.1f s (%.0f KB/s), result %08lX\n", p->workerCount, url, delivered, p->size,
                 secs, secs > 0 ? delivered / 1024 / secs : 0, res);
    }

    free(p);
    return res;
}

Result http_download_callback(const char* url, u32 bufferSize, void* userData, Result (*callback)(void* userData, void* buffer, size_t size),
                                                                               Result (*checkRunning)(void* userData),
                                                                               Result (*progress)(void* userData, u64 total, u64 curr)) {
    // A big file from a server that serves ranges: several connections.
    char* effective = (char*) malloc(1024);
    u64 size = 0;
    if(effective != NULL && http_probe(url, effective, 1024, &size) && size >= HTTP_PARALLEL_MIN) {
        Result res = http_parallel_download(effective, size, userData, callback, checkRunning, progress);
        free(effective);
        return res;
    }

    free(effective);

    bool connected = false;
    Result res = http_curl_download(url, bufferSize, userData, callback, checkRunning, progress, &connected);

    // curl could not reach it (an old TLS, say): the system's service may.
    if(R_FAILED(res) && !connected && res != R_APP_CANCELLED && res != R_APP_OUT_OF_MEMORY) {
        u64 started = osGetTime();
        res = http_httpc_download(url, bufferSize, userData, callback, checkRunning, progress);
        if(http_log != NULL) {
            http_log("http: system service %s: %.1f s, result %08lX\n", url, (osGetTime() - started) / 1000.0, res);
        }
    }

    return res;
}

typedef struct {
    void* buf;
    size_t size;

    size_t pos;
} http_buffer_data;

static Result http_download_buffer_callback(void* userData, void* buffer, size_t size) {
    http_buffer_data* data = (http_buffer_data*) userData;

    size_t remaining = data->size - data->pos;
    size_t copySize = size;
    if(copySize > remaining) {
        copySize = remaining;
    }

    if(copySize > 0) {
        memcpy((u8*) data->buf + data->pos, buffer, copySize);
        data->pos += copySize;
    }

    return 0;
}


Result http_download_buffer(const char* url, u32* downloadedSize, void* buf, size_t size) {
    http_buffer_data data = {buf, size, 0};
    Result res = http_download_callback(url, size, &data, http_download_buffer_callback, NULL, NULL);

    if(R_SUCCEEDED(res)) {
        *downloadedSize = data.pos;
    }

    return res;
}

Result http_download_json(const char* url, json_t** json, size_t maxSize) {
    if(url == NULL || json == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    Result res = 0;

    char* text = (char*) calloc(sizeof(char), maxSize);
    if(text != NULL) {
        u32 textSize = 0;
        if(R_SUCCEEDED(res = http_download_buffer(url, &textSize, text, maxSize))) {
            json_error_t error;
            json_t* parsed = json_loads(text, 0, &error);
            if(parsed != NULL) {
                *json = parsed;
            } else {
                res = R_APP_PARSE_FAILED;
            }
        }

        free(text);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static Result FSUSER_AddSeed(u64 titleId, const void* seed) {
    u32 *cmdbuf = getThreadCommandBuffer();

    cmdbuf[0] = 0x087A0180;
    cmdbuf[1] = (u32) (titleId & 0xFFFFFFFF);
    cmdbuf[2] = (u32) (titleId >> 32);
    memcpy(&cmdbuf[3], seed, 16);

    Result ret = 0;
    if(R_FAILED(ret = svcSendSyncRequest(*fsGetSessionHandle()))) return ret;

    ret = cmdbuf[1];
    return ret;
}

Result http_download_seed(u64 titleId) {
    char pathBuf[64];
    snprintf(pathBuf, 64, APP_DATA_DIR "seed/%016llX.dat", titleId);

    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(pathBuf);
    if(fsPath != NULL) {
        u8 seed[16];

        Handle fileHandle = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenFileDirectly(&fileHandle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, FS_OPEN_READ, 0))) {
            u32 bytesRead = 0;
            res = FSFILE_Read(fileHandle, &bytesRead, 0, seed, sizeof(seed));

            FSFILE_Close(fileHandle);
        }

        fs_free_path_utf8(fsPath);

        if(R_FAILED(res)) {
            u8 region = CFG_REGION_USA;
            CFGU_SecureInfoGetRegion(&region);

            if(region <= CFG_REGION_TWN) {
                static const char* regionStrings[] = {"JP", "US", "GB", "GB", "HK", "KR", "TW"};

                char url[128];
                snprintf(url, 128, "https://kagiya-ctr.cdn.nintendo.net/title/0x%016llX/ext_key?country=%s", titleId, regionStrings[region]);

                u32 downloadedSize = 0;
                if(R_SUCCEEDED(res = http_download_buffer(url, &downloadedSize, seed, sizeof(seed))) && downloadedSize != sizeof(seed)) {
                    res = R_APP_BAD_DATA;
                }
            } else {
                res = R_APP_OUT_OF_RANGE;
            }
        }

        if(R_SUCCEEDED(res)) {
            res = FSUSER_AddSeed(titleId, seed);
        }
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}