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
#include "netpoll.h"
#include "speedlog.h"
#include "stringutil.h"

#define MAKE_HTTP_USER_AGENT_(major, minor, micro) ("Mozilla/5.0 (Nintendo 3DS; Mobile; rv:10.0) Gecko/20100101 " APP_NAME "/" #major "." #minor "." #micro)
#define MAKE_HTTP_USER_AGENT(major, minor, micro) MAKE_HTTP_USER_AGENT_(major, minor, micro)
#define HTTP_USER_AGENT MAKE_HTTP_USER_AGENT(VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO)

static void (*http_log)(const char* format, ...) = NULL;

void http_set_log(void (*log)(const char* format, ...)) {
    http_log = log;
}

// Transfers in flight, so that leaving the app (HOME -> close) can wait for them
// before the sockets go away: a thread inside curl when socExit runs crashes.
static volatile u32 http_active = 0;
static volatile bool http_quit = false;

static CURLcode http_perform(CURL* curl) {
    __sync_fetch_and_add(&http_active, 1);
    CURLcode ret = http_quit ? CURLE_ABORTED_BY_CALLBACK : curl_easy_perform(curl);
    __sync_fetch_and_sub(&http_active, 1);
    return ret;
}

void http_shutdown() {
    http_quit = true;
    for(u32 i = 0; i < 1000 && http_active > 0; i++) {
        svcSleepThread(10000000);
    }
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
                                  Result (*checkRunning)(void* userData), Result (*progress)(void* userData, u64 total, u64 curr, u32 connections)) {
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
                progress(userData, dlSize, 0, 1);
            }

            u32 total = 0;
            u32 currSize = 0;
            while(total < dlSize
                  && (checkRunning == NULL || R_SUCCEEDED(res = checkRunning(userData)))
                  && R_SUCCEEDED(res = httpc_read(context, &currSize, buf, bufferSize))
                  && R_SUCCEEDED(res = callback(userData, buf, currSize))) {
                if(progress != NULL) {
                    progress(userData, dlSize, total, 1);
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

    return f->abort || http_quit ? 1 : 0;
}

// Bigger socket buffers widen the TCP window and keep the data coming while
// curl is busy with a block. Set before connecting, when the window is sized.
static int http_fetch_sockopt(void* clientp, curl_socket_t fd, curlsocktype purpose) {
    net_set_buffers(fd);
    return CURL_SOCKOPT_OK;
}

// What the connections of one download share: DNS answers and TLS sessions,
// so that after the first one they skip most of the handshake (much of the
// CPU an HTTPS connection costs, which an Old 3DS feels).
typedef struct {
    CURLSH* share;
    LightLock locks[CURL_LOCK_DATA_LAST];
} http_shared;

static void http_shared_lock(CURL* handle, curl_lock_data data, curl_lock_access access, void* userptr) {
    LightLock_Lock(&((http_shared*) userptr)->locks[data]);
}

static void http_shared_unlock(CURL* handle, curl_lock_data data, void* userptr) {
    LightLock_Unlock(&((http_shared*) userptr)->locks[data]);
}

static void http_shared_open(http_shared* s) {
    for(u32 i = 0; i < CURL_LOCK_DATA_LAST; i++) {
        LightLock_Init(&s->locks[i]);
    }

    if((s->share = curl_share_init()) != NULL) {
        curl_share_setopt(s->share, CURLSHOPT_LOCKFUNC, http_shared_lock);
        curl_share_setopt(s->share, CURLSHOPT_UNLOCKFUNC, http_shared_unlock);
        curl_share_setopt(s->share, CURLSHOPT_USERDATA, (void*) s);
        curl_share_setopt(s->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(s->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    }
}

// After every handle that used it is cleaned up.
static void http_shared_close(http_shared* s) {
    if(s->share != NULL) {
        curl_share_cleanup(s->share);
        s->share = NULL;
    }
}

static void http_fetch_thread(void* arg) {
    http_fetch* f = (http_fetch*) arg;

    f->ret = http_perform(f->curl);
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
// From byte `from` on (a download that began over several connections), the caller's progress counting from 0.
static Result http_curl_download(const char* url, u64 from, u32 bufferSize, void* userData, Result (*callback)(void* userData, void* buffer, size_t size),
                                 Result (*checkRunning)(void* userData), Result (*progress)(void* userData, u64 total, u64 curr, u32 connections), bool* connected,
                                 http_shared* shared) {
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
        if(from > 0) {
            curl_easy_setopt(f->curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t) from);
        }

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
        if(shared->share != NULL) {
            curl_easy_setopt(f->curl, CURLOPT_SHARE, shared->share);
        }
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
            // The trace: what the network brought, the card's (the install's) time, the time with nothing to hand over.
            speedlog* trace = speedlog_begin("url", url, 0);
            u64 traced = 0;
            while(true) {
                u64 got = f->received;
                if(got > traced) {
                    speedlog_net(trace, got - traced, 0);
                    traced = got;
                }

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
                    speedlog_gauge(trace, f->filled - f->taken);
                    u64 start = speedlog_ticks();
                    res = callback(userData, f->bufs[index], f->sizes[index]);
                    speedlog_disk(trace, f->sizes[index], speedlog_ticks() - start);
                    if(R_FAILED(res)) {
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
                    progress(userData, from + (f->total > received ? f->total : received), from + received, 1);
                }

                u64 start = speedlog_ticks();
                svcSleepThread(2000000);
                speedlog_wait(trace, speedlog_ticks() - start);
            }

            speedlog_end(trace, R_SUCCEEDED(res) && f->ret == CURLE_OK);

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
//
// The segments wait in a window of buffers ahead of the one the caller needs
// next: a connection that is done takes the next free segment, so one slow
// connection no longer holds the others up (each used to own one segment in
// turn and waited for the caller to take it, so the download went at the
// slowest one's pace).
//
// A segment can still stall (a connection that stops for seconds, often near
// the end): when a connection has nothing else to take, it asks again for the
// segment the install is waiting for (near the end, any left) once it has been
// on its way three times as long as segments take (at least HTTP_HEDGE_MS),
// and whichever copy arrives first is used. Not sooner: on a slow link every
// segment takes many seconds (~10 s on the 2DS), and asking again for all of
// them wasted half the bandwidth. Each connection fetches into a buffer of its
// own and copies a finished segment into the window, so the two never write
// the same memory.
//
// The connections ask for the file where the redirects ended (no redirects
// for each segment), unless that address refuses a second request: Dropbox's
// `dl.dropboxusercontent.com/cd/0/get/...` serves one and answers 403 to the
// rest. Then the rest of the file comes over one connection from the address
// given (HTTP_RES_ONE_LINK), as 3dsDL installs: going through the redirects
// for every segment (a new host and TLS handshake each) went at half the
// speed (~470 KB/s from Dropbox).

// http_parallel_download's own: the rest goes over one connection (never returned to the caller).
#define HTTP_RES_ONE_LINK MAKERESULT(RL_TEMPORARY, RS_NOTSUPPORTED, RM_APPLICATION, 801)

#define HTTP_PARALLEL 8
#define HTTP_PARALLEL_OLD 8
#define HTTP_SEGMENT (512 * 1024)
#define HTTP_WINDOW_PER_CONNECTION 2
#define HTTP_WINDOW_MIN 6
#define HTTP_WINDOW_MAX (HTTP_PARALLEL * HTTP_WINDOW_PER_CONNECTION)
#define HTTP_PARALLEL_MIN (4 * 1024 * 1024)   // Smaller files: one connection.
#define HTTP_SEGMENT_TRIES 5
#define HTTP_HEDGE_MS 2000

typedef struct http_parallel_s http_parallel;

typedef struct {
    http_parallel* p;
    CURL* curl;
    u8* buf;                 // Its own: the segment being fetched.
    u32 len;
    u32 want;
    s32 segment;             // The one being fetched.
    volatile u64 received;   // Only this worker writes it; the caller adds them up.
    Thread thread;
} http_worker;

struct http_parallel_s {
    char url[1024];                        // Where the redirects ended.
    char original[1024];                   // The address given, when the redirects went elsewhere.
    u64 size;
    u32 segments;
    u32 workerCount;
    http_worker workers[HTTP_PARALLEL];

    u32 window;                            // Slots; segment n goes to slot n % window.
    u8* slots[HTTP_WINDOW_MAX];
    volatile s32 ready[HTTP_WINDOW_MAX];   // The segment whole in the slot, or -1.
    u32 lengths[HTTP_WINDOW_MAX];
    u32 fetchers[HTTP_WINDOW_MAX];         // Connections on the slot's segment.
    u64 started[HTTP_WINDOW_MAX];          // When its first connection took it.
    LightLock lock;                        // Around everything above but the slots' data, and these:
    u32 nextAssign;                        // The next segment no connection has taken.
    volatile u32 nextDeliver;              // The next segment the caller needs.
    u32 hedges;                            // Segments asked for twice.
    u32 completed;                         // Segments in, for averageMs.
    u32 averageMs;                         // How long a segment takes, lately.

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

static void http_curl_common(CURL* curl, const char* url, http_shared* shared) {
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
    if(shared->share != NULL) {
        curl_easy_setopt(curl, CURLOPT_SHARE, shared->share);
    }
}

typedef struct {
    u64 total;    // From Content-Range: bytes 0-0/<total>.
    u32 body;
    u8* buf;      // The first bytes, when asked for (NULL: one byte, not kept).
    u32 bufSize;
} http_probe_state;

static size_t http_probe_header(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t len = size * nitems;
    if(len > 14 && strncasecmp(buffer, "content-range:", 14) == 0) {
        char line[128];
        size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, buffer, n);
        line[n] = '\0';

        char* slash = strchr(line, '/');
        if(slash != NULL && slash[1] >= '0' && slash[1] <= '9') {
            ((http_probe_state*) userdata)->total = strtoull(slash + 1, NULL, 10);
        }
    }

    return len;
}

// The bytes asked for; a server that ignores the range sends the whole file: stop it.
static size_t http_probe_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    http_probe_state* state = (http_probe_state*) userdata;
    size_t len = size * nmemb;
    u32 limit = state->buf != NULL ? state->bufSize : 1;
    if(state->body + len > limit) {
        return 0;
    }

    if(state->buf != NULL) {
        memcpy(state->buf + state->body, ptr, len);
    }

    state->body += len;
    return len;
}

// Asks for the first byte only: where the file is after redirects, how big,
// and whether ranges are served (a 206 with the total). Not HEAD and
// Accept-Ranges, as before: some servers (catbox.moe) answer HEAD with no size
// and no Accept-Ranges yet serve ranges, and the download then got one
// connection. As 3dsDL asks. False when it can not tell.
//
// With `first`, it asks for the first `*firstSize` bytes instead and keeps them
// (`*firstSize` becomes what came: less for a smaller file).
static bool http_probe(const char* url, char* effective, size_t effectiveSize, u64* size, u8* first, u32* firstSize, http_shared* shared) {
    CURL* curl = curl_easy_init();
    if(curl == NULL) {
        return false;
    }

    http_probe_state state = {0, 0, first, first != NULL ? *firstSize : 0};
    char range[32] = "0-0";
    if(first != NULL) {
        snprintf(range, sizeof(range), "0-%lu", *firstSize - 1);
    }

    http_curl_common(curl, url, shared);
    curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, http_probe_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void*) &state);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_probe_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*) &state);

    bool ok = false;
    long code = 0;
    char* where = NULL;
    if(http_perform(curl) == CURLE_OK
       && curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK && code == 206 && state.total > 0
       && curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &where) == CURLE_OK && where != NULL
       && strlen(where) < effectiveSize) {
        string_copy(effective, where, effectiveSize);
        *size = state.total;
        ok = true;
        if(first != NULL) {
            ok = state.body == (state.total < *firstSize ? state.total : *firstSize);
            *firstSize = state.body;
        }
    }

    curl_easy_cleanup(curl);
    return ok;
}

