#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "wifislot.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "../libs/qrcodegen/qrcodegen.h"
#include "../libs/quirc/quirc_internal.h"

// System -> Network: the console's three Wi-Fi slots, then the copies saved on
// the SD card (NSA's and WiFiManager's). The top screen shows the selected one;
// A or a tap opens its actions (writing to the console in red, asked first);
// X scans a phone's "share network" QR code; Y shows the passwords.

typedef enum {
    ENTRY_SLOT,
    ENTRY_BACKUP
} wifi_entry_kind;

typedef struct {
    wifi_entry_kind kind;
    u32 index;              // The slot (0..2).
    bool readable;          // The slot could be read.
    wifi_backup backup;     // A backup; for a slot, only backup.slot is used.
} wifi_entry;

typedef struct {
    ui_view* view;
    linked_list* items;
    bool populated;
    bool showPasswords;

    // The entry the sheet, an edit or a choice of slot is for.
    wifi_entry* entry;

    // A network made from a QR code or an edit, waiting to be written or saved.
    wifi_slot pending;
    u32 target;

    // An edit in progress.
    char editSsid[0x21];
    char editPassword[0x41];
    wifi_security editSecurity;
} wifi_data;

#define ROW_HEIGHT 40

static wifi_slot* wifi_entry_slot(wifi_entry* entry) {
    return &entry->backup.slot;
}

static void wifi_populate(wifi_data* data);

// ---- the list ----

static void wifi_clear_items(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        free(item->data);
        free(item);
        linked_list_iter_remove(&iter);
    }
}

static void wifi_add_entry(linked_list* items, wifi_entry* entry) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    if(item == NULL) {
        free(entry);
        return;
    }

    wifi_slot* slot = wifi_entry_slot(entry);
    if(entry->kind == ENTRY_SLOT) {
        snprintf(item->name, sizeof(item->name), "%s", slot->exists ? slot->ssid : _("Empty"));
    } else {
        snprintf(item->name, sizeof(item->name), "%s", entry->backup.name);
    }

    item->color = COLOR_TEXT;
    item->data = entry;
    linked_list_add(items, item);
}

static void wifi_populate(wifi_data* data) {
    wifi_clear_items(data->items);

    for(u32 i = 0; i < WIFI_SLOT_COUNT; i++) {
        wifi_entry* entry = (wifi_entry*) calloc(1, sizeof(wifi_entry));
        if(entry == NULL) {
            continue;
        }

        entry->kind = ENTRY_SLOT;
        entry->index = i;
        entry->readable = wifi_slot_read(i, wifi_entry_slot(entry));
        wifi_add_entry(data->items, entry);
    }

    wifi_backup* backups = NULL;
    u32 count = wifi_backup_list(&backups);
    for(u32 i = 0; i < count; i++) {
        wifi_entry* entry = (wifi_entry*) calloc(1, sizeof(wifi_entry));
        if(entry == NULL) {
            continue;
        }

        entry->kind = ENTRY_BACKUP;
        entry->backup = backups[i];
        entry->readable = true;
        wifi_add_entry(data->items, entry);
    }

    free(backups);
    data->populated = true;
}

static void wifi_date_text(char* out, size_t size, s64 saved) {
    if(saved == 0) {
        snprintf(out, size, "-");
        return;
    }

    time_t when = (time_t) saved;
    struct tm* tm = gmtime(&when);
    strftime(out, size, "%Y-%m-%d %H:%M", tm);
}

