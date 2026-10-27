#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <mbedtls/sha256.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

// Builds a CIA from an installed CTR title.
//
// fs:USER only hands out a title's RomFS and banner; the raw contents (NCCH as
// installed: SD layer off, title key layer already removed at install) are only
// open to FSPXI, the service the fs module talks to Process9 with. Luma3DS lets
// a process share fs's session to it (svcControlService). The CIA then carries:
// - the certificate chain, read from nand:/dbs/certs.db;
// - a ticket made here (all contents, empty title key, no signature);
// - a TMD made here: the title's contents with their real hashes, marked as not
//   encrypted, no signature;
// - the contents, and the title's SMDH as meta when it has one.
// Installing it needs signature patches (Luma3DS has them).

#define CIA_HEADER_SIZE 0x2020
#define CIA_CERT_SIZE 0xA00
#define CIA_TICKET_SIZE 0x350
#define CIA_META_SIZE 0x3AC0
#define CIA_ALIGN(x) (((x) + 0x3F) & ~((u64) 0x3F))

#define TMD_HEADER_SIZE 0xB04
#define TMD_CHUNK_SIZE 0x30

#define ARCHIVE_TITLE_ACCESS ((FS_ArchiveID) 0x2345678A)

typedef struct {
    title_info* title;

    char path[FILE_PATH_MAX];

    Handle pxiHandle;
    FSPXI_Archive pxiArchive;
    bool pxiArchiveOpen;
    FSPXI_File pxiFile;

    Handle ciaHandle;

    u32 contentCount;
    AM_ContentInfo* contents;
    u8 (*hashes)[0x20];
    u64 contentsOffset;
    u64 contentOffset;
    u64 tmdOffset;
    u32 tmdSize;

    mbedtls_sha256_context sha;

    // The block being written and hashed while the next one is read.
    u8* jobBuffer;
    u32 jobSize;
    u64 jobOffset;
    Thread writer;
    Thread hasher;
    Result writeResult;

    data_op_data exportInfo;
} export_cia_data;

// Luma3DS: svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, &handle, name).
static __attribute__((naked)) Result action_export_cia_control_service(u32 op, Handle* out, const char* name) {
    __asm__ volatile("svc 0xB0\n\tbx lr");
}

static void action_export_cia_put16(u8* p, u16 v) {
    p[0] = (u8) (v >> 8);
    p[1] = (u8) v;
}

static void action_export_cia_put32(u8* p, u32 v) {
    p[0] = (u8) (v >> 24);
    p[1] = (u8) (v >> 16);
    p[2] = (u8) (v >> 8);
    p[3] = (u8) v;
}

static void action_export_cia_put64(u8* p, u64 v) {
    action_export_cia_put32(p, (u32) (v >> 32));
    action_export_cia_put32(p + 4, (u32) v);
}

static Result action_export_cia_write(export_cia_data* data, u64 offset, const void* buffer, u32 size) {
    u32 bytesWritten = 0;
    Result res = FSFILE_Write(data->ciaHandle, &bytesWritten, offset, buffer, size, 0);
    if(R_SUCCEEDED(res) && bytesWritten != size) {
        res = R_APP_IO_ERROR;
    }

    return res;
}

// ---- certificate chain ----

static u32 action_export_cia_sig_size(u32 type) {
    switch(type) {
        case 0x10000: case 0x10003: return 0x200;
        case 0x10001: case 0x10004: return 0x100;
        default: return 0;
    }
}

// Certificates sit whole in certs.db (a save-like container): look for each one
// by its signature type, issuer and name instead of parsing the container.
static bool action_export_cia_find_cert(const u8* db, u32 dbSize, const char* issuer, const char* name, u32 certSize, u8* out) {
    for(u32 off = 0; off + certSize <= dbSize; off += 4) {
        u32 type = (u32) db[off] << 24 | (u32) db[off + 1] << 16 | (u32) db[off + 2] << 8 | db[off + 3];
        u32 sigSize = action_export_cia_sig_size(type);
        if(sigSize == 0 || 4 + sigSize + 0x3C + 0x88 > certSize) {
            continue;
        }

        const char* body = (const char*) db + off + 4 + sigSize + 0x3C;
        if(strncmp(body, issuer, 0x40) == 0 && strncmp(body + 0x44, name, 0x40) == 0) {
            memcpy(out, db + off, certSize);
            return true;
        }
    }

    return false;
}

