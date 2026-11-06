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
#include "../libs/lzma/Lzma2Dec.h"
#include "../libs/lzma/LzmaDec.h"

#define UNARCHIVE_CHUNK (64 * 1024)
#define UNARCHIVE_WRITE_BUFFER (256 * 1024)
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

    // Every write to the SD card costs a lot on its own: big ones (as the FTP server does).
    setvbuf(out, NULL, _IOFBF, UNARCHIVE_WRITE_BUFFER);

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
    unarchive_progress* progress;   // A .7z decodes a whole block at once: its bar follows what is read of the archive.
} unarchive_file_stream;

static SRes unarchive_7z_read(ISeekInStreamPtr p, void* buf, size_t* size) {
    unarchive_file_stream* s = (unarchive_file_stream*) (void*) p;
    if(*size == 0) {
        return SZ_OK;
    }

    *size = fread(buf, 1, *size, s->fd);
    if(s->progress != NULL) {
        s->progress->done += *size;
    }

    if(s->progress != NULL && s->progress->cancel != NULL && *s->progress->cancel) {
        return SZ_ERROR_PROGRESS;   // Cancelled in the middle of a block.
    }

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
        case SZ_ERROR_PROGRESS:
            return R_APP_CANCELLED;
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

// A block packed with LZMA, LZMA2 or nothing (by far the usual ones) is decoded as it is read, a piece at a time, each file
// written as it comes out: only the dictionary is in memory (no bigger than the block), not the whole block, which may not
// fit (HoriHD-Gallery's is 43 MB). Each file's CRC is checked. Other methods go through SzArEx_Extract, all in memory.
#define UNARCHIVE_7Z_COPY 0
#define UNARCHIVE_7Z_LZMA 0x30101
#define UNARCHIVE_7Z_LZMA2 0x21

typedef struct {
    const CSzArEx* db;
    char** targets;          // Where each wanted file goes (NULL: not wanted).
    unarchive_progress* progress;
    u32* count;
    UInt32 file;             // The file the output is in now,
    UInt64 fileEnd;          // where it ends in the block,
    FILE* out;               // and its open file (NULL when not wanted).
    UInt32 crc;
} unarchive_7z_writer;

u32 unarchive_last_dictionary = 0;   // The last .7z block's dictionary, for SITE UNARCHIVE.

static bool unarchive_7z_streamable(const CSzArEx* db, UInt32 folder, CSzFolder* f, const Byte** props) {
    const Byte* data = db->db.CodersData + db->db.FoCodersOffsets[folder];
    CSzData sd;
    sd.Data = data;
    sd.Size = db->db.FoCodersOffsets[(size_t) folder + 1] - db->db.FoCodersOffsets[folder];
    if(SzGetNextFolderItem(f, &sd) != SZ_OK || f->NumCoders != 1 || f->NumPackStreams != 1) {
        return false;
    }

    UInt32 method = f->Coders[0].MethodID;
    *props = data + f->Coders[0].PropsOffset;
    return method == UNARCHIVE_7Z_COPY || method == UNARCHIVE_7Z_LZMA || method == UNARCHIVE_7Z_LZMA2;
}

// Closes the file being written; a wrong CRC deletes it.
static Result unarchive_7z_close(unarchive_7z_writer* w) {
    Result res = 0;
    if(w->out != NULL) {
        if(fclose(w->out) != 0) {
            res = R_APP_IO_ERROR;
        }

        w->out = NULL;
        const char* target = w->targets[w->file];
        if(R_SUCCEEDED(res) && SzBitWithVals_Check(&w->db->CRCs, w->file) && w->db->CRCs.Vals[w->file] != CRC_GET_DIGEST(w->crc)) {
            res = R_APP_BAD_DATA;
        }

        if(R_SUCCEEDED(res)) {
            unarchive_finished_file(w->progress, target, w->count);
        } else {
            remove(target);
        }
    }

    return res;
}

// Starts w->file: where it ends, and its file opened if it is wanted.
static Result unarchive_7z_open(unarchive_7z_writer* w) {
    w->fileEnd = w->db->UnpackPositions[w->file + 1];
    w->crc = CRC_INIT_VAL;
    const char* target = w->targets[w->file];
    if(target != NULL && !SzArEx_IsDir(w->db, w->file)) {
        unarchive_make_parents(target);
        w->out = fopen(target, "wb");
        if(w->out == NULL) {
            return R_APP_IO_ERROR;
        }

        setvbuf(w->out, NULL, _IOFBF, UNARCHIVE_WRITE_BUFFER);
    }

    return 0;
}

// Moves on to the file that holds position `pos` of the block (empty files on the way are written as they are passed).
static Result unarchive_7z_seek_file(unarchive_7z_writer* w, UInt64 blockStart, UInt64 pos, UInt32 lastFile) {
    while(w->file < lastFile && blockStart + pos >= w->fileEnd) {
        Result res = unarchive_7z_close(w);
        if(R_FAILED(res)) {
            return res;
        }

        w->file++;
        if(w->file < lastFile && R_FAILED(res = unarchive_7z_open(w))) {
            return res;
        }
    }

    return 0;
}

// What came out of the decoder: positions [pos, pos + size) of the block.
static Result unarchive_7z_write(unarchive_7z_writer* w, UInt64 blockStart, UInt64 pos, const Byte* data, size_t size, UInt32 lastFile) {
    while(size > 0) {
        Result res = unarchive_7z_seek_file(w, blockStart, pos, lastFile);
        if(R_FAILED(res)) {
            return res;
        }

        if(w->file >= lastFile) {
            return 0;   // Past the last file: nothing more to keep.
        }

        UInt64 left = w->fileEnd - (blockStart + pos);
        size_t chunk = (UInt64) size < left ? size : (size_t) left;
        if(w->out != NULL) {
            if(fwrite(data, 1, chunk, w->out) != chunk) {
                return R_APP_IO_ERROR;
            }

            w->crc = CrcUpdate(w->crc, data, chunk);
        }

        data += chunk;
        size -= chunk;
        pos += chunk;
    }

    return 0;
}

static Result unarchive_7z_stream_folder(FILE* fd, const CSzArEx* db, UInt32 folder, const CSzFolder* f, const Byte* props, char** targets,
                                         unarchive_progress* progress, u32* count) {
    ISzAlloc alloc = {SzAlloc, SzFree};
    UInt32 firstFile = db->FolderToFile[folder];
    UInt32 lastFile = db->FolderToFile[folder + 1];
    UInt64 blockStart = db->UnpackPositions[firstFile];
    UInt64 unpackSize = SzAr_GetFolderUnpackSize(&db->db, folder);
    UInt32 packIndex = db->db.FoStartPackStreamIndex[folder];
    UInt64 packPos = db->dataPos + db->db.PackPositions[packIndex];
    UInt64 packSize = db->db.PackPositions[packIndex + 1] - db->db.PackPositions[packIndex];
    UInt32 method = f->Coders[0].MethodID;
    unsigned propsSize = f->Coders[0].PropsSize;

    unarchive_7z_writer w;
    memset(&w, 0, sizeof(w));
    w.db = db;
    w.targets = targets;
    w.progress = progress;
    w.count = count;
    w.file = firstFile;

    if(fseek(fd, (long) packPos, SEEK_SET) != 0) {
        return R_APP_IO_ERROR;
    }

    Byte* in = (Byte*) malloc(UNARCHIVE_CHUNK);
    if(in == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = unarchive_7z_open(&w);
    CLzmaDec lzma;
    CLzma2Dec lzma2;
    LzmaDec_CONSTRUCT(&lzma)
    Lzma2Dec_CONSTRUCT(&lzma2)
    CLzmaDec* dec = NULL;
    Byte* dic = NULL;
    bool dicLinear = false;
    SizeT dicSize = 0;

    if(method != UNARCHIVE_7Z_COPY) {
        UInt32 wanted = 0;
        if(method == UNARCHIVE_7Z_LZMA) {
            CLzmaProps p;
            if(LzmaProps_Decode(&p, props, propsSize) != SZ_OK || LzmaDec_AllocateProbs(&lzma, props, propsSize, &alloc) != SZ_OK) {
                res = R_APP_BAD_DATA;
            }

            wanted = p.dicSize;
            dec = &lzma;
        } else {
            Byte prop = propsSize == 1 ? props[0] : 41;
            if(prop > 40 || Lzma2Dec_AllocateProbs(&lzma2, prop, &alloc) != SZ_OK) {
                res = R_APP_BAD_DATA;
            }

            wanted = prop == 40 ? 0xFFFFFFFF : ((UInt32) 2 | (prop & 1)) << (prop / 2 + 11);
            dec = &lzma2.decoder;
        }

        unarchive_last_dictionary = wanted;
        // The dictionary need not be bigger than the block.
        dicSize = (SizeT) (wanted < unpackSize ? wanted : unpackSize);
        if(dicSize < 4096) {
            dicSize = 4096;
        }

        // More than the console could ever give (an archive made with a huge dictionary): not even tried.
        if(R_SUCCEEDED(res) && dicSize > 0x8000000) {
            res = R_APP_OUT_OF_MEMORY;
        }

        // The heap first, else the linear heap (the other half of the app's memory, mostly free outside the music and the
        // pictures): one 16 MB dictionary may not fit in what a download left of the heap.
        if(R_SUCCEEDED(res) && (dic = (Byte*) malloc(dicSize)) == NULL) {
            dic = (Byte*) linearAlloc(dicSize);
            dicLinear = dic != NULL;
            if(dic == NULL) {
                res = R_APP_OUT_OF_MEMORY;
            }
        }

        if(R_SUCCEEDED(res)) {
            dec->dic = dic;
            dec->dicBufSize = dicSize;
            if(method == UNARCHIVE_7Z_LZMA) {
                LzmaDec_Init(&lzma);
            } else {
                Lzma2Dec_Init(&lzma2);
            }
        }
    }

    UInt64 packLeft = packSize;
    UInt64 outPos = 0;
    size_t inSize = 0;
    size_t inPos = 0;
    while(R_SUCCEEDED(res) && outPos < unpackSize) {
        if(unarchive_cancelled(progress)) {
            res = R_APP_CANCELLED;
            break;
        }

        if(inPos == inSize && packLeft > 0) {
            size_t want = packLeft < UNARCHIVE_CHUNK ? (size_t) packLeft : UNARCHIVE_CHUNK;
            inSize = fread(in, 1, want, fd);
            inPos = 0;
            if(inSize == 0) {
                res = R_APP_BAD_DATA;
                break;
            }

            packLeft -= inSize;
            if(progress != NULL) {
                progress->done += inSize;
            }
        }

        if(method == UNARCHIVE_7Z_COPY) {
            size_t n = inSize - inPos;
            if((UInt64) n > unpackSize - outPos) {
                n = (size_t) (unpackSize - outPos);
            }

            res = unarchive_7z_write(&w, blockStart, outPos, in + inPos, n, lastFile);
            inPos += n;
            outPos += n;
            continue;
        }

        if(dec->dicPos == dicSize) {
            dec->dicPos = 0;   // The dictionary wraps round: what was written out is no longer needed.
        }

        SizeT before = dec->dicPos;
        SizeT inProcessed = inSize - inPos;
        SizeT limit = dicSize;
        if((UInt64) (limit - before) > unpackSize - outPos) {
            limit = before + (SizeT) (unpackSize - outPos);
        }

        ELzmaStatus status;
        SRes sr = method == UNARCHIVE_7Z_LZMA ? LzmaDec_DecodeToDic(&lzma, limit, in + inPos, &inProcessed, LZMA_FINISH_ANY, &status)
                                              : Lzma2Dec_DecodeToDic(&lzma2, limit, in + inPos, &inProcessed, LZMA_FINISH_ANY, &status);
        inPos += inProcessed;
        SizeT produced = dec->dicPos - before;
        if(sr != SZ_OK) {
            res = sr == SZ_ERROR_MEM ? R_APP_OUT_OF_MEMORY : R_APP_BAD_DATA;
            break;
        }

        if(produced > 0) {
            res = unarchive_7z_write(&w, blockStart, outPos, dic + before, produced, lastFile);
            outPos += produced;
        } else if(inProcessed == 0 && (packLeft == 0 || status == LZMA_STATUS_FINISHED_WITH_MARK)) {
            res = R_APP_BAD_DATA;   // No progress and nothing more to read: the stream ended early.
        }
    }

    // The last file (and any empty ones after it).
    if(R_SUCCEEDED(res)) {
        res = unarchive_7z_seek_file(&w, blockStart, unpackSize, lastFile);
    }

    if(R_SUCCEEDED(res)) {
        res = unarchive_7z_close(&w);
    } else if(w.out != NULL) {
        fclose(w.out);
        remove(w.targets[w.file]);
    }

    if(method == UNARCHIVE_7Z_LZMA) {
        LzmaDec_FreeProbs(&lzma, &alloc);
    } else if(method == UNARCHIVE_7Z_LZMA2) {
        Lzma2Dec_FreeProbs(&lzma2, &alloc);
    }

    if(dicLinear) {
        linearFree(dic);
    } else {
        free(dic);
    }

    free(in);
    return res;
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

    // The bar: the archive's bytes as they are read (the header, then each block as it decodes).
    stream.progress = progress;
    if(progress != NULL) {
        fseek(stream.fd, 0, SEEK_END);
        progress->total = (u64) ftell(stream.fd);
        progress->done = 0;
        fseek(stream.fd, 0, SEEK_SET);
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

    // Where each file goes, if it is wanted; folders are made now.
    char** targets = NULL;
    UInt16* name16 = NULL;
    size_t name16Size = 0;
    char name[UNARCHIVE_PATH_MAX];
    char target[UNARCHIVE_PATH_MAX];
    if(R_SUCCEEDED(res) && (targets = (char**) calloc(db.NumFiles + 1, sizeof(char*))) == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    }

    for(UInt32 i = 0; R_SUCCEEDED(res) && i < db.NumFiles; i++) {
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
            size_t tl = strlen(target);
            if(tl > 0 && target[tl - 1] == '/') {
                target[tl - 1] = '\0';
            }

            unarchive_make_parents(target);
            mkdir(target, 0777);
            continue;
        }

        if((targets[i] = strdup(target)) == NULL) {
            res = R_APP_OUT_OF_MEMORY;
        }
    }

    // Empty files belong to no block.
    for(UInt32 i = 0; R_SUCCEEDED(res) && i < db.NumFiles; i++) {
        if(targets[i] != NULL && db.FileToFolder[i] == (UInt32) -1) {
            unarchive_make_parents(targets[i]);
            FILE* out = fopen(targets[i], "wb");
            if(out == NULL) {
                res = R_APP_IO_ERROR;
                break;
            }

            setvbuf(out, NULL, _IOFBF, UNARCHIVE_WRITE_BUFFER);

            fclose(out);
            unarchive_finished_file(progress, targets[i], count);
        }
    }

    // Block by block, only those holding something wanted.
    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = NULL;
    size_t outBufferSize = 0;
    for(UInt32 folder = 0; R_SUCCEEDED(res) && folder < db.db.NumFolders; folder++) {
        bool wanted = false;
        for(UInt32 i = db.FolderToFile[folder]; i < db.FolderToFile[folder + 1]; i++) {
            if(targets[i] != NULL) {
                wanted = true;
                break;
            }
        }

        if(!wanted) {
            continue;
        }

        CSzFolder f;
        const Byte* props = NULL;
        if(unarchive_7z_streamable(&db, folder, &f, &props)) {
            res = unarchive_7z_stream_folder(stream.fd, &db, folder, &f, props, targets, progress, count);
            continue;
        }

        for(UInt32 i = db.FolderToFile[folder]; R_SUCCEEDED(res) && i < db.FolderToFile[folder + 1]; i++) {
            if(targets[i] == NULL) {
                continue;
            }

            if(unarchive_cancelled(progress)) {
                res = R_APP_CANCELLED;
                break;
            }

            size_t offset = 0;
            size_t processed = 0;
            if(R_FAILED(res = unarchive_7z_result(SzArEx_Extract(&db, &look.vt, i, &blockIndex, &outBuffer, &outBufferSize, &offset, &processed, &alloc, &alloc)))) {
                break;
            }

            unarchive_make_parents(targets[i]);
            FILE* out = fopen(targets[i], "wb");
            if(out == NULL) {
                res = R_APP_IO_ERROR;
                break;
            }

            setvbuf(out, NULL, _IOFBF, UNARCHIVE_WRITE_BUFFER);

            size_t done = 0;
            while(done < processed) {
                size_t chunk = processed - done < UNARCHIVE_CHUNK ? processed - done : UNARCHIVE_CHUNK;
                if(fwrite(outBuffer + offset + done, 1, chunk, out) != chunk) {
                    res = R_APP_IO_ERROR;
                    break;
                }

                done += chunk;
            }

            if(fclose(out) != 0 && R_SUCCEEDED(res)) {
                res = R_APP_IO_ERROR;
            }

            if(R_FAILED(res)) {
                remove(targets[i]);
                break;
            }

            unarchive_finished_file(progress, targets[i], count);
        }
    }

    if(progress != NULL && R_SUCCEEDED(res)) {
        progress->done = progress->total;   // What was not read (other blocks) is not left to do.
    }

    if(targets != NULL) {
        for(UInt32 i = 0; i < db.NumFiles; i++) {
            free(targets[i]);
        }

        free(targets);
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
