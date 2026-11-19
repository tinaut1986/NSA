#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>
#include <mbedtls/sha256.h>

#include "ndsfwd.h"
#include "resources.h"
#include "section.h"
#include "storescript.h"
#include "action/ciacerts.h"
#include "../core/core.h"

// Files -> a .nds -> "Install in the HOME Menu": a DS game forwarder (ndsfwd.h). The question shows the game's icon and
// name; yes checks for nds-bootstrap's forwarder pack (sd:/_nds/ntr-forwarder, what the icon starts; the Store's script
// engine downloads and unpacks it when missing), then a thread fetches the template the first time (Olmectron's, the one
// ndsForwarder and the DS-Homebrew guide use; kept in sdmc:/nsa/forwarder/), fills it, wraps it in a CIA and installs it
// to the console's DSiWare storage. Installing needs signature patches, as NSA's other CIAs (Luma3DS has them).

#define NF_STACK 0x8000
#define NF_TEMPLATE_DIR "sdmc:" APP_DATA_DIR "forwarder"
#define NF_TEMPLATE_SRL NF_TEMPLATE_DIR "/sdcard.nds"
#define NF_TEMPLATE_FWD NF_TEMPLATE_DIR "/sdcard.fwd"
#define NF_TEMPLATE_URL "https://olmectron.github.io/forwarders/sdcard"
#define NF_TEMPLATE_MAX (1024 * 1024)
#define NF_FWD_MAX 4096
#define NF_PACK_FILE "sdmc:/_nds/ntr-forwarder/sdcard.nds"

// nds-bootstrap's forwarder pack, as Universal-DB's script for ndsForwarder installs it.
static const char* nf_pack_script =
        "[{\"type\":\"downloadFile\",\"file\":\"https://github.com/RocketRobz/NTR_Forwarder/releases/latest/download/DS.Game.Forwarder.pack.nds-bootstrap.7z\","
        "\"output\":\"/DS.Game.Forwarder.pack.nds-bootstrap.7z\"},"
        "{\"type\":\"extractFile\",\"file\":\"/DS.Game.Forwarder.pack.nds-bootstrap.7z\",\"input\":\"^for SD Card root/\",\"output\":\"/\"},"
        "{\"type\":\"deleteFile\",\"file\":\"/DS.Game.Forwarder.pack.nds-bootstrap.7z\"}]";

#define CIA_HEADER_SIZE 0x2020
#define CIA_CERT_SIZE 0xA00
#define CIA_TICKET_SIZE 0x350
#define CIA_TMD_SIZE (0xB04 + 0x30)
#define CIA_ALIGN(x) (((x) + 0x3F) & ~((u32) 0x3F))

typedef struct {
    char path[FILE_PATH_MAX];   // On the SD card: "/roms/nds/game.nds".
    ndsfwd_game game;
    char name[128];
    u64 titleId;
    u32 iconTexture;

    Thread thread;
    volatile bool finished;
    Result result;
    const char* failure;        // A reason worth its own words (a literal), or NULL.
} ndsfwd_view;

static void nf_free(ndsfwd_view* v) {
    if(v->iconTexture != 0) {
        screen_unload_texture(v->iconTexture);
    }

    free(v);
}

static void nf_put16(u8* p, u16 v) {
    p[0] = (u8) (v >> 8);
    p[1] = (u8) v;
}

static void nf_put32(u8* p, u32 v) {
    p[0] = (u8) (v >> 24);
    p[1] = (u8) (v >> 16);
    p[2] = (u8) (v >> 8);
    p[3] = (u8) v;
}

static void nf_put64(u8* p, u64 v) {
    nf_put32(p, (u32) (v >> 32));
    nf_put32(p + 4, (u32) v);
}

static bool nf_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// ---- the template ----

static Result nf_read_file(const char* path, u8* buf, u32 max, u32* size) {
    FILE* file = fopen(path, "rb");
    if(file == NULL) {
        return R_APP_IO_ERROR;
    }

    *size = (u32) fread(buf, 1, max, file);
    bool more = fgetc(file) != EOF;
    fclose(file);
    return more ? R_APP_OUT_OF_RANGE : 0;
}

