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
#include "../core/core.h"

#define FTP_PATH_MAX 512
#define FTP_LINE_MAX 600
#define FTP_IO_SIZE (64 * 1024)
#define FTP_NAP_MS 10
#define FTP_SEND_CHUNK (8 * 1024)
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
static int ftp_listen = -1;
static volatile int ftp_ctrl = -1;
static volatile int ftp_data = -1;
static u32 ftp_ip = 0;
static char ftp_status[48] = "Off";
static char* ftp_io = NULL;
static unsigned ftp_next_pasv = 0;

static volatile ftp_phase ftp_current_phase = PHASE_NONE;
static char ftp_phase_file[64];
static volatile u64 ftp_allo = 0;
static volatile u64 ftp_done = 0;
static volatile u64 ftp_total = 0;

typedef struct {
    int ctrl;
    int pasv;  // Listening data socket after PASV/EPSV, -1 when none.
    char cwd[FTP_PATH_MAX];
    char rnfr[FTP_PATH_MAX];
    char in[FTP_LINE_MAX * 2];
    unsigned inUsed;
} ftp_session;

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
// a blocking poll() would hold up the updater's and Remote Install's network
// calls for its whole timeout: ask without waiting and nap between asks.
static bool ftp_wait_fd(int fd, short events, int timeoutMs) {
    int waited = 0;
    while(ftp_run) {
        struct pollfd p = {.fd = fd, .events = events, .revents = 0};
        int r = poll(&p, 1, 0);
        if(r > 0) {
            return (p.revents & (events | POLLHUP | POLLERR)) != 0;
        }

        if(r < 0) {
            return false;
        }

        svcSleepThread((s64) FTP_NAP_MS * 1000000);
        waited += FTP_NAP_MS;
        if(timeoutMs > 0 && waited >= timeoutMs) {
            return false;
        }
    }

    return false;
}

