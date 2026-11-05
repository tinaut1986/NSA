#include <ctype.h>
#include <malloc.h>
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
#define STORE_FULL_URL "https://db.universal-team.net/data/full.json"
#define STORE_CACHE_PATH "sdmc:" APP_DATA_DIR "store-cache.json"
#define STORE_OLD_CACHE_PATH "sdmc:" APP_DATA_DIR "store.unistore"
#define STORE_ICON_DIR "sdmc:" APP_DATA_DIR "store-icons"
#define STORE_ICON_PATH "sdmc:" APP_DATA_DIR "store-icons/%s.png"
#define STORE_ICON_ARCHIVE_PATH APP_DATA_DIR "store-icons/%s.png"
#define STORE_FULL_MAX (4 * 1024 * 1024)
#define STORE_INFO_MAX 8192
#define STORE_ICON_MAX (512 * 1024)
#define STORE_ICON_QUEUE 24
#define STORE_PREFETCH_PAUSE 4000   // ms between the icons fetched in the background, when no row is waiting for one.
#define STORE_INSTALLED_PATH "sdmc:" APP_DATA_DIR "store-installed.json"
#define STORE_RECORDS_MAX 512
#define STORE_CACHE_FORMAT 3   // Raised when the copy on the SD card gains fields: the old one is downloaded again.

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
    char* iconUrl;
    u32 stars;
    u32 uniqueId;       // The title's unique ID (a CIA's), or 0 when the catalogue has none.
    char updated[12];   // "2026-03-19".
    u8 category;        // An index of store_categories.
    u8 iconState;
    u32 iconFileSize;
} store_app;

typedef struct {
    char slug[64];
    char* url;
} store_icon_job;

typedef struct {
    char slug[64];
    u8 state;
    u32 size;
} store_icon_result;

// What the Store itself installed or downloaded, and which version of the catalogue it was: the only way to tell an
// update is waiting (the console's version number is not the catalogue's).
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
    char version[24];
    char recCia[24];
    char recDsx[24];
} store_scan_entry;

// Bits of an app's status.
#define STATUS_CIA 1           // The title is installed.
#define STATUS_DSX 2           // The .3dsx is in /3ds/.
#define STATUS_CIA_UPDATE 4    // The catalogue has another version than the one the Store installed.
#define STATUS_DSX_UPDATE 8

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

    // What shows and in which order (kept in settings.json).
    u32 kinds;        // Bits: 1 CIA, 2 3DSX.
    u32 categories;   // One bit per store_categories entry.
    u32 sort;
    bool resort;
} store_data;

static void store_free_apps(store_app* apps, u32 count) {
    if(apps == NULL) {
        return;
    }

    for(u32 i = 0; i < count; i++) {
        free(apps[i].ciaUrl);
        free(apps[i].dsxUrl);
        free(apps[i].iconUrl);
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

// The apps for 3DS that publish a CIA or a .3dsx, from the full catalogue (Universal-DB's data/full.json).
static int store_parse_full(const char* text, size_t size, store_app** out) {
    json_error_t error;
    json_t* root = json_loadb(text, size, 0, &error);
    if(root == NULL || !json_is_array(root)) {
        json_decref(root);
        return -1;
    }

    store_app* apps = (store_app*) calloc(json_array_size(root) + 1, sizeof(store_app));
    if(apps == NULL) {
        json_decref(root);
        return -1;
    }

    u32 count = 0;
    for(size_t i = 0; i < json_array_size(root); i++) {
        json_t* entry = json_array_get(root, i);
        if(!json_is_object(entry)) {
            continue;
        }

        bool for3ds = false;
        json_t* systems = json_object_get(entry, "systems");
        size_t index;
        json_t* value;
        json_array_foreach(systems, index, value) {
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
        json_object_foreach(json_object_get(entry, "downloads"), key, file) {
            const char* url = store_str(file, "url");
            if(!json_is_object(file) || !store_https(url)) {
                continue;
            }

            if(store_has_ext(key, ".cia") && app->ciaUrl == NULL) {
                app->ciaUrl = strdup(url);
                string_copy(app->ciaSize, store_str(file, "size_str"), sizeof(app->ciaSize));
            } else if(store_has_ext(key, ".3dsx") && app->dsxUrl == NULL) {
                app->dsxUrl = strdup(url);
                string_copy(app->dsxSize, store_str(file, "size_str"), sizeof(app->dsxSize));
            }
        }

        string_copy(app->title, store_str(entry, "title"), sizeof(app->title));
        if((app->ciaUrl == NULL && app->dsxUrl == NULL) || app->title[0] == '\0') {
            free(app->ciaUrl);
            free(app->dsxUrl);
            memset(app, 0, sizeof(*app));
            continue;
        }

        string_copy(app->author, store_str(entry, "author"), sizeof(app->author));
        string_copy(app->version, store_str(entry, "version"), sizeof(app->version));
        string_copy(app->description, store_str(entry, "description"), sizeof(app->description));
        store_make_slug(app->slug, sizeof(app->slug), store_str(entry, "slug"));

        json_t* categories = json_object_get(entry, "categories");
        if(json_is_array(categories) && json_array_size(categories) > 0 && json_is_string(json_array_get(categories, 0))) {
            string_copy(app->categoryId, json_string_value(json_array_get(categories, 0)), sizeof(app->categoryId));
        }

        app->category = store_category_index(app->categoryId);
        json_t* stars = json_object_get(entry, "stars");
        app->stars = json_is_integer(stars) && json_integer_value(stars) > 0 ? (u32) json_integer_value(stars) : 0;
        const char* updated = store_str(entry, "updated");
        string_copy(app->updated, updated, 11);
        json_t* ids = json_object_get(entry, "unique_ids");
        if(json_is_array(ids) && json_array_size(ids) > 0 && json_is_integer(json_array_get(ids, 0))) {
            app->uniqueId = (u32) json_integer_value(json_array_get(ids, 0));
        }

        const char* icon = store_str(entry, "icon");
        if(app->slug[0] != '\0' && store_https(icon)) {
            app->iconUrl = strdup(icon);
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
    }

    if(app->iconUrl != NULL) {
        json_object_set_new(object, "i", json_string(app->iconUrl));
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
        }

        if(store_str(entry, "i")[0] != '\0') {
            app->iconUrl = strdup(store_str(entry, "i"));
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
    int cachedCount = store_read_cache(&cached, &cachedRevision);
    bool haveCache = cachedCount > 0;
    if(haveCache) {
        store_hand_off(store, cached, (u32) cachedCount);
    } else {
        store_free_apps(cached, cachedCount > 0 ? (u32) cachedCount : 0);
    }

    // Has the catalogue changed? A few hundred bytes tell.
    int revision = store->cancelled ? -1 : store_server_revision();
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

            if(count >= 0) {
                store_write_cache(apps, (u32) count, revision);
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

// What is on the console for one app. An update is only told when the Store installed that app before, as that is when
// the version it left is known.
static u8 store_compute_status(const store_scan_entry* entry) {
    u8 bits = 0;
    if(entry->hasCia) {
        // Without a unique ID, what the Store installed is all there is to go by.
        bool installed = entry->uniqueId != 0 ? store_title_installed(entry->uniqueId) : entry->recCia[0] != '\0';
        if(installed) {
            bits |= STATUS_CIA;
            if(entry->recCia[0] != '\0' && strcmp(entry->recCia, entry->version) != 0) {
                bits |= STATUS_CIA_UPDATE;
            }
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
            if(entry->recDsx[0] != '\0' && strcmp(entry->recDsx, entry->version) != 0) {
                bits |= STATUS_DSX_UPDATE;
            }
        }
    }

    return bits;
}

// A few apps' status, for the background thread to call now and then.
static void store_scan_step(store_data* store) {
    for(int n = 0; n < 4; n++) {
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

        u8 bits = store_compute_status(&entry);
        LightLock_Lock(&store->lock);
        if(generation == store->generation && store->status != NULL && index < store->scanCount) {
            store->status[index] = bits | 0x80;
        }

        LightLock_Unlock(&store->lock);
    }
}

// ---- the icon thread ----

static void store_push_result(store_data* store, const char* slug, u8 state, u32 size) {
    LightLock_Lock(&store->lock);
    if(store->resultCount < STORE_ICON_QUEUE) {
        store_icon_result* r = &store->results[store->resultCount++];
        string_copy(r->slug, slug, sizeof(r->slug));
        r->state = state;
        r->size = size;
    }

    LightLock_Unlock(&store->lock);
}

// Downloads one icon to the SD card; false when it could not (and leaves no half file).
static bool store_download_icon(const char* slug, const char* url, u8* buffer, u32* fileSize) {
    char path[FILE_PATH_MAX];
    snprintf(path, sizeof(path), STORE_ICON_PATH, slug);

    u32 size = 0;
    if(R_FAILED(http_download_buffer(url, &size, buffer, STORE_ICON_MAX)) || size == 0 || size >= STORE_ICON_MAX) {
        return false;
    }

    FILE* file = fopen(path, "wb");
    bool written = file != NULL && fwrite(buffer, 1, size, file) == size;
    if(file != NULL) {
        fclose(file);
    }

    if(!written) {
        remove(path);
        return false;
    }

    *fileSize = size;
    return true;
}

// One step of the slow background fetch: skips the icons already on the SD card (a quick look each) and downloads the first
// missing one. False when there is nothing left to fetch.
static bool store_prefetch_step(store_data* store, u8* buffer) {
    for(int looked = 0; looked < 24; looked++) {
        char slug[64];
        char* url = NULL;
        LightLock_Lock(&store->lock);
        if(store->prefetchNext < store->prefetchCount) {
            store_prefetch_entry* entry = &store->prefetch[store->prefetchNext++];
            string_copy(slug, entry->slug, sizeof(slug));
            url = strdup(entry->url);
        }

        LightLock_Unlock(&store->lock);

        if(url == NULL) {
            return false;
        }

        char path[FILE_PATH_MAX];
        snprintf(path, sizeof(path), STORE_ICON_PATH, slug);
        struct stat st;
        if(stat(path, &st) == 0 && st.st_size > 0) {
            free(url);
            continue;
        }

        u32 size = 0;
        store_download_icon(slug, url, buffer, &size);
        free(url);
        return true;
    }

    return true;
}

static void store_icon_thread(void* arg) {
    store_data* store = (store_data*) arg;

    mkdir(STORE_ICON_DIR, 0777);
    u8* buffer = (u8*) malloc(STORE_ICON_MAX);
    u64 nextPrefetch = osGetTime() + STORE_PREFETCH_PAUSE;

    while(!store->cancelled) {
        // The newest request first: it is what the rows show now.
        store_icon_job job = {{0}, NULL};
        LightLock_Lock(&store->lock);
        if(store->jobCount > 0) {
            job = store->jobs[--store->jobCount];
        }

        LightLock_Unlock(&store->lock);

        if(job.url == NULL) {
            // Nothing asked for: find out what is installed, a few apps at a time, and now and then fetch one of the
            // icons not on the SD card yet.
            store_scan_step(store);
            if(buffer != NULL && osGetTime() >= nextPrefetch) {
                nextPrefetch = osGetTime() + (store_prefetch_step(store, buffer) ? STORE_PREFETCH_PAUSE : 60 * 1000);
            }

            svcSleepThread(50 * 1000 * 1000LL);
            continue;
        }

        char path[FILE_PATH_MAX];
        snprintf(path, sizeof(path), STORE_ICON_PATH, job.slug);

        struct stat st;
        if(stat(path, &st) == 0 && st.st_size > 0) {
            store_push_result(store, job.slug, ICON_READY, (u32) st.st_size);
        } else if(buffer != NULL) {
            u32 size = 0;
            store_push_result(store, job.slug, store_download_icon(job.slug, job.url, buffer, &size) ? ICON_READY : ICON_FAILED, size);
        } else {
            store_push_result(store, job.slug, ICON_FAILED, 0);
        }

        free(job.url);
    }

    free(buffer);
    store_release(store);
}

static bool store_start_icon_thread(store_data* store) {
    if(store->iconThread) {
        return true;
    }

    LightLock_Lock(&store->lock);
    store->users++;
    LightLock_Unlock(&store->lock);
    if(threadCreate(store_icon_thread, store, 0x10000, 0x18, 1, true) == NULL) {
        LightLock_Lock(&store->lock);
        store->users--;
        LightLock_Unlock(&store->lock);
        return false;
    }

    store->iconThread = true;
    return true;
}

// The texture of an app's icon, or 0 (the generic one is drawn): asks for the icon the first time. Main thread.
static u32 store_icon(store_data* store, store_app* app) {
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
            app->iconState = ICON_QUEUED;
        }

        LightLock_Unlock(&store->lock);
        return 0;
    }

    if(app->iconState == ICON_READY) {
        char path[FILE_PATH_MAX];
        snprintf(path, sizeof(path), STORE_ICON_ARCHIVE_PATH, app->slug);
        return thumbs_get(store->archive, path, app->iconFileSize, NULL);
    }

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
        for(u32 i = 0; i < store->count; i++) {
            if(strcmp(store->apps[i].slug, results[r].slug) == 0) {
                store->apps[i].iconState = results[r].state;
                store->apps[i].iconFileSize = results[r].size;
                break;
            }
        }
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
    return index < store->scanCount ? store->status[index] & 0x0F : 0;
}

// A small pill for a kind that is on the console: soft when up to date, filled with a refresh mark when a newer one waits.
// Drawn ending at `right`; returns where it starts.
static float store_draw_pill(const char* text, float right, float y, float height, bool update, bool selected) {
    const theme_current* t = theme();
    float textW = widget_text_width(text, WIDGET_SMALL_SCALE);
    float pillH = widget_text_height(WIDGET_SMALL_SCALE) + 4;
    float width = textW + 12 + (update ? 12 : 0);
    float x = right - width;
    float py = y + (height - pillH) / 2;
    u32 fill = update ? (selected ? t->onAccent : t->accent) : (selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft);
    u32 ink = update ? (selected ? t->accent : t->onAccent) : (selected ? t->onAccent : t->accent);
    screen_fill_round_rect(x, py, width, pillH, pillH / 2, fill);
    float tx = x + 6;
    if(update) {
        screen_draw_icon(ICON_REFRESH, tx - 1, py + (pillH - 12) / 2, 12, ink);
        tx += 12;
    }

    screen_draw_text(text, tx, py + 2, WIDGET_SMALL_SCALE, ink);
    return x;
}

// A badge on the top card for a kind the app offers: soft while it is not on the console, filled with a check when it
// is, and with a refresh mark when a newer version waits. Returns where the next badge goes.
static float store_draw_kind_badge(float x, float y, const char* text, bool present, bool update) {
    const theme_current* t = theme();
    float textW = widget_text_width(text, WIDGET_SMALL_SCALE);
    float height = widget_text_height(WIDGET_SMALL_SCALE) + 2;
    float width = textW + 12 + (present ? 14 : 0);
    screen_fill_round_rect(x, y, width, height, 6, present ? t->accent : t->accentSoft);
    float tx = x + 6;
    if(present) {
        screen_draw_icon(update ? ICON_REFRESH : ICON_CHECK, tx - 1, y + (height - 12) / 2, 12, t->onAccent);
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
        bool update = (status & (kind == 1 ? STATUS_DSX_UPDATE : STATUS_CIA_UPDATE)) != 0;
        if(present) {
            right = store_draw_pill(label, right, y, height, update, selected) - 5;
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
        store_record* record = store_record_find(store, app->slug);
        char have[96] = "";
        size_t used = 0;
        for(int kind = 0; kind < 2; kind++) {
            u8 bit = kind == 0 ? STATUS_CIA : STATUS_DSX;
            if(!(status & bit)) {
                continue;
            }

            const char* version = record != NULL ? (kind == 0 ? record->cia : record->dsx) : "";
            used += snprintf(have + used, sizeof(have) - used, "%s%s%s%s", used > 0 ? ", " : "", kind == 0 ? "CIA" : "3DSX", version[0] != '\0' ? " " : "", version);
        }

        char text[160];
        float ly = by - smallH - 8;
        if(status & (STATUS_CIA_UPDATE | STATUS_DSX_UPDATE)) {
            snprintf(text, sizeof(text), _("Update available: %s"), app->version);
            screen_draw_icon(ICON_REFRESH, textX, ly - 2, 16, t->accent);
            widget_text_fit(text, textX + 20, ly, WIDGET_SMALL_SCALE, t->accent, right - textX - 20);
            ly -= smallH + 4;
        }

        snprintf(text, sizeof(text), _("On this console: %s"), have);
        widget_text_fit(text, textX, ly, WIDGET_SMALL_SCALE, t->palette->textMuted, right - textX);
    }

    // What it offers, at the bottom.
    float bx = textX;
    if(app->ciaUrl != NULL) {
        char text[40];
        snprintf(text, sizeof(text), "CIA %s", app->ciaSize);
        bx = store_draw_kind_badge(bx, by, text, (status & STATUS_CIA) != 0, (status & STATUS_CIA_UPDATE) != 0);
    }

    if(app->dsxUrl != NULL) {
        char text[40];
        snprintf(text, sizeof(text), "3DSX %s", app->dsxSize);
        bx = store_draw_kind_badge(bx, by, text, (status & STATUS_DSX) != 0, (status & STATUS_DSX_UPDATE) != 0);
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
                    string_copy(store->scan[i].recDsx, record->dsx, sizeof(store->scan[i].recDsx));
                }

                store_scan_entry entry = store->scan != NULL && i < store->scanCount ? store->scan[i] : (store_scan_entry) {{0}};
                LightLock_Unlock(&store->lock);

                u8 bits = store_compute_status(&entry);
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
    action_install_url(what, cia ? app->ciaUrl : app->dsxUrl, NULL, ctx, store_install_finished_url, store_install_finished_all, NULL);
}

static void store_install_cia(void* data) {
    char message[200];
    snprintf(message, sizeof(message), _("Download and install %s?"), store_sheet_app->title);
    store_start(store_sheet_store, store_sheet_app, true, message);
}

// Only downloads the CIA (to /nsa/received/), to install it later from Files.
static void store_save_cia(void* data) {
    action_save_url(store_sheet_app->ciaUrl, NULL, NULL, NULL);
}

static void store_download_dsx(void* data) {
    char message[200];
    snprintf(message, sizeof(message), _("Download %s into /3ds/?"), store_sheet_app->title);
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
    const char* installLabel = (status & STATUS_CIA_UPDATE) ? _("Update the CIA") : (status & STATUS_CIA) ? _("Reinstall the CIA") : _("Install the CIA");
    sheet_action actions[3];
    u32 count = 0;
    actions[count++] = (sheet_action) {ICON_DOWNLOAD, installLabel, false, store_install_cia, false, _("Installs it as a title on the console.")};
    actions[count++] = (sheet_action) {ICON_UPLOAD, _("Download the CIA"), false, store_save_cia, false, _("Saves it in /nsa/received/ to install it later from Files.")};
    if(app->dsxUrl != NULL) {
        const char* dsxLabel = (status & STATUS_DSX_UPDATE) ? _("Update the 3DSX") : (status & STATUS_DSX) ? _("Download the 3DSX again") : _("Download the 3DSX");
        actions[count++] = (sheet_action) {ICON_WORLD_DOWNLOAD, dsxLabel, false, store_download_dsx, false, _("Saves it in /3ds/ to run from the Homebrew Launcher.")};
    }

    sheet_display_list(app->title, 0, actions, count, app);
}

// ---- tools: view, filter and sort ----

static void store_save(store_data* store) {
    store_settings* saved = &settings_get()->store;
    saved->kinds = store->kinds;
    saved->categories = store->categories;
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
    return kind && ((store->categories >> app->category) & 1) != 0;
}

static bool store_filter_active(ui_view* view, void* data) {
    store_data* store = (store_data*) data;
    return store->kinds != 3 || store->categories != (1u << STORE_CATEGORY_COUNT) - 1;
}

// Popover groups: type, category, sort.
static bool store_popover_is_on(void* data, u32 group, u32 option) {
    store_data* store = (store_data*) data;
    switch(group) {
        case 0: return (store->kinds >> option) & 1;
        case 1: return (store->categories >> option) & 1;
        default: return store->sort == option;
    }
}

static void store_popover_select(void* data, u32 group, u32 option) {
    store_data* store = (store_data*) data;
    switch(group) {
        case 0: store->kinds ^= 1u << option; break;
        case 1: store->categories ^= 1u << option; break;
        default:
            store->sort = option;
            store->resort = true;
            break;
    }

    store_save(store);
}

static void store_filter_open(ui_view* view, void* data) {
    popover_def def = {
            .groupCount = 3,
            .groups = {
                    {_("Type"), true, 2, {"CIA", "3DSX"}, {-1, -1}},
                    {_("Category"), true, STORE_CATEGORY_COUNT, {_("Games"), _("Apps"), _("Utilities"), _("Emulators"), _("Media"), _("Save tools"), _("Plugins"), _("Other")},
                     {-1, -1, -1, -1, -1, -1, -1, -1}},
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

    // A mark in the corner when something of it is on the console (a refresh mark when a newer version waits).
    u8 status = store_status(store, app);
    if(status & (STATUS_CIA | STATUS_DSX)) {
        bool update = (status & (STATUS_CIA_UPDATE | STATUS_DSX_UPDATE)) != 0;
        float r = size >= 64 ? 20 : 16;
        float mx = x + size - r - 1;
        float my = y + size - r - 1;
        screen_fill_round_rect(mx - 1, my - 1, r + 2, r + 2, (r + 2) / 2, t->palette->surface);
        screen_fill_round_rect(mx, my, r, r, r / 2, t->accent);
        screen_draw_icon(update ? ICON_REFRESH : ICON_CHECK, mx + (r - 12) / 2, my + (r - 12) / 2, 12, t->onAccent);
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
    kbd_display(_("Search the Store"), list_get_search(view), SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, 63, data, store_search_done);
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

static void store_close(ui_view* view, store_data* store, linked_list* items) {
    ui_pop();
    store_clear_items(items);
    list_destroy(view);

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

    // The rows point into the old apps: they go first.
    store_clear_items(items);
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
            if(app->dsxUrl != NULL) {
                store_dsx_name(app->dsxUrl, scan[i].dsxName, sizeof(scan[i].dsxName));
            }

            string_copy(scan[i].version, app->version, sizeof(scan[i].version));
            store_record* record = store_record_find(store, app->slug);
            if(record != NULL) {
                string_copy(scan[i].recCia, record->cia, sizeof(scan[i].recCia));
                string_copy(scan[i].recDsx, record->dsx, sizeof(scan[i].recDsx));
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

    store_fill(store, view, items, keep);
    store->resort = true;
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

    store->kinds = settings_get()->store.kinds;
    store->categories = settings_get()->store.categories;
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
