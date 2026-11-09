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

#include "ftpserver.h"
#include "relaunch.h"
#include "selftest.h"
#include "../core/core.h"
#include "../core/minire.h"
#include "../core/speedlog.h"
#include "../core/unarchive.h"
#include "../core/xferturn.h"
#include "action/action.h"
#include "transferconn.h"
#include "wifislots.h"

#define FTP_PATH_MAX 512
#define FTP_LINE_MAX 600
#define FTP_IO_SIZE (64 * 1024)
#define FTP_FILE_BUFFER (256 * 1024)
// Writes to the SD card that start in the middle of a block are some 28 times slower (an upload picked up
// at byte 3158073 ran at 34 KB/s, at 3145728 at 965): a STOR after REST or an APPE that starts there
// first writes up to the next multiple of this, so that the buffer's writes after it all start on one.
#define FTP_WRITE_ALIGN (64 * 1024)
#define FTP_NAP_MS 10
#define FTP_SEND_CHUNK (64 * 1024)
// The most one send() offers, in bytes (SITE SENDCHUNK <KB>, DEBUG, to compare).
static u32 ftp_send_chunk = FTP_SEND_CHUNK;
// Clients at once, each on a thread of its own (FileZilla keeps one connection to browse and opens another for each
// transfer). Data still moves for one transfer at a time (xferturn.h); listings and SITE commands answer meanwhile.
#define FTP_MAX_SESSIONS 3
#define FTP_SESSION_STACK 0x10000
#define FTP_DATA_ACCEPT_MS 15000
#define FTP_IDLE_MS (10 * 60 * 1000)
#define FTP_PASV_FIRST (FTP_SERVER_PORT + 1)
#define FTP_PASV_COUNT 64

#define NSA_TITLE_ID (0x0004000000000000ULL | ((u64) APP_UNIQUE_ID << 8))

typedef enum {
    PHASE_NONE,
    PHASE_RECEIVING,
    PHASE_INSTALLING
} ftp_phase;

static Thread ftp_thread = NULL;
static volatile bool ftp_run = false;
static bool ftp_paused = false;

// ftpserver_resume_later(): starts are tried until the Wi-Fi is back.
static bool ftp_retry = false;
static u64 ftp_retry_next = 0;
static u64 ftp_retry_end = 0;
static int ftp_listen = -1;
// The data connection of the transfer that holds the turn: it waits inside poll() (ftp_wait_fd).
static volatile int ftp_owner_data = -1;
static volatile u32 ftp_data_open = 0;    // Data connections open, all sessions.
static volatile u64 ftp_data_since = 0;   // When the last one opened.
static volatile u64 ftp_data_ended = 0;   // When the last one closed,
static volatile bool ftp_data_long = false;   // and whether it lasted a second or more.
static u32 ftp_ip = 0;
static char ftp_status[48] = "Off";
static unsigned ftp_next_pasv = 0;

// The sessions' list, the data counters and the PASV ports; installs (SITE INSTALL, the web page) one at a time; the
// main thread's errands (SITE SCREENSHOT, SITE TEST) one at a time.
static LightLock ftp_lock;
static LightLock ftp_install_lock;
static LightLock ftp_errand_lock;
static bool ftp_locks_ready = false;

// Before the first start, and before an install from the web page (which may come with the server off).
static void ftp_init_locks() {
    if(!ftp_locks_ready) {
        LightLock_Init(&ftp_lock);
        LightLock_Init(&ftp_install_lock);
        LightLock_Init(&ftp_errand_lock);
        ftp_locks_ready = true;
    }
}

static volatile ftp_phase ftp_current_phase = PHASE_NONE;
static char ftp_phase_file[64];
static volatile u64 ftp_done = 0;
static volatile u64 ftp_total = 0;

// A screenshot asked for by SITE SCREENSHOT, taken by the main thread.
static volatile bool shot_requested = false;
static volatile bool shot_done = false;
static volatile bool shot_ok = false;
static volatile u32 shot_ms = 0;

// A SITE TEST command, run by the main thread (it opens screens, reads the top one).
static volatile bool test_requested = false;
static volatile bool test_done = false;
static char test_command[FTP_LINE_MAX];
static char test_reply[FTP_LINE_MAX];

typedef struct {
    int ctrl;
    int pasv;  // Listening data socket after PASV/EPSV, -1 when none.
    volatile int data;   // The data connection under way, -1 when none.
    u32 turn;  // The transfer turn (xferturn.h) its RETR or STOR holds, 0 when none.
    u64 allo;  // ALLO: the size of the next STOR.
    u64 rest;  // REST: where the next RETR or STOR starts (a download or upload picked up where it stopped).
    char* io;  // Its transfers' buffer (FTP_IO_SIZE).
    char cwd[FTP_PATH_MAX];
    char rnfr[FTP_PATH_MAX];
    char in[FTP_LINE_MAX * 2];
    unsigned inUsed;
} ftp_session;

static ftp_session* ftp_sessions[FTP_MAX_SESSIONS];

// ---- sockets ----

static void ftp_close_socket(int* fd) {
    if(*fd >= 0) {
        shutdown(*fd, SHUT_RDWR);
        close(*fd);
        *fd = -1;
    }
}

// Waits until fd is readable (or writable); false on timeout, error or stop.
// libctru sends every socket call of the process through one SOC session, so
// a blocking poll() would hold up the downloads' and Remote Install's network
// calls for its whole timeout: ask without waiting and nap between asks
// (net_nap_ms: short while data flows, longer once idle).
//
// The data connection of a transfer that holds the turn waits inside poll() instead, a few ms at a
// time (net_poll_owner), so it wakes as soon as the data or the room is there: nothing else moves
// data meanwhile. The sockets still never block, so a client that went away costs those ms, not
// minutes (see netpoll.h).
static bool ftp_wait_fd(int fd, short events, int timeoutMs) {
    bool owner = fd >= 0 && fd == ftp_owner_data;
    int waited = 0;
    while(ftp_run) {
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

        if(polls) {
            u64 took = osGetTime() - start;
            waited += took > 0 ? (int) took : 1;
        } else {
            u32 nap = net_nap_ms((u32) waited);
            svcSleepThread((s64) nap * 1000000);
            waited += nap;
        }

        if(timeoutMs > 0 && waited >= timeoutMs) {
            return false;
        }
    }

    return false;
}

// A data connection's wait, up to `timeoutMs`. Other clients connect on their own threads meanwhile.
static bool ftp_wait_data(int fd, short events, int timeoutMs) {
    return ftp_wait_fd(fd, events, timeoutMs);
}

