#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <3ds.h>
#include <jansson.h>

#include "resources.h"
#include "section.h"
#include "storelist.h"
#include "../core/core.h"

// The Store's first screen: the stores to open. Universal-DB comes first and stays; the user's own (any .unistore, the
// format Universal-Updater reads) follow, added by a QR code or by typing the link, and can be changed or deleted. A store
// added is downloaded once to check it is one, and its name, author and description come from its storeInfo.

#define STORELIST_PATH "sdmc:" APP_DATA_DIR "stores.json"
#define STORELIST_DIR "sdmc:" APP_DATA_DIR "stores"
#define STORELIST_MAX 16                     // The user's own.
#define STORELIST_CHECK_MAX (3 * 1024 * 1024)

static const store_source storelist_universal_db = {
        "",
        "https://db.universal-team.net/unistore/universal-db.unistore",
        "https://db.universal-team.net/unistore/universal-db-info.json",
        "Universal-DB",
        "Universal-Team",
        "An online database of 3DS and DS homebrew."
};

void store_source_file(const store_source* source, store_file which, char* out, size_t size) {
    static const char* const universalDb[STORE_FILE_COUNT] = {"store-cache.json", "store-icons.pack", "store-icons/sheets.rev", "store-installed.json"};
    static const char* const own[STORE_FILE_COUNT] = {"cache.json", "icons.pack", "sheets.rev", "installed.json"};
    if(source->key[0] == '\0') {
        snprintf(out, size, "sdmc:" APP_DATA_DIR "%s", universalDb[which]);
    } else {
        snprintf(out, size, STORELIST_DIR "/%s/%s", source->key, own[which]);
    }
}

void store_source_make_dir(const store_source* source) {
    if(source->key[0] == '\0') {
        mkdir("sdmc:" APP_DATA_DIR "store-icons", 0777);
        return;
    }

    char path[FILE_PATH_MAX];
    mkdir(STORELIST_DIR, 0777);
    snprintf(path, sizeof(path), STORELIST_DIR "/%s", source->key);
    mkdir(path, 0777);
}

// A store's files go, and its folder.
static void storelist_remove_files(const store_source* source) {
    if(source->key[0] == '\0') {
        return;
    }

    char path[FILE_PATH_MAX];
    for(u32 i = 0; i < STORE_FILE_COUNT; i++) {
        store_source_file(source, (store_file) i, path, sizeof(path));
        remove(path);
    }

    snprintf(path, sizeof(path), STORELIST_DIR "/%s", source->key);
    rmdir(path);
}

static void storelist_make_key(store_source* source) {
    u32 hash = 2166136261u;
    for(const char* c = source->url; *c != '\0'; c++) {
        hash = (hash ^ (u8) *c) * 16777619u;
    }

    snprintf(source->key, sizeof(source->key), "%08lX", hash);
}

typedef struct {
    store_source stores[STORELIST_MAX + 1];   // [0] is Universal-DB.
    u32 count;
    ui_view* view;
    bool refill;
    char select[STORE_SOURCE_URL_MAX];        // The store to select once the list is filled again.
    int target;                               // The store an option or a check is about (-1: a new one).
} storelist_data;

static const char* storelist_str(json_t* object, const char* key) {
    json_t* value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : "";
}

static void storelist_load(storelist_data* data) {
    data->stores[0] = storelist_universal_db;
    data->count = 1;

    json_error_t error;
    json_t* root = json_load_file(STORELIST_PATH, 0, &error);
    size_t index;
    json_t* entry;
    json_array_foreach(root, index, entry) {
        if(data->count > STORELIST_MAX || !json_is_object(entry) || storelist_str(entry, "url")[0] == '\0') {
            continue;
        }

        store_source* source = &data->stores[data->count++];
        memset(source, 0, sizeof(*source));
        string_copy(source->url, storelist_str(entry, "url"), sizeof(source->url));
        string_copy(source->infoUrl, storelist_str(entry, "info"), sizeof(source->infoUrl));
        string_copy(source->title, storelist_str(entry, "title"), sizeof(source->title));
        string_copy(source->author, storelist_str(entry, "author"), sizeof(source->author));
        string_copy(source->description, storelist_str(entry, "description"), sizeof(source->description));
        storelist_make_key(source);
    }

    json_decref(root);
}

