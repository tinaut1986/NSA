#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>
#include <jansson.h>
#include <mbedtls/sha256.h>

#include "resources.h"
#include "section.h"
#include "../core/core.h"
#include "../core/sound.h"
#include "../core/xferturn.h"

// Utilities: what people used to open Luma3DS's Rosalina menu or GodMode9 for. The DSP firmware and Luma3DS's game
// patching are rows with their state on the right (A or a tap: the DSP firmware asks where it should come from, game
// patching changes at once; Left and Right turn the list's pages); restarting and turning off the console ask first.

// ---- the DSP firmware ----
//
// Homebrew plays sound through the DSP, and the DSP needs its firmware, which only the console has: HOME Menu carries it
// in its code. As DSP1 does (zoogie): read HOME Menu's .code from its ExeFS on the NAND, undo its compression (LZSS from
// the end, as ctrtool), find "DSP1" (the firmware starts 0x100 earlier, with its signature; the size follows the magic)
// and write it to /3ds/dspfirm.cdc. On top of DSP1, the sizes are checked and every segment's SHA-256 too, so nothing
// that is not a whole firmware lands on the SD card. (Rosalina reads the same bytes from HOME Menu's memory.)

#define DSP_DIR "sdmc:/3ds"
#define DSP_PATH "sdmc:/3ds/dspfirm.cdc"
#define DSP_TEMP_PATH "sdmc:/3ds/dspfirm.cdc.tmp"

#define DSP_MAGIC_OFFSET 0x100
#define DSP_SIZE_OFFSET 0x104
#define DSP_SEGMENT_COUNT_OFFSET 0x10D
#define DSP_SEGMENTS_OFFSET 0x120
#define DSP_SEGMENT_SIZE 0x30
#define DSP_SEGMENTS_MAX 10
// The DSP's memory (512 KiB) is the bound. Nintendo's firmware is 49756 bytes, but OpenDSP (an open source one, unsigned,
// that libctru's ndsp loads all the same) is over 200 KiB.
#define DSP_SIZE_MAX 0x80000
#define DSP_SIGNATURE_SIZE 0x100

static u32 utilities_le32(const u8* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24);
}

// A whole firmware at `firm`, `available` bytes readable from there: its size, else 0.
static u32 utilities_dsp_check(const u8* firm, u32 available) {
    if(available < DSP_SEGMENTS_OFFSET + DSP_SEGMENTS_MAX * DSP_SEGMENT_SIZE || memcmp(firm + DSP_MAGIC_OFFSET, "DSP1", 4) != 0) {
        return 0;
    }

    u32 size = utilities_le32(firm + DSP_SIZE_OFFSET);
    if(size < DSP_SEGMENTS_OFFSET + DSP_SEGMENTS_MAX * DSP_SEGMENT_SIZE || size > DSP_SIZE_MAX || size > available
       || firm[DSP_SEGMENT_COUNT_OFFSET] > DSP_SEGMENTS_MAX) {
        return 0;
    }

    u32 segments = 0;
    for(u32 i = 0; i < DSP_SEGMENTS_MAX; i++) {
        const u8* header = firm + DSP_SEGMENTS_OFFSET + i * DSP_SEGMENT_SIZE;
        u32 offset = utilities_le32(header);
        u32 length = utilities_le32(header + 8);
        if(length == 0) {
            break;
        }

        if(offset > size || length > size - offset) {
            return 0;
        }

        u8 hash[32];
        mbedtls_sha256_ret(firm + offset, length, hash, 0);
        if(memcmp(hash, header + 0x10, sizeof(hash)) != 0) {
            return 0;
        }

        segments++;
    }

    return segments > 0 ? size : 0;
}