static Result nf_write_file(const char* path, const u8* buf, u32 size) {
    FILE* file = fopen(path, "wb");
    if(file == NULL) {
        return R_APP_IO_ERROR;
    }

    bool ok = fwrite(buf, 1, size, file) == size;
    ok &= fclose(file) == 0;
    return ok ? 0 : R_APP_IO_ERROR;
}

// The template SRL (`*srl`, malloc'd) and its description: from sdmc:/nsa/forwarder/, downloaded there the first time.
static Result nf_load_template(ndsfwd_view* v, u8** srl, u32* srlSize, ndsfwd_template* t) {
    u8* buf = (u8*) malloc(NF_TEMPLATE_MAX);
    char* fwd = (char*) calloc(1, NF_FWD_MAX + 1);
    if(buf == NULL || fwd == NULL) {
        free(buf);
        free(fwd);
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = 0;
    u32 fwdSize = 0;
    bool have = R_SUCCEEDED(nf_read_file(NF_TEMPLATE_SRL, buf, NF_TEMPLATE_MAX, srlSize))
                && R_SUCCEEDED(nf_read_file(NF_TEMPLATE_FWD, (u8*) fwd, NF_FWD_MAX, &fwdSize));
    if(have) {
        fwd[fwdSize] = '\0';
        have = ndsfwd_parse_template(fwd, t) && ndsfwd_check_template(buf, *srlSize, t);
    }

    if(!have) {
        u32 size = 0;
        memset(fwd, 0, NF_FWD_MAX + 1);
        if(R_FAILED(res = http_download_buffer(NF_TEMPLATE_URL ".fwd", &fwdSize, fwd, NF_FWD_MAX))
           || R_FAILED(res = http_download_buffer(NF_TEMPLATE_URL ".nds", &size, buf, NF_TEMPLATE_MAX))) {
            v->failure = "Could not download the forwarder template (olmectron.github.io). Check the internet connection.";
        } else if(!ndsfwd_parse_template(fwd, t) || !ndsfwd_check_template(buf, size, t)) {
            v->failure = "The downloaded forwarder template is not one NSA knows how to fill.";
            res = R_APP_BAD_DATA;
        } else {
            *srlSize = size;
            mkdir(NF_TEMPLATE_DIR, 0777);
            // Kept for the next time; a failure here only means downloading it again then.
            if(R_FAILED(nf_write_file(NF_TEMPLATE_SRL, buf, size)) || R_FAILED(nf_write_file(NF_TEMPLATE_FWD, (const u8*) fwd, fwdSize))) {
                remove(NF_TEMPLATE_SRL);
            }
        }
    }

    free(fwd);
    if(R_FAILED(res)) {
        free(buf);
        return res;
    }

    *srl = buf;
    return 0;
}

// ---- the CIA ----

// A CIA around the SRL: the public certificate chain, a ticket and a TMD made here without signatures, the SRL as its
// one content (DSiWare contents are not encrypted).
static u8* nf_make_cia(const u8* srl, u32 srlSize, u64 titleId, u32* ciaSize) {
    u32 certOffset = CIA_ALIGN(CIA_HEADER_SIZE);
    u32 ticketOffset = certOffset + CIA_ALIGN(CIA_CERT_SIZE);
    u32 tmdOffset = ticketOffset + CIA_ALIGN(CIA_TICKET_SIZE);
    u32 contentOffset = tmdOffset + CIA_ALIGN(CIA_TMD_SIZE);
    u32 size = contentOffset + CIA_ALIGN(srlSize);

    u8* cia = (u8*) calloc(1, size);
    if(cia == NULL) {
        return NULL;
    }

    // Header (little endian).
    u32 header[7] = {CIA_HEADER_SIZE, 0, CIA_CERT_SIZE, CIA_TICKET_SIZE, CIA_TMD_SIZE, 0, srlSize};
    memcpy(cia, &header[0], 4);
    memcpy(cia + 0x08, &header[2], 4);
    memcpy(cia + 0x0C, &header[3], 4);
    memcpy(cia + 0x10, &header[4], 4);
    memcpy(cia + 0x14, &header[5], 4);
    u64 contentSize = srlSize;
    memcpy(cia + 0x18, &contentSize, 8);
    cia[0x20] = 0x80;   // Content 0 is there.

    memcpy(cia + certOffset, CIA_CERT_CHAIN, CIA_CERT_SIZE);

    // Ticket.
    static const u8 contentIndex[0x2C] = {
        0x00, 0x01, 0x00, 0x14, 0x00, 0x00, 0x00, 0xAC, 0x00, 0x00, 0x00, 0x14, 0x00, 0x01, 0x00, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x84,
        0x00, 0x00, 0x00, 0x84, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    u8* tik = cia + ticketOffset;
    nf_put32(tik, 0x10004);
    memset(tik + 4, 0xFF, 0x100);
    u8* body = tik + 0x140;
    strcpy((char*) body, "Root-CA00000003-XS0000000c");
    body[0x7C] = 1;
    nf_put64(body + 0x9C, titleId);
    body[0xE1] = 1;
    memcpy(body + 0x164, contentIndex, sizeof(contentIndex));
    memset(body + 0x164 + sizeof(contentIndex), 0xFF, 0x80);

    // TMD: one content, its hash; save sizes and flags from the SRL's DSi header.
    u8* tmd = cia + tmdOffset;
    nf_put32(tmd, 0x10004);
    memset(tmd + 4, 0xFF, 0x100);
    u8* th = tmd + 0x140;
    strcpy((char*) th, "Root-CA00000003-CP0000000b");
    th[0x40] = 1;
    nf_put64(th + 0x4C, titleId);
    nf_put32(th + 0x54, 0x40);
    memcpy(th + 0x5A, srl + 0x238, 4);   // Public save data size (little endian, as the SRL has it).
    memcpy(th + 0x5E, srl + 0x23C, 4);   // Private save data size.
    th[0x66] = srl[0x1C];                // SRL flag.
    nf_put16(th + 0x9C, (u16) ((srl[0x1E] << 8) | srl[0x1D]));
    nf_put16(th + 0x9E, 1);

    u8* chunk = tmd + 0xB04;
    nf_put32(chunk, 0);
    nf_put16(chunk + 4, 0);
    nf_put16(chunk + 6, 0);
    nf_put64(chunk + 8, srlSize);
    mbedtls_sha256_ret(srl, srlSize, chunk + 0x10, 0);

    u8* infoRecords = tmd + 0x204;
    nf_put16(infoRecords + 2, 1);
    mbedtls_sha256_ret(chunk, 0x30, infoRecords + 4, 0);
    mbedtls_sha256_ret(infoRecords, 0x900, th + 0xA4, 0);

    memcpy(cia + contentOffset, srl, srlSize);
    *ciaSize = size;
    return cia;
}

static bool nf_installed(u64 titleId) {
    u32 count = 0;
    if(R_FAILED(AM_GetTitleCount(MEDIATYPE_NAND, &count)) || count == 0) {
        return false;
    }

    u64* ids = (u64*) calloc(count, sizeof(u64));
    if(ids == NULL) {
        return false;
    }

    u32 read = 0;
    bool found = false;
    if(R_SUCCEEDED(AM_GetTitleList(&read, MEDIATYPE_NAND, count, ids))) {
        for(u32 i = 0; i < read && !found; i++) {
            found = ids[i] == titleId;
        }
    }

    free(ids);
    return found;
}

static Result nf_install(ndsfwd_view* v) {
    u8* srl = NULL;
    u32 srlSize = 0;
    ndsfwd_template t;
    Result res = nf_load_template(v, &srl, &srlSize, &t);
    if(R_FAILED(res)) {
        return res;
    }

    ndsfwd_result made = ndsfwd_make_srl(srl, srlSize, &t, &v->game, v->path);
    if(made != NDSFWD_OK) {
        free(srl);
        v->failure = made == NDSFWD_PATH_TOO_LONG ? "The game's path is too long for a forwarder: move it to a shorter one."
                                                  : "The forwarder template is not one NSA knows how to fill.";
        return R_APP_BAD_DATA;
    }

    u64 titleId = ndsfwd_title_id(srl);
    u32 ciaSize = 0;
    u8* cia = nf_make_cia(srl, srlSize, titleId, &ciaSize);
    free(srl);
    if(cia == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    AM_TWLPartitionInfo info;
    if(R_SUCCEEDED(res = AM_GetTWLPartitionInfo(&info)) && info.titlesFreeSpace < ciaSize) {
        v->failure = "There is no room left for DSiWare on the console. Delete a DSiWare title or a forwarder first.";
        res = R_APP_OUT_OF_RANGE;
    }

    if(R_SUCCEEDED(res)) {
        Handle handle = 0;
        if(R_SUCCEEDED(res = AM_StartCiaInstall(MEDIATYPE_NAND, &handle))) {
            u32 written = 0;
            if(R_SUCCEEDED(res = FSFILE_Write(handle, &written, 0, cia, ciaSize, 0)) && written != ciaSize) {
                res = R_APP_IO_ERROR;
            }

            if(R_SUCCEEDED(res)) {
                res = AM_FinishCiaInstall(handle);
            } else {
                AM_CancelCIAInstall(handle);
            }
        }
    }

    free(cia);
    v->titleId = titleId;
    return res;
}

// ---- screens ----

static void nf_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    ndsfwd_view* v = (ndsfwd_view*) data;
    const float side = 96;
    float lineHeight = widget_text_height(WIDGET_TEXT_SCALE);
    float top = y1 + (y2 - y1 - side - 12 - lineHeight) / 2;
    float x = x1 + (x2 - x1 - side) / 2;
    if(v->iconTexture != 0) {
        screen_draw_texture(v->iconTexture, x, top, side, side);
    } else {
        screen_draw_icon(ICON_DEVICE_GAMEPAD_2, x + 16, top + 16, 64, theme()->palette->textMuted);
    }

    widget_text_centered(v->name, x1, x2, top + side + 12, WIDGET_TEXT_SCALE, theme()->palette->text);
}

static void nf_thread(void* arg) {
    ndsfwd_view* v = (ndsfwd_view*) arg;
    v->result = nf_install(v);
    v->finished = true;
}

static void nf_progress_update(ui_view* view, void* data, float* progress, char* text) {
    ndsfwd_view* v = (ndsfwd_view*) data;
    if(!v->finished) {
        return;
    }

    threadJoin(v->thread, U64_MAX);
    threadFree(v->thread);
    v->thread = NULL;

    ui_pop();
    info_destroy(view);

    if(R_SUCCEEDED(v->result)) {
        char done[400];
        snprintf(done, sizeof(done), _("\"%s\" is in the HOME Menu now. Keep the .nds where it is: the icon starts it from there with nds-bootstrap."), v->name);
        prompt_display_notify("Success", done, COLOR_TEXT, NULL, NULL, NULL);
    } else if(v->failure != NULL) {
        prompt_display_notify("Failure", v->failure, COLOR_TEXT, NULL, NULL, NULL);
    } else {
        error_display_res(NULL, NULL, v->result, "Failed to install the game in the HOME Menu.");
    }

    nf_free(v);
}

static void nf_start(ndsfwd_view* v) {
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    v->finished = false;
    v->failure = NULL;
    v->thread = threadCreate(nf_thread, v, NF_STACK, priority + 1, -2, false);
    if(v->thread == NULL) {
        error_display(NULL, NULL, "Failed to create the install thread.");
        nf_free(v);
        return;
    }

    info_display("Installing in the HOME Menu", "", false, v, nf_progress_update, nf_draw_top);
}

static void nf_pack_done(void* data, bool ok) {
    ndsfwd_view* v = (ndsfwd_view*) data;
    if(nf_exists(NF_PACK_FILE)) {
        nf_start(v);
        return;
    }

    if(ok) {
        prompt_display_notify("Failure", "The forwarder pack did not leave sd:/_nds/ntr-forwarder/sdcard.nds on the SD card.", COLOR_TEXT, NULL, NULL, NULL);
    }

    nf_free(v);
}

static void nf_confirmed(ui_view* view, void* data, u32 response) {
    ndsfwd_view* v = (ndsfwd_view*) data;
    if(response != PROMPT_YES) {
        nf_free(v);
        return;
    }

    if(!nf_exists(NF_PACK_FILE)) {
        storescript_run_now(nf_pack_script,
                            _("A game started from the HOME Menu needs nds-bootstrap's forwarder pack (sd:/_nds/ntr-forwarder), which is not on the SD card. Download it from GitHub (RocketRobz/NTR_Forwarder, about 2 MB) and put it there?"),
                            v, nf_pack_done);
        return;
    }

    nf_start(v);
}

// The game's name in NSA's language when the game has it.
static int nf_language(void) {
    switch(i18n_effective_language()) {
        case LANGUAGE_JAPANESE: return 0;
        case LANGUAGE_FRENCH: return 2;
        case LANGUAGE_GERMAN: return 3;
        case LANGUAGE_ITALIAN: return 4;
        case LANGUAGE_SPANISH:
        case LANGUAGE_CATALAN: return 5;
        case LANGUAGE_CHINESE_SIMPLIFIED:
        case LANGUAGE_CHINESE_TRADITIONAL: return 6;
        default: return 1;
    }
}

void ndsfwd_open(const char* path) {
    ndsfwd_view* v = (ndsfwd_view*) calloc(1, sizeof(ndsfwd_view));
    u8* header = (u8*) calloc(1, NDSFWD_HEADER_SIZE);
    u8* banner = (u8*) calloc(1, NDSFWD_BANNER_MAX);
    if(v == NULL || header == NULL || banner == NULL) {
        free(v);
        free(header);
        free(banner);
        error_display(NULL, NULL, "Failed to allocate program data.");
        return;
    }

    string_copy(v->path, path, sizeof(v->path));

    char sdPath[FILE_PATH_MAX + 8];
    snprintf(sdPath, sizeof(sdPath), "sdmc:%s", path);
    FILE* file = fopen(sdPath, "rb");
    ndsfwd_result read = NDSFWD_NOT_NDS;
    if(file != NULL) {
        if(fread(header, 1, NDSFWD_HEADER_SIZE, file) == NDSFWD_HEADER_SIZE) {
            u32 offset = ndsfwd_banner_offset(header);
            size_t bannerSize = 0;
            if(offset != 0 && fseek(file, (long) offset, SEEK_SET) == 0) {
                bannerSize = fread(banner, 1, NDSFWD_BANNER_MAX, file);
            }

            read = ndsfwd_read_game(header, banner, bannerSize, &v->game);
        }

        fclose(file);
    }

    free(header);
    free(banner);

    if(read != NDSFWD_OK) {
        const char* why = read == NDSFWD_NO_BANNER ? "This game has no icon and title, which a HOME Menu icon is made from."
                          : read == NDSFWD_BAD_BANNER ? "This game's icon and title are damaged."
                                                      : "This is not a DS game, or the file is damaged.";
        prompt_display_notify("Failure", why, COLOR_TEXT, NULL, NULL, NULL);
        free(v);
        return;
    }

    ndsfwd_name(&v->game, nf_language(), v->name, sizeof(v->name));
    v->titleId = ndsfwd_game_title_id(&v->game, v->path);

    // The icon as a texture, its bytes as the GPU reads RGBA8: A, B, G, R.
    u8 rgba[32 * 32 * 4];
    ndsfwd_icon_rgba(&v->game, rgba);
    for(u32 i = 0; i < 32 * 32; i++) {
        u8* p = rgba + i * 4;
        u8 r = p[0];
        u8 g = p[1];
        p[0] = p[3];
        p[1] = p[2];
        p[2] = g;
        p[3] = r;
    }

    v->iconTexture = screen_allocate_free_texture();
    if(v->iconTexture != 0) {
        screen_load_texture_untiled(v->iconTexture, rgba, sizeof(rgba), 32, 32, GPU_RGBA8, false);
    }

    const char* question = nf_installed(v->titleId)
                                   ? "This game already has an icon in the HOME Menu. Make it again from this file?"
                                   : "Put this game in the HOME Menu? The .nds stays where it is: the icon starts it from there with nds-bootstrap.";
    prompt_display_yes_no("Confirmation", question, COLOR_TEXT, v, nf_draw_top, nf_confirmed);
}
