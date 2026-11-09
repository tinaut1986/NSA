#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

// Installing a CIA of a lower version than the installed title (issue #27): AM will not install it over the title,
// which has to be deleted first, and its save data with it. So the user is asked, the save is copied to
// sdmc:/nsa/saves/<title ID>/<date>-before-install/ (a backup of NSA's, listed with the others), the title deleted
// and the CIA installed; once it is installed, the save is made again with the same format and put back. If the
// install fails, the backup stays where it is and the user is told.

#define INSTALL_SAVE_DIR APP_DATA_DIR "saves/"
#define INSTALL_SAVE_BUFFER (64 * 1024)

static struct {
    bool active;
    u64 titleId;
    FS_MediaType media;
    char dir[FILE_PATH_MAX];   // The backup folder, "/nsa/saves/<ID>/<date>-before-install/" (empty: no save).
} install_save_pending;

static FS_Path install_save_archive_path(u32* path, u64 titleId, FS_MediaType media) {
    path[0] = media;
    path[1] = (u32) (titleId & 0xFFFFFFFF);
    path[2] = (u32) (titleId >> 32);
    return (FS_Path) {PATH_BINARY, 12, path};
}

static void install_save_version_text(char* out, size_t size, u16 version) {
    snprintf(out, size, "%u.%u.%u", (unsigned) (version >> 10), (unsigned) ((version >> 4) & 0x3F), (unsigned) (version & 0xF));
}

// The title's name from its icon, or its ID.
static void install_save_title_name(char* out, size_t size, u64 titleId, FS_MediaType media) {
    snprintf(out, size, "%016llX", (unsigned long long) titleId);

    static const u32 filePath[5] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000};
    u32 archivePath[4] = {(u32) (titleId & 0xFFFFFFFF), (u32) (titleId >> 32), media, 0};
    Handle file = 0;
    if(R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SAVEDATA_AND_CONTENT, fs_make_path_binary(archivePath, sizeof(archivePath)),
                                        fs_make_path_binary(filePath, sizeof(filePath)), FS_OPEN_READ, 0))) {
        return;
    }

    SMDH* smdh = (SMDH*) calloc(1, sizeof(SMDH));
    u32 got = 0;
    if(smdh != NULL && R_SUCCEEDED(FSFILE_Read(file, &got, 0, smdh, sizeof(SMDH))) && got == sizeof(SMDH)) {
        SMDH_title* title = smdh_select_title(smdh);
        char name[0x81] = "";
        utf16_to_utf8((uint8_t*) name, title->shortDescription, sizeof(name) - 1);
        if(name[0] != '\0') {
            string_copy(out, name, size);
        }
    }

    free(smdh);
    FSFILE_Close(file);
}

// ---- the save to the SD card ----

