#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <3ds.h>
#include <mbedtls/sha256.h>

#include "selftest.h"
#include "transfer.h"
#include "udslink.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"

#define SELFTEST_FAULTS_MAX 8
#define SELFTEST_POINT_MAX 32
#define SELFTEST_DIR APP_DATA_DIR "tests/"

// ---- faults ----

typedef struct {
    char point[SELFTEST_POINT_MAX];
    u32 skip;    // Hits that go through before the first failure.
    u32 count;   // Failures, then it goes through again.
    u32 hits;
    u32 fired;
} selftest_fault_entry;

static LightLock selftest_lock;
static selftest_fault_entry selftest_faults[SELFTEST_FAULTS_MAX];
static u32 selftest_fault_count = 0;

void selftest_init() {
    LightLock_Init(&selftest_lock);
}

#if NSA_DEBUG
bool selftest_fault(const char* point) {
    bool fail = false;

    LightLock_Lock(&selftest_lock);
    for(u32 i = 0; i < selftest_fault_count; i++) {
        selftest_fault_entry* f = &selftest_faults[i];
        if(strcmp(f->point, point) == 0) {
            f->hits++;
            if(f->hits > f->skip && f->fired < f->count) {
                f->fired++;
                fail = true;
            }
        }
    }

    LightLock_Unlock(&selftest_lock);

    if(fail) {
        udslink_log("selftest: fault %s fired\n", point);
    }

    return fail;
}
#endif

void selftest_fault_arm(const char* point, u32 skip, u32 count) {
    LightLock_Lock(&selftest_lock);

    selftest_fault_entry* f = NULL;
    for(u32 i = 0; i < selftest_fault_count && f == NULL; i++) {
        if(strcmp(selftest_faults[i].point, point) == 0) {
            f = &selftest_faults[i];
        }
    }

    if(f == NULL && selftest_fault_count < SELFTEST_FAULTS_MAX) {
        f = &selftest_faults[selftest_fault_count++];
    }

    if(f != NULL) {
        memset(f, 0, sizeof(*f));
        string_copy(f->point, point, sizeof(f->point));
        f->skip = skip;
        f->count = count;
    }

    LightLock_Unlock(&selftest_lock);
}

void selftest_fault_clear() {
    LightLock_Lock(&selftest_lock);
    selftest_fault_count = 0;
    LightLock_Unlock(&selftest_lock);
}

void selftest_fault_list(char* out, size_t size) {
    size_t used = 0;
    out[0] = '\0';

    LightLock_Lock(&selftest_lock);
    for(u32 i = 0; i < selftest_fault_count && used < size; i++) {
        const selftest_fault_entry* f = &selftest_faults[i];
        int n = snprintf(out + used, size - used, "%s%s %lu/%lu", used > 0 ? " " : "", f->point, f->hits, f->fired);
        if(n < 0) {
            break;
        }

        used += (size_t) n;
    }

    LightLock_Unlock(&selftest_lock);
}

// ---- files ----

bool selftest_sha256(const char* path, char* hex, size_t size) {
    if(size < 65) {
        return false;
    }

    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        return false;
    }

    u8* buffer = (u8*) malloc(64 * 1024);
    if(buffer == NULL) {
        fclose(f);
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);

    bool ok = true;
    size_t n = 0;
    while((n = fread(buffer, 1, 64 * 1024, f)) > 0) {
        mbedtls_sha256_update_ret(&sha, buffer, n);
    }

    if(ferror(f)) {
        ok = false;
    }

    u8 digest[32];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);
    free(buffer);
    fclose(f);

    for(u32 i = 0; i < sizeof(digest); i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }

    return ok;
}

#if NSA_DEBUG

// ---- what is on screen ----

typedef struct {
    char* out;
    size_t size;
    size_t used;
} selftest_json;

static void json_raw(selftest_json* j, const char* text) {
    while(*text != '\0' && j->used + 1 < j->size) {
        j->out[j->used++] = *text++;
    }

    j->out[j->used] = '\0';
}

// A JSON string, cut at `max` bytes; control characters become spaces (one line).
static void json_string(selftest_json* j, const char* text, size_t max) {
    if(text == NULL) {
        json_raw(j, "null");
        return;
    }

    json_raw(j, "\"");
    for(size_t i = 0; text[i] != '\0' && i < max; i++) {
        char c = text[i];
        char piece[3] = {c, '\0', '\0'};
        if(c == '"' || c == '\\') {
            piece[0] = '\\';
            piece[1] = c;
        } else if((u8) c < 0x20) {
            piece[0] = ' ';
        }

        json_raw(j, piece);
    }

    json_raw(j, "\"");
}

