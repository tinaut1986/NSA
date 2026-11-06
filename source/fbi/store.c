#include <ctype.h>
#include <dirent.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <3ds.h>
#include <jansson.h>

#include "resources.h"
#include "section.h"
#include "settings.h"
#include "storescript.h"
#include "thumbs.h"
#include "action/action.h"
#include "../core/core.h"
#include "../core/task/dataop.h"

// Store: the homebrew of Universal-DB (db.universal-team.net), the catalogue Universal-Updater reads, without leaving
// NSA. An app that publishes a CIA is downloaded and installed, one that publishes a .3dsx is downloaded into /3ds/.
//
// Opening it is quick because the list comes from a small copy kept on the SD card (`store-cache.json`, only the fields
// the list needs); meanwhile a thread asks the server for the catalogue's revision (a few hundred bytes) and only when it
// changed downloads the whole catalogue (about 2 MB), boils it down, saves the new copy and hands it to the screen, which
// swaps it in. Each app's icon is a small PNG somewhere else: a second thread fetches the ones the rows show, keeps them
// in `sdmc:/nsa/store-icons/` and the previews of `thumbs.c` turn them into textures.

#define STORE_INFO_URL "https://db.universal-team.net/unistore/universal-db-info.json"
#define STORE_FULL_URL "https://db.universal-team.net/unistore/universal-db.unistore"
// The icons of every app, already shrunk, in two sprite sheets (Tex3DS) next to the catalogue: two downloads instead of one per app.
#define STORE_SHEET_URL "https://db.universal-team.net/unistore/universal-db-%d.t3x"
#define STORE_SHEET_MAX (2 * 1024 * 1024)
#define STORE_PACK_PATH "sdmc:" APP_DATA_DIR "store-icons.pack"
#define STORE_SHEETS_MARK "sdmc:" APP_DATA_DIR "store-icons/sheets.rev"
#define STORE_CACHE_PATH "sdmc:" APP_DATA_DIR "store-cache.json"
#define STORE_OLD_CACHE_PATH "sdmc:" APP_DATA_DIR "store.unistore"
#define STORE_ICON_DIR "sdmc:" APP_DATA_DIR "store-icons"
#define STORE_ICON_PATH "sdmc:" APP_DATA_DIR "store-icons/%s.png"
#define STORE_ICON_ARCHIVE_PATH APP_DATA_DIR "store-icons/%s.png"
// The icon already shrunk to STORE_ICON_SIDE pixels square, as the raw pixels the GPU wants: reading these 9 KB takes a few
// milliseconds, where decoding the PNG (which may be much bigger than it looks) took about 300 ms each time.
#define STORE_ICON_RAW_PATH "sdmc:" APP_DATA_DIR "store-icons/%s.t48"
#define STORE_FULL_MAX (3 * 1024 * 1024)
#define STORE_INFO_MAX 8192
#define STORE_ICON_MAX (512 * 1024)
#define STORE_ICON_QUEUE 24
#define STORE_LOG_PATH "sdmc:" APP_DATA_DIR "store-log.txt"   // DEBUG builds: how long the Store takes to do things.
#define STORE_ICON_SIDE 48   // The icons are kept as textures this many pixels square, for as long as the Store is open.
#define STORE_PREFETCH_PAUSE 4000   // ms between the icons fetched in the background, when no row is waiting for one.
#define STORE_INSTALLED_PATH "sdmc:" APP_DATA_DIR "store-installed.json"
#define STORE_RECORDS_MAX 512
#define STORE_CACHE_FORMAT 5   // Raised when the copy on the SD card gains fields: the old one is downloaded again.

typedef enum {
    ICON_NONE,      // Not asked for yet.
    ICON_QUEUED,    // The icon thread has it.
    ICON_READY,     // On the SD card: `thumbs.c` makes the texture.
    ICON_FAILED     // No icon (no link, too big, unreachable): the generic one.
} icon_state;

typedef struct {
    char title[80];
    char author[48];
    char version[24];
    char categoryId[24];   // As Universal-DB names it ("game", "utility"...).
    char description[300];
    char slug[64];
    char* ciaUrl;
    char ciaSize[16];
    char* dsxUrl;
    char dsxSize[16];
    // Universal-DB's steps for each (storescript.h, as JSON text): NULL for a plain CIA, which installs as it downloads.
    char* ciaScript;
    char* dsxScript;
    char dsxName[64];   // The .3dsx it leaves, without the extension, to look for it in /3ds/.
    char* iconUrl;      // Where the icon is: "sheet:index" in the catalogue's sprite sheets.
    char* preMessage;   // What the author asks to be read before installing (NULL: nothing).
    u32 stars;
    u32 uniqueId;       // The title's unique ID (a CIA's), or 0 when the catalogue has none.
    char updated[12];   // "2026-03-19".
    u8 category;        // An index of store_categories.
    u8 iconState;
    u32 iconFileSize;
    u32 texture;        // The icon, once made (kept until the Store closes, so scrolling never loses it); 0 until then.
} store_app;

typedef struct {
    char slug[64];
    char* url;
    bool decode;   // Not used any more: a job always ends with the icon's pixels.
    u64 requested; // When a row asked for it (ms).
} store_icon_job;

typedef struct {
    char slug[64];
    u8 state;
    u32 size;
    bool decoded;   // The answer to a job: `pixels` (NULL when it failed).
    u8* pixels;

    // How long each step took, for the log.
    u64 requested;
    u32 waitMs;
    u32 statMs;
    u32 downloadMs;
    u32 decodeMs;
    u32 fileSize;
    bool downloaded;
} store_icon_result;

// What the Store itself installed or downloaded, and which version of the catalogue it was. It only tells whether an
// app with no unique ID in the catalogue is installed: it cannot tell an update is waiting, as an app installed any
// other way, or updated by its own updater, leaves it behind (and the console's version number is not the catalogue's).
typedef struct {
    char slug[64];
    char cia[24];
    char dsx[24];
} store_record;

// What the background scan needs about an app (copied, so the scan never reads the apps that the screen owns).
typedef struct {
    char slug[64];
    u32 uniqueId;
    bool hasCia;
    bool hasDsx;
    char dsxName[96];
    char recCia[24];     // The Store installed its CIA (an app without a unique ID).
} store_scan_entry;

// Bits of an app's status.
#define STATUS_CIA 1           // The title is installed.
#define STATUS_DSX 2           // The .3dsx is in /3ds/.

// An icon nobody has asked for yet: fetched slowly in the background so that scrolling finds it there.
typedef struct {
    char slug[64];
    char* url;
} store_prefetch_entry;

typedef struct {
    store_app* apps;
    u32 count;

    // From the loading thread to the screen: a catalogue ready to be swapped in.
    store_app* pending;
    u32 pendingCount;
    volatile int handoff;   // 1: `pending` is waiting for the screen, which sets it back to 0.
    volatile bool offline;  // The server could not be asked: what is shown is the copy on the SD card.
    volatile bool done;     // The loading thread has finished.
    Result res;
    bool noticeShown;

    // Who still uses this: the screen, the loading thread and the icon thread. The last one frees it.
    LightLock lock;
    int users;
    volatile bool cancelled;
    bool started;
    bool haveCatalogue;
    bool iconThread;

    FS_Archive archive;     // The SD card, for the previews.
    store_icon_job jobs[STORE_ICON_QUEUE];
    u32 jobCount;
    store_icon_result results[STORE_ICON_QUEUE];
    u32 resultCount;
    store_prefetch_entry* prefetch;
    u32 prefetchCount;
    u32 prefetchNext;

    // What is on the console, found slowly in the background (`status` goes with `apps`, one byte each).
    store_scan_entry* scan;
    u8* status;
    u32 scanCount;
    u32 scanNext;
    u32 generation;
    store_record records[STORE_RECORDS_MAX];
    u32 recordCount;

    ui_view* view;
    linked_list* items;

    // The log (DEBUG builds): lines wait here and the background thread writes them out.
    char logBuf[6144];
    u32 logLen;
    u64 opened;
    u32 outstanding;     // Icons asked for and not yet there.
    volatile int revision;        // The catalogue's revision now in use (the icons are made from the sheets of that one).
    volatile u32 lastRequestMs;   // When a row last asked for an icon (osGetTime, low 32 bits).
    u64 batchStart;
    u32 batchCount;

    // What shows and in which order (kept in settings.json).
    u32 kinds;        // Bits: 1 CIA, 2 3DSX.
    u32 categories;   // One bit per store_categories entry.
    u32 shown;        // 0 all, 1 what is on the console, 2 what is not.
    u32 sort;
    bool resort;
} store_data;

static void store_logf(store_data* store, const char* format, ...) {
    if(!NSA_DEBUG) {
        return;
    }

    char line[256];
    int n = snprintf(line, sizeof(line), "[%7llu] ", (unsigned long long) (osGetTime() - store->opened));
    va_list args;
    va_start(args, format);
    vsnprintf(line + n, sizeof(line) - n, format, args);
    va_end(args);

    LightLock_Lock(&store->lock);
    size_t len = strlen(line);
    if(store->logLen + len + 1 < sizeof(store->logBuf)) {
        memcpy(store->logBuf + store->logLen, line, len);
        store->logBuf[store->logLen + len] = '\n';
        store->logLen += len + 1;
    }

    LightLock_Unlock(&store->lock);
}

// Writes the waiting lines to the file (never from the screen's thread while it draws: the SD card is slow).
static void store_log_flush(store_data* store) {
    if(!NSA_DEBUG) {
        return;
    }

    char text[sizeof(store->logBuf)];
    LightLock_Lock(&store->lock);
    u32 len = store->logLen;
    memcpy(text, store->logBuf, len);
    store->logLen = 0;
    LightLock_Unlock(&store->lock);

    if(len > 0) {
        FILE* file = fopen(STORE_LOG_PATH, "ab");
        if(file != NULL) {
            fwrite(text, 1, len, file);
            fclose(file);
        }
    }
}