// The firmware already on the SD card: 1 whole, 2 whole but unsigned (not Nintendo's: OpenDSP, shown so), 0 none, -1 there but
// not a whole firmware. `size` gets its size.
static int utilities_dsp_on_sd(u32* size) {
    *size = 0;
    FILE* f = fopen(DSP_PATH, "rb");
    if(f == NULL) {
        return 0;
    }

    int state = -1;
    u8* buffer = (u8*) malloc(DSP_SIZE_MAX);
    if(buffer != NULL) {
        u32 length = fread(buffer, 1, DSP_SIZE_MAX, f);
        u32 checked = utilities_dsp_check(buffer, length);
        if(checked != 0 && checked == length) {
            state = 1;
            // Nintendo's carries an RSA signature in its first 0x100 bytes; OpenDSP leaves them zero.
            bool signedFirm = false;
            for(u32 i = 0; i < DSP_SIGNATURE_SIZE && !signedFirm; i++) {
                signedFirm = buffer[i] != 0;
            }

            if(!signedFirm) {
                state = 2;
            }
        }

        *size = length;
        free(buffer);
    }

    fclose(f);
    return state;
}

// ctrtool's decompression of a code binary: the data is undone from its end, in place in `out` (`outSize` bytes, the
// compressed bytes copied at its start).
static bool utilities_lzss_decompress(const u8* in, u32 inSize, u8* out, u32 outSize) {
    if(inSize < 8) {
        return false;
    }

    u32 topAndBottom = utilities_le32(in + inSize - 8);
    u32 headerSize = (topAndBottom >> 24) & 0xFF;
    u32 compressed = topAndBottom & 0xFFFFFF;
    if(headerSize > inSize || compressed > inSize) {
        return false;
    }

    memset(out, 0, outSize);
    memcpy(out, in, inSize);

    u32 index = inSize - headerSize;
    u32 stop = inSize - compressed;
    u32 pos = outSize;
    while(index > stop) {
        u8 control = in[--index];
        for(u32 i = 0; i < 8; i++) {
            if(index <= stop || pos == 0) {
                break;
            }

            if(control & 0x80) {
                if(index < 2) {
                    return false;
                }

                index -= 2;
                u32 segmentOffset = in[index] | (in[index + 1] << 8);
                u32 segmentSize = ((segmentOffset >> 12) & 15) + 3;
                segmentOffset = (segmentOffset & 0x0FFF) + 2;
                if(pos < segmentSize) {
                    return false;
                }

                for(u32 j = 0; j < segmentSize; j++) {
                    if(pos + segmentOffset >= outSize) {
                        return false;
                    }

                    u8 value = out[pos + segmentOffset];
                    out[--pos] = value;
                }
            } else {
                if(pos < 1) {
                    return false;
                }

                out[--pos] = in[--index];
            }

            control <<= 1;
        }
    }

    return true;
}

// HOME Menu's title for each region (CFG_Region), DSP1's list.
static const u64 home_menu_ids[] = {
        0x0004003000008202ULL,   // JPN
        0x0004003000008F02ULL,   // USA
        0x0004003000009802ULL,   // EUR
        0x0004003000009802ULL,   // AUS
        0x000400300000A102ULL,   // CHN
        0x000400300000A902ULL,   // KOR
        0x000400300000B102ULL,   // TWN
};

#define COUNT_OF_HOME_MENUS (sizeof(home_menu_ids) / sizeof(*home_menu_ids))

static Result utilities_dsp_read_code(u64 titleId, u8** code, u32* size) {
    static const u32 filePath[5] = {0x00000000, 0x00000000, 0x00000002, 0x646F632E, 0x00000065};   // ExeFS ".code"
    u32 archivePath[4] = {(u32) (titleId & 0xFFFFFFFF), (u32) (titleId >> 32), MEDIATYPE_NAND, 0x00000000};

    Handle file = 0;
    Result res = FSUSER_OpenFileDirectly(&file, ARCHIVE_SAVEDATA_AND_CONTENT, fs_make_path_binary(archivePath, sizeof(archivePath)),
                                         fs_make_path_binary(filePath, sizeof(filePath)), FS_OPEN_READ, 0);
    if(R_FAILED(res)) {
        return res;
    }

    u64 fileSize = 0;
    if(R_SUCCEEDED(res = FSFILE_GetSize(file, &fileSize))) {
        if(fileSize < 8 || fileSize > 32 * 1024 * 1024) {
            res = R_APP_BAD_DATA;
        } else if((*code = (u8*) malloc((size_t) fileSize)) == NULL) {
            res = R_APP_OUT_OF_MEMORY;
        } else {
            u32 bytesRead = 0;
            if(R_SUCCEEDED(res = FSFILE_Read(file, &bytesRead, 0, *code, (u32) fileSize)) && bytesRead != fileSize) {
                res = R_APP_BAD_DATA;
            }

            if(R_FAILED(res)) {
                free(*code);
                *code = NULL;
            } else {
                *size = (u32) fileSize;
            }
        }
    }

    FSFILE_Close(file);
    return res;
}

