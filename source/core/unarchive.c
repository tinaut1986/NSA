#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>
#include <zlib.h>

#include "error.h"
#include "minire.h"
#include "unarchive.h"
#include "../libs/lzma/7z.h"
#include "../libs/lzma/7zAlloc.h"
#include "../libs/lzma/7zCrc.h"

#define UNARCHIVE_CHUNK (64 * 1024)
#define UNARCHIVE_PATH_MAX 512

// ---- shared ----

static bool unarchive_cancelled(unarchive_progress* progress) {
    return progress != NULL && progress->cancel != NULL && *progress->cancel;
}

// Makes every folder above `path` ("sdmc:/a/b/file": sdmc:/a and sdmc:/a/b).
static void unarchive_make_parents(const char* path) {
    char dir[UNARCHIVE_PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char* start = strchr(dir, '/');
    if(start == NULL) {
        return;
    }

    for(char* c = start + 1; *c != '\0'; c++) {
        if(*c == '/') {
            *c = '\0';
            mkdir(dir, 0777);
            *c = '/';
        }
    }
}

// Where an entry goes: `output` and what follows the match in its name.
static bool unarchive_target(const char* name, const char* pattern, const char* output, char* out, size_t size) {
    size_t start = 0;
    size_t end = 0;
    if(!minire_search(pattern, name, &start, &end)) {
        return false;
    }

    const char* rest = name + end;
    size_t outLen = strlen(output);
    if(outLen > 0 && output[outLen - 1] == '/' && *rest == '/') {
        rest++;
    }

    return (size_t) snprintf(out, size, "%s%s", output, rest) < size;
}

static bool unarchive_is_dir_name(const char* name) {
    size_t len = strlen(name);
    return len > 0 && name[len - 1] == '/';
}

static void unarchive_finished_file(unarchive_progress* progress, const char* path, u32* count) {
    (*count)++;
    if(progress != NULL && progress->created != NULL) {
        progress->created(progress->userData, path);
    }
}

char unarchive_kind(const char* path) {
    FILE* fd = fopen(path, "rb");
    if(fd == NULL) {
        return 0;
    }

    u8 magic[6] = {0};
    size_t got = fread(magic, 1, sizeof(magic), fd);
    fclose(fd);

    if(got >= 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4) {
        return 'z';
    }

    if(got == 6 && memcmp(magic, "7z\xBC\xAF\x27\x1C", 6) == 0) {
        return '7';
    }

    return 0;
}

// ---- zip ----

static u16 unarchive_u16(const u8* p) {
    return (u16) (p[0] | (p[1] << 8));
}

static u32 unarchive_u32(const u8* p) {
    return (u32) p[0] | ((u32) p[1] << 8) | ((u32) p[2] << 16) | ((u32) p[3] << 24);
}

// One entry's data, from where its local header says it starts, into `path`.
static Result unarchive_zip_entry(FILE* fd, u32 localOffset, u16 method, u32 packed, u32 unpacked, const char* path, unarchive_progress* progress) {
    u8 header[30];
    if(fseek(fd, (long) localOffset, SEEK_SET) != 0 || fread(header, 1, sizeof(header), fd) != sizeof(header) || unarchive_u32(header) != 0x04034B50) {
        return R_APP_BAD_DATA;
    }

    if(fseek(fd, (long) (localOffset + 30 + unarchive_u16(header + 26) + unarchive_u16(header + 28)), SEEK_SET) != 0) {
        return R_APP_BAD_DATA;
    }

    if(method != 0 && method != 8) {
        return R_APP_NOT_IMPLEMENTED;
    }

    unarchive_make_parents(path);
    FILE* out = fopen(path, "wb");
    if(out == NULL) {
        return R_APP_IO_ERROR;
    }

    u8* in = (u8*) malloc(UNARCHIVE_CHUNK);
    u8* buf = method == 8 ? (u8*) malloc(UNARCHIVE_CHUNK) : NULL;
    Result res = 0;
    if(in == NULL || (method == 8 && buf == NULL)) {
        res = R_APP_OUT_OF_MEMORY;
    }

    z_stream z;
    memset(&z, 0, sizeof(z));
    bool inflating = false;
    if(R_SUCCEEDED(res) && method == 8) {
        if(inflateInit2(&z, -MAX_WBITS) != Z_OK) {
            res = R_APP_OUT_OF_MEMORY;
        } else {
            inflating = true;
        }
    }

    u32 left = packed;
    u64 written = 0;
    bool streamEnd = false;
    while(R_SUCCEEDED(res) && left > 0 && !streamEnd) {
        if(unarchive_cancelled(progress)) {
            res = R_APP_CANCELLED;
            break;
        }

        size_t want = left < UNARCHIVE_CHUNK ? left : UNARCHIVE_CHUNK;
        size_t got = fread(in, 1, want, fd);
        if(got == 0) {
            res = R_APP_BAD_DATA;
            break;
        }

        left -= got;

        if(method == 0) {
            if(fwrite(in, 1, got, out) != got) {
                res = R_APP_IO_ERROR;
                break;
            }

            written += got;
            if(progress != NULL) {
                progress->done += got;
            }

            continue;
        }

        z.next_in = in;
        z.avail_in = (uInt) got;
        while(z.avail_in > 0 && !streamEnd) {
            z.next_out = buf;
            z.avail_out = UNARCHIVE_CHUNK;
            int zr = inflate(&z, Z_NO_FLUSH);
            if(zr != Z_OK && zr != Z_STREAM_END) {
                res = zr == Z_MEM_ERROR ? R_APP_OUT_OF_MEMORY : R_APP_BAD_DATA;
                break;
            }

            size_t have = UNARCHIVE_CHUNK - z.avail_out;
            if(have > 0 && fwrite(buf, 1, have, out) != have) {
                res = R_APP_IO_ERROR;
                break;
            }

            written += have;
            if(progress != NULL) {
                progress->done += have;
            }

            streamEnd = zr == Z_STREAM_END;
        }
    }

    // What is left of a deflated stream once the input is all read.
    while(R_SUCCEEDED(res) && method == 8 && !streamEnd) {
        z.next_out = buf;
        z.avail_out = UNARCHIVE_CHUNK;
        int zr = inflate(&z, Z_FINISH);
        if(zr != Z_OK && zr != Z_STREAM_END && zr != Z_BUF_ERROR) {
            res = R_APP_BAD_DATA;
            break;
        }

        size_t have = UNARCHIVE_CHUNK - z.avail_out;
        if(have == 0 && zr != Z_STREAM_END) {
            res = R_APP_BAD_DATA;
            break;
        }

        if(have > 0 && fwrite(buf, 1, have, out) != have) {
            res = R_APP_IO_ERROR;
            break;
        }

        written += have;
        if(progress != NULL) {
            progress->done += have;
        }

        streamEnd = zr == Z_STREAM_END;
    }

    if(inflating) {
        inflateEnd(&z);
    }

    free(in);
    free(buf);
    if(fclose(out) != 0 && R_SUCCEEDED(res)) {
        res = R_APP_IO_ERROR;
    }

    if(R_SUCCEEDED(res) && written != unpacked) {
        res = R_APP_BAD_DATA;
    }

    if(R_FAILED(res)) {
        remove(path);
    }

    return res;
}

static Result unarchive_zip(const char* archive, const char* pattern, const char* output, unarchive_progress* progress, u32* count) {
    FILE* fd = fopen(archive, "rb");
    if(fd == NULL) {
        return R_APP_IO_ERROR;
    }

    Result res = 0;
    u8* tail = NULL;
    u8* dir = NULL;

    // The end of central directory record, in the last 64 KB (its comment) and 22 bytes.
    fseek(fd, 0, SEEK_END);
    long size = ftell(fd);
    long tailSize = size < 65557 ? size : 65557;
    tail = (u8*) malloc((size_t) tailSize);
    if(tail == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    } else if(fseek(fd, size - tailSize, SEEK_SET) != 0 || fread(tail, 1, (size_t) tailSize, fd) != (size_t) tailSize) {
        res = R_APP_BAD_DATA;
    }

    long eocd = -1;
    for(long i = tailSize - 22; R_SUCCEEDED(res) && i >= 0; i--) {
        if(unarchive_u32(tail + i) == 0x06054B50) {
            eocd = i;
            break;
        }
    }

    u32 entries = 0;
    u32 dirSize = 0;
    u32 dirOffset = 0;
    if(R_SUCCEEDED(res)) {
        if(eocd < 0) {
            res = R_APP_BAD_DATA;
        } else {
            entries = unarchive_u16(tail + eocd + 10);
            dirSize = unarchive_u32(tail + eocd + 12);
            dirOffset = unarchive_u32(tail + eocd + 16);
            // Zip64 (0xFFFFFFFF here) is for archives far bigger than anything in the catalogue.
            if(dirOffset == 0xFFFFFFFF || (long) dirOffset + (long) dirSize > size) {
                res = R_APP_NOT_IMPLEMENTED;
            }
        }
    }

    if(R_SUCCEEDED(res)) {
        dir = (u8*) malloc(dirSize > 0 ? dirSize : 1);
        if(dir == NULL) {
            res = R_APP_OUT_OF_MEMORY;
        } else if(fseek(fd, (long) dirOffset, SEEK_SET) != 0 || fread(dir, 1, dirSize, fd) != dirSize) {
            res = R_APP_BAD_DATA;
        }
    }

    char name[UNARCHIVE_PATH_MAX];
    char target[UNARCHIVE_PATH_MAX];

    // Two passes: how much will come out (for the bar), then the files.
    for(int pass = 0; pass < 2 && R_SUCCEEDED(res); pass++) {
        u32 at = 0;
        for(u32 i = 0; i < entries && R_SUCCEEDED(res); i++) {
            if(at + 46 > dirSize || unarchive_u32(dir + at) != 0x02014B50) {
                res = R_APP_BAD_DATA;
                break;
            }

            const u8* e = dir + at;
            u16 flags = unarchive_u16(e + 8);
            u16 method = unarchive_u16(e + 10);
            u32 packed = unarchive_u32(e + 20);
            u32 unpacked = unarchive_u32(e + 24);
            u16 nameLen = unarchive_u16(e + 28);
            u16 extraLen = unarchive_u16(e + 30);
            u16 commentLen = unarchive_u16(e + 32);
            u32 local = unarchive_u32(e + 42);
            if(at + 46 + nameLen > dirSize) {
                res = R_APP_BAD_DATA;
                break;
            }

            size_t copy = nameLen < sizeof(name) - 1 ? nameLen : sizeof(name) - 1;
            memcpy(name, e + 46, copy);
            name[copy] = '\0';
            for(char* c = name; *c != '\0'; c++) {
                if(*c == '\\') {
                    *c = '/';
                }
            }

            at += 46 + nameLen + extraLen + commentLen;

            if(!unarchive_target(name, pattern, output, target, sizeof(target))) {
                continue;
            }

            if(unarchive_is_dir_name(name)) {
                if(pass == 1) {
                    size_t len = strlen(target);
                    if(len > 0 && target[len - 1] == '/') {
                        target[len - 1] = '\0';
                    }

                    unarchive_make_parents(target);
                    mkdir(target, 0777);
                }

                continue;
            }

            if(pass == 0) {
                if(progress != NULL) {
                    progress->total += unpacked;
                }

                continue;
            }

            if(flags & 1) {
                res = R_APP_NOT_IMPLEMENTED;   // Encrypted.
                break;
            }

            if(R_SUCCEEDED(res = unarchive_zip_entry(fd, local, method, packed, unpacked, target, progress))) {
                unarchive_finished_file(progress, target, count);
            }
        }
    }

    free(tail);
    free(dir);
    fclose(fd);
    return res;
}

// ---- 7z ----

typedef struct {
    ISeekInStream vt;
    FILE* fd;
} unarchive_file_stream;

static SRes unarchive_7z_read(ISeekInStreamPtr p, void* buf, size_t* size) {
    unarchive_file_stream* s = (unarchive_file_stream*) (void*) p;
    if(*size == 0) {
        return SZ_OK;
    }

    *size = fread(buf, 1, *size, s->fd);
    return ferror(s->fd) ? SZ_ERROR_READ : SZ_OK;
}

static SRes unarchive_7z_seek(ISeekInStreamPtr p, Int64* pos, ESzSeek origin) {
    unarchive_file_stream* s = (unarchive_file_stream*) (void*) p;
    int whence = origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if(fseek(s->fd, (long) *pos, whence) != 0) {
        return SZ_ERROR_READ;
    }

    *pos = ftell(s->fd);
    return SZ_OK;
}

static Result unarchive_7z_result(SRes sr) {
    switch(sr) {
        case SZ_OK:
            return 0;
        case SZ_ERROR_MEM:
            return R_APP_OUT_OF_MEMORY;
        case SZ_ERROR_UNSUPPORTED:
            return R_APP_NOT_IMPLEMENTED;
        case SZ_ERROR_READ:
        case SZ_ERROR_WRITE:
            return R_APP_IO_ERROR;
        default:
            return R_APP_BAD_DATA;
    }
}

// A name from the archive (UTF-16) as UTF-8.
static void unarchive_utf16_to_utf8(const UInt16* in, char* out, size_t size) {
    size_t o = 0;
    for(size_t i = 0; in[i] != 0 && o + 4 < size; i++) {
        u32 c = in[i];
        if(c >= 0xD800 && c < 0xDC00 && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        }

        if(c == '\\') {
            c = '/';
        }

        if(c < 0x80) {
            out[o++] = (char) c;
        } else if(c < 0x800) {
            out[o++] = (char) (0xC0 | (c >> 6));
            out[o++] = (char) (0x80 | (c & 0x3F));
        } else if(c < 0x10000) {
            out[o++] = (char) (0xE0 | (c >> 12));
            out[o++] = (char) (0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char) (0x80 | (c & 0x3F));
        } else {
            out[o++] = (char) (0xF0 | (c >> 18));
            out[o++] = (char) (0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char) (0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char) (0x80 | (c & 0x3F));
        }
    }

    out[o] = '\0';
}

static Result unarchive_7z(const char* archive, const char* pattern, const char* output, unarchive_progress* progress, u32* count) {
    static bool crcReady = false;
    if(!crcReady) {
        CrcGenerateTable();
        crcReady = true;
    }

    ISzAlloc alloc = {SzAlloc, SzFree};

    unarchive_file_stream stream;
    stream.vt.Read = unarchive_7z_read;
    stream.vt.Seek = unarchive_7z_seek;
    stream.fd = fopen(archive, "rb");
    if(stream.fd == NULL) {
        return R_APP_IO_ERROR;
    }

    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    look.buf = (Byte*) malloc(UNARCHIVE_CHUNK);
    look.bufSize = UNARCHIVE_CHUNK;
    look.realStream = &stream.vt;
    LookToRead2_INIT(&look);

    Result res = look.buf != NULL ? 0 : R_APP_OUT_OF_MEMORY;

    CSzArEx db;
    SzArEx_Init(&db);
    if(R_SUCCEEDED(res)) {
        res = unarchive_7z_result(SzArEx_Open(&db, &look.vt, &alloc, &alloc));
    }

    UInt16* name16 = NULL;
    size_t name16Size = 0;
    char name[UNARCHIVE_PATH_MAX];
    char target[UNARCHIVE_PATH_MAX];

    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = NULL;
    size_t outBufferSize = 0;

    for(int pass = 0; pass < 2 && R_SUCCEEDED(res); pass++) {
        for(UInt32 i = 0; i < db.NumFiles && R_SUCCEEDED(res); i++) {
            if(unarchive_cancelled(progress)) {
                res = R_APP_CANCELLED;
                break;
            }

            size_t len = SzArEx_GetFileNameUtf16(&db, i, NULL);
            if(len > name16Size) {
                UInt16* grown = (UInt16*) realloc(name16, len * sizeof(UInt16));
                if(grown == NULL) {
                    res = R_APP_OUT_OF_MEMORY;
                    break;
                }

                name16 = grown;
                name16Size = len;
            }

            SzArEx_GetFileNameUtf16(&db, i, name16);
            unarchive_utf16_to_utf8(name16, name, sizeof(name));

            bool isDir = SzArEx_IsDir(&db, i);
            if(isDir) {
                size_t nameLen = strlen(name);
                if(nameLen + 1 < sizeof(name)) {
                    name[nameLen] = '/';   // As a zip writes a folder, so "^dir/" patterns match it too.
                    name[nameLen + 1] = '\0';
                }
            }

            if(!unarchive_target(name, pattern, output, target, sizeof(target))) {
                continue;
            }

            if(isDir) {
                if(pass == 1) {
                    size_t tl = strlen(target);
                    if(tl > 0 && target[tl - 1] == '/') {
                        target[tl - 1] = '\0';
                    }

                    unarchive_make_parents(target);
                    mkdir(target, 0777);
                }

                continue;
            }

            if(pass == 0) {
                if(progress != NULL) {
                    progress->total += SzArEx_GetFileSize(&db, i);
                }

                continue;
            }

            size_t offset = 0;
            size_t processed = 0;
            if(R_FAILED(res = unarchive_7z_result(SzArEx_Extract(&db, &look.vt, i, &blockIndex, &outBuffer, &outBufferSize, &offset, &processed, &alloc, &alloc)))) {
                break;
            }

            unarchive_make_parents(target);
            FILE* out = fopen(target, "wb");
            if(out == NULL) {
                res = R_APP_IO_ERROR;
                break;
            }

            size_t done = 0;
            while(done < processed) {
                size_t chunk = processed - done < UNARCHIVE_CHUNK ? processed - done : UNARCHIVE_CHUNK;
                if(fwrite(outBuffer + offset + done, 1, chunk, out) != chunk) {
                    res = R_APP_IO_ERROR;
                    break;
                }

                done += chunk;
                if(progress != NULL) {
                    progress->done += chunk;
                }
            }

            if(fclose(out) != 0 && R_SUCCEEDED(res)) {
                res = R_APP_IO_ERROR;
            }

            if(R_FAILED(res)) {
                remove(target);
                break;
            }

            unarchive_finished_file(progress, target, count);
        }
    }

    ISzAlloc_Free(&alloc, outBuffer);
    SzArEx_Free(&db, &alloc);
    free(name16);
    free(look.buf);
    fclose(stream.fd);
    return res;
}

Result unarchive_extract(const char* archive, const char* pattern, const char* output, unarchive_progress* progress, u32* count) {
    *count = 0;
    switch(unarchive_kind(archive)) {
        case 'z':
            return unarchive_zip(archive, pattern, output, progress, count);
        case '7':
            return unarchive_7z(archive, pattern, output, progress, count);
        default:
            return R_APP_BAD_DATA;
    }
}