static void store_free_apps(store_app* apps, u32 count) {
    if(apps == NULL) {
        return;
    }

    for(u32 i = 0; i < count; i++) {
        free(apps[i].ciaUrl);
        free(apps[i].dsxUrl);
        free(apps[i].ciaScript);
        free(apps[i].dsxScript);
        free(apps[i].iconUrl);
        free(apps[i].preMessage);
    }

    free(apps);
}

static void store_release(store_data* store) {
    LightLock_Lock(&store->lock);
    bool last = --store->users == 0;
    LightLock_Unlock(&store->lock);

    if(last) {
        store_free_apps(store->apps, store->count);
        store_free_apps(store->pending, store->pendingCount);
        for(u32 i = 0; i < store->jobCount; i++) {
            free(store->jobs[i].url);
        }

        for(u32 i = 0; i < store->prefetchCount; i++) {
            free(store->prefetch[i].url);
        }

        for(u32 i = 0; i < store->resultCount; i++) {
            free(store->results[i].pixels);
        }

        free(store->prefetch);
        free(store->scan);
        free(store->status);

        if(store->archive != 0) {
            fs_close_archive(store->archive);
        }

        free(store);
    }
}

// ---- the catalogue: from the server's JSON, and from the copy on the SD card ----

static bool store_has_ext(const char* name, const char* ext) {
    size_t n = strlen(name);
    size_t e = strlen(ext);
    if(n < e) {
        return false;
    }

    for(size_t i = 0; i < e; i++) {
        if(tolower((unsigned char) name[n - e + i]) != ext[i]) {
            return false;
        }
    }

    return true;
}

static const char* store_str(json_t* object, const char* key) {
    json_t* value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : "";
}

static bool store_https(const char* url) {
    return strncmp(url, "https://", 8) == 0 && strlen(url) < DOWNLOAD_URL_MAX;
}

static int store_compare(const void* a, const void* b) {
    return strcasecmp(((const store_app*) a)->title, ((const store_app*) b)->title);
}

// The categories of Universal-DB, as the filter groups them: the ones it has (in this order) and "Other" for the rest.
#define STORE_CATEGORY_COUNT 8
static const char* const store_category_ids[STORE_CATEGORY_COUNT - 1] = {"game", "app", "utility", "emulator", "media", "save-tool", "plugin"};

static u8 store_category_index(const char* id) {
    for(u32 i = 0; i < STORE_CATEGORY_COUNT - 1; i++) {
        if(strcmp(id, store_category_ids[i]) == 0) {
            return (u8) i;
        }
    }

    return STORE_CATEGORY_COUNT - 1;
}

static void store_make_slug(char* out, size_t size, const char* text) {
    size_t n = 0;
    for(; *text != '\0' && n < size - 1; text++) {
        out[n++] = isalnum((unsigned char) *text) || *text == '-' || *text == '_' ? *text : '-';
    }

    out[n] = '\0';
}

// The apps for 3DS that publish a CIA or a .3dsx, from the catalogue (Universal-DB's .unistore, the file Universal-Updater reads).
static int store_parse_full(const char* text, size_t size, store_app** out) {
    json_error_t error;
    json_t* root = json_loadb(text, size, 0, &error);
    json_t* content = root != NULL ? json_object_get(root, "storeContent") : NULL;
    if(!json_is_array(content)) {
        json_decref(root);
        return -1;
    }

    store_app* apps = (store_app*) calloc(json_array_size(content) + 1, sizeof(store_app));
    if(apps == NULL) {
        json_decref(root);
        return -1;
    }

    u32 count = 0;
    for(size_t i = 0; i < json_array_size(content); i++) {
        json_t* entry = json_array_get(content, i);
        json_t* info = json_is_object(entry) ? json_object_get(entry, "info") : NULL;
        if(!json_is_object(info)) {
            continue;
        }

        bool for3ds = false;
        size_t index;
        json_t* value;
        json_array_foreach(json_object_get(info, "console"), index, value) {
            if(json_is_string(value) && strcasecmp(json_string_value(value), "3DS") == 0) {
                for3ds = true;
            }
        }

        if(!for3ds) {
            continue;
        }

        store_app* app = &apps[count];
        const char* key;
        json_t* file;
        json_object_foreach(entry, key, file) {
            if(strcmp(key, "info") == 0 || !json_is_object(file)) {
                continue;
            }

            // Only the steps NSA can follow (storescript.h): downloads, .zip and .7z, CIAs, moves and deletes.
            json_t* script = json_object_get(file, "script");
            if(!storescript_supported(script)) {
                continue;
            }

            const char* url = storescript_first_url(script);
            if(!store_https(url)) {
                continue;
            }

            if(store_has_ext(key, ".cia") && app->ciaUrl == NULL) {
                app->ciaUrl = strdup(url);
                string_copy(app->ciaSize, store_str(file, "size"), sizeof(app->ciaSize));
                if(!storescript_plain_cia(script)) {
                    app->ciaScript = json_dumps(script, JSON_COMPACT);
                }
            } else if(store_has_ext(key, ".3dsx") && app->dsxUrl == NULL) {
                app->dsxUrl = strdup(url);
                string_copy(app->dsxSize, store_str(file, "size"), sizeof(app->dsxSize));
                app->dsxScript = json_dumps(script, JSON_COMPACT);
                storescript_dsx_name(script, app->dsxName, sizeof(app->dsxName));
            }
        }

        string_copy(app->title, store_str(info, "title"), sizeof(app->title));
        if((app->ciaUrl == NULL && app->dsxUrl == NULL) || app->title[0] == '\0') {
            free(app->ciaUrl);
            free(app->dsxUrl);
            free(app->ciaScript);
            free(app->dsxScript);
            memset(app, 0, sizeof(*app));
            continue;
        }

        string_copy(app->author, store_str(info, "author"), sizeof(app->author));
        string_copy(app->version, store_str(info, "version"), sizeof(app->version));
        string_copy(app->description, store_str(info, "description"), sizeof(app->description));
        store_make_slug(app->slug, sizeof(app->slug), app->title);

        json_t* categories = json_object_get(info, "category");
        if(json_is_array(categories) && json_array_size(categories) > 0 && json_is_string(json_array_get(categories, 0))) {
            string_copy(app->categoryId, json_string_value(json_array_get(categories, 0)), sizeof(app->categoryId));
        }

        app->category = store_category_index(app->categoryId);
        json_t* stars = json_object_get(info, "stars");
        app->stars = json_is_integer(stars) && json_integer_value(stars) > 0 ? (u32) json_integer_value(stars) : 0;
        string_copy(app->updated, store_str(info, "last_updated"), 11);   // "2026-03-19 at 21:43 (UTC)".
        json_t* ids = json_object_get(info, "title_ids");
        if(json_is_array(ids) && json_array_size(ids) > 0 && json_is_integer(json_array_get(ids, 0))) {
            json_int_t id = json_integer_value(json_array_get(ids, 0));
            app->uniqueId = (u32) (id > 0xFFFFFF ? (id >> 8) & 0xFFFFFF : id);
        }

        json_t* sheet = json_object_get(info, "sheet_index");
        json_t* position = json_object_get(info, "icon_index");
        if(json_is_integer(sheet) && json_is_integer(position) && json_integer_value(sheet) >= 0 && json_integer_value(position) >= 0) {
            char where[24];
            snprintf(where, sizeof(where), "%d:%d", (int) json_integer_value(sheet), (int) json_integer_value(position));
            app->iconUrl = strdup(where);
        }

        const char* message = store_str(info, "preinstall_message");
        if(message[0] != '\0') {
            app->preMessage = strdup(message);
        }

        count++;
    }

    json_decref(root);
    qsort(apps, count, sizeof(store_app), store_compare);
    *out = apps;
    return (int) count;
}

static json_t* store_app_to_json(const store_app* app) {
    json_t* object = json_object();
    json_object_set_new(object, "t", json_string(app->title));
    json_object_set_new(object, "a", json_string(app->author));
    json_object_set_new(object, "v", json_string(app->version));
    json_object_set_new(object, "c", json_string(app->categoryId));
    json_object_set_new(object, "st", json_integer(app->stars));
    json_object_set_new(object, "id", json_integer(app->uniqueId));
    json_object_set_new(object, "u", json_string(app->updated));
    json_object_set_new(object, "d", json_string(app->description));
    json_object_set_new(object, "s", json_string(app->slug));
    if(app->ciaUrl != NULL) {
        json_object_set_new(object, "cu", json_string(app->ciaUrl));
        json_object_set_new(object, "cs", json_string(app->ciaSize));
    }

    if(app->dsxUrl != NULL) {
        json_object_set_new(object, "du", json_string(app->dsxUrl));
        json_object_set_new(object, "ds", json_string(app->dsxSize));
        json_object_set_new(object, "dn", json_string(app->dsxName));
    }

    if(app->ciaScript != NULL) {
        json_object_set_new(object, "cx", json_string(app->ciaScript));
    }

    if(app->dsxScript != NULL) {
        json_object_set_new(object, "dx", json_string(app->dsxScript));
    }

    if(app->iconUrl != NULL) {
        json_object_set_new(object, "i", json_string(app->iconUrl));
    }

    if(app->preMessage != NULL) {
        json_object_set_new(object, "pm", json_string(app->preMessage));
    }

    return object;
}

static void store_write_cache(const store_app* apps, u32 count, int revision) {
    json_t* list = json_array();
    for(u32 i = 0; i < count; i++) {
        json_array_append_new(list, store_app_to_json(&apps[i]));
    }

    json_t* root = json_pack("{s:i,s:i,s:o}", "format", STORE_CACHE_FORMAT, "revision", revision, "apps", list);
    if(root != NULL) {
        json_dump_file(root, STORE_CACHE_PATH, JSON_COMPACT);
        json_decref(root);
    }
}