static void wifi_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    wifi_entry* entry = (wifi_entry*) item->data;
    wifi_slot* slot = wifi_entry_slot(entry);

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    // A console slot or a file.
    float tile = height - 10;
    screen_fill_round_rect(x + 5, y + 5, tile, tile, tile / 6, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft);
    u32 icon = entry->kind == ENTRY_SLOT ? (slot->exists ? ICON_WIFI : ICON_WIFI_OFF) : ICON_DEVICE_FLOPPY;
    screen_draw_icon(icon, x + 5 + (tile - 16) / 2, y + 5 + (tile - 16) / 2, 16, selected ? t->onAccent : t->accent);

    float tx = x + 5 + tile + 8;
    float right = x + width - 8;

    char tag[32];
    if(entry->kind == ENTRY_SLOT) {
        snprintf(tag, sizeof(tag), _("Slot %lu"), entry->index + 1);
    } else {
        snprintf(tag, sizeof(tag), "%s", entry->backup.legacy ? "WiFiManager" : "SD");
    }

    float tagW = widget_text_width(tag, WIDGET_SMALL_SCALE) + 12;
    widget_badge(right - tagW, y + 4, tag, selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft, selected ? t->onAccent : t->accent);

    bool empty = entry->kind == ENTRY_SLOT && !slot->exists;
    widget_text_fit(item->name, tx, y + 4, WIDGET_TEXT_SCALE, empty ? muted : ink, right - tagW - 6 - tx);

    // Second line: the security, and for a copy when it was saved.
    char line[96];
    if(entry->kind == ENTRY_SLOT && !entry->readable) {
        snprintf(line, sizeof(line), "%s", _("Cannot be read"));
    } else if(empty) {
        snprintf(line, sizeof(line), "%s", _("No network"));
    } else if(entry->kind == ENTRY_BACKUP) {
        char date[32];
        wifi_date_text(date, sizeof(date), entry->backup.saved);
        snprintf(line, sizeof(line), "%s   %s", wifi_encryption_name(slot->encryption), date);
    } else {
        snprintf(line, sizeof(line), "%s", wifi_encryption_name(slot->encryption));
    }

    widget_text_fit(line, tx, y + height - 4 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, muted, right - tx);
}

// ---- the top screen: the selected network ----

#define CELL_HEIGHT 34
#define CELL_GAP 6

static void wifi_cell(float x, float y, float w, const char* label, const char* value) {
    const theme_current* t = theme();
    screen_frame_round_rect(x, y, w, CELL_HEIGHT, t->def->radiusSmall, t->def->border, t->palette->border, t->palette->surface);
    screen_draw_text(label, x + 8, y + 3, WIDGET_SMALL_SCALE, t->palette->textMuted);
    float scale = widget_text_width(value, WIDGET_TEXT_SCALE) <= w - 14 ? WIDGET_TEXT_SCALE : WIDGET_SMALL_SCALE;
    widget_text_fit(value, x + 8, y + 3 + widget_text_height(WIDGET_SMALL_SCALE), scale, t->palette->text, w - 14);
}

static void wifi_grid(float x, float y, float w, u32 column, u32 row, u32 span, const char* label, const char* value) {
    float cellW = (w - 2 * CELL_GAP) / 3;
    wifi_cell(x + column * (cellW + CELL_GAP), y + row * (CELL_HEIGHT + CELL_GAP), cellW * span + CELL_GAP * (span - 1), label, value);
}

