#include <ctype.h>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"
#include "../../core/unarchive.h"
#include "../dlqueue.h"

typedef enum content_type_e {
    CONTENT_CIA,
    CONTENT_TICKET,
    CONTENT_3DSX_SMDH,
    CONTENT_SAVED      // "Save": the file goes to INSTALL_URL_SAVE_DIR as it is, nothing is installed.
} content_type;

// Where "Save" keeps the downloads, to install later from Files: with what arrives from another 3DS, as the SD
// card's root is the owner's.
#define INSTALL_URL_SAVE_DIR APP_DATA_DIR "received/"

// What became of each URL, for the list on the install screen.
typedef enum {
    URL_QUEUED,
    URL_INSTALLED,
    URL_FAILED,
    URL_SKIPPED,
    URL_SAVED
} url_state;

typedef struct {
    char urls[INSTALL_URLS_MAX][DOWNLOAD_URL_MAX];
    u8 states[INSTALL_URLS_MAX];

    char paths[INSTALL_URLS_MAX][FILE_PATH_MAX];

    void* userData;
    void (*finishedURL)(void* data, u32 index);
    void (*finishedAll)(void* data);
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index);

    bool saveOnly;   // A "Save" choice: the files go to INSTALL_URL_SAVE_DIR (and may be installed from there).
    bool exactPaths; // Saving: each file goes to its `paths` entry, replacing what is there (the Store's scripts).
    bool quiet;      // No "Saved in" or "Install finished." at the end: the caller goes on (errors are still shown).
    u32 mode;        // INSTALL_URL_CHOICE_*.
    bool currIsCia;  // The file being saved is a CIA.
    char* toInstall; // The CIAs saved, one a line, to install once the downloads end ("Save and install").
    // In the background (issue #41): B hides the progress screen and the download goes on (installurl_bg); Y pauses it
    // where it saves to the card (the .part stays), and A on the screen opened again resumes from there.
    ui_view* view;
    bool background;
    bool pausing;
    bool paused;
    u32 pausedAt;            // The URL that was coming when it paused.
    u32 before[URL_SAVED + 1];   // What the URLs done before a pause came to, for the summary.
    // A run of the download queue (dlqueue.c): in the background from the start, quiet, no retry questions; queueDone
    // tells it how it ended, and the queue (not this) runs finishedAll.
    void* queueItem;
    void (*queueDone)(void* item, u32 ok, u32 failed, bool paused, bool cancelled, const char* error);
    char queueError[160];
    bool archives;   // A remote install: a .zip or .7z is saved and offered to be extracted (issue #51).
    bool currIsArchive;
    char* toExtract; // The archives saved, one a line, to offer once the downloads end.
    char confirm[256];
    content_type contentType;
    u64 currTitleId;
    cia_seed_watch seedWatch;
    volatile bool n3dsContinue;
    ticket_info ticketInfo;
    char currPath[FILE_PATH_MAX];
    bool currIsPart;                  // Saving to /nsa/received/: currPath + ".part" is written, renamed once whole.
    char resumePath[FILE_PATH_MAX];   // An earlier try of this URL left resumePath + ".part" (issue #28), or empty.

    data_op_data installInfo;
} install_url_data;

// The confirmation's choices. Install: downloads and installs at once. The others save the file to
// INSTALL_URL_SAVE_DIR first (long pieces over the connections that pay off, http_download_file), and
// install it from there once the downloads end, deleting it after the third.
#define INSTALL_URL_CHOICE_INSTALL 0
#define INSTALL_URL_CHOICE_SAVE_INSTALL 1
#define INSTALL_URL_CHOICE_SAVE_INSTALL_DELETE 2
#define INSTALL_URL_CHOICE_SAVE 3
#define INSTALL_URL_CHOICE_QUEUE 4
#define INSTALL_URL_CHOICES 5
#define INSTALL_URL_CHOICE_CANCEL 5

static const char* action_install_url_choice_names[INSTALL_URL_CHOICES] = {"Install", "Save and install", "Save, install and delete", "Save", "Add to the download queue"};
static const char* action_install_url_choice_infos[INSTALL_URL_CHOICES] = {
        "Downloads and installs at the same time, with no extra space on the SD card. With links good for one request (Dropbox) it comes over one connection. If it fails, it starts again from the beginning.",
        "Downloads the file to /nsa/received/ first (it needs room on the SD card for all of it), over the connections that make it faster; then installs it and keeps the file.",
        "Downloads the file to /nsa/received/ first (it needs room on the SD card for all of it), over the connections that make it faster; then installs it and deletes the file.",
        "Only downloads the file to /nsa/received/ (it needs room on the SD card for all of it), over the connections that make it faster, to install it later from Files.",
        "Waits its turn in the download queue and installs in the background, one after the other, while you do something else. The queue pauses, reorders and cancels downloads."
};

static void action_install_url_free_data(install_url_data* data) {
    if(data->finishedAll != NULL) {
        data->finishedAll(data->userData);
    }

    free(data->toInstall);
    free(data->toExtract);
    free(data);
}

static void action_install_url_queue(char** list, const char* path) {
    size_t have = *list != NULL ? strlen(*list) : 0;
    size_t len = strlen(path);
    char* grown = (char*) realloc(*list, have + len + 2);
    if(grown != NULL) {
        memcpy(grown + have, path, len);
        grown[have + len] = '\n';
        grown[have + len + 1] = '\0';
        *list = grown;
    }
}

static void action_install_url_queue_install(install_url_data* data, const char* path) {
    action_install_url_queue(&data->toInstall, path);
}

// A .zip or a .7z, from its first bytes.
static bool action_install_url_is_archive(const void* block) {
    const u8* b = (const u8*) block;
    return b != NULL && ((b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4)
                         || (b[0] == '7' && b[1] == 'z' && b[2] == 0xBC && b[3] == 0xAF && b[4] == 0x27 && b[5] == 0x1C));
}

static void action_install_url_install_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2);

static void action_install_url_n3ds_onresponse(ui_view* view, void* data, u32 response) {
    ((install_url_data*) data)->n3dsContinue = response == PROMPT_YES;
}

static Result action_install_url_get_src_url(void* data, u32 index, char* url, size_t maxSize) {
    install_url_data* installData = (install_url_data*) data;

    strncpy(url, installData->urls[index], maxSize);
    return 0;
}

// "Game Name.cia" from ".../Game%20Name.cia?x=1": the URL's last part, decoded, without the characters FAT refuses.
static void action_install_url_save_name(const char* url, const void* initialReadBlock, char* out, size_t size) {
    // The name the server gave (Google Drive: "EUR - Paper Mario.cia" for .../file/d/ID/view), else the URL's.
    char given[FILE_NAME_MAX];
    if(http_file_name(given, sizeof(given))) {
        string_escape_file_name(out, given, size);
        out[size - 1] = '\0';
        return;
    }

    const char* start = strstr(url, "://");
    start = start != NULL ? start + 3 : url;
    const char* slash = strrchr(start, '/');
    const char* name = slash != NULL && slash[1] != '\0' ? slash + 1 : "";

    char decoded[FILE_NAME_MAX];
    size_t len = 0;
    for(const char* c = name; *c != '\0' && *c != '?' && *c != '#' && len < sizeof(decoded) - 1; c++) {
        unsigned hex = 0;
        if(c[0] == '%' && c[1] != '\0' && c[2] != '\0' && sscanf(c + 1, "%2x", &hex) == 1 && hex >= 0x20) {
            decoded[len++] = (char) hex;
            c += 2;
        } else {
            decoded[len++] = *c == '+' ? ' ' : *c;
        }
    }

    decoded[len] = '\0';
    if(len == 0) {
        string_copy(decoded, "download", sizeof(decoded));
    }

    // No extension: the one its content says.
    if(strchr(decoded, '.') == NULL) {
        u32 magic = *(const u32*) initialReadBlock;
        const char* ext = *(const u16*) initialReadBlock == 0x2020 ? ".cia"
                          : *(const u16*) initialReadBlock == 0x0100 ? ".tik"
                          : magic == 0x58534433 ? ".3dsx" : "";
        size_t used = strlen(decoded);
        string_copy(decoded + used, ext, sizeof(decoded) - used);
    }

    string_escape_file_name(out, decoded, size);
    out[size - 1] = '\0';
}

static bool action_install_url_exists(FS_Archive archive, const char* path) {
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return false;
    }

    Handle file = 0;
    bool exists = R_SUCCEEDED(FSUSER_OpenFile(&file, archive, *fsPath, FS_OPEN_READ, 0));
    if(exists) {
        FSFILE_Close(file);
    }

    fs_free_path_utf8(fsPath);
    return exists || fs_is_dir(archive, path);
}

// "Save": opens INSTALL_URL_SAVE_DIR's file for the download; a name in use gets " (2)", " (3)"...
// ---- a saved download that failed halfway, kept to resume (issue #28) ----
//
// Saving to /nsa/received/ writes <name>.part; once whole it becomes <name>. A try that fails (or is cancelled) after
// some bytes keeps <name>.part and writes <name>.part.info: the URL, the file's size and how much of it is whole (a
// download in order: everything before that). Retry, or a later download of the same URL, finds it and asks the
// server for the rest (http_download_callback_from); a server without ranges, or a file of another size, starts again.

#define INSTALL_URL_PART_INFO ".part.info"