static bool ftp_send_all(int fd, const char* data, size_t n) {
    while(n > 0) {
        if(!ftp_wait_fd(fd, POLLOUT, 30000)) {
            return false;
        }

        // Small pieces keep each blocking send short (see ftp_wait_fd).
        ssize_t w = send(fd, data, n > FTP_SEND_CHUNK ? FTP_SEND_CHUNK : n, 0);
        if(w <= 0) {
            return false;
        }

        data += w;
        n -= (size_t) w;
    }

    return true;
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

        if(!ftp_wait_fd(s->ctrl, POLLIN, FTP_IDLE_MS)) {
            return false;
        }

        ssize_t r = recv(s->ctrl, s->in + s->inUsed, sizeof(s->in) - 1 - s->inUsed, 0);
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

static int ftp_open_pasv(ftp_session* s) {
    ftp_close_pasv(s);

    for(unsigned attempt = 0; attempt < FTP_PASV_COUNT; attempt++) {
        unsigned port = FTP_PASV_FIRST + (ftp_next_pasv++ % FTP_PASV_COUNT);

        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if(fd < 0) {
            return -1;
        }

        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

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
    }

    ftp_close_pasv(s);
    ftp_data = fd;
    return fd;
}

static void ftp_close_data(int* fd) {
    ftp_close_socket(fd);
    ftp_data = -1;
}

static void ftp_set_phase(ftp_phase phase, const char* vpath) {
    if(vpath != NULL) {
        const char* name = strrchr(vpath, '/');
        string_copy(ftp_phase_file, name != NULL ? name + 1 : vpath, sizeof(ftp_phase_file));
    }

    ftp_current_phase = phase;
}

// ---- SITE INSTALL ----

static bool ftp_install_cia(const char* vpath, char* error, size_t errorSize) {
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
        if(R_FAILED(res = FSFILE_Read(input, &bytesRead, offset, ftp_io, want)) || bytesRead != want) {
            snprintf(error, errorSize, "SD read failed (%08lX)", res);
            goto done;
        }

        // AM refuses a lower version than the installed one at the first
        // write. Unlike the ports, NSA does not delete the installed title to
        // force it: that would also delete the game's save data.
        if(R_FAILED(res = FSFILE_Write(output, &bytesWritten, offset, ftp_io, bytesRead, 0)) || bytesWritten != bytesRead) {
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
    ftp_close_data(&data);
    ftp_reply(s, ok ? "226 Done." : "426 Transfer aborted.");
}

static void ftp_cmd_retr(ftp_session* s, const char* arg) {
    char vpath[FTP_PATH_MAX];
    char rpath[FTP_PATH_MAX + 8];
    ftp_resolve_path(s, arg, vpath, sizeof(vpath));
    ftp_real_path(vpath, rpath, sizeof(rpath));

    FILE* f = fopen(rpath, "rb");
    if(f == NULL) {
        ftp_close_pasv(s);
        ftp_reply(s, "550 Cannot open the file.");
        return;
    }

    ftp_reply(s, "150 Sending.");
    int data = ftp_accept_data(s);
    if(data < 0) {
        fclose(f);
        ftp_reply(s, "425 No data connection.");
        return;
    }

    bool ok = true;
    size_t n;
    while(ok && (n = fread(ftp_io, 1, FTP_IO_SIZE, f)) > 0) {
        ok = ftp_send_all(data, ftp_io, n);
    }

    fclose(f);
    ftp_close_data(&data);
    ftp_reply(s, ok ? "226 Done." : "426 Transfer aborted.");
}

static void ftp_cmd_stor(ftp_session* s, const char* arg, bool append) {
    char vpath[FTP_PATH_MAX];
    char rpath[FTP_PATH_MAX + 8];
    ftp_resolve_path(s, arg, vpath, sizeof(vpath));
    ftp_real_path(vpath, rpath, sizeof(rpath));

    FILE* f = fopen(rpath, append ? "ab" : "wb");
    if(f == NULL) {
        ftp_close_pasv(s);
        ftp_reply(s, "553 Cannot create the file.");
        return;
    }

    // A CIA coming in is shown on the bottom screen; ALLO gave its size.
    size_t len = strlen(vpath);
    bool cia = len > 4 && strcasecmp(vpath + len - 4, ".cia") == 0;
    if(cia) {
        ftp_done = 0;
        ftp_total = ftp_allo;
        ftp_set_phase(PHASE_RECEIVING, vpath);
    }

    ftp_allo = 0;

    ftp_reply(s, "150 Receiving.");
    int data = ftp_accept_data(s);
    if(data < 0) {
        fclose(f);
        if(cia) {
            ftp_set_phase(PHASE_NONE, NULL);
        }

        ftp_reply(s, "425 No data connection.");
        return;
    }

    bool ok = true;
    for(;;) {
        if(!ftp_wait_fd(data, POLLIN, 30000)) {
            ok = false;
            break;
        }

        ssize_t r = recv(data, ftp_io, FTP_IO_SIZE, 0);
        if(r == 0) {
            break;
        }

        if(r < 0 || fwrite(ftp_io, 1, (size_t) r, f) != (size_t) r) {
            ok = false;
            break;
        }

        ftp_done += (u64) r;
    }

    if(fclose(f) != 0) {
        ok = false;
    }

    ftp_close_data(&data);
    if(cia) {
        ftp_set_phase(PHASE_NONE, NULL);
    }

    ftp_reply(s, ok ? "226 Done." : "451 Transfer failed.");
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

static void ftp_handle_session(int ctrl) {
    ftp_session* s = (ftp_session*) calloc(1, sizeof(ftp_session));
    if(s == NULL) {
        return;
    }

    s->ctrl = ctrl;
    s->pasv = -1;
    snprintf(s->cwd, sizeof(s->cwd), "/");

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
            ftp_reply(s, "211-Features:\r\n SIZE\r\n MDTM\r\n MLSD\r\n EPSV\r\n PASV\r\n UTF8\r\n211 End");
        } else if(strcmp(line, "OPTS") == 0 || strcmp(line, "TYPE") == 0 || strcmp(line, "MODE") == 0 || strcmp(line, "STRU") == 0 || strcmp(line, "NOOP") == 0) {
            ftp_reply(s, "200 OK.");
        } else if(strcmp(line, "ALLO") == 0) {
            // The size of the next STOR: the progress bar's total.
            ftp_allo = strtoull(arg, NULL, 10);
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
    free(s);
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

        ftp_ctrl = fd;
        ftp_handle_session(fd);
        ftp_close_socket(&fd);
        ftp_ctrl = -1;
    }
}

bool ftpserver_start() {
    if(ftp_run) {
        return true;
    }

    ftp_paused = false;

    u32 wifi = 0;
    if(R_FAILED(ACU_GetWifiStatus(&wifi)) || wifi == 0) {
        snprintf(ftp_status, sizeof(ftp_status), "No Wi-Fi");
        return false;
    }

    ftp_io = (char*) malloc(FTP_IO_SIZE);
    ftp_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(ftp_listen >= 0) {
        // Restarting right after a stop must not find the port still held.
        int yes = 1;
        setsockopt(ftp_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FTP_SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if(ftp_io == NULL || ftp_listen < 0 || bind(ftp_listen, (struct sockaddr*) &addr, sizeof(addr)) != 0 || listen(ftp_listen, 1) != 0) {
        snprintf(ftp_status, sizeof(ftp_status), "Port %d busy", FTP_SERVER_PORT);
        ftp_close_socket(&ftp_listen);
        free(ftp_io);
        ftp_io = NULL;
        return false;
    }

    ftp_ip = (u32) gethostid();
    ftp_run = true;

    // Below the UI thread's priority, on the system core like NSA's tasks.
    ftp_thread = threadCreate(ftp_main, NULL, 0x10000, 0x19, 1, false);
    if(ftp_thread == NULL) {
        ftpserver_stop();
        snprintf(ftp_status, sizeof(ftp_status), "No thread");
        return false;
    }

    return true;
}

void ftpserver_stop() {
    ftp_run = false;

    // Wake anything blocked: the thread polls with short naps anyway.
    int fd = ftp_data;
    if(fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }

    fd = ftp_ctrl;
    if(fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }

    if(ftp_thread != NULL) {
        threadJoin(ftp_thread, U64_MAX);
        threadFree(ftp_thread);
        ftp_thread = NULL;
    }

    ftp_close_socket(&ftp_listen);
    free(ftp_io);
    ftp_io = NULL;
    ftp_set_phase(PHASE_NONE, NULL);
    snprintf(ftp_status, sizeof(ftp_status), "Off");
}

bool ftpserver_running() {
    return ftp_run;
}

void ftpserver_pause() {
    if(ftp_run) {
        ftpserver_stop();
        ftp_paused = true;
    }
}

void ftpserver_resume() {
    if(ftp_paused) {
        ftp_paused = false;
        ftpserver_start();
    }
}

void ftpserver_address(char* out, size_t size) {
    if(!ftp_run) {
        snprintf(out, size, "%s", ftp_status);
        return;
    }

    struct in_addr addr = {.s_addr = ftp_ip};
    snprintf(out, size, "%s:%d", inet_ntoa(addr), FTP_SERVER_PORT);
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
            snprintf(out, size, "FTP: receiving %s", ftp_phase_file);
            return true;
        case PHASE_INSTALLING:
            snprintf(out, size, "FTP: installing %s", ftp_phase_file);
            return true;
        default:
            return false;
    }
}