static void storelist_save(storelist_data* data) {
    json_t* root = json_array();
    for(u32 i = 1; i < data->count; i++) {
        const store_source* source = &data->stores[i];
        json_array_append_new(root, json_pack("{s:s,s:s,s:s,s:s,s:s}", "url", source->url, "info", source->infoUrl, "title", source->title,
                                              "author", source->author, "description", source->description));
    }

    json_dump_file(root, STORELIST_PATH, JSON_INDENT(2));
    json_decref(root);
}

// ---- drawing ----

static void storelist_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    const store_source* source = (const store_source*) item->data;

    u32 ink = 0;
    u32 muted = 0;
    widget_item_frame(x, y, width, height, selected, &ink, &muted);

    float box = height - 10;
    screen_fill_round_rect(x + 6, y + 5, box, box, box / 4, selected ? THEME_ALPHA(t->onAccent, 0x30) : t->accentSoft);
    screen_draw_icon(source != NULL ? ICON_BUILDING_STORE : ICON_PLUS, x + 6 + (box - 16) / 2, y + 5 + (box - 16) / 2, 16, selected ? t->onAccent : t->accent);

    float tx = x + 6 + box + 10;
    float right = x + width - 10;
    float lineH = widget_text_height(WIDGET_TEXT_SCALE);
    if(source == NULL) {
        widget_text_fit(_("Add a store"), tx, y + (height - lineH) / 2, WIDGET_TEXT_SCALE, ink, right - tx);
        return;
    }

    float smallH = widget_text_height(WIDGET_SMALL_SCALE);
    float top = y + (height - lineH - smallH - 1) / 2;
    widget_text_fit(source->title, tx, top, WIDGET_TEXT_SCALE, ink, right - tx);

    char sub[160];
    if(source->author[0] != '\0') {
        snprintf(sub, sizeof(sub), _("by %s"), source->author);
    } else {
        string_copy(sub, source->url, sizeof(sub));
    }

    widget_text_fit(sub, tx, top + lineH + 1, WIDGET_SMALL_SCALE, muted, right - tx);
}

static void storelist_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    const theme_current* t = theme();
    if(selected == NULL) {
        return;
    }

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = y2 - y1 - 28;
    widget_card(cardX, cardY, cardW, cardH);

    float textX = cardX + 16;
    float right = cardX + cardW - 14;
    float y = cardY + 12;
    const store_source* source = (const store_source*) selected->data;
    if(source == NULL) {
        widget_text_fit(_("Add a store"), textX, y, WIDGET_TITLE_SCALE, t->palette->text, right - textX);
        y += widget_text_height(WIDGET_TITLE_SCALE) + 10;
        screen_draw_text_wrap(_("Any store in the Universal-Updater format (a .unistore file): scan its QR code or type its link."), textX, y,
                              WIDGET_TEXT_SCALE, t->palette->text, right);
        return;
    }

    widget_text_fit(source->title, textX, y, WIDGET_TITLE_SCALE, t->palette->text, right - textX);
    y += widget_text_height(WIDGET_TITLE_SCALE) + 4;
    if(source->author[0] != '\0') {
        char by[160];
        snprintf(by, sizeof(by), _("by %s"), source->author);
        widget_text_fit(by, textX, y, WIDGET_SMALL_SCALE, t->palette->textMuted, right - textX);
        y += widget_text_height(WIDGET_SMALL_SCALE) + 8;
    }

    if(source->description[0] != '\0') {
        // Universal-DB's own is translated; another store's is as it wrote it.
        const char* description = source->key[0] == '\0' ? _("An online database of 3DS and DS homebrew.") : source->description;
        screen_draw_text_wrap(description, textX, y, WIDGET_TEXT_SCALE, t->palette->text, right);
    }

    float smallH = widget_text_height(WIDGET_SMALL_SCALE);
    float bottom = cardY + cardH - 12 - smallH;
    widget_text_fit(source->url, textX, bottom, WIDGET_SMALL_SCALE, t->palette->textMuted, right - textX);
    if(source->key[0] == '\0') {
        // The credit promised to Universal-Team.
        screen_draw_text_wrap(_("Universal-DB is made and kept by Universal-Team (universal-team.net), the makers of Universal-Updater. Thank you!"),
                              textX, bottom - smallH * 3 - 14, WIDGET_SMALL_SCALE, t->palette->textMuted, right);
    }
}