static void wifi_ip_text(char* out, size_t size, const u8* ip) {
    if((ip[0] | ip[1] | ip[2] | ip[3]) == 0) {
        snprintf(out, size, "-");
    } else {
        snprintf(out, size, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
}

static void wifi_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL || selected->data == NULL) {
        return;
    }

    const theme_current* t = theme();
    wifi_data* listData = (wifi_data*) data;
    wifi_entry* entry = (wifi_entry*) selected->data;
    wifi_slot* slot = wifi_entry_slot(entry);
    const wifi_slot_raw* raw = &slot->raw;

    float x = x1 + 10;
    float w = x2 - x1 - 20;
    float cardY = y1 + 8;
    float cardH = 44;
    widget_card(x, cardY, w, cardH);

    float tile = 36;
    float depth = screen_get_depth();
    screen_set_depth(depth + 1);
    screen_fill_round_rect(x + 5, cardY + 4, tile, tile, t->def->radius, t->accentSoft);
    screen_draw_icon(entry->kind == ENTRY_SLOT ? ICON_WIFI : ICON_DEVICE_FLOPPY, x + 7, cardY + 6, 32, t->accent);
    screen_set_depth(depth);

    char subtitle[160];
    if(entry->kind == ENTRY_SLOT) {
        snprintf(subtitle, sizeof(subtitle), _("Slot %lu of the console"), entry->index + 1);
    } else {
        const char* file = strrchr(entry->backup.path, '/');
        snprintf(subtitle, sizeof(subtitle), "%s", file != NULL ? file + 1 : entry->backup.path);
    }

    float tx = x + 5 + tile + 10;
    float titleH = widget_text_height(WIDGET_TITLE_SCALE);
    float textY = cardY + (cardH - titleH - 2 - widget_text_height(WIDGET_SMALL_SCALE)) / 2;
    widget_text_fit(slot->exists ? slot->ssid : _("Empty"), tx, textY, WIDGET_TITLE_SCALE, t->palette->text, x + w - 10 - tx);
    widget_text_fit(subtitle, tx, textY + titleH + 2, WIDGET_SMALL_SCALE, t->palette->textMuted, x + w - 10 - tx);

    float y = cardY + cardH + CELL_GAP;
    if(!slot->exists) {
        widget_text_centered(entry->readable ? _("No network in this slot. A: write one.") : _("This slot cannot be read."), x1, x2, y + 20, WIDGET_TEXT_SCALE, t->palette->textMuted);
        return;
    }

    // The password: hidden until Y.
    char password[0x50];
    if(slot->encryption == WIFI_ENCRYPTION_OPEN) {
        snprintf(password, sizeof(password), "%s", _("None"));
    } else if(slot->password[0] == '\0') {
        snprintf(password, sizeof(password), "%s", slot->wps ? _("Not readable (set up with WPS)") : _("Not readable"));
    } else if(listData->showPasswords) {
        snprintf(password, sizeof(password), "%s", slot->password);
    } else {
        size_t len = strlen(slot->password);
        len = len < sizeof(password) - 1 ? len : sizeof(password) - 1;
        memset(password, '*', len);
        password[len] = '\0';
    }

    char ip[24];
    char gateway[24];
    char netmask[24];
    if(raw->dhcp) {
        snprintf(ip, sizeof(ip), "%s", _("Automatic"));
        snprintf(gateway, sizeof(gateway), "%s", _("Automatic"));
        snprintf(netmask, sizeof(netmask), "%s", _("Automatic"));
    } else {
        wifi_ip_text(ip, sizeof(ip), raw->ip);
        wifi_ip_text(gateway, sizeof(gateway), raw->gateway);
        wifi_ip_text(netmask, sizeof(netmask), raw->netmask);
    }

    char dns[48];
    if(raw->autoDns) {
        snprintf(dns, sizeof(dns), "%s", _("Automatic"));
    } else {
        char first[24];
        char second[24];
        wifi_ip_text(first, sizeof(first), raw->dnsPrimary);
        wifi_ip_text(second, sizeof(second), raw->dnsSecondary);
        snprintf(dns, sizeof(dns), "%s, %s", first, second);
    }

    char proxy[0x40];
    if(raw->proxyEnable) {
        char host[0x31];
        memcpy(host, raw->proxyHost, sizeof(raw->proxyHost));
        host[sizeof(raw->proxyHost)] = '\0';
        snprintf(proxy, sizeof(proxy), "%s:%u", host, raw->proxyPort);
    } else {
        snprintf(proxy, sizeof(proxy), "%s", _("Off"));
    }

    char mtu[16];
    snprintf(mtu, sizeof(mtu), "%u", raw->mtu);

    wifi_grid(x, y, w, 0, 0, 2, _("Password"), password);
    wifi_grid(x, y, w, 2, 0, 1, _("Security"), wifi_encryption_name(slot->encryption));
    wifi_grid(x, y, w, 0, 1, 1, _("IP address"), ip);
    wifi_grid(x, y, w, 1, 1, 1, _("Gateway"), gateway);
    wifi_grid(x, y, w, 2, 1, 1, _("Netmask"), netmask);
    wifi_grid(x, y, w, 0, 2, 2, "DNS", dns);
    wifi_grid(x, y, w, 2, 2, 1, _("Proxy"), proxy);

    if(entry->kind == ENTRY_BACKUP) {
        char date[32];
        wifi_date_text(date, sizeof(date), entry->backup.saved);
        wifi_grid(x, y, w, 0, 3, 1, _("Saved"), date);
        wifi_grid(x, y, w, 1, 3, 1, "MTU", mtu);
        wifi_grid(x, y, w, 2, 3, 1, _("Alias"), entry->backup.alias[0] != '\0' ? entry->backup.alias : "-");
    } else {
        wifi_grid(x, y, w, 0, 3, 1, "MTU", mtu);
        wifi_grid(x, y, w, 1, 3, 2, _("Connection test"), raw->connectionTestSuccess ? _("Passed") : _("Not passed"));
    }
}

// ---- writing a network to a slot ----

static void wifi_written(wifi_data* data, Result res) {
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to write the Wi-Fi slot."));
        return;
    }

    wifi_populate(data);
    prompt_display_notify("Success", _("Written. The console uses it after a restart, or after opening its Internet settings."), COLOR_TEXT, NULL, NULL, NULL);
}

static void wifi_write_onresponse(ui_view* view, void* data, u32 response) {
    wifi_data* listData = (wifi_data*) data;
    if(response == PROMPT_YES) {
        wifi_written(listData, wifi_slot_write(listData->target, &listData->pending));
    }
}