// The copy on the SD card; -1 when there is none or it is not readable.
static int store_read_cache(store_app** out, int* revision) {
    json_error_t error;
    json_t* root = json_load_file(STORE_CACHE_PATH, 0, &error);
    json_t* list = root != NULL ? json_object_get(root, "apps") : NULL;
    json_t* format = root != NULL ? json_object_get(root, "format") : NULL;
    if(!json_is_array(list) || !json_is_integer(format) || json_integer_value(format) != STORE_CACHE_FORMAT) {
        json_decref(root);
        return -1;
    }

    store_app* apps = (store_app*) calloc(json_array_size(list) + 1, sizeof(store_app));
    if(apps == NULL) {
        json_decref(root);
        return -1;
    }

    u32 count = 0;
    for(size_t i = 0; i < json_array_size(list); i++) {
        json_t* entry = json_array_get(list, i);
        store_app* app = &apps[count];
        string_copy(app->title, store_str(entry, "t"), sizeof(app->title));
        if(!json_is_object(entry) || app->title[0] == '\0') {
            continue;
        }

        string_copy(app->author, store_str(entry, "a"), sizeof(app->author));
        string_copy(app->version, store_str(entry, "v"), sizeof(app->version));
        string_copy(app->categoryId, store_str(entry, "c"), sizeof(app->categoryId));
        app->category = store_category_index(app->categoryId);
        json_t* stars = json_object_get(entry, "st");
        app->stars = json_is_integer(stars) && json_integer_value(stars) > 0 ? (u32) json_integer_value(stars) : 0;
        string_copy(app->updated, store_str(entry, "u"), sizeof(app->updated));
        json_t* uniqueId = json_object_get(entry, "id");
        app->uniqueId = json_is_integer(uniqueId) && json_integer_value(uniqueId) > 0 ? (u32) json_integer_value(uniqueId) : 0;
        string_copy(app->description, store_str(entry, "d"), sizeof(app->description));
        string_copy(app->slug, store_str(entry, "s"), sizeof(app->slug));
        if(store_str(entry, "cu")[0] != '\0') {
            app->ciaUrl = strdup(store_str(entry, "cu"));
            string_copy(app->ciaSize, store_str(entry, "cs"), sizeof(app->ciaSize));
        }

        if(store_str(entry, "du")[0] != '\0') {
            app->dsxUrl = strdup(store_str(entry, "du"));
            string_copy(app->dsxSize, store_str(entry, "ds"), sizeof(app->dsxSize));
            string_copy(app->dsxName, store_str(entry, "dn"), sizeof(app->dsxName));
        }

        if(store_str(entry, "cx")[0] != '\0') {
            app->ciaScript = strdup(store_str(entry, "cx"));
        }

        if(store_str(entry, "dx")[0] != '\0') {
            app->dsxScript = strdup(store_str(entry, "dx"));
        }

        if(store_str(entry, "i")[0] != '\0') {
            app->iconUrl = strdup(store_str(entry, "i"));
        }

        if(store_str(entry, "pm")[0] != '\0') {
            app->preMessage = strdup(store_str(entry, "pm"));
        }

        count++;
    }

    json_t* rev = json_object_get(root, "revision");
    *revision = json_is_integer(rev) ? (int) json_integer_value(rev) : -1;
    json_decref(root);
    *out = apps;
    return (int) count;
}

// ---- the loading thread ----

// Gives the screen a catalogue and waits for it to be taken (the one before may still be waiting).
static void store_hand_off(store_data* store, store_app* apps, u32 count) {
    while(store->handoff != 0 && !store->cancelled) {
        svcSleepThread(10 * 1000 * 1000LL);
    }

    if(store->cancelled) {
        store_free_apps(apps, count);
        return;
    }

    store->pending = apps;
    store->pendingCount = count;
    store->handoff = 1;
}

// The catalogue's revision from the server, or -1.
static int store_server_revision() {
    char* text = (char*) malloc(STORE_INFO_MAX);
    if(text == NULL) {
        return -1;
    }

    int revision = -1;
    u32 size = 0;
    if(R_SUCCEEDED(http_download_buffer(STORE_INFO_URL, &size, text, STORE_INFO_MAX)) && size < STORE_INFO_MAX) {
        json_error_t error;
        json_t* root = json_loadb(text, size, 0, &error);
        json_t* rev = root != NULL ? json_object_get(root, "revision") : NULL;
        if(json_is_integer(rev)) {
            revision = (int) json_integer_value(rev);
        }

        json_decref(root);
    }

    free(text);
    return revision;
}

static void store_load_thread(void* arg) {
    store_data* store = (store_data*) arg;

    // The copy on the SD card first: the list can show at once.
    store_app* cached = NULL;
    int cachedRevision = -1;
    u64 loadStart = osGetTime();
    int cachedCount = store_read_cache(&cached, &cachedRevision);
    store_logf(store, "copy on the SD card: %d apps read in %u ms", cachedCount, (u32) (osGetTime() - loadStart));
    bool haveCache = cachedCount > 0;
    if(haveCache) {
        store->revision = cachedRevision;
        store_hand_off(store, cached, (u32) cachedCount);
    } else {
        store_free_apps(cached, cachedCount > 0 ? (u32) cachedCount : 0);
    }

    // Has the catalogue changed? A few hundred bytes tell.
    u64 revisionStart = osGetTime();
    int revision = store->cancelled ? -1 : store_server_revision();
    store_logf(store, "revision %d (the copy has %d), asked in %u ms", revision, cachedRevision, (u32) (osGetTime() - revisionStart));
    if(store->cancelled) {
        store_release(store);
        return;
    }

    if(revision < 0) {
        // Not reachable: the copy has to do (and with none there is nothing to show).
        store->offline = true;
        store->res = haveCache ? 0 : R_APP_HTTP_ERROR_BASE;
    } else if(!haveCache || revision != cachedRevision) {
        char* text = (char*) malloc(STORE_FULL_MAX);
        if(text == NULL) {
            store->res = haveCache ? 0 : R_APP_OUT_OF_MEMORY;
        } else {
            u32 size = 0;
            Result res = http_download_buffer(STORE_FULL_URL, &size, text, STORE_FULL_MAX);
            store_app* apps = NULL;
            int count = R_SUCCEEDED(res) && size < STORE_FULL_MAX ? store_parse_full(text, size, &apps) : -1;
            free(text);

            store_logf(store, "catalogue: %d apps downloaded and read in %u ms", count, (u32) (osGetTime() - revisionStart));
            if(count >= 0) {
                store_write_cache(apps, (u32) count, revision);
                store->revision = revision;
                store_hand_off(store, apps, (u32) count);
                store->res = 0;
            } else {
                store->offline = true;
                store->res = haveCache ? 0 : (R_FAILED(res) ? res : R_APP_PARSE_FAILED);
            }
        }
    }

    store->done = true;
    store_release(store);
}

// ---- what is on this console ----

static store_record* store_record_find(store_data* store, const char* slug) {
    for(u32 i = 0; i < store->recordCount; i++) {
        if(strcmp(store->records[i].slug, slug) == 0) {
            return &store->records[i];
        }
    }

    return NULL;
}

static void store_records_load(store_data* store) {
    json_error_t error;
    json_t* root = json_load_file(STORE_INSTALLED_PATH, 0, &error);
    if(!json_is_object(root)) {
        json_decref(root);
        return;
    }

    const char* key;
    json_t* value;
    json_object_foreach(root, key, value) {
        if(store->recordCount < STORE_RECORDS_MAX && json_is_object(value)) {
            store_record* record = &store->records[store->recordCount++];
            string_copy(record->slug, key, sizeof(record->slug));
            string_copy(record->cia, store_str(value, "c"), sizeof(record->cia));
            string_copy(record->dsx, store_str(value, "d"), sizeof(record->dsx));
        }
    }

    json_decref(root);
}

static void store_records_save(store_data* store) {
    json_t* root = json_object();
    for(u32 i = 0; i < store->recordCount; i++) {
        json_t* entry = json_object();
        if(store->records[i].cia[0] != '\0') {
            json_object_set_new(entry, "c", json_string(store->records[i].cia));
        }

        if(store->records[i].dsx[0] != '\0') {
            json_object_set_new(entry, "d", json_string(store->records[i].dsx));
        }

        json_object_set_new(root, store->records[i].slug, entry);
    }

    json_dump_file(root, STORE_INSTALLED_PATH, JSON_COMPACT);
    json_decref(root);
}

// The name a .3dsx is saved under: the last part of its link, without the extension.
static void store_dsx_name(const char* url, char* out, size_t size) {
    const char* start = strrchr(url, '/');
    start = start != NULL ? start + 1 : url;
    size_t len = 0;
    while(start[len] != '\0' && start[len] != '?' && start[len] != '#' && len < size - 1) {
        out[len] = start[len];
        len++;
    }

    out[len] = '\0';
    if(len >= 5 && strcasecmp(out + len - 5, ".3dsx") == 0) {
        out[len - 5] = '\0';
    }
}

static bool store_title_installed(u32 uniqueId) {
    u64 titleId = (0x00040000ULL << 32) | ((u64) uniqueId << 8);
    AM_TitleEntry info;
    return R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &titleId, &info)) || R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_NAND, 1, &titleId, &info));
}

