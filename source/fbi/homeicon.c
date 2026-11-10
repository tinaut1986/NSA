#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <mbedtls/sha256.h>

#include "homeicon.h"
#include "action/action.h"
#include "../core/core.h"
#include "../libs/stb_image/stb_image.h"

// How a .3dsx becomes a title. The template (romfs:/ciatemplate.cia, built by ciatemplate/) gives the certificates, the
// ticket, the TMD, the NCCH header, the exheader (a homebrew title's permissions and memory), the logo, the banner's
// model and sound, and the meta section. NSA keeps those and makes the rest:
// - .code: the .3dsx's three segments placed where a title's go (text at 0x100000, then rodata, then data), with its
//   relocations applied as Luma3DS's 3DSX loader applies them (sysmodules/loader/source/3dsx.c). One page after the
//   rodata holds the program's arguments (argv[0] is the .3dsx's path), which its start code (_prm) is pointed at;
//   nothing else in _prm is set, so libctru sees a title, not homebrew: it mounts the title's RomFS, and on quitting
//   goes back to the HOME Menu.
// - RomFS: the .3dsx's RomFS (libctru's format is a title's RomFS level 3) with the IVFC hash levels a title's has.
// - ExeFS: .code, the banner (the template's model with the picture replaced) and the icon (the .3dsx's SMDH).
// - Title ID, sizes and every hash: NCCH header, exheader, ExeFS, RomFS, TMD.
// The signatures stay the template's (invalid for the new contents): installing needs signature patches, as NSA's
// exported CIAs do (Luma3DS has them). The RomFS is read three times: its hashes, the content's hash (the TMD comes
// before the content in a CIA) and the install.

#define HI_TEMPLATE "romfs:/ciatemplate.cia"
#define HI_PRODUCT_CODE "CTR-P-NSAF"

// Unique IDs for these titles: a hash of the path picks one in this range, away from NSA's own (0xFB1A0-0xFB1AF).
#define HI_UID_FIRST 0xC8000
#define HI_UID_LAST 0xEFFFF

#define HI_CODE_BASE 0x100000
#define HI_CODE_MAX (48 * 1024 * 1024)
#define HI_PAGE 0x1000
#define HI_BLOCK 0x1000
#define HI_IO_CHUNK 0x40000
#define HI_RELOC_CHUNK 512

#define HI_PICTURE_MAX_FILE (12 * 1024 * 1024)
#define HI_PICTURE_MAX_PIXELS 5000000

#define NCCH_CONTENT_SIZE 0x104
#define NCCH_PARTITION_ID 0x108
#define NCCH_PROGRAM_ID 0x118
#define NCCH_EXHEADER_HASH 0x160
#define NCCH_EXHEADER_SIZE 0x180
#define NCCH_FLAGS 0x188
#define NCCH_EXEFS 0x1A0
#define NCCH_ROMFS 0x1B0
#define NCCH_EXEFS_HASH 0x1C0
#define NCCH_ROMFS_HASH 0x1E0
#define EXH_TEXT 0x10
#define EXH_RODATA 0x20
#define EXH_DATA 0x30
#define EXH_BSS 0x3C
#define EXH_JUMP_ID 0x1C8
#define EXH_ACI_PROGRAM_ID 0x200
#define EXH_DESC_PROGRAM_ID 0x600
#define TMD_TITLE_ID 0x4C
#define TMD_INFO_HASH 0xA4
#define TMD_INFO 0xC4
#define TMD_INFO_SIZE (64 * 0x24)
#define TICKET_TITLE_ID 0x9C
#define META_SMDH 0x400

#define BANNER_SIZE (HOMEICON_BANNER_WIDTH * HOMEICON_BANNER_HEIGHT)

