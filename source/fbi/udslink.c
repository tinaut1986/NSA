#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <3ds.h>

#include "ftpserver.h"
#include "webserver.h"
#include "relstream.h"
#include "udslink.h"
#include "../core/core.h"

#define UDS_WLANCOMM_ID 0x4E534131
#define UDS_ID8 0x4E
#define UDS_CHANNEL 1
#define UDS_PASSPHRASE "nsa-local"
#define UDS_SHARED_SIZE 0x30000
#define UDS_RECV_SIZE 0x20000
#define UDS_PUMP_PRIORITY 0x31

static struct {
    bool active;
    u64 autoLeaveAt;     // Tests (nearby.flag auto): when this session closes itself; 0 = never.
    bool hosting;
    udsBindContext hostBind;
    bool hostBound;
    LightLock lock;

    // Scanning.
    Thread scanThread;
    volatile bool scanRun;
    udslink_peer found[UDSLINK_PEERS_MAX];
    u32 foundCount;
} uds;

typedef struct {
    udslink* link;
} io_user;

struct udslink {
    io_user ioUser;
    rs* stream;
    udsBindContext bind;
    bool ownsBind;       // A client binds when it connects; the host's bind is shared.
    udsBindContext* use;
    bool host;
    Thread pump;
    volatile bool stop;
    volatile bool dead;
    LightLock lock;
};

// sdmc:/nsa/nearby.flag, for tests: the Wi-Fi is taken as off, so the local
// wireless is used. If it says "auto" the transfer goes ahead without asking.
static int flag_state = 0;
static u64 flag_read_at = 0;

static void read_flag() {
    flag_state = 0;
    flag_read_at = osGetTime();

    Handle file = 0;
    if(R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, "/nsa/nearby.flag"), FS_OPEN_READ, 0))) {
        char text[16] = {0};
        u32 read = 0;
        FSFILE_Read(file, &read, 0, text, sizeof(text) - 1);
        FSFILE_Close(file);

        // "auto": the Wi-Fi is taken as off and there are no questions. "autosend": no
        // questions either, but the Wi-Fi counts (the sender still has to pick "Search nearby").
        flag_state = strncmp(text, "autosend", 8) == 0 ? 3 : strncmp(text, "auto", 4) == 0 ? 2 : 1;
    }
}

// The file is read again every few seconds, so taking it away takes effect (it was
// once read only at start, and a console went on without Wi-Fi after it was deleted).
static void refresh_flag() {
    if(flag_read_at == 0 || osGetTime() - flag_read_at > 3000) {
        read_flag();
    }
}

bool udslink_forced() {
    refresh_flag();

    return flag_state == 1 || flag_state == 2;
}

bool udslink_auto() {
    refresh_flag();

    return flag_state == 2 || flag_state == 3;
}

// Every send and receive leaves lines in sdmc:/nsa/transfer-log.txt, to read after
// an error. It starts over when it passes 96 KB.
void udslink_log(const char* format, ...) {
    char text[240];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    time_t now = time(NULL);
    struct tm* t = gmtime(&now);
    char line[300];
    snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d %s", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, text);

    Handle file = 0;
    if(R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, "/nsa/transfer-log.txt"), FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        u64 size = 0;
        FSFILE_GetSize(file, &size);
        if(size > 96 * 1024) {
            FSFILE_SetSize(file, 0);
            size = 0;
        }

        u32 written = 0;
        FSFILE_Write(file, &written, size, line, strlen(line), FS_WRITE_FLUSH);
        FSFILE_Close(file);
    }
}

bool udslink_wifi_connected() {
    if(udslink_forced()) {
        return false;
    }

    u32 status = 0;
    return R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0;
}

// For tests: the local wireless is closed after a while whatever the screens
// are doing, so the Wi-Fi (and the FTP server) come back. On the main thread,
// and only the session that set the time: a later one has its own.
void udslink_poll_main() {
    if(uds.active && uds.autoLeaveAt != 0 && osGetTime() >= uds.autoLeaveAt) {
        udslink_log("uds: closed by the nearby.flag timeout\n");
        udslink_leave();
    }
}

bool udslink_enter() {
    if(uds.active) {
        return true;
    }

    // The Wi-Fi goes away with UDS: the FTP server with it.
    ftpserver_pause();
    webserver_pause();

    if(R_FAILED(udsInit(UDS_SHARED_SIZE, NULL))) {
        ftpserver_resume_later();
        webserver_resume_later();
        return false;
    }

    LightLock_Init(&uds.lock);
    uds.active = true;

    uds.autoLeaveAt = udslink_auto() ? osGetTime() + 150000 : 0;

    return true;
}

void udslink_leave() {
    if(!uds.active) {
        return;
    }

    udslink_scan_stop();
    udslink_host_stop();
    udsExit();
    uds.active = false;
    uds.autoLeaveAt = 0;

    // The system brings the Wi-Fi back a few seconds after UDS closes: the FTP
    // server starts again once it has.
    ftpserver_resume_later();
    webserver_resume_later();
}

