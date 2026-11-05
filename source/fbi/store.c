#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <jansson.h>

#include "resources.h"
#include "section.h"
#include "action/action.h"
#include "../core/core.h"
#include "../core/task/dataop.h"

// Store: the homebrew of Universal-DB (db.universal-team.net), the catalogue Universal-Updater reads, without leaving
// NSA. The catalogue is its "UniStore" file; every app lists the files it publishes and, for each, a script. Only the
// plain ones are used: a CIA (downloaded and installed) or a .3dsx (downloaded into /3ds/<name>/). Apps that come in a
// zip or need several steps are left out.
//
// The file (about 1 MB) is downloaded on a thread, kept on the SD card as the copy to use without a connection, and
// boiled down to the few fields the list needs; the JSON is freed at once.

#define STORE_URL "https://db.universal-team.net/unistore/universal-db.unistore"
#define STORE_CACHE_PATH "sdmc:" APP_DATA_DIR "store.unistore"
#define STORE_RESPONSE_MAX (3 * 1024 * 1024)

typedef struct {
    char title[80];
    char author[48];
    char version[24];
    char category[24];
    char description[300];
    char* ciaUrl;
    char ciaSize[16];
    char* dsxUrl;
    char dsxSize[16];
} store_app;

typedef struct {
    store_app* apps;
    u32 count;
    bool fromCache;   // The catalogue on the SD card, as the download failed.

    volatile bool done;
    volatile bool cancelled;   // The screen was left while loading: the thread frees all when it ends.
    Result res;
    bool started;
    bool populated;
    ui_view* view;
} store_data;

static void store_free_apps(store_app* apps, u32 count) {
    if(apps == NULL) {
        return;
    }

    for(u32 i = 0; i < count; i++) {
        free(apps[i].ciaUrl);
        free(apps[i].dsxUrl);
    }

    free(apps);
}

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

static const char* store_json_string(json_t* object, const char* key) {
    json_t* value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : "";
}

// The URL of the file's first step when that step just downloads it; NULL otherwise. Only https links.
static const char* store_file_url(json_t* file) {
    json_t* script = json_object_get(file, "script");
    if(!json_is_array(script) || json_array_size(script) == 0) {
        return NULL;
    }

    json_t* step = json_array_get(script, 0);
    if(!json_is_object(step) || strcmp(store_json_string(step, "type"), "downloadFile") != 0) {
        return NULL;
    }

    const char* url = store_json_string(step, "file");
    return strncmp(url, "https://", 8) == 0 && strlen(url) < DOWNLOAD_URL_MAX ? url : NULL;
}

static bool store_is_3ds(json_t* info) {
    json_t* console = json_object_get(info, "console");
    size_t index;
    json_t* value;
    json_array_foreach(console, index, value) {
        if(json_is_string(value) && strcmp(json_string_value(value), "3DS") == 0) {
            return true;
        }
    }

    return false;
}

// Reads the catalogue's JSON into the apps for 3DS that have a CIA or a .3dsx. Returns the number found, or -1.
static int store_parse(const char* text, size_t size, store_app** out) {
    json_error_t error;
    json_t* root = json_loadb(text, size, 0, &error);
    if(root == NULL) {
        return -1;
    }

    json_t* content = json_object_get(root, "storeContent");
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
        json_t* info = json_object_get(entry, "info");
        if(!json_is_object(entry) || !json_is_object(info) || !store_is_3ds(info)) {
            continue;
        }

        store_app* app = &apps[count];
        const char* key;
        json_t* file;
        json_object_foreach(entry, key, file) {
            if(!json_is_object(file) || strcmp(key, "info") == 0) {
                continue;
            }

            const char* url = store_file_url(file);
            if(url == NULL) {
                continue;
            }

            if(store_has_ext(key, ".cia") && app->ciaUrl == NULL) {
                app->ciaUrl = strdup(url);
                string_copy(app->ciaSize, store_json_string(file, "size"), sizeof(app->ciaSize));
            } else if(store_has_ext(key, ".3dsx") && app->dsxUrl == NULL) {
                app->dsxUrl = strdup(url);
                string_copy(app->dsxSize, store_json_string(file, "size"), sizeof(app->dsxSize));
            }
        }

        if(app->ciaUrl == NULL && app->dsxUrl == NULL) {
            continue;
        }

        string_copy(app->title, store_json_string(info, "title"), sizeof(app->title));
        string_copy(app->author, store_json_string(info, "author"), sizeof(app->author));
        string_copy(app->version, store_json_string(info, "version"), sizeof(app->version));
        string_copy(app->description, store_json_string(info, "description"), sizeof(app->description));

        json_t* category = json_object_get(info, "category");
        if(json_is_array(category) && json_array_size(category) > 0) {
            string_copy(app->category, json_string_value(json_array_get(category, 0)) != NULL ? json_string_value(json_array_get(category, 0)) : "",
                        sizeof(app->category));
        }

        if(app->title[0] != '\0') {
            count++;
        } else {
            free(app->ciaUrl);
            free(app->dsxUrl);
            memset(app, 0, sizeof(*app));
        }
    }

    json_decref(root);
    *out = apps;
    return (int) count;
}