// Finds a whole firmware in `data`; its start, else NULL.
static const u8* utilities_dsp_find(const u8* data, u32 size, u32* firmSize) {
    for(u32 i = DSP_MAGIC_OFFSET; i + 4 <= size; i += 4) {
        if(memcmp(data + i, "DSP1", 4) == 0) {
            const u8* firm = data + i - DSP_MAGIC_OFFSET;
            u32 checked = utilities_dsp_check(firm, size - (i - DSP_MAGIC_OFFSET));
            if(checked != 0) {
                *firmSize = checked;
                return firm;
            }
        }
    }

    return NULL;
}

// HOME Menu's code of `titleId`, undone if compressed; the firmware found in it goes to `out` (malloc'd).
static Result utilities_dsp_from_title(u64 titleId, u8** out, u32* outSize) {
    u8* code = NULL;
    u32 codeSize = 0;
    Result res = utilities_dsp_read_code(titleId, &code, &codeSize);
    if(R_FAILED(res)) {
        return res;
    }

    u32 firmSize = 0;
    const u8* firm = utilities_dsp_find(code, codeSize, &firmSize);
    u8* plain = NULL;
    if(firm == NULL) {
        // The footer's last word is how much bigger the code is once undone.
        u32 extra = utilities_le32(code + codeSize - 4);
        if(extra > 0 && extra <= 32 * 1024 * 1024 && (plain = (u8*) malloc(codeSize + extra)) != NULL) {
            if(utilities_lzss_decompress(code, codeSize, plain, codeSize + extra)) {
                firm = utilities_dsp_find(plain, codeSize + extra, &firmSize);
            }
        }
    }

    if(firm == NULL) {
        res = R_APP_BAD_DATA;
    } else if((*out = (u8*) malloc(firmSize)) == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    } else {
        memcpy(*out, firm, firmSize);
        *outSize = firmSize;
    }

    free(plain);
    free(code);
    return res;
}

// A whole firmware to /3ds/dspfirm.cdc: into a file of its own first, so the old one stays until the new one is whole.
static Result utilities_dsp_write(const u8* firm, u32 size) {
    mkdir(DSP_DIR, 0777);
    FILE* f = fopen(DSP_TEMP_PATH, "wb");
    if(f == NULL) {
        return R_APP_BAD_DATA;
    }

    bool written = fwrite(firm, 1, size, f) == size;
    written = fclose(f) == 0 && written;
    if(!written) {
        remove(DSP_TEMP_PATH);
        return R_APP_BAD_DATA;
    }

    remove(DSP_PATH);
    if(rename(DSP_TEMP_PATH, DSP_PATH) != 0) {
        return R_APP_BAD_DATA;
    }

    sound_dsp_firmware_added();
    return 0;
}

static Result utilities_dsp_dump(u32* size) {
    u8 region = 0xFF;
    CFGU_SecureInfoGetRegion(&region);

    u8* firm = NULL;
    Result res = R_APP_BAD_DATA;
    // The console's own region first, then the others (a console whose HOME Menu is another region's); the error told
    // is the own region's.
    if(region < COUNT_OF_HOME_MENUS) {
        res = utilities_dsp_from_title(home_menu_ids[region], &firm, size);
    }

    for(u32 i = 0; i < COUNT_OF_HOME_MENUS && firm == NULL; i++) {
        if(i != region && home_menu_ids[i] != (region < COUNT_OF_HOME_MENUS ? home_menu_ids[region] : 0)) {
            Result tried = utilities_dsp_from_title(home_menu_ids[i], &firm, size);
            if(region >= COUNT_OF_HOME_MENUS && tried != (Result) R_APP_BAD_DATA) {
                res = tried;
            }
        }
    }

    if(firm == NULL) {
        return R_FAILED(res) ? res : R_APP_BAD_DATA;
    }

    res = utilities_dsp_write(firm, *size);
    free(firm);
    return res;
}