// Asks (in red) before replacing the slot `target` with `pending`.
static void wifi_write_ask(wifi_data* data) {
    wifi_slot current;
    bool replaces = wifi_slot_read(data->target, &current) && current.exists;

    static char text[256];
    if(replaces) {
        snprintf(text, sizeof(text), _("Replace slot %lu (%s) with \"%s\"?\n\nThe 3DS only joins 2.4 GHz networks."), data->target + 1, current.ssid, data->pending.ssid);
    } else {
        snprintf(text, sizeof(text), _("Write \"%s\" to slot %lu?\n\nThe 3DS only joins 2.4 GHz networks."), data->pending.ssid, data->target + 1);
    }

    prompt_display_warning(_("Wi-Fi slot"), text, COLOR_TEXT, data, NULL, wifi_write_onresponse);
}

// Which slot: the three, with what each holds.
static void wifi_choose_slot_onresponse(ui_view* view, void* data, u32 response) {
    wifi_data* listData = (wifi_data*) data;
    if(response < WIFI_SLOT_COUNT) {
        listData->target = response;
        wifi_write_ask(listData);
    }
}

static void wifi_choose_slot(wifi_data* data) {
    static char labels[WIFI_SLOT_COUNT][64];
    static const char* options[WIFI_SLOT_COUNT + 1];
    static u32 buttons[WIFI_SLOT_COUNT + 1] = {KEY_A, KEY_X, KEY_Y, KEY_B};
    for(u32 i = 0; i < WIFI_SLOT_COUNT; i++) {
        wifi_slot slot;
        char what[0x24];
        snprintf(what, sizeof(what), "%s", wifi_slot_read(i, &slot) && slot.exists ? slot.ssid : _("empty"));
        snprintf(labels[i], sizeof(labels[i]), _("Slot %lu: %s"), i + 1, what);
        options[i] = labels[i];
    }

    options[WIFI_SLOT_COUNT] = "Cancel";

    static char text[128];
    snprintf(text, sizeof(text), _("Write \"%s\" to which slot?"), data->pending.ssid);
    prompt_display_multi_choice(_("Wi-Fi slot"), text, COLOR_TEXT, options, buttons, WIFI_SLOT_COUNT + 1, data, NULL, wifi_choose_slot_onresponse);
}

// ---- saving to the SD card ----

static void wifi_save_alias_done(ui_view* view, void* data, SwkbdButton button, const char* response) {
    wifi_data* listData = (wifi_data*) data;
    if(button != SWKBD_BUTTON_CONFIRM) {
        return;
    }

    char path[256];
    Result res = wifi_backup_save(&listData->pending, response, path, sizeof(path));
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to save the network."));
        return;
    }

    wifi_populate(listData);

    static char text[320];
    snprintf(text, sizeof(text), _("Saved as %s."), path + strlen("sdmc:"));
    prompt_display_notify("Success", text, COLOR_TEXT, NULL, NULL, NULL);
}

// An optional alias, then the file.
static void wifi_save_pending(wifi_data* data) {
    kbd_display(_("Alias (optional)"), "", SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, 48, data, wifi_save_alias_done);
}

// ---- a QR code on the top screen ----

typedef struct {
    u8 code[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
    bool ok;
    char ssid[0x21];
} wifi_qr_view;

static void wifi_qr_view_update(ui_view* view, void* data, float* progress, char* text) {
    wifi_qr_view* qr = (wifi_qr_view*) data;
    if(hidKeysDown() & (KEY_A | KEY_B)) {
        ui_pop();
        info_destroy(view);
        free(qr);
        return;
    }

    snprintf(text, PROGRESS_TEXT_MAX, _("Scan it with a phone to join \"%s\"."), qr->ssid);
}

static void wifi_qr_view_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    wifi_qr_view* qr = (wifi_qr_view*) data;
    if(!qr->ok) {
        return;
    }

    // Black on white whatever the theme: phones read it best so.
    int size = qrcodegen_getSize(qr->code);
    float room = y2 - y1 - 12;
    float module = (float) (int) (room / (size + 8));
    if(module < 1) {
        module = 1;
    }

    float side = module * (size + 8);
    float left = (float) (int) ((x1 + x2 - side) / 2);
    float top = (float) (int) (y1 + (y2 - y1 - side) / 2);
    screen_fill_rect(left, top, side, side, 0xFFFFFFFF);
    for(int row = 0; row < size; row++) {
        // One rectangle per run of dark modules.
        int start = -1;
        for(int column = 0; column <= size; column++) {
            bool dark = column < size && qrcodegen_getModule(qr->code, column, row);
            if(dark && start < 0) {
                start = column;
            } else if(!dark && start >= 0) {
                screen_fill_rect(left + (start + 4) * module, top + (row + 4) * module, (column - start) * module, module, 0xFF000000);
                start = -1;
            }
        }
    }
}