static void json_number(selftest_json* j, const char* format, ...) {
    char text[32];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    json_raw(j, text);
}

static const char* selftest_button_name(u32 button) {
    static const struct {
        u32 key;
        const char* name;
    } names[] = {
            {KEY_A, "A"}, {KEY_B, "B"}, {KEY_X, "X"}, {KEY_Y, "Y"}, {KEY_L, "L"}, {KEY_R, "R"},
            {KEY_START, "START"}, {KEY_SELECT, "SELECT"}, {PROMPT_BUTTON_ANY, "ANY"}
    };

    for(u32 i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if(names[i].key == button) {
            return names[i].name;
        }
    }

    return "?";
}

// {"depth":2,"name":"Confirmation","kind":"prompt","text":"Retry?","options":["Yes","No"],"buttons":["A","B"],...}
static void selftest_describe(char* reply, size_t size) {
    selftest_json j = {reply, size, 0};
    json_raw(&j, "200 {\"depth\":");
    json_number(&j, "%lu", ui_depth());

    ui_view* view = ui_top();
    ui_description d;
    memset(&d, 0, sizeof(d));
    d.kind = "other";
    d.progress = -1;
    d.selected = -1;
    if(view != NULL && !prompt_describe(view, &d) && !info_describe(view, &d)) {
        list_describe(view, &d);
    }

    json_raw(&j, ",\"name\":");
    json_string(&j, view != NULL ? (view->id != NULL ? view->id : view->name) : NULL, 80);
    json_raw(&j, ",\"kind\":");
    json_string(&j, d.kind, 16);
    json_raw(&j, ",\"text\":");
    json_string(&j, d.text, 200);

    json_raw(&j, ",\"options\":[");
    for(u32 i = 0; i < d.optionCount; i++) {
        json_raw(&j, i > 0 ? "," : "");
        json_string(&j, d.options[i], 40);
    }

    json_raw(&j, "],\"buttons\":[");
    for(u32 i = 0; i < d.optionCount; i++) {
        json_raw(&j, i > 0 ? "," : "");
        json_string(&j, selftest_button_name(d.buttons[i]), 8);
    }

    json_raw(&j, "],\"progress\":");
    json_number(&j, "%.3f", (double) d.progress);
    json_raw(&j, ",\"count\":");
    json_number(&j, "%lu", d.count);
    json_raw(&j, ",\"selected\":");
    json_number(&j, "%d", d.selected);
    json_raw(&j, ",\"selectedName\":");
    json_string(&j, d.selectedName, 80);
    json_raw(&j, "}");
}

// ---- commands ----

// The title an EXPORT or SENDTITLE works on; kept until the next one (the action
// uses it while it runs, and the tests run one at a time).
static list_item* selftest_title = NULL;

static list_item* selftest_load_title(const char* args) {
    char media[8] = "";
    unsigned long long titleId = 0;
    if(sscanf(args, "%llx %7s", &titleId, media) < 1) {
        return NULL;
    }

    if(selftest_title != NULL) {
        task_free_title(selftest_title);
        selftest_title = NULL;
    }

    FS_MediaType mediaType = strcasecmp(media, "nand") == 0 ? MEDIATYPE_NAND : MEDIATYPE_SD;
    selftest_title = task_title_item(mediaType, (u64) titleId);
    return selftest_title;
}