// `log` (may be NULL) gets the time spent waiting for room and inside send().
//
// It sends first and waits only when nothing fits (as ftpd does: writes until EWOULDBLOCK, then polls), not a poll
// before every send: each is a request to the SOC service, which on an Old 3DS takes milliseconds.
static bool ftp_send_logged(int fd, const char* data, size_t n, speedlog* log) {
    while(n > 0) {
        // The sockets do not block (net_set_nonblocking): what does not fit now waits for room.
        u64 start = speedlog_ticks();
        ssize_t w = send(fd, data, n > ftp_send_chunk ? ftp_send_chunk : n, 0);
        speedlog_net(log, w > 0 ? (u64) w : 0, speedlog_ticks() - start);
        if(w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            start = speedlog_ticks();
            bool ready = ftp_wait_data(fd, POLLOUT, 30000);
            speedlog_wait(log, speedlog_ticks() - start);
            if(!ready) {
                return false;
            }

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

static bool ftp_send_all(int fd, const char* data, size_t n) {
    return ftp_send_logged(fd, data, n, NULL);
}

static bool ftp_reply(ftp_session* s, const char* fmt, ...) {
    char line[FTP_LINE_MAX];

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);

    if(n < 0) {
        return false;
    }

    if(n > (int) sizeof(line) - 3) {
        n = (int) sizeof(line) - 3;
    }

    line[n++] = '\r';
    line[n++] = '\n';
    return ftp_send_all(s->ctrl, line, (size_t) n);
}

// Waits for the client's next command. Each session has its thread, so a silent one keeps no one else out; one
// silent for FTP_IDLE_MS (a client that vanished with the Wi-Fi) is closed, which frees its place.
static bool ftp_wait_command(ftp_session* s) {
    u32 waited = 0;
    while(ftp_run) {
        struct pollfd p = {.fd = s->ctrl, .events = POLLIN, .revents = 0};
        int r = poll(&p, 1, 0);
        if(r < 0 || (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            return r >= 0;
        }

        u32 nap = net_nap_ms(waited);
        svcSleepThread((s64) nap * 1000000);
        waited += nap;
        if(waited >= FTP_IDLE_MS) {
            return false;
        }
    }

    return false;
}

// One command line, CR/LF stripped. False when the client left or we stop.
static bool ftp_read_line(ftp_session* s, char* out, unsigned cap) {
    for(;;) {
        for(unsigned i = 0; i < s->inUsed; i++) {
            if(s->in[i] == '\n') {
                unsigned n = i;
                if(n > 0 && s->in[n - 1] == '\r') {
                    n--;
                }

                if(n >= cap) {
                    n = cap - 1;
                }

                memcpy(out, s->in, n);
                out[n] = '\0';
                memmove(s->in, s->in + i + 1, s->inUsed - i - 1);
                s->inUsed -= i + 1;
                return true;
            }
        }

        // An overlong line is dropped.
        if(s->inUsed >= sizeof(s->in) - 1) {
            s->inUsed = 0;
        }

        if(!ftp_wait_command(s)) {
            return false;
        }

        ssize_t r = net_recv(s->ctrl, s->in + s->inUsed, sizeof(s->in) - 1 - s->inUsed, FTP_IDLE_MS);
        if(r <= 0) {
            return false;
        }

        s->inUsed += (unsigned) r;
    }
}

// ---- paths: "/" is the root of the SD card, and ".." never leaves it ----

static void ftp_resolve_path(const ftp_session* s, const char* arg, char* out, unsigned cap) {
    char joined[FTP_PATH_MAX * 2];
    if(arg == NULL || arg[0] == '\0') {
        snprintf(joined, sizeof(joined), "%s", s->cwd);
    } else if(arg[0] == '/') {
        snprintf(joined, sizeof(joined), "%s", arg);
    } else {
        snprintf(joined, sizeof(joined), "%s/%s", s->cwd, arg);
    }

    static char parts[64][128];
    int count = 0;
    char* save = NULL;
    for(char* p = strtok_r(joined, "/", &save); p != NULL; p = strtok_r(NULL, "/", &save)) {
        if(strcmp(p, ".") == 0) {
            continue;
        }

        if(strcmp(p, "..") == 0) {
            if(count > 0) {
                count--;
            }

            continue;
        }

        if(count < 64) {
            snprintf(parts[count++], sizeof(parts[0]), "%s", p);
        }
    }

    unsigned n = 0;
    out[0] = '\0';
    for(int i = 0; i < count && n + 1 < cap; i++) {
        n += (unsigned) snprintf(out + n, cap - n, "/%s", parts[i]);
    }

    if(n == 0) {
        snprintf(out, cap, "/");
    }
}

static void ftp_real_path(const char* virtualPath, char* out, unsigned cap) {
    snprintf(out, cap, "sdmc:%s", virtualPath);
}

// ---- data connection ----

static void ftp_close_pasv(ftp_session* s) {
    ftp_close_socket(&s->pasv);
}

// The passive socket's buffers in KB, which its data connection inherits, as ftpd sets them on the 3DS; 0 leaves the
// system's (SITE PASVBUF, DEBUG). New 3DS XL, 64 MB (2026-11-04): the system's 802-836 up / 805-815 down KB/s, 16 KB
// 817 / 986, 24 KB 776 / 1137, 32 KB 974-1116 / 1148-1208, 40 KB 953 / 1168 (48 KB reset every data connection,
// 2026-11-01).
static u32 ftp_pasv_buffer_kb = 32;

static int ftp_open_pasv(ftp_session* s) {
    ftp_close_pasv(s);

    for(unsigned attempt = 0; attempt < FTP_PASV_COUNT; attempt++) {
        LightLock_Lock(&ftp_lock);
        unsigned port = FTP_PASV_FIRST + (ftp_next_pasv++ % FTP_PASV_COUNT);
        LightLock_Unlock(&ftp_lock);

        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if(fd < 0) {
            return -1;
        }

        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if(ftp_pasv_buffer_kb > 0) {
            int size = (int) (ftp_pasv_buffer_kb * 1024);
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
        }

        net_set_nonblocking(fd);

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u16) port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if(bind(fd, (struct sockaddr*) &addr, sizeof(addr)) == 0 && listen(fd, 1) == 0) {
            s->pasv = fd;
            return (int) port;
        }

        close(fd);
    }

    return -1;
}

// Accepts the client's data connection; -1 when none came.
static int ftp_accept_data(ftp_session* s) {
    if(s->pasv < 0) {
        return -1;
    }

    int fd = -1;
    if(ftp_wait_fd(s->pasv, POLLIN, FTP_DATA_ACCEPT_MS)) {
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        fd = accept(s->pasv, (struct sockaddr*) &peer, &len);
        if(fd >= 0) {
            net_set_nonblocking(fd);
        }
    }

    ftp_close_pasv(s);
    if(fd >= 0) {
        LightLock_Lock(&ftp_lock);
        ftp_data_open++;
        ftp_data_since = osGetTime();
        s->data = fd;
        if(s->turn != 0) {
            ftp_owner_data = fd;
        }

        LightLock_Unlock(&ftp_lock);
    }

    return fd;
}

static void ftp_close_data(ftp_session* s, int* fd) {
    if(*fd < 0) {
        return;
    }

    LightLock_Lock(&ftp_lock);
    u64 now = osGetTime();
    ftp_data_long = now - ftp_data_since >= 1000;
    ftp_data_ended = now;
    if(ftp_data_open > 0) {
        ftp_data_open--;
    }

    if(ftp_owner_data == *fd) {
        ftp_owner_data = -1;
    }

    s->data = -1;
    LightLock_Unlock(&ftp_lock);
    net_close_gently(*fd);
    *fd = -1;
}

static void ftp_set_phase(ftp_phase phase, const char* vpath) {
    if(vpath != NULL) {
        const char* name = strrchr(vpath, '/');
        string_copy(ftp_phase_file, name != NULL ? name + 1 : vpath, sizeof(ftp_phase_file));
    }

    ftp_current_phase = phase;
}

// ---- SITE INSTALL ----

static bool ftp_install_cia_locked(const char* vpath, char* io, char* error, size_t errorSize);

// One install at a time (SITE INSTALL from any session, the web page's), with a buffer of its own.
static bool ftp_install_cia(const char* vpath, char* error, size_t errorSize) {
    char* io = (char*) malloc(FTP_IO_SIZE);
    if(io == NULL) {
        snprintf(error, errorSize, "out of memory");
        return false;
    }

    ftp_init_locks();
    LightLock_Lock(&ftp_install_lock);
    bool ok = ftp_install_cia_locked(vpath, io, error, errorSize);
    LightLock_Unlock(&ftp_install_lock);
    free(io);
    return ok;
}

static bool ftp_install_cia_locked(const char* vpath, char* io, char* error, size_t errorSize) {
    Handle input = 0;
    Handle output = 0;
    bool started = false;
    bool ok = false;
    u64 size = 0;
    AM_TitleInfo info;
    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(vpath);
    if(fsPath == NULL) {
        snprintf(error, errorSize, "out of memory");
        return false;
    }

    res = FSUSER_OpenFileDirectly(&input, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, FS_OPEN_READ, 0);
    fs_free_path_utf8(fsPath);
    if(R_FAILED(res) || R_FAILED(res = FSFILE_GetSize(input, &size))) {
        snprintf(error, errorSize, "cannot open %s (%08lX)", vpath, res);
        goto done;
    }

    if(R_FAILED(res = AM_GetCiaFileInfo(MEDIATYPE_SD, &info, input))) {
        snprintf(error, errorSize, "not a CIA (%08lX)", res);
        goto done;
    }

    // Only titles that install to the SD card: system titles and DSiWare go
    // to the NAND, which is never reachable from the network.
    if(fs_get_title_destination(info.titleID) != MEDIATYPE_SD) {
        snprintf(error, errorSize, "title %016llX installs to the NAND; use NSA's own screens", info.titleID);
        goto done;
    }

    if(R_FAILED(res = AM_StartCiaInstallOverwrite(&output, MEDIATYPE_SD))) {
        snprintf(error, errorSize, "install start failed (%08lX)", res);
        goto done;
    }

    started = true;
    ftp_done = 0;
    ftp_total = size;

    for(u64 offset = 0; offset < size;) {
        u32 want = size - offset > FTP_IO_SIZE ? FTP_IO_SIZE : (u32) (size - offset);
        u32 bytesRead = 0;
        u32 bytesWritten = 0;
        if(R_FAILED(res = FSFILE_Read(input, &bytesRead, offset, io, want)) || bytesRead != want) {
            snprintf(error, errorSize, "SD read failed (%08lX)", res);
            goto done;
        }

        // AM refuses a lower version than the installed one at the first
        // write. Unlike the ports, NSA does not delete the installed title to
        // force it: that would also delete the game's save data.
        if(R_FAILED(res = FSFILE_Write(output, &bytesWritten, offset, io, bytesRead, 0)) || bytesWritten != bytesRead) {
            snprintf(error, errorSize, "install write failed (%08lX)%s", res, offset == 0 ? "; an older version than the installed one?" : "");
            goto done;
        }

        offset += bytesRead;
        ftp_done = offset;
    }

    started = false;
    if(R_FAILED(res = AM_FinishCiaInstall(output))) {
        snprintf(error, errorSize, "install finish failed (%08lX)", res);
        goto done;
    }

    ok = true;

    // NSA itself (CIA build only: a 3DSX is not replaced by a CIA install).
    if(info.titleID == NSA_TITLE_ID) {
        relaunch_request();
    }

done:
    if(started) {
        AM_CancelCIAInstall(output);
    }

    if(input != 0) {
        FSFILE_Close(input);
    }

    return ok;
}

// For the web server: the same install, with its progress on the bottom screen's toast.
bool ftpserver_install(const char* vpath, char* error, size_t errorSize) {
    ftp_set_phase(PHASE_INSTALLING, vpath);
    bool ok = ftp_install_cia(vpath, error, errorSize);
    ftp_set_phase(PHASE_NONE, NULL);
    return ok;
}

// ---- commands ----

static void ftp_format_list_line(char* out, unsigned cap, const char* name, const struct stat* st, bool mlsd) {
    bool dir = S_ISDIR(st->st_mode);
    if(mlsd) {
        snprintf(out, cap, "type=%s;size=%llu; %s\r\n", dir ? "dir" : "file", (unsigned long long) st->st_size, name);
        return;
    }

    // libctru leaves st_mtime at 0: a fixed date reads better than 1970.
    snprintf(out, cap, "%s 1 3ds 3ds %12llu Jan  1 00:00 %s\r\n", dir ? "drwxrwxrwx" : "-rw-rw-rw-", (unsigned long long) st->st_size, name);
}

typedef enum {
    LIST_LONG,
    LIST_NAMES,
    LIST_MACHINE
} list_kind;

static void ftp_cmd_list(ftp_session* s, const char* arg, list_kind kind) {
    // Clients send options such as "-a" or "-la": not a path.
    if(arg != NULL && arg[0] == '-') {
        const char* space = strchr(arg, ' ');
        arg = space != NULL ? space + 1 : "";
    }

    char vpath[FTP_PATH_MAX];
    char rpath[FTP_PATH_MAX + 8];
    ftp_resolve_path(s, arg, vpath, sizeof(vpath));
    ftp_real_path(vpath, rpath, sizeof(rpath));

    DIR* dir = opendir(rpath);
    if(dir == NULL) {
        ftp_close_pasv(s);
        ftp_reply(s, "550 No such directory.");
        return;
    }

    ftp_reply(s, "150 Listing.");
    int data = ftp_accept_data(s);
    if(data < 0) {
        closedir(dir);
        ftp_reply(s, "425 No data connection.");
        return;
    }

    bool ok = true;
    struct dirent* e;
    while(ok && (e = readdir(dir)) != NULL) {
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }

        char line[FTP_PATH_MAX + 96];
        if(kind == LIST_NAMES) {
            snprintf(line, sizeof(line), "%s\r\n", e->d_name);
        } else {
            char full[FTP_PATH_MAX * 2];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", rpath, e->d_name);
            if(stat(full, &st) != 0) {
                memset(&st, 0, sizeof(st));
            }

            if(e->d_type == DT_DIR) {
                st.st_mode = S_IFDIR;
            }

            ftp_format_list_line(line, sizeof(line), e->d_name, &st, kind == LIST_MACHINE);
        }

        ok = ftp_send_all(data, line, strlen(line));
    }

    closedir(dir);
    ftp_close_data(s, &data);
    ftp_reply(s, ok ? "226 Done." : "426 Transfer aborted.");
}

// RETR and STOR take the transfer turn; a small download (NSA's own logs, a PIN, a screenshot) does
// not, so that those stay readable while something else transfers.
#define FTP_TURN_MIN_RETR (1024 * 1024)

// Takes the turn for `vpath`, or says who has it (450) and gives the PASV socket up.
static bool ftp_take_turn(ftp_session* s, const char* vpath, u64 total, bool incoming) {
    s->turn = xferturn_take(XFER_FTP, vpath, total);
    if(s->turn != 0) {
        xferturn_incoming(s->turn, incoming);
        return true;
    }

    char busy[128];
    if(!xferturn_describe(busy, sizeof(busy))) {
        string_copy(busy, "Busy", sizeof(busy));
    }

    ftp_close_pasv(s);
    ftp_reply(s, "450 %s. One transfer at a time: try again when it ends.", busy);
    return false;
}

static void ftp_release_turn(ftp_session* s) {
    xferturn_release(s->turn);
    s->turn = 0;
}

// Test files for tools/speed_tests.py, to tell the network from the card: RETR /dev/zero/<MB> sends
// that many MB from memory, STOR /dev/null (or anything under it) throws what comes away.
#define FTP_DEV_ZERO "/dev/zero/"
#define FTP_DEV_NULL "/dev/null"

static void ftp_cmd_retr(ftp_session* s, const char* arg) {
    char vpath[FTP_PATH_MAX];
    char rpath[FTP_PATH_MAX + 8];
    ftp_resolve_path(s, arg, vpath, sizeof(vpath));
    ftp_real_path(vpath, rpath, sizeof(rpath));

    bool zero = strncmp(vpath, FTP_DEV_ZERO, strlen(FTP_DEV_ZERO)) == 0;
    u64 size = 0;
    FILE* f = NULL;
    if(zero) {
        size = (u64) strtoul(vpath + strlen(FTP_DEV_ZERO), NULL, 10) * 1024 * 1024;
    } else {
        struct stat st;
        if(stat(rpath, &st) == 0 && !S_ISDIR(st.st_mode)) {
            size = (u64) st.st_size;
            f = fopen(rpath, "rb");
        }
    }

    u64 rest = s->rest;
    s->rest = 0;

    if(zero ? size == 0 : f == NULL) {
        ftp_close_pasv(s);
        ftp_reply(s, "550 Cannot open the file.");
        return;
    }

    // REST: from there on. The turn, the toast and the trace count what is left to send.
    if(rest > size || (f != NULL && rest > 0 && fseeko(f, (off_t) rest, SEEK_SET) != 0)) {
        if(f != NULL) {
            fclose(f);
        }

        ftp_close_pasv(s);
        ftp_reply(s, "554 Cannot restart there.");
        return;
    }

    size -= rest;

    // NSA's own speed traces are read while something transfers (tools/speed_tests.py): never the turn for them.
    bool traces = strcmp(vpath, APP_DATA_DIR "speed-log.txt") == 0;
    if(!traces && (zero || size >= FTP_TURN_MIN_RETR) && !ftp_take_turn(s, vpath, size, false)) {
        if(f != NULL) {
            fclose(f);
        }

        return;
    }

    ftp_reply(s, "150 Sending.");
    int data = ftp_accept_data(s);
    if(data < 0) {
        if(f != NULL) {
            fclose(f);
        }

        ftp_release_turn(s);
        ftp_reply(s, "425 No data connection.");
        return;
    }

    // Not the trace of fetching the traces (tools/speed_tests.py reads them while a download runs).
    speedlog* log = traces ? NULL : speedlog_begin(zero ? "ftp-zero" : "ftp-down", vpath, size);
    if(zero) {
        memset(s->io, 0, FTP_IO_SIZE);
    }

    bool ok = true;
    u64 left = size;
    u64 sent = 0;
    while(ok) {
        size_t n;
        if(zero) {
            n = left > FTP_IO_SIZE ? FTP_IO_SIZE : (size_t) left;
            left -= n;
        } else {
            u64 start = speedlog_ticks();
            n = fread(s->io, 1, FTP_IO_SIZE, f);
            speedlog_disk(log, n, speedlog_ticks() - start);
        }

        if(n == 0) {
            break;
        }

        ok = ftp_send_logged(data, s->io, n, log);
        sent += n;
        xferturn_progress(s->turn, sent);
    }

    if(f != NULL) {
        fclose(f);
    }

    ftp_close_data(s, &data);
    ftp_release_turn(s);
    speedlog_end(log, ok);
    ftp_reply(s, ok ? "226 Done." : "426 Transfer aborted.");
}

static void ftp_cmd_stor(ftp_session* s, const char* arg, bool append) {
    char vpath[FTP_PATH_MAX];
    char rpath[FTP_PATH_MAX + 8];
    ftp_resolve_path(s, arg, vpath, sizeof(vpath));
    ftp_real_path(vpath, rpath, sizeof(rpath));

    bool null = strncmp(vpath, FTP_DEV_NULL, strlen(FTP_DEV_NULL)) == 0 && (vpath[strlen(FTP_DEV_NULL)] == '\0' || vpath[strlen(FTP_DEV_NULL)] == '/');

    // REST before a STOR: the file is kept and written from there on (APPE goes to its end anyway).
    u64 rest = append || null ? 0 : s->rest;
    s->rest = 0;

    // Before opening: "wb" would empty a file that is not going to be written.
    if(!ftp_take_turn(s, vpath, s->allo > rest ? s->allo - rest : 0, true)) {
        s->allo = 0;
        return;
    }

    FILE* f = null ? NULL : fopen(rpath, append ? "ab" : rest > 0 ? "r+b" : "wb");
    if(!null && f == NULL) {
        ftp_release_turn(s);
        ftp_close_pasv(s);
        ftp_reply(s, rest > 0 ? "554 Cannot restart there." : "553 Cannot create the file.");
        return;
    }

    struct stat st;
    if(rest > 0 && (fstat(fileno(f), &st) != 0 || (u64) st.st_size < rest || fseeko(f, (off_t) rest, SEEK_SET) != 0)) {
        fclose(f);
        ftp_release_turn(s);
        ftp_close_pasv(s);
        ftp_reply(s, "554 Cannot restart there.");
        return;
    }

    // Where the writing starts, and how much to write before the first aligned offset (FTP_WRITE_ALIGN).
    u64 at = rest;
    if(append && f != NULL && fstat(fileno(f), &st) == 0) {
        at = (u64) st.st_size;
    }

    u64 head = f != NULL && at % FTP_WRITE_ALIGN != 0 ? FTP_WRITE_ALIGN - at % FTP_WRITE_ALIGN : 0;

    // The network hands over a few KB at a time, and every write to the SD card costs a lot on its own:
    // gather them into big writes, as ftpd does.
    if(f != NULL) {
        setvbuf(f, NULL, _IOFBF, FTP_FILE_BUFFER);
    }

    // A CIA coming in is shown on the bottom screen; ALLO gave its size.
    size_t len = strlen(vpath);
    bool cia = len > 4 && strcasecmp(vpath + len - 4, ".cia") == 0;
    if(cia) {
        ftp_done = rest;
        ftp_total = s->allo;
        ftp_set_phase(PHASE_RECEIVING, vpath);
    }

    // ALLO gave the file's size: set it first, the card writes faster so (issue #26: +26% on the New 3DS, +40%
    // on the 2DS, and the 2DS's writes of over a second halve). Not for APPE, which writes at the end. Cut back
    // to what came if less does, so that a REST resumes from there.
    u64 presized = 0;
    if(f != NULL && !append && s->allo > at && ftruncate(fileno(f), (off_t) s->allo) == 0) {
        presized = s->allo;
    }

    u64 allo = s->allo > rest ? s->allo - rest : 0;
    s->allo = 0;

    ftp_reply(s, "150 Receiving.");
    int data = ftp_accept_data(s);
    if(data < 0) {
        if(f != NULL) {
            fclose(f);
        }
        if(cia) {
            ftp_set_phase(PHASE_NONE, NULL);
        }

        ftp_release_turn(s);
        ftp_reply(s, "425 No data connection.");
        return;
    }

    speedlog* log = speedlog_begin(null ? "ftp-null" : "ftp-up", vpath, allo);
    u64 received = 0;
    bool ok = true;
    for(;;) {
        u64 start = speedlog_ticks();
        bool ready = ftp_wait_data(data, POLLIN, 30000);
        speedlog_wait(log, speedlog_ticks() - start);
        if(!ready) {
            ok = false;
            break;
        }

        start = speedlog_ticks();
        // The socket said it is readable: nothing within a second means a client that reset (see the web server's).
        ssize_t r = net_recv(data, s->io, FTP_IO_SIZE, 1000);
        speedlog_net(log, r > 0 ? (u64) r : 0, speedlog_ticks() - start);
        if(r == 0) {
            break;
        }

        if(r < 0) {
            ok = false;
            break;
        }

        if(f != NULL) {
            start = speedlog_ticks();
            const char* from = s->io;
            size_t n = (size_t) r;
            bool written = true;
            if(head > 0) {
                size_t first = n < head ? n : (size_t) head;
                written = fwrite(from, 1, first, f) == first;
                head -= first;
                from += first;
                n -= first;
                if(written && head == 0) {
                    written = fflush(f) == 0;
                }
            }

            if(written && n > 0) {
                written = fwrite(from, 1, n, f) == n;
            }

            speedlog_disk(log, (u64) r, speedlog_ticks() - start);
            if(!written) {
                ok = false;
                break;
            }
        }

        ftp_done += (u64) r;
        received += (u64) r;
        xferturn_progress(s->turn, received);
    }

    u64 start = speedlog_ticks();
    if(f != NULL && presized > 0 && at + received < presized && (fflush(f) != 0 || ftruncate(fileno(f), (off_t) (at + received)) != 0)) {
        ok = false;
    }

    if(f != NULL && fclose(f) != 0) {
        ok = false;
    }

    speedlog_disk(log, 0, speedlog_ticks() - start);
    ftp_close_data(s, &data);
    ftp_release_turn(s);
    speedlog_end(log, ok);
    if(cia) {
        ftp_set_phase(PHASE_NONE, NULL);
    }

    ftp_reply(s, ok ? "226 Done." : "451 Transfer failed.");
}

// Driving the console from a computer (screenshots, key presses and touches, installing a CIA) is
// always on in DEBUG builds. In the others the FTP server only serves files, and these SITE
// commands work only while this file exists on the SD card: the owner turns remote use on by
// creating it, and off by deleting it.
#define FTP_REMOTE_FLAG "/nsa/remote.flag"

static bool ftp_remote_allowed() {
    if(NSA_DEBUG) {
        return true;
    }

    Handle file = 0;
    if(R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, FTP_REMOTE_FLAG), FS_OPEN_READ, 0))) {
        FSFILE_Close(file);
        return true;
    }

    return false;
}