static void wifi_show_qr(wifi_slot* slot) {
    wifi_qr_view* qr = (wifi_qr_view*) calloc(1, sizeof(wifi_qr_view));
    if(qr == NULL) {
        return;
    }

    char text[256];
    wifi_qr_encode(slot, text, sizeof(text));
    string_copy(qr->ssid, slot->ssid, sizeof(qr->ssid));

    u8 temp[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
    qr->ok = qrcodegen_encodeText(text, temp, qr->code, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, 15, qrcodegen_Mask_AUTO, true);
    info_display(_("Wi-Fi QR code"), " back", false, qr, wifi_qr_view_update, wifi_qr_view_draw_top);
}

// ---- scanning a QR code ----

#define SCAN_WIDTH 400
#define SCAN_HEIGHT 240

typedef struct {
    wifi_data* list;
    struct quirc* qr;
    u32 tex;
    bool capturing;
    capture_cam_data capture;
} wifi_scan;

static void wifi_scan_stop(wifi_scan* scan) {
    if(!scan->capture.finished) {
        svcSignalEvent(scan->capture.cancelEvent);
        while(!scan->capture.finished) {
            svcSleepThread(1000000);
        }
    }

    scan->capturing = false;
}

static void wifi_scan_free(wifi_scan* scan) {
    wifi_scan_stop(scan);
    free(scan->capture.buffer);
    if(scan->tex != 0) {
        screen_unload_texture(scan->tex);
    }

    if(scan->qr != NULL) {
        quirc_destroy(scan->qr);
    }

    free(scan);
}

static void wifi_scan_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    wifi_scan* scan = (wifi_scan*) data;
    if(scan->tex != 0) {
        svcWaitSynchronization(scan->capture.mutex, U64_MAX);
        screen_load_texture_untiled(scan->tex, scan->capture.buffer, SCAN_WIDTH * SCAN_HEIGHT * sizeof(u16), SCAN_WIDTH, SCAN_HEIGHT, GPU_RGB565, false);
        svcReleaseMutex(scan->capture.mutex);
        screen_draw_texture(scan->tex, 0, 0, SCAN_WIDTH, SCAN_HEIGHT);
    }
}

// What to do with a scanned network.
static void wifi_scanned_onresponse(ui_view* view, void* data, u32 response) {
    wifi_data* listData = (wifi_data*) data;
    if(response == 0) {
        wifi_save_pending(listData);
    } else if(response == 1) {
        wifi_choose_slot(listData);
    }
}

static void wifi_scanned(wifi_data* data, const char* payload) {
    char ssid[0x21];
    char password[0x41];
    wifi_security security;
    if(!wifi_qr_parse(payload, ssid, sizeof(ssid), password, sizeof(password), &security)) {
        error_display(NULL, NULL, _("The QR code holds no Wi-Fi network."));
        return;
    }

    Result res = wifi_slot_from_credentials(&data->pending, NULL, ssid, password, security);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("The network in the QR code cannot be used: check its password."));
        return;
    }

    static const char* options[3] = {"Save to the SD card", "Write to a slot", "Cancel"};
    static u32 buttons[3] = {KEY_A, KEY_X, KEY_B};
    static char text[160];
    snprintf(text, sizeof(text), _("Found \"%s\" (%s).\nWhat do you want to do with it?"), ssid, wifi_encryption_name(data->pending.encryption));
    prompt_display_multi_choice(_("Wi-Fi QR code"), text, COLOR_TEXT, options, buttons, 3, data, NULL, wifi_scanned_onresponse);
}