// The row's state, worked out when Utilities opens and after each change (it hashes the file: not every frame).
static int utilities_dsp_state = -2;   // utilities_dsp_on_sd's, or -2: not known yet.

static void utilities_dsp_delete_answered(ui_view* view, void* data, u32 response) {
    if(response != PROMPT_YES) {
        return;
    }

    if(remove(DSP_PATH) != 0) {
        error_display(NULL, NULL, _("Failed to delete /3ds/dspfirm.cdc."));
    }

    utilities_dsp_state = -2;
}

const char* utilities_dsp_value() {
    if(utilities_dsp_state == -2) {
        u32 size = 0;
        utilities_dsp_state = utilities_dsp_on_sd(&size);
    }

    return utilities_dsp_state == 2 ? "OpenDSP" : utilities_dsp_state > 0 ? _("On") : utilities_dsp_state < 0 ? _("Damaged") : _("Off");
}

// ---- OpenDSP ----
//
// OpenDSP (Cellenseres, BSD-2-Clause: romfs:/opendsp/LICENSE.txt) is an open source DSP firmware that libctru's ndsp
// loads as it loads Nintendo's, for homebrew that only uses what it plays. For a console whose HOME Menu cannot be
// dumped, or for whoever prefers it. The latest release comes from GitHub, checked against that release's
// SHA256SUMS.txt and as any firmware; without internet (or while another transfer holds the turn) NSA's own copy goes
// in, without asking: whoever chose OpenDSP wants it in place, not to pick where it comes from.

#define OPENDSP_RELEASE_URL "https://api.github.com/repos/Cellenseres/OpenDSP/releases/latest"
#define OPENDSP_RELEASE_MAX (256 * 1024)
#define OPENDSP_SUMS_MAX (16 * 1024)
#define OPENDSP_FILE "opendsp.cdc"
#define OPENDSP_SUMS_FILE "SHA256SUMS.txt"
#define OPENDSP_ROMFS "romfs:/opendsp/opendsp.cdc"
#define OPENDSP_ROMFS_VERSION "romfs:/opendsp/version.txt"
#define OPENDSP_STACK 0x8000

typedef struct {
    Thread thread;
    volatile bool finished;
    Result result;
    u8* firm;              // DSP_SIZE_MAX bytes; written on the UI thread once whole (sound_dsp_firmware_added).
    u32 size;
    bool bundled;          // NSA's own copy (the download failed).
    char version[32];
} opendsp_job;

static int utilities_hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// The SHA-256 that SHA256SUMS.txt gives for opendsp.cdc (lines "<64 hex digits>  <name>", sha256sum's).
static bool utilities_opendsp_sum(const char* sums, u8* hash) {
    const char* line = sums;
    while(*line != '\0') {
        bool hex = true;
        for(u32 i = 0; i < 64 && hex; i++) {
            hex = utilities_hex(line[i]) >= 0;
        }

        if(hex) {
            const char* name = line + 64;
            while(*name == ' ' || *name == '*') {
                name++;
            }

            size_t length = strlen(OPENDSP_FILE);
            if(strncmp(name, OPENDSP_FILE, length) == 0 && (name[length] == '\0' || name[length] == '\r' || name[length] == '\n')) {
                for(u32 i = 0; i < 32; i++) {
                    hash[i] = (u8) ((utilities_hex(line[i * 2]) << 4) | utilities_hex(line[i * 2 + 1]));
                }

                return true;
            }
        }

        const char* next = strchr(line, '\n');
        if(next == NULL) {
            break;
        }

        line = next + 1;
    }

    return false;
}