static void action_install_url_part_paths(const char* path, char* part, char* info, size_t size) {
    snprintf(part, size, "%s.part", path);
    snprintf(info, size, "%s" INSTALL_URL_PART_INFO, path);
}

static void action_install_url_forget_part(const char* path, bool keepPart) {
    char part[FILE_PATH_MAX + 16];
    char info[FILE_PATH_MAX + 16];
    action_install_url_part_paths(path, part, info, sizeof(part));
    char local[FILE_PATH_MAX + 24];
    if(!keepPart) {
        snprintf(local, sizeof(local), "sdmc:%s", part);
        remove(local);
    }

    snprintf(local, sizeof(local), "sdmc:%s", info);
    remove(local);
}

static void action_install_url_keep_part(const char* path, const char* url, u64 size, u64 whole) {
    char part[FILE_PATH_MAX + 16];
    char info[FILE_PATH_MAX + 16];
    action_install_url_part_paths(path, part, info, sizeof(part));
    char local[FILE_PATH_MAX + 24];
    snprintf(local, sizeof(local), "sdmc:%s", info);
    FILE* f = fopen(local, "wb");
    if(f != NULL) {
        fprintf(f, "url=%s\nsize=%llu\nwhole=%llu\n", url, (unsigned long long) size, (unsigned long long) whole);
        fclose(f);
    }
}

static Result action_install_url_resume_from(void* data, u32 index, u64* from, u64* expected) {
    install_url_data* installData = (install_url_data*) data;
    installData->resumePath[0] = '\0';
    *from = 0;
    *expected = 0;
    if(!installData->saveOnly || installData->exactPaths) {
        return 0;
    }

    DIR* dir = opendir("sdmc:" INSTALL_URL_SAVE_DIR);
    if(dir == NULL) {
        return 0;
    }

    size_t suffix = strlen(INSTALL_URL_PART_INFO);
    struct dirent* entry = NULL;
    while((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if(len <= suffix || strcmp(entry->d_name + len - suffix, INSTALL_URL_PART_INFO) != 0) {
            continue;
        }

        char local[FILE_PATH_MAX + 24];
        snprintf(local, sizeof(local), "sdmc:" INSTALL_URL_SAVE_DIR "%s", entry->d_name);
        FILE* f = fopen(local, "rb");
        if(f == NULL) {
            continue;
        }

        char url[DOWNLOAD_URL_MAX + 8] = "";
        unsigned long long size = 0;
        unsigned long long whole = 0;
        char line[DOWNLOAD_URL_MAX + 16];
        while(fgets(line, sizeof(line), f) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            if(strncmp(line, "url=", 4) == 0) {
                string_copy(url, line + 4, sizeof(url));
            } else if(strncmp(line, "size=", 5) == 0) {
                size = strtoull(line + 5, NULL, 10);
            } else if(strncmp(line, "whole=", 6) == 0) {
                whole = strtoull(line + 6, NULL, 10);
            }
        }

        fclose(f);
        if(strcmp(url, installData->urls[index]) != 0 || size == 0 || whole == 0 || whole >= size) {
            continue;
        }

        // Its piece must still be there, at the file's size (it is made whole-sized at the start).
        char path[FILE_PATH_MAX];
        snprintf(path, sizeof(path), INSTALL_URL_SAVE_DIR "%.*s", (int) (len - suffix), entry->d_name);
        snprintf(local, sizeof(local), "sdmc:%s.part", path);
        struct stat st;
        if(stat(local, &st) != 0 || (u64) st.st_size != size) {
            continue;
        }

        string_copy(installData->resumePath, path, sizeof(installData->resumePath));
        *from = whole;
        *expected = size;
        break;
    }

    closedir(dir);
    return 0;
}

static Result action_install_url_open_save(install_url_data* installData, u32 index, void* initialReadBlock, u64 size, u32* handle) {
    installData->contentType = CONTENT_SAVED;
    installData->currIsCia = initialReadBlock != NULL && *(u16*) initialReadBlock == 0x2020;
    installData->currIsArchive = installData->archives && action_install_url_is_archive(initialReadBlock);
    installData->currIsPart = false;

    // What an earlier try left: on from it (the server agreed), or gone (it starts again).
    if(installData->resumePath[0] != '\0') {
        if(installData->installInfo.currResumeFrom > 0) {
            string_copy(installData->currPath, installData->resumePath, FILE_PATH_MAX);
            installData->currIsPart = true;
            size_t len = strlen(installData->currPath);
            installData->currIsCia = len > 4 && strcasecmp(installData->currPath + len - 4, ".cia") == 0;
            installData->currIsArchive = installData->archives && len > 4
                                         && (strcasecmp(installData->currPath + len - 4, ".zip") == 0 || strcasecmp(installData->currPath + len - 3, ".7z") == 0);

            char part[FILE_PATH_MAX + 16];
            char info[FILE_PATH_MAX + 16];
            action_install_url_part_paths(installData->currPath, part, info, sizeof(part));
            FS_Path* path = fs_make_path_utf8(part);
            if(path == NULL) {
                return R_APP_OUT_OF_MEMORY;
            }

            Result res = FSUSER_OpenFileDirectly(handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *path, FS_OPEN_WRITE, 0);
            fs_free_path_utf8(path);
            return res;
        }

        action_install_url_forget_part(installData->resumePath, false);
        installData->resumePath[0] = '\0';
    }

    char name[FILE_NAME_MAX];
    action_install_url_save_name(installData->urls[index], initialReadBlock, name, sizeof(name));

    FS_Archive sdmcArchive = 0;
    Result res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if(R_FAILED(res)) {
        return res;
    }

    if(installData->exactPaths && installData->paths[index][0] == '/') {
        // Where the caller says: its folders made, and a file already there replaced.
        string_copy(installData->currPath, installData->paths[index], FILE_PATH_MAX);
        char dir[FILE_PATH_MAX];
        string_copy(dir, installData->currPath, sizeof(dir));
        for(char* c = dir + 1; *c != '\0'; c++) {
            if(*c == '/') {
                *c = '\0';
                FS_Path* dirPath = fs_make_path_utf8(dir);
                if(dirPath != NULL) {
                    FSUSER_CreateDirectory(sdmcArchive, *dirPath, 0);
                    fs_free_path_utf8(dirPath);
                }

                *c = '/';
            }
        }

        FS_Path* path = fs_make_path_utf8(installData->currPath);
        if(path != NULL) {
            FSUSER_DeleteFile(sdmcArchive, *path);
            res = FSUSER_OpenFile(handle, sdmcArchive, *path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
            if(R_SUCCEEDED(res) && size > 0 && R_FAILED(res = FSFILE_SetSize(*handle, size))) {
                FSFILE_Close(*handle);
                *handle = 0;
                FSUSER_DeleteFile(sdmcArchive, *path);
            }

            fs_free_path_utf8(path);
        } else {
            res = R_APP_OUT_OF_MEMORY;
        }
    } else if(R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, APP_DATA_DIR)) && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, INSTALL_URL_SAVE_DIR))) {
        char base[FILE_NAME_MAX];
        string_get_file_name(base, name, sizeof(base));
        const char* ext = strlen(base) < strlen(name) ? name + strlen(base) : "";

        char part[FILE_PATH_MAX + 16];
        char info[FILE_PATH_MAX + 16];
        snprintf(installData->currPath, FILE_PATH_MAX, INSTALL_URL_SAVE_DIR "%s", name);
        action_install_url_part_paths(installData->currPath, part, info, sizeof(part));
        for(u32 n = 2; n < 1000 && (action_install_url_exists(sdmcArchive, installData->currPath) || action_install_url_exists(sdmcArchive, part)); n++) {
            snprintf(installData->currPath, FILE_PATH_MAX, INSTALL_URL_SAVE_DIR "%s (%lu)%s", base, n, ext);
            action_install_url_part_paths(installData->currPath, part, info, sizeof(part));
        }

        installData->currIsPart = true;
        FS_Path* path = fs_make_path_utf8(part);
        if(path != NULL) {
            res = FSUSER_OpenFile(handle, sdmcArchive, *path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);

            // Its whole size at once: the pieces are written where they go, and a write far past the end would
            // first fill the gap. A card without room for it gets no file.
            if(R_SUCCEEDED(res) && size > 0 && R_FAILED(res = FSFILE_SetSize(*handle, size))) {
                FSFILE_Close(*handle);
                *handle = 0;
                FSUSER_DeleteFile(sdmcArchive, *path);
            }

            fs_free_path_utf8(path);
        } else {
            res = R_APP_OUT_OF_MEMORY;
        }
    }

    FSUSER_CloseArchive(sdmcArchive);
    return res;
}

