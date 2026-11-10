#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <mbedtls/sha256.h>

#include "forwarder.h"
#include "launch3dsx.h"
#include "action/action.h"
#include "../core/core.h"

// The template is a whole CIA (forwarder/Makefile): header, certificates, ticket, TMD, one content (the NCCH) and the
// meta section with the template's SMDH. A copy changes in:
// - the .code: the .3dsx's path after the marker (the stub reads it, forwarder/source/main.c);
// - the ExeFS icon and the meta's SMDH: the .3dsx's names and icons;
// - the title ID: NCCH header (partition and program ID), exheader (jump ID, both ARM11 program IDs), ticket, TMD;
// then the hashes are made again: each ExeFS file, the ExeFS header, the exheader, the content in the TMD and the TMD's
// info records. The signatures stay the template's (invalid for the new contents): installing it needs signature patches,
// as NSA's exported CIAs do (Luma3DS has them).

#define FWD_TEMPLATE "romfs:/forwarder.cia"
#define FWD_MAGIC "NSAFWD01"
#define FWD_MAGIC_LEN 8
#define FWD_PATH_MAX 512
#define FWD_PRODUCT_CODE "CTR-P-NSAF"

// Unique IDs for forwarders: a hash of the path picks one in this range, away from NSA's own (0xFB1A0-0xFB1AF).
#define FWD_UID_FIRST 0xC8000
#define FWD_UID_LAST 0xEFFFF

#define NCCH_PARTITION_ID 0x108
#define NCCH_PROGRAM_ID 0x118
#define NCCH_EXHEADER_HASH 0x160
#define NCCH_EXHEADER_SIZE 0x180
#define NCCH_FLAGS 0x188
#define NCCH_EXEFS 0x1A0
#define NCCH_EXEFS_HASH 0x1C0
#define EXH_JUMP_ID 0x1C8
#define EXH_ACI_PROGRAM_ID 0x200
#define EXH_DESC_PROGRAM_ID 0x600
#define TMD_TITLE_ID 0x4C
#define TMD_CONTENT_COUNT 0x9E
#define TMD_INFO_HASH 0xA4
#define TMD_INFO 0xC4
#define TMD_INFO_SIZE (64 * 0x24)
#define TICKET_TITLE_ID 0x9C
#define META_SMDH 0x400

typedef struct {
    u8* data;
    u32 size;
    u32 ticket;    // The ticket's and the TMD's bodies (after their signatures).
    u32 tmd;
    u32 content;   // The NCCH.
    u32 meta;
    u32 exheader;
    u32 exheaderSize;
    u32 exefs;
    u32 exefsHashSize;
} fwd_cia;