// SITE SDBENCH: how fast the card writes and reads a file of `mb` MB, through a stdio buffer of
// `bufferKb` KB (0: none) in 64 KB pieces, as STOR and RETR do; `preallocate` sets the size first. The
// speed traces have each second ("sd-write", "sd-read"). Without the network, to tell the two apart.
#define FTP_SDBENCH_FILE "sdmc:" APP_DATA_DIR "sdbench.tmp"

static void ftp_sdbench(char* ftp_io, u32 mb, u32 bufferKb, bool preallocate, char* out, size_t size) {
    u64 total = (u64) mb * 1024 * 1024;
    for(u32 i = 0; i < FTP_IO_SIZE; i++) {
        ftp_io[i] = (char) (i * 7 + 3);
    }

    char* buffer = bufferKb > 0 ? (char*) malloc(bufferKb * 1024) : NULL;
    FILE* f = fopen(FTP_SDBENCH_FILE, "wb");
    if(f == NULL || (bufferKb > 0 && buffer == NULL)) {
        if(f != NULL) {
            fclose(f);
        }

        free(buffer);
        snprintf(out, size, "550 Could not open " FTP_SDBENCH_FILE " (or no memory).");
        return;
    }

    setvbuf(f, buffer, buffer != NULL ? _IOFBF : _IONBF, bufferKb * 1024);
    if(preallocate) {
        fflush(f);
        ftruncate(fileno(f), (off_t) total);
    }

    speedlog* log = speedlog_begin("sd-write", preallocate ? "preallocated" : "growing", total);
    u64 start = speedlog_ticks();
    u64 maxWrite = 0;
    bool ok = true;
    for(u64 done = 0; ok && done < total; done += FTP_IO_SIZE) {
        u64 t = speedlog_ticks();
        ok = fwrite(ftp_io, 1, FTP_IO_SIZE, f) == FTP_IO_SIZE;
        t = speedlog_ticks() - t;
        maxWrite = t > maxWrite ? t : maxWrite;
        // The bytes count as the "network" column so that the lines' KB/s is the card's.
        speedlog_net(log, FTP_IO_SIZE, 0);
        speedlog_disk(log, FTP_IO_SIZE, t);
    }

    u64 t = speedlog_ticks();
    ok = fclose(f) == 0 && ok;
    speedlog_disk(log, 0, speedlog_ticks() - t);
    u64 writeTicks = speedlog_ticks() - start;
    speedlog_end(log, ok);

    u64 readTicks = 0;
    if(ok && (f = fopen(FTP_SDBENCH_FILE, "rb")) != NULL) {
        setvbuf(f, buffer, buffer != NULL ? _IOFBF : _IONBF, bufferKb * 1024);
        log = speedlog_begin("sd-read", preallocate ? "preallocated" : "growing", total);
        start = speedlog_ticks();
        size_t n;
        do {
            t = speedlog_ticks();
            n = fread(ftp_io, 1, FTP_IO_SIZE, f);
            speedlog_net(log, n, 0);
            speedlog_disk(log, n, speedlog_ticks() - t);
        } while(n > 0);

        readTicks = speedlog_ticks() - start;
        fclose(f);
        speedlog_end(log, true);
    }

    remove(FTP_SDBENCH_FILE);
    free(buffer);

    u32 writeKbs = writeTicks > 0 ? (u32) (total * SYSCLOCK_ARM11 / writeTicks / 1024) : 0;
    u32 readKbs = readTicks > 0 ? (u32) (total * SYSCLOCK_ARM11 / readTicks / 1024) : 0;
    snprintf(out, size, "%s %lu MB, buffer %lu KB%s: write %lu KB/s (slowest write %lu ms), read %lu KB/s.", ok ? "200" : "451 Failed:",
             mb, bufferKb, preallocate ? ", preallocated" : "", writeKbs, (u32) (maxWrite * 1000 / SYSCLOCK_ARM11), readKbs);
}