static Result action_export_cia_read_certs(u8* out) {
    Result res = 0;

    FS_Archive nandArchive = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenArchive(&nandArchive, ARCHIVE_NAND_CTR_FS, fsMakePath(PATH_EMPTY, "")))) {
        Handle fileHandle = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenFile(&fileHandle, nandArchive, fsMakePath(PATH_ASCII, "/dbs/certs.db"), FS_OPEN_READ, 0))) {
            u64 size = 0;
            if(R_SUCCEEDED(res = FSFILE_GetSize(fileHandle, &size))) {
                u8* db = size > 0 && size <= 0x100000 ? (u8*) malloc((size_t) size) : NULL;
                if(db != NULL) {
                    u32 bytesRead = 0;
                    if(R_SUCCEEDED(res = FSFILE_Read(fileHandle, &bytesRead, 0, db, (u32) size))) {
                        if(!action_export_cia_find_cert(db, bytesRead, "Root", "CA00000003", 0x400, out)
                           || !action_export_cia_find_cert(db, bytesRead, "Root-CA00000003", "XS0000000c", 0x300, out + 0x400)
                           || !action_export_cia_find_cert(db, bytesRead, "Root-CA00000003", "CP0000000b", 0x300, out + 0x700)) {
                            res = R_APP_BAD_DATA;
                        }
                    }

                    free(db);
                } else {
                    res = R_APP_OUT_OF_MEMORY;
                }
            }

            FSFILE_Close(fileHandle);
        }

        FSUSER_CloseArchive(nandArchive);
    }

    return res;
}

// ---- ticket, TMD, meta ----