static Result action_install_url_open_dst(void* data, u32 index, void* initialReadBlock, u64 size, u32* handle) {
    install_url_data* installData = (install_url_data*) data;

    Result res = 0;

    installData->contentType = CONTENT_CIA;
    installData->currTitleId = 0;
    installData->n3dsContinue = false;
    memset(&installData->ticketInfo, 0, sizeof(installData->ticketInfo));
    memset(&installData->currPath, 0, sizeof(installData->currPath));

    // A .zip or .7z in a remote install is saved, to be offered for extraction once the downloads end.
    if(installData->saveOnly || (installData->archives && action_install_url_is_archive(initialReadBlock))) {
        return action_install_url_open_save(installData, index, initialReadBlock, size, handle);
    }

    if(*(u16*) initialReadBlock == 0x2020) {
        installData->contentType = CONTENT_CIA;

        u64 titleId = 0;
        if(R_SUCCEEDED(res = cia_get_title_id(&titleId, (u8*) initialReadBlock, installData->installInfo.bufferSize))) {
            FS_MediaType dest = fs_get_title_destination(titleId);

            bool n3ds = false;
            if(R_SUCCEEDED(APT_CheckNew3DS(&n3ds)) && !n3ds && ((titleId >> 28) & 0xF) == 2) {
                ui_view* view = prompt_display_yes_no("Confirmation", "Title is intended for New 3DS systems.\nContinue?", COLOR_TEXT, data, action_install_url_install_draw_top, action_install_url_n3ds_onresponse);
                if(view != NULL) {
                    svcWaitSynchronization(view->active, U64_MAX);
                }

                if(!installData->n3dsContinue) {
                    installData->states[index] = URL_SKIPPED;
                    return R_APP_SKIPPED;
                }
            }

            u16 version = 0;
            cia_get_title_version(&version, (u8*) initialReadBlock, installData->installInfo.bufferSize);

            if(R_SUCCEEDED(res = action_install_cia_start(dest, titleId, version, handle))) {
                installData->currTitleId = titleId;
                cia_seed_watch_start(&installData->seedWatch, (u8*) initialReadBlock, installData->installInfo.bufferSize);
            }
        }
    } else if(*(u16*) initialReadBlock == 0x0100) {
        if(R_SUCCEEDED(res = ticket_get_title_id(&installData->ticketInfo.titleId, (u8*) initialReadBlock, installData->installInfo.bufferSize))) {
            installData->contentType = CONTENT_TICKET;

            installData->ticketInfo.inUse = false;
            installData->ticketInfo.loaded = true;

            AM_DeleteTicket(installData->ticketInfo.titleId);
            res = AMNET_InstallTicketBegin(handle);
        }
    } else if(*(u32*) initialReadBlock == 0x58534433 /* 3DSX */ || *(u32*) initialReadBlock == 0x48444D53 /* SMDH */) {
        installData->contentType = CONTENT_3DSX_SMDH;

        FS_Archive sdmcArchive = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
            char dir[FILE_PATH_MAX];
            if(strlen(installData->paths[index]) > 0) {
                string_get_parent_path(dir, installData->paths[index], FILE_PATH_MAX);
                string_copy(installData->currPath, installData->paths[index], FILE_PATH_MAX);
            } else {
                char filename[FILE_NAME_MAX];
                string_get_path_file(filename, installData->urls[index], FILE_NAME_MAX);

                char name[FILE_NAME_MAX];
                string_get_file_name(name, filename, FILE_NAME_MAX);

                snprintf(dir, FILE_PATH_MAX, "/3ds/%s/", name);
                snprintf(installData->currPath, FILE_PATH_MAX, "/3ds/%s/%s.%s", name, name, *(u32*) initialReadBlock == 0x58534433 ? "3dsx" : "smdh");
            }

            if(R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, "/3ds/")) && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, dir))) {
                FS_Path* path = fs_make_path_utf8(installData->currPath);
                if(path != NULL) {
                    res = FSUSER_OpenFileDirectly(handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);

                    fs_free_path_utf8(path);
                } else {
                    res = R_APP_OUT_OF_MEMORY;
                }
            }

            FSUSER_CloseArchive(sdmcArchive);
        }
    } else {
        res = R_APP_BAD_DATA;
    }

    return res;
}

static Result action_install_url_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    install_url_data* installData = (install_url_data*) data;

    Result res = 0;

    if(succeeded) {
        if(installData->contentType == CONTENT_CIA) {
            if(R_SUCCEEDED(res = AM_FinishCiaInstall(handle))) {
                if(cia_seed_watch_needed(&installData->seedWatch)) {
                    http_download_seed(installData->currTitleId);
                }

                if(installData->currTitleId == 0x0004013800000002 || installData->currTitleId == 0x0004013820000002) {
                    res = AM_InstallFirm(installData->currTitleId);
                }
            }

            action_install_cia_ended(installData->currTitleId, R_SUCCEEDED(res));
        } else if(installData->contentType == CONTENT_TICKET) {
            res = AMNET_InstallTicketFinish(handle);
        } else if(installData->contentType == CONTENT_3DSX_SMDH || installData->contentType == CONTENT_SAVED) {
            res = FSFILE_Close(handle);

            // Whole: <name>.part becomes <name>.
            if(R_SUCCEEDED(res) && installData->contentType == CONTENT_SAVED && installData->currIsPart) {
                char part[FILE_PATH_MAX + 16];
                char info[FILE_PATH_MAX + 16];
                action_install_url_part_paths(installData->currPath, part, info, sizeof(part));
                char from[FILE_PATH_MAX + 24];
                char to[FILE_PATH_MAX + 24];
                snprintf(from, sizeof(from), "sdmc:%s", part);
                snprintf(to, sizeof(to), "sdmc:%s", installData->currPath);
                if(rename(from, to) != 0) {
                    res = R_APP_IO_ERROR;
                }

                action_install_url_forget_part(installData->currPath, true);
            }
        }
    } else if(installData->contentType == CONTENT_SAVED && installData->currIsPart) {
        // Failed or cancelled: what came stays, to resume.
        res = FSFILE_Close(handle);
        u64 whole = installData->installInfo.currWritten;
        if(whole > 0 && installData->installInfo.currTotal > whole) {
            action_install_url_keep_part(installData->currPath, installData->urls[index], installData->installInfo.currTotal, whole);
        } else {
            action_install_url_forget_part(installData->currPath, false);
        }
    } else {
        if(installData->contentType == CONTENT_CIA) {
            res = AM_CancelCIAInstall(handle);
            action_install_cia_ended(installData->currTitleId, false);
        } else if(installData->contentType == CONTENT_TICKET) {
            res = AMNET_InstallTicketAbort(handle);
        } else if(installData->contentType == CONTENT_3DSX_SMDH || installData->contentType == CONTENT_SAVED) {
            res = FSFILE_Close(handle);

            FS_Archive sdmcArchive = 0;
            if(R_SUCCEEDED(FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
                FS_Path* path = fs_make_path_utf8(installData->currPath);
                if(path != NULL) {
                    FSUSER_DeleteFile(sdmcArchive, *path);

                    fs_free_path_utf8(path);
                }

                FSUSER_CloseArchive(sdmcArchive);
            }
        }
    }

    if(succeeded && R_SUCCEEDED(res)) {
        installData->states[index] = installData->contentType == CONTENT_SAVED ? URL_SAVED : URL_INSTALLED;

        // "Save and install": installed from the SD card once the downloads end (what is not a CIA stays saved).
        if(installData->contentType == CONTENT_SAVED && installData->currIsCia && installData->mode != INSTALL_URL_CHOICE_SAVE) {
            action_install_url_queue_install(installData, installData->currPath);
        }

        // An archive: offered for extraction once the downloads end (a plain "Save" only saves it).
        if(installData->contentType == CONTENT_SAVED && installData->currIsArchive && installData->mode != INSTALL_URL_CHOICE_SAVE) {
            action_install_url_queue(&installData->toExtract, installData->currPath);
        }
    }

    if(R_SUCCEEDED(res) && installData->finishedURL != NULL) {
        installData->finishedURL(installData->userData, index);
    }

    return res;
}

static Result action_install_url_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    install_url_data* installData = (install_url_data*) data;
    if(installData->contentType == CONTENT_CIA && !installData->saveOnly) {
        cia_seed_watch_feed(&installData->seedWatch, buffer, offset, size);
    }

    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

static void action_install_url_file_name(const char* url, char* out, size_t size);

static bool action_install_url_error(void* data, u32 index, Result res, ui_view** errorView) {
    install_url_data* installData = (install_url_data*) data;
    installData->states[index] = URL_FAILED;

    // The queue shows it on its item, and goes on with the next one.
    if(installData->queueItem != NULL) {
        char name[FILE_NAME_MAX];
        action_install_url_file_name(installData->urls[index], name, sizeof(name));
        if((u32) res >= (u32) R_APP_HTTP_ERROR_BASE && (u32) res < (u32) R_APP_HTTP_ERROR_END) {
            snprintf(installData->queueError, sizeof(installData->queueError), "%s: HTTP %lu", name, (u32) res - (u32) R_APP_HTTP_ERROR_BASE);
        } else {
            snprintf(installData->queueError, sizeof(installData->queueError), "%s: %s (%08lX)", name,
                     res == R_APP_BUSY ? _("another transfer has the connection") : _("failed"), (u32) res);
        }
        *errorView = NULL;
        return true;
    }

    led_blink(false);

    char* url = installData->urls[index];
    if(installData->saveOnly) {
        *errorView = error_display_res(data, action_install_url_install_draw_top, res, strlen(url) > 38 ? "Failed to download from URL.\n%.35s..." : "Failed to download from URL.\n%.38s", url);
    } else if(strlen(url) > 38) {
        *errorView = error_display_res(data, action_install_url_install_draw_top, res, "Failed to install from URL.\n%.35s...", url);
    } else {
        *errorView = error_display_res(data, action_install_url_install_draw_top, res, "Failed to install from URL.\n%.38s", url);
    }

    return true;
}

// ---- the install screen: the current URL's progress on top, every URL below ----

// "Game Name.cia" from ".../Game%20Name.cia?x=1": the part after the last slash, decoded as the saved name is.
static void action_install_url_file_name(const char* url, char* out, size_t size) {
    const char* start = strstr(url, "://");
    start = start != NULL ? start + 3 : url;

    const char* slash = strrchr(start, '/');
    const char* name = slash != NULL && slash[1] != '\0' ? slash + 1 : start;
    size_t len = strcspn(name, "?#");

    size_t used = 0;
    for(size_t i = 0; i < len && used < size - 1; i++) {
        unsigned hex = 0;
        if(name[i] == '%' && i + 2 < len && sscanf(name + i + 1, "%2x", &hex) == 1 && hex >= 0x20) {
            out[used++] = (char) hex;
            i += 2;
        } else {
            out[used++] = name[i] == '+' ? ' ' : name[i];
        }
    }

    out[used] = '\0';
}

// "192.168.1.143" from "http://192.168.1.143:8080/game.cia".
static void action_install_url_host(const char* url, char* out, size_t size) {
    const char* start = strstr(url, "://");
    start = start != NULL ? start + 3 : url;

    size_t len = strcspn(start, ":/?#");
    if(len >= size) {
        len = size - 1;
    }

    memcpy(out, start, len);
    out[len] = '\0';
}

static u32 action_install_url_count(install_url_data* data, url_state state) {
    u32 count = data->before[state];
    for(u32 i = 0; i < data->installInfo.total; i++) {
        if(data->states[i] == state) {
            count++;
        }
    }

    return count;
}

// ---- archives in a remote install (issue #51): where each one goes, asked one after the other ----

typedef struct {
    char* archives;      // Still to offer, one a line (taken from the front).
    char* cias;          // To install once the archives are done (or NULL).
    bool deleteCias;
    bool deleteArchives;
    char path[FILE_PATH_MAX];
    char pattern[FILE_PATH_MAX + 8];
    char output[FILE_PATH_MAX + 8];
    char shown[FILE_PATH_MAX];
    bool sdLayout;           // Only what goes in the SD card's own folders (the loose files stay in the archive)...
    char top[FILE_NAME_MAX]; // ... under this top folder of it ("" at its root).
} url_extract;

// What an archive holds, read before it is taken out.
typedef struct {
    u32 files;
    bool allKnown;        // Every file is under a folder of the SD card's own layout (action_extract_sd_place), or loose.
    bool allKnownInTop;   // Every file is under one top folder, and under it in such a folder (or loose in it).
    bool has3dsx;
    bool oneTop;          // Every entry is under the same top folder (`top`).
    char top[FILE_NAME_MAX];
    u32 known;            // Files in such folders, at the root and under the top folder.
    u32 knownInTop;
    char places[96];      // The SD card's folders it fills, as the question names them ("/3ds/, /luma/").
    char placesInTop[96]; // The same for what its one top folder holds.
} url_archive_plan;

// A file with no folder that is no app (a README, a LICENSE): left in the archive when the rest goes to the SD card.
static bool action_install_url_loose(const char* path) {
    size_t len = strlen(path);
    return strchr(path, '/') == NULL && !(len > 5 && strcasecmp(path + len - 5, ".3dsx") == 0) && !(len > 4 && strcasecmp(path + len - 4, ".cia") == 0)
           && !(len > 5 && strcasecmp(path + len - 5, ".firm") == 0);
}

static void action_install_url_add_place(char* places, size_t size, const char* path) {
    char place[16];
    const char* slash = strchr(path, '/');
    size_t len = slash != NULL ? (size_t) (slash - path) : strlen(path);
    if(len == 0 || len + 3 > sizeof(place)) {
        return;
    }

    snprintf(place, sizeof(place), "/%.*s/", (int) len, path);
    for(char* c = place; *c != '\0'; c++) {
        *c = (char) tolower((unsigned char) *c);
    }

    if(strstr(places, place) == NULL && strlen(places) + strlen(place) + 3 < size) {
        if(places[0] != '\0') {
            strcat(places, ", ");
        }

        strcat(places, place);
    }
}

static void action_install_url_plan_entry(void* userData, const char* name, u64 size, bool dir) {
    url_archive_plan* plan = (url_archive_plan*) userData;
    while(name[0] == '/' || (name[0] == '.' && name[1] == '/')) {
        name += name[0] == '/' ? 1 : 2;
    }

    // The same top folder for every entry?
    const char* slash = strchr(name, '/');
    size_t topLen = slash != NULL ? (size_t) (slash - name) : strlen(name);
    bool inFolder = slash != NULL && (slash[1] != '\0' || dir);
    if(!inFolder && !dir) {
        plan->oneTop = false;
    } else if(plan->top[0] == '\0' && plan->oneTop) {
        if(topLen < sizeof(plan->top)) {
            memcpy(plan->top, name, topLen);
            plan->top[topLen] = '\0';
        } else {
            plan->oneTop = false;
        }
    } else if(strlen(plan->top) != topLen || strncmp(plan->top, name, topLen) != 0) {
        plan->oneTop = false;
    }

    if(dir) {
        return;
    }

    plan->files++;
    size_t len = strlen(name);
    if(len > 5 && strcasecmp(name + len - 5, ".3dsx") == 0) {
        plan->has3dsx = true;
    }

    if(action_extract_sd_place(name)) {
        plan->known++;
        action_install_url_add_place(plan->places, sizeof(plan->places), name);
    } else if(!action_install_url_loose(name)) {
        plan->allKnown = false;
    }

    if(slash != NULL && action_extract_sd_place(slash + 1)) {
        plan->knownInTop++;
        action_install_url_add_place(plan->placesInTop, sizeof(plan->placesInTop), slash + 1);
    } else if(slash == NULL || !action_install_url_loose(slash + 1)) {
        plan->allKnownInTop = false;
    }
}

// "^Name/" for one folder, its special characters taken as they are (minire.h).
static void action_install_url_folder_pattern(char* out, size_t size, const char* folder) {
    size_t at = 0;
    if(at + 1 < size) {
        out[at++] = '^';
    }

    for(const char* c = folder; *c != '\0' && at + 3 < size; c++) {
        if(strchr(".[]*+?^$\\", *c) != NULL) {
            out[at++] = '\\';
        }

        out[at++] = *c;
    }

    if(at + 1 < size) {
        out[at++] = '/';
    }

    out[at] = '\0';
}

static void action_install_url_extract_next(url_extract* run);

static void action_install_url_extract_done(void* userData) {
    action_install_url_extract_next((url_extract*) userData);
}

static void action_install_url_extract_answered(ui_view* view, void* data, u32 response) {
    url_extract* run = (url_extract*) data;
    if(response == PROMPT_YES) {
        action_extract_archive_to(run->path, run->pattern, run->output, run->shown, run->deleteArchives, run->sdLayout ? run->top : NULL,
                                  action_install_url_extract_done, run);
    } else {
        // No: it stays in /nsa/received/.
        action_install_url_extract_next(run);
    }
}

static void action_install_url_extract_skipped(ui_view* view, void* data, u32 response) {
    action_install_url_extract_next((url_extract*) data);
}

static void action_install_url_extract_next(url_extract* run) {
    char* line = run->archives;
    char* end = line != NULL ? strchr(line, '\n') : NULL;
    if(line == NULL || *line == '\0') {
        if(run->cias != NULL) {
            action_install_cia_paths_now(run->cias, run->deleteCias);
        }

        free(run->archives);
        free(run->cias);
        free(run);
        return;
    }

    size_t len = end != NULL ? (size_t) (end - line) : strlen(line);
    string_copy(run->path, line, len + 1 < sizeof(run->path) ? len + 1 : sizeof(run->path));
    memmove(line, end != NULL ? end + 1 : line + len, strlen(end != NULL ? end + 1 : line + len) + 1);

    const char* slash = strrchr(run->path, '/');
    const char* name = slash != NULL ? slash + 1 : run->path;

    url_archive_plan plan;
    memset(&plan, 0, sizeof(plan));
    plan.allKnown = true;
    plan.allKnownInTop = true;
    plan.oneTop = true;
    unarchive_progress listing;
    memset(&listing, 0, sizeof(listing));
    listing.userData = &plan;
    listing.listed = action_install_url_plan_entry;
    char local[FILE_PATH_MAX + 8];
    snprintf(local, sizeof(local), "sdmc:%s", run->path);
    u32 count = 0;
    Result res = unarchive_extract(local, "^", "", &listing, &count);

    static char text[FILE_PATH_MAX * 2 + 512];
    const char* sdLayoutTop = NULL;
    if(R_SUCCEEDED(res) && plan.known > 0 && plan.allKnown) {
        // Laid out as the SD card is (3ds/..., luma/...): onto its root.
        string_copy(run->pattern, "^", sizeof(run->pattern));
        string_copy(run->output, "sdmc:/", sizeof(run->output));
        string_copy(run->shown, "/", sizeof(run->shown));
        sdLayoutTop = "";
    } else if(R_SUCCEEDED(res) && plan.knownInTop > 0 && plan.oneTop && plan.top[0] != '\0' && plan.allKnownInTop) {
        // The same in one folder ("App-1.0/3ds/..."): what that folder holds, onto the root.
        action_install_url_folder_pattern(run->pattern, sizeof(run->pattern), plan.top);
        string_copy(run->output, "sdmc:/", sizeof(run->output));
        string_copy(run->shown, "/", sizeof(run->shown));
        string_copy(plan.places, plan.placesInTop, sizeof(plan.places));
        sdLayoutTop = plan.top;
    } else if(R_SUCCEEDED(res) && plan.has3dsx) {
        // A .3dsx and what goes with it: into a folder of its own in /3ds/ (its top folder's name, or the archive's).
        char folder[FILE_NAME_MAX];
        if(plan.oneTop && plan.top[0] != '\0') {
            action_install_url_folder_pattern(run->pattern, sizeof(run->pattern), plan.top);
            string_escape_file_name(folder, plan.top, sizeof(folder));
        } else {
            string_copy(run->pattern, "^", sizeof(run->pattern));
            string_get_file_name(folder, name, sizeof(folder));
        }

        snprintf(run->shown, sizeof(run->shown), "/3ds/%s/", folder);
        snprintf(run->output, sizeof(run->output), "sdmc:%s", run->shown);
        string_copy(plan.places, run->shown, sizeof(plan.places));
    } else {
        snprintf(text, sizeof(text),
                 R_FAILED(res) ? _("%s is saved in /nsa/received/, but it could not be read as an archive.")
                               : _("%s is saved in /nsa/received/. Its files are not laid out for the SD card (3ds/, luma/...) and it holds no .3dsx, so it is not extracted: extract it from Files where you want it."),
                 name);
        prompt_display_notify(_("Remote install"), text, COLOR_TEXT, run, NULL, action_install_url_extract_skipped);
        return;
    }

    run->sdLayout = sdLayoutTop != NULL;
    string_copy(run->top, sdLayoutTop != NULL ? sdLayoutTop : "", sizeof(run->top));
    snprintf(text, sizeof(text), run->deleteArchives ? _("Extract %s?\n\nIts files go to %s on the SD card; files with the same names are replaced. The archive is deleted afterwards. No keeps it in /nsa/received/.")
                                                     : _("Extract %s?\n\nIts files go to %s on the SD card; files with the same names are replaced. The archive stays in /nsa/received/."),
             name, plan.places);
    prompt_display_yes_no(_("Extract"), text, COLOR_TEXT, run, NULL, action_install_url_extract_answered);
}

static void action_install_url_extract_all(char* archives, char* cias, bool deleteCias, bool deleteArchives) {
    url_extract* run = (url_extract*) calloc(1, sizeof(url_extract));
    if(run == NULL) {
        free(archives);
        if(cias != NULL) {
            action_install_cia_paths_now(cias, deleteCias);
            free(cias);
        }

        return;
    }

    run->archives = archives;
    run->cias = cias;
    run->deleteCias = deleteCias;
    run->deleteArchives = deleteArchives;
    action_install_url_extract_next(run);
}

// The one download that runs, or waits paused, behind the screens (NULL: none).
static install_url_data* installurl_bg = NULL;

static void action_install_url_finish(ui_view* view, install_url_data* installData, bool onScreen);

// Paused: what was saved stays (its .part, resumed later); the rest waits. The screen stays for A to resume it.
static void action_install_url_paused(install_url_data* installData) {
    installData->pausing = false;
    installData->paused = true;
    installData->pausedAt = installData->installInfo.processed;
    installurl_bg = installData;
}

// Resumed: the URLs not done yet, from the one that paused (its .part goes on), in a new run of the same download.
static void action_install_url_resume(install_url_data* installData) {
    u32 kept = 0;
    for(u32 i = 0; i < installData->installInfo.total; i++) {
        u8 state = installData->states[i];
        if(state != URL_QUEUED) {
            installData->before[state]++;
            continue;
        }

        if(kept != i) {
            string_copy(installData->urls[kept], installData->urls[i], sizeof(installData->urls[kept]));
            string_copy(installData->paths[kept], installData->paths[i], sizeof(installData->paths[kept]));
        }

        installData->states[kept++] = URL_QUEUED;
    }

    installData->installInfo.total = kept;
    installData->installInfo.quietCancel = false;
    installData->paused = false;
    if(kept == 0) {
        installData->installInfo.finished = true;
        return;
    }

    Result res = task_data_op(&installData->installInfo);
    if(R_FAILED(res)) {
        installData->installInfo.finished = true;
        installData->installInfo.result = res;
        error_display_res(NULL, NULL, res, "Failed to initiate installation.");
    }
}

// The download behind the screens, on screen again (a tap on its toast): true when there was one.
bool action_install_url_reopen() {
    install_url_data* installData = installurl_bg;
    if(installData == NULL || installData->view == NULL || (!installData->background && !installData->paused) || ui_top() == installData->view) {
        return false;
    }

    installData->background = false;
    ui_push(installData->view);
    return true;
}

// The main loop: the download behind the screens ends there (its messages show over whatever is open).
void action_install_url_tick() {
    install_url_data* installData = installurl_bg;
    if(installData != NULL && installData->background && installData->installInfo.finished) {
        if(installData->pausing && installData->installInfo.result == R_APP_CANCELLED) {
            action_install_url_paused(installData);
            return;
        }

        installData->background = false;
        action_install_url_finish(installData->view, installData, false);
    }
}

// The toast while it runs or waits behind the screens.
bool action_install_url_notice(char* out, size_t size, float* progress) {
    install_url_data* installData = installurl_bg;
    if(installData == NULL || (!installData->background && !installData->paused) || ui_top() == installData->view) {
        return false;
    }

    data_op_data* info = &installData->installInfo;
    u32 index = installData->paused ? installData->pausedAt : info->processed;
    if(index >= info->total) {
        index = info->total > 0 ? info->total - 1 : 0;
    }

    char name[FILE_NAME_MAX];
    action_install_url_file_name(installData->urls[index], name, sizeof(name));
    if(installData->paused) {
        snprintf(out, size, "%s\n%s", _("Download paused: tap to open"), name);
        *progress = info->currTotal > 0 ? (float) ((double) info->currProcessed / (double) info->currTotal) : -1;
        return true;
    }

    u32 percent = info->currTotal > 0 ? (u32) (info->currProcessed * 100 / info->currTotal) : 0;
    char head[96];
    snprintf(head, sizeof(head), _("In the background, %lu %%: tap to open"), percent);
    if(info->total > 1) {
        size_t used = strlen(head);
        snprintf(head + used, sizeof(head) - used, " (%lu/%lu)", index + 1, info->total);
    }

    snprintf(out, size, "%s\n%s", head, name);
    *progress = info->currTotal > 0 ? (float) ((double) info->currProcessed / (double) info->currTotal) : WIDGET_PROGRESS_UNKNOWN;
    return true;
}

static void action_install_url_install_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    install_url_data* installData = (install_url_data*) data;
    // The Store's scripts drive their downloads step by step: those stay on screen.
    bool queued = installData->queueItem != NULL;
    bool canHide = queued || (!installData->exactPaths && (installurl_bg == NULL || installurl_bg == installData));
    bool canPause = queued || (installData->saveOnly && !installData->exactPaths);
    u32 down = hidKeysDown();

    if(installData->paused) {
        view->info = _(" resume    cancel    background");
        if(down & KEY_A) {
            ui_wake();
            action_install_url_resume(installData);
        } else if(down & KEY_X) {
            // What was saved of it goes too.
            installData->paused = false;
            installurl_bg = NULL;
            if(installData->pausedAt < installData->installInfo.total && installData->currPath[0] != '\0') {
                action_install_url_forget_part(installData->currPath, false);
            }

            installData->installInfo.result = R_APP_CANCELLED;
            action_install_url_finish(view, installData, true);
        } else if(down & KEY_B) {
            ui_pop();
        }

        return;
    }

    if(installData->installInfo.finished) {
        if(!queued && installData->pausing && installData->installInfo.result == R_APP_CANCELLED) {
            action_install_url_paused(installData);
            return;
        }

        action_install_url_finish(view, installData, true);
        return;
    }

    view->info = canHide ? (canPause ? _(" background    cancel    pause") : _(" background    cancel")) : _(" cancel");
    if(canHide && (down & KEY_B)) {
        installData->background = true;
        if(!queued) {
            installurl_bg = installData;
        }

        ui_pop();
    } else if((canHide && (down & KEY_X)) || (!canHide && (down & KEY_B))) {
        installData->installInfo.quietCancel = queued;
        svcSignalEvent(installData->installInfo.cancelEvent);
    } else if(canPause && (down & KEY_Y)) {
        installData->pausing = true;
        installData->installInfo.quietCancel = true;
        svcSignalEvent(installData->installInfo.cancelEvent);
    }
}

