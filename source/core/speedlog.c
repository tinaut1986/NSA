#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <3ds.h>

#include "speedlog.h"
#include "stringutil.h"

#define SPEEDLOG_PATH "sdmc:/nsa/speed-log.txt"
#define SPEEDLOG_FLAG "/nsa/speed.flag"
#define SPEEDLOG_FILE_MAX (2 * 1024 * 1024)
// About 100-120 bytes a second: a 2 GB file at ~350 KB/s (the 2DS) is about 6000 s.
#define SPEEDLOG_TEXT_MAX (768 * 1024)
#define SPEEDLOG_EDGE 10   // Seconds averaged at the start and at the end.

struct speedlog {
    char kind[16];
    char name[128];
    u64 total;
    u64 start;
    u64 sampled;           // When the last line was taken.

    // The second under way.
    u64 bytes;
    u64 wait;
    u64 net;
    u64 disk;
    u64 diskMax;
    u32 waits;
    u32 netCalls;
    u32 diskCalls;
    u32 frames;
    u32 gauge;

    // The whole transfer.
    u64 allBytes;
    u64 allDiskBytes;
    u64 allWait;
    u64 allNet;
    u64 allDisk;
    u64 allDiskMax;
    u32 lines;
    u32 slowest;           // KB/s of the slowest full second.
    u32 fastest;
    u64 edgeBytes;         // Moved in the first SPEEDLOG_EDGE seconds.
    u64 edgeTicks;
    u64 tailBytes[SPEEDLOG_EDGE];
    u64 tailTicks[SPEEDLOG_EDGE];

    char* text;
    size_t len;
    bool cut;
};

static LightLock speedlog_lock;
static volatile u32 speedlog_frames = 0;
static void (*speedlog_context)(char* out, size_t size) = NULL;

static u32 speedlog_ms(u64 ticks) {
    return (u32) (ticks * 1000 / SYSCLOCK_ARM11);
}

void speedlog_init() {
    LightLock_Init(&speedlog_lock);
}

void speedlog_frame() {
    speedlog_frames++;
}

void speedlog_set_context(void (*context)(char* out, size_t size)) {
    speedlog_context = context;
}

static bool speedlog_on() {
    if(NSA_DEBUG) {
        return true;
    }

    Handle file = 0;
    if(R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, SPEEDLOG_FLAG), FS_OPEN_READ, 0))) {
        FSFILE_Close(file);
        return true;
    }

    return false;
}

static void speedlog_append(speedlog* l, const char* format, ...) {
    if(l->cut) {
        return;
    }

    char line[256];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    if(n <= 0) {
        return;
    }

    if((size_t) n >= sizeof(line)) {
        n = sizeof(line) - 1;
    }

    if(l->len + (size_t) n + 1 > SPEEDLOG_TEXT_MAX) {
        l->cut = true;
        return;
    }

    memcpy(l->text + l->len, line, (size_t) n);
    l->len += (size_t) n;
    l->text[l->len] = '\0';
}

speedlog* speedlog_begin(const char* kind, const char* name, u64 total) {
    if(!speedlog_on()) {
        return NULL;
    }

    speedlog* l = (speedlog*) calloc(1, sizeof(speedlog));
    if(l == NULL) {
        return NULL;
    }

    if((l->text = (char*) malloc(SPEEDLOG_TEXT_MAX)) == NULL) {
        free(l);
        return NULL;
    }

    string_copy(l->kind, kind, sizeof(l->kind));
    string_copy(l->name, name != NULL ? name : "", sizeof(l->name));
    l->total = total;
    l->start = l->sampled = speedlog_ticks();
    l->frames = speedlog_frames;
    l->slowest = UINT32_MAX;
    return l;
}

// A line for the second just gone: t, its length, KB/s, then where the time went.
static void speedlog_sample(speedlog* l, u64 now) {
    u64 ticks = now - l->sampled;
    u32 kbs = ticks > 0 ? (u32) (l->bytes * SYSCLOCK_ARM11 / ticks / 1024) : 0;

    char context[64] = "";
    if(speedlog_context != NULL) {
        speedlog_context(context, sizeof(context));
    }

    u32 frames = speedlog_frames;
    speedlog_append(l, "s,%.1f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%s\n",
                    (double) (now - l->start) / SYSCLOCK_ARM11, speedlog_ms(ticks), kbs, speedlog_ms(l->wait), l->waits,
                    speedlog_ms(l->net), l->netCalls, l->netCalls > 0 ? (u32) (l->bytes / l->netCalls) : 0,
                    speedlog_ms(l->disk), speedlog_ms(l->diskMax), l->diskCalls, frames - l->frames, l->gauge, context);

    if(l->lines < SPEEDLOG_EDGE) {
        l->edgeBytes += l->bytes;
        l->edgeTicks += ticks;
    }

    l->tailBytes[l->lines % SPEEDLOG_EDGE] = l->bytes;
    l->tailTicks[l->lines % SPEEDLOG_EDGE] = ticks;
    l->lines++;
    if(kbs < l->slowest) {
        l->slowest = kbs;
    }

    if(kbs > l->fastest) {
        l->fastest = kbs;
    }

    l->sampled = now;
    l->bytes = l->wait = l->net = l->disk = l->diskMax = 0;
    l->waits = l->netCalls = l->diskCalls = 0;
    l->frames = frames;
}