// Whether another copy of the worker's segment has already arrived (or been used).
static bool http_worker_beaten(http_worker* w) {
    http_parallel* p = w->p;
    return (u32) w->segment < p->nextDeliver || p->ready[(u32) w->segment % p->window] == w->segment;
}

static size_t http_worker_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    http_worker* w = (http_worker*) userdata;
    size_t len = size * nmemb;
    if(w->p->abort || w->len + len > w->want || http_worker_beaten(w)) {
        return 0;
    }

    memcpy(w->buf + w->len, ptr, len);
    w->len += len;
    w->received += len;
    return len;
}

// Also called with no data coming: a stalled connection stops once the other copy is in.
static int http_worker_info(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    http_worker* w = (http_worker*) clientp;
    return w->p->abort || http_quit || http_worker_beaten(w) ? 1 : 0;
}

// The next segment there is room for in the window; else the oldest one on
// its way for too long, asked for again; else -1 (all delivered: done; or wait).
static s32 http_worker_take(http_parallel* p, bool* done) {
    s32 segment = -1;
    u64 now = osGetTime();
    LightLock_Lock(&p->lock);
    if(p->nextDeliver >= p->segments) {
        *done = true;
    } else if(p->nextAssign < p->segments && p->nextAssign < p->nextDeliver + p->window) {
        // Its slot held segment - window, which the caller has taken.
        segment = (s32) p->nextAssign++;
        u32 slot = (u32) segment % p->window;
        p->fetchers[slot] = 1;
        p->started[slot] = now;
    } else if(p->completed >= p->workerCount) {
        u64 late = (u64) p->averageMs * 3 > HTTP_HEDGE_MS ? (u64) p->averageMs * 3 : HTTP_HEDGE_MS;
        u32 last = p->nextAssign >= p->segments ? p->nextAssign : p->nextDeliver + 1;
        for(u32 n = p->nextDeliver; n < last && n < p->nextAssign; n++) {
            u32 slot = n % p->window;
            if(p->ready[slot] != (s32) n && p->fetchers[slot] == 1 && now - p->started[slot] >= late) {
                p->fetchers[slot]++;
                p->hedges++;
                segment = (s32) n;
                break;
            }
        }
    }

    LightLock_Unlock(&p->lock);
    return segment;
}