static void action_export_cia_make_ticket(export_cia_data* data, u8* tik) {
    static const u8 contentIndex[0x2C] = {
        0x00, 0x01, 0x00, 0x14, 0x00, 0x00, 0x00, 0xAC, 0x00, 0x00, 0x00, 0x14, 0x00, 0x01, 0x00, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x84,
        0x00, 0x00, 0x00, 0x84, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    memset(tik, 0, CIA_TICKET_SIZE);
    action_export_cia_put32(tik, 0x10004);
    memset(tik + 4, 0xFF, 0x100);

    u8* body = tik + 0x140;
    strcpy((char*) body, "Root-CA00000003-XS0000000c");
    body[0x7C] = 1; // format version
    action_export_cia_put64(body + 0x9C, data->title->titleId);
    action_export_cia_put16(body + 0xA6, data->title->version);
    body[0xE1] = 1; // audit

    // Rights to every content.
    memcpy(body + 0x164, contentIndex, sizeof(contentIndex));
    memset(body + 0x164 + sizeof(contentIndex), 0xFF, 0x80);
}

static u32 action_export_cia_save_size(export_cia_data* data) {
    // The exheader's save data size (system info, 0x1C0); a DLC has none.
    u32 archivePath[4] = {(u32) data->title->titleId, (u32) (data->title->titleId >> 32), data->title->mediaType, 0};
    u32 filePath[5] = {0, 0, 3, 0, 0};

    FSPXI_Archive archive = 0;
    if(R_FAILED(FSPXI_OpenArchive(data->pxiHandle, &archive, ARCHIVE_TITLE_ACCESS, fs_make_path_binary(archivePath, sizeof(archivePath))))) {
        return 0;
    }

    u64 saveSize = 0;
    FSPXI_File file = 0;
    if(R_SUCCEEDED(FSPXI_OpenFile(data->pxiHandle, &file, archive, fs_make_path_binary(filePath, sizeof(filePath)), FS_OPEN_READ, 0))) {
        u32 bytesRead = 0;
        if(R_FAILED(FSPXI_ReadFile(data->pxiHandle, file, &bytesRead, 0x1C0, &saveSize, sizeof(saveSize))) || bytesRead != sizeof(saveSize)) {
            saveSize = 0;
        }

        FSPXI_CloseFile(data->pxiHandle, file);
    }

    FSPXI_CloseArchive(data->pxiHandle, archive);
    return saveSize <= 0xFFFFFFFF ? (u32) saveSize : 0;
}

static Result action_export_cia_write_tmd(export_cia_data* data) {
    u8* tmd = (u8*) calloc(1, data->tmdSize);
    if(tmd == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    action_export_cia_put32(tmd, 0x10004);
    memset(tmd + 4, 0xFF, 0x100);

    u8* header = tmd + 0x140;
    strcpy((char*) header, "Root-CA00000003-CP0000000b");
    header[0x40] = 1; // format version
    action_export_cia_put64(header + 0x4C, data->title->titleId);
    action_export_cia_put32(header + 0x54, 0x40);

    u32 saveSize = action_export_cia_save_size(data);
    memcpy(header + 0x5A, &saveSize, sizeof(saveSize)); // little endian

    action_export_cia_put16(header + 0x9C, data->title->version);
    action_export_cia_put16(header + 0x9E, (u16) data->contentCount);

    u8* infoRecords = tmd + 0x204;
    u8* chunks = tmd + TMD_HEADER_SIZE;
    for(u32 i = 0; i < data->contentCount; i++) {
        AM_ContentInfo* content = &data->contents[i];
        u8* chunk = chunks + i * TMD_CHUNK_SIZE;

        action_export_cia_put32(chunk, content->contentId);
        action_export_cia_put16(chunk + 4, content->index);
        // The contents leave the console without the title key layer.
        action_export_cia_put16(chunk + 6, content->type & ~AM_CONTENT_ENCRYPTED);
        action_export_cia_put64(chunk + 8, content->size);
        memcpy(chunk + 0x10, data->hashes[i], 0x20);
    }

    action_export_cia_put16(infoRecords + 2, (u16) data->contentCount);
    mbedtls_sha256_ret(chunks, data->contentCount * TMD_CHUNK_SIZE, infoRecords + 4, 0);
    mbedtls_sha256_ret(infoRecords, 0x900, header + 0xA4, 0);

    Result res = action_export_cia_write(data, data->tmdOffset, tmd, data->tmdSize);
    free(tmd);
    return res;
}

static Result action_export_cia_write_meta(export_cia_data* data, u64 offset) {
    static const u32 filePath[5] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000};
    u32 archivePath[4] = {(u32) data->title->titleId, (u32) (data->title->titleId >> 32), data->title->mediaType, 0};

    u8* meta = (u8*) calloc(1, CIA_META_SIZE);
    if(meta == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Handle fileHandle = 0;
    if(R_SUCCEEDED(FSUSER_OpenFileDirectly(&fileHandle, ARCHIVE_SAVEDATA_AND_CONTENT, fs_make_path_binary(archivePath, sizeof(archivePath)),
                                           fs_make_path_binary(filePath, sizeof(filePath)), FS_OPEN_READ, 0))) {
        u32 bytesRead = 0;
        FSFILE_Read(fileHandle, &bytesRead, 0, meta + 0x400, sizeof(SMDH));
        FSFILE_Close(fileHandle);
    }

    Result res = action_export_cia_write(data, offset, meta, CIA_META_SIZE);
    free(meta);
    return res;
}

// ---- preparing the file ----

static Result action_export_cia_list_contents(export_cia_data* data) {
    Result res = 0;

    u32 count = 0;
    if(R_SUCCEEDED(res = AM_GetNumExistingContentInfos(&count, data->title->titleId, data->title->mediaType))) {
        if(count == 0) {
            return R_APP_BAD_DATA;
        }

        data->contents = (AM_ContentInfo*) calloc(count, sizeof(AM_ContentInfo));
        data->hashes = calloc(count, 0x20);
        if(data->contents == NULL || data->hashes == NULL) {
            return R_APP_OUT_OF_MEMORY;
        }

        if(R_SUCCEEDED(res = AM_ListExistingContentInfos(&data->contentCount, data->contents, 0, count, data->title->titleId, data->title->mediaType))
           && data->contentCount == 0) {
            res = R_APP_BAD_DATA;
        }
    }

    return res;
}

static void action_export_cia_make_path(export_cia_data* data) {
    char base[0x80] = {'\0'};
    string_copy(base, data->title->hasMeta && !string_is_empty(data->title->meta.shortDescription) ? data->title->meta.shortDescription : data->title->productCode, sizeof(base));
    for(char* c = base; *c != '\0'; c++) {
        if(*c == '\n') {
            *c = ' ';
        }
    }

    char name[0x80] = {'\0'};
    string_escape_file_name(name, base, sizeof(name));

    snprintf(data->path, sizeof(data->path), APP_DATA_DIR "cias/%s [%016llX] v%u.cia", name, data->title->titleId, data->title->version);
}

static Result action_export_cia_prepare(export_cia_data* data) {
    Result res = 0;

    // Only Luma3DS has the call (an unknown SVC would kill the app).
    s64 luma = 0;
    if(R_FAILED(svcGetSystemInfo(&luma, 0x10000, 0)) || ((luma >> 8) & 0xFFFFFF) == 0) {
        return R_APP_NOT_IMPLEMENTED;
    }

    if(R_FAILED(res = action_export_cia_control_service(0, &data->pxiHandle, "PxiFS0"))) {
        data->pxiHandle = 0;
        return res;
    }

    if(R_FAILED(res = action_export_cia_list_contents(data))) {
        return res;
    }

    u32 archivePath[4] = {(u32) data->title->titleId, (u32) (data->title->titleId >> 32), data->title->mediaType, 0};
    if(R_FAILED(res = FSPXI_OpenArchive(data->pxiHandle, &data->pxiArchive, ARCHIVE_TITLE_ACCESS, fs_make_path_binary(archivePath, sizeof(archivePath))))) {
        return res;
    }

    data->pxiArchiveOpen = true;

    u8* head = (u8*) calloc(1, CIA_HEADER_SIZE);
    u8* certs = (u8*) calloc(1, CIA_CERT_SIZE);
    u8* ticket = (u8*) calloc(1, CIA_TICKET_SIZE);
    if(head == NULL || certs == NULL || ticket == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    } else if(R_SUCCEEDED(res = action_export_cia_read_certs(certs))) {
        u64 contentSize = 0;
        for(u32 i = 0; i < data->contentCount; i++) {
            AM_ContentInfo* content = &data->contents[i];
            contentSize += content->size;
            head[0x20 + content->index / 8] |= (u8) (0x80 >> (content->index % 8));
        }

        bool hasMeta = data->title->hasMeta;

        data->tmdSize = TMD_HEADER_SIZE + data->contentCount * TMD_CHUNK_SIZE;
        u64 certOffset = CIA_ALIGN(CIA_HEADER_SIZE);
        u64 ticketOffset = CIA_ALIGN(certOffset + CIA_CERT_SIZE);
        data->tmdOffset = CIA_ALIGN(ticketOffset + CIA_TICKET_SIZE);
        data->contentsOffset = CIA_ALIGN(data->tmdOffset + data->tmdSize);
        u64 metaOffset = CIA_ALIGN(data->contentsOffset + contentSize);
        u64 totalSize = hasMeta ? metaOffset + CIA_META_SIZE : data->contentsOffset + contentSize;

        u32 headerSize = CIA_HEADER_SIZE, certSize = CIA_CERT_SIZE, ticketSize = CIA_TICKET_SIZE, metaSize = hasMeta ? CIA_META_SIZE : 0;
        memcpy(head + 0x00, &headerSize, 4);
        memcpy(head + 0x08, &certSize, 4);
        memcpy(head + 0x0C, &ticketSize, 4);
        memcpy(head + 0x10, &data->tmdSize, 4);
        memcpy(head + 0x14, &metaSize, 4);
        memcpy(head + 0x18, &contentSize, 8);

        action_export_cia_make_ticket(data, ticket);
        action_export_cia_make_path(data);

        FS_Archive sdmcArchive = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
            if(R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, APP_DATA_DIR)) && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, APP_DATA_DIR "cias/"))) {
                FS_Path* fsPath = fs_make_path_utf8(data->path);
                if(fsPath != NULL) {
                    FSUSER_DeleteFile(sdmcArchive, *fsPath);
                    if(R_SUCCEEDED(res = FSUSER_OpenFile(&data->ciaHandle, sdmcArchive, *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
                        // The whole size first: a full SD card fails here, not at the end.
                        if(R_FAILED(res = FSFILE_SetSize(data->ciaHandle, totalSize))
                           || R_FAILED(res = action_export_cia_write(data, 0, head, CIA_HEADER_SIZE))
                           || R_FAILED(res = action_export_cia_write(data, certOffset, certs, CIA_CERT_SIZE))
                           || R_FAILED(res = action_export_cia_write(data, ticketOffset, ticket, CIA_TICKET_SIZE))
                           || (hasMeta && R_FAILED(res = action_export_cia_write_meta(data, metaOffset)))) {
                            FSFILE_Close(data->ciaHandle);
                            data->ciaHandle = 0;
                            FSUSER_DeleteFile(sdmcArchive, *fsPath);
                        }
                    }

                    fs_free_path_utf8(fsPath);
                } else {
                    res = R_APP_OUT_OF_MEMORY;
                }
            }

            FSUSER_CloseArchive(sdmcArchive);
        }
    }

    free(head);
    free(certs);
    free(ticket);
    return res;
}