// The end of the download: its messages, then what comes next (the CIAs saved, the archives). `onScreen`: its screen is
// the top one (popped here), else it ended behind the screens.
static void action_install_url_finish(ui_view* view, install_url_data* installData, bool onScreen) {
    if(installurl_bg == installData) {
        installurl_bg = NULL;
    }

    {
        if(onScreen) {
            ui_pop();
        }

        ui_destroy(view);

        // A cancel has said so already, and so has a single URL that failed.
        u32 installed = action_install_url_count(installData, URL_INSTALLED);
        u32 saved = action_install_url_count(installData, URL_SAVED);
        u32 failed = action_install_url_count(installData, URL_FAILED);
        u32 skipped = action_install_url_count(installData, URL_SKIPPED);
        bool installAfter = installData->toInstall != NULL && installData->installInfo.result != R_APP_CANCELLED;
        bool extractAfter = installData->toExtract != NULL && installData->installInfo.result != R_APP_CANCELLED;
        // A failure lit it as its error showed; the summary of several URLs lights it again. Saved to install
        // next: the install says when it is done.
        if(installData->installInfo.result != R_APP_CANCELLED) {
            if(failed == 0 && skipped == 0 && (installed > 0 || (saved > 0 && !installAfter && !extractAfter))) {
                led_blink(true);
            } else if(failed > 0 && installData->installInfo.total > 1) {
                led_blink(false);
            }
        }
        if(installData->installInfo.result != R_APP_CANCELLED && !installData->quiet) {
            if(failed == 0 && skipped == 0) {
                if(saved > 0 && (installAfter || extractAfter)) {
                    // The install, or the extraction, says when it is done.
                } else if(saved > 0) {
                    char text[FILE_PATH_MAX + 64];
                    if(saved == 1) {
                        snprintf(text, sizeof(text), _("Saved in %s"), installData->currPath);
                    } else {
                        snprintf(text, sizeof(text), _("%lu files saved in %s"), saved, INSTALL_URL_SAVE_DIR);
                    }

                    prompt_display_notify("Success", text, COLOR_TEXT, NULL, NULL, NULL);
                } else if(installed > 0) {
                    prompt_display_notify("Success", "Install finished.", COLOR_TEXT, NULL, NULL, NULL);
                }
            } else if(installData->installInfo.total > 1) {
                char text[128];
                if(installData->saveOnly) {
                    snprintf(text, sizeof(text), _("Saved: %lu\nFailed: %lu\nSkipped: %lu"), saved, failed, skipped);
                } else {
                    snprintf(text, sizeof(text), _("Installed: %lu\nFailed: %lu\nSkipped: %lu"), installed, failed, skipped);
                }
                prompt_display_notify(failed > 0 ? "Failure" : "Success", text, COLOR_TEXT, NULL, NULL, NULL);
            }
        }

        char* paths = installAfter ? installData->toInstall : NULL;
        bool delete = installData->mode == INSTALL_URL_CHOICE_SAVE_INSTALL_DELETE;
        if(paths != NULL) {
            installData->toInstall = NULL;
        }

        // "Install" keeps nothing on the card, nor does "Save, install and delete": the archive goes once extracted.
        char* archives = extractAfter ? installData->toExtract : NULL;
        bool deleteArchives = installData->mode == INSTALL_URL_CHOICE_INSTALL || installData->mode == INSTALL_URL_CHOICE_SAVE_INSTALL_DELETE;
        if(archives != NULL) {
            installData->toExtract = NULL;
        }

        // A queue run: how it went, to the queue (which runs finishedAll itself when the item is over).
        void (*queueDone)(void* item, u32 ok, u32 failed, bool paused, bool cancelled, const char* error) = installData->queueDone;
        void* queueItem = installData->queueItem;
        bool queuePaused = installData->pausing && installData->installInfo.result == R_APP_CANCELLED;
        bool queueCancelled = !queuePaused && installData->installInfo.result == R_APP_CANCELLED;
        char queueError[160];
        string_copy(queueError, installData->queueError, sizeof(queueError));
        if(queueDone != NULL) {
            installData->finishedAll = NULL;
        }

        action_install_url_free_data(installData);

        if(queueDone != NULL) {
            queueDone(queueItem, installed + saved, failed, queuePaused, queueCancelled, queueError);
        }

        if(archives != NULL) {
            // The archives first, one question each; then the CIAs.
            action_install_url_extract_all(archives, paths, delete, deleteArchives);
        } else if(paths != NULL) {
            action_install_cia_paths_now(paths, delete);
            free(paths);
        }

        return;
    }
}

static void action_install_url_install_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    install_url_data* installData = (install_url_data*) data;
    const theme_current* t = theme();
    data_op_data* info = &installData->installInfo;
    if(info->total == 0) {
        return;
    }

    u32 index = info->processed < info->total ? info->processed : info->total - 1;

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = 100;
    widget_card(cardX, cardY, cardW, cardH);

    float cx = cardX + 14;
    float right = cardX + cardW - 14;
    float y = cardY + 12;

    char name[FILE_NAME_MAX];
    action_install_url_file_name(installData->urls[index], name, sizeof(name));
    screen_draw_icon(ICON_PACKAGE, cx, y + 1, 16, t->accent);
    widget_text_fit(name, cx + 22, y, WIDGET_TITLE_SCALE, t->palette->text, right - cx - 22);
    y += widget_text_height(WIDGET_TITLE_SCALE) + 4;

    char host[64];
    action_install_url_host(installData->urls[index], host, sizeof(host));
    char line[160];
    // As 3dsDL does: how many connections the file comes over.
    if(info->connections > 1) {
        snprintf(line, sizeof(line), _("From %s, %lu connections"), host, info->connections);
    } else if(info->connections == 1) {
        snprintf(line, sizeof(line), _("From %s, 1 connection"), host);
    } else {
        snprintf(line, sizeof(line), _("From %s"), host);
    }
    widget_text_fit(line, cx, y, WIDGET_TEXT_SCALE, t->palette->textMuted, right - cx);
    y += widget_text_height(WIDGET_TEXT_SCALE) + 10;

    float progress = info->currTotal != 0 ? (float) ((double) info->currProcessed / (double) info->currTotal) : 0;
    widget_progress(cx, y, right - cx, 8, progress);
    y += 8 + 8;

    snprintf(line, sizeof(line), _("%.2f %s / %.2f %s, %.2f %s/s, ETA %s"),
             ui_get_display_size(info->currProcessed), ui_get_display_size_units(info->currProcessed),
             ui_get_display_size(info->currTotal), ui_get_display_size_units(info->currTotal),
             ui_get_display_size(info->bytesPerSecond), ui_get_display_size_units(info->bytesPerSecond),
             ui_get_display_eta(info->estimatedRemainingSeconds));
    widget_text_fit(line, cx, y, WIDGET_SMALL_SCALE, t->palette->textMuted, right - cx);
    y += widget_text_height(WIDGET_SMALL_SCALE) + 8;

    if(info->total > 1) {
        snprintf(line, sizeof(line), _("%lu of %lu"), index + 1, info->total);
        widget_text_fit(line, cx, y, WIDGET_SMALL_SCALE, t->palette->text, right - cx);
    }

    widget_storage(SYSTEM_MEDIATYPE_SD, cardX + 4, y2 - 52, cardW - 8);
}

#define INSTALL_ROW_HEIGHT 28
#define INSTALL_ROW_GAP 4