static bool store_file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// What is on the console for one app.
static int store_compare_ids(const void* a, const void* b) {
    u64 x = *(const u64*) a;
    u64 y = *(const u64*) b;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Every title installed (SD card and system memory), sorted: two calls to AM instead of one per app. NULL when it fails.
static u64* store_load_installed(u32* count) {
    u32 sd = 0;
    u32 nand = 0;
    AM_GetTitleCount(MEDIATYPE_SD, &sd);
    AM_GetTitleCount(MEDIATYPE_NAND, &nand);
    u64* ids = (u64*) malloc((sd + nand + 1) * sizeof(u64));
    if(ids == NULL) {
        return NULL;
    }

    u32 total = 0;
    u32 read = 0;
    if(sd > 0 && R_SUCCEEDED(AM_GetTitleList(&read, MEDIATYPE_SD, sd, ids))) {
        total += read;
    }

    if(nand > 0 && R_SUCCEEDED(AM_GetTitleList(&read, MEDIATYPE_NAND, nand, ids + total))) {
        total += read;
    }

    qsort(ids, total, sizeof(u64), store_compare_ids);
    *count = total;
    return ids;
}

// `installed` is the sorted list of titles (NULL: ask AM about this one title).
static u8 store_compute_status(const store_scan_entry* entry, const u64* installed, u32 installedCount) {
    u8 bits = 0;
    if(entry->hasCia) {
        // Without a unique ID, what the Store installed is all there is to go by.
        u64 titleId = (0x00040000ULL << 32) | ((u64) entry->uniqueId << 8);
        bool installedHere = entry->uniqueId != 0 ? (installed != NULL ? bsearch(&titleId, installed, installedCount, sizeof(u64), store_compare_ids) != NULL
                                                                        : store_title_installed(entry->uniqueId))
                                                  : entry->recCia[0] != '\0';
        if(installedHere) {
            bits |= STATUS_CIA;
        }
    }

    if(entry->hasDsx && entry->dsxName[0] != '\0') {
        char path[FILE_PATH_MAX];
        snprintf(path, sizeof(path), "sdmc:/3ds/%s/%s.3dsx", entry->dsxName, entry->dsxName);
        bool present = store_file_exists(path);
        if(!present) {
            snprintf(path, sizeof(path), "sdmc:/3ds/%s.3dsx", entry->dsxName);
            present = store_file_exists(path);
        }

        if(present) {
            bits |= STATUS_DSX;
        }
    }

    return bits;
}

// A few apps' status, for the background thread to call now and then (`installed` is its list of titles).
static void store_scan_step(store_data* store, const u64* installed, u32 installedCount) {
    for(int n = 0; n < 16; n++) {
        store_scan_entry entry;
        u32 index = 0;
        u32 generation = 0;
        LightLock_Lock(&store->lock);
        bool have = store->scanNext < store->scanCount;
        if(have) {
            index = store->scanNext++;
            entry = store->scan[index];
            generation = store->generation;
        }

        LightLock_Unlock(&store->lock);
        if(!have) {
            return;
        }

        u8 bits = store_compute_status(&entry, installed, installedCount);
        LightLock_Lock(&store->lock);
        if(generation == store->generation && store->status != NULL && index < store->scanCount) {
            store->status[index] = bits | 0x80;
        }

        LightLock_Unlock(&store->lock);
    }
}

// ---- the icon thread ----

// The pixels of an icon (NULL when it could not be made) for the screen to turn into a texture, with how long it took.
static void store_push_pixels(store_data* store, const store_icon_result* info, u8* pixels) {
    LightLock_Lock(&store->lock);
    if(store->resultCount < STORE_ICON_QUEUE) {
        store_icon_result* r = &store->results[store->resultCount++];
        *r = *info;
        r->state = pixels != NULL ? ICON_READY : ICON_FAILED;
        r->decoded = true;
        r->pixels = pixels;
        pixels = NULL;
    }

    LightLock_Unlock(&store->lock);
    free(pixels);
}

// ---- the shrunk icons kept on the SD card ----

// All the shrunk icons in one file (a name of 64 bytes, then the pixels, for each): one read brings them all.
#define STORE_PACK_RECORD (64 + STORE_ICON_SIDE * STORE_ICON_SIDE * 4)

static u8* store_pack_load(u32* count) {
    *count = 0;
    FILE* file = fopen(STORE_PACK_PATH, "rb");
    if(file == NULL) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    u8* data = size >= STORE_PACK_RECORD ? (u8*) malloc((size_t) size) : NULL;
    if(data != NULL && fread(data, 1, (size_t) size, file) != (size_t) size) {
        free(data);
        data = NULL;
    }

    fclose(file);
    if(data != NULL) {
        *count = (u32) size / STORE_PACK_RECORD;
    }

    return data;
}

// A copy of an icon's pixels from the loaded pack, or NULL.
static u8* store_pack_get(const u8* pack, u32 count, const char* slug) {
    for(u32 i = 0; pack != NULL && i < count; i++) {
        const u8* record = pack + (size_t) i * STORE_PACK_RECORD;
        if(strncmp((const char*) record, slug, 64) == 0) {
            u8* pixels = (u8*) malloc(STORE_ICON_SIDE * STORE_ICON_SIDE * 4);
            if(pixels != NULL) {
                memcpy(pixels, record + 64, STORE_ICON_SIDE * STORE_ICON_SIDE * 4);
            }

            return pixels;
        }
    }

    return NULL;
}

// ---- the sprite sheets ----

typedef struct {
    char slug[64];
    int sheet;
    int index;
} store_sheet_icon;

static int store_read_marker(void) {
    int revision = -1;
    FILE* file = fopen(STORE_SHEETS_MARK, "r");
    if(file != NULL) {
        if(fscanf(file, "%d", &revision) != 1) {
            revision = -1;
        }

        fclose(file);
    }

    return revision;
}

static void store_write_marker(int revision) {
    FILE* file = fopen(STORE_SHEETS_MARK, "w");
    if(file != NULL) {
        fprintf(file, "%d\n", revision);
        fclose(file);
    }
}

// The offset of a pixel inside an 8x8 tile of a GPU texture (bits of x and y interleaved).
static u32 store_morton(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// One icon cut out of a sheet's texture, as STORE_ICON_SIDE square raw pixels (NULL when it cannot be).
static u8* store_cut_icon(const C3D_Tex* tex, const Tex3DS_SubTexture* sub) {
    if(tex->fmt != GPU_RGBA8 || Tex3DS_SubTextureRotated(sub) || sub->width == 0 || sub->height == 0) {
        return NULL;
    }

    int width = tex->width;
    int height = tex->height;
    int left = (int) (sub->left * width + 0.5f);
    int top = height - (int) (sub->top * height + 0.5f);   // The coordinates count up from the bottom; the memory's first row is the picture's top.
    u8* out = (u8*) malloc((size_t) STORE_ICON_SIDE * STORE_ICON_SIDE * 4);
    if(out == NULL) {
        return NULL;
    }

    const u8* data = (const u8*) tex->data;
    for(int dy = 0; dy < STORE_ICON_SIDE; dy++) {
        int y = top + dy * sub->height / STORE_ICON_SIDE;
        for(int dx = 0; dx < STORE_ICON_SIDE; dx++) {
            int x = left + dx * sub->width / STORE_ICON_SIDE;
            u8* dst = out + ((size_t) dy * STORE_ICON_SIDE + dx) * 4;
            if(x < 0 || y < 0 || x >= width || y >= height) {
                memset(dst, 0, 4);
                continue;
            }

            const u8* src = data + (((size_t) (y / 8) * (width / 8) + x / 8) * 64 + store_morton(x & 7, y & 7)) * 4;
            memcpy(dst, src, 4);
        }
    }

    return out;
}

// Downloads the catalogue's sheets and keeps every app's icon as its small raw copy on the SD card. False when a sheet failed.
static bool store_build_from_sheets(store_data* store, int revision) {
    LightLock_Lock(&store->lock);
    u32 total = store->prefetchCount;
    store_sheet_icon* icons = (store_sheet_icon*) calloc(total + 1, sizeof(store_sheet_icon));
    u32 listed = 0;
    int sheets = 0;
    for(u32 i = 0; icons != NULL && i < total; i++) {
        int sheet = 0;
        int index = 0;
        if(sscanf(store->prefetch[i].url, "%d:%d", &sheet, &index) == 2 && sheet >= 0) {
            string_copy(icons[listed].slug, store->prefetch[i].slug, sizeof(icons[listed].slug));
            icons[listed].sheet = sheet;
            icons[listed].index = index;
            listed++;
            if(sheet + 1 > sheets) {
                sheets = sheet + 1;
            }
        }
    }

    LightLock_Unlock(&store->lock);

    u8* data = (u8*) malloc(STORE_SHEET_MAX);
    u8* pack = (u8*) calloc(listed + 1, STORE_PACK_RECORD);
    bool ok = icons != NULL && data != NULL && pack != NULL && listed > 0;
    u32 made = 0;
    u32 made_total = 0;
    for(int sheet = 0; ok && sheet < sheets && !store->cancelled; sheet++) {
        char url[128];
        snprintf(url, sizeof(url), STORE_SHEET_URL, sheet);
        u64 t = osGetTime();
        u32 size = 0;
        Result res = http_download_buffer(url, &size, data, STORE_SHEET_MAX);
        store_logf(store, "sheet %d: %u B downloaded in %u ms (%08lX)", sheet, size, (u32) (osGetTime() - t), res);
        if(R_FAILED(res) || size == 0 || size >= STORE_SHEET_MAX) {
            ok = false;
            break;
        }

        t = osGetTime();
        C3D_Tex tex;
        Tex3DS_Texture sheetInfo = Tex3DS_TextureImport(data, size, &tex, NULL, false);
        if(sheetInfo == NULL) {
            store_logf(store, "sheet %d: could not be read", sheet);
            ok = false;
            break;
        }

        store_logf(store, "sheet %d: %dx%d format %d read in %u ms", sheet, (int) tex.width, (int) tex.height, (int) tex.fmt, (u32) (osGetTime() - t));
        t = osGetTime();
        u32 cut = 0;
        u32 skipped = 0;
        for(u32 i = 0; i < listed; i++) {
            if(icons[i].sheet != sheet) {
                continue;
            }

            const Tex3DS_SubTexture* sub = icons[i].index < (int) Tex3DS_GetNumSubTextures(sheetInfo) ? Tex3DS_GetSubTexture(sheetInfo, icons[i].index) : NULL;
            u8* pixels = sub != NULL ? store_cut_icon(&tex, sub) : NULL;
            if(pixels != NULL) {
                u8* record = pack + (size_t) made_total * STORE_PACK_RECORD;
                string_copy((char*) record, icons[i].slug, 64);
                memcpy(record + 64, pixels, STORE_ICON_SIDE * STORE_ICON_SIDE * 4);
                made_total++;
                free(pixels);
                cut++;
            } else {
                skipped++;
            }
        }

        store_logf(store, "sheet %d: %u icons kept (%u skipped) in %u ms", sheet, cut, skipped, (u32) (osGetTime() - t));
        made += cut;
        Tex3DS_TextureFree(sheetInfo);
        C3D_TexDelete(&tex);
    }

    if(ok && made > 0) {
        u64 t = osGetTime();
        FILE* file = fopen(STORE_PACK_PATH, "wb");
        bool written = file != NULL && fwrite(pack, STORE_PACK_RECORD, made_total, file) == made_total;
        if(file != NULL) {
            fclose(file);
        }

        store_logf(store, "pack of %u icons written in %u ms (%s)", made_total, (u32) (osGetTime() - t), written ? "ok" : "FAILED");
        if(written) {
            store_write_marker(revision);
        } else {
            remove(STORE_PACK_PATH);
            ok = false;
        }
    }

    free(pack);
    free(data);
    free(icons);
    return ok;
}

// Fetches the icons that rows are waiting for: the small copy on the SD card (a few milliseconds) or, the first time, made from
// the PNG (downloaded if it is not there either). Nothing else runs here, so a download for later (the other thread's) never
// holds a row up.
static void store_icon_thread(void* arg) {
    store_data* store = (store_data*) arg;

    mkdir(STORE_ICON_DIR, 0777);
    int sheetsChecked = -2;
    u8* pack = NULL;
    u32 packCount = 0;

    while(!store->cancelled) {
        // The newest request first: it is what the rows show now.
        store_icon_job job = {{0}, NULL, false, 0};
        LightLock_Lock(&store->lock);
        if(store->jobCount > 0) {
            job = store->jobs[--store->jobCount];
        }

        LightLock_Unlock(&store->lock);

        if(job.url == NULL) {
            svcSleepThread(10 * 1000 * 1000LL);
            continue;
        }

        store_icon_result info;
        memset(&info, 0, sizeof(info));
        string_copy(info.slug, job.slug, sizeof(info.slug));
        info.requested = job.requested;
        u64 t = osGetTime();
        info.waitMs = (u32) (t - job.requested);

        // The sheets' icons are made once per catalogue revision (downloadMs: that time, when it was this icon that waited for it).
        int revision = store->revision;
        if(revision >= 0 && sheetsChecked != revision) {
            sheetsChecked = revision;
            free(pack);
            pack = NULL;
            if(store_read_marker() != revision) {
                store_logf(store, "icons of revision %d are not on the SD card: building them from the sheets", revision);
                store_build_from_sheets(store, revision);
                info.downloadMs = (u32) (osGetTime() - t);
                info.downloaded = true;
                t = osGetTime();
            }

            u64 loadStart = osGetTime();
            pack = store_pack_load(&packCount);
            store_logf(store, "pack: %u icons read in %u ms", packCount, (u32) (osGetTime() - loadStart));
        }

        u8* pixels = store_pack_get(pack, packCount, job.slug);
        info.statMs = (u32) (osGetTime() - t);
        info.fileSize = pixels != NULL ? STORE_ICON_SIDE * STORE_ICON_SIDE * 4 : 0;

        store_push_pixels(store, &info, pixels);
        free(job.url);
    }

    free(pack);
    store_release(store);
}

// What is not urgent, at the lowest priority so the screen never waits for it: which apps are on the console, a few at a
// time, and now and then one of the icons' small copies (made from a PNG already there at a quick pace, downloaded slowly).
static void store_background_thread(void* arg) {
    store_data* store = (store_data*) arg;

    u64 nextFlush = osGetTime() + 500;
    u64* installed = NULL;
    u32 installedCount = 0;
    u32 listGeneration = 0xFFFFFFFF;
    u32 loggedGeneration = 0xFFFFFFFF;
    u64 scanStart = 0;

    while(!store->cancelled) {
        // The list of installed titles is read again for each catalogue.
        LightLock_Lock(&store->lock);
        u32 generation = store->generation;
        bool scanning = store->scanNext < store->scanCount;
        u32 scanCount = store->scanCount;
        LightLock_Unlock(&store->lock);
        if(scanning && listGeneration != generation) {
            u64 t = osGetTime();
            free(installed);
            installed = store_load_installed(&installedCount);
            listGeneration = generation;
            scanStart = t;
            store_logf(store, "installed titles: %u read in %u ms", installedCount, (u32) (osGetTime() - t));
        }

        if(scanning) {
            store_scan_step(store, installed, installedCount);
        } else {
            if(listGeneration == generation && loggedGeneration != generation) {
                loggedGeneration = generation;
                store_logf(store, "status of %u apps found in %u ms", scanCount, (u32) (osGetTime() - scanStart));
            }
        }

        if(osGetTime() >= nextFlush) {
            store_log_flush(store);
            nextFlush = osGetTime() + 500;
        }

        svcSleepThread((scanning ? 5 : 50) * 1000 * 1000LL);
    }

    store_log_flush(store);
    free(installed);
    store_release(store);
}

static bool store_start_icon_thread(store_data* store) {
    if(store->iconThread) {
        return true;
    }

    LightLock_Lock(&store->lock);
    store->users += 2;
    LightLock_Unlock(&store->lock);
    // Core 2 on a New 3DS (full speed, as the HTTP threads use); the system core (1) only gets a fraction of the time, which
    // made decoding several times slower. An Old 3DS has no such core: the default one.
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    int core = isNew ? 2 : -2;
    bool first = threadCreate(store_icon_thread, store, 0x10000, 0x18, core, true) != NULL;
    // The slow one runs under the screen's own priority (a higher number is a lower priority).
    bool second = threadCreate(store_background_thread, store, 0x10000, 0x3E, core, true) != NULL;
    if(!first || !second) {
        LightLock_Lock(&store->lock);
        store->users -= (first ? 0 : 1) + (second ? 0 : 1);
        LightLock_Unlock(&store->lock);
        if(!first) {
            return false;
        }
    }

    store->iconThread = true;
    return true;
}

// The texture of an app's icon, or 0 (the generic one is drawn): asks for the icon the first time. Main thread.
static u32 store_icon(store_data* store, store_app* app) {
    if(app->texture != 0) {
        return app->texture;
    }

    if(app->iconUrl == NULL) {
        return 0;
    }

    if(app->iconState == ICON_NONE) {
        if(!store_start_icon_thread(store)) {
            app->iconState = ICON_FAILED;
            return 0;
        }

        LightLock_Lock(&store->lock);
        if(store->jobCount < STORE_ICON_QUEUE) {
            store_icon_job* job = &store->jobs[store->jobCount++];
            string_copy(job->slug, app->slug, sizeof(job->slug));
            job->url = strdup(app->iconUrl);
            job->decode = false;
            job->requested = osGetTime();
            app->iconState = ICON_QUEUED;
            if(store->outstanding == 0) {
                store->batchStart = job->requested;
                store->batchCount = 0;
            }

            store->outstanding++;
            store->batchCount++;
            store->lastRequestMs = (u32) job->requested;
        }

        LightLock_Unlock(&store->lock);
        return 0;
    }

    // Asked for: its texture arrives when the thread has made it, and then stays.
    return 0;
}

// What the icon thread has found out, for the apps now listed.
static void store_apply_results(store_data* store) {
    LightLock_Lock(&store->lock);
    u32 n = store->resultCount;
    store_icon_result results[STORE_ICON_QUEUE];
    memcpy(results, store->results, n * sizeof(*results));
    store->resultCount = 0;
    LightLock_Unlock(&store->lock);

    for(u32 r = 0; r < n; r++) {
        bool found = false;
        u32 textureMs = 0;
        for(u32 i = 0; i < store->count; i++) {
            if(strcmp(store->apps[i].slug, results[r].slug) != 0) {
                continue;
            }

            found = true;
            store_app* app = &store->apps[i];
            if(results[r].pixels != NULL) {
                u64 t = osGetTime();
                u32 id = screen_allocate_free_texture();
                screen_load_texture_untiled(id, results[r].pixels, STORE_ICON_SIDE * STORE_ICON_SIDE * 4, STORE_ICON_SIDE, STORE_ICON_SIDE, GPU_RGBA8, true);
                app->texture = id;
                textureMs = (u32) (osGetTime() - t);
            } else {
                app->iconState = ICON_FAILED;
            }

            break;
        }

        store_icon_result* x = &results[r];
        if(store->outstanding > 0) {
            store->outstanding--;
        }

        store_logf(store, "icon %s: waited %u ms, stat %u ms, %s %u ms (file %u B), decode %u ms, texture %u ms, in all %u ms%s%s",
                   x->slug, x->waitMs, x->statMs, x->downloaded ? "download" : "on the SD card", x->downloadMs, x->fileSize, x->decodeMs, textureMs,
                   (u32) (osGetTime() - x->requested), x->pixels == NULL ? " FAILED" : "", found ? "" : " (app gone)");
        if(store->outstanding == 0) {
            store_logf(store, "-- %u icons asked for in a row, all there in %u ms", store->batchCount, (u32) (osGetTime() - store->batchStart));
        }

        free(x->pixels);
    }
}

// ---- drawing ----

static store_app* store_of(const list_item* item) {
    return (store_app*) item->data;
}

// What is on the console for an app (0 until the background scan reaches it).
static u8 store_status(store_data* store, const store_app* app) {
    if(store->status == NULL) {
        return 0;
    }

    u32 index = (u32) (app - store->apps);
    return index < store->scanCount ? store->status[index] & (STATUS_CIA | STATUS_DSX) : 0;
}

// A small pill for a kind that is on the console. Drawn ending at `right`; returns where it starts.
static float store_draw_pill(const char* text, float right, float y, float height, bool selected) {
    const theme_current* t = theme();
    float textW = widget_text_width(text, WIDGET_SMALL_SCALE);
    float pillH = widget_text_height(WIDGET_SMALL_SCALE) + 4;
    float width = textW + 12;
    float x = right - width;
    float py = y + (height - pillH) / 2;
    u32 fill = selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft;
    u32 ink = selected ? t->onAccent : t->accent;
    screen_fill_round_rect(x, py, width, pillH, pillH / 2, fill);
    screen_draw_text(text, x + 6, py + 2, WIDGET_SMALL_SCALE, ink);
    return x;
}

// A badge on the top card for a kind the app offers: soft while it is not on the console, filled with a check when it
// is. Returns where the next badge goes.
static float store_draw_kind_badge(float x, float y, const char* text, bool present) {
    const theme_current* t = theme();
    float textW = widget_text_width(text, WIDGET_SMALL_SCALE);
    float height = widget_text_height(WIDGET_SMALL_SCALE) + 2;
    float width = textW + 12 + (present ? 14 : 0);
    screen_fill_round_rect(x, y, width, height, 6, present ? t->accent : t->accentSoft);
    float tx = x + 6;
    if(present) {
        screen_draw_icon(ICON_CHECK, tx - 1, y + (height - 12) / 2, 12, t->onAccent);
        tx += 14;
    }

    screen_draw_text(text, tx, y + 1, WIDGET_SMALL_SCALE, present ? t->onAccent : t->accent);
    return x + width + 6;
}

// The icon of an app in a box, or the generic one while it is not there.
static void store_draw_app_icon(store_data* store, store_app* app, float x, float y, float size, bool selected) {
    const theme_current* t = theme();
    u32 texture = store_icon(store, app);
    if(texture != 0) {
        screen_draw_texture(texture, x, y, size, size);
        return;
    }

    u32 back = selected ? THEME_ALPHA(t->onAccent, 0x30) : t->accentSoft;
    screen_fill_round_rect(x, y, size, size, size / 6, back);
    screen_draw_icon(app->ciaUrl != NULL ? ICON_PACKAGE : ICON_APPS, x + size * 0.2f, y + size * 0.2f, size * 0.6f, selected ? t->onAccent : t->accent);
}

static void store_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    store_data* store = (store_data*) data;
    const theme_current* t = theme();
    store_app* app = store_of(item);

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    float icon = height - 8;
    store_draw_app_icon(store, app, x + 6, y + 4, icon, selected);

    float tx = x + 6 + icon + 8;
    float right = x + width - 9;

    // What it comes as, from the right: a plain label for a kind that is not on the console, a pill for one that is.
    u8 status = store_status(store, app);
    for(int kind = 1; kind >= 0; kind--) {
        bool offered = kind == 1 ? app->dsxUrl != NULL : app->ciaUrl != NULL;
        if(!offered) {
            continue;
        }

        const char* label = kind == 1 ? "3DSX" : "CIA";
        bool present = (status & (kind == 1 ? STATUS_DSX : STATUS_CIA)) != 0;
        if(present) {
            right = store_draw_pill(label, right, y, height, selected) - 5;
        } else {
            float labelW = widget_text_width(label, WIDGET_SMALL_SCALE);
            screen_draw_text(label, right - labelW, y + (height - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, muted);
            right -= labelW + 8;
        }
    }

    float lineH = widget_text_height(WIDGET_TEXT_SCALE);
    float smallH = widget_text_height(WIDGET_SMALL_SCALE);
    float top = y + (height - lineH - smallH - 1) / 2;
    widget_text_fit(app->title, tx, top, WIDGET_TEXT_SCALE, ink, right - tx);

    char sub[96];
    snprintf(sub, sizeof(sub), "%s%s%s", app->author, app->author[0] != '\0' && app->version[0] != '\0' ? "  -  " : "", app->version);
    widget_text_fit(sub, tx, top + lineH + 1, WIDGET_SMALL_SCALE, muted, right - tx);
}

static void store_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    store_data* store = (store_data*) data;
    const theme_current* t = theme();

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = y2 - y1 - 28;
    widget_card(cardX, cardY, cardW, cardH);

    float textX = cardX + 16;
    float right = cardX + cardW - 14;

    if(selected == NULL) {
        const char* message = !store->done ? _("Downloading the catalogue...") : _("Nothing to show.");
        widget_text_fit(_("Store"), textX, cardY + 12, WIDGET_TITLE_SCALE, t->palette->text, right - textX);
        screen_draw_text_wrap(message, textX, cardY + 12 + widget_text_height(WIDGET_TITLE_SCALE) + 10, WIDGET_TEXT_SCALE, t->palette->textMuted, right);
        return;
    }

    store_app* app = store_of(selected);
    float icon = 48;
    store_draw_app_icon(store, app, textX, cardY + 12, icon, false);
    float titleX = textX + icon + 10;
    widget_text_fit(app->title, titleX, cardY + 12, WIDGET_TITLE_SCALE, t->palette->text, right - titleX);

    char line[160];
    snprintf(line, sizeof(line), "%s%s%s", app->author, app->author[0] != '\0' && app->version[0] != '\0' ? "  -  " : "", app->version);
    widget_text_fit(line, titleX, cardY + 12 + widget_text_height(WIDGET_TITLE_SCALE) + 4, WIDGET_SMALL_SCALE, t->palette->textMuted, right - titleX);

    float y = cardY + 12 + icon + 8;
    screen_draw_text_wrap(app->description, textX, y, WIDGET_TEXT_SCALE, t->palette->text, right);

    // What is on the console, above the badges.
    u8 status = store_status(store, app);
    float smallH = widget_text_height(WIDGET_SMALL_SCALE);
    float by = cardY + cardH - smallH - 14;
    if(status & (STATUS_CIA | STATUS_DSX)) {
        char have[96] = "";
        size_t used = 0;
        for(int kind = 0; kind < 2; kind++) {
            u8 bit = kind == 0 ? STATUS_CIA : STATUS_DSX;
            if(!(status & bit)) {
                continue;
            }

            // No version: what the Store once installed may have been updated since, or installed another way.
            used += snprintf(have + used, sizeof(have) - used, "%s%s", used > 0 ? ", " : "", kind == 0 ? "CIA" : "3DSX");
        }

        char text[160];
        float ly = by - smallH - 8;
        snprintf(text, sizeof(text), _("On this console: %s"), have);
        widget_text_fit(text, textX, ly, WIDGET_SMALL_SCALE, t->palette->textMuted, right - textX);
    }

    // What it offers, at the bottom.
    float bx = textX;
    if(app->ciaUrl != NULL) {
        char text[40];
        snprintf(text, sizeof(text), "CIA %s", app->ciaSize);
        bx = store_draw_kind_badge(bx, by, text, (status & STATUS_CIA) != 0);
    }

    if(app->dsxUrl != NULL) {
        char text[40];
        snprintf(text, sizeof(text), "3DSX %s", app->dsxSize);
        bx = store_draw_kind_badge(bx, by, text, (status & STATUS_DSX) != 0);
    }

    if(app->categoryId[0] != '\0') {
        widget_badge(bx, by, app->categoryId, t->palette->border, t->palette->text);
    }
}

// ---- installing ----

// What an install or a download of an app needs to remember when it ends well.
typedef struct {
    store_data* store;
    char slug[64];
    char version[24];
    bool cia;
    bool ok;
} store_install_ctx;

static void store_install_finished_url(void* data, u32 index) {
    ((store_install_ctx*) data)->ok = true;
}

// The Store notes which version it put on the console, and the app's status is looked at again.
static void store_install_finished_all(void* data) {
    store_install_ctx* ctx = (store_install_ctx*) data;
    store_data* store = ctx->store;
    if(ctx->ok) {
        store_record* record = store_record_find(store, ctx->slug);
        if(record == NULL && store->recordCount < STORE_RECORDS_MAX) {
            record = &store->records[store->recordCount++];
            memset(record, 0, sizeof(*record));
            string_copy(record->slug, ctx->slug, sizeof(record->slug));
        }

        if(record != NULL) {
            string_copy(ctx->cia ? record->cia : record->dsx, ctx->version, sizeof(record->cia));
            store_records_save(store);

            for(u32 i = 0; i < store->count; i++) {
                if(strcmp(store->apps[i].slug, ctx->slug) != 0) {
                    continue;
                }

                LightLock_Lock(&store->lock);
                if(store->scan != NULL && i < store->scanCount) {
                    string_copy(store->scan[i].recCia, record->cia, sizeof(store->scan[i].recCia));
                }

                store_scan_entry entry = store->scan != NULL && i < store->scanCount ? store->scan[i] : (store_scan_entry) {{0}};
                LightLock_Unlock(&store->lock);

                u8 bits = store_compute_status(&entry, NULL, 0);
                LightLock_Lock(&store->lock);
                if(store->status != NULL && i < store->scanCount) {
                    store->status[i] = bits | 0x80;
                }

                LightLock_Unlock(&store->lock);
                break;
            }
        }
    }

    free(ctx);
}

static store_data* store_sheet_store;
static store_app* store_sheet_app;

static void store_script_done(void* data, bool ok) {
    ((store_install_ctx*) data)->ok = ok;
    store_install_finished_all(data);
}

static void store_start(store_data* store, store_app* app, bool cia, const char* what) {
    store_install_ctx* ctx = (store_install_ctx*) calloc(1, sizeof(store_install_ctx));
    if(ctx == NULL) {
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("Failed to open the Store."));
        return;
    }

    ctx->store = store;
    ctx->cia = cia;
    string_copy(ctx->slug, app->slug, sizeof(ctx->slug));
    string_copy(ctx->version, app->version, sizeof(ctx->version));
    // A plain CIA installs as it downloads (the usual choice of how); the rest follow Universal-DB's steps.
    const char* script = cia ? app->ciaScript : app->dsxScript;
    if(script == NULL) {
        action_install_url(what, cia ? app->ciaUrl : app->dsxUrl, NULL, ctx, store_install_finished_url, store_install_finished_all, NULL);
        return;
    }

    storescript_run(script, what, ctx, store_script_done);
}