// The installed titles on the SD card, smallest first, in SELFTEST_DIR "titles.txt":
// "<title ID> <size> <version> <name>" per line.
static void selftest_list_titles(char* reply, size_t size) {
    u32 count = 0;
    u64* ids = NULL;
    if(R_FAILED(AM_GetTitleCount(MEDIATYPE_SD, &count)) || count == 0 || (ids = (u64*) calloc(count, sizeof(u64))) == NULL
       || R_FAILED(AM_GetTitleList(&count, MEDIATYPE_SD, count, ids))) {
        free(ids);
        snprintf(reply, size, "451 Could not list the titles.");
        return;
    }

    AM_TitleEntry* entries = (AM_TitleEntry*) calloc(count, sizeof(AM_TitleEntry));
    if(entries == NULL || R_FAILED(AM_GetTitleInfo(MEDIATYPE_SD, count, ids, entries))) {
        free(entries);
        free(ids);
        snprintf(reply, size, "451 Could not read the titles.");
        return;
    }

    // Smallest first: the tests take a small title.
    for(u32 i = 1; i < count; i++) {
        for(u32 k = i; k > 0 && entries[k].size < entries[k - 1].size; k--) {
            AM_TitleEntry swap = entries[k];
            entries[k] = entries[k - 1];
            entries[k - 1] = swap;
        }
    }

    mkdir("sdmc:" APP_DATA_DIR, 0777);
    mkdir("sdmc:" SELFTEST_DIR, 0777);
    FILE* f = fopen("sdmc:" SELFTEST_DIR "titles.txt", "w");
    if(f == NULL) {
        free(entries);
        free(ids);
        snprintf(reply, size, "451 Could not write " SELFTEST_DIR "titles.txt.");
        return;
    }

    for(u32 i = 0; i < count; i++) {
        meta_info meta;
        memset(&meta, 0, sizeof(meta));
        // Only the name: the icon texture is not wanted.
        char name[0x100] = "";
        if(task_title_load_meta(MEDIATYPE_SD, entries[i].titleID, &meta)) {
            string_copy(name, meta.shortDescription, sizeof(name));
            screen_unload_texture(meta.texture);
        }

        for(char* c = name; *c; c++) {
            if(*c == '\n' || *c == '\r') {
                *c = ' ';
            }
        }

        fprintf(f, "%016llX %llu %u %s\n", entries[i].titleID, entries[i].size, entries[i].version, name);
    }

    fclose(f);
    free(entries);
    free(ids);
    snprintf(reply, size, "200 " SELFTEST_DIR "titles.txt %lu", count);
}

static FS_Archive selftest_sd = 0;

static void selftest_send_file(const char* path, char* reply, size_t size) {
    if(selftest_sd == 0 && R_FAILED(FSUSER_OpenArchive(&selftest_sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        selftest_sd = 0;
        snprintf(reply, size, "451 Could not open the SD card.");
        return;
    }

    u64 fileSize = 0;
    Handle file = 0;
    FS_Path* fsPath = fs_make_path_utf8(path);
    Result res = fsPath != NULL ? FSUSER_OpenFile(&file, selftest_sd, *fsPath, FS_OPEN_READ, 0) : R_APP_OUT_OF_MEMORY;
    if(R_SUCCEEDED(res)) {
        res = FSFILE_GetSize(file, &fileSize);
        FSFILE_Close(file);
    }

    if(fsPath != NULL) {
        fs_free_path_utf8(fsPath);
    }

    if(R_FAILED(res)) {
        snprintf(reply, size, "451 Could not open %s: %08lX", path, res);
        return;
    }

    const char* name = strrchr(path, '/');
    transfer_send_file(selftest_sd, path, name != NULL ? name + 1 : path, fileSize);
    snprintf(reply, size, "200 Sending %s (%llu bytes).", path, fileSize);
}

#endif

void selftest_run(const char* command, char* reply, size_t size) {
#if NSA_DEBUG
    char verb[16] = "";
    const char* args = "";
    sscanf(command, "%15s", verb);
    const char* space = strchr(command, ' ');
    if(space != NULL) {
        args = space + 1;
    }

    for(char* c = verb; *c; c++) {
        if(*c >= 'a' && *c <= 'z') {
            *c = (char) (*c - 32);
        }
    }

    if(strcmp(verb, "UI") == 0) {
        selftest_describe(reply, size);
    } else if(strcmp(verb, "TITLES") == 0) {
        selftest_list_titles(reply, size);
    } else if(strcmp(verb, "EXPORT") == 0 || strcmp(verb, "SENDTITLE") == 0) {
        list_item* item = selftest_load_title(args);
        if(item == NULL) {
            snprintf(reply, size, "501 SITE TEST %s <title ID> [sd|nand]", verb);
            return;
        }

        if(verb[0] == 'E') {
            action_export_cia(NULL, item);
        } else {
            action_export_cia_send(NULL, item);
        }

        snprintf(reply, size, "200 %s %s.", verb, item->name);
    } else if(strcmp(verb, "SEND") == 0) {
        if(*args == '\0') {
            snprintf(reply, size, "501 SITE TEST SEND <path on the SD card>");
            return;
        }

        selftest_send_file(args, reply, size);
    } else if(strcmp(verb, "RECEIVE") == 0) {
        transfer_receive_open();
        snprintf(reply, size, "200 Receiving.");
    } else {
        snprintf(reply, size, "504 SITE TEST UI|TITLES|EXPORT|SENDTITLE|SEND|RECEIVE");
    }
#else
    (void) command;
    snprintf(reply, size, "502 Tests are for DEBUG builds.");
#endif
}