static void wifi_scan_update(ui_view* view, void* data, float* progress, char* text) {
    wifi_scan* scan = (wifi_scan*) data;
    if(hidKeysDown() & KEY_B) {
        ui_pop();
        info_destroy(view);
        wifi_scan_free(scan);
        return;
    }

    if(hidKeysDown() & KEY_X) {
        wifi_scan_stop(scan);
        scan->capture.camera = scan->capture.camera == CAMERA_OUTER ? CAMERA_INNER : CAMERA_OUTER;
        return;
    }

    if(!scan->capturing) {
        Result res = task_capture_cam(&scan->capture);
        if(R_FAILED(res)) {
            ui_pop();
            info_destroy(view);
            wifi_scan_free(scan);
            error_display_res(NULL, NULL, res, "Failed to start camera capture.");
            return;
        }

        scan->capturing = true;
    }

    if(scan->capture.finished) {
        Result res = scan->capture.result;
        ui_pop();
        info_destroy(view);
        wifi_scan_free(scan);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Error while capturing camera frames.");
        }

        return;
    }

    int w = 0;
    int h = 0;
    uint8_t* gray = quirc_begin(scan->qr, &w, &h);
    svcWaitSynchronization(scan->capture.mutex, U64_MAX);
    for(int py = 0; py < h; py++) {
        for(int px = 0; px < w; px++) {
            u16 pixel = scan->capture.buffer[py * SCAN_WIDTH + px];
            gray[py * w + px] = (u8) (((((pixel >> 11) & 0x1F) << 3) + (((pixel >> 5) & 0x3F) << 2) + ((pixel & 0x1F) << 3)) / 3);
        }
    }
    svcReleaseMutex(scan->capture.mutex);
    quirc_end(scan->qr);

    for(int i = 0; i < quirc_count(scan->qr); i++) {
        struct quirc_code code;
        struct quirc_data decoded;
        quirc_extract(scan->qr, i, &code);
        if(quirc_decode(&code, &decoded) == 0) {
            wifi_data* list = scan->list;
            char payload[sizeof(decoded.payload) + 1];
            memcpy(payload, decoded.payload, sizeof(decoded.payload));
            payload[sizeof(decoded.payload)] = '\0';

            ui_pop();
            info_destroy(view);
            wifi_scan_free(scan);
            wifi_scanned(list, payload);
            return;
        }
    }

    snprintf(text, PROGRESS_TEXT_MAX, "%s", _("Point the camera at a Wi-Fi QR code\n(a phone's \"share network\")."));
}

static void wifi_scan_open(wifi_data* data) {
    wifi_scan* scan = (wifi_scan*) calloc(1, sizeof(wifi_scan));
    if(scan == NULL) {
        return;
    }

    scan->list = data;
    scan->capture.width = SCAN_WIDTH;
    scan->capture.height = SCAN_HEIGHT;
    scan->capture.camera = CAMERA_OUTER;
    scan->capture.finished = true;
    scan->qr = quirc_new();
    scan->capture.buffer = (u16*) calloc(1, SCAN_WIDTH * SCAN_HEIGHT * sizeof(u16));
    if(scan->qr == NULL || quirc_resize(scan->qr, SCAN_WIDTH, SCAN_HEIGHT) != 0 || scan->capture.buffer == NULL) {
        wifi_scan_free(scan);
        error_display(NULL, NULL, "Failed to start the QR scanner.");
        return;
    }

    scan->tex = screen_allocate_free_texture();
    info_display(_("Scan a Wi-Fi QR code"), " switch camera    back", false, scan, wifi_scan_update, wifi_scan_draw_top);
}

// ---- editing: name, security, password ----

static void wifi_edit_finish(wifi_data* data) {
    wifi_entry* entry = data->entry;
    Result res = wifi_slot_from_credentials(&data->pending, wifi_entry_slot(entry), data->editSsid, data->editPassword, data->editSecurity);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("A WPA password is 8 to 63 characters."));
        return;
    }

    if(entry->kind == ENTRY_SLOT) {
        data->target = entry->index;
        wifi_write_ask(data);
        return;
    }

    entry->backup.slot = data->pending;
    res = wifi_backup_rewrite(&entry->backup);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to save the network."));
        return;
    }

    wifi_populate(data);
    prompt_display_notify("Success", _("Saved."), COLOR_TEXT, NULL, NULL, NULL);
}

static void wifi_edit_password_done(ui_view* view, void* data, SwkbdButton button, const char* response) {
    wifi_data* listData = (wifi_data*) data;
    if(button == SWKBD_BUTTON_CONFIRM) {
        string_copy(listData->editPassword, response, sizeof(listData->editPassword));
        wifi_edit_finish(listData);
    }
}

// The prompt's answers, in order: WPA2, WPA, open (then Cancel). WEP comes only from QR codes.
static const wifi_security wifi_edit_securities[3] = {WIFI_SECURITY_WPA2, WIFI_SECURITY_WPA, WIFI_SECURITY_OPEN};