static Result utilities_opendsp_download(opendsp_job* job) {
    u32 turn = xferturn_take(XFER_DOWNLOAD, OPENDSP_FILE, 0);
    if(turn == 0) {
        return R_APP_BAD_DATA;
    }

    char cdcUrl[512] = "";
    char sumsUrl[512] = "";
    json_t* release = NULL;
    Result res = http_download_json(OPENDSP_RELEASE_URL, &release, OPENDSP_RELEASE_MAX);
    if(R_SUCCEEDED(res)) {
        json_t* tag = json_object_get(release, "tag_name");
        json_t* assets = json_object_get(release, "assets");
        if(json_is_string(tag)) {
            string_copy(job->version, json_string_value(tag), sizeof(job->version));
        }

        if(json_is_array(assets)) {
            size_t i = 0;
            json_t* asset = NULL;
            json_array_foreach(assets, i, asset) {
                json_t* name = json_object_get(asset, "name");
                json_t* url = json_object_get(asset, "browser_download_url");
                if(json_is_string(name) && json_is_string(url)) {
                    if(strcmp(json_string_value(name), OPENDSP_FILE) == 0) {
                        string_copy(cdcUrl, json_string_value(url), sizeof(cdcUrl));
                    } else if(strcmp(json_string_value(name), OPENDSP_SUMS_FILE) == 0) {
                        string_copy(sumsUrl, json_string_value(url), sizeof(sumsUrl));
                    }
                }
            }
        }

        // `tag` lives in `release`: asked before it goes.
        if(!json_is_string(tag) || cdcUrl[0] == '\0' || sumsUrl[0] == '\0') {
            res = R_APP_BAD_DATA;
        }

        json_decref(release);
    }

    u8 expected[32];
    if(R_SUCCEEDED(res)) {
        char* sums = (char*) calloc(1, OPENDSP_SUMS_MAX + 1);
        u32 length = 0;
        if(sums == NULL) {
            res = R_APP_OUT_OF_MEMORY;
        } else if(R_SUCCEEDED(res = http_download_buffer(sumsUrl, &length, sums, OPENDSP_SUMS_MAX)) && !utilities_opendsp_sum(sums, expected)) {
            res = R_APP_BAD_DATA;
        }

        free(sums);
    }

    xferturn_progress(turn, 0);
    if(R_SUCCEEDED(res) && R_SUCCEEDED(res = http_download_buffer(cdcUrl, &job->size, job->firm, DSP_SIZE_MAX))) {
        // A file bigger than the buffer comes cut at DSP_SIZE_MAX: its hash tells.
        u8 hash[32];
        mbedtls_sha256_ret(job->firm, job->size, hash, 0);
        if(memcmp(hash, expected, sizeof(hash)) != 0 || utilities_dsp_check(job->firm, job->size) != job->size) {
            res = R_APP_BAD_DATA;
        }
    }

    xferturn_release(turn);
    return res;
}

static Result utilities_opendsp_bundled(opendsp_job* job) {
    FILE* f = fopen(OPENDSP_ROMFS, "rb");
    if(f == NULL) {
        return R_APP_BAD_DATA;
    }

    job->size = fread(job->firm, 1, DSP_SIZE_MAX, f);
    fclose(f);
    if(utilities_dsp_check(job->firm, job->size) != job->size) {
        return R_APP_BAD_DATA;
    }

    job->version[0] = '\0';
    if((f = fopen(OPENDSP_ROMFS_VERSION, "rb")) != NULL) {
        size_t length = fread(job->version, 1, sizeof(job->version) - 1, f);
        fclose(f);
        while(length > 0 && (job->version[length - 1] == '\n' || job->version[length - 1] == '\r' || job->version[length - 1] == ' ')) {
            length--;
        }

        job->version[length] = '\0';
    }

    return 0;
}

static void utilities_opendsp_thread(void* arg) {
    opendsp_job* job = (opendsp_job*) arg;
    if(R_FAILED(utilities_opendsp_download(job))) {
        job->bundled = true;
        job->result = utilities_opendsp_bundled(job);
    }

    job->finished = true;
}

static void utilities_opendsp_free(opendsp_job* job) {
    free(job->firm);
    free(job);
}

static void utilities_opendsp_update(ui_view* view, void* data, float* progress, char* text) {
    opendsp_job* job = (opendsp_job*) data;
    if(!job->finished) {
        return;
    }

    threadJoin(job->thread, U64_MAX);
    threadFree(job->thread);
    ui_pop();
    info_destroy(view);

    Result res = job->result;
    if(R_SUCCEEDED(res)) {
        res = utilities_dsp_write(job->firm, job->size);
    }

    utilities_dsp_state = -2;
    if(R_SUCCEEDED(res)) {
        static char done[256];
        snprintf(done, sizeof(done), job->bundled ? _("OpenDSP %s is now /3ds/dspfirm.cdc. It could not be downloaded, so it is the copy that came with NSA.")
                                                  : _("OpenDSP %s is now /3ds/dspfirm.cdc."),
                 job->version);
        prompt_display_notify("Success", done, COLOR_TEXT, NULL, NULL, NULL);
    } else {
        error_display_res(NULL, NULL, res, _("Failed to put OpenDSP in /3ds/dspfirm.cdc."));
    }

    utilities_opendsp_free(job);
}