bool udslink_active() {
    return uds.active;
}

// ---- the stream over the network ----

static bool io_send(void* user, const uint8_t* data, size_t len) {
    udslink* link = ((io_user*) user)->link;

    Result res = udsSendTo(link->host ? UDS_BROADCAST_NETWORKNODEID : UDS_HOST_NETWORKNODEID, UDS_CHANNEL, UDS_SENDFLAG_Default, data, len);
    if(UDS_CHECK_SENDTO_FATALERROR(res)) {
        link->dead = true;
        return false;
    }

    return R_SUCCEEDED(res);
}

static int io_recv(void* user, uint8_t* buffer, size_t max) {
    udslink* link = ((io_user*) user)->link;

    size_t actual = 0;
    u16 src = 0;
    if(R_FAILED(udsPullPacket(link->use, buffer, max, &actual, &src))) {
        return 0;
    }

    return (int) actual;
}

static uint64_t io_now(void* user) {
    (void) user;
    return osGetTime();
}

static void pump_thread(void* arg) {
    udslink* link = (udslink*) arg;

    while(!link->stop) {
        LightLock_Lock(&link->lock);
        rs_poll(link->stream);
        bool failed = rs_failed(link->stream);
        LightLock_Unlock(&link->lock);

        if(failed) {
            link->dead = true;
        }

        svcSleepThread(3000000);
    }
}

static udslink* link_create(udsBindContext* bind, bool host) {
    udslink* link = (udslink*) calloc(1, sizeof(udslink));
    if(link == NULL) {
        return NULL;
    }

    LightLock_Init(&link->lock);
    link->host = host;
    link->use = bind;
    link->ioUser.link = link;

    rs_io io = {io_send, io_recv, io_now, &link->ioUser};
    link->stream = rs_create(&io);
    if(link->stream == NULL) {
        free(link);
        return NULL;
    }

    link->pump = threadCreate(pump_thread, link, 0x4000, UDS_PUMP_PRIORITY, -2, false);
    if(link->pump == NULL) {
        rs_free(link->stream);
        free(link);
        return NULL;
    }

    return link;
}

// ---- hosting ----

Result udslink_host_start(const char* name) {
    if(!uds.active) {
        return R_APP_INVALID_ARGUMENT;
    }

    udsNetworkStruct network;
    udsGenerateDefaultNetworkStruct(&network, UDS_WLANCOMM_ID, UDS_ID8, 2);

    Result res = udsCreateNetwork(&network, UDS_PASSPHRASE, sizeof(UDS_PASSPHRASE), &uds.hostBind, UDS_CHANNEL, UDS_RECV_SIZE);
    udslink_log("host start: %08lX\n", res);
    if(R_SUCCEEDED(res)) {
        uds.hostBound = true;
        uds.hosting = true;
        udsSetApplicationData(name, strlen(name));
    }

    return res;
}

void udslink_host_stop() {
    if(uds.hosting) {
        if(uds.hostBound) {
            udsUnbind(&uds.hostBind);
            uds.hostBound = false;
        }

        udsDestroyNetwork();
        uds.hosting = false;
    }
}

udslink* udslink_host_accept() {
    if(!uds.hosting) {
        return NULL;
    }

    udsConnectionStatus status;
    if(R_FAILED(udsGetConnectionStatus(&status)) || status.total_nodes < 2) {
        return NULL;
    }

    udslink* link = link_create(&uds.hostBind, true);
    udslink_log("host accept: nodes=%u link=%s\n", status.total_nodes, link != NULL ? "ok" : "failed");
    return link;
}

// ---- joining ----

static void scan_thread(void* arg) {
    (void) arg;

    static u8 buffer[0x4000] __attribute__((aligned(0x1000)));
    while(uds.scanRun) {
        udsNetworkScanInfo* networks = NULL;
        size_t total = 0;

        Result scanRes = udsScanBeacons(buffer, sizeof(buffer), &networks, &total, UDS_WLANCOMM_ID, UDS_ID8, NULL, false);
        static int scans = 0;
        if(scans < 6 || total > 0) {
            udslink_log("scan %d: %08lX found=%u\n", scans, scanRes, (unsigned) total);
        }

        scans++;

        if(R_SUCCEEDED(scanRes)) {
            LightLock_Lock(&uds.lock);
            uds.foundCount = 0;
            for(size_t i = 0; i < total && uds.foundCount < UDSLINK_PEERS_MAX; i++) {
                udslink_peer* peer = &uds.found[uds.foundCount];
                memset(peer, 0, sizeof(*peer));

                size_t actual = 0;
                if(R_FAILED(udsGetNetworkStructApplicationData(&networks[i].network, peer->name, UDSLINK_NAME_MAX, &actual))) {
                    actual = 0;
                }

                peer->name[actual] = '\0';
                if(peer->name[0] == '\0') {
                    snprintf(peer->name, sizeof(peer->name), "3DS");
                }

                if(sizeof(networks[i].network) <= sizeof(peer->network)) {
                    memcpy(peer->network, &networks[i].network, sizeof(networks[i].network));
                    uds.foundCount++;
                }
            }

            LightLock_Unlock(&uds.lock);
        }

        for(int i = 0; i < 10 && uds.scanRun; i++) {
            svcSleepThread(100000000);
        }
    }
}