static void action_install_url_install_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    install_url_data* installData = (install_url_data*) data;
    const theme_current* t = theme();
    data_op_data* info = &installData->installInfo;

    float x = x1 + 8;
    float w = x2 - x1 - 16;
    u32 fits = (u32) ((y2 - y1 - 8) / (INSTALL_ROW_HEIGHT + INSTALL_ROW_GAP));
    if(fits == 0) {
        return;
    }

    // Keep the current URL in view.
    u32 first = info->processed >= fits ? info->processed - fits + 1 : 0;
    if(first + fits > info->total) {
        first = info->total > fits ? info->total - fits : 0;
    }

    for(u32 i = first; i < info->total && i < first + fits; i++) {
        float y = y1 + 8 + (i - first) * (INSTALL_ROW_HEIGHT + INSTALL_ROW_GAP);
        bool current = i == info->processed && !info->finished;
        bool paused = installData->paused && i == installData->pausedAt;
        screen_frame_round_rect(x, y, w, INSTALL_ROW_HEIGHT, t->def->radiusSmall, t->def->border, current ? t->accent : t->palette->border, t->palette->surface);

        u32 icon = ICON_CLOCK;
        u32 iconColor = t->palette->textMuted;
        char status[32];
        if(current) {
            icon = ICON_DOWNLOAD;
            iconColor = t->accent;
            u32 hundredths = info->currTotal != 0 ? (u32) ((u64) info->currProcessed * 10000 / info->currTotal) : 0;
            snprintf(status, sizeof(status), "%lu.%02lu %%", hundredths / 100, hundredths % 100);
        } else if(installData->states[i] == URL_INSTALLED || installData->states[i] == URL_SAVED) {
            icon = ICON_CHECK;
            iconColor = t->palette->success;
            string_copy(status, installData->states[i] == URL_SAVED ? _("Saved") : _("Installed"), sizeof(status));
        } else if(installData->states[i] == URL_FAILED) {
            icon = ICON_X;
            iconColor = t->palette->danger;
            string_copy(status, _("Failed"), sizeof(status));
        } else if(installData->states[i] == URL_SKIPPED) {
            icon = ICON_X;
            string_copy(status, _("Skipped"), sizeof(status));
        } else if(paused) {
            icon = ICON_CLOCK;
            iconColor = t->accent;
            string_copy(status, _("Paused"), sizeof(status));
        } else {
            string_copy(status, _("Queued"), sizeof(status));
        }

        screen_draw_icon(icon, x + 7, y + (INSTALL_ROW_HEIGHT - 16) / 2, 16, iconColor);

        float statusW = widget_text_width(status, WIDGET_SMALL_SCALE);
        screen_draw_text(status, x + w - 8 - statusW, y + (INSTALL_ROW_HEIGHT - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, iconColor == t->palette->textMuted ? t->palette->textMuted : iconColor);

        char name[FILE_NAME_MAX];
        action_install_url_file_name(installData->urls[i], name, sizeof(name));
        float tx = x + 29;
        widget_text_fit(name, tx, y + (INSTALL_ROW_HEIGHT - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, t->palette->text, x + w - 16 - statusW - tx);
    }
}

// The queue run install_show has just made (action_install_url_run_queued takes it).
static install_url_data* action_install_url_last_queued = NULL;

static void action_install_url_install_show(install_url_data* data) {
    ui_view* view = ui_create();
    view->name = data->saveOnly ? _("Downloading") : _("Installing");
    view->id = data->saveOnly ? "Downloading" : "Installing";
    view->info = _(" cancel");
    view->data = data;
    data->view = view;
    view->update = action_install_url_install_update;
    view->drawTop = action_install_url_install_draw_top;
    view->drawBottom = action_install_url_install_draw_bottom;
    if(data->queueItem == NULL) {
        ui_push(view);
    } else {
        action_install_url_last_queued = data;
    }
}

static bool action_install_url_in_order = true;

void action_install_url_save_in_order(bool inOrder) {
    action_install_url_in_order = inOrder;
}

// "Add to the download queue": the URLs, and the caller's callbacks, go to the queue; this request ends here.
static void action_install_url_to_queue(install_url_data* installData) {
    size_t size = 1;
    for(u32 i = 0; i < installData->installInfo.total; i++) {
        size += strlen(installData->urls[i]) + 1;
    }

    char* urls = (char*) calloc(1, size);
    if(urls == NULL) {
        action_install_url_free_data(installData);
        return;
    }

    for(u32 i = 0; i < installData->installInfo.total; i++) {
        strcat(urls, installData->urls[i]);
        strcat(urls, "\n");
    }

    char name[FILE_NAME_MAX + 16];
    action_install_url_file_name(installData->urls[0], name, sizeof(name));
    if(installData->installInfo.total > 1) {
        size_t used = strlen(name);
        snprintf(name + used, sizeof(name) - used, " (+%lu)", installData->installInfo.total - 1);
    }

    if(dlqueue_add(name, urls, installData->archives, installData->userData, installData->finishedURL, installData->finishedAll)) {
        installData->finishedAll = NULL;   // The queue's now.
    }

    free(urls);
    action_install_url_free_data(installData);
}

static void action_install_url_confirm_onresponse(ui_view* view, void* data, u32 response) {
    install_url_data* installData = (install_url_data*) data;
    if(response == INSTALL_URL_CHOICE_QUEUE) {
        action_install_url_to_queue(installData);
        return;
    }

    installData->mode = response;
    installData->saveOnly = response != INSTALL_URL_CHOICE_INSTALL;
    // A file is saved in order, over the connections that pay off (as an install), on every model: long pieces written
    // where they go (http_download_file) made the SD card take seconds for each write once its cache was full (2DS: from
    // the start, 3.2-3.9 s for 256 KB; New 3DS XL: after ~400 MB of Google Drive, 54 ms -> 4 s), as cards do with several
    // places written at once. SITE SAVEORDER ANY (DEBUG) still tries the long pieces.
    installData->installInfo.dstAnyOrder = installData->saveOnly && !action_install_url_in_order;
    if(response < INSTALL_URL_CHOICES) {
        Result res = task_data_op(&installData->installInfo);
        if(R_SUCCEEDED(res)) {
            action_install_url_install_show(installData);
        } else if(installData->queueDone != NULL) {
            // A queue run that did not start: the queue says it on the item.
            void (*queueDone)(void* item, u32 ok, u32 failed, bool paused, bool cancelled, const char* error) = installData->queueDone;
            void* queueItem = installData->queueItem;
            action_install_url_free_data(installData);
            queueDone(queueItem, 0, 1, false, false, _("The download did not start."));
        } else {
            error_display_res(NULL, NULL, res, "Failed to initiate installation.");

            action_install_url_free_data(installData);
        }
    } else {
        action_install_url_free_data(installData);
    }
}

static void action_install_url_choose_clear(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static void action_install_url_choose_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    install_url_data* installData = (install_url_data*) data;

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < INSTALL_URL_CHOICES; i++) {
            list_item* item = (list_item*) calloc(1, sizeof(list_item));
            if(item != NULL) {
                string_copy(item->name, _(action_install_url_choice_names[i]), sizeof(item->name));
                item->color = COLOR_TEXT;
                item->data = (void*) (uintptr_t) i;
                linked_list_add(items, item);
            }
        }

        return;
    }

    // The web page can answer too: the same choices, and Cancel last.
    char webText[512];
    int webLen = snprintf(webText, sizeof(webText), "%s", installData->confirm);
    u32 shown = installData->installInfo.total < 5 ? installData->installInfo.total : 5;
    for(u32 i = 0; i < shown && webLen > 0 && (size_t) webLen < sizeof(webText); i++) {
        char name[FILE_NAME_MAX];
        action_install_url_file_name(installData->urls[i], name, sizeof(name));
        webLen += snprintf(webText + webLen, sizeof(webText) - webLen, "\n%s", name);
    }

    if(installData->installInfo.total > shown && webLen > 0 && (size_t) webLen < sizeof(webText)) {
        snprintf(webText + webLen, sizeof(webText) - webLen, "\n");
        webLen = (int) strlen(webText);
        snprintf(webText + webLen, sizeof(webText) - webLen, _("and %lu more"), installData->installInfo.total - shown);
    }

    char webNames[INSTALL_URL_CHOICES + 1][PROMPT_OPTION_TEXT_MAX];
    const char* webOptions[INSTALL_URL_CHOICES + 1];
    for(u32 i = 0; i < INSTALL_URL_CHOICES; i++) {
        string_copy(webNames[i], _(action_install_url_choice_names[i]), sizeof(webNames[i]));
        webOptions[i] = webNames[i];
    }

    string_copy(webNames[INSTALL_URL_CHOICES], _("Cancel"), sizeof(webNames[INSTALL_URL_CHOICES]));
    webOptions[INSTALL_URL_CHOICES] = webNames[INSTALL_URL_CHOICES];
    int webChoice = prompt_web_offer(installData, _("Install from URL"), webText, webOptions, INSTALL_URL_CHOICES + 1, false);

    // X saves at once, as the confirmation did (and the tests press).
    u32 choice = UINT32_MAX;
    u32 down = hidKeysDown();
    if(webChoice >= 0 && webChoice <= INSTALL_URL_CHOICE_CANCEL) {
        choice = (u32) webChoice;
    } else if(down & KEY_B) {
        choice = INSTALL_URL_CHOICE_CANCEL;
    } else if(down & KEY_X) {
        choice = INSTALL_URL_CHOICE_SAVE;
    } else if(selected != NULL && (selectedTouched || (down & KEY_A))) {
        choice = (u32) (uintptr_t) selected->data;
    }

    if(choice == UINT32_MAX) {
        return;
    }

    prompt_web_forget(installData);
    ui_pop();
    action_install_url_choose_clear(items);
    list_destroy(view);
    action_install_url_confirm_onresponse(NULL, installData, choice);
}

static void action_install_url_choose_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    install_url_data* installData = (install_url_data*) data;
    const theme_current* t = theme();

    // The question and what comes.
    float x = x1 + 12;
    float width = x2 - x1 - 24;
    float y = y1 + 10;
    float w = 0;
    float h = 0;
    screen_get_string_size_wrap(&w, &h, installData->confirm, 0.5f, 0.5f, width);
    screen_draw_string_wrap(installData->confirm, x, y, 0.5f, 0.5f, COLOR_TEXT, false, x + width);
    y += h + 6;

    // Each name whole, over as many lines as it needs; one address is shown whole too (decoded).
    u32 shown = installData->installInfo.total < 3 ? installData->installInfo.total : 3;
    for(u32 i = 0; i < shown; i++) {
        char name[FILE_NAME_MAX];
        action_install_url_file_name(installData->urls[i], name, sizeof(name));
        screen_draw_icon(ICON_PACKAGE, x, y + 1, 14, t->accent);
        float nameW = 0;
        float nameH = 0;
        screen_get_string_size_wrap(&nameW, &nameH, name, 0.5f, 0.5f, width - 20);
        screen_draw_text_wrap(name, x + 20, y, 0.5f, t->palette->text, x + width);
        y += nameH + 3;
    }

    if(installData->installInfo.total > shown) {
        char more[64];
        snprintf(more, sizeof(more), _("and %lu more"), installData->installInfo.total - shown);
        widget_text_fit(more, x + 20, y, 0.5f, t->palette->textMuted, width - 20);
        y += widget_text_height(0.5f) + 3;
    } else if(installData->installInfo.total == 1) {
        char address[DOWNLOAD_URL_MAX];
        string_url_decode(address, installData->urls[0], sizeof(address));
        float addressW = 0;
        float addressH = 0;
        screen_get_string_size_wrap(&addressW, &addressH, address, 0.4f, 0.4f, width);
        // Only while the card under it keeps room for its text.
        if(y + addressH + 4 + 70 <= y2) {
            screen_draw_text_wrap(address, x, y, 0.4f, t->palette->textMuted, x + width);
            y += addressH + 4;
        }
    }

    // What the chosen way does.
    if(selected == NULL) {
        return;
    }

    u32 choice = (u32) (uintptr_t) selected->data;
    if(choice >= INSTALL_URL_CHOICES) {
        return;
    }

    float cardY = y + 6;
    float cardH = y2 - 8 - cardY;
    widget_card(x1 + 8, cardY, x2 - x1 - 16, cardH);
    widget_text_fit(_(action_install_url_choice_names[choice]), x + 4, cardY + 8, WIDGET_TITLE_SCALE, t->accent, width - 8);
    screen_draw_string_wrap(_(action_install_url_choice_infos[choice]), x + 4, cardY + 10 + widget_text_height(WIDGET_TITLE_SCALE) + 4, 0.5f, 0.5f,
                            COLOR_TEXT, false, x + width - 4);
}