// ---- the list ----

static void storelist_clear(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static void storelist_fill(storelist_data* data, linked_list* items) {
    storelist_clear(items);
    list_item* select = NULL;
    for(u32 i = 0; i <= data->count; i++) {
        list_item* item = (list_item*) calloc(1, sizeof(list_item));
        if(item == NULL) {
            continue;
        }

        // The last row adds one.
        item->data = i < data->count ? &data->stores[i] : NULL;
        string_copy(item->name, i < data->count ? data->stores[i].title : _("Add a store"), sizeof(item->name));
        item->color = COLOR_TEXT;
        linked_list_add(items, item);
        if(i < data->count && data->select[0] != '\0' && strcmp(data->stores[i].url, data->select) == 0) {
            select = item;
        }
    }

    if(select != NULL) {
        list_select(data->view, select);
    }

    data->select[0] = '\0';
}

// ---- checking a store before it goes in the list ----

typedef struct {
    storelist_data* list;
    store_source source;
    int target;
    volatile bool done;
    Result res;
} storelist_check;

static void storelist_check_thread(void* arg) {
    storelist_check* check = (storelist_check*) arg;
    char* text = (char*) malloc(STORELIST_CHECK_MAX);
    if(text == NULL) {
        check->res = R_APP_OUT_OF_MEMORY;
        check->done = true;
        return;
    }

    u32 size = 0;
    if(R_SUCCEEDED(check->res = http_download_buffer(check->source.url, &size, text, STORELIST_CHECK_MAX))) {
        json_error_t error;
        json_t* root = size < STORELIST_CHECK_MAX ? json_loadb(text, size, 0, &error) : NULL;
        json_t* info = json_object_get(root, "storeInfo");
        if(!json_is_array(json_object_get(root, "storeContent")) || !json_is_object(info)) {
            check->res = R_APP_PARSE_FAILED;
        } else {
            store_source* source = &check->source;
            string_copy(source->title, storelist_str(info, "title"), sizeof(source->title));
            string_copy(source->author, storelist_str(info, "author"), sizeof(source->author));
            string_copy(source->description, storelist_str(info, "description"), sizeof(source->description));
            const char* infoUrl = storelist_str(info, "infoURL");
            string_copy(source->infoUrl, strncmp(infoUrl, "http", 4) == 0 ? infoUrl : "", sizeof(source->infoUrl));
            if(source->title[0] == '\0') {
                // No name: the file's.
                const char* slash = strrchr(source->url, '/');
                string_copy(source->title, slash != NULL && slash[1] != '\0' ? slash + 1 : source->url, sizeof(source->title));
            }
        }

        json_decref(root);
    }

    free(text);
    check->done = true;
}

static void storelist_check_update(ui_view* view, void* data, float* progress, char* text) {
    storelist_check* check = (storelist_check*) data;
    snprintf(text, PROGRESS_TEXT_MAX, "%s", _("Checking the store..."));
    if(!check->done) {
        return;
    }

    ui_pop();
    info_destroy(view);

    storelist_data* list = check->list;
    if(R_FAILED(check->res)) {
        error_display_res(NULL, NULL, check->res, _("No store could be read at that link."));
        free(check);
        return;
    }

    if(check->target > 0 && (u32) check->target < list->count) {
        // A changed link: the files of the old one go.
        store_source* old = &list->stores[check->target];
        if(strcmp(old->key, check->source.key) != 0) {
            storelist_remove_files(old);
        }

        *old = check->source;
    } else if(list->count <= STORELIST_MAX) {
        list->stores[list->count++] = check->source;
    }

    storelist_save(list);
    string_copy(list->select, check->source.url, sizeof(list->select));
    list->refill = true;
    free(check);
}

// `target`: the store whose link changes, or -1 for a new one.
static void storelist_check_start(storelist_data* list, const char* given, int target) {
    // The link as typed or read: without spaces or line breaks around it, https:// when it has no scheme.
    while(*given == ' ' || *given == '\n' || *given == '\r' || *given == '\t') {
        given++;
    }

    char url[STORE_SOURCE_URL_MAX];
    snprintf(url, sizeof(url), "%s%s", strstr(given, "://") != NULL ? "" : "https://", given);
    size_t len = strlen(url);
    while(len > 0 && (url[len - 1] == ' ' || url[len - 1] == '\n' || url[len - 1] == '\r' || url[len - 1] == '\t')) {
        url[--len] = '\0';
    }

    if(strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) {
        error_display(NULL, NULL, _("No store could be read at that link."));
        return;
    }

    for(u32 i = 0; i < list->count; i++) {
        if((int) i != target && strcmp(list->stores[i].url, url) == 0) {
            prompt_display_notify(_("Store"), _("That store is already in the list."), COLOR_TEXT, NULL, NULL, NULL);
            return;
        }
    }

    if(target < 0 && list->count > STORELIST_MAX) {
        prompt_display_notify(_("Store"), _("The list is full: delete a store first."), COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    storelist_check* check = (storelist_check*) calloc(1, sizeof(storelist_check));
    if(check == NULL) {
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("No store could be read at that link."));
        return;
    }

    check->list = list;
    check->target = target;
    string_copy(check->source.url, url, sizeof(check->source.url));
    storelist_make_key(&check->source);

    // On the app's core (core 2 on a New 3DS), below the UI.
    bool isNew = false;
    Thread thread = NULL;
    if(R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
        thread = threadCreate(storelist_check_thread, check, 0x10000, 0x31, 2, true);
    }

    if(thread == NULL) {
        thread = threadCreate(storelist_check_thread, check, 0x10000, 0x31, -2, true);
    }

    if(thread == NULL) {
        free(check);
        error_display_res(NULL, NULL, R_APP_THREAD_CREATE_FAILED, _("No store could be read at that link."));
        return;
    }

    info_display(_("Store"), "", false, check, storelist_check_update, NULL);
}

// ---- adding, changing and deleting ----

static storelist_data* storelist_active = NULL;

static void storelist_qr_found(void* data, const char* text) {
    if(storelist_active != NULL) {
        storelist_check_start(storelist_active, text, -1);
    }
}

static void storelist_add_qr(void* data) {
    qr_scan("Add a store", "Point the camera at the store's QR code.", NULL, storelist_qr_found);
}

static void storelist_typed(ui_view* view, void* data, SwkbdButton button, const char* response) {
    if(button == SWKBD_BUTTON_CONFIRM && storelist_active != NULL) {
        storelist_check_start(storelist_active, response, storelist_active->target);
    }
}

static void storelist_add_typed(void* data) {
    storelist_active->target = -1;
    osk_display(_("The store's link (.unistore)"), "https://", STORE_SOURCE_URL_MAX - 1, OSK_NOT_BLANK, NULL, storelist_typed);
}

static void storelist_add(storelist_data* data) {
    sheet_action actions[2] = {
            {ICON_QRCODE, _("Scan a QR code"), false, storelist_add_qr, false, _("The store's QR code, as Universal-Updater reads it.")},
            {ICON_KEYBOARD, _("Type the link"), false, storelist_add_typed, false, _("The address of its .unistore file.")},
    };

    sheet_display_list(_("Add a store"), 0, actions, 2, data);
}

static void storelist_edit(void* data) {
    storelist_data* list = storelist_active;
    if(list == NULL || list->target <= 0 || (u32) list->target >= list->count) {
        return;
    }

    osk_display(_("The store's link (.unistore)"), list->stores[list->target].url, STORE_SOURCE_URL_MAX - 1, OSK_NOT_BLANK, NULL, storelist_typed);
}

static void storelist_delete_onresponse(ui_view* view, void* data, u32 response) {
    storelist_data* list = storelist_active;
    if(response != PROMPT_YES || list == NULL || list->target <= 0 || (u32) list->target >= list->count) {
        return;
    }

    sheet_confirmed();
    storelist_remove_files(&list->stores[list->target]);
    for(u32 i = (u32) list->target; i + 1 < list->count; i++) {
        list->stores[i] = list->stores[i + 1];
    }

    list->count--;
    storelist_save(list);
    list->refill = true;
}

static void storelist_delete(void* data) {
    storelist_data* list = storelist_active;
    if(list == NULL || list->target <= 0 || (u32) list->target >= list->count) {
        return;
    }

    static char question[256];
    snprintf(question, sizeof(question), _("Delete %s from your stores? What it keeps on the SD card goes too (not the apps you got from it)."),
             list->stores[list->target].title);
    prompt_display_yes_no(_("Delete"), question, COLOR_TEXT, NULL, NULL, storelist_delete_onresponse);
}

// The options of one of the user's stores (Universal-DB has none: it stays).
static void storelist_options(storelist_data* data, list_item* item) {
    const store_source* source = item != NULL ? (const store_source*) item->data : NULL;
    if(source == NULL || source->key[0] == '\0') {
        return;
    }

    data->target = (int) (source - data->stores);
    sheet_action actions[2] = {
            {ICON_PENCIL, _("Change the link"), false, storelist_edit, false, NULL},
            {ICON_TRASH, _("Delete"), true, storelist_delete, false, NULL},
    };

    sheet_display_list(source->title, 0, actions, 2, data);
}

static void storelist_long_press(ui_view* view, void* data, linked_list* items, list_item* item) {
    storelist_options((storelist_data*) data, item);
}

static void storelist_add_tool(ui_view* view, void* data) {
    storelist_add((storelist_data*) data);
}

static void storelist_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    storelist_data* list = (storelist_data*) data;
    view->name = _("Store");
    view->info = _("\uE000 open   \uE002 options   \uE001 back");

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        storelist_clear(items);
        list_destroy(view);
        storelist_active = NULL;
        free(list);
        return;
    }

    if(list->refill) {
        list->refill = false;
        storelist_fill(list, items);
        return;
    }

    if(selected != NULL && selectedTouched) {
        const store_source* source = (const store_source*) selected->data;
        if(source == NULL) {
            storelist_add(list);
        } else {
            store_open_source(source);
        }

        return;
    }

    if(hidKeysDown() & KEY_X) {
        storelist_options(list, selected);
    }
}

void store_open() {
    storelist_data* data = (storelist_data*) calloc(1, sizeof(storelist_data));
    if(data == NULL) {
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, _("Failed to open the Store."));
        return;
    }

    storelist_load(data);
    data->target = -1;
    data->refill = true;
    storelist_active = data;

    ui_view* view = list_display(_("Store"), "", data, storelist_update, storelist_draw_top);
    if(view == NULL) {
        storelist_active = NULL;
        free(data);
        return;
    }

    data->view = view;
    list_style style = {
            .rowHeight = 40,
            .drawRow = storelist_draw_row,
    };
    list_set_style(view, &style);
    list_set_long_press(view, storelist_long_press);
    list_show_title_bar(view);
    list_add_tool(view, ICON_PLUS, NULL, storelist_add_tool);
}