static void ftp_cmd_site(ftp_session* s, char* arg) {
    char* sub = arg;
    char* path = strchr(sub, ' ');
    if(path != NULL) {
        *path++ = '\0';
    }

    for(char* c = sub; *c; c++) {
        if(*c >= 'a' && *c <= 'z') {
            *c = (char) (*c - 32);
        }
    }

    if(!ftp_remote_allowed()) {
        ftp_reply(s, "502 Remote use is off: create sdmc:" FTP_REMOTE_FLAG " on the SD card to turn it on.");
        return;
    }

    if(strcmp(sub, "SCREENSHOT") == 0) {
        LightLock_Lock(&ftp_errand_lock);
        shot_done = false;
        shot_requested = true;
        // Writing two BMPs to a real SD card takes a few seconds.
        for(u32 waited = 0; !shot_done && waited < 15000 && ftp_run; waited += FTP_NAP_MS) {
            svcSleepThread((s64) FTP_NAP_MS * 1000000);
        }

        shot_requested = false;
        LightLock_Unlock(&ftp_errand_lock);
        if(shot_done && shot_ok) {
            ftp_reply(s, "200 " APP_DATA_DIR "screenshots/top.bmp " APP_DATA_DIR "screenshots/bottom.bmp (%lu ms)", shot_ms);
        } else {
            ftp_reply(s, shot_done ? "451 Screenshot failed." : "451 Screenshot not taken in time.");
        }
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "TEST") == 0) {
        LightLock_Lock(&ftp_errand_lock);
        string_copy(test_command, path != NULL ? path : "", sizeof(test_command));
        test_done = false;
        test_requested = true;
        for(u32 waited = 0; !test_done && waited < 15000 && ftp_run; waited += FTP_NAP_MS) {
            svcSleepThread((s64) FTP_NAP_MS * 1000000);
        }

        test_requested = false;
        LightLock_Unlock(&ftp_errand_lock);
        if(test_done) {
            ftp_reply(s, "%s", test_reply);
        } else {
            ftp_reply(s, "451 The main thread did not answer in time.");
        }
        return;
    }

    // How much memory is left: the biggest malloc block (found by halving) and the linear heap.
    if(NSA_DEBUG && strcmp(sub, "MEM") == 0) {
        size_t lo = 0;
        size_t hi = 64 * 1024 * 1024;
        while(hi - lo > 64 * 1024) {
            size_t mid = lo + (hi - lo) / 2;
            void* p = malloc(mid);
            if(p != NULL) {
                free(p);
                lo = mid;
            } else {
                hi = mid;
            }
        }

        ftp_reply(s, "200 malloc %lu KB, linear free %lu KB", (unsigned long) (lo / 1024), (unsigned long) (linearSpaceFree() / 1024));
        return;
    }

    // Tests of the Store's scripts: "SITE UNARCHIVE /a.zip|^dir/|/out/" (paths on the SD card), "SITE MINIRE pattern|text".
    if(NSA_DEBUG && (strcmp(sub, "UNARCHIVE") == 0 || strcmp(sub, "MINIRE") == 0)) {
        char args[512];
        string_copy(args, path != NULL ? path : "", sizeof(args));
        char* second = strchr(args, '|');
        char* third = second != NULL ? strchr(second + 1, '|') : NULL;
        if(second != NULL) {
            *second++ = '\0';
        }

        if(third != NULL) {
            *third++ = '\0';
        }

        if(strcmp(sub, "MINIRE") == 0) {
            size_t start = 0;
            size_t end = 0;
            bool found = second != NULL && minire_search(args, second, &start, &end);
            ftp_reply(s, found ? "200 match %u-%u" : "200 no match", (unsigned) start, (unsigned) end);
            return;
        }

        if(second == NULL || third == NULL) {
            ftp_reply(s, "501 SITE UNARCHIVE archive|pattern|output");
            return;
        }

        char archive[300];
        char output[300];
        snprintf(archive, sizeof(archive), "sdmc:%s", args);
        snprintf(output, sizeof(output), "sdmc:%s", third);
        unarchive_progress progress;
        memset(&progress, 0, sizeof(progress));
        u32 count = 0;
        u64 start = osGetTime();
        Result res = unarchive_extract(archive, second, output, &progress, &count);
        extern u32 unarchive_last_dictionary;
        ftp_reply(s, "200 res %08lX files %lu bytes %llu of %llu in %llu ms, dictionary %lu", res, count, progress.done, progress.total, osGetTime() - start,
                  unarchive_last_dictionary);
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "FAULT") == 0) {
        char point[32] = "";
        unsigned skip = 0, count = 1;
        int got = sscanf(path != NULL ? path : "", "%31s %u %u", point, &skip, &count);
        if(got >= 1 && strcasecmp(point, "CLEAR") == 0) {
            selftest_fault_clear();
            ftp_reply(s, "200 No faults.");
        } else if(got >= 1 && strcasecmp(point, "LIST") == 0) {
            char list[256];
            selftest_fault_list(list, sizeof(list));
            ftp_reply(s, "200 %s", list[0] != '\0' ? list : "none");
        } else if(got >= 1) {
            selftest_fault_arm(point, skip, count);
            ftp_reply(s, "200 %s fails %u time(s) after %u.", point, count, skip);
        } else {
            ftp_reply(s, "501 SITE FAULT <point> [skip] [count] | CLEAR | LIST");
        }
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "CPU") == 0) {
        unsigned ms = 2000;
        if(path != NULL && strcasecmp(path, "OFF") == 0) {
            selftest_cpu_stop();
            ftp_reply(s, "200 CPU counting stopped.");
            return;
        }

        if(path != NULL) {
            sscanf(path, "%u", &ms);
        }

        char text[128];
        selftest_cpu_measure(ms < 100 ? 100 : ms > 10000 ? 10000 : ms, text, sizeof(text));
        ftp_reply(s, "200 %s", text);
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "CPULIMIT") == 0) {
        // The share of the system core (core 1) this app keeps; the network runs there on an Old 3DS. The next transfer
        // turn sets it again (xferturn_cpu_limits).
        unsigned percent = 0;
        if(path != NULL && sscanf(path, "%u", &percent) == 1) {
            Result res = APT_SetAppCpuTimeLimit(percent);
            if(R_FAILED(res)) {
                ftp_reply(s, "550 APT_SetAppCpuTimeLimit(%u) failed: %08lX", percent, res);
                return;
            }
        }

        u32 limit = 0;
        APT_GetAppCpuTimeLimit(&limit);
        ftp_reply(s, "200 CPU time limit %lu%%.", limit);
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "SHA256") == 0) {
        char vpath[FTP_PATH_MAX];
        char rpath[FTP_PATH_MAX + 8];
        ftp_resolve_path(s, path != NULL ? path : "", vpath, sizeof(vpath));
        ftp_real_path(vpath, rpath, sizeof(rpath));

        char hex[65];
        if(selftest_sha256(rpath, hex, sizeof(hex))) {
            ftp_reply(s, "200 %s", hex);
        } else {
            ftp_reply(s, "550 Could not read %s.", vpath);
        }
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "POLL") == 0) {
        static const char* names[] = {"POLL", "NAP", "HYBRID"};
        for(u32 i = 0; path != NULL && i < sizeof(names) / sizeof(*names); i++) {
            if(strcasecmp(path, names[i]) == 0) {
                net_set_wait_mode((net_wait_mode) i);
            }
        }

        ftp_reply(s, "200 The transfer with the turn waits by %s (SITE POLL POLL|NAP|HYBRID).", names[net_get_wait_mode()]);
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "SAVEORDER") == 0) {
        static bool inOrder = true;
        if(path != NULL && strcasecmp(path, "INORDER") == 0) {
            inOrder = true;
        } else if(path != NULL && strcasecmp(path, "ANY") == 0) {
            inOrder = false;
        }

        action_install_url_save_in_order(inOrder);
        ftp_reply(s, "200 Save from a URL takes the file %s (SITE SAVEORDER ANY|INORDER).", inOrder ? "in order, as an install" : "in any order");
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "FTPPRIO") == 0) {
        s32 prio = 0;
        Handle thread = ftp_thread != NULL ? threadGetHandle(ftp_thread) : 0;
        if(path != NULL && thread != 0) {
            u32 wanted = (u32) strtoul(path, NULL, 16);
            if(wanted >= 0x18 && wanted <= 0x3F) {
                svcSetThreadPriority(thread, (s32) wanted);
            }
        }

        if(thread != 0) {
            svcGetThreadPriority(&prio, thread);
        }

        ftp_reply(s, "200 The FTP server's thread runs at priority 0x%02lX (SITE FTPPRIO <hex>; the UI is 0x30).", prio);
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "SENDCHUNK") == 0) {
        u32 kb = path != NULL ? (u32) strtoul(path, NULL, 10) : 0;
        if(kb >= 1 && kb <= 64) {
            ftp_send_chunk = kb * 1024;
        }

        ftp_reply(s, "200 A send() offers at most %lu KB (SITE SENDCHUNK 1-64).", ftp_send_chunk / 1024);
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "PASVBUF") == 0) {
        if(path != NULL && path[0] >= '0' && path[0] <= '9') {
            ftp_pasv_buffer_kb = (u32) strtoul(path, NULL, 10);
        }

        ftp_reply(s, "200 The passive socket's buffers: %lu KB (0: the system's) (SITE PASVBUF <KB>).", ftp_pasv_buffer_kb);
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "CONNS") == 0) {
        if(path != NULL && path[0] >= '1' && path[0] <= '9') {
            transfer_conn_set_limit((u32) (path[0] - '0'));
        }

        ftp_reply(s, "200 A send to another 3DS opens at most %lu connections (SITE CONNS 1-%d).", transfer_conn_get_limit(), TRANSFER_CONNECTIONS);
        return;
    }

    // The Wi-Fi connections: what the config and ac say of them; a write through ac:i read back (wifislots.c).
    if(NSA_DEBUG && strcmp(sub, "WIFIREAD") == 0) {
        char reply[1024];
        wifislots_debug_read(reply, sizeof(reply));
        ftp_reply(s, "200 %s", reply);
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "WIFIKEYCHECK") == 0) {
        char reply[600];
        wifislots_debug_keycheck(reply, sizeof(reply));
        ftp_reply(s, "200 %s", reply);
        return;
    }

    if(NSA_DEBUG && strcmp(sub, "WIFIROUNDTRIP") == 0) {
        char reply[600];
        unsigned slot = 1, change = 0;
        sscanf(path != NULL ? path : "", "%u %u", &slot, &change);
        bool done = wifislots_debug_roundtrip(slot, change, reply, sizeof(reply));
        ftp_reply(s, "%s %s", done ? "200" : "550", reply);
        return;
    }

    // The notification LED: SITE LED <ok 0/1> <delay> <smoothing> <loop delay> <blink speed> <blinks> <on> <off>.
    if(NSA_DEBUG && strcmp(sub, "LED") == 0) {
        unsigned v[8] = {1, 3, 0, 0xFF, 0, 3, 1, 1};
        sscanf(path != NULL ? path : "", "%u %u %u %u %u %u %u %u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        Result res = led_pattern(v[0] != 0, (u8) v[1], (u8) v[2], (u8) v[3], (u8) v[4], v[5], v[6], v[7]);
        ftp_reply(s, "200 LED %08lX", (u32) res);
        return;
    }

    // The raw routing and DNS tables of the network service, into /nsa/netdump.txt (System -> Network's gateway / DNS).
    if(NSA_DEBUG && strcmp(sub, "NETDUMP") == 0) {
        FILE* f = fopen("sdmc:" APP_DATA_DIR "netdump.txt", "w");
        if(f == NULL) {
            ftp_reply(s, "550 Could not create " APP_DATA_DIR "netdump.txt.");
            return;
        }

        static const struct { s32 opt; const char* name; } opts[] = {
            {NETOPT_ROUTING_TABLE, "routing"}, {NETOPT_DNS_TABLE, "dns"}, {NETOPT_ARP_TABLE, "arp"},
        };
        static u8 buf[2048];
        for(u32 o = 0; o < sizeof(opts) / sizeof(*opts); o++) {
            memset(buf, 0xEE, sizeof(buf));
            socklen_t length = sizeof(buf);
            int ret = SOCU_GetNetworkOpt(SOL_CONFIG, opts[o].opt, buf, &length);
            fprintf(f, "%s: ret %d errno %d length %u\n", opts[o].name, ret, errno, (unsigned) length);
            for(u32 i = 0; i < length && i < sizeof(buf); i++) {
                fprintf(f, "%02X%s", buf[i], (i % 24) == 23 ? "\n" : " ");
            }
            fprintf(f, "\n");

            // The same with a small buffer, as systeminfo.c asks.
            length = 160;
            ret = SOCU_GetNetworkOpt(SOL_CONFIG, opts[o].opt, buf, &length);
            fprintf(f, "  with 160: ret %d errno %d length %u\n", ret, errno, (unsigned) length);
        }

        u8 cid[0x10] = {0};
        Result cidRes = FSUSER_GetSdmcCid(cid, sizeof(cid));
        fprintf(f, "sd cid: res %08lX\n", (u32) cidRes);
        for(u32 i = 0; i < sizeof(cid); i++) {
            fprintf(f, "%02X ", cid[i]);
        }
        fprintf(f, "\n");

        fclose(f);
        ftp_reply(s, "200 " APP_DATA_DIR "netdump.txt written.");
        return;
    }

    // Each game's ext data ID from its exheader against the guesses (issue #13), into /nsa/extids.csv.
    if(NSA_DEBUG && strcmp(sub, "EXTIDS") == 0) {
        if(!action_ext_data_debug("sdmc:" APP_DATA_DIR "extids.csv")) {
            ftp_reply(s, "550 Could not create " APP_DATA_DIR "extids.csv.");
            return;
        }

        ftp_reply(s, "200 " APP_DATA_DIR "extids.csv written.");
        return;
    }

    // Tests of installing over a title (issue #27): SITE TESTSAVE <title ID> FORMAT | WRITE <text> | READ, on its
    // save data on the SD card.
    if(NSA_DEBUG && strcmp(sub, "TESTSAVE") == 0) {
        char op[16] = "";
        char text[64] = "";
        unsigned long long tid = 0;
        if(sscanf(path != NULL ? path : "", "%llx %15s %63s", &tid, op, text) < 2) {
            ftp_reply(s, "501 SITE TESTSAVE <title ID> FORMAT | WRITE <text> | READ");
            return;
        }

        u32 archivePath[3] = {MEDIATYPE_SD, (u32) tid, (u32) (tid >> 32)};
        FS_Path apath = {PATH_BINARY, sizeof(archivePath), archivePath};
        Result res = 0;
        char got[64] = "";
        if(strcasecmp(op, "FORMAT") == 0) {
            res = FSUSER_FormatSaveData(ARCHIVE_USER_SAVEDATA, apath, 0x200, 10, 10, 11, 11, false);
        } else {
            FS_Archive archive = 0;
            if(R_SUCCEEDED(res = FSUSER_OpenArchive(&archive, ARCHIVE_USER_SAVEDATA, apath))) {
                Handle file = 0;
                bool write = strcasecmp(op, "WRITE") == 0;
                if(R_SUCCEEDED(res = FSUSER_OpenFile(&file, archive, fsMakePath(PATH_ASCII, "/marker.txt"), write ? FS_OPEN_WRITE | FS_OPEN_CREATE : FS_OPEN_READ, 0))) {
                    u32 n = 0;
                    if(write) {
                        if(R_SUCCEEDED(res = FSFILE_SetSize(file, 0))) {
                            res = FSFILE_Write(file, &n, 0, text, strlen(text), FS_WRITE_FLUSH);
                        }
                    } else {
                        res = FSFILE_Read(file, &n, 0, got, sizeof(got) - 1);
                        got[n < sizeof(got) ? n : sizeof(got) - 1] = '\0';
                    }

                    FSFILE_Close(file);
                }

                if(write && R_SUCCEEDED(res)) {
                    res = FSUSER_ControlArchive(archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
                }

                FSUSER_CloseArchive(archive);
            }
        }

        ftp_reply(s, "200 TESTSAVE %s %08lX %s", op, (u32) res, got);
        return;
    }

    if(strcmp(sub, "SDBENCH") == 0) {
        unsigned mb = 0, bufferKb = FTP_FILE_BUFFER / 1024;
        char flag[8] = "";
        if(sscanf(path != NULL ? path : "", "%u %u %7s", &mb, &bufferKb, flag) < 1 || mb == 0 || mb > 2047 || bufferKb > 4096) {
            ftp_reply(s, "501 SITE SDBENCH <MB> [buffer KB, 0 for none] [PRE]");
            return;
        }

        char reply[200];
        ftp_sdbench(s->io, mb, bufferKb, strcasecmp(flag, "PRE") == 0, reply, sizeof(reply));
        ftp_reply(s, "%s", reply);
        return;
    }

    if(strcmp(sub, "DRAG") == 0) {
        char* args = path != NULL ? path : "";
        unsigned x = 0, y1 = 0, y2 = 0, frames = 20;
        if(sscanf(args, "%u %u %u %u", &x, &y1, &y2, &frames) < 3 || x >= 320 || y1 >= 240 || y2 >= 240) {
            ftp_reply(s, "501 SITE DRAG <x> <y1> <y2> [frames]");
            return;
        }

        input_inject_drag((u16) x, (u16) y1, (u16) y2, frames);
        ftp_reply(s, "200 Dragged %u,%u to %u.", x, y1, y2);
        return;
    }

    if(strcmp(sub, "PRESS") == 0 || strcmp(sub, "TAP") == 0) {
        static const struct {
            const char* name;
            u32 key;
        } names[] = {
                {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R},
                {"ZL", KEY_ZL}, {"ZR", KEY_ZR}, {"START", KEY_START}, {"SELECT", KEY_SELECT},
                {"UP", KEY_DUP}, {"DOWN", KEY_DDOWN}, {"LEFT", KEY_DLEFT}, {"RIGHT", KEY_DRIGHT}
        };

        char* args = path != NULL ? path : "";
        if(sub[0] == 'T') {
            unsigned x = 0, y = 0, frames = 4;
            if(sscanf(args, "%u %u %u", &x, &y, &frames) < 2 || x >= 320 || y >= 240) {
                ftp_reply(s, "501 SITE TAP <x> <y> [frames]");
                return;
            }

            input_inject_tap((u16) x, (u16) y, frames);
            ftp_reply(s, "200 Tapped %u,%u.", x, y);
            return;
        }

        char keysText[64] = "";
        unsigned frames = 4;
        sscanf(args, "%63s %u", keysText, &frames);
        for(char* c = keysText; *c; c++) {
            if(*c >= 'a' && *c <= 'z') {
                *c = (char) (*c - 32);
            }
        }

        u32 keys = 0;
        char* save = NULL;
        for(char* k = strtok_r(keysText, "+", &save); k != NULL; k = strtok_r(NULL, "+", &save)) {
            u32 found = 0;
            for(u32 i = 0; i < sizeof(names) / sizeof(*names); i++) {
                if(strcmp(names[i].name, k) == 0) {
                    found = names[i].key;
                }
            }

            if(found == 0) {
                ftp_reply(s, "501 Unknown button %s.", k);
                return;
            }

            keys |= found;
        }

        if(keys == 0) {
            ftp_reply(s, "501 SITE PRESS <A|B|...|UP|DOWN|LEFT|RIGHT>[+...] [frames]");
            return;
        }

        input_inject_keys(keys, frames);
        ftp_reply(s, "200 Pressed.");
        return;
    }

    if(strcmp(sub, "INSTALL") != 0) {
        ftp_reply(s, "504 Unknown SITE command.");
        return;
    }

    if(path == NULL || *path == '\0') {
        ftp_reply(s, "501 SITE INSTALL <path to a .cia on the SD card>");
        return;
    }

    if(relaunch_pending()) {
        ftp_reply(s, "450 " APP_NAME " is restarting into the build just installed.");
        return;
    }

    char vpath[FTP_PATH_MAX];
    ftp_resolve_path(s, path, vpath, sizeof(vpath));

    char error[128] = "";
    ftp_set_phase(PHASE_INSTALLING, vpath);
    if(ftp_install_cia(vpath, error, sizeof(error))) {
        ftp_set_phase(PHASE_NONE, NULL);
        if(relaunch_pending()) {
            ftp_reply(s, "200 Installed %s; " APP_NAME " restarts in %d s.", vpath, RELAUNCH_DELAY_MS / 1000);
        } else {
            ftp_reply(s, "200 Installed %s.", vpath);
        }
    } else {
        ftp_set_phase(PHASE_NONE, NULL);
        ftp_reply(s, "550 Install failed: %s", error);
    }
}

static void ftp_handle_session(ftp_session* s) {
    ftp_reply(s, "220 " APP_NAME " FTP. SD card root; " APP_NAME "'s files are in " APP_DATA_DIR);

    char line[FTP_LINE_MAX];
    while(ftp_run && ftp_read_line(s, line, sizeof(line))) {
        char* arg = strchr(line, ' ');
        if(arg != NULL) {
            *arg++ = '\0';
        } else {
            arg = line + strlen(line);
        }

        for(char* c = line; *c; c++) {
            if(*c >= 'a' && *c <= 'z') {
                *c = (char) (*c - 32);
            }
        }

        char vpath[FTP_PATH_MAX];
        char rpath[FTP_PATH_MAX + 8];
        struct stat st;

        if(strcmp(line, "USER") == 0) {
            ftp_reply(s, "331 Any password.");
        } else if(strcmp(line, "PASS") == 0) {
            ftp_reply(s, "230 Logged in.");
        } else if(strcmp(line, "SYST") == 0) {
            ftp_reply(s, "215 UNIX Type: L8");
        } else if(strcmp(line, "FEAT") == 0) {
            ftp_reply(s, "211-Features:\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n MLSD\r\n EPSV\r\n PASV\r\n UTF8\r\n211 End");
        } else if(strcmp(line, "OPTS") == 0 || strcmp(line, "TYPE") == 0 || strcmp(line, "MODE") == 0 || strcmp(line, "STRU") == 0 || strcmp(line, "NOOP") == 0) {
            ftp_reply(s, "200 OK.");
        } else if(strcmp(line, "REST") == 0) {
            char* end = NULL;
            unsigned long long at = strtoull(arg, &end, 10);
            if(end == arg || *end != '\0') {
                ftp_reply(s, "501 REST takes a byte offset.");
            } else {
                s->rest = (u64) at;
                ftp_reply(s, "350 Restarting at %llu.", at);
            }
        } else if(strcmp(line, "ALLO") == 0) {
            // The size of the next STOR: the progress bar's total.
            s->allo = strtoull(arg, NULL, 10);
            ftp_reply(s, "200 OK.");
        } else if(strcmp(line, "PWD") == 0 || strcmp(line, "XPWD") == 0) {
            ftp_reply(s, "257 \"%s\"", s->cwd);
        } else if(strcmp(line, "CWD") == 0 || strcmp(line, "CDUP") == 0) {
            ftp_resolve_path(s, strcmp(line, "CDUP") == 0 ? ".." : arg, vpath, sizeof(vpath));
            ftp_real_path(vpath, rpath, sizeof(rpath));

            DIR* d = opendir(rpath);
            if(d != NULL) {
                closedir(d);
                snprintf(s->cwd, sizeof(s->cwd), "%s", vpath);
                ftp_reply(s, "250 OK.");
            } else {
                ftp_reply(s, "550 No such directory.");
            }
        } else if(strcmp(line, "PASV") == 0) {
            int port = ftp_open_pasv(s);
            if(port < 0) {
                ftp_reply(s, "425 Cannot open a data port.");
            } else {
                ftp_reply(s, "227 Entering Passive Mode (%lu,%lu,%lu,%lu,%d,%d).",
                          ftp_ip & 0xFF, (ftp_ip >> 8) & 0xFF, (ftp_ip >> 16) & 0xFF, (ftp_ip >> 24) & 0xFF, port >> 8, port & 0xFF);
            }
        } else if(strcmp(line, "EPSV") == 0) {
            int port = ftp_open_pasv(s);
            if(port < 0) {
                ftp_reply(s, "425 Cannot open a data port.");
            } else {
                ftp_reply(s, "229 Entering Extended Passive Mode (|||%d|).", port);
            }
        } else if(strcmp(line, "LIST") == 0) {
            ftp_cmd_list(s, arg, LIST_LONG);
        } else if(strcmp(line, "NLST") == 0) {
            ftp_cmd_list(s, arg, LIST_NAMES);
        } else if(strcmp(line, "MLSD") == 0) {
            ftp_cmd_list(s, arg, LIST_MACHINE);
        } else if(strcmp(line, "RETR") == 0) {
            ftp_cmd_retr(s, arg);
        } else if(strcmp(line, "STOR") == 0) {
            ftp_cmd_stor(s, arg, false);
        } else if(strcmp(line, "APPE") == 0) {
            ftp_cmd_stor(s, arg, true);
        } else if(strcmp(line, "SIZE") == 0 || strcmp(line, "MDTM") == 0) {
            ftp_resolve_path(s, arg, vpath, sizeof(vpath));
            ftp_real_path(vpath, rpath, sizeof(rpath));
            if(stat(rpath, &st) != 0 || S_ISDIR(st.st_mode)) {
                ftp_reply(s, "550 No such file.");
            } else if(line[0] == 'S') {
                ftp_reply(s, "213 %llu", (unsigned long long) st.st_size);
            } else {
                ftp_reply(s, "213 20260101000000");
            }
        } else if(strcmp(line, "DELE") == 0 || strcmp(line, "RMD") == 0 || strcmp(line, "XRMD") == 0 || strcmp(line, "MKD") == 0 || strcmp(line, "XMKD") == 0) {
            ftp_resolve_path(s, arg, vpath, sizeof(vpath));
            ftp_real_path(vpath, rpath, sizeof(rpath));

            int r;
            if(line[0] == 'D') {
                r = remove(rpath);
            } else if(strstr(line, "RMD") != NULL) {
                r = rmdir(rpath);
            } else {
                r = mkdir(rpath, 0777);
            }

            if(r != 0) {
                ftp_reply(s, "550 Failed.");
            } else if(strstr(line, "MKD") != NULL) {
                ftp_reply(s, "257 \"%s\" created.", vpath);
            } else {
                ftp_reply(s, "250 OK.");
            }
        } else if(strcmp(line, "RNFR") == 0) {
            ftp_resolve_path(s, arg, s->rnfr, sizeof(s->rnfr));
            ftp_reply(s, "350 Ready for RNTO.");
        } else if(strcmp(line, "RNTO") == 0) {
            char from[FTP_PATH_MAX + 8];
            ftp_real_path(s->rnfr, from, sizeof(from));
            ftp_resolve_path(s, arg, vpath, sizeof(vpath));
            ftp_real_path(vpath, rpath, sizeof(rpath));
            ftp_reply(s, s->rnfr[0] && rename(from, rpath) == 0 ? "250 Renamed." : "550 Rename failed.");
            s->rnfr[0] = '\0';
        } else if(strcmp(line, "SITE") == 0) {
            ftp_cmd_site(s, arg);
        } else if(strcmp(line, "QUIT") == 0) {
            ftp_reply(s, "221 Bye.");
            break;
        } else {
            ftp_reply(s, "502 Not implemented.");
        }
    }

    ftp_close_pasv(s);
}

static void ftp_session_free(ftp_session* s) {
    free(s->io);
    free(s);
}

// A session's thread: it serves its client, then takes itself off the list (ftp_shutdown waits for that).
static void ftp_session_thread(void* arg) {
    ftp_session* s = (ftp_session*) arg;
    ftp_handle_session(s);
    net_close_gently(s->ctrl);

    LightLock_Lock(&ftp_lock);
    for(u32 i = 0; i < FTP_MAX_SESSIONS; i++) {
        if(ftp_sessions[i] == s) {
            ftp_sessions[i] = NULL;
        }
    }

    LightLock_Unlock(&ftp_lock);
    ftp_session_free(s);
}

// Same core and priority as the server's thread (net_server_thread); detached: it frees itself when done.
static bool ftp_start_session_thread(ftp_session* s) {
    bool isNew = false;
    Thread thread = NULL;
    if(R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
        thread = threadCreate(ftp_session_thread, s, FTP_SESSION_STACK, 0x31, 2, true);
    }

    if(thread == NULL) {
        thread = threadCreate(ftp_session_thread, s, FTP_SESSION_STACK, 0x31, -2, true);
    }

    return thread != NULL;
}

// A new client: a session and its thread, or "421" when all places are taken (never left waiting for the greeting).
static void ftp_accept_session(int fd) {
    ftp_session* s = (ftp_session*) calloc(1, sizeof(ftp_session));
    if(s != NULL) {
        s->ctrl = fd;
        s->pasv = -1;
        s->data = -1;
        snprintf(s->cwd, sizeof(s->cwd), "/");
        s->io = (char*) malloc(FTP_IO_SIZE);
    }

    int slot = -1;
    if(s != NULL && s->io != NULL) {
        LightLock_Lock(&ftp_lock);
        for(int i = 0; i < FTP_MAX_SESSIONS && slot < 0; i++) {
            if(ftp_sessions[i] == NULL) {
                slot = i;
                ftp_sessions[i] = s;
            }
        }

        LightLock_Unlock(&ftp_lock);
    }

    if(slot >= 0 && ftp_start_session_thread(s)) {
        return;
    }

    if(slot >= 0) {
        LightLock_Lock(&ftp_lock);
        ftp_sessions[slot] = NULL;
        LightLock_Unlock(&ftp_lock);
    }

    const char* busy = slot < 0 && s != NULL && s->io != NULL ? "421 Too many connections: " APP_NAME " serves 3 at a time.\r\n"
                                                                : "421 Out of memory.\r\n";
    send(fd, busy, strlen(busy), 0);
    net_close_gently(fd);
    if(s != NULL) {
        ftp_session_free(s);
    }
}

static void ftp_main(void* arg) {
    while(ftp_run) {
        if(!ftp_wait_fd(ftp_listen, POLLIN, 0)) {
            continue;
        }

        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int fd = accept(ftp_listen, (struct sockaddr*) &peer, &len);
        if(fd < 0) {
            continue;
        }

        net_set_nonblocking(fd);
        ftp_accept_session(fd);
    }
}

static void ftp_shutdown();

bool ftpserver_start() {
    if(ftp_run) {
        return true;
    }

    ftp_paused = false;

    u32 wifi = 0;
    if(R_FAILED(ACU_GetWifiStatus(&wifi)) || wifi == 0) {
        snprintf(ftp_status, sizeof(ftp_status), "%s", _("No Wi-Fi"));
        return false;
    }

    ftp_init_locks();
    ftp_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(ftp_listen >= 0) {
        // Restarting right after a stop must not find the port still held.
        int yes = 1;
        setsockopt(ftp_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        net_set_nonblocking(ftp_listen);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FTP_SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if(ftp_listen < 0 || bind(ftp_listen, (struct sockaddr*) &addr, sizeof(addr)) != 0 || listen(ftp_listen, FTP_MAX_SESSIONS + 1) != 0) {
        snprintf(ftp_status, sizeof(ftp_status), _("Port %d busy"), FTP_SERVER_PORT);
        ftp_close_socket(&ftp_listen);
        return false;
    }

    ftp_ip = (u32) gethostid();
    ftp_run = true;

    ftp_thread = net_server_thread(ftp_main, 0x10000);
    if(ftp_thread == NULL) {
        ftp_shutdown();
        snprintf(ftp_status, sizeof(ftp_status), "%s", _("No thread"));
        return false;
    }

    return true;
}

static void ftp_shutdown() {
    ftp_run = false;

    // Wake anything blocked: the threads poll with short naps anyway.
    if(ftp_locks_ready) {
        LightLock_Lock(&ftp_lock);
        for(u32 i = 0; i < FTP_MAX_SESSIONS; i++) {
            ftp_session* s = ftp_sessions[i];
            if(s != NULL) {
                int fd = s->data;
                if(fd >= 0) {
                    shutdown(fd, SHUT_RDWR);
                }

                shutdown(s->ctrl, SHUT_RDWR);
            }
        }

        LightLock_Unlock(&ftp_lock);
    }

    if(ftp_thread != NULL) {
        threadJoin(ftp_thread, U64_MAX);
        threadFree(ftp_thread);
        ftp_thread = NULL;
    }

    // The sessions' threads end on their own (ftp_run is false) and take themselves off the list.
    for(bool any = ftp_locks_ready; any;) {
        any = false;
        LightLock_Lock(&ftp_lock);
        for(u32 i = 0; i < FTP_MAX_SESSIONS; i++) {
            any = any || ftp_sessions[i] != NULL;
        }

        LightLock_Unlock(&ftp_lock);
        if(any) {
            svcSleepThread(10 * 1000000LL);
        }
    }

    ftp_close_socket(&ftp_listen);
    ftp_set_phase(PHASE_NONE, NULL);
    snprintf(ftp_status, sizeof(ftp_status), "%s", _("Off"));
}

// Off for good (Settings, leaving NSA): a start still waiting for the Wi-Fi is
// dropped too, or it would turn the server back on.
// Started now, or as soon as there is Wi-Fi (NSA opened without it): it tries every couple of seconds, with no end.
void ftpserver_start_when_online() {
    if(ftpserver_start()) {
        return;
    }

    ftp_retry = true;
    ftp_retry_next = osGetTime() + 2000;
    ftp_retry_end = U64_MAX;
}

bool ftpserver_waiting() {
    return ftp_retry && !ftpserver_running();
}

void ftpserver_stop() {
    ftp_retry = false;
    ftp_paused = false;
    ftp_shutdown();
}

bool ftpserver_running() {
    return ftp_run;
}

void ftpserver_pause() {
    // A start still waiting for the Wi-Fi counts as running: otherwise the
    // resume after this pause would find nothing to bring back.
    if(ftp_run || ftp_retry) {
        ftp_retry = false;
        ftp_shutdown();
        ftp_paused = true;
    }
}

void ftpserver_resume_later() {
    if(ftp_paused) {
        ftp_paused = false;
        ftp_retry = true;
        ftp_retry_next = osGetTime() + 3000;
        ftp_retry_end = osGetTime() + 120000;
    }
}

// The Wi-Fi gone while serving: the server kept looking on but could not serve (its sockets dead) until HOME. It stops,
// and starts again once the Wi-Fi is back (as ftpserver_start_when_online).
static u64 ftp_watch_next = 0;

static void ftp_watch_wifi() {
    if(!ftp_run || ftp_paused || osGetTime() < ftp_watch_next) {
        return;
    }

    ftp_watch_next = osGetTime() + 2000;
    u32 wifi = 0;
    if(R_FAILED(ACU_GetWifiStatus(&wifi)) || wifi == 0) {
        ftp_shutdown();
        ftp_retry = true;
        ftp_retry_next = osGetTime() + 2000;
        ftp_retry_end = U64_MAX;
    }
}

void ftpserver_poll_main() {
    ftp_watch_wifi();

    if(ftp_retry && osGetTime() >= ftp_retry_next) {
        if(ftpserver_start() || osGetTime() > ftp_retry_end) {
            ftp_retry = false;
        } else {
            ftp_retry_next = osGetTime() + 2000;
        }
    }

    if(test_requested && !test_done) {
        selftest_run(test_command, test_reply, sizeof(test_reply));
        test_done = true;
    }

    if(!shot_requested || shot_done) {
        return;
    }

    u64 start = osGetTime();
    mkdir("sdmc:" APP_DATA_DIR, 0777);
    mkdir("sdmc:" APP_DATA_DIR "screenshots", 0777);
    shot_ok = screen_capture(GFX_TOP, "sdmc:" APP_DATA_DIR "screenshots/top.bmp")
              && screen_capture(GFX_BOTTOM, "sdmc:" APP_DATA_DIR "screenshots/bottom.bmp");
    shot_ms = (u32) (osGetTime() - start);
    shot_done = true;
}

void ftpserver_address(char* out, size_t size) {
    if(!ftp_run) {
        snprintf(out, size, "%s", ftp_status);
        return;
    }

    struct in_addr addr = {.s_addr = ftp_ip};
    snprintf(out, size, "%s:%d", inet_ntoa(addr), FTP_SERVER_PORT);
}

bool ftpserver_transferring() {
    return ftp_run && ftp_data_open > 0;
}

bool ftpserver_holding_music() {
    u64 now = osGetTime();
    if(ftp_run && ftp_data_open > 0 && now - ftp_data_since >= 1000) {
        return true;
    }

    return ftp_data_long && ftp_data_ended != 0 && now - ftp_data_ended < 3000;
}

void ftpserver_corner(char* out, size_t size) {
    if(!ftp_run) {
        out[0] = '\0';
        return;
    }

    struct in_addr addr = {.s_addr = ftp_ip};
    snprintf(out, size, "FTP %s:%d", inet_ntoa(addr), FTP_SERVER_PORT);
}

bool ftpserver_notice(char* out, size_t size, float* progress) {
    ftp_phase phase = ftp_current_phase;
    u64 done = ftp_done;
    u64 total = ftp_total;

    *progress = total > 0 ? (float) ((double) done / (double) total) : -1.0f;

    switch(phase) {
        case PHASE_RECEIVING:
            snprintf(out, size, _("FTP: receiving %s"), ftp_phase_file);
            return true;
        case PHASE_INSTALLING:
            snprintf(out, size, _("FTP: installing %s"), ftp_phase_file);
            return true;
        default:
            return false;
    }
}