static void wifi_edit_security_onresponse(ui_view* view, void* data, u32 response) {
    wifi_data* listData = (wifi_data*) data;
    if(response >= 3) {
        return;
    }

    listData->editSecurity = wifi_edit_securities[response];
    if(listData->editSecurity == WIFI_SECURITY_OPEN) {
        listData->editPassword[0] = '\0';
        wifi_edit_finish(listData);
        return;
    }

    kbd_display(_("Password"), listData->editPassword, SWKBD_TYPE_NORMAL, 0, SWKBD_NOTEMPTY_NOTBLANK, 63, listData, wifi_edit_password_done);
}

static void wifi_edit_ssid_done(ui_view* view, void* data, SwkbdButton button, const char* response) {
    wifi_data* listData = (wifi_data*) data;
    if(button != SWKBD_BUTTON_CONFIRM) {
        return;
    }

    string_copy(listData->editSsid, response, sizeof(listData->editSsid));

    static const char* options[4] = {"WPA2", "WPA", "Open", "Cancel"};
    static u32 buttons[4] = {KEY_A, KEY_X, KEY_Y, KEY_B};
    prompt_display_multi_choice(_("Security"), _("Which security does the network use?"), COLOR_TEXT, options, buttons, 4, listData, NULL, wifi_edit_security_onresponse);
}

static void wifi_edit(wifi_data* data) {
    wifi_slot* slot = wifi_entry_slot(data->entry);
    string_copy(data->editSsid, slot->exists ? slot->ssid : "", sizeof(data->editSsid));
    string_copy(data->editPassword, slot->password, sizeof(data->editPassword));
    data->editSecurity = slot->exists ? wifi_security_of(slot->encryption) : WIFI_SECURITY_WPA2;
    kbd_display(_("Network name (SSID)"), data->editSsid, SWKBD_TYPE_NORMAL, 0, SWKBD_NOTEMPTY_NOTBLANK, 32, data, wifi_edit_ssid_done);
}

// ---- the actions ----

static void wifi_action_save(void* data) {
    wifi_data* listData = (wifi_data*) data;
    listData->pending = *wifi_entry_slot(listData->entry);
    wifi_save_pending(listData);
}

static void wifi_action_qr(void* data) {
    wifi_data* listData = (wifi_data*) data;
    wifi_show_qr(wifi_entry_slot(listData->entry));
}

static void wifi_action_edit(void* data) {
    wifi_edit((wifi_data*) data);
}

static void wifi_action_write(void* data) {
    wifi_data* listData = (wifi_data*) data;
    listData->pending = *wifi_entry_slot(listData->entry);
    wifi_choose_slot(listData);
}

static void wifi_action_scan(void* data) {
    wifi_scan_open((wifi_data*) data);
}

static void wifi_clear_onresponse(ui_view* view, void* data, u32 response) {
    wifi_data* listData = (wifi_data*) data;
    if(response != PROMPT_YES) {
        return;
    }

    Result res = wifi_slot_clear(listData->entry->index);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to write the Wi-Fi slot."));
        return;
    }

    wifi_populate(listData);
    prompt_display_notify("Success", _("Slot emptied."), COLOR_TEXT, NULL, NULL, NULL);
}

static void wifi_action_clear(void* data) {
    wifi_data* listData = (wifi_data*) data;
    static char text[160];
    snprintf(text, sizeof(text), _("Empty slot %lu (%s)?\nThe console forgets this network."), listData->entry->index + 1, wifi_entry_slot(listData->entry)->ssid);
    prompt_display_warning(_("Wi-Fi slot"), text, COLOR_TEXT, listData, NULL, wifi_clear_onresponse);
}

static void wifi_rename_done(ui_view* view, void* data, SwkbdButton button, const char* response) {
    wifi_data* listData = (wifi_data*) data;
    if(button != SWKBD_BUTTON_CONFIRM) {
        return;
    }

    string_copy(listData->entry->backup.alias, response, sizeof(listData->entry->backup.alias));
    Result res = wifi_backup_rewrite(&listData->entry->backup);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to save the network."));
        return;
    }

    wifi_populate(listData);
}

static void wifi_action_rename(void* data) {
    wifi_data* listData = (wifi_data*) data;
    kbd_display(_("Alias (empty: the SSID)"), listData->entry->backup.alias, SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, 48, listData, wifi_rename_done);
}

static void wifi_delete_onresponse(ui_view* view, void* data, u32 response) {
    wifi_data* listData = (wifi_data*) data;
    if(response != PROMPT_YES) {
        return;
    }

    if(remove(listData->entry->backup.path) != 0) {
        error_display(NULL, NULL, _("Failed to delete the file."));
        return;
    }

    wifi_populate(listData);
}