static install_url_data* action_install_url_start(const char* confirmMessage, const char* urls, const char* paths, void* userData,
                                     void (*finishedURL)(void* data, u32 index),
                                     void (*finishedAll)(void* data),
                                     void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index),
                                     int choice, bool exactPaths, bool archives, void* queueItem,
                                     void (*queueDone)(void* item, u32 ok, u32 failed, bool paused, bool cancelled, const char* error)) {
    install_url_data* data = (install_url_data*) calloc(1, sizeof(install_url_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate URL install data.");

        return NULL;
    }


    data->installInfo.total = 0;

    size_t payloadLen = strlen(urls);
    if(payloadLen > 0) {
        const char* currStart = urls;
        while(data->installInfo.total < INSTALL_URLS_MAX && currStart - urls < payloadLen) {
            const char* currEnd = strchr(currStart, '\n');
            if(currEnd == NULL) {
                currEnd = urls + payloadLen;
            }

            u32 len = currEnd - currStart;

            if((len < 7 || strncmp(currStart, "http://", 7) != 0) && (len < 8 || strncmp(currStart, "https://", 8) != 0)) {
                if(len > DOWNLOAD_URL_MAX - 8) {
                    len = DOWNLOAD_URL_MAX - 8;
                }

                string_copy(data->urls[data->installInfo.total], "http://", 8);
                string_copy(&data->urls[data->installInfo.total][7], currStart, len + 1);
            } else {
                if(len > DOWNLOAD_URL_MAX - 1) {
                    len = DOWNLOAD_URL_MAX - 1;
                }

                string_copy(data->urls[data->installInfo.total], currStart, len + 1);
            }

            data->installInfo.total++;
            currStart = currEnd + 1;
        }
    }

    if(paths != NULL) {
        size_t pathsLen = strlen(paths);
        if(pathsLen > 0) {
            const char* currStart = paths;
            for(u32 i = 0; i < data->installInfo.total && currStart - paths < pathsLen; i++) {
                const char* currEnd = strchr(currStart, '\n');
                if(currEnd == NULL) {
                    currEnd = paths + pathsLen;
                }

                u32 len = currEnd - currStart;
                if(len > FILE_PATH_MAX - 1) {
                    len = FILE_PATH_MAX - 1;
                }

                string_copy(data->paths[i], currStart, len + 1);

                currStart = currEnd + 1;
            }
        }
    }

    data->exactPaths = exactPaths;
    data->archives = archives;
    data->quiet = exactPaths;
    // A run of the download queue: in the background from the start, with no message and no retry question.
    if(queueItem != NULL) {
        data->queueItem = queueItem;
        data->queueDone = queueDone;
        data->quiet = true;
        data->background = true;
        data->installInfo.noRetry = true;
    }
    data->userData = userData;
    data->finishedURL = finishedURL;
    data->finishedAll = finishedAll;
    data->drawTop = drawTop;

    data->contentType = CONTENT_CIA;
    data->currTitleId = 0;
    data->n3dsContinue = false;
    memset(&data->ticketInfo, 0, sizeof(data->ticketInfo));
    memset(&data->currPath, 0, sizeof(data->currPath));

    data->installInfo.data = data;

    data->installInfo.op = DATAOP_DOWNLOAD;

    data->installInfo.bufferSize = 128 * 1024;

    data->installInfo.processed = data->installInfo.total;

    data->installInfo.getSrcUrl = action_install_url_get_src_url;

    data->installInfo.openDst = action_install_url_open_dst;
    data->installInfo.resumeFrom = action_install_url_resume_from;
    data->installInfo.closeDst = action_install_url_close_dst;
    data->installInfo.writeDst = action_install_url_write_dst;

    data->installInfo.error = action_install_url_error;

    data->installInfo.finished = true;

    // A choice made by the caller (a plain download, say): no question.
    if(choice >= 0) {
        action_install_url_confirm_onresponse(NULL, data, (u32) choice);
        return data;
    }

    // No message: the caller already asked (the updater shows its own prompt).
    if(confirmMessage == NULL) {
        action_install_url_confirm_onresponse(NULL, data, INSTALL_URL_CHOICE_INSTALL);
        return data;
    }

    // Install, save, or both (issues #22 and #29): what each one does on the top screen.
    string_copy(data->confirm, i18n_translate_text(confirmMessage), sizeof(data->confirm));
    ui_view* view = list_display(_("Install from URL"), _(" choose    save    cancel"), data, action_install_url_choose_update,
                                 action_install_url_choose_draw_top);
    if(view == NULL) {
        action_install_url_free_data(data);
        return NULL;
    }

    list_show_title_bar(view);
    view->id = "Confirmation";
    return data;
}

void action_install_url_choice(const char* confirmMessage, const char* urls, const char* paths, void* userData,
                               void (*finishedURL)(void* data, u32 index),
                               void (*finishedAll)(void* data),
                               void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index),
                               int choice) {
    action_install_url_start(confirmMessage, urls, paths, userData, finishedURL, finishedAll, drawTop, choice, false, false, NULL, NULL);
}

void action_install_url_remote(const char* confirmMessage, const char* urls, void* userData, void (*finishedAll)(void* data)) {
    action_install_url_start(confirmMessage, urls, NULL, userData, NULL, finishedAll, NULL, -1, false, true, NULL, NULL);
}

// The Store's scripts: each URL saved to its path (one a line), with no question and nothing said at the end.
void action_download_urls_to(const char* urls, const char* paths, void* userData, void (*finishedURL)(void* data, u32 index),
                             void (*finishedAll)(void* data)) {
    action_install_url_start(NULL, urls, paths, userData, finishedURL, finishedAll, NULL, INSTALL_URL_CHOICE_SAVE, true, false, NULL, NULL);
}

void action_install_url(const char* confirmMessage, const char* urls, const char* paths, void* userData,
                        void (*finishedURL)(void* data, u32 index),
                        void (*finishedAll)(void* data),
                        void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index)) {
    action_install_url_choice(confirmMessage, urls, paths, userData, finishedURL, finishedAll, drawTop, -1);
}

// Only downloads the file(s) to INSTALL_URL_SAVE_DIR, to install later from Files: no question first.
void action_save_url(const char* urls, void* userData, void (*finishedURL)(void* data, u32 index), void (*finishedAll)(void* data)) {
    action_install_url_choice(NULL, urls, NULL, userData, finishedURL, finishedAll, NULL, INSTALL_URL_CHOICE_SAVE);
}

// ---- the download queue's runs (dlqueue.c) ----

void* action_install_url_run_queued(const char* urls, bool archives, void* userData, void (*finishedURL)(void* data, u32 index),
                                    void (*done)(void* item, u32 ok, u32 failed, bool paused, bool cancelled, const char* error), void* item) {
    action_install_url_last_queued = NULL;
    action_install_url_start(NULL, urls, NULL, userData, finishedURL, NULL, NULL, INSTALL_URL_CHOICE_INSTALL, false, archives, item, done);
    // A run that did not start has already ended (and said so to the queue).
    install_url_data* data = action_install_url_last_queued;
    action_install_url_last_queued = NULL;
    return data;
}

void action_install_url_queued_poll(void* run) {
    install_url_data* data = (install_url_data*) run;
    if(data->installInfo.finished && ui_top() != data->view) {
        data->background = false;
        action_install_url_finish(data->view, data, false);
    }
}

void action_install_url_queued_stop(void* run, bool pause) {
    install_url_data* data = (install_url_data*) run;
    data->pausing = pause;
    data->installInfo.quietCancel = true;
    if(!data->installInfo.finished) {
        svcSignalEvent(data->installInfo.cancelEvent);
    }
}

void action_install_url_queued_open(void* run) {
    install_url_data* data = (install_url_data*) run;
    if(ui_top() != data->view) {
        data->background = false;
        ui_push(data->view);
    }
}

bool action_install_url_queued_on_screen(void* run) {
    return ui_top() == ((install_url_data*) run)->view;
}

void action_install_url_queued_progress(void* run, u64* done, u64* total, char* name, size_t size) {
    install_url_data* data = (install_url_data*) run;
    data_op_data* info = &data->installInfo;
    u32 index = info->processed < info->total ? info->processed : (info->total > 0 ? info->total - 1 : 0);
    *done = info->currProcessed;
    *total = info->currTotal;
    action_install_url_file_name(data->urls[index], name, size);
}