static void action_export_cia_free(export_cia_data* data) {
    if(data->ciaHandle != 0) {
        FSFILE_Close(data->ciaHandle);
    }

    if(data->pxiArchiveOpen) {
        FSPXI_CloseArchive(data->pxiHandle, data->pxiArchive);
    }

    if(data->pxiHandle != 0) {
        svcCloseHandle(data->pxiHandle);
    }

    free(data->contents);
    free(data->hashes);
    free(data->jobBuffer);
    free(data);
}

static void action_export_cia_delete_file(export_cia_data* data) {
    FS_Path* fsPath = fs_make_path_utf8(data->path);
    if(fsPath != NULL) {
        FS_Archive sdmcArchive = 0;
        if(R_SUCCEEDED(FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
            FSUSER_DeleteFile(sdmcArchive, *fsPath);
            FSUSER_CloseArchive(sdmcArchive);
        }

        fs_free_path_utf8(fsPath);
    }
}

// ---- copying the contents ----

static void action_export_cia_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    task_draw_title_info(view, ((export_cia_data*) data)->title, x1, y1, x2, y2);
}

static Result action_export_cia_is_src_directory(void* data, u32 index, bool* isDirectory) {
    *isDirectory = false;
    return 0;
}

static Result action_export_cia_make_dst_directory(void* data, u32 index) {
    return 0;
}