static u32 fwd_get32le(const u8* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

static u32 fwd_get32be(const u8* p) {
    return ((u32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void fwd_put64le(u8* p, u64 v) {
    for(u32 i = 0; i < 8; i++) {
        p[i] = (u8) (v >> (i * 8));
    }
}

static void fwd_put64be(u8* p, u64 v) {
    for(u32 i = 0; i < 8; i++) {
        p[i] = (u8) (v >> (56 - i * 8));
    }
}

static u32 fwd_align(u32 v) {
    return (v + 0x3F) & ~0x3Fu;
}

static u32 fwd_signature_size(u32 type) {
    switch(type) {
        case 0x10003: return 0x23C;
        case 0x10004: return 0x13C;
        case 0x10005: return 0x7C;
        default: return 0;
    }
}

static Result fwd_open(fwd_cia* cia) {
    u8* d = cia->data;
    if(cia->size < 0x2040) {
        return R_APP_BAD_DATA;
    }

    u32 cert = fwd_align(fwd_get32le(d));
    u32 ticket = cert + fwd_align(fwd_get32le(d + 0x08));
    u32 tmd = ticket + fwd_align(fwd_get32le(d + 0x0C));
    cia->content = tmd + fwd_align(fwd_get32le(d + 0x10));
    u32 contentSize = fwd_get32le(d + 0x18);
    cia->meta = cia->content + fwd_align(contentSize);
    if(cia->meta + fwd_get32le(d + 0x14) > cia->size || fwd_get32le(d + 0x14) < META_SMDH + sizeof(SMDH)) {
        return R_APP_BAD_DATA;
    }

    u32 ticketSig = fwd_signature_size(fwd_get32be(d + ticket));
    u32 tmdSig = fwd_signature_size(fwd_get32be(d + tmd));
    if(ticketSig == 0 || tmdSig == 0) {
        return R_APP_BAD_DATA;
    }

    cia->ticket = ticket + 4 + ticketSig;
    cia->tmd = tmd + 4 + tmdSig;

    u8* ncch = d + cia->content;
    if(memcmp(ncch + 0x100, "NCCH", 4) != 0) {
        return R_APP_BAD_DATA;
    }

    u32 unit = 0x200u << ncch[NCCH_FLAGS + 6];
    cia->exheader = cia->content + 0x200;
    cia->exheaderSize = fwd_get32le(ncch + NCCH_EXHEADER_SIZE);
    cia->exefs = cia->content + fwd_get32le(ncch + NCCH_EXEFS) * unit;
    cia->exefsHashSize = fwd_get32le(ncch + NCCH_EXEFS + 8) * unit;
    if(cia->exefs + 0x200 > cia->meta) {
        return R_APP_BAD_DATA;
    }

    return 0;
}

// An ExeFS file by name: where it is and how big.
static u8* fwd_exefs_file(fwd_cia* cia, const char* name, u32* size, int* index) {
    for(int i = 0; i < 10; i++) {
        const u8* entry = cia->data + cia->exefs + i * 0x10;
        if(entry[0] != 0 && strncmp((const char*) entry, name, 8) == 0) {
            *size = fwd_get32le(entry + 12);
            *index = i;
            return cia->data + cia->exefs + 0x200 + fwd_get32le(entry + 8);
        }
    }

    return NULL;
}

static Result fwd_set_path(fwd_cia* cia, const char* path) {
    size_t len = strlen(path);
    if(len >= FWD_PATH_MAX) {
        return R_APP_OUT_OF_RANGE;
    }

    u32 size = 0;
    int index = 0;
    u8* code = fwd_exefs_file(cia, ".code", &size, &index);
    if(code == NULL) {
        return R_APP_BAD_DATA;
    }

    for(u32 i = 0; i + FWD_MAGIC_LEN + FWD_PATH_MAX <= size; i += 4) {
        if(memcmp(code + i, FWD_MAGIC, FWD_MAGIC_LEN) == 0) {
            memset(code + i + FWD_MAGIC_LEN, 0, FWD_PATH_MAX);
            memcpy(code + i + FWD_MAGIC_LEN, path, len);
            return 0;
        }
    }

    return R_APP_BAD_DATA;
}

static void fwd_set_title_id(fwd_cia* cia, u64 titleId) {
    u8* d = cia->data;
    fwd_put64le(d + cia->content + NCCH_PARTITION_ID, titleId);
    fwd_put64le(d + cia->content + NCCH_PROGRAM_ID, titleId);
    fwd_put64le(d + cia->exheader + EXH_JUMP_ID, titleId);
    fwd_put64le(d + cia->exheader + EXH_ACI_PROGRAM_ID, titleId);
    fwd_put64le(d + cia->exheader + EXH_DESC_PROGRAM_ID, titleId);
    fwd_put64be(d + cia->ticket + TICKET_TITLE_ID, titleId);
    fwd_put64be(d + cia->tmd + TMD_TITLE_ID, titleId);
}

static void fwd_sha256(const u8* data, u32 size, u8* out) {
    mbedtls_sha256_ret(data, size, out, 0);
}

static void fwd_reseal(fwd_cia* cia) {
    u8* d = cia->data;
    for(int i = 0; i < 10; i++) {
        const u8* entry = d + cia->exefs + i * 0x10;
        if(entry[0] != 0) {
            // The files' hashes sit at the end of the ExeFS header, the first file's last.
            fwd_sha256(d + cia->exefs + 0x200 + fwd_get32le(entry + 8), fwd_get32le(entry + 12), d + cia->exefs + 0x200 - (i + 1) * 0x20);
        }
    }

    fwd_sha256(d + cia->exefs, cia->exefsHashSize, d + cia->content + NCCH_EXEFS_HASH);
    fwd_sha256(d + cia->exheader, cia->exheaderSize, d + cia->content + NCCH_EXHEADER_HASH);

    u8* info = d + cia->tmd + TMD_INFO;
    u8* chunks = info + TMD_INFO_SIZE;
    u32 count = (d[cia->tmd + TMD_CONTENT_COUNT] << 8) | d[cia->tmd + TMD_CONTENT_COUNT + 1];
    u32 offset = cia->content;
    for(u32 i = 0; i < count; i++) {
        u8* chunk = chunks + i * 0x30;
        u32 size = fwd_get32be(chunk + 0x0C);   // The contents are far under 4 GB.
        fwd_sha256(d + offset, size, chunk + 0x10);
        offset += size;
    }

    u32 covered = (info[2] << 8) | info[3];
    fwd_sha256(chunks, covered * 0x30, info + 4);
    fwd_sha256(info, TMD_INFO_SIZE, d + cia->tmd + TMD_INFO_HASH);
}

// The .3dsx's SMDH, when it has one.
static bool fwd_read_smdh(const char* path, SMDH* smdh) {
    char full[FILE_PATH_MAX + 8];
    snprintf(full, sizeof(full), "sdmc:%s", path);
    FILE* file = fopen(full, "rb");
    if(file == NULL) {
        return false;
    }

    bool found = false;
    u8 header[0x2C];
    if(fread(header, 1, sizeof(header), file) == sizeof(header) && memcmp(header, "3DSX", 4) == 0) {
        u16 headerSize = (u16) (header[4] | (header[5] << 8));
        u32 offset = fwd_get32le(header + 0x20);
        u32 smdhSize = fwd_get32le(header + 0x24);
        found = headerSize >= 0x2C && smdhSize >= sizeof(SMDH) && fseek(file, offset, SEEK_SET) == 0 && fread(smdh, 1, sizeof(SMDH), file) == sizeof(SMDH)
                && memcmp(smdh->magic, "SMDH", 4) == 0;
    }

    fclose(file);
    return found;
}

static void fwd_put_text(u16* out, size_t count, const char* text) {
    memset(out, 0, count * sizeof(u16));
    ssize_t units = utf8_to_utf16(out, (const uint8_t*) text, count - 1);
    if(units < 0 || (size_t) units >= count) {
        out[count - 1] = 0;
    }
}

// The SMDH a copy gets: the template's flags, ratings and region (shown in the HOME Menu, any region), with the
// .3dsx's names and icons; a .3dsx without one keeps the template's icon and gets its file name.
static void fwd_make_smdh(const SMDH* template, const char* path, SMDH* out, char* name, size_t nameSize) {
    memcpy(out, template, sizeof(SMDH));

    SMDH own;
    if(fwd_read_smdh(path, &own)) {
        memcpy(out->titles, own.titles, sizeof(out->titles));
        memcpy(out->smallIcon, own.smallIcon, sizeof(out->smallIcon));
        memcpy(out->largeIcon, own.largeIcon, sizeof(out->largeIcon));
    } else {
        char file[FILE_NAME_MAX];
        string_get_path_file(file, path, sizeof(file));
        char* dot = strrchr(file, '.');
        if(dot != NULL && dot != file) {
            *dot = '\0';
        }

        for(u32 i = 0; i < 0x10; i++) {
            fwd_put_text(out->titles[i].shortDescription, 0x40, file);
            fwd_put_text(out->titles[i].longDescription, 0x80, file);
            fwd_put_text(out->titles[i].publisher, 0x40, "Homebrew");
        }
    }

    // Titles left empty in some languages: the English one there.
    for(u32 i = 0; i < 0x10; i++) {
        if(out->titles[i].shortDescription[0] == 0) {
            memcpy(&out->titles[i], &out->titles[1], sizeof(SMDH_title));
        }
    }

    SMDH_title* title = smdh_select_title(out);
    memset(name, 0, nameSize);
    utf16_to_utf8((uint8_t*) name, title->shortDescription, nameSize - 1);
}

// A title ID of its own for this path: the same path always gets the same one, so making it again updates it. One taken
// by something else (not one of NSA's forwarders) moves to the next.
static u64 fwd_title_id(const char* path) {
    u32 hash = 2166136261u;
    for(const char* c = path; *c != '\0'; c++) {
        hash = (hash ^ (u8) *c) * 16777619u;
    }

    u32 span = FWD_UID_LAST - FWD_UID_FIRST + 1;
    u32 uid = FWD_UID_FIRST + hash % span;
    for(u32 tries = 0; tries < 64; tries++) {
        u64 titleId = 0x0004000000000000ULL | ((u64) uid << 8);
        char product[16] = {0};
        if(R_FAILED(AM_GetTitleProductCode(MEDIATYPE_SD, titleId, product)) || strncmp(product, FWD_PRODUCT_CODE, sizeof(product)) == 0) {
            return titleId;
        }

        uid = uid >= FWD_UID_LAST ? FWD_UID_FIRST : uid + 1;
    }

    return 0;
}

static Result fwd_load_template(fwd_cia* cia) {
    FILE* file = fopen(FWD_TEMPLATE, "rb");
    if(file == NULL) {
        return R_APP_IO_ERROR;
    }

    Result res = 0;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    cia->size = size > 0 ? (u32) size : 0;
    cia->data = (u8*) malloc(cia->size);
    if(cia->data == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    } else if(fread(cia->data, 1, cia->size, file) != cia->size) {
        res = R_APP_IO_ERROR;
    }

    fclose(file);
    return R_SUCCEEDED(res) ? fwd_open(cia) : res;
}

static Result fwd_install(fwd_cia* cia, u64 titleId) {
    Handle handle = 0;
    Result res = action_install_cia_start(MEDIATYPE_SD, titleId, 0, &handle);
    if(R_FAILED(res)) {
        return res;
    }

    u32 written = 0;
    for(u32 offset = 0; offset < cia->size && R_SUCCEEDED(res); offset += written) {
        u32 chunk = cia->size - offset < 0x20000 ? cia->size - offset : 0x20000;
        res = FSFILE_Write(handle, &written, offset, cia->data + offset, chunk, 0);
        if(R_SUCCEEDED(res) && written == 0) {
            res = R_APP_IO_ERROR;
        }
    }

    if(R_SUCCEEDED(res)) {
        return AM_FinishCiaInstall(handle);
    }

    AM_CancelCIAInstall(handle);
    return res;
}

bool forwarder_available(char* why, size_t size) {
    return launch3dsx_available_for(why, size, true);
}

// The forwarder for `path`, ready to install or save (`cia->data` is the caller's to free).
static Result fwd_build(const char* path, char* name, size_t nameSize, fwd_cia* cia, u64* titleId) {
    Result res = fwd_load_template(cia);
    if(R_SUCCEEDED(res)) {
        res = fwd_set_path(cia, path);
    }

    u32 iconSize = 0;
    int iconIndex = 0;
    u8* icon = R_SUCCEEDED(res) ? fwd_exefs_file(cia, "icon", &iconSize, &iconIndex) : NULL;
    if(R_SUCCEEDED(res) && (icon == NULL || iconSize != sizeof(SMDH))) {
        res = R_APP_BAD_DATA;
    }

    if(R_SUCCEEDED(res) && (*titleId = fwd_title_id(path)) == 0) {
        res = R_APP_OUT_OF_RANGE;
    }

    if(R_SUCCEEDED(res)) {
        SMDH smdh;
        fwd_make_smdh((const SMDH*) icon, path, &smdh, name, nameSize);
        memcpy(icon, &smdh, sizeof(SMDH));
        memcpy(cia->data + cia->meta + META_SMDH, &smdh, sizeof(SMDH));

        fwd_set_title_id(cia, *titleId);
        fwd_reseal(cia);
    }

    return res;
}

Result forwarder_install(const char* path, char* name, size_t nameSize) {
    fwd_cia cia = {0};
    u64 titleId = 0;
    Result res = fwd_build(path, name, nameSize, &cia, &titleId);
    if(R_SUCCEEDED(res)) {
        res = fwd_install(&cia, titleId);
    }

    free(cia.data);
    return res;
}

Result forwarder_save(const char* path, const char* out, char* name, size_t nameSize, u64* titleId) {
    fwd_cia cia = {0};
    Result res = fwd_build(path, name, nameSize, &cia, titleId);
    if(R_SUCCEEDED(res)) {
        FILE* file = fopen(out, "wb");
        if(file == NULL) {
            res = R_APP_IO_ERROR;
        } else {
            if(fwrite(cia.data, 1, cia.size, file) != cia.size) {
                res = R_APP_IO_ERROR;
            }

            fclose(file);
        }
    }

    free(cia.data);
    return res;
}