static void store_write_cache(const char* text, size_t size) {
    FILE* file = fopen(STORE_CACHE_PATH, "wb");
    if(file != NULL) {
        fwrite(text, 1, size, file);
        fclose(file);
    }
}

static char* store_read_cache(size_t* size) {
    FILE* file = fopen(STORE_CACHE_PATH, "rb");
    if(file == NULL) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* text = length > 0 && length < STORE_RESPONSE_MAX ? (char*) malloc((size_t) length) : NULL;
    if(text != NULL && fread(text, 1, (size_t) length, file) == (size_t) length) {
        *size = (size_t) length;
    } else {
        free(text);
        text = NULL;
    }

    fclose(file);
    return text;
}

static void store_load_thread(void* arg) {
    store_data* data = (store_data*) arg;

    char* text = (char*) malloc(STORE_RESPONSE_MAX);
    if(text == NULL) {
        data->res = R_APP_OUT_OF_MEMORY;
        if(data->cancelled) {
            free(data);
        } else {
            data->done = true;
        }

        return;
    }

    u32 size = 0;
    Result res = http_download_buffer(STORE_URL, &size, text, STORE_RESPONSE_MAX);
    if(R_SUCCEEDED(res) && size < STORE_RESPONSE_MAX) {
        int count = store_parse(text, size, &data->apps);
        if(count >= 0) {
            data->count = (u32) count;
            store_write_cache(text, size);
            data->res = 0;
        } else {
            data->res = R_APP_PARSE_FAILED;
        }
    } else {
        data->res = R_FAILED(res) ? res : R_APP_OUT_OF_RANGE;
    }

    free(text);

    // No connection (or a bad answer): the copy from the last time.
    if(R_FAILED(data->res)) {
        size_t cachedSize = 0;
        char* cached = store_read_cache(&cachedSize);
        if(cached != NULL) {
            int count = store_parse(cached, cachedSize, &data->apps);
            if(count >= 0) {
                data->count = (u32) count;
                data->fromCache = true;
                data->res = 0;
            }

            free(cached);
        }
    }

    if(data->cancelled) {
        store_free_apps(data->apps, data->count);
        free(data);
        return;
    }

    data->done = true;
}

// ---- drawing ----

static store_app* store_of(const list_item* item) {
    return (store_app*) item->data;
}

static void store_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    store_app* app = store_of(item);

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    screen_draw_icon(app->ciaUrl != NULL ? ICON_PACKAGE : ICON_APPS, x + 9, y + (height - 16) / 2, 16, selected ? t->onAccent : t->accent);

    float tx = x + 33;
    float right = x + width - 9;

    // What it comes as.
    const char* kind = app->ciaUrl != NULL && app->dsxUrl != NULL ? "CIA 3DSX" : app->ciaUrl != NULL ? "CIA" : "3DSX";
    float kindW = widget_text_width(kind, WIDGET_SMALL_SCALE);
    screen_draw_text(kind, right - kindW, y + (height - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, muted);
    right -= kindW + 8;

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
    widget_text_fit(app->title, textX, cardY + 12, WIDGET_TITLE_SCALE, t->palette->text, right - textX);

    float y = cardY + 12 + widget_text_height(WIDGET_TITLE_SCALE) + 6;
    char line[160];
    snprintf(line, sizeof(line), "%s%s%s", app->author, app->author[0] != '\0' && app->version[0] != '\0' ? "  -  " : "", app->version);
    widget_text_fit(line, textX, y, WIDGET_SMALL_SCALE, t->palette->textMuted, right - textX);
    y += widget_text_height(WIDGET_SMALL_SCALE) + 8;

    screen_draw_text_wrap(app->description, textX, y, WIDGET_TEXT_SCALE, t->palette->text, right);

    // What it offers, at the bottom.
    float by = cardY + cardH - widget_text_height(WIDGET_SMALL_SCALE) - 14;
    float bx = textX;
    if(app->ciaUrl != NULL) {
        char text[40];
        snprintf(text, sizeof(text), "CIA %s", app->ciaSize);
        widget_badge(bx, by, text, t->accent, t->onAccent);
        bx += widget_text_width(text, WIDGET_SMALL_SCALE) + 18;
    }

    if(app->dsxUrl != NULL) {
        char text[40];
        snprintf(text, sizeof(text), "3DSX %s", app->dsxSize);
        widget_badge(bx, by, text, t->accent, t->onAccent);
        bx += widget_text_width(text, WIDGET_SMALL_SCALE) + 18;
    }

    if(app->category[0] != '\0') {
        widget_badge(bx, by, app->category, t->palette->border, t->palette->text);
    }
}