static Result action_export_cia_open_src(void* data, u32 index, u32* handle) {
    export_cia_data* exportData = (export_cia_data*) data;

    // A retry starts over from the first content.
    if(index == 0) {
        exportData->contentOffset = 0;
    }

    // Raw content access by TMD index. Handles are the index plus one: the data
    // operation takes 0 as no handle and would skip closing it.
    u32 filePath[5] = {2, exportData->contents[index].index, 0, 0, 0};
    *handle = index + 1;
    return FSPXI_OpenFile(exportData->pxiHandle, &exportData->pxiFile, exportData->pxiArchive, fs_make_path_binary(filePath, sizeof(filePath)), FS_OPEN_READ, 0);
}

static Result action_export_cia_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    export_cia_data* exportData = (export_cia_data*) data;
    return FSPXI_CloseFile(exportData->pxiHandle, exportData->pxiFile);
}

static Result action_export_cia_get_src_size(void* data, u32 handle, u64* size) {
    export_cia_data* exportData = (export_cia_data*) data;

    Result res = FSPXI_GetFileSize(exportData->pxiHandle, exportData->pxiFile, size);
    if(R_SUCCEEDED(res) && *size != exportData->contents[handle - 1].size) {
        res = R_APP_BAD_DATA;
    }

    return res;
}

static Result action_export_cia_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    export_cia_data* exportData = (export_cia_data*) data;
    return FSPXI_ReadFile(exportData->pxiHandle, exportData->pxiFile, bytesRead, offset, buffer, size);
}