static u32 hi_get32(const u8* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

static u32 hi_get32be(const u8* p) {
    return ((u32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void hi_put32(u8* p, u32 v) {
    for(u32 i = 0; i < 4; i++) {
        p[i] = (u8) (v >> (i * 8));
    }
}

static void hi_put64(u8* p, u64 v) {
    for(u32 i = 0; i < 8; i++) {
        p[i] = (u8) (v >> (i * 8));
    }
}

static void hi_put64be(u8* p, u64 v) {
    for(u32 i = 0; i < 8; i++) {
        p[i] = (u8) (v >> (56 - i * 8));
    }
}

static u64 hi_align(u64 v, u64 to) {
    return (v + to - 1) & ~(to - 1);
}

static void hi_sha256(const u8* data, u32 size, u8* out) {
    mbedtls_sha256_ret(data, size, out, 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// The template

typedef struct {
    u8* data;
    u32 size;
    u32 ticket;        // The ticket's and the TMD's bodies (after their signatures).
    u32 tmd;
    u32 content;       // The NCCH.
    u32 contentSize;
    u32 meta;
    u32 metaSize;
    u32 exefs;         // Where the template's ExeFS starts: everything before it (header, exheader, logo) is kept.
    u32 romfs;
    u32 romfsSize;
} hi_template;

static u32 hi_signature_size(u32 type) {
    switch(type) {
        case 0x10003: return 0x23C;
        case 0x10004: return 0x13C;
        case 0x10005: return 0x7C;
        default: return 0;
    }
}

static Result hi_load_template(hi_template* t) {
    FILE* file = fopen(HI_TEMPLATE, "rb");
    if(file == NULL) {
        return R_APP_IO_ERROR;
    }

    Result res = 0;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    t->size = size > 0 ? (u32) size : 0;
    t->data = (u8*) malloc(t->size);
    if(t->data == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    } else if(fread(t->data, 1, t->size, file) != t->size) {
        res = R_APP_IO_ERROR;
    }

    fclose(file);
    if(R_FAILED(res)) {
        return res;
    }

    u8* d = t->data;
    if(t->size < 0x2040) {
        return R_APP_BAD_DATA;
    }

    u32 cert = (u32) hi_align(hi_get32(d), 0x40);
    u32 ticket = cert + (u32) hi_align(hi_get32(d + 0x08), 0x40);
    u32 tmd = ticket + (u32) hi_align(hi_get32(d + 0x0C), 0x40);
    t->content = tmd + (u32) hi_align(hi_get32(d + 0x10), 0x40);
    t->contentSize = hi_get32(d + 0x18);
    t->meta = t->content + (u32) hi_align(t->contentSize, 0x40);
    t->metaSize = hi_get32(d + 0x14);
    if(t->meta + t->metaSize > t->size || t->metaSize < META_SMDH + sizeof(SMDH)) {
        return R_APP_BAD_DATA;
    }

    u32 ticketSig = hi_signature_size(hi_get32be(d + ticket));
    u32 tmdSig = hi_signature_size(hi_get32be(d + tmd));
    if(ticketSig == 0 || tmdSig == 0) {
        return R_APP_BAD_DATA;
    }

    t->ticket = ticket + 4 + ticketSig;
    t->tmd = tmd + 4 + tmdSig;

    u8* ncch = d + t->content;
    // Media units of 0x200 bytes: the sizes below are worked out in those.
    if(memcmp(ncch + 0x100, "NCCH", 4) != 0 || ncch[NCCH_FLAGS + 6] != 0 || hi_get32(ncch + NCCH_EXHEADER_SIZE) != 0x400) {
        return R_APP_BAD_DATA;
    }

    t->exefs = hi_get32(ncch + NCCH_EXEFS) * 0x200;
    t->romfs = hi_get32(ncch + NCCH_ROMFS) * 0x200;
    t->romfsSize = hi_get32(ncch + NCCH_ROMFS + 4) * 0x200;
    if(t->exefs < 0xA00 || t->exefs + 0x200 > t->contentSize || t->romfs == 0 || t->romfs + t->romfsSize > t->contentSize) {
        return R_APP_BAD_DATA;
    }

    return 0;
}

// A file of the template's ExeFS by name.
static u8* hi_template_exefs_file(hi_template* t, const char* name, u32* size) {
    u8* exefs = t->data + t->content + t->exefs;
    for(int i = 0; i < 10; i++) {
        const u8* entry = exefs + i * 0x10;
        if(entry[0] != 0 && strncmp((const char*) entry, name, 8) == 0) {
            u32 offset = hi_get32(entry + 8);
            *size = hi_get32(entry + 12);
            if(t->exefs + 0x200 + offset + *size > t->contentSize) {
                return NULL;
            }

            return exefs + 0x200 + offset;
        }
    }

    return NULL;
}

// ---------------------------------------------------------------------------------------------------------------------
// The .3dsx's names and icon

static Result hi_open_file(const char* path, Handle* handle) {
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = FSUSER_OpenFileDirectly(handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, FS_OPEN_READ, 0);
    fs_free_path_utf8(fsPath);
    return res;
}

static Result hi_read(Handle handle, u64 offset, void* out, u32 size) {
    u32 got = 0;
    Result res = FSFILE_Read(handle, &got, offset, out, size);
    return R_SUCCEEDED(res) && got != size ? R_APP_IO_ERROR : res;
}

bool homeicon_read_smdh(const char* path, SMDH* smdh) {
    Handle handle = 0;
    if(R_FAILED(hi_open_file(path, &handle))) {
        return false;
    }

    bool found = false;
    u8 header[0x2C];
    if(R_SUCCEEDED(hi_read(handle, 0, header, sizeof(header))) && memcmp(header, "3DSX", 4) == 0) {
        u16 headerSize = (u16) (header[4] | (header[5] << 8));
        found = headerSize >= 0x2C && hi_get32(header + 0x24) >= sizeof(SMDH) && R_SUCCEEDED(hi_read(handle, hi_get32(header + 0x20), smdh, sizeof(SMDH)))
                && memcmp(smdh->magic, "SMDH", 4) == 0;
    }

    FSFILE_Close(handle);
    return found;
}

static void hi_file_title(const char* path, char* out, size_t size) {
    string_get_path_file(out, path, size);
    char* dot = strrchr(out, '.');
    if(dot != NULL && dot != out) {
        *dot = '\0';
    }
}

void homeicon_name(const char* path, const SMDH* smdh, char* name, size_t size) {
    memset(name, 0, size);
    if(smdh != NULL) {
        // Read only: the title for the system's language, else the English one.
        SMDH_title* title = smdh_select_title((SMDH*) smdh);
        if(title->shortDescription[0] == 0) {
            title = (SMDH_title*) &smdh->titles[1];
        }

        utf16_to_utf8((uint8_t*) name, title->shortDescription, size - 1);
    }

    if(name[0] == '\0') {
        hi_file_title(path, name, size);
    }
}

static void hi_put_text(u16* out, size_t count, const char* text) {
    memset(out, 0, count * sizeof(u16));
    ssize_t units = utf8_to_utf16(out, (const uint8_t*) text, count - 1);
    if(units < 0 || (size_t) units >= count) {
        out[count - 1] = 0;
    }
}

// The title's SMDH: the template's flags, ratings and region (shown in the HOME Menu, any region), with the .3dsx's
// names and icons; a .3dsx without one keeps NSA's icon and gets its file name.
static void hi_make_smdh(const SMDH* template, const char* path, SMDH* out, char* name, size_t nameSize) {
    memcpy(out, template, sizeof(SMDH));

    // An SMDH is 14 KB: on the heap, the install thread's stack is small.
    SMDH* own = (SMDH*) malloc(sizeof(SMDH));
    if(own != NULL && homeicon_read_smdh(path, own)) {
        memcpy(out->titles, own->titles, sizeof(out->titles));
        memcpy(out->smallIcon, own->smallIcon, sizeof(out->smallIcon));
        memcpy(out->largeIcon, own->largeIcon, sizeof(out->largeIcon));
    } else {
        char file[FILE_NAME_MAX];
        hi_file_title(path, file, sizeof(file));
        for(u32 i = 0; i < 0x10; i++) {
            hi_put_text(out->titles[i].shortDescription, 0x40, file);
            hi_put_text(out->titles[i].longDescription, 0x80, file);
            hi_put_text(out->titles[i].publisher, 0x40, "Homebrew");
        }
    }

    free(own);

    // Titles left empty in some languages: the English one there.
    for(u32 i = 0; i < 0x10; i++) {
        if(out->titles[i].shortDescription[0] == 0) {
            memcpy(&out->titles[i], &out->titles[1], sizeof(SMDH_title));
        }
    }

    homeicon_name(path, out, name, nameSize);
}

// ---------------------------------------------------------------------------------------------------------------------
// Banner pictures

// Pixel (x, y) of a 48x48 SMDH icon (RGB565, 8x8 tiles in Morton order, top row first) as 0xRRGGBB.
static u32 hi_icon_pixel(const SMDH* smdh, u32 x, u32 y) {
    u32 morton = 0;
    for(u32 bit = 0; bit < 3; bit++) {
        morton |= ((x >> bit) & 1) << (bit * 2);
        morton |= ((y >> bit) & 1) << (bit * 2 + 1);
    }

    u32 index = ((y / 8) * 6 + x / 8) * 64 + morton;
    u16 v = (u16) (smdh->largeIcon[index * 2] | (smdh->largeIcon[index * 2 + 1] << 8));
    u32 r = (v >> 11) & 0x1F;
    u32 g = (v >> 5) & 0x3F;
    u32 b = v & 0x1F;
    return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

u32 homeicon_icon_color(const SMDH* smdh) {
    if(smdh == NULL) {
        return 0x3A4250;
    }

    // The average of the icon's coloured pixels (its greys and near-whites and blacks only when it has nothing else).
    u32 sum[3] = {0, 0, 0};
    u32 all[3] = {0, 0, 0};
    u32 count = 0;
    for(u32 y = 0; y < 48; y++) {
        for(u32 x = 0; x < 48; x++) {
            u32 c = hi_icon_pixel(smdh, x, y);
            u32 r = (c >> 16) & 0xFF;
            u32 g = (c >> 8) & 0xFF;
            u32 b = c & 0xFF;
            all[0] += r;
            all[1] += g;
            all[2] += b;

            u32 max = r > g ? (r > b ? r : b) : (g > b ? g : b);
            u32 min = r < g ? (r < b ? r : b) : (g < b ? g : b);
            if(max - min > 40) {
                sum[0] += r;
                sum[1] += g;
                sum[2] += b;
                count++;
            }
        }
    }

    if(count < 48) {
        return ((all[0] / 2304) << 16) | ((all[1] / 2304) << 8) | (all[2] / 2304);
    }

    return ((sum[0] / count) << 16) | ((sum[1] / count) << 8) | (sum[2] / count);
}

static void hi_fill(u8* rgba, u32 x, u32 y, u32 color) {
    u8* p = rgba + (y * HOMEICON_BANNER_WIDTH + x) * 4;
    p[0] = (u8) (color >> 16);
    p[1] = (u8) (color >> 8);
    p[2] = (u8) color;
    p[3] = 0xFF;
}

// `color` scaled to `percent` of its brightness.
static u32 hi_shade(u32 color, u32 percent) {
    u32 r = ((color >> 16) & 0xFF) * percent / 100;
    u32 g = ((color >> 8) & 0xFF) * percent / 100;
    u32 b = (color & 0xFF) * percent / 100;
    return ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

void homeicon_plain_banner(const SMDH* smdh, u8* rgba) {
    u32 color = homeicon_icon_color(smdh);
    for(u32 y = 0; y < HOMEICON_BANNER_HEIGHT; y++) {
        u32 shade = hi_shade(color, 70 - y * 30 / HOMEICON_BANNER_HEIGHT);
        for(u32 x = 0; x < HOMEICON_BANNER_WIDTH; x++) {
            hi_fill(rgba, x, y, shade);
        }
    }

    if(smdh != NULL) {
        // The icon at twice its size, in the middle.
        u32 left = (HOMEICON_BANNER_WIDTH - 96) / 2;
        u32 top = (HOMEICON_BANNER_HEIGHT - 96) / 2;
        for(u32 y = 0; y < 96; y++) {
            for(u32 x = 0; x < 96; x++) {
                hi_fill(rgba, left + x, top + y, hi_icon_pixel(smdh, x / 2, y / 2));
            }
        }
    }
}

Result homeicon_picture_banner(const char* path, u8* rgba) {
    char full[FILE_PATH_MAX + 8];
    snprintf(full, sizeof(full), "sdmc:%s", path);
    FILE* file = fopen(full, "rb");
    if(file == NULL) {
        return R_APP_IO_ERROR;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if(size <= 0 || size > HI_PICTURE_MAX_FILE) {
        fclose(file);
        return size <= 0 ? R_APP_BAD_DATA : R_APP_OUT_OF_RANGE;
    }

    u8* data = (u8*) malloc((size_t) size);
    if(data == NULL) {
        fclose(file);
        return R_APP_OUT_OF_MEMORY;
    }

    bool read = fread(data, 1, (size_t) size, file) == (size_t) size;
    fclose(file);

    int w = 0;
    int h = 0;
    int comp = 0;
    // The size is checked before decoding: a picture's pixels may need far more memory than its file.
    if(!read || !stbi_info_from_memory(data, (int) size, &w, &h, &comp) || w <= 0 || h <= 0) {
        free(data);
        return R_APP_BAD_DATA;
    }

    if((u64) w * (u64) h > HI_PICTURE_MAX_PIXELS) {
        free(data);
        return R_APP_OUT_OF_RANGE;
    }

    u8* src = stbi_load_from_memory(data, (int) size, &w, &h, &comp, 4);
    free(data);
    if(src == NULL) {
        return R_APP_BAD_DATA;
    }

    // The background: the picture's average colour, a little darker.
    u64 sum[3] = {0, 0, 0};
    for(size_t i = 0; i < (size_t) w * h; i++) {
        u32 a = src[i * 4 + 3];
        sum[0] += src[i * 4] * a / 255;
        sum[1] += src[i * 4 + 1] * a / 255;
        sum[2] += src[i * 4 + 2] * a / 255;
    }

    u64 pixels = (u64) w * h;
    u32 background = hi_shade((u32) (((sum[0] / pixels) << 16) | ((sum[1] / pixels) << 8) | (sum[2] / pixels)), 60);
    for(u32 y = 0; y < HOMEICON_BANNER_HEIGHT; y++) {
        for(u32 x = 0; x < HOMEICON_BANNER_WIDTH; x++) {
            hi_fill(rgba, x, y, background);
        }
    }

    // Whole, as big as fits: each banner pixel the average of the picture's pixels under it (or the nearest one, when the
    // picture is smaller than the banner).
    u32 outW = HOMEICON_BANNER_WIDTH;
    u32 outH = (u32) ((u64) h * HOMEICON_BANNER_WIDTH / w);
    if(outH > HOMEICON_BANNER_HEIGHT) {
        outH = HOMEICON_BANNER_HEIGHT;
        outW = (u32) ((u64) w * HOMEICON_BANNER_HEIGHT / h);
    }

    outW = outW == 0 ? 1 : outW;
    outH = outH == 0 ? 1 : outH;
    u32 left = (HOMEICON_BANNER_WIDTH - outW) / 2;
    u32 top = (HOMEICON_BANNER_HEIGHT - outH) / 2;
    for(u32 dy = 0; dy < outH; dy++) {
        u32 y0 = (u32) ((u64) dy * h / outH);
        u32 y1 = (u32) ((u64) (dy + 1) * h / outH);
        y1 = y1 <= y0 ? y0 + 1 : y1;
        for(u32 dx = 0; dx < outW; dx++) {
            u32 x0 = (u32) ((u64) dx * w / outW);
            u32 x1 = (u32) ((u64) (dx + 1) * w / outW);
            x1 = x1 <= x0 ? x0 + 1 : x1;

            u32 acc[4] = {0, 0, 0, 0};
            for(u32 y = y0; y < y1; y++) {
                const u8* p = src + ((size_t) y * w + x0) * 4;
                for(u32 x = x0; x < x1; x++, p += 4) {
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    acc[3] += p[3];
                }
            }

            u32 n = (y1 - y0) * (x1 - x0);
            u32 a = acc[3] / n;
            u8* out = rgba + ((top + dy) * HOMEICON_BANNER_WIDTH + left + dx) * 4;
            for(u32 c = 0; c < 3; c++) {
                out[c] = (u8) ((acc[c] / n * a + out[c] * (255 - a)) / 255);
            }
        }
    }

    stbi_image_free(src);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The banner file: the template's CBMD with the picture written into its model's texture

static u8* hi_lz11_decompress(const u8* in, u32 inSize, u32* outSize) {
    if(inSize < 4 || in[0] != 0x11) {
        return NULL;
    }

    u32 size = in[1] | (in[2] << 8) | (in[3] << 16);
    u32 pos = 4;
    if(size == 0) {
        if(inSize < 8) {
            return NULL;
        }

        size = hi_get32(in + 4);
        pos = 8;
    }

    if(size == 0 || size > 0x100000) {
        return NULL;
    }

    u8* out = (u8*) malloc(size);
    if(out == NULL) {
        return NULL;
    }

    u32 len = 0;
    while(len < size) {
        if(pos >= inSize) {
            free(out);
            return NULL;
        }

        u8 flags = in[pos++];
        for(u32 bit = 0; bit < 8 && len < size; bit++) {
            if((flags & (0x80 >> bit)) == 0) {
                if(pos >= inSize) {
                    free(out);
                    return NULL;
                }

                out[len++] = in[pos++];
                continue;
            }

            if(pos + 2 > inSize) {
                free(out);
                return NULL;
            }

            u32 b0 = in[pos];
            u32 count = 0;
            u32 disp = 0;
            switch(b0 >> 4) {
                case 0:
                    if(pos + 3 > inSize) {
                        free(out);
                        return NULL;
                    }

                    count = (((b0 & 0xF) << 4) | (in[pos + 1] >> 4)) + 0x11;
                    disp = ((in[pos + 1] & 0xF) << 8) | in[pos + 2];
                    pos += 3;
                    break;
                case 1:
                    if(pos + 4 > inSize) {
                        free(out);
                        return NULL;
                    }

                    count = (((b0 & 0xF) << 12) | (in[pos + 1] << 4) | (in[pos + 2] >> 4)) + 0x111;
                    disp = ((in[pos + 2] & 0xF) << 8) | in[pos + 3];
                    pos += 4;
                    break;
                default:
                    count = (b0 >> 4) + 1;
                    disp = ((b0 & 0xF) << 8) | in[pos + 1];
                    pos += 2;
                    break;
            }

            if(disp + 1 > len) {
                free(out);
                return NULL;
            }

            for(u32 i = 0; i < count && len < size; i++, len++) {
                out[len] = out[len - disp - 1];
            }
        }
    }

    *outSize = size;
    return out;
}

// LZ11 without back references (every byte a literal): an eighth bigger than the data, and simple.
static u32 hi_lz11_store(const u8* in, u32 size, u8* out) {
    u32 pos = 0;
    out[pos++] = 0x11;
    out[pos++] = (u8) size;
    out[pos++] = (u8) (size >> 8);
    out[pos++] = (u8) (size >> 16);
    for(u32 i = 0; i < size; i += 8) {
        out[pos++] = 0;
        for(u32 j = i; j < i + 8 && j < size; j++) {
            out[pos++] = in[j];
        }
    }

    return pos;
}

// The model's 256x128 RGBA4444 texture (bannertool's banner): its TXOB's height, width, format, type and data size, then
// a pointer to the data relative to where it is stored.
static u8* hi_banner_texture(u8* cgfx, u32 size) {
    for(u32 i = 0; i + 0x48 <= size; i += 4) {
        if(memcmp(cgfx + i, "TXOB", 4) == 0 && hi_get32(cgfx + i + 0x14) == HOMEICON_BANNER_HEIGHT && hi_get32(cgfx + i + 0x18) == HOMEICON_BANNER_WIDTH
           && hi_get32(cgfx + i + 0x1C) == 0x6752 && hi_get32(cgfx + i + 0x20) == 0x8033 && hi_get32(cgfx + i + 0x40) == BANNER_SIZE * 2) {
            u32 data = i + 0x44 + hi_get32(cgfx + i + 0x44);
            return data + BANNER_SIZE * 2 <= size ? cgfx + data : NULL;
        }
    }

    return NULL;
}

// Writes `rgba` into the texture: 8x8 tiles in Morton order, top row first (as bannertool stores it), 4 bits a channel
// with an ordered dither so gradients do not band.
static void hi_banner_pixels(u8* texture, const u8* rgba) {
    static const u8 bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for(u32 y = 0; y < HOMEICON_BANNER_HEIGHT; y++) {
        for(u32 x = 0; x < HOMEICON_BANNER_WIDTH; x++) {
            u32 morton = 0;
            for(u32 bit = 0; bit < 3; bit++) {
                morton |= ((x >> bit) & 1) << (bit * 2);
                morton |= ((y >> bit) & 1) << (bit * 2 + 1);
            }

            const u8* p = rgba + (y * HOMEICON_BANNER_WIDTH + x) * 4;
            u32 t = bayer[y & 3][x & 3] * 16;
            u32 q[4];
            for(u32 c = 0; c < 4; c++) {
                q[c] = (p[c] * 15 + (c == 3 ? 128 : t)) / 255;
                q[c] = q[c] > 15 ? 15 : q[c];
            }

            u16 v = (u16) ((q[0] << 12) | (q[1] << 8) | (q[2] << 4) | q[3]);
            u32 index = ((y / 8) * (HOMEICON_BANNER_WIDTH / 8) + x / 8) * 64 + morton;
            texture[index * 2] = (u8) v;
            texture[index * 2 + 1] = (u8) (v >> 8);
        }
    }
}

// The template's banner with `rgba` as its picture.
static Result hi_make_banner(hi_template* t, const u8* rgba, u8** out, u32* outSize) {
    u32 size = 0;
    u8* cbmd = hi_template_exefs_file(t, "banner", &size);
    if(cbmd == NULL || size < 0x88 || memcmp(cbmd, "CBMD", 4) != 0 || hi_get32(cbmd + 0x08) != 0x88) {
        return R_APP_BAD_DATA;
    }

    u32 cwav = hi_get32(cbmd + 0x84);
    if(cwav <= 0x88 || cwav >= size) {
        return R_APP_BAD_DATA;
    }

    u32 cgfxSize = 0;
    u8* cgfx = hi_lz11_decompress(cbmd + 0x88, cwav - 0x88, &cgfxSize);
    if(cgfx == NULL) {
        return R_APP_BAD_DATA;
    }

    u8* texture = hi_banner_texture(cgfx, cgfxSize);
    if(texture == NULL) {
        free(cgfx);
        return R_APP_BAD_DATA;
    }

    hi_banner_pixels(texture, rgba);

    u32 packedMax = 4 + cgfxSize + (cgfxSize + 7) / 8;
    u32 newCwav = (u32) hi_align(0x88 + packedMax, 0x20);
    u32 total = newCwav + (size - cwav);
    u8* banner = (u8*) calloc(1, total);
    if(banner == NULL) {
        free(cgfx);
        return R_APP_OUT_OF_MEMORY;
    }

    memcpy(banner, cbmd, 0x88);
    u32 packed = hi_lz11_store(cgfx, cgfxSize, banner + 0x88);
    free(cgfx);
    newCwav = (u32) hi_align(0x88 + packed, 0x20);
    hi_put32(banner + 0x84, newCwav);
    memcpy(banner + newCwav, cbmd + cwav, size - cwav);

    *out = banner;
    *outSize = newCwav + (size - cwav);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The code

typedef struct {
    u8* code;
    u32 codeSize;      // The .code: text, rodata (with the arguments' page) and data, each in whole pages.
    u32 textSize;
    u32 rodataSize;
    u32 dataSize;      // In bytes (its pages are in codeSize).
    u32 bssSize;
    u64 romfsOffset;   // In the .3dsx; 0 when it has none.
    u64 romfsSize;
} hi_program;

static Result hi_load_program(Handle file, u64 fileSize, const char* path, hi_program* p) {
    u8 hdr[0x2C] = {0};
    Result res = hi_read(file, 0, hdr, 0x20);
    if(R_FAILED(res)) {
        return res;
    }

    u16 headerSize = (u16) (hdr[4] | (hdr[5] << 8));
    u16 relocHdrSize = (u16) (hdr[6] | (hdr[7] << 8));
    u32 codeSeg = hi_get32(hdr + 0x10);
    u32 rodataSeg = hi_get32(hdr + 0x14);
    u32 dataSeg = hi_get32(hdr + 0x18);
    u32 bss = hi_get32(hdr + 0x1C);
    if(memcmp(hdr, "3DSX", 4) != 0 || headerSize < 0x20 || relocHdrSize < 8 || relocHdrSize > 0x100 || (relocHdrSize & 3) != 0
       || codeSeg == 0 || bss > dataSeg || codeSeg > HI_CODE_MAX || rodataSeg > HI_CODE_MAX || dataSeg > HI_CODE_MAX) {
        return R_APP_BAD_DATA;
    }

    if(headerSize >= 0x2C && R_SUCCEEDED(hi_read(file, 0x20, hdr + 0x20, 0x0C))) {
        u64 romfs = hi_get32(hdr + 0x28);
        if(romfs != 0 && romfs + 0x28 <= fileSize) {
            p->romfsOffset = romfs;
            p->romfsSize = fileSize - romfs;
        }
    }

    u32 seg[3] = {(u32) hi_align(codeSeg, HI_PAGE), (u32) hi_align(rodataSeg, HI_PAGE), (u32) hi_align(dataSeg, HI_PAGE)};
    u64 bufferSize = (u64) seg[0] + seg[1] + HI_PAGE + seg[2];
    if(bufferSize > HI_CODE_MAX) {
        return R_APP_OUT_OF_RANGE;
    }

    u8* code = (u8*) memalign(HI_PAGE, (size_t) bufferSize);
    if(code == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    memset(code, 0, (size_t) bufferSize);

    // Where each segment is in the buffer and in the title's memory: the arguments' page sits between rodata and data.
    u8* segPtr[3] = {code, code + seg[0], code + seg[0] + seg[1] + HI_PAGE};
    u32 segAddr[3] = {HI_CODE_BASE, HI_CODE_BASE + seg[0], HI_CODE_BASE + seg[0] + seg[1] + HI_PAGE};
    u32 argvAddr = HI_CODE_BASE + seg[0] + seg[1];
    u8* argvPage = code + seg[0] + seg[1];

    u32 tables = relocHdrSize / 4;
    u32 counts[3][64];
    u64 offset = headerSize;
    for(u32 i = 0; i < 3 && R_SUCCEEDED(res); i++) {
        res = hi_read(file, offset, counts[i], relocHdrSize);
        offset += relocHdrSize;
    }

    u32 loaded[3] = {codeSeg, rodataSeg, dataSeg - bss};
    for(u32 i = 0; i < 3 && R_SUCCEEDED(res); i++) {
        if(loaded[i] > 0) {
            res = hi_read(file, offset, segPtr[i], loaded[i]);
        }

        offset += loaded[i];
    }

    // The relocations, as Luma3DS's loader does them: absolute (an address in the .3dsx's layout made a real one) and
    // cross-segment relative ones; the first two tables of each segment, the others skipped.
    u32* relocs = (u32*) malloc(HI_RELOC_CHUNK * 4);
    if(R_SUCCEEDED(res) && relocs == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    }

    u32 layout[2] = {seg[0], seg[0] + seg[1]};
    for(u32 i = 0; i < 3 && R_SUCCEEDED(res); i++) {
        for(u32 j = 0; j < tables && R_SUCCEEDED(res); j++) {
            u32 remaining = counts[i][j];
            if(j >= 2) {
                offset += (u64) remaining * 4;
                continue;
            }

            u32* pos = (u32*) segPtr[i];
            u32* end = pos + seg[i] / 4;
            while(remaining > 0 && R_SUCCEEDED(res)) {
                u32 todo = remaining > HI_RELOC_CHUNK ? HI_RELOC_CHUNK : remaining;
                remaining -= todo;
                if(R_FAILED(res = hi_read(file, offset, relocs, todo * 4))) {
                    break;
                }

                offset += todo * 4;
                for(u32 k = 0; k < todo && pos < end; k++) {
                    pos += relocs[k] & 0xFFFF;
                    u32 patches = relocs[k] >> 16;
                    for(u32 m = 0; m < patches && pos < end; m++, pos++) {
                        u32 value = *pos;
                        u32 sub = value >> 28;
                        u32 off = value & ~0xF0000000u;
                        u32 addr = off < layout[0] ? segAddr[0] + off : off < layout[1] ? segAddr[1] + off - layout[0] : segAddr[2] + off - layout[1];
                        if(j == 0) {
                            if(sub != 0) {
                                res = R_APP_BAD_DATA;
                                break;
                            }

                            *pos = addr;
                        } else {
                            u32 at = segAddr[i] + (u32) ((u8*) pos - segPtr[i]);
                            u32 rel = addr - at;
                            if(sub == 0) {
                                *pos = rel;
                            } else if(sub == 1) {
                                *pos = rel & ~0x80000000u;
                            } else {
                                res = R_APP_BAD_DATA;
                                break;
                            }
                        }
                    }

                    if(R_FAILED(res)) {
                        break;
                    }
                }
            }
        }
    }

    free(relocs);

    if(R_SUCCEEDED(res)) {
        // The arguments: argc, then the strings (argv[0] the .3dsx, as the Homebrew Launcher gives it), for _prm.
        hi_put32(argvPage, 1);
        snprintf((char*) argvPage + 4, HI_PAGE - 4, "sdmc:%s", path);
        if(memcmp(code + 4, "_prm", 4) == 0) {
            hi_put32(code + 0x18, argvAddr);
        }

        // The data in the file up to its last non-zero word (relocations may write past the loaded part); the rest is BSS.
        u32 dataSize = dataSeg - bss;
        for(u32 i = dataSeg & ~3u; i > dataSize; i -= 4) {
            if(hi_get32(segPtr[2] + i - 4) != 0) {
                dataSize = i;
                break;
            }
        }

        p->code = code;
        p->textSize = seg[0];
        p->rodataSize = seg[1] + HI_PAGE;
        p->dataSize = dataSize;
        p->bssSize = dataSeg - dataSize;
        p->codeSize = seg[0] + seg[1] + HI_PAGE + (u32) hi_align(dataSize, HI_PAGE);
        if(dataSize == 0) {
            // A title's data section is never empty: one zero word, taken from the BSS.
            p->dataSize = 4;
            p->bssSize = dataSeg > 4 ? dataSeg - 4 : 0;
            p->codeSize += HI_PAGE;
        }
    } else {
        free(code);
    }

    return res;
}

// ---------------------------------------------------------------------------------------------------------------------
// The CIA

typedef struct {
    homeicon_job* job;
    hi_template t;
    hi_program program;
    Handle file;

    u8 exefsHeader[0x200];
    u8* banner;
    u32 bannerSize;
    SMDH smdh;
    u32 exefsSize;

    // The RomFS, when the .3dsx has one: its first part (IVFC header and master hash), then level 3 (the .3dsx's),
    // level 1 and level 2.
    u8* romfsHead;
    u32 romfsHeadSize;
    u8* level1;
    u32 level1Size;
    u8* level2;
    u32 level2Size;
    u64 romfsSize;

    u32 romfsOffset;   // In the NCCH.
    u64 ncchSize;
} hi_build;

// Where the bytes go: a running hash, or the install (or a file).
typedef struct {
    mbedtls_sha256_context* sha;
    Handle cia;
    FILE* out;
    u8* buffer;
    u32 fill;
    u64 offset;
    homeicon_job* job;
} hi_out;

static Result hi_flush(hi_out* o) {
    if(o->fill == 0) {
        return 0;
    }

    Result res = 0;
    if(o->out != NULL) {
        if(fwrite(o->buffer, 1, o->fill, o->out) != o->fill) {
            res = R_APP_IO_ERROR;
        }
    } else {
        u32 written = 0;
        res = FSFILE_Write(o->cia, &written, o->offset, o->buffer, o->fill, 0);
        if(R_SUCCEEDED(res) && written != o->fill) {
            res = R_APP_IO_ERROR;
        }
    }

    o->offset += o->fill;
    o->fill = 0;
    return res;
}

static Result hi_emit(hi_out* o, const void* data, u32 size) {
    if(o->job->cancel) {
        return R_APP_CANCELLED;
    }

    if(o->sha != NULL) {
        if(data != NULL) {
            mbedtls_sha256_update_ret(o->sha, (const u8*) data, size);
        } else {
            static const u8 zeros[0x200] = {0};
            for(u32 done = 0; done < size; done += sizeof(zeros)) {
                mbedtls_sha256_update_ret(o->sha, zeros, size - done < sizeof(zeros) ? size - done : sizeof(zeros));
            }
        }

        return 0;
    }

    const u8* in = (const u8*) data;
    while(size > 0) {
        u32 n = HI_IO_CHUNK - o->fill < size ? HI_IO_CHUNK - o->fill : size;
        if(in != NULL) {
            memcpy(o->buffer + o->fill, in, n);
            in += n;
        } else {
            memset(o->buffer + o->fill, 0, n);
        }

        o->fill += n;
        size -= n;
        if(o->fill == HI_IO_CHUNK) {
            Result res = hi_flush(o);
            if(R_FAILED(res)) {
                return res;
            }
        }
    }

    return 0;
}

// The NCCH, byte by byte, into `o`: the template's header, exheader and logo, the ExeFS, then the RomFS (level 3 read
// from the .3dsx again).
static Result hi_emit_ncch(hi_build* b, hi_out* o, u8* chunk) {
    hi_template* t = &b->t;
    Result res = hi_emit(o, t->data + t->content, t->exefs);
    if(R_SUCCEEDED(res)) {
        res = hi_emit(o, b->exefsHeader, sizeof(b->exefsHeader));
    }

    const void* files[3] = {b->program.code, b->banner, &b->smdh};
    u32 sizes[3] = {b->program.codeSize, b->bannerSize, sizeof(SMDH)};
    for(u32 i = 0; i < 3 && R_SUCCEEDED(res); i++) {
        if(R_SUCCEEDED(res = hi_emit(o, files[i], sizes[i]))) {
            res = hi_emit(o, NULL, (u32) hi_align(sizes[i], 0x200) - sizes[i]);
        }
    }

    if(R_SUCCEEDED(res)) {
        res = hi_emit(o, NULL, b->romfsOffset - (t->exefs + b->exefsSize));
    }

    if(R_FAILED(res)) {
        return res;
    }

    if(b->romfsHead == NULL) {
        return hi_emit(o, t->data + t->content + t->romfs, t->romfsSize);
    }

    res = hi_emit(o, b->romfsHead, b->romfsHeadSize);
    u64 l3 = b->program.romfsSize;
    for(u64 done = 0; done < l3 && R_SUCCEEDED(res); done += HI_IO_CHUNK) {
        u32 n = (u32) (l3 - done < HI_IO_CHUNK ? l3 - done : HI_IO_CHUNK);
        if(R_SUCCEEDED(res = hi_read(b->file, b->program.romfsOffset + done, chunk, n))) {
            res = hi_emit(o, chunk, n);
            b->job->done += n;
        }
    }

    if(R_SUCCEEDED(res)) {
        res = hi_emit(o, NULL, (u32) (hi_align(l3, HI_BLOCK) - l3));
    }

    if(R_SUCCEEDED(res) && R_SUCCEEDED(res = hi_emit(o, b->level1, b->level1Size))) {
        res = hi_emit(o, NULL, (u32) hi_align(b->level1Size, HI_BLOCK) - b->level1Size);
    }

    if(R_SUCCEEDED(res) && R_SUCCEEDED(res = hi_emit(o, b->level2, b->level2Size))) {
        res = hi_emit(o, NULL, (u32) hi_align(b->level2Size, HI_BLOCK) - b->level2Size);
    }

    return res;
}

// The hashes of every 0x1000-byte block of `data` (the last one padded with zeros).
static void hi_hash_blocks(const u8* data, u32 size, u8* out) {
    for(u32 i = 0; i < size; i += HI_BLOCK) {
        if(size - i >= HI_BLOCK) {
            hi_sha256(data + i, HI_BLOCK, out + i / HI_BLOCK * 0x20);
        } else {
            mbedtls_sha256_context sha;
            mbedtls_sha256_init(&sha);
            mbedtls_sha256_starts_ret(&sha, 0);
            mbedtls_sha256_update_ret(&sha, data + i, size - i);
            static const u8 zeros[HI_BLOCK] = {0};
            mbedtls_sha256_update_ret(&sha, zeros, HI_BLOCK - (size - i));
            mbedtls_sha256_finish_ret(&sha, out + i / HI_BLOCK * 0x20);
            mbedtls_sha256_free(&sha);
        }
    }
}

// The RomFS's hash levels: level 2 from the .3dsx's RomFS (level 3), level 1 from level 2, the master hash from level 1.
static Result hi_build_romfs(hi_build* b, u8* chunk) {
    u64 l3 = b->program.romfsSize;
    if(l3 == 0) {
        return 0;
    }

    if(l3 > 0xFFFFFFFFull - HI_BLOCK) {
        return R_APP_OUT_OF_RANGE;
    }

    b->level2Size = (u32) (hi_align(l3, HI_BLOCK) / HI_BLOCK * 0x20);
    b->level1Size = (u32) (hi_align(b->level2Size, HI_BLOCK) / HI_BLOCK * 0x20);
    u32 masterSize = (u32) (hi_align(b->level1Size, HI_BLOCK) / HI_BLOCK * 0x20);
    u32 level0Size = 0x60 + masterSize;
    b->romfsHeadSize = (u32) hi_align(level0Size, HI_BLOCK);

    b->level2 = (u8*) calloc(1, b->level2Size);
    b->level1 = (u8*) calloc(1, b->level1Size);
    b->romfsHead = (u8*) calloc(1, b->romfsHeadSize);
    if(b->level2 == NULL || b->level1 == NULL || b->romfsHead == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = 0;
    for(u64 done = 0; done < l3 && R_SUCCEEDED(res); done += HI_IO_CHUNK) {
        if(b->job->cancel) {
            return R_APP_CANCELLED;
        }

        u32 n = (u32) (l3 - done < HI_IO_CHUNK ? l3 - done : HI_IO_CHUNK);
        if(R_SUCCEEDED(res = hi_read(b->file, b->program.romfsOffset + done, chunk, n))) {
            hi_hash_blocks(chunk, n, b->level2 + done / HI_BLOCK * 0x20);
            b->job->done += n;
        }
    }

    if(R_FAILED(res)) {
        return res;
    }

    hi_hash_blocks(b->level2, b->level2Size, b->level1);

    u8* h = b->romfsHead;
    memcpy(h, "IVFC", 4);
    hi_put32(h + 0x04, 0x10000);
    hi_put32(h + 0x08, masterSize);
    u64 logical[3] = {0, hi_align(b->level1Size, HI_BLOCK), 0};
    logical[2] = hi_align(logical[1] + b->level2Size, HI_BLOCK);
    u64 sizes[3] = {b->level1Size, b->level2Size, l3};
    for(u32 i = 0; i < 3; i++) {
        u8* level = h + 0x0C + i * 0x18;
        hi_put64(level, logical[i]);
        hi_put64(level + 8, sizes[i]);
        hi_put32(level + 16, 12);
    }

    hi_put32(h + 0x54, 0x5C);
    hi_hash_blocks(b->level1, b->level1Size, h + 0x60);

    b->romfsSize = b->romfsHeadSize + hi_align(l3, HI_BLOCK) + hi_align(b->level1Size, HI_BLOCK) + hi_align(b->level2Size, HI_BLOCK);
    return 0;
}

// A title ID of its own for this path: the same path always gets the same one, so making it again updates it. One taken
// by something else (not one of NSA's) moves to the next.
static u64 hi_title_id(const char* path) {
    u32 hash = 2166136261u;
    for(const char* c = path; *c != '\0'; c++) {
        hash = (hash ^ (u8) *c) * 16777619u;
    }

    u32 span = HI_UID_LAST - HI_UID_FIRST + 1;
    u32 uid = HI_UID_FIRST + hash % span;
    for(u32 tries = 0; tries < 64; tries++) {
        u64 titleId = 0x0004000000000000ULL | ((u64) uid << 8);
        char product[16] = {0};
        if(R_FAILED(AM_GetTitleProductCode(MEDIATYPE_SD, titleId, product)) || strncmp(product, HI_PRODUCT_CODE, sizeof(product)) == 0) {
            return titleId;
        }

        uid = uid >= HI_UID_LAST ? HI_UID_FIRST : uid + 1;
    }

    return 0;
}

// Everything but the passes over the RomFS that write: the code, the ExeFS, the RomFS's hashes, the NCCH header and
// exheader, then the content's hash into the TMD.
static Result hi_prepare(hi_build* b, u8* chunk) {
    homeicon_job* job = b->job;
    Result res = hi_load_template(&b->t);
    if(R_FAILED(res)) {
        return res;
    }

    hi_template* t = &b->t;
    u32 iconSize = 0;
    u8* icon = hi_template_exefs_file(t, "icon", &iconSize);
    if(icon == NULL || iconSize != sizeof(SMDH)) {
        return R_APP_BAD_DATA;
    }

    hi_make_smdh((const SMDH*) icon, job->path, &b->smdh, job->name, sizeof(job->name));
    if((job->titleId = hi_title_id(job->path)) == 0) {
        return R_APP_OUT_OF_RANGE;
    }

    if(R_FAILED(res = hi_open_file(job->path, &b->file))) {
        return res;
    }

    u64 fileSize = 0;
    if(R_FAILED(res = FSFILE_GetSize(b->file, &fileSize))) {
        return res;
    }

    if(R_FAILED(res = hi_load_program(b->file, fileSize, job->path, &b->program))) {
        return res;
    }

    job->total = b->program.codeSize + b->program.romfsSize * 3;
    job->done = b->program.codeSize;

    const u8* picture = job->banner;
    u8* plain = NULL;
    if(picture == NULL) {
        if((plain = (u8*) malloc(BANNER_SIZE * 4)) == NULL) {
            return R_APP_OUT_OF_MEMORY;
        }

        SMDH* own = (SMDH*) malloc(sizeof(SMDH));
        homeicon_plain_banner(own != NULL && homeicon_read_smdh(job->path, own) ? own : NULL, plain);
        free(own);
        picture = plain;
    }

    res = hi_make_banner(t, picture, &b->banner, &b->bannerSize);
    free(plain);
    if(R_FAILED(res)) {
        return res;
    }

    // ExeFS: .code, banner, icon, each from a 0x200 boundary; their hashes at the end of the header, the first file's last.
    const char* names[3] = {".code", "banner", "icon"};
    const u8* files[3] = {b->program.code, b->banner, (const u8*) &b->smdh};
    u32 sizes[3] = {b->program.codeSize, b->bannerSize, sizeof(SMDH)};
    u32 offset = 0;
    for(u32 i = 0; i < 3; i++) {
        u8* entry = b->exefsHeader + i * 0x10;
        strncpy((char*) entry, names[i], 8);
        hi_put32(entry + 8, offset);
        hi_put32(entry + 12, sizes[i]);
        hi_sha256(files[i], sizes[i], b->exefsHeader + 0x200 - (i + 1) * 0x20);
        offset += (u32) hi_align(sizes[i], 0x200);
    }

    b->exefsSize = 0x200 + offset;

    if(R_FAILED(res = hi_build_romfs(b, chunk))) {
        return res;
    }

    b->romfsOffset = (u32) hi_align(t->exefs + b->exefsSize, HI_BLOCK);
    u64 romfsSize = b->romfsHead != NULL ? b->romfsSize : t->romfsSize;
    b->ncchSize = b->romfsOffset + romfsSize;

    // Exheader: where the code goes, its name (the .3dsx's file name, for process lists), the title ID.
    u8* ncch = t->data + t->content;
    u8* exh = ncch + 0x200;
    hi_program* p = &b->program;
    u32 rodataAddr = HI_CODE_BASE + p->textSize;
    u32 dataAddr = rodataAddr + p->rodataSize;
    u32 fields[3][3] = {{HI_CODE_BASE, p->textSize / HI_PAGE, p->textSize}, {rodataAddr, p->rodataSize / HI_PAGE, p->rodataSize},
                        {dataAddr, (u32) (hi_align(p->dataSize, HI_PAGE) / HI_PAGE), p->dataSize}};
    u32 at[3] = {EXH_TEXT, EXH_RODATA, EXH_DATA};
    for(u32 i = 0; i < 3; i++) {
        for(u32 j = 0; j < 3; j++) {
            hi_put32(exh + at[i] + j * 4, fields[i][j]);
        }
    }

    hi_put32(exh + EXH_BSS, p->bssSize);

    char file[FILE_NAME_MAX];
    hi_file_title(job->path, file, sizeof(file));
    memset(exh, 0, 8);
    for(u32 i = 0, n = 0; file[i] != '\0' && n < 8; i++) {
        char c = file[i];
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            exh[n++] = (u8) c;
        }
    }

    if(exh[0] == 0) {
        memcpy(exh, "Homebrew", 8);
    }

    hi_put64(ncch + NCCH_PARTITION_ID, job->titleId);
    hi_put64(ncch + NCCH_PROGRAM_ID, job->titleId);
    hi_put64(exh + EXH_JUMP_ID, job->titleId);
    hi_put64(exh + EXH_ACI_PROGRAM_ID, job->titleId);
    hi_put64(exh + EXH_DESC_PROGRAM_ID, job->titleId);
    hi_sha256(exh, 0x400, ncch + NCCH_EXHEADER_HASH);

    // NCCH header: sizes in media units of 0x200 bytes, the ExeFS's and the RomFS's hashes.
    hi_put32(ncch + NCCH_CONTENT_SIZE, (u32) (b->ncchSize / 0x200));
    hi_put32(ncch + NCCH_EXEFS, t->exefs / 0x200);
    hi_put32(ncch + NCCH_EXEFS + 4, b->exefsSize / 0x200);
    hi_put32(ncch + NCCH_EXEFS + 8, 1);
    hi_sha256(b->exefsHeader, 0x200, ncch + NCCH_EXEFS_HASH);
    hi_put32(ncch + NCCH_ROMFS, b->romfsOffset / 0x200);
    hi_put32(ncch + NCCH_ROMFS + 4, (u32) (romfsSize / 0x200));
    if(b->romfsHead != NULL) {
        u32 hashed = (u32) hi_align(0x60 + hi_get32(b->romfsHead + 0x08), 0x200);
        hi_put32(ncch + NCCH_ROMFS + 8, hashed / 0x200);
        hi_sha256(b->romfsHead, hashed, ncch + NCCH_ROMFS_HASH);
    }

    // The ticket, the TMD and the CIA header: title ID, the content's size and hash.
    u8* d = t->data;
    hi_put64be(d + t->ticket + TICKET_TITLE_ID, job->titleId);
    hi_put64be(d + t->tmd + TMD_TITLE_ID, job->titleId);
    hi_put64(d + 0x18, b->ncchSize);
    memcpy(d + t->meta + META_SMDH, &b->smdh, sizeof(SMDH));

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    hi_out hashing = {.sha = &sha, .job = job};
    res = hi_emit_ncch(b, &hashing, chunk);

    u8* info = d + t->tmd + TMD_INFO;
    u8* first = info + TMD_INFO_SIZE;
    hi_put64be(first + 0x08, b->ncchSize);
    mbedtls_sha256_finish_ret(&sha, first + 0x10);
    mbedtls_sha256_free(&sha);

    u32 covered = (info[2] << 8) | info[3];
    hi_sha256(first, covered * 0x30, info + 4);
    hi_sha256(info, TMD_INFO_SIZE, d + t->tmd + TMD_INFO_HASH);
    return res;
}

// The whole CIA into `o`: the template's header, certificates, ticket and TMD, the NCCH, the meta section.
static Result hi_emit_cia(hi_build* b, hi_out* o, u8* chunk) {
    hi_template* t = &b->t;
    Result res = hi_emit(o, t->data, t->content);
    if(R_SUCCEEDED(res)) {
        res = hi_emit_ncch(b, o, chunk);
    }

    if(R_SUCCEEDED(res)) {
        res = hi_emit(o, NULL, (u32) (hi_align(b->ncchSize, 0x40) - b->ncchSize));
    }

    if(R_SUCCEEDED(res)) {
        res = hi_emit(o, t->data + t->meta, t->metaSize);
    }

    return R_SUCCEEDED(res) ? hi_flush(o) : res;
}

static void hi_free(hi_build* b) {
    if(b->file != 0) {
        FSFILE_Close(b->file);
    }

    free(b->t.data);
    free(b->program.code);
    free(b->banner);
    free(b->romfsHead);
    free(b->level1);
    free(b->level2);
}

static Result hi_run(homeicon_job* job, const char* out) {
    job->done = 0;
    job->total = 0;

    // On the heap (it holds an SMDH): the install thread's stack is small.
    hi_build* b = (hi_build*) calloc(1, sizeof(hi_build));
    u8* chunk = (u8*) malloc(HI_IO_CHUNK);
    u8* buffer = (u8*) malloc(HI_IO_CHUNK);
    if(b == NULL || chunk == NULL || buffer == NULL) {
        free(b);
        free(chunk);
        free(buffer);
        return R_APP_OUT_OF_MEMORY;
    }

    b->job = job;
    Result res = hi_prepare(b, chunk);

    hi_out o = {.buffer = buffer, .job = job};
    if(R_SUCCEEDED(res)) {
        if(out != NULL) {
            if((o.out = fopen(out, "wb")) == NULL) {
                res = R_APP_IO_ERROR;
            }
        } else {
            res = action_install_cia_start(MEDIATYPE_SD, job->titleId, 0, &o.cia);
        }
    }

    if(R_SUCCEEDED(res)) {
        res = hi_emit_cia(b, &o, chunk);
        if(o.out != NULL) {
            fclose(o.out);
        } else if(R_SUCCEEDED(res)) {
            res = AM_FinishCiaInstall(o.cia);
        } else {
            AM_CancelCIAInstall(o.cia);
        }
    }

    hi_free(b);
    free(b);
    free(chunk);
    free(buffer);
    return res;
}

Result homeicon_install(homeicon_job* job) {
    return hi_run(job, NULL);
}

Result homeicon_save(homeicon_job* job, const char* out) {
    return hi_run(job, out);
}