// The question, followed by what the author asks to be read first (if anything).
static void store_ask_text(char* out, size_t size, const store_app* app, const char* question) {
    snprintf(out, size, question, app->title);
    if(app->preMessage != NULL) {
        size_t used = strlen(out);
        snprintf(out + used, size - used, "\n\n%.300s%s", app->preMessage, strlen(app->preMessage) > 300 ? "..." : "");

        // Authors break their lines for Universal-Updater's screen: a lone line break joins the words again (a blank line
        // between paragraphs stays).
        for(char* c = out + used + 2; *c != '\0'; c++) {
            if(*c == '\n' && c[1] != '\n' && c[-1] != '\n') {
                *c = ' ';
            }
        }
    }
}

static void store_install_cia(void* data) {
    char message[700];
    store_ask_text(message, sizeof(message), store_sheet_app, _("Download and install %s?"));
    store_start(store_sheet_store, store_sheet_app, true, message);
}

static void store_download_dsx(void* data) {
    char message[700];
    store_ask_text(message, sizeof(message), store_sheet_app, _("Download %s into /3ds/?"));
    store_start(store_sheet_store, store_sheet_app, false, message);
}

static void store_open_app(store_data* store, store_app* app) {
    store_sheet_store = store;
    store_sheet_app = app;

    // Only a .3dsx: nothing to choose.
    if(app->ciaUrl == NULL) {
        store_download_dsx(app);
        return;
    }

    u8 status = store_status(store, app);
    const char* installLabel = (status & STATUS_CIA) ? _("Reinstall the CIA") : _("Install the CIA");
    sheet_action actions[2];
    u32 count = 0;
    actions[count++] = (sheet_action) {ICON_DOWNLOAD, installLabel, false, store_install_cia, false, _("Installs it as a title on the console.")};
    if(app->dsxUrl != NULL) {
        const char* dsxLabel = (status & STATUS_DSX) ? _("Download the 3DSX again") : _("Download the 3DSX");
        actions[count++] = (sheet_action) {ICON_WORLD_DOWNLOAD, dsxLabel, false, store_download_dsx, false, _("Saves it in /3ds/ to run from the Homebrew Launcher.")};
    }

    sheet_display_list(app->title, 0, actions, count, app);
}