void udslink_scan_start() {
    if(!uds.active || uds.scanThread != NULL) {
        return;
    }

    uds.scanRun = true;
    uds.foundCount = 0;
    uds.scanThread = threadCreate(scan_thread, NULL, 0x4000, 0x32, -2, false);
    if(uds.scanThread == NULL) {
        uds.scanRun = false;
    }
}

void udslink_scan_stop() {
    if(uds.scanThread != NULL) {
        uds.scanRun = false;
        threadJoin(uds.scanThread, U64_MAX);
        threadFree(uds.scanThread);
        uds.scanThread = NULL;
    }
}

u32 udslink_scan_results(udslink_peer* out, u32 max) {
    LightLock_Lock(&uds.lock);
    u32 count = uds.foundCount < max ? uds.foundCount : max;
    memcpy(out, uds.found, count * sizeof(udslink_peer));
    LightLock_Unlock(&uds.lock);

    return count;
}

Result udslink_connect(const udslink_peer* peer, udslink** out) {
    // Scanning and joining do not go together.
    udslink_scan_stop();

    udsBindContext* bind = (udsBindContext*) calloc(1, sizeof(udsBindContext));
    if(bind == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = udsConnectNetwork((const udsNetworkStruct*) peer->network, UDS_PASSPHRASE, sizeof(UDS_PASSPHRASE), bind, UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, UDS_CHANNEL, UDS_RECV_SIZE);
    udslink_log("connect: %08lX\n", res);
    if(R_FAILED(res)) {
        free(bind);
        return res;
    }

    udslink* link = link_create(bind, false);
    if(link == NULL) {
        udsUnbind(bind);
        udsDisconnectNetwork();
        free(bind);
        return R_APP_OUT_OF_MEMORY;
    }

    link->ownsBind = true;
    *out = link;
    return 0;
}

// ---- the connection ----

Result udslink_io(udslink* link, bool write, void* buffer, size_t len, Handle cancel, volatile bool* stop) {
    size_t done = 0;
    u64 last = osGetTime();

    while(done < len) {
        if((cancel != 0 && svcWaitSynchronization(cancel, 0) == 0) || (stop != NULL && *stop)) {
            return R_APP_CANCELLED;
        }

        if(link->dead) {
            return R_APP_IO_ERROR;
        }

        LightLock_Lock(&link->lock);
        size_t n = write ? rs_write(link->stream, (u8*) buffer + done, len - done) : rs_read(link->stream, (u8*) buffer + done, len - done);
        bool closed = !write && n == 0 && rs_peer_closed(link->stream);
        LightLock_Unlock(&link->lock);

        if(n > 0) {
            done += n;
            last = osGetTime();
        } else if(closed) {
            return R_APP_IO_ERROR;
        } else {
            if(osGetTime() - last > 20000) {
                return R_APP_IO_ERROR;
            }

            svcSleepThread(2000000);
        }
    }

    return 0;
}

int udslink_recv_some(udslink* link, void* buffer, size_t len) {
    if(link->dead) {
        return -2;
    }

    LightLock_Lock(&link->lock);
    size_t n = rs_read(link->stream, buffer, len);
    bool closed = n == 0 && rs_peer_closed(link->stream);
    LightLock_Unlock(&link->lock);

    return n > 0 ? (int) n : closed ? 0 : -1;
}

void udslink_close(udslink* link) {
    if(link == NULL) {
        return;
    }

    // What was written goes out before the end does.
    LightLock_Lock(&link->lock);
    rs_close(link->stream);
    LightLock_Unlock(&link->lock);

    u64 begin = osGetTime();
    while(!link->dead && osGetTime() - begin < 3000) {
        LightLock_Lock(&link->lock);
        bool flushed = rs_flushed(link->stream);
        LightLock_Unlock(&link->lock);

        if(flushed) {
            break;
        }

        svcSleepThread(10000000);
    }

    link->stop = true;
    threadJoin(link->pump, U64_MAX);
    threadFree(link->pump);

    rs_stats stats;
    rs_get_stats(link->stream, &stats);
    udslink_log("link closed: frames=%llu resent=%llu acks=%llu rto=%u dead=%d\n", stats.framesSent, stats.framesResent, stats.acksSent, stats.rto, link->dead ? 1 : 0);

    if(link->ownsBind) {
        udsUnbind(link->use);
        udsDisconnectNetwork();
        free(link->use);
    }

    rs_free(link->stream);
    free(link);
}