static Result install_save_copy_out(FS_Archive save, const char* from, const char* to, u8* buffer, u32* files) {
    FS_Path* dirPath = fs_make_path_utf8(from);
    if(dirPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Handle dir = 0;
    Result res = FSUSER_OpenDirectory(&dir, save, *dirPath);
    fs_free_path_utf8(dirPath);
    if(R_FAILED(res)) {
        return res;
    }

    FS_DirectoryEntry entry;
    u32 count = 0;
    while(R_SUCCEEDED(res) && R_SUCCEEDED(res = FSDIR_Read(dir, &count, 1, &entry)) && count == 1) {
        char name[FILE_NAME_MAX] = "";
        utf16_to_utf8((uint8_t*) name, entry.name, sizeof(name) - 1);

        char inner[FILE_PATH_MAX];
        char outer[FILE_PATH_MAX];
        snprintf(inner, sizeof(inner), "%s%s", from, name);
        snprintf(outer, sizeof(outer), "%s%s", to, name);

        if(entry.attributes & FS_ATTRIBUTE_DIRECTORY) {
            mkdir(outer, 0777);
            strncat(inner, "/", sizeof(inner) - strlen(inner) - 1);
            strncat(outer, "/", sizeof(outer) - strlen(outer) - 1);
            res = install_save_copy_out(save, inner, outer, buffer, files);
            continue;
        }

        FS_Path* filePath = fs_make_path_utf8(inner);
        if(filePath == NULL) {
            res = R_APP_OUT_OF_MEMORY;
            break;
        }

        Handle file = 0;
        res = FSUSER_OpenFile(&file, save, *filePath, FS_OPEN_READ, 0);
        fs_free_path_utf8(filePath);
        if(R_FAILED(res)) {
            break;
        }

        FILE* out = fopen(outer, "wb");
        if(out == NULL) {
            res = R_APP_IO_ERROR;
        }

        for(u64 at = 0; out != NULL && R_SUCCEEDED(res) && at < entry.fileSize;) {
            u32 got = 0;
            if(R_SUCCEEDED(res = FSFILE_Read(file, &got, at, buffer, INSTALL_SAVE_BUFFER)) && (got == 0 || fwrite(buffer, 1, got, out) != got)) {
                res = R_APP_IO_ERROR;
            }

            at += got;
        }

        if(out != NULL && fclose(out) != 0 && R_SUCCEEDED(res)) {
            res = R_APP_IO_ERROR;
        }

        FSFILE_Close(file);
        (*files)++;
    }

    FSDIR_Close(dir);
    return res;
}

// Copies the title's save to a new backup folder (`dir` gets it, empty when the title has no save).
static Result install_save_backup(u64 titleId, FS_MediaType media, u16 version, const char* name, char* dir, size_t size) {
    dir[0] = '\0';

    u32 pathData[3];
    FS_Path path = install_save_archive_path(pathData, titleId, media);

    FS_Archive save = 0;
    if(R_FAILED(FSUSER_OpenArchive(&save, ARCHIVE_USER_SAVEDATA, path))) {
        return 0;   // No save (never started): nothing to keep.
    }

    u32 totalSize = 0;
    u32 directories = 0;
    u32 files = 0;
    bool duplicate = false;
    Result res = FSUSER_GetFormatInfo(&totalSize, &directories, &files, &duplicate, ARCHIVE_USER_SAVEDATA, path);

    char stamp[32];
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    if(tm == NULL || strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm) == 0) {
        snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long) osGetTime());
    }

    snprintf(dir, size, INSTALL_SAVE_DIR "%016llX/%s-before-install/", (unsigned long long) titleId, stamp);

    char local[FILE_PATH_MAX + 16];
    if(R_SUCCEEDED(res)) {
        mkdir("sdmc:" APP_DATA_DIR, 0777);
        mkdir("sdmc:" INSTALL_SAVE_DIR, 0777);
        snprintf(local, sizeof(local), "sdmc:" INSTALL_SAVE_DIR "%016llX", (unsigned long long) titleId);
        mkdir(local, 0777);
        snprintf(local, sizeof(local), "sdmc:%s", dir);
        mkdir(local, 0777);
        snprintf(local, sizeof(local), "sdmc:%ssave/", dir);
        if(mkdir(local, 0777) != 0) {
            res = R_APP_IO_ERROR;
        }
    }

    u8* buffer = R_SUCCEEDED(res) ? (u8*) malloc(INSTALL_SAVE_BUFFER) : NULL;
    if(R_SUCCEEDED(res) && buffer == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    }

    u32 copied = 0;
    if(R_SUCCEEDED(res)) {
        res = install_save_copy_out(save, "/", local, buffer, &copied);
    }

    free(buffer);
    FSUSER_CloseArchive(save);

    if(R_SUCCEEDED(res)) {
        // NSA's manifest (so it shows with the title's backups), and the format to make the save again.
        snprintf(local, sizeof(local), "sdmc:%smanifest.txt", dir);
        FILE* f = fopen(local, "wb");
        if(f != NULL) {
            fprintf(f, "source=NSA\nkind=save\ntitleid=%016llX\nversion=%u\nname=%s\ndate=%s\n", (unsigned long long) titleId,
                    (unsigned) version, name, stamp);
            fclose(f);
        }

        snprintf(local, sizeof(local), "sdmc:%sformat.txt", dir);
        f = fopen(local, "wb");
        if(f == NULL || fprintf(f, "size=%lu\ndirectories=%lu\nfiles=%lu\nduplicate=%d\n", totalSize, directories, files, duplicate ? 1 : 0) <= 0) {
            res = R_APP_IO_ERROR;
        }

        if(f != NULL) {
            fclose(f);
        }
    }

    if(R_FAILED(res)) {
        FS_Archive sd = 0;
        if(R_SUCCEEDED(FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
            char trimmed[FILE_PATH_MAX];
            string_copy(trimmed, dir, sizeof(trimmed));
            trimmed[strlen(trimmed) - 1] = '\0';
            FS_Path* fsPath = fs_make_path_utf8(trimmed);
            if(fsPath != NULL) {
                FSUSER_DeleteDirectoryRecursively(sd, *fsPath);
                fs_free_path_utf8(fsPath);
            }

            FSUSER_CloseArchive(sd);
        }

        dir[0] = '\0';
    }

    return res;
}