// The copy runs on the system core, which only gets 30% of its time: reading,
// hashing and writing one after another there crawled. Each block read is
// handed over to two threads, one writing it (I/O, system core) and one hashing
// it (core 2 on a New 3DS, else the app core), while the next block is read.
static void action_export_cia_write_thread(void* arg) {
    export_cia_data* exportData = (export_cia_data*) arg;

    u32 bytesWritten = 0;
    exportData->writeResult = FSFILE_Write(exportData->ciaHandle, &bytesWritten, exportData->jobOffset, exportData->jobBuffer, exportData->jobSize, 0);
    if(R_SUCCEEDED(exportData->writeResult) && bytesWritten != exportData->jobSize) {
        exportData->writeResult = R_APP_IO_ERROR;
    }
}

static void action_export_cia_hash_thread(void* arg) {
    export_cia_data* exportData = (export_cia_data*) arg;
    mbedtls_sha256_update_ret(&exportData->sha, exportData->jobBuffer, exportData->jobSize);
}

// Waits for the block in flight; its write result.
static Result action_export_cia_wait_job(export_cia_data* exportData) {
    if(exportData->writer != NULL) {
        threadJoin(exportData->writer, U64_MAX);
        threadFree(exportData->writer);
        exportData->writer = NULL;
    }

    if(exportData->hasher != NULL) {
        threadJoin(exportData->hasher, U64_MAX);
        threadFree(exportData->hasher);
        exportData->hasher = NULL;
    }

    Result res = exportData->writeResult;
    exportData->writeResult = 0;
    return res;
}

static Result action_export_cia_open_dst(void* data, u32 index, void* initialReadBlock, u64 size, u32* handle) {
    export_cia_data* exportData = (export_cia_data*) data;

    if(exportData->jobBuffer == NULL && (exportData->jobBuffer = (u8*) malloc(exportData->exportInfo.bufferSize)) == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    mbedtls_sha256_init(&exportData->sha);
    mbedtls_sha256_starts_ret(&exportData->sha, 0);

    *handle = index + 1;
    return 0;
}

static Result action_export_cia_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    export_cia_data* exportData = (export_cia_data*) data;

    Result res = action_export_cia_wait_job(exportData);

    mbedtls_sha256_finish_ret(&exportData->sha, exportData->hashes[index]);
    mbedtls_sha256_free(&exportData->sha);

    if(!succeeded || R_FAILED(res)) {
        return res;
    }

    exportData->contentOffset += exportData->contents[index].size;

    // The TMD goes in once every content's hash is known.
    return index == exportData->contentCount - 1 ? action_export_cia_write_tmd(exportData) : 0;
}

static Result action_export_cia_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    export_cia_data* exportData = (export_cia_data*) data;

    Result res = action_export_cia_wait_job(exportData);
    if(R_FAILED(res)) {
        return res;
    }

    memcpy(exportData->jobBuffer, buffer, size);
    exportData->jobSize = size;
    exportData->jobOffset = exportData->contentsOffset + exportData->contentOffset + offset;

    exportData->writer = threadCreate(action_export_cia_write_thread, exportData, 0x4000, 0x18, 1, false);
    exportData->hasher = threadCreate(action_export_cia_hash_thread, exportData, 0x4000, 0x31, 2, false);
    if(exportData->hasher == NULL) {
        exportData->hasher = threadCreate(action_export_cia_hash_thread, exportData, 0x4000, 0x31, 0, false);
    }

    if(exportData->writer == NULL || exportData->hasher == NULL) {
        action_export_cia_wait_job(exportData);
        return R_APP_THREAD_CREATE_FAILED;
    }

    *bytesWritten = size;
    return 0;
}

static Result action_export_cia_suspend(void* data, u32 index) {
    return 0;
}

static Result action_export_cia_restore(void* data, u32 index) {
    return 0;
}

static bool action_export_cia_error(void* data, u32 index, Result res, ui_view** errorView) {
    *errorView = error_display_res(((export_cia_data*) data)->title, task_draw_title_info, res, "Failed to export CIA.");
    // Never skip a content: the file would be broken.
    return false;
}