// ---- tools: view, filter and sort ----

static void store_save(store_data* store) {
    store_settings* saved = &settings_get()->store;
    saved->kinds = store->kinds;
    saved->categories = store->categories;
    saved->shown = store->shown;
    saved->sort = store->sort;
    saved->grid = store->view != NULL && list_get_mode(store->view) == LIST_MODE_GRID;
    settings_save();
}

// The tiles of the grid in the next size (and the grid itself, when the rows were showing).
static void store_next_tile_size(ui_view* view) {
    store_settings* saved = &settings_get()->store;
    saved->tile = (saved->tile + 1) % LIST_TILE_LEVELS;
    if(list_get_mode(view) != LIST_MODE_GRID) {
        list_set_mode(view, LIST_MODE_GRID);
        saved->grid = true;
    }

    list_set_tile_size(view, list_tile_size_of(saved->tile));
    settings_save();
}

static bool store_tile_active(ui_view* view, void* data) {
    return false;
}

static void store_tile_press(ui_view* view, void* data) {
    store_next_tile_size(view);
}

static bool store_is_rows(ui_view* view, void* data) {
    return list_get_mode(view) == LIST_MODE_ROWS;
}

static bool store_is_grid(ui_view* view, void* data) {
    return list_get_mode(view) == LIST_MODE_GRID;
}