static void utilities_opendsp_start(void* data) {
    opendsp_job* job = (opendsp_job*) calloc(1, sizeof(opendsp_job));
    if(job == NULL || (job->firm = (u8*) malloc(DSP_SIZE_MAX)) == NULL) {
        free(job);
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("Failed to put OpenDSP in /3ds/dspfirm.cdc."));
        return;
    }

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    if((job->thread = threadCreate(utilities_opendsp_thread, job, OPENDSP_STACK, priority + 1, -2, false)) == NULL) {
        utilities_opendsp_free(job);
        error_display(NULL, NULL, "Failed to create the install thread.");
        return;
    }

    info_display("Installing OpenDSP", "", false, job, utilities_opendsp_update, NULL);
}

// ---- the row ----

static void utilities_dsp_dump_failed_answered(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        utilities_opendsp_start(NULL);
    }
}

static void utilities_dsp_choose_dump(void* data) {
    u32 size = 0;
    Result res = utilities_dsp_dump(&size);
    utilities_dsp_state = -2;
    if(R_FAILED(res)) {
        // No HOME Menu to dump it from (a damaged NAND...): OpenDSP is what is left.
        static char text[384];
        snprintf(text, sizeof(text), "%s\n\n%s (%08lX)", _("Could not dump the DSP firmware from HOME Menu. Put OpenDSP, an open source firmware, instead?"),
                 _("Error"), res);
        prompt_display_yes_no(_("DSP firmware"), text, COLOR_TEXT, NULL, NULL, utilities_dsp_dump_failed_answered);
    }
}

static void utilities_dsp_choose_delete(void* data) {
    prompt_display_warning(_("DSP firmware"), _("Delete /3ds/dspfirm.cdc? Homebrew will have no sound until it is dumped again."), COLOR_TEXT, NULL, NULL,
                           utilities_dsp_delete_answered);
}

static void utilities_dsp_nothing(void* data) {
}

// Where the firmware comes from (the console's own, or OpenDSP), and Delete when there is a file.
void utilities_dsp_toggle() {
    utilities_dsp_value();
    static sheet_action actions[3];
    actions[0] = (sheet_action) {ICON_CPU, _("Dump the console's"), false, utilities_dsp_choose_dump, false,
                                 _("Nintendo's, from HOME Menu. The usual choice.")};
    actions[1] = (sheet_action) {ICON_DOWNLOAD, "OpenDSP", false, utilities_opendsp_start, false,
                                 _("Open source, from GitHub. Does not do everything.")};
    actions[2] = (sheet_action) {ICON_TRASH, _("Delete"), true, utilities_dsp_choose_delete, false, _("Homebrew will have no sound.")};
    sheet_display_choice(_("DSP firmware"), actions, utilities_dsp_state != 0 ? 3 : 2, NULL, NULL, utilities_dsp_nothing);
    sheet_set_id("DSP firmware");
}

// ---- Luma3DS ----
//
// Luma3DS 13 and later keep their options in /luma/config.ini and read it at boot. Game patching (LayeredFS mods in
// /luma/titles, IPS patches, locale emulation) is "enable_game_patching = 0|1" in [boot]. Turning it on or off changes
// that one character in place: the rest of the file stays as Luma wrote it. It takes effect at the next boot.

#define LUMA_CONFIG_PATH "sdmc:/luma/config.ini"
#define LUMA_GAME_PATCHING "enable_game_patching"