// ---- and back ----

static Result install_save_copy_in(FS_Archive save, const char* from, const char* to, u8* buffer) {
    DIR* dir = opendir(from);
    if(dir == NULL) {
        return R_APP_IO_ERROR;
    }

    Result res = 0;
    struct dirent* entry = NULL;
    while(R_SUCCEEDED(res) && (entry = readdir(dir)) != NULL) {
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char outer[FILE_PATH_MAX + 16];
        char inner[FILE_PATH_MAX];
        snprintf(outer, sizeof(outer), "%s%s", from, entry->d_name);
        snprintf(inner, sizeof(inner), "%s%s", to, entry->d_name);

        struct stat st;
        if(stat(outer, &st) != 0) {
            res = R_APP_IO_ERROR;
            break;
        }

        FS_Path* path = fs_make_path_utf8(inner);
        if(path == NULL) {
            res = R_APP_OUT_OF_MEMORY;
            break;
        }

        if(S_ISDIR(st.st_mode)) {
            res = FSUSER_CreateDirectory(save, *path, 0);
            fs_free_path_utf8(path);
            strncat(outer, "/", sizeof(outer) - strlen(outer) - 1);
            strncat(inner, "/", sizeof(inner) - strlen(inner) - 1);
            if(R_SUCCEEDED(res)) {
                res = install_save_copy_in(save, outer, inner, buffer);
            }

            continue;
        }

        // A save's files have their size when they are made.
        Handle file = 0;
        if(R_SUCCEEDED(res = FSUSER_CreateFile(save, *path, 0, (u64) st.st_size))
           && R_SUCCEEDED(res = FSUSER_OpenFile(&file, save, *path, FS_OPEN_WRITE, 0))) {
            FILE* in = fopen(outer, "rb");
            if(in == NULL) {
                res = R_APP_IO_ERROR;
            }

            for(u64 at = 0; in != NULL && R_SUCCEEDED(res) && at < (u64) st.st_size;) {
                size_t got = fread(buffer, 1, INSTALL_SAVE_BUFFER, in);
                u32 written = 0;
                if(got == 0) {
                    res = R_APP_IO_ERROR;
                } else if(R_SUCCEEDED(res = FSFILE_Write(file, &written, at, buffer, (u32) got, 0)) && written != got) {
                    res = R_APP_IO_ERROR;
                }

                at += got;
            }

            if(in != NULL) {
                fclose(in);
            }

            FSFILE_Close(file);
        }

        fs_free_path_utf8(path);
    }

    closedir(dir);
    return res;
}

static Result install_save_restore(u64 titleId, FS_MediaType media, const char* dir) {
    char local[FILE_PATH_MAX + 16];
    snprintf(local, sizeof(local), "sdmc:%sformat.txt", dir);
    unsigned long size = 0, directories = 0, files = 0;
    int duplicate = 0;
    FILE* f = fopen(local, "rb");
    if(f == NULL) {
        return R_APP_IO_ERROR;
    }

    int read = fscanf(f, "size=%lu\ndirectories=%lu\nfiles=%lu\nduplicate=%d", &size, &directories, &files, &duplicate);
    fclose(f);
    if(read != 4) {
        return R_APP_BAD_DATA;
    }

    // 0x200 blocks, as games format their own save: the size that counts is the one in the title's exheader (3dbrew,
    // FS:FormatSaveData). GetFormatInfo's size is not in blocks of 512 bytes (a save made with 0x200 gave 134328240).
    u32 pathData[3];
    FS_Path path = install_save_archive_path(pathData, titleId, media);
    u32 directoryBuckets = directories > 0 ? directories : 1;
    u32 fileBuckets = files > 0 ? files : 1;
    Result res = FSUSER_FormatSaveData(ARCHIVE_USER_SAVEDATA, path, 0x200, directories, files, directoryBuckets, fileBuckets, duplicate != 0);
    if(R_FAILED(res)) {
        return res;
    }

    FS_Archive save = 0;
    if(R_FAILED(res = FSUSER_OpenArchive(&save, ARCHIVE_USER_SAVEDATA, path))) {
        return res;
    }

    u8* buffer = (u8*) malloc(INSTALL_SAVE_BUFFER);
    if(buffer == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    } else {
        snprintf(local, sizeof(local), "sdmc:%ssave/", dir);
        res = install_save_copy_in(save, local, "/", buffer);
        free(buffer);
    }

    if(R_SUCCEEDED(res)) {
        res = FSUSER_ControlArchive(save, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
    }

    FSUSER_CloseArchive(save);
    return res;
}

// ---- the question, and what follows the install ----

typedef struct {
    volatile bool yes;
} install_save_answer;

static void install_save_onresponse(ui_view* view, void* data, u32 response) {
    ((install_save_answer*) data)->yes = response == PROMPT_YES;
}

Result action_install_cia_lower(FS_MediaType dest, u64 titleId, u16 version, u16 installedVersion) {
    char name[FILE_NAME_MAX];
    install_save_title_name(name, sizeof(name), titleId, dest);

    u32 pathData[3];
    FS_Archive save = 0;
    bool hasSave = R_SUCCEEDED(FSUSER_OpenArchive(&save, ARCHIVE_USER_SAVEDATA, install_save_archive_path(pathData, titleId, dest)));
    if(hasSave) {
        FSUSER_CloseArchive(save);
    }

    char have[16];
    char want[16];
    install_save_version_text(have, sizeof(have), installedVersion);
    install_save_version_text(want, sizeof(want), version);

    char text[512];
    snprintf(text, sizeof(text),
             hasSave ? _("%s is installed (v%s); this CIA is v%s, an older one.\nTo install it, the installed title is deleted first.\nIts save data is backed up and put back.")
                     : _("%s is installed (v%s); this CIA is v%s, an older one.\nTo install it, the installed title is deleted first."),
             name, have, want);

    install_save_answer answer = {false};
    ui_view* view = prompt_display_yes_no(_("Older version"), text, COLOR_TEXT, &answer, NULL, install_save_onresponse);
    if(view != NULL) {
        svcWaitSynchronization(view->active, U64_MAX);
    }

    if(!answer.yes) {
        return R_APP_SKIPPED;
    }

    install_save_pending.active = false;
    char dir[FILE_PATH_MAX] = "";
    if(hasSave) {
        Result res = install_save_backup(titleId, dest, installedVersion, name, dir, sizeof(dir));
        if(R_FAILED(res)) {
            // Nothing deleted: the install stops here with this error.
            return res;
        }
    }

    install_save_pending.active = true;
    install_save_pending.titleId = titleId;
    install_save_pending.media = dest;
    string_copy(install_save_pending.dir, dir, sizeof(install_save_pending.dir));
    return 0;
}

void action_install_cia_ended(u64 titleId, bool installed) {
    if(!install_save_pending.active || install_save_pending.titleId != titleId) {
        return;
    }

    install_save_pending.active = false;
    if(install_save_pending.dir[0] == '\0') {
        return;
    }

    char text[FILE_PATH_MAX + 128];
    if(!installed) {
        snprintf(text, sizeof(text), _("The install did not finish. The save data of the title that was deleted is kept in %s."), install_save_pending.dir);
        prompt_display_notify(_("Save data kept"), text, COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    Result res = install_save_restore(titleId, install_save_pending.media, install_save_pending.dir);
    if(R_FAILED(res)) {
        snprintf(text, sizeof(text), _("Installed, but its save data could not be put back. It is kept in %s."), install_save_pending.dir);
        error_display_res(NULL, NULL, res, "%s", text);
    }
}
