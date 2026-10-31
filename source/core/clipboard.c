#include <malloc.h>
#include <string.h>

#include <3ds.h>

#include "clipboard.h"
#include "fs.h"
#include "stringutil.h"

static bool clipboard_has = false;
static bool clipboard_contents_only;
static bool clipboard_cut;
static FS_ArchiveID clipboard_archive_id;

static FS_Archive clipboard_archive;
static char clipboard_path[FILE_PATH_MAX];

bool clipboard_has_contents() {
    return clipboard_has;
}

FS_Archive clipboard_get_archive() {
    return clipboard_archive;
}

char* clipboard_get_path() {
    return clipboard_path;
}

bool clipboard_is_contents_only() {
    return clipboard_contents_only;
}

bool clipboard_is_cut() {
    return clipboard_cut;
}

FS_ArchiveID clipboard_get_archive_id() {
    return clipboard_archive_id;
}

Result clipboard_set_cut(FS_Archive archive, FS_ArchiveID archiveId, const char* path) {
    Result res = clipboard_set_contents(archive, path, false);
    if(R_SUCCEEDED(res)) {
        clipboard_cut = true;
        clipboard_archive_id = archiveId;
    }

    return res;
}

Result clipboard_set_contents(FS_Archive archive, const char* path, bool contentsOnly) {
    clipboard_clear();

    Result res = 0;
    if(R_SUCCEEDED(res = fs_ref_archive(archive))) {
        clipboard_has = true;
        clipboard_contents_only = contentsOnly;

        clipboard_archive = archive;
        string_copy(clipboard_path, path, FILE_PATH_MAX);
    }

    return res;
}

void clipboard_clear() {
    if(clipboard_archive != 0) {
        fs_close_archive(clipboard_archive);
    }

    clipboard_has = false;
    clipboard_contents_only = false;
    clipboard_cut = false;
    clipboard_archive_id = 0;

    clipboard_archive = 0;
    memset(clipboard_path, '\0', FILE_PATH_MAX);
}