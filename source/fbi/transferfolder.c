#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "transferfolder.h"
#include "foldermanifest.h"
#include "../core/core.h"

// How deep the sender goes into folders; beyond this the list is refused.
#define FOLDER_DEPTH_MAX 24
#define FOLDER_DIR_BATCH 64
// How long a file's bytes wait for the manifest, which is the first chunk of the payload.
#define FOLDER_MANIFEST_WAIT_NS (60LL * 1000 * 1000 * 1000)

// ---- sending ----

struct transfer_folder_send {
    folder_layout layout;
    char root[FILE_PATH_MAX];   // Without a trailing slash.
    u8* manifest;
    u32 manifestSize;
};

static Result folder_scan_dir(transfer_folder_send* folder, FS_Archive archive, const char* dir, const char* rel, u32 depth) {
    if(depth > FOLDER_DEPTH_MAX) {
        return R_APP_OUT_OF_RANGE;
    }

    FS_Path* dirPath = fs_make_path_utf8(dir);
    if(dirPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Handle handle = 0;
    Result res = FSUSER_OpenDirectory(&handle, archive, *dirPath);
    fs_free_path_utf8(dirPath);
    if(R_FAILED(res)) {
        return res;
    }

    FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(FOLDER_DIR_BATCH, sizeof(FS_DirectoryEntry));
    if(entries == NULL) {
        FSDIR_Close(handle);
        return R_APP_OUT_OF_MEMORY;
    }

    // The folders of this one are walked after it is read whole, so only one handle is open per level.
    char (*subDirs)[FILE_NAME_MAX] = NULL;
    u32 subCount = 0;
    u32 subCap = 0;

    for(;;) {
        u32 count = 0;
        res = FSDIR_Read(handle, &count, FOLDER_DIR_BATCH, entries);
        if(R_FAILED(res) || count == 0) {
            break;
        }

        for(u32 i = 0; i < count && R_SUCCEEDED(res); i++) {
            char name[FILE_NAME_MAX] = {'\0'};
            utf16_to_utf8((uint8_t*) name, entries[i].name, sizeof(name) - 1);

            char entryRel[FILE_PATH_MAX];
            if((size_t) snprintf(entryRel, sizeof(entryRel), "%s%s", rel, name) >= sizeof(entryRel)) {
                res = R_APP_OUT_OF_RANGE;
                break;
            }

            bool isDir = (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
            if(!folder_layout_add(&folder->layout, entryRel, isDir, entries[i].fileSize)) {
                res = R_APP_OUT_OF_RANGE;
                break;
            }

            if(isDir) {
                if(subCount == subCap) {
                    u32 cap = subCap == 0 ? 8 : subCap * 2;
                    void* grown = realloc(subDirs, (size_t) cap * sizeof(*subDirs));
                    if(grown == NULL) {
                        res = R_APP_OUT_OF_MEMORY;
                        break;
                    }

                    subDirs = (char (*)[FILE_NAME_MAX]) grown;
                    subCap = cap;
                }

                string_copy(subDirs[subCount++], name, FILE_NAME_MAX);
            }
        }

        if(R_FAILED(res)) {
            break;
        }
    }

    FSDIR_Close(handle);
    free(entries);

    for(u32 i = 0; R_SUCCEEDED(res) && i < subCount; i++) {
        char subDir[FILE_PATH_MAX];
        char subRel[FILE_PATH_MAX];
        if((size_t) snprintf(subDir, sizeof(subDir), "%s/%s", dir, subDirs[i]) >= sizeof(subDir)
           || (size_t) snprintf(subRel, sizeof(subRel), "%s%s/", rel, subDirs[i]) >= sizeof(subRel)) {
            res = R_APP_OUT_OF_RANGE;
        } else {
            res = folder_scan_dir(folder, archive, subDir, subRel, depth + 1);
        }
    }

    free(subDirs);
    return res;
}

Result transfer_folder_send_scan(transfer_folder_send** out, FS_Archive archive, const char* path) {
    transfer_folder_send* folder = (transfer_folder_send*) calloc(1, sizeof(transfer_folder_send));
    if(folder == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    string_copy(folder->root, path, sizeof(folder->root));
    size_t len = strlen(folder->root);
    while(len > 1 && folder->root[len - 1] == '/') {
        folder->root[--len] = '\0';
    }

    Result res = folder_scan_dir(folder, archive, len == 1 ? "/" : folder->root, "", 0);
    if(R_SUCCEEDED(res) && !folder_layout_finish(&folder->layout)) {
        res = R_APP_OUT_OF_RANGE;
    }

    if(R_SUCCEEDED(res)) {
        folder->manifest = folder_manifest_build(&folder->layout, &folder->manifestSize);
        if(folder->manifest == NULL) {
            res = R_APP_OUT_OF_MEMORY;
        }
    }

    if(R_FAILED(res)) {
        transfer_folder_send_free(folder);
        return res;
    }

    *out = folder;
    return 0;
}

void transfer_folder_send_free(transfer_folder_send* folder) {
    if(folder != NULL) {
        folder_layout_free(&folder->layout);
        free(folder->manifest);
        free(folder);
    }
}

u64 transfer_folder_send_total(const transfer_folder_send* folder) {
    return folder->layout.total;
}

u32 transfer_folder_send_files(const transfer_folder_send* folder) {
    return folder->layout.fileCount;
}

const u8* transfer_folder_send_manifest(const transfer_folder_send* folder, u32* size) {
    *size = folder->manifestSize;
    return folder->manifest;
}

u64* transfer_folder_send_segments(const transfer_folder_send* folder, u32* count) {
    *count = 1 + folder->layout.fileCount;

    u64* segments = (u64*) calloc(*count, sizeof(u64));
    if(segments == NULL) {
        return NULL;
    }

    segments[0] = folder->manifestSize;
    for(u32 i = 0; i < folder->layout.fileCount; i++) {
        segments[1 + i] = folder->layout.entries[folder->layout.files[i]].size;
    }

    return segments;
}

Result transfer_folder_send_read(const transfer_folder_send* folder, FS_Archive archive, const transfer_chunk* chunk, void* buffer) {
    if(chunk->segment == 0) {
        if(chunk->segmentOffset + chunk->len > folder->manifestSize) {
            return R_APP_OUT_OF_RANGE;
        }

        memcpy(buffer, folder->manifest + chunk->segmentOffset, chunk->len);
        return 0;
    }

    if(chunk->segment > folder->layout.fileCount) {
        return R_APP_OUT_OF_RANGE;
    }

    const folder_entry* entry = &folder->layout.entries[folder->layout.files[chunk->segment - 1]];

    char path[FILE_PATH_MAX];
    if((size_t) snprintf(path, sizeof(path), "%s/%s", folder->root, entry->path) >= sizeof(path)) {
        return R_APP_OUT_OF_RANGE;
    }

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Handle file = 0;
    Result res = FSUSER_OpenFile(&file, archive, *fsPath, FS_OPEN_READ, 0);
    fs_free_path_utf8(fsPath);
    if(R_SUCCEEDED(res)) {
        u32 bytesRead = 0;
        res = FSFILE_Read(file, &bytesRead, chunk->segmentOffset, buffer, chunk->len);
        if(R_SUCCEEDED(res) && bytesRead != chunk->len) {
            res = R_APP_IO_ERROR;   // The file changed since the list was made.
        }

        FSFILE_Close(file);
    }

    return res;
}

// ---- receiving ----

struct transfer_folder_recv {
    FS_Archive sd;
    char root[FILE_PATH_MAX];       // Ends with a slash.
    u32 head;

    u8* manifest;
    bool* seen;                     // Per chunk of the manifest.
    u32 seenCount;
    u32 seenTotal;

    folder_layout layout;
    LightEvent ready;               // The manifest has been read (or was refused).
    volatile bool isReady;
    volatile bool failed;
    LightLock lock;
};

Result transfer_folder_recv_open(transfer_folder_recv** out, FS_Archive sd, const char* root, u32 head) {
    if(head < 8 || head > FOLDER_MANIFEST_MAX_SIZE) {
        return R_APP_BAD_DATA;
    }

    transfer_folder_recv* folder = (transfer_folder_recv*) calloc(1, sizeof(transfer_folder_recv));
    if(folder == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    folder->sd = sd;
    folder->head = head;
    if((size_t) snprintf(folder->root, sizeof(folder->root), "%s", root) >= sizeof(folder->root) - 1) {
        free(folder);
        return R_APP_OUT_OF_RANGE;
    }

    size_t len = strlen(folder->root);
    if(len == 0 || folder->root[len - 1] != '/') {
        folder->root[len++] = '/';
        folder->root[len] = '\0';
    }

    folder->seenTotal = (head + TRANSFER_CHUNK - 1) / TRANSFER_CHUNK;
    folder->manifest = (u8*) malloc(head);
    folder->seen = (bool*) calloc(folder->seenTotal, sizeof(bool));
    if(folder->manifest == NULL || folder->seen == NULL) {
        transfer_folder_recv_free(folder);
        return R_APP_OUT_OF_MEMORY;
    }

    LightEvent_Init(&folder->ready, RESET_STICKY);
    LightLock_Init(&folder->lock);

    Result res = fs_ensure_dir(sd, folder->root);
    if(R_FAILED(res)) {
        transfer_folder_recv_free(folder);
        return res;
    }

    *out = folder;
    return 0;
}

void transfer_folder_recv_free(transfer_folder_recv* folder) {
    if(folder != NULL) {
        folder_layout_free(&folder->layout);
        free(folder->manifest);
        free(folder->seen);
        free(folder);
    }
}

u32 transfer_folder_recv_files(transfer_folder_recv* folder) {
    return folder->isReady ? folder->layout.fileCount : 0;
}

// <root><rel> in `out`.
static bool folder_recv_path(const transfer_folder_recv* folder, char* out, size_t size, const char* rel) {
    return (size_t) snprintf(out, size, "%s%s", folder->root, rel) < size;
}

// Makes the folders above a file: "a/b/c" needs "a" and "a/b".
static Result folder_recv_make_parents(transfer_folder_recv* folder, const char* rel) {
    char path[FILE_PATH_MAX];
    if(!folder_recv_path(folder, path, sizeof(path), rel)) {
        return R_APP_OUT_OF_RANGE;
    }

    size_t rootLen = strlen(folder->root);
    for(char* slash = strchr(path + rootLen, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        Result res = fs_ensure_dir(folder->sd, path);
        *slash = '/';

        if(R_FAILED(res)) {
            return res;
        }
    }

    return 0;
}

// The manifest is all here: reads it and makes the folders.
static Result folder_recv_manifest_ready(transfer_folder_recv* folder) {
    if(!folder_manifest_parse(folder->manifest, folder->head, &folder->layout)) {
        return R_APP_BAD_DATA;
    }

    for(u32 i = 0; i < folder->layout.count; i++) {
        const folder_entry* entry = &folder->layout.entries[i];
        if(!entry->isDir) {
            continue;
        }

        char path[FILE_PATH_MAX];
        if(!folder_recv_path(folder, path, sizeof(path), entry->path)) {
            return R_APP_OUT_OF_RANGE;
        }

        Result res = folder_recv_make_parents(folder, entry->path);
        if(R_SUCCEEDED(res)) {
            res = fs_ensure_dir(folder->sd, path);
        }

        if(R_FAILED(res)) {
            return res;
        }
    }

    return 0;
}

static Result folder_recv_open_file(transfer_folder_recv* folder, const folder_entry* entry, Handle* file) {
    char path[FILE_PATH_MAX];
    if(!folder_recv_path(folder, path, sizeof(path), entry->path)) {
        return R_APP_OUT_OF_RANGE;
    }

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = FSUSER_OpenFile(file, folder->sd, *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if(R_FAILED(res) && R_SUCCEEDED(folder_recv_make_parents(folder, entry->path))) {
        res = FSUSER_OpenFile(file, folder->sd, *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    }

    fs_free_path_utf8(fsPath);
    return res;
}

Result transfer_folder_recv_write(transfer_folder_recv* folder, u64 offset, const void* buffer, u32 size) {
    if(offset < folder->head) {
        // The manifest. A chunk lies inside one segment, so it ends inside it too.
        if(offset + size > folder->head || offset % TRANSFER_CHUNK != 0) {
            return R_APP_BAD_DATA;
        }

        Result res = 0;
        LightLock_Lock(&folder->lock);

        memcpy(folder->manifest + offset, buffer, size);

        u32 index = (u32) (offset / TRANSFER_CHUNK);
        if(!folder->seen[index]) {
            folder->seen[index] = true;
            folder->seenCount++;
        }

        if(folder->seenCount == folder->seenTotal && !folder->isReady && !folder->failed) {
            res = folder_recv_manifest_ready(folder);
            if(R_SUCCEEDED(res)) {
                folder->isReady = true;
            } else {
                folder->failed = true;
            }

            LightEvent_Signal(&folder->ready);
        }

        LightLock_Unlock(&folder->lock);
        return res;
    }

    if(!folder->isReady && !folder->failed && LightEvent_WaitTimeout(&folder->ready, FOLDER_MANIFEST_WAIT_NS) != 0) {
        return R_APP_IO_ERROR;
    }

    if(!folder->isReady) {
        return R_APP_BAD_DATA;
    }

    const folder_entry* entry = folder_layout_find(&folder->layout, offset);
    if(entry == NULL || offset + size > entry->start + entry->size) {
        return R_APP_BAD_DATA;
    }

    Result res = 0;
    LightLock_Lock(&folder->lock);

    Handle file = 0;
    if(R_SUCCEEDED(res = folder_recv_open_file(folder, entry, &file))) {
        // The size at once: a full card fails here, and chunks that arrive out of order have a place to go.
        u64 have = 0;
        if(R_SUCCEEDED(res = FSFILE_GetSize(file, &have)) && have != entry->size) {
            res = FSFILE_SetSize(file, entry->size);
        }

        u32 written = 0;
        if(R_SUCCEEDED(res)) {
            res = FSFILE_Write(file, &written, offset - entry->start, buffer, size, 0);
            if(R_SUCCEEDED(res) && written != size) {
                res = R_APP_IO_ERROR;
            }
        }

        Result closeRes = FSFILE_Close(file);
        if(R_SUCCEEDED(res)) {
            res = closeRes;
        }
    }

    LightLock_Unlock(&folder->lock);
    return res;
}

Result transfer_folder_recv_finish(transfer_folder_recv* folder) {
    if(!folder->isReady) {
        return R_APP_BAD_DATA;
    }

    // A file with no bytes never had a chunk.
    for(u32 i = 0; i < folder->layout.fileCount; i++) {
        const folder_entry* entry = &folder->layout.entries[folder->layout.files[i]];
        if(entry->size != 0) {
            continue;
        }

        Handle file = 0;
        Result res = folder_recv_open_file(folder, entry, &file);
        if(R_FAILED(res)) {
            return res;
        }

        FSFILE_Close(file);
    }

    return 0;
}