static void action_export_cia_update(ui_view* view, void* data, float* progress, char* text) {
    export_cia_data* exportData = (export_cia_data*) data;

    if(exportData->exportInfo.finished) {
        ui_pop();
        info_destroy(view);

        FSFILE_Close(exportData->ciaHandle);
        exportData->ciaHandle = 0;

        if(R_SUCCEEDED(exportData->exportInfo.result)) {
            prompt_display_notify("Success", "CIA exported to /nsa/cias/.", COLOR_TEXT, exportData->title, task_draw_title_info, NULL);
        } else {
            action_export_cia_delete_file(exportData);
        }

        action_export_cia_free(exportData);

        return;
    }

    if(hidKeysDown() & KEY_B) {
        svcSignalEvent(exportData->exportInfo.cancelEvent);
    }

    *progress = exportData->exportInfo.currTotal != 0 ? (float) ((double) exportData->exportInfo.currProcessed / (double) exportData->exportInfo.currTotal) : 0;
    snprintf(text, PROGRESS_TEXT_MAX, _("%lu / %lu\n%.2f %s / %.2f %s\n%.2f %s/s, ETA %s"),
             exportData->exportInfo.processed + 1 > exportData->exportInfo.total ? exportData->exportInfo.total : exportData->exportInfo.processed + 1,
             exportData->exportInfo.total,
             ui_get_display_size(exportData->exportInfo.currProcessed),
             ui_get_display_size_units(exportData->exportInfo.currProcessed),
             ui_get_display_size(exportData->exportInfo.currTotal),
             ui_get_display_size_units(exportData->exportInfo.currTotal),
             ui_get_display_size(exportData->exportInfo.bytesPerSecond),
             ui_get_display_size_units(exportData->exportInfo.bytesPerSecond),
             ui_get_display_eta(exportData->exportInfo.estimatedRemainingSeconds));
}

static void action_export_cia_onresponse(ui_view* view, void* data, u32 response) {
    export_cia_data* exportData = (export_cia_data*) data;

    if(response != PROMPT_YES) {
        action_export_cia_free(exportData);
        return;
    }

    Result res = action_export_cia_prepare(exportData);
    if(R_FAILED(res)) {
        if(res == R_APP_NOT_IMPLEMENTED) {
            error_display(exportData->title, task_draw_title_info, "Exporting a CIA needs Luma3DS.");
        } else {
            error_display_res(exportData->title, task_draw_title_info, res, "Failed to prepare the CIA.");
        }

        action_export_cia_free(exportData);
        return;
    }

    exportData->exportInfo.total = exportData->contentCount;
    if(R_SUCCEEDED(res = task_data_op(&exportData->exportInfo))) {
        info_display("Exporting CIA", "Press B to cancel.", true, data, action_export_cia_update, action_export_cia_draw_top);
    } else {
        error_display_res(exportData->title, task_draw_title_info, res, "Failed to initiate CIA export.");

        FSFILE_Close(exportData->ciaHandle);
        exportData->ciaHandle = 0;
        action_export_cia_delete_file(exportData);
        action_export_cia_free(exportData);
    }
}

void action_export_cia(linked_list* items, list_item* selected) {
    export_cia_data* data = (export_cia_data*) calloc(1, sizeof(export_cia_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate export CIA data.");

        return;
    }

    data->title = (title_info*) selected->data;

    data->exportInfo.data = data;

    data->exportInfo.op = DATAOP_COPY;

    data->exportInfo.bufferSize = 1024 * 1024;
    data->exportInfo.copyEmpty = false;

    data->exportInfo.isSrcDirectory = action_export_cia_is_src_directory;
    data->exportInfo.makeDstDirectory = action_export_cia_make_dst_directory;

    data->exportInfo.openSrc = action_export_cia_open_src;
    data->exportInfo.closeSrc = action_export_cia_close_src;
    data->exportInfo.getSrcSize = action_export_cia_get_src_size;
    data->exportInfo.readSrc = action_export_cia_read_src;

    data->exportInfo.openDst = action_export_cia_open_dst;
    data->exportInfo.closeDst = action_export_cia_close_dst;
    data->exportInfo.writeDst = action_export_cia_write_dst;

    data->exportInfo.suspend = action_export_cia_suspend;
    data->exportInfo.restore = action_export_cia_restore;

    data->exportInfo.error = action_export_cia_error;

    data->exportInfo.finished = true;

    prompt_display_yes_no("Confirmation", "Export the selected title as a CIA?", COLOR_TEXT, data, action_export_cia_draw_top, action_export_cia_onresponse);
}