static void speedlog_tick(speedlog* l) {
    u64 now = speedlog_ticks();
    if(now - l->sampled >= SYSCLOCK_ARM11) {
        speedlog_sample(l, now);
    }
}

void speedlog_wait(speedlog* l, u64 ticks) {
    if(l == NULL) {
        return;
    }

    l->wait += ticks;
    l->allWait += ticks;
    l->waits++;
    speedlog_tick(l);
}

void speedlog_net(speedlog* l, u64 bytes, u64 ticks) {
    if(l == NULL) {
        return;
    }

    l->bytes += bytes;
    l->allBytes += bytes;
    l->net += ticks;
    l->allNet += ticks;
    l->netCalls++;
    speedlog_tick(l);
}

void speedlog_disk(speedlog* l, u64 bytes, u64 ticks) {
    if(l == NULL) {
        return;
    }

    l->allDiskBytes += bytes;
    l->disk += ticks;
    l->allDisk += ticks;
    l->diskCalls++;
    if(ticks > l->diskMax) {
        l->diskMax = ticks;
    }

    if(ticks > l->allDiskMax) {
        l->allDiskMax = ticks;
    }

    speedlog_tick(l);
}

void speedlog_gauge(speedlog* l, u32 value) {
    if(l != NULL) {
        l->gauge = value;
    }
}

static u32 speedlog_kbs(u64 bytes, u64 ticks) {
    return ticks > 0 ? (u32) (bytes * SYSCLOCK_ARM11 / ticks / 1024) : 0;
}

void speedlog_end(speedlog* l, bool ok) {
    if(l == NULL) {
        return;
    }

    u64 now = speedlog_ticks();
    // The last part second gets its line too when the transfer had any.
    if(l->lines > 0 && now - l->sampled >= SYSCLOCK_ARM11 / 10) {
        speedlog_sample(l, now);
    }

    u64 ticks = now - l->start;
    u64 tailBytes = 0, tailTicks = 0;
    for(u32 i = 0; i < SPEEDLOG_EDGE && i < l->lines; i++) {
        tailBytes += l->tailBytes[i];
        tailTicks += l->tailTicks[i];
    }

    time_t clock = time(NULL);
    struct tm* t = gmtime(&clock);
    bool isNew = false;
    APT_CheckNew3DS(&isNew);

    char head[400];
    snprintf(head, sizeof(head), "# %04d-%02d-%02d %02d:%02d:%02d %s %s %s %s total=%llu\n"
                                 "# s,t,ms,KB/s,wait_ms,waits,net_ms,net_calls,bytes_per_call,disk_ms,disk_max_ms,disk_calls,frames,gauge,context\n",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, VERSION_STRING, isNew ? "new3ds" : "old3ds",
             l->kind, l->name, (unsigned long long) l->total);

    char tail[400];
    snprintf(tail, sizeof(tail), "e,%s,%s,%s,%llu,%.2f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%llu,%s\n"
                                 "# e,kind,name,ok,bytes,secs,KB/s,first10_KB/s,last10_KB/s,slowest,fastest,wait_ms,net_ms,disk_ms,disk_max_ms,disk_bytes,cut\n\n",
             l->kind, l->name, ok ? "ok" : "failed", (unsigned long long) l->allBytes, (double) ticks / SYSCLOCK_ARM11, speedlog_kbs(l->allBytes, ticks),
             speedlog_kbs(l->edgeBytes, l->edgeTicks), speedlog_kbs(tailBytes, tailTicks), l->lines > 0 ? l->slowest : 0, l->fastest,
             speedlog_ms(l->allWait), speedlog_ms(l->allNet), speedlog_ms(l->allDisk), speedlog_ms(l->allDiskMax),
             (unsigned long long) l->allDiskBytes, l->cut ? "cut" : "");

    LightLock_Lock(&speedlog_lock);
    FILE* f = fopen(SPEEDLOG_PATH, "ab");
    if(f != NULL && fseek(f, 0, SEEK_END) == 0 && ftell(f) > SPEEDLOG_FILE_MAX) {
        fclose(f);
        f = fopen(SPEEDLOG_PATH, "wb");
    }

    if(f != NULL) {
        fputs(head, f);
        fwrite(l->text, 1, l->len, f);
        fputs(tail, f);
        fclose(f);
    }
    LightLock_Unlock(&speedlog_lock);

    free(l->text);
    free(l);
}