static void wifi_action_delete(void* data) {
    wifi_data* listData = (wifi_data*) data;
    static char text[200];
    snprintf(text, sizeof(text), _("Delete the saved network \"%s\"?"), listData->entry->backup.name);
    prompt_display_yes_no(_("Delete"), text, COLOR_TEXT, listData, NULL, wifi_delete_onresponse);
}

static void wifi_actions_open(wifi_data* data, list_item* selected) {
    wifi_entry* entry = (wifi_entry*) selected->data;
    wifi_slot* slot = wifi_entry_slot(entry);
    data->entry = entry;

    if(entry->kind == ENTRY_SLOT && !entry->readable) {
        return;
    }

    if(entry->kind == ENTRY_SLOT && !slot->exists) {
        sheet_action actions[] = {
                {ICON_QRCODE, _("Write from a QR code"), false, wifi_action_scan, false},
                {ICON_PENCIL, _("Type a network"), false, wifi_action_edit, false},
        };
        sheet_display_list(NULL, 0, actions, sizeof(actions) / sizeof(*actions), data);
        return;
    }

    if(entry->kind == ENTRY_SLOT) {
        sheet_action actions[] = {
                {ICON_DOWNLOAD, _("Save to the SD card"), false, wifi_action_save, false},
                {ICON_QRCODE, _("Show as a QR code"), false, wifi_action_qr, true},
                {ICON_COPY, _("Copy to another slot"), false, wifi_action_write, false},
                {ICON_PENCIL, _("Edit"), true, wifi_action_edit, false},
                {ICON_TRASH, _("Empty the slot"), true, wifi_action_clear, false},
        };
        sheet_display_list(NULL, 0, actions, sizeof(actions) / sizeof(*actions), data);
        return;
    }

    sheet_action actions[] = {
            {ICON_ARROW_BACK_UP, _("Write to a slot"), true, wifi_action_write, false},
            {ICON_QRCODE, _("Show as a QR code"), false, wifi_action_qr, true},
            {ICON_PENCIL, _("Edit"), false, wifi_action_edit, false},
            {ICON_KEYBOARD, _("Rename"), false, wifi_action_rename, false},
            {ICON_TRASH, _("Delete"), true, wifi_action_delete, false},
    };
    sheet_display_list(NULL, 0, actions, sizeof(actions) / sizeof(*actions), data);
}

// ---- tools ----

static bool wifi_passwords_shown(ui_view* view, void* data) {
    return ((wifi_data*) data)->showPasswords;
}

static void wifi_toggle_passwords(ui_view* view, void* data) {
    wifi_data* listData = (wifi_data*) data;
    listData->showPasswords = !listData->showPasswords;
}

static void wifi_tool_scan(ui_view* view, void* data) {
    wifi_scan_open((wifi_data*) data);
}

static void wifi_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    wifi_data* listData = (wifi_data*) data;
    view->name = "Wi-Fi";
    view->info = _(" actions    scan QR    passwords    back");

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        wifi_clear_items(items);
        list_destroy(view);
        free(listData);
        return;
    }

    if(!listData->populated) {
        listData->items = items;
        wifi_populate(listData);
    }

    if(hidKeysDown() & KEY_Y) {
        wifi_toggle_passwords(view, listData);
    }

    if(hidKeysDown() & KEY_X) {
        wifi_scan_open(listData);
        return;
    }

    if(selected != NULL && selected->data != NULL && selectedTouched) {
        wifi_actions_open(listData, selected);
    }
}

static void wifi_long_press(ui_view* view, void* data, linked_list* items, list_item* item) {
    if(item->data != NULL) {
        wifi_actions_open((wifi_data*) data, item);
    }
}

void wifi_open() {
    wifi_data* data = (wifi_data*) calloc(1, sizeof(wifi_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate Wi-Fi data.");
        return;
    }

    ui_view* view = list_display("Wi-Fi", "", data, wifi_update, wifi_draw_top);
    if(view == NULL) {
        free(data);
        return;
    }

    data->view = view;
    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = wifi_draw_row
    };
    list_set_style(view, &style);
    list_set_long_press(view, wifi_long_press);
    list_show_title_bar(view);

    // Right to left.
    list_add_tool(view, ICON_KEY, wifi_passwords_shown, wifi_toggle_passwords);
    list_add_tool(view, ICON_QRCODE, NULL, wifi_tool_scan);
}