// The offset of the value of `key` in config.ini (one '0' or '1'), or -1.
static long utilities_luma_find(const char* key, char* value) {
    FILE* f = fopen(LUMA_CONFIG_PATH, "rb");
    if(f == NULL) {
        return -1;
    }

    long found = -1;
    char line[256];
    long lineStart = ftell(f);
    size_t keyLength = strlen(key);
    while(found < 0 && fgets(line, sizeof(line), f) != NULL) {
        const char* c = line;
        while(*c == ' ' || *c == '\t') {
            c++;
        }

        if(strncmp(c, key, keyLength) == 0) {
            const char* v = c + keyLength;
            while(*v == ' ' || *v == '\t') {
                v++;
            }

            if(*v == '=') {
                v++;
                while(*v == ' ' || *v == '\t') {
                    v++;
                }

                const char* end = v + 1;
                if((*v == '0' || *v == '1') && (*end == '\r' || *end == '\n' || *end == '\0' || *end == ' ' || *end == ';')) {
                    *value = *v;
                    found = lineStart + (long) (v - line);
                }
            }
        }

        lineStart = ftell(f);
    }

    fclose(f);
    return found;
}

static bool utilities_luma_set(long offset, char value) {
    FILE* f = fopen(LUMA_CONFIG_PATH, "r+b");
    if(f == NULL) {
        return false;
    }

    bool ok = fseek(f, offset, SEEK_SET) == 0 && fputc(value, f) == value;
    ok = fclose(f) == 0 && ok;
    return ok;
}

static void utilities_restart_answered(ui_view* view, void* data, u32 response);

static int utilities_luma_state = -2;   // 1 on, 0 off, -1 no config.ini (or no such line), -2 not read yet.

const char* utilities_luma_value() {
    if(utilities_luma_state == -2) {
        char value = 0;
        utilities_luma_state = utilities_luma_find(LUMA_GAME_PATCHING, &value) < 0 ? -1 : value == '1';
    }

    return utilities_luma_state > 0 ? _("On") : utilities_luma_state == 0 ? _("Off") : _("Not found");
}

// Turns it the other way at once, then offers to restart (Luma reads it at boot).
void utilities_luma_toggle() {
    char value = 0;
    long offset = utilities_luma_find(LUMA_GAME_PATCHING, &value);
    if(offset < 0) {
        utilities_luma_state = -1;
        prompt_display_notify(_("Luma3DS"), _("Luma3DS's options were not found (/luma/config.ini). Luma3DS 13 or later writes that file; with an older one, change them in its menu (hold SELECT while the console starts)."),
                              COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    char wanted = value == '1' ? '0' : '1';
    utilities_luma_state = -2;
    if(!utilities_luma_set(offset, wanted)) {
        error_display(NULL, NULL, _("Failed to change Luma3DS's /luma/config.ini."));
        return;
    }

    prompt_display_yes_no(_("Game patching"), wanted == '1' ? _("Game patching will be on from the next start of the console.\n\nRestart it now?")
                                                            : _("Game patching will be off from the next start of the console.\n\nRestart it now?"),
                          COLOR_TEXT, NULL, NULL, utilities_restart_answered);
}

// Utilities opened: read both states again (the SD card may have changed meanwhile).
void utilities_refresh() {
    utilities_dsp_state = -2;
    utilities_luma_state = -2;
}

// ---- restart and power off ----
//
// As Rosalina's power menu: a restart is APT's hardware reset; turning off is the POWER button's own request (0x203),
// which HOME Menu carries out as usual. Both close NSA first. Not while a transfer moves data.

static bool utilities_busy() {
    char busy[128];
    if(!xferturn_describe(busy, sizeof(busy))) {
        return false;
    }

    static char text[256];
    snprintf(text, sizeof(text), "%s\n\n%s", _("Wait until the transfer ends."), busy);
    prompt_display_notify(_("Busy"), text, COLOR_TEXT, NULL, NULL, NULL);
    return true;
}

static void utilities_restart_answered(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES && !utilities_busy()) {
        APT_HardwareResetAsync();
    }
}

static void utilities_power_off_answered(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES && !utilities_busy()) {
        srvPublishToSubscriber(0x203, 0);
    }
}

void utilities_restart_open() {
    if(!utilities_busy()) {
        prompt_display_yes_no(_("Restart"), _("Restart the console? NSA closes first."), COLOR_TEXT, NULL, NULL, utilities_restart_answered);
    }
}

void utilities_power_off_open() {
    if(!utilities_busy()) {
        prompt_display_yes_no(_("Power off"), _("Turn the console off? NSA closes first."), COLOR_TEXT, NULL, NULL, utilities_power_off_answered);
    }
}