static void http_worker_thread(void* arg) {
    http_worker* w = (http_worker*) arg;
    http_parallel* p = w->p;

    bool done = false;
    while(!p->abort && !done) {
        s32 segment = http_worker_take(p, &done);
        if(segment < 0) {
            if(!done) {
                svcSleepThread(10000000);
            }

            continue;
        }

        u32 slot = (u32) segment % p->window;
        w->segment = segment;

        u64 start = (u64) segment * HTTP_SEGMENT;
        u64 end = start + HTTP_SEGMENT < p->size ? start + HTTP_SEGMENT : p->size;
        char range[64];
        snprintf(range, sizeof(range), "%llu-%llu", start, end - 1);

        CURLcode ret = CURLE_OK;
        long code = 0;
        w->len = 0;
        for(u32 attempt = 0; attempt < HTTP_SEGMENT_TRIES && !p->abort && !http_worker_beaten(w); attempt++) {
            // Again: what the failed try got does not count.
            w->received -= w->len;
            w->len = 0;
            w->want = (u32) (end - start);
            curl_easy_setopt(w->curl, CURLOPT_RANGE, range);
            ret = http_perform(w->curl);
            curl_easy_getinfo(w->curl, CURLINFO_RESPONSE_CODE, &code);
            if(ret == CURLE_OK && code == 206 && w->len == w->want) {
                break;
            }

            // The address the redirects ended at served once and refuses the rest: one connection from the one given.
            if(ret == CURLE_HTTP_RETURNED_ERROR && (code == 403 || code == 410) && p->original[0] != '\0') {
                if(!p->abort) {
                    p->failure = HTTP_RES_ONE_LINK;
                    if(http_log != NULL) {
                        http_log("http: segment %ld: http %ld where the redirects ended, the rest over one connection\n", segment, code);
                    }
                }

                p->abort = true;
                break;
            }

            ret = ret == CURLE_OK ? CURLE_PARTIAL_FILE : ret;

            // A connection that could not get through tries again in a moment, not at once.
            if(ret == CURLE_COULDNT_CONNECT || ret == CURLE_COULDNT_RESOLVE_HOST) {
                svcSleepThread(500000000LL * (attempt + 1));
            }
        }

        bool whole = ret == CURLE_OK && code == 206 && w->len == w->want;
        bool won = false;
        bool failed = false;
        LightLock_Lock(&p->lock);
        if(!http_worker_beaten(w)) {
            if(whole) {
                memcpy(p->slots[slot], w->buf, w->len);
                p->lengths[slot] = w->len;
                __sync_synchronize();
                p->ready[slot] = segment;
                won = true;

                u32 took = (u32) (osGetTime() - p->started[slot]);
                p->averageMs = p->completed == 0 ? took : (p->averageMs * 7 + took) / 8;
                p->completed++;
            } else {
                // Another connection may still bring it.
                failed = p->fetchers[slot] <= 1;
            }
        }

        // Only the copy used counts for the bar.
        if(!won) {
            w->received -= w->len;
        }

        p->fetchers[slot]--;
        LightLock_Unlock(&p->lock);

        if(failed) {
            if(!p->abort) {
                p->failure = ret == CURLE_HTTP_RETURNED_ERROR ? R_APP_HTTP_ERROR_BASE + code : R_APP_CURL_ERROR_BASE + ret;
                if(http_log != NULL) {
                    http_log("http: segment %ld failed: curl %d, http %ld\n", segment, (int) ret, code);
                }
            }

            p->abort = true;
            break;
        }
    }
}