static void store_show_rows(ui_view* view, void* data) {
    list_set_mode(view, LIST_MODE_ROWS);
    store_save((store_data*) data);
}

static void store_show_grid(ui_view* view, void* data) {
    list_set_mode(view, LIST_MODE_GRID);
    store_save((store_data*) data);
}

static bool store_visible(void* data, list_item* item) {
    store_data* store = (store_data*) data;
    store_app* app = store_of(item);
    bool kind = ((store->kinds & 1) != 0 && app->ciaUrl != NULL) || ((store->kinds & 2) != 0 && app->dsxUrl != NULL);
    if(!kind || ((store->categories >> app->category) & 1) == 0) {
        return false;
    }

    if(store->shown != 0) {
        bool onConsole = (store_status(store, app) & (STATUS_CIA | STATUS_DSX)) != 0;
        return onConsole == (store->shown == 1);
    }

    return true;
}

static bool store_filter_active(ui_view* view, void* data) {
    store_data* store = (store_data*) data;
    return store->shown != 0 || store->kinds != 3 || store->categories != (1u << STORE_CATEGORY_COUNT) - 1;
}

// Popover groups: type, category, sort.
static bool store_popover_is_on(void* data, u32 group, u32 option) {
    store_data* store = (store_data*) data;
    switch(group) {
        case 0: return (store->kinds >> option) & 1;
        case 1: return (store->categories >> option) & 1;
        case 2: return store->shown == option;
        default: return store->sort == option;
    }
}

static void store_popover_select(void* data, u32 group, u32 option) {
    store_data* store = (store_data*) data;
    switch(group) {
        case 0: store->kinds ^= 1u << option; break;
        case 1: store->categories ^= 1u << option; break;
        case 2: store->shown = option; break;
        default:
            store->sort = option;
            store->resort = true;
            break;
    }

    store_save(store);
}

static void store_filter_open(ui_view* view, void* data) {
    popover_def def = {
            .groupCount = 4,
            .groups = {
                    {_("Type"), true, 2, {"CIA", "3DSX"}, {-1, -1}},
                    {_("Category"), true, STORE_CATEGORY_COUNT, {_("Games"), _("Apps"), _("Utilities"), _("Emulators"), _("Media"), _("Save tools"), _("Plugins"), _("Other")},
                     {-1, -1, -1, -1, -1, -1, -1, -1}},
                    {_("Show"), false, 3, {_("All"), _("Installed / downloaded"), _("Not installed / downloaded")}, {ICON_APPS, ICON_CHECK, ICON_X}, true},
                    // Icons with the name in a tooltip: four names this long do not fit in one line.
                    {_("Sort by"), false, 4, {_("Name"), _("Author"), _("Newest"), _("Stars")}, {ICON_ABC, ICON_USER, ICON_CLOCK, ICON_STAR}, true}
            },
            .isOn = store_popover_is_on,
            .select = store_popover_select
    };

    popover_display(&def, data);
}

// Name, author, newest first, most stars first; the name breaks ties.
static int store_compare_items(void* data, const void* p1, const void* p2) {
    store_data* store = (store_data*) data;
    const store_app* a = store_of((const list_item*) p1);
    const store_app* b = store_of((const list_item*) p2);
    int result = 0;
    switch(store->sort) {
        case 1: result = strcasecmp(a->author, b->author); break;
        case 2: result = strcmp(b->updated, a->updated); break;
        case 3: result = a->stars < b->stars ? 1 : a->stars > b->stars ? -1 : 0; break;
        default: break;
    }

    return result != 0 ? result : strcasecmp(a->title, b->title);
}

// A tile: the icon over the whole of it (the name is on the top screen).
static void store_draw_tile(void* data, list_item* item, float x, float y, float size, bool selected) {
    store_data* store = (store_data*) data;
    const theme_current* t = theme();
    if(selected) {
        screen_fill_round_rect(x - 3, y - 3, size + 6, size + 6, t->def->radius + 2, t->accent);
        screen_fill_round_rect(x, y, size, size, t->def->radius, t->accentSoft);
    } else {
        screen_frame_round_rect(x, y, size, size, t->def->radius, t->def->border, t->palette->border, t->palette->surface);
    }

    store_app* app = store_of(item);
    store_draw_app_icon(store, app, x + 4, y + 4, size - 8, false);

    // A mark in the corner when something of it is on the console.
    u8 status = store_status(store, app);
    if(status & (STATUS_CIA | STATUS_DSX)) {
        float r = size >= 64 ? 20 : 16;
        float mx = x + size - r - 1;
        float my = y + size - r - 1;
        screen_fill_round_rect(mx - 1, my - 1, r + 2, r + 2, (r + 2) / 2, t->palette->surface);
        screen_fill_round_rect(mx, my, r, r, r / 2, t->accent);
        screen_draw_icon(ICON_CHECK, mx + (r - 12) / 2, my + (r - 12) / 2, 12, t->onAccent);
    }
}

// ---- search ----

static bool store_search_active(ui_view* view, void* data) {
    return list_get_search(view)[0] != '\0';
}

static void store_search_done(ui_view* kbdView, void* data, SwkbdButton button, const char* response) {
    store_data* store = (store_data*) data;
    if(button == SWKBD_BUTTON_CONFIRM) {
        list_set_search(store->view, response);
    }
}

static void store_search_open(ui_view* view, void* data) {
    osk_display(_("Search the Store"), list_get_search(view), 64, 0, data, store_search_done);
}

// ---- the screen ----

static void store_clear_items(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

// The icons' textures go back (main thread: the textures belong to it).
static void store_unload_textures(store_app* apps, u32 count) {
    for(u32 i = 0; i < count; i++) {
        if(apps[i].texture != 0) {
            screen_unload_texture(apps[i].texture);
            apps[i].texture = 0;
        }
    }
}

static void store_close(ui_view* view, store_data* store, linked_list* items) {
    ui_pop();
    store_clear_items(items);
    list_destroy(view);
    store_unload_textures(store->apps, store->count);
    store_logf(store, "Store closed");
    store_log_flush(store);

    // The threads may still be running: each lets go of the data when it ends, and the last one frees it.
    store->cancelled = true;
    store_release(store);
}

// Lists the apps of `store->apps`, keeping the selection on the same app (by title) when there was one.
static void store_fill(store_data* store, ui_view* view, linked_list* items, const char* keep) {
    list_item* select = NULL;
    for(u32 i = 0; i < store->count; i++) {
        list_item* item = (list_item*) calloc(1, sizeof(list_item));
        if(item != NULL) {
            string_copy(item->name, store->apps[i].title, sizeof(item->name));
            item->color = COLOR_TEXT;
            item->data = &store->apps[i];
            linked_list_add(items, item);
            if(keep != NULL && keep[0] != '\0' && strcmp(keep, store->apps[i].title) == 0) {
                select = item;
            }
        }
    }

    if(select != NULL) {
        list_select(view, select);
    }
}

// A catalogue from the loading thread: the first one (the copy on the SD card) or a newer one from the server.
static void store_take(store_data* store, ui_view* view, linked_list* items, list_item* selected) {
    char keep[80] = "";
    if(selected != NULL) {
        string_copy(keep, store_of(selected)->title, sizeof(keep));
    }

    // The rows point into the old apps: they go first. The icons of apps still there are handed to the new ones.
    store_clear_items(items);
    for(u32 j = 0; j < store->pendingCount; j++) {
        for(u32 i = 0; i < store->count; i++) {
            if(strcmp(store->apps[i].slug, store->pending[j].slug) == 0) {
                store->pending[j].texture = store->apps[i].texture;
                store->pending[j].iconState = store->apps[i].iconState;
                store->pending[j].iconFileSize = store->apps[i].iconFileSize;
                store->apps[i].texture = 0;
                break;
            }
        }
    }

    store_unload_textures(store->apps, store->count);
    store_free_apps(store->apps, store->count);
    store->apps = store->pending;
    store->count = store->pendingCount;
    store->pending = NULL;
    store->pendingCount = 0;
    store->haveCatalogue = true;
    store->handoff = 0;

    // What is on the console, to find out slowly in the background.
    store_scan_entry* scan = (store_scan_entry*) calloc(store->count + 1, sizeof(store_scan_entry));
    u8* status = (u8*) calloc(store->count + 1, 1);
    if(scan != NULL && status != NULL) {
        for(u32 i = 0; i < store->count; i++) {
            const store_app* app = &store->apps[i];
            string_copy(scan[i].slug, app->slug, sizeof(scan[i].slug));
            scan[i].uniqueId = app->uniqueId;
            scan[i].hasCia = app->ciaUrl != NULL;
            scan[i].hasDsx = app->dsxUrl != NULL;
            if(app->dsxName[0] != '\0') {
                string_copy(scan[i].dsxName, app->dsxName, sizeof(scan[i].dsxName));
            } else if(app->dsxUrl != NULL) {
                store_dsx_name(app->dsxUrl, scan[i].dsxName, sizeof(scan[i].dsxName));
            }

            store_record* record = store_record_find(store, app->slug);
            if(record != NULL) {
                string_copy(scan[i].recCia, record->cia, sizeof(scan[i].recCia));
            }
        }
    }

    LightLock_Lock(&store->lock);
    free(store->scan);
    free(store->status);
    store->scan = scan != NULL && status != NULL ? scan : NULL;
    store->status = scan != NULL && status != NULL ? status : NULL;
    store->scanCount = scan != NULL && status != NULL ? store->count : 0;
    store->scanNext = 0;
    store->generation++;
    LightLock_Unlock(&store->lock);
    if(scan == NULL || status == NULL) {
        free(scan);
        free(status);
    }

    // The icons to fetch slowly in the background, in the order of the list.
    store_prefetch_entry* list = (store_prefetch_entry*) calloc(store->count + 1, sizeof(store_prefetch_entry));
    u32 listed = 0;
    for(u32 i = 0; list != NULL && i < store->count; i++) {
        if(store->apps[i].iconUrl != NULL && store->apps[i].slug[0] != '\0') {
            string_copy(list[listed].slug, store->apps[i].slug, sizeof(list[listed].slug));
            list[listed].url = strdup(store->apps[i].iconUrl);
            listed++;
        }
    }

    LightLock_Lock(&store->lock);
    for(u32 i = 0; i < store->prefetchCount; i++) {
        free(store->prefetch[i].url);
    }

    free(store->prefetch);
    store->prefetch = list;
    store->prefetchCount = list != NULL ? listed : 0;
    store->prefetchNext = 0;
    LightLock_Unlock(&store->lock);
    store_start_icon_thread(store);

    u64 fillStart = osGetTime();
    store_fill(store, view, items, keep);
    store->resort = true;
    store_logf(store, "catalogue of %u apps taken by the screen (list filled in %u ms)", store->count, (u32) (osGetTime() - fillStart));
}

static void store_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    store_data* store = (store_data*) data;

    view->name = _("Store");
    view->info = _(" get    search    back");

    if(!store->started) {
        store->started = true;
        // The first version kept the catalogue in another file.
        remove(STORE_OLD_CACHE_PATH);
        store_records_load(store);
        if(NSA_DEBUG) {
            remove(STORE_LOG_PATH);
        }

        store_logf(store, "Store opened");

        if(R_FAILED(fs_open_archive(&store->archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
            store->archive = 0;
        }

        if(threadCreate(store_load_thread, store, 0x10000, 0x18, 1, true) == NULL) {
            error_display_res(NULL, NULL, R_APP_THREAD_CREATE_FAILED, _("Failed to open the Store."));
            store_release(store);   // The loading thread's share: it never started.
            store_close(view, store, items);
            return;
        }
    }

    // Until the first catalogue is here (the copy on the SD card, or the server's when there is none), a wheel turns.
    list_set_busy(view, store->haveCatalogue || store->done ? NULL : "Loading the catalogue...");

    if(hidKeysDown() & KEY_B) {
        if(list_get_search(view)[0] != '\0') {
            list_set_search(view, "");
            return;
        }

        store_close(view, store, items);
        return;
    }

    store->items = items;
    store_apply_results(store);

    if(store->resort) {
        linked_list_sort(items, store, store_compare_items);
        store->resort = false;
    }

    if(store->handoff == 1) {
        store_take(store, view, items, selected);
        return;
    }

    if(store->done && !store->haveCatalogue && !store->noticeShown) {
        store->noticeShown = true;
        error_display_res(NULL, NULL, store->res != 0 ? store->res : R_APP_BAD_DATA, _("Failed to download the Store's catalogue."));
        store_close(view, store, items);
        return;
    }

    if(store->done && store->offline && store->haveCatalogue && !store->noticeShown) {
        store->noticeShown = true;
        prompt_display_notify(_("Store"), _("No connection: this is the catalogue saved from last time."), COLOR_TEXT, NULL, NULL, NULL);
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        store_open_app(store, store_of(selected));
    }

    if(hidKeysDown() & KEY_SELECT) {
        store_next_tile_size(view);
    }

    if(hidKeysDown() & KEY_Y) {
        list_set_mode(view, list_get_mode(view) == LIST_MODE_GRID ? LIST_MODE_ROWS : LIST_MODE_GRID);
        store_save(store);
    }

    if(hidKeysDown() & KEY_L) {
        store_filter_open(view, store);
        return;
    }

    if(hidKeysDown() & KEY_R) {
        store_search_open(view, store);
    }
}

void store_open() {
    store_data* store = (store_data*) calloc(1, sizeof(store_data));
    if(store == NULL) {
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("Failed to open the Store."));
        return;
    }

    LightLock_Init(&store->lock);
    store->users = 2;   // The screen and the loading thread.
    store->opened = osGetTime();

    store->kinds = settings_get()->store.kinds;
    store->categories = settings_get()->store.categories;
    store->shown = settings_get()->store.shown;
    store->sort = settings_get()->store.sort;

    ui_view* view = list_display(_("Store"), _(" get    search    back"), store, store_update, store_draw_top);
    if(view == NULL) {
        free(store);
        return;
    }

    store->view = view;
    if(settings_get()->store.grid) {
        list_set_mode(view, LIST_MODE_GRID);
    }

    list_style style = {
            .rowHeight = 36,
            .drawRow = store_draw_row,
            .tileSize = list_tile_size_of(settings_get()->store.tile),
            .drawTile = store_draw_tile
    };
    list_set_style(view, &style);
    list_set_filter(view, store_visible);
    list_show_title_bar(view);

    // Right to left.
    list_add_tool(view, ICON_SEARCH, store_search_active, store_search_open);
    list_add_tool(view, ICON_FILTER, store_filter_active, store_filter_open);
    list_add_tool(view, ICON_ZOOM_IN, store_tile_active, store_tile_press);
    list_add_tool(view, ICON_LAYOUT_GRID, store_is_grid, store_show_grid);
    list_add_tool(view, ICON_LIST, store_is_rows, store_show_rows);
}