// ---- installing ----

static void store_start(const char* url, const char* what) {
    char message[160];
    snprintf(message, sizeof(message), "%s", what);
    action_install_url(message, url, NULL, NULL, NULL, NULL, NULL);
}

static void store_install_cia(void* data) {
    store_app* app = (store_app*) data;
    char message[200];
    snprintf(message, sizeof(message), _("Download and install %s?"), app->title);
    store_start(app->ciaUrl, message);
}

static void store_download_dsx(void* data) {
    store_app* app = (store_app*) data;
    char message[200];
    snprintf(message, sizeof(message), _("Download %s into /3ds/?"), app->title);
    store_start(app->dsxUrl, message);
}

static void store_open_app(store_app* app) {
    if(app->ciaUrl != NULL && app->dsxUrl == NULL) {
        store_install_cia(app);
    } else if(app->ciaUrl == NULL && app->dsxUrl != NULL) {
        store_download_dsx(app);
    } else {
        sheet_action actions[] = {
                {ICON_DOWNLOAD, _("Install the CIA"), false, store_install_cia, false, _("Installs it as a title on the console.")},
                {ICON_WORLD_DOWNLOAD, _("Download the 3DSX"), false, store_download_dsx, false, _("Saves it in /3ds/ to run from the Homebrew Launcher.")},
        };
        sheet_display_list(app->title, 0, actions, 2, app);
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

static void store_close(ui_view* view, store_data* store, linked_list* items) {
    ui_pop();

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }

    list_destroy(view);

    // The thread owns the data until it is done: leaving while it runs hands it the cleaning.
    if(store->started && !store->done) {
        store->cancelled = true;
        return;
    }

    store_free_apps(store->apps, store->count);
    free(store);
}

static void store_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    store_data* store = (store_data*) data;

    view->name = _("Store");
    view->info = _(" get    back");

    if(hidKeysDown() & KEY_B) {
        if(list_get_search(view)[0] != '\0') {
            list_set_search(view, "");
            return;
        }

        store_close(view, store, items);
        return;
    }

    if(!store->started) {
        if(threadCreate(store_load_thread, store, 0x10000, 0x18, 1, true) == NULL) {
            error_display_res(NULL, NULL, R_APP_THREAD_CREATE_FAILED, _("Failed to open the Store."));
            store_close(view, store, items);
            return;
        }

        store->started = true;
    }

    if(store->done && !store->populated) {
        store->populated = true;

        if(R_FAILED(store->res)) {
            error_display_res(NULL, NULL, store->res, _("Failed to download the Store's catalogue."));
            store_close(view, store, items);
            return;
        }

        for(u32 i = 0; i < store->count; i++) {
            list_item* item = (list_item*) calloc(1, sizeof(list_item));
            if(item != NULL) {
                string_copy(item->name, store->apps[i].title, sizeof(item->name));
                item->color = COLOR_TEXT;
                item->data = &store->apps[i];
                linked_list_add(items, item);
            }
        }

        if(store->fromCache) {
            prompt_display_notify(_("Store"), _("No connection: this is the catalogue saved from last time."), COLOR_TEXT, NULL, NULL, NULL);
        }
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        store_open_app(store_of(selected));
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

    ui_view* view = list_display(_("Store"), _(" get    back"), store, store_update, store_draw_top);
    if(view == NULL) {
        free(store);
        return;
    }

    store->view = view;

    list_style style = {
            .rowHeight = 36,
            .drawRow = store_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
    list_add_tool(view, ICON_SEARCH, store_search_active, store_search_open);
}