// `*handed` is what the caller got, in order.
static Result http_parallel_download(const char* url, const char* original, u64 size, void* userData, Result (*callback)(void* userData, void* buffer, size_t size),
                                     Result (*checkRunning)(void* userData), Result (*progress)(void* userData, u64 total, u64 curr, u32 connections),
                                     http_shared* shared, u64* handed) {
    http_parallel* p = (http_parallel*) calloc(1, sizeof(http_parallel));
    if(p == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    string_copy(p->url, url, sizeof(p->url));
    if(original != NULL && strcmp(original, url) != 0) {
        string_copy(p->original, original, sizeof(p->original));
    }

    p->size = size;
    p->segments = (u32) ((size + HTTP_SEGMENT - 1) / HTTP_SEGMENT);
    // Two was the most an Old 3DS took before the waits stopped holding the SOC session and TLS
    // sessions were shared. Now the 2DS holds eight with its CPU at 20% (though its network tops out
    // around 500 KB/s with any number of them).
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    u32 connections = isNew ? HTTP_PARALLEL : HTTP_PARALLEL_OLD;
    p->workerCount = p->segments < connections ? p->segments : connections;
    p->window = p->workerCount * HTTP_WINDOW_PER_CONNECTION;
    p->window = p->window < HTTP_WINDOW_MIN ? HTTP_WINDOW_MIN : p->window;
    p->window = p->window > p->segments ? p->segments : p->window;
    LightLock_Init(&p->lock);

    Result res = 0;
    for(u32 i = 0; i < p->workerCount && R_SUCCEEDED(res); i++) {
        if((p->workers[i].buf = (u8*) malloc(HTTP_SEGMENT)) == NULL) {
            res = R_APP_OUT_OF_MEMORY;
        }
    }

    // A smaller window when memory is short, as long as each connection has a slot.
    u32 slots = 0;
    while(R_SUCCEEDED(res) && slots < p->window && (p->slots[slots] = (u8*) malloc(HTTP_SEGMENT)) != NULL) {
        p->ready[slots++] = -1;
    }

    if(R_SUCCEEDED(res) && slots < p->workerCount && slots < p->window) {
        res = R_APP_OUT_OF_MEMORY;
    }

    p->window = slots;

    for(u32 i = 0; i < p->workerCount && R_SUCCEEDED(res); i++) {
        http_worker* w = &p->workers[i];
        w->p = p;
        if((w->curl = curl_easy_init()) == NULL) {
            res = R_APP_CURL_INIT_FAILED;
        } else {
            http_curl_common(w->curl, p->url, shared);
            curl_easy_setopt(w->curl, CURLOPT_BUFFERSIZE, (long) HTTP_CHUNK_MAX);
            // Each connection keeps two open (the server it redirects from and the one it ends at), not curl's five:
            // at the address given, every segment ends at a new Dropbox host, and eight connections' worth of the
            // old ones left the console without sockets (curl 7).
            curl_easy_setopt(w->curl, CURLOPT_MAXCONNECTS, 2L);
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
    // The trace: what the network brought, the card's (the install's) time, the time waiting for the next segment,
    // and the segments ahead as the gauge.
    speedlog* trace = speedlog_begin("url", url, p->size);
    u64 traced = 0;
    for(u32 segment = 0; segment < p->segments && R_SUCCEEDED(res);) {
        u64 got = http_parallel_received(p);
        if(got > traced) {
            speedlog_net(trace, got - traced, 0);
            traced = got;
        }

        speedlog_gauge(trace, p->nextAssign - p->nextDeliver);

        if(checkRunning != NULL && R_FAILED(res = checkRunning(userData))) {
            break;
        }

        if(http_log != NULL && osGetTime() - lastLog >= 10000) {
            u64 received = http_parallel_received(p);
            http_log("http: %llu KB so far (%llu KB written), %llu KB/s over %lu connections, %lu segments ahead\n", received / 1024,
                     delivered / 1024, received > lastReceived ? (received - lastReceived) / 1024 * 1000 / (osGetTime() - lastLog) : 0,
                     p->workerCount, p->nextAssign - p->nextDeliver);
            lastReceived = received;
            lastLog = osGetTime();
        }

        u32 slot = segment % p->window;
        if(p->ready[slot] == (s32) segment) {
            __sync_synchronize();   // The slot's data and length, written before `ready`.
            u64 start = speedlog_ticks();
            res = callback(userData, p->slots[slot], p->lengths[slot]);
            speedlog_disk(trace, p->lengths[slot], speedlog_ticks() - start);
            if(R_FAILED(res)) {
                break;
            }

            delivered += p->lengths[slot];
            segment++;
            LightLock_Lock(&p->lock);
            p->ready[slot] = -1;
            p->nextDeliver = segment;
            LightLock_Unlock(&p->lock);
        } else if(p->abort) {
            res = p->failure != 0 ? p->failure : R_APP_CANCELLED;
            break;
        } else {
            u64 start = speedlog_ticks();
            svcSleepThread(2000000);
            speedlog_wait(trace, speedlog_ticks() - start);
        }

        // A segment tried again goes back: the bar does not.
        if(progress != NULL) {
            u64 received = http_parallel_received(p);
            shown = received > shown ? received : shown;
            progress(userData, p->size, shown, p->workerCount);
        }
    }

    speedlog_end(trace, R_SUCCEEDED(res));
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

    for(u32 i = 0; i < p->window; i++) {
        free(p->slots[i]);
    }

    if(http_log != NULL) {
        double secs = (osGetTime() - started) / 1000.0;
        http_log("http: %lu connections %s: %llu of %llu bytes in %.1f s (%.0f KB/s), %lu segments asked for twice, result %08lX\n",
                 p->workerCount, url, delivered, p->size, secs, secs > 0 ? delivered / 1024 / secs : 0, p->hedges, res);
    }

    *handed = delivered;
    free(p);
    return res;
}

Result http_download_callback(const char* url, u32 bufferSize, void* userData, Result (*callback)(void* userData, void* buffer, size_t size),
                                                                               Result (*checkRunning)(void* userData),
                                                                               Result (*progress)(void* userData, u64 total, u64 curr, u32 connections)) {
    http_shared* shared = (http_shared*) calloc(1, sizeof(http_shared));
    if(shared == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    http_shared_open(shared);

    // A big file from a server that serves ranges: several connections.
    char* effective = (char*) malloc(1024);
    u64 size = 0;
    if(effective != NULL && http_probe(url, effective, 1024, &size, NULL, NULL, shared) && size >= HTTP_PARALLEL_MIN) {
        u64 handed = 0;
        Result res = http_parallel_download(effective, url, size, userData, callback, checkRunning, progress, shared, &handed);
        if(res == HTTP_RES_ONE_LINK) {
            bool connected = false;
            res = http_curl_download(url, handed, bufferSize, userData, callback, checkRunning, progress, &connected, shared);
        }

        free(effective);
        http_shared_close(shared);
        free(shared);
        return res;
    }

    free(effective);

    bool connected = false;
    Result res = http_curl_download(url, 0, bufferSize, userData, callback, checkRunning, progress, &connected, shared);
    http_shared_close(shared);
    free(shared);

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

// ---- Saving to a file: each piece written where it goes ----
//
// When the caller can write anywhere in its file (saving, not installing), a
// big file comes as 3dsDL fetches it: the file is split among the
// connections, each fetches its piece in one request and writes it where it
// goes, and a connection that is done takes half of what is left of the
// biggest piece. One request a piece, not one a 512 KB segment: links that
// redirect to an address good for one request (Dropbox) cost a redirect a
// piece. With segments every one went through the redirects to a new host,
// and the console ran out of connections (curl 7).
//
// The connections ask where the redirects ended until it refuses them (an
// HTTP error), then at the address given. A connection that fails there is
// opened again a moment later (HTTP_SCATTER_RECONNECTS in all); one the
// server refused makes the download keep the connections still open as its
// most (some servers turn extra ones away), as 3dsDL does.
//
// It starts with two connections and opens another while the last one made it
// HTTP_SCATTER_RAMP_GAIN faster (measured over HTTP_SCATTER_RAMP_MS, from
// HTTP_SCATTER_RAMP_SETTLE_MS after it opened: its handshake and redirects
// would count against it); the first that does not is the last. A fast server
// (Dropbox: ~1000 KB/s on two) keeps two and leaves the console's Wi-Fi room
// for the FTP and the web page, which eight saturated for less (790 KB/s); a
// slow one per connection (catbox.moe) gets more (five).

#define HTTP_SCATTER_START 2
#define HTTP_SCATTER_RAMP_MS 5000
#define HTTP_SCATTER_RAMP_SETTLE_MS 1500
#define HTTP_SCATTER_RAMP_GAIN 115              // Percent of the speed before.
#define HTTP_SCATTER_PIECES 64
#define HTTP_SCATTER_MIN_PIECE (1024 * 1024)   // Pieces are split only if both halves are at least this.
#define HTTP_SCATTER_ALIGN (64 * 1024)         // Where they are split: the card's writes stay aligned.
#define HTTP_SCATTER_BUFFER (256 * 1024)       // Each connection writes this much at a time.
#define HTTP_SCATTER_RECONNECTS 16

typedef struct {
    u64 next;      // [next, end) is left; what is before next has come.
    u64 end;
    bool taken;    // A connection is on it.
} http_piece;

typedef struct http_scatter_s http_scatter;

typedef struct {
    http_scatter* s;
    u32 index;
    CURL* curl;
    u8* buf;
    u32 len;
    u64 bufStart;            // Where buf[0] goes in the file.
    s32 piece;
    bool cut;                // Stopped at its piece's end on purpose (the piece was split).
    bool badRange;           // The server answered with something else than the range.
    char error[CURL_ERROR_SIZE];   // curl's own words for the last failure.
    volatile u64 written;    // Only this worker writes it; the caller adds them up.
    Thread thread;
} http_scatter_worker;

struct http_scatter_s {
    char url[1024];          // The address given.
    char direct[1024];       // Where the redirects ended; empty once it refused a request.
    u64 size;
    http_piece pieces[HTTP_SCATTER_PIECES];
    u32 pieceCount;
    u32 maxConnections;      // The most the server takes.
    volatile u32 level;      // The connections opened so far (HTTP_SCATTER_RAMP_MS).
    u32 active;              // Connections in a request.
    u32 reconnectsLeft;
    LightLock lock;          // Around the pieces and the counts above.
    LightLock diskLock;      // One write to the file at a time.

    void* userData;
    Result (*writeAt)(void* userData, u64 offset, void* buffer, size_t size);
    speedlog* trace;

    u32 workerCount;
    http_scatter_worker workers[HTTP_PARALLEL];
    volatile u32 running;    // Workers not yet finished.

    volatile bool abort;
    volatile Result failure;
};

static bool http_scatter_done(http_scatter* s) {
    for(u32 i = 0; i < s->pieceCount; i++) {
        if(s->pieces[i].next < s->pieces[i].end) {
            return false;
        }
    }

    return true;
}

// Under the lock: a piece nobody is on with something left; else half of the biggest one in flight; else -1.
static s32 http_scatter_take(http_scatter* s) {
    for(u32 i = 0; i < s->pieceCount; i++) {
        if(!s->pieces[i].taken && s->pieces[i].next < s->pieces[i].end) {
            s->pieces[i].taken = true;
            return (s32) i;
        }
    }

    if(s->pieceCount >= HTTP_SCATTER_PIECES) {
        return -1;
    }

    s32 biggest = -1;
    u64 left = 0;
    for(u32 i = 0; i < s->pieceCount; i++) {
        http_piece* piece = &s->pieces[i];
        if(piece->taken && piece->end - piece->next > left) {
            biggest = (s32) i;
            left = piece->end - piece->next;
        }
    }

    if(biggest < 0 || left < 2 * HTTP_SCATTER_MIN_PIECE) {
        return -1;
    }

    http_piece* piece = &s->pieces[biggest];
    u64 middle = piece->next + left / 2;
    middle -= middle % HTTP_SCATTER_ALIGN;
    if(middle <= piece->next) {
        return -1;
    }

    http_piece* half = &s->pieces[s->pieceCount];
    half->next = middle;
    half->end = piece->end;
    half->taken = true;
    piece->end = middle;
    return (s32) s->pieceCount++;
}

// What the worker holds goes to the file. False when the file refused it.
static bool http_scatter_flush(http_scatter_worker* w) {
    http_scatter* s = w->s;
    if(w->len == 0) {
        return true;
    }

    u64 start = speedlog_ticks();
    LightLock_Lock(&s->diskLock);
    Result res = s->writeAt(s->userData, w->bufStart, w->buf, w->len);
    LightLock_Unlock(&s->diskLock);
    speedlog_disk(s->trace, w->len, speedlog_ticks() - start);

    if(R_FAILED(res)) {
        if(!s->abort) {
            s->failure = res;
        }

        s->abort = true;
        return false;
    }

    w->written += w->len;
    w->bufStart += w->len;
    w->len = 0;
    return true;
}

static size_t http_scatter_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    http_scatter_worker* w = (http_scatter_worker*) userdata;
    http_scatter* s = w->s;
    size_t len = size * nmemb;
    if(s->abort) {
        return 0;
    }

    // A server that ignores the range would put the file's start here.
    long code = 0;
    if(curl_easy_getinfo(w->curl, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK || code != 206) {
        w->badRange = true;
        return 0;
    }

    // What fits in the piece, which a connection that took half of it may have shortened.
    LightLock_Lock(&s->lock);
    http_piece* piece = &s->pieces[w->piece];
    u64 room = piece->end > piece->next ? piece->end - piece->next : 0;
    size_t take = len < room ? len : (size_t) room;
    piece->next += take;
    LightLock_Unlock(&s->lock);

    for(size_t done = 0; done < take;) {
        if(w->len == HTTP_SCATTER_BUFFER && !http_scatter_flush(w)) {
            return 0;
        }

        size_t n = take - done < HTTP_SCATTER_BUFFER - w->len ? take - done : HTTP_SCATTER_BUFFER - w->len;
        memcpy(w->buf + w->len, ptr + done, n);
        w->len += n;
        done += n;
    }

    if(take < len) {
        w->cut = true;
        return 0;
    }

    return len;
}

static int http_scatter_info(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    http_scatter_worker* w = (http_scatter_worker*) clientp;
    return w->s->abort || http_quit ? 1 : 0;
}

static void http_scatter_thread(void* arg) {
    http_scatter_worker* w = (http_scatter_worker*) arg;
    http_scatter* s = w->s;

    while(!s->abort && !http_quit) {
        LightLock_Lock(&s->lock);
        bool done = http_scatter_done(s);
        bool allowed = w->index < s->maxConnections && w->index < s->level;
        s32 piece = !done && allowed ? http_scatter_take(s) : -1;
        u64 from = 0;
        u64 to = 0;
        bool direct = s->direct[0] != '\0';
        if(piece >= 0) {
            s->active++;
            from = s->pieces[piece].next;
            to = s->pieces[piece].end;
        }

        LightLock_Unlock(&s->lock);

        if(done) {
            break;
        }

        // Not opened (yet), or nothing to take: the others are on the last pieces, and one that fails frees its own.
        if(piece < 0) {
            svcSleepThread(allowed ? 20000000 : 100000000);
            continue;
        }

        char range[64];
        snprintf(range, sizeof(range), "%llu-%llu", from, to - 1);
        w->piece = piece;
        w->len = 0;
        w->bufStart = from;
        w->cut = false;
        w->badRange = false;
        w->error[0] = '\0';
        curl_easy_setopt(w->curl, CURLOPT_URL, direct ? s->direct : s->url);
        curl_easy_setopt(w->curl, CURLOPT_RANGE, range);
        CURLcode ret = http_perform(w->curl);
        long code = 0;
        curl_easy_getinfo(w->curl, CURLINFO_RESPONSE_CODE, &code);

        // What came is good, even from a request that failed afterwards.
        bool flushed = http_scatter_flush(w);

        LightLock_Lock(&s->lock);
        s->active--;
        s->pieces[piece].taken = false;
        bool complete = s->pieces[piece].next >= s->pieces[piece].end;
        LightLock_Unlock(&s->lock);

        if(!flushed || s->abort || http_quit) {
            break;
        }

        if(w->cut || complete) {
            continue;
        }

        if(w->badRange) {
            s->failure = code >= 200 && code < 300 ? R_APP_HTTP_ERROR_BASE + 416 : R_APP_HTTP_ERROR_BASE + code;
            s->abort = true;
            if(http_log != NULL) {
                http_log("http: piece at %llu: http %ld instead of the range\n", from, code);
            }

            break;
        }

        ret = ret == CURLE_OK ? CURLE_PARTIAL_FILE : ret;
        bool refused = ret == CURLE_HTTP_RETURNED_ERROR;

        // Where the redirects ended served once (Dropbox) or expired: the address given, at no cost.
        if(refused && direct) {
            LightLock_Lock(&s->lock);
            bool first = s->direct[0] != '\0';
            s->direct[0] = '\0';
            LightLock_Unlock(&s->lock);
            if(first && http_log != NULL) {
                http_log("http: http %ld where the redirects ended, asking at the address given\n", code);
            }

            continue;
        }

        LightLock_Lock(&s->lock);
        bool again = s->reconnectsLeft > 0;
        if(again) {
            s->reconnectsLeft--;
            // A server that turns this one away: as many as are still open, at most.
            if(refused && s->active > 0 && s->active < s->maxConnections) {
                s->maxConnections = s->active;
            }
        }

        LightLock_Unlock(&s->lock);

        if(http_log != NULL) {
            http_log("http: piece at %llu failed: curl %d, http %ld (%s)%s\n", from, (int) ret, code, w->error, again ? ", trying again" : "");
        }

        if(!again) {
            s->failure = refused ? R_APP_HTTP_ERROR_BASE + code : R_APP_CURL_ERROR_BASE + ret;
            s->abort = true;
            break;
        }

        svcSleepThread(500000000);
    }

    __sync_fetch_and_sub(&s->running, 1);
}

static u64 http_scatter_written(http_scatter* s) {
    u64 total = 0;
    for(u32 i = 0; i < s->workerCount; i++) {
        total += s->workers[i].written;
    }

    return total;
}

// [start, size) of the file at `url` (`direct` where its redirects end), each piece written where it goes.
static Result http_scatter_download(const char* url, const char* direct, u64 start, u64 size, void* userData,
                                    Result (*writeAt)(void* userData, u64 offset, void* buffer, size_t size),
                                    Result (*checkRunning)(void* userData), Result (*progress)(void* userData, u64 total, u64 curr, u32 connections),
                                    http_shared* shared) {
    http_scatter* s = (http_scatter*) calloc(1, sizeof(http_scatter));
    if(s == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    string_copy(s->url, url, sizeof(s->url));
    string_copy(s->direct, direct, sizeof(s->direct));
    s->size = size;
    s->userData = userData;
    s->writeAt = writeAt;
    s->reconnectsLeft = HTTP_SCATTER_RECONNECTS;
    LightLock_Init(&s->lock);
    LightLock_Init(&s->diskLock);

    // The file split among the connections, on the alignment.
    u64 left = size - start;
    u32 count = HTTP_PARALLEL;
    while(count > 1 && left / count < HTTP_SCATTER_MIN_PIECE) {
        count--;
    }

    u64 at = start;
    for(u32 i = 0; i < count; i++) {
        u64 end = i + 1 == count ? size : start + left / count * (i + 1);
        end -= i + 1 == count ? 0 : end % HTTP_SCATTER_ALIGN;
        s->pieces[i].next = at;
        s->pieces[i].end = end > at ? end : at;
        at = s->pieces[i].end;
    }

    s->pieceCount = count;
    s->workerCount = count;
    s->maxConnections = count;
    s->level = count < HTTP_SCATTER_START ? count : HTTP_SCATTER_START;

    bool isNew = false;
    APT_CheckNew3DS(&isNew);

    Result res = 0;
    for(u32 i = 0; i < s->workerCount && R_SUCCEEDED(res); i++) {
        http_scatter_worker* w = &s->workers[i];
        w->s = s;
        w->index = i;
        if((w->buf = (u8*) malloc(HTTP_SCATTER_BUFFER)) == NULL) {
            res = R_APP_OUT_OF_MEMORY;
        } else if((w->curl = curl_easy_init()) == NULL) {
            res = R_APP_CURL_INIT_FAILED;
        } else {
            http_curl_common(w->curl, s->url, shared);
            curl_easy_setopt(w->curl, CURLOPT_BUFFERSIZE, (long) HTTP_CHUNK_MAX);
            curl_easy_setopt(w->curl, CURLOPT_FORBID_REUSE, 1L);
            curl_easy_setopt(w->curl, CURLOPT_ERRORBUFFER, w->error);
            curl_easy_setopt(w->curl, CURLOPT_WRITEFUNCTION, http_scatter_write);
            curl_easy_setopt(w->curl, CURLOPT_WRITEDATA, (void*) w);
            curl_easy_setopt(w->curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(w->curl, CURLOPT_XFERINFOFUNCTION, http_scatter_info);
            curl_easy_setopt(w->curl, CURLOPT_XFERINFODATA, (void*) w);
        }
    }

    s->trace = speedlog_begin("url", url, size);
    for(u32 i = 0; i < s->workerCount && R_SUCCEEDED(res); i++) {
        http_scatter_worker* w = &s->workers[i];
        __sync_fetch_and_add(&s->running, 1);
        w->thread = isNew ? threadCreate(http_scatter_thread, w, HTTP_FETCH_STACK, HTTP_FETCH_PRIORITY, 2, false) : NULL;
        if(w->thread == NULL) {
            w->thread = threadCreate(http_scatter_thread, w, HTTP_FETCH_STACK, HTTP_FETCH_PRIORITY, -2, false);
        }

        if(w->thread == NULL) {
            __sync_fetch_and_sub(&s->running, 1);
            res = R_APP_THREAD_CREATE_FAILED;
        }
    }

    u64 started = osGetTime();
    u64 lastLog = started;
    u64 lastWritten = 0;
    u64 traced = 0;
    u64 rampAt = 0;         // When the next measure begins (0: at the first data).
    u64 rampStart = 0;      // When this one began (0: not yet).
    u64 rampWritten = 0;
    u32 rampRate = 0;
    bool ramping = true;
    while(R_SUCCEEDED(res) && s->running > 0) {
        u64 written = http_scatter_written(s);

        // Another connection while the last one paid off.
        u64 now = osGetTime();
        if(ramping && written > 0 && rampStart == 0 && now >= rampAt) {
            rampStart = now;
            rampWritten = written;
        } else if(ramping && rampStart != 0 && now - rampStart >= HTTP_SCATTER_RAMP_MS) {
            u32 rate = (u32) ((written - rampWritten) * 1000 / (now - rampStart) / 1024);
            u32 level = s->level;
            u32 before = rampRate;
            ramping = (before == 0 || rate * 100 >= before * HTTP_SCATTER_RAMP_GAIN) && level < s->workerCount && level < s->maxConnections;
            if(ramping) {
                s->level = level + 1;
                rampRate = rate;
            }

            if(http_log != NULL) {
                http_log("http: %lu KB/s over %lu connections (%lu KB/s with one less): %s\n", rate, level, before, ramping ? "one more" : "keeping them");
            }

            rampAt = now + HTTP_SCATTER_RAMP_SETTLE_MS;
            rampStart = 0;
        }

        if(written > traced) {
            speedlog_net(s->trace, written - traced, 0);
            traced = written;
        }

        speedlog_gauge(s->trace, s->active);

        if(checkRunning != NULL && R_FAILED(res = checkRunning(userData))) {
            break;
        }

        if(progress != NULL) {
            progress(userData, size, start + written, s->active);
        }

        if(http_log != NULL && osGetTime() - lastLog >= 10000) {
            http_log("http: %llu KB written, %llu KB/s over %lu connections, %lu pieces\n", (start + written) / 1024,
                     written > lastWritten ? (written - lastWritten) / 1024 * 1000 / (osGetTime() - lastLog) : 0, s->active, s->pieceCount);
            lastWritten = written;
            lastLog = osGetTime();
        }

        u64 waitStart = speedlog_ticks();
        svcSleepThread(100000000);
        speedlog_wait(s->trace, speedlog_ticks() - waitStart);
    }

    s->abort = true;
    for(u32 i = 0; i < s->workerCount; i++) {
        http_scatter_worker* w = &s->workers[i];
        if(w->thread != NULL) {
            threadJoin(w->thread, U64_MAX);
            threadFree(w->thread);
        }

        if(w->curl != NULL) {
            curl_easy_cleanup(w->curl);
        }

        free(w->buf);
    }

    if(R_SUCCEEDED(res) && !http_scatter_done(s)) {
        res = s->failure != 0 ? s->failure : R_APP_CANCELLED;
    }

    speedlog_end(s->trace, R_SUCCEEDED(res));

    if(http_log != NULL) {
        double secs = (osGetTime() - started) / 1000.0;
        u64 written = http_scatter_written(s);
        http_log("http: %lu connections (%lu opened, %lu taken) %s: %llu of %llu bytes in %.1f s (%.0f KB/s), %lu pieces, result %08lX\n",
                 s->workerCount, s->level, s->maxConnections, url, start + written, size, secs, secs > 0 ? written / 1024 / secs : 0, s->pieceCount, res);
    }

    free(s);
    return res;
}

typedef struct {
    void* userData;
    Result (*open)(void* userData, void* first, size_t size, u64 total);
    Result (*writeAt)(void* userData, u64 offset, void* buffer, size_t size);
    Result (*checkRunning)(void* userData);
    Result (*progress)(void* userData, u64 total, u64 curr, u32 connections);
    bool opened;
    u64 offset;
} http_file_order;

// One connection after all: the file in order, opened at its first bytes.
static Result http_file_in_order(void* userData, void* buffer, size_t size) {
    http_file_order* order = (http_file_order*) userData;
    Result res = 0;
    if(!order->opened) {
        order->opened = true;
        if(R_FAILED(res = order->open(order->userData, buffer, size, 0))) {
            return res;
        }
    }

    if(R_SUCCEEDED(res = order->writeAt(order->userData, order->offset, buffer, size))) {
        order->offset += size;
    }

    return res;
}

static Result http_file_in_order_running(void* userData) {
    http_file_order* order = (http_file_order*) userData;
    return order->checkRunning != NULL ? order->checkRunning(order->userData) : 0;
}

static Result http_file_in_order_progress(void* userData, u64 total, u64 curr, u32 connections) {
    http_file_order* order = (http_file_order*) userData;
    return order->progress != NULL ? order->progress(order->userData, total, curr, connections) : 0;
}

Result http_download_file(const char* url, u32 firstSize, void* userData, Result (*open)(void* userData, void* first, size_t size, u64 total),
                          Result (*writeAt)(void* userData, u64 offset, void* buffer, size_t size),
                          Result (*checkRunning)(void* userData), Result (*progress)(void* userData, u64 total, u64 curr, u32 connections)) {
    http_shared* shared = (http_shared*) calloc(1, sizeof(http_shared));
    char* effective = (char*) malloc(1024);
    u8* first = (u8*) malloc(firstSize);
    if(shared == NULL || effective == NULL || first == NULL) {
        free(shared);
        free(effective);
        free(first);
        return R_APP_OUT_OF_MEMORY;
    }

    http_shared_open(shared);

    // The first bytes name the file (a CIA's title) before it is opened.
    u64 size = 0;
    u32 got = firstSize;
    Result res = 0;
    bool scatter = http_probe(url, effective, 1024, &size, first, &got, shared) && size >= HTTP_PARALLEL_MIN;
    if(scatter) {
        if(R_SUCCEEDED(res = open(userData, first, got, size)) && R_SUCCEEDED(res = writeAt(userData, 0, first, got))) {
            res = http_scatter_download(url, effective, got, size, userData, writeAt, checkRunning, progress, shared);
        }
    }

    free(first);
    free(effective);
    http_shared_close(shared);
    free(shared);

    if(!scatter) {
        http_file_order order = {userData, open, writeAt, checkRunning, progress, false, 0};
        res = http_download_callback(url, firstSize, &order, http_file_in_order, http_file_in_order_running, http_file_in_order_progress);
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