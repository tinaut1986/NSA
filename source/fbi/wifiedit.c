#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "wifiedit.h"
#include "../core/core.h"

// Editing a Wi-Fi connection or a saved copy by hand: the network's name, its security and password, the address
// (automatic or typed) and the DNS. A list of fields; A changes the one with the focus (the keyboard, or a list of
// choices); the last row saves. Saving a connection writes it through ac:i (wifislots_write), so the console uses it at
// once; saving a copy rewrites its file. B leaves, asking first when something changed.

typedef enum {
    FIELD_SSID,
    FIELD_SECURITY,
    FIELD_PASSWORD,
    FIELD_ADDRESS,
    FIELD_IP,
    FIELD_NETMASK,
    FIELD_GATEWAY,
    FIELD_DNS,
    FIELD_DNS1,
    FIELD_DNS2,
    FIELD_SAVE,
    FIELD_COUNT
} edit_field;

typedef struct {
    wifislots_slot slot;      // As it is being changed.
    char ssid[33];
    u8 security;
    char password[65];
    bool changed;
    bool showPassword;
    bool rebuild;

    int connection;           // The connection it goes to (0-2), or -1 for a copy.
    wifislots_backup* backup;
    void (*saved)(void* data);
    void* data;

    list_item* items[FIELD_COUNT];
    edit_field current;
} edit_data;

static const u8 edit_securities[] = {WIFISLOTS_OPEN, WIFISLOTS_WEP_64, WIFISLOTS_WEP_128, WIFISLOTS_WEP_152, WIFISLOTS_WPA_TKIP, WIFISLOTS_WPA_AES,
                                     WIFISLOTS_WPA2_TKIP, WIFISLOTS_WPA2_AES};
#define SECURITY_COUNT (sizeof(edit_securities) / sizeof(*edit_securities))

static const char* edit_label(edit_field field) {
    switch(field) {
        case FIELD_SSID: return _("Network name");
        case FIELD_SECURITY: return _("Security");
        case FIELD_PASSWORD: return _("Password");
        case FIELD_ADDRESS: return _("Address");
        case FIELD_IP: return _("IP address");
        case FIELD_NETMASK: return _("Netmask");
        case FIELD_GATEWAY: return _("Gateway");
        case FIELD_DNS: return _("DNS");
        case FIELD_DNS1: return _("Primary DNS");
        case FIELD_DNS2: return _("Secondary DNS");
        case FIELD_SAVE: return _("Save changes");
        default: return "";
    }
}

static void edit_ip_text(const u8* ip, char* out, size_t size) {
    snprintf(out, size, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void edit_value(edit_data* e, edit_field field, char* out, size_t size) {
    const wifislots_settings* s = &e->slot.settings;
    out[0] = '\0';
    switch(field) {
        case FIELD_SSID: string_copy(out, e->ssid, size); break;
        case FIELD_SECURITY: string_copy(out, e->security == WIFISLOTS_OPEN ? _("Open network") : wifislots_security_name(e->security), size); break;
        case FIELD_PASSWORD:
            if(e->security == WIFISLOTS_OPEN) {
                string_copy(out, _("No password"), size);
            } else if(e->password[0] == '\0') {
                string_copy(out, _("Not stored"), size);
            } else {
                string_copy(out, e->showPassword ? e->password : "********", size);
            }
            break;
        case FIELD_ADDRESS: string_copy(out, s->dhcp ? _("Automatic (DHCP)") : _("Typed by hand"), size); break;
        case FIELD_IP: edit_ip_text(s->ip, out, size); break;
        case FIELD_NETMASK: edit_ip_text(s->netmask, out, size); break;
        case FIELD_GATEWAY: edit_ip_text(s->gateway, out, size); break;
        case FIELD_DNS: string_copy(out, s->autoDns ? _("Automatic") : _("Typed by hand"), size); break;
        case FIELD_DNS1: edit_ip_text(s->dns1, out, size); break;
        case FIELD_DNS2: edit_ip_text(s->dns2, out, size); break;
        default: break;
    }
}

static bool edit_shown(edit_data* e, edit_field field) {
    switch(field) {
        case FIELD_PASSWORD: return true;
        case FIELD_IP:
        case FIELD_NETMASK:
        case FIELD_GATEWAY: return !e->slot.settings.dhcp;
        case FIELD_DNS1:
        case FIELD_DNS2: return !e->slot.settings.autoDns;
        default: return true;
    }
}

static void edit_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    edit_data* e = (edit_data*) data;
    edit_field field = (edit_field) (u32) item->data;

    u32 ink = 0;
    u32 muted = 0;
    widget_item_frame(x, y, width, height, selected, &ink, &muted);
    if(field == FIELD_SAVE) {
        u32 color = selected ? ink : e->changed ? t->accent : muted;
        screen_draw_icon(ICON_DEVICE_FLOPPY, x + 8, y + (height - 16) / 2, 16, color);
        widget_text_fit(item->name, x + 32, y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, color, width - 40);
        return;
    }

    char value[80];
    edit_value(e, field, value, sizeof(value));
    widget_text_fit(item->name, x + 8, y + 3, WIDGET_SMALL_SCALE, muted, width - 16);
    widget_text_fit(value, x + 8, y + height - 3 - widget_text_height(WIDGET_TEXT_SCALE), WIDGET_TEXT_SCALE, ink, width - 16);
}

static void edit_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    const theme_current* t = theme();
    edit_data* e = (edit_data*) data;
    float x = x1 + 10;
    float w = x2 - x1 - 20;
    widget_card(x, y1 + 10, w, 52);
    screen_fill_round_rect(x + 8, y1 + 18, 36, 36, t->def->radius, t->accentSoft);
    screen_draw_icon(e->connection >= 0 ? ICON_WIFI : ICON_DEVICE_FLOPPY, x + 10, y1 + 20, 32, t->accent);
    char subtitle[64];
    if(e->connection >= 0) {
        snprintf(subtitle, sizeof(subtitle), _("Connection %lu"), (u32) e->connection + 1);
    } else {
        string_copy(subtitle, _("Saved copy"), sizeof(subtitle));
    }

    widget_text_fit(e->ssid[0] != '\0' ? e->ssid : _("Empty"), x + 54, y1 + 18, WIDGET_TITLE_SCALE, t->palette->text, w - 64);
    widget_text_fit(subtitle, x + 54, y1 + 20 + widget_text_height(WIDGET_TITLE_SCALE), WIDGET_SMALL_SCALE, t->palette->textMuted, w - 64);

    const char* hint = e->connection >= 0 ? _("Saving writes the connection: the console uses it at once.") : _("Saving rewrites the copy's file; its alias and date stay.");
    screen_draw_text_wrap(hint, x + 2, y1 + 72, WIDGET_SMALL_SCALE + 0.04f, t->palette->textMuted, x + w - 2);
}

static void edit_rebuild(edit_data* e, linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        linked_list_iter_next(&iter);
        linked_list_iter_remove(&iter);
    }

    for(u32 f = 0; f < FIELD_COUNT; f++) {
        if(edit_shown(e, (edit_field) f) && e->items[f] != NULL) {
            linked_list_add(items, e->items[f]);
        }
    }
}

// ---- changing a field ----

static void edit_typed(ui_view* view, void* data, SwkbdButton button, const char* response) {
    edit_data* e = (edit_data*) data;
    if(button != SWKBD_BUTTON_CONFIRM) {
        return;
    }

    wifislots_settings* s = &e->slot.settings;
    u8* ip = NULL;
    switch(e->current) {
        case FIELD_SSID: string_copy(e->ssid, response, sizeof(e->ssid)); break;
        case FIELD_PASSWORD: string_copy(e->password, response, sizeof(e->password)); break;
        case FIELD_IP: ip = s->ip; break;
        case FIELD_NETMASK: ip = s->netmask; break;
        case FIELD_GATEWAY: ip = s->gateway; break;
        case FIELD_DNS1: ip = s->dns1; break;
        case FIELD_DNS2: ip = s->dns2; break;
        default: return;
    }

    if(ip != NULL && !wifislots_parse_ip(response, ip)) {
        error_display(NULL, NULL, "Not an address: four numbers from 0 to 255 with dots, as 192.168.1.50.");
        return;
    }

    e->changed = true;
}

static edit_data* edit_choosing;

#define EDIT_CHOOSE(n)                                                       \
    static void edit_security_##n(void* data) {                              \
        edit_data* e = (edit_data*) data;                                    \
        e->security = edit_securities[n];                                    \
        e->changed = true;                                                   \
    }

EDIT_CHOOSE(0) EDIT_CHOOSE(1) EDIT_CHOOSE(2) EDIT_CHOOSE(3) EDIT_CHOOSE(4) EDIT_CHOOSE(5) EDIT_CHOOSE(6) EDIT_CHOOSE(7)

static void (*const edit_security_runs[SECURITY_COUNT])(void*) = {edit_security_0, edit_security_1, edit_security_2, edit_security_3,
                                                                  edit_security_4, edit_security_5, edit_security_6, edit_security_7};

static void edit_mode_auto(void* data) {
    edit_data* e = (edit_data*) data;
    if(e->current == FIELD_ADDRESS) {
        e->slot.settings.dhcp = 1;
    } else {
        e->slot.settings.autoDns = 1;
    }

    e->changed = true;
    e->rebuild = true;
}

static void edit_mode_manual(void* data) {
    edit_data* e = (edit_data*) data;
    if(e->current == FIELD_ADDRESS) {
        e->slot.settings.dhcp = 0;
    } else {
        e->slot.settings.autoDns = 0;
    }

    e->changed = true;
    e->rebuild = true;
}

static void edit_nothing(void* data) {
}

static void edit_open_field(edit_data* e, edit_field field) {
    e->current = field;
    char value[80];
    switch(field) {
        case FIELD_SSID:
            osk_display(_("Network name"), e->ssid, 32, OSK_NOT_BLANK, e, edit_typed);
            break;
        case FIELD_PASSWORD:
            if(e->security == WIFISLOTS_OPEN) {
                return;
            }

            osk_display(_("Password"), e->password, 63, 0, e, edit_typed);
            break;
        case FIELD_IP:
        case FIELD_NETMASK:
        case FIELD_GATEWAY:
        case FIELD_DNS1:
        case FIELD_DNS2:
            edit_value(e, field, value, sizeof(value));
            osk_display(edit_label(field), value, 15, OSK_NOT_BLANK, e, edit_typed);
            break;
        case FIELD_SECURITY: {
            static sheet_action actions[SECURITY_COUNT];
            for(u32 i = 0; i < SECURITY_COUNT; i++) {
                const char* name = edit_securities[i] == WIFISLOTS_OPEN ? _("Open network") : wifislots_security_name(edit_securities[i]);
                actions[i] = (sheet_action) {edit_securities[i] == WIFISLOTS_OPEN ? ICON_WIFI : ICON_SHIELD_LOCK, name, false, edit_security_runs[i]};
            }

            edit_choosing = e;
            sheet_display_choice(_("Security"), actions, SECURITY_COUNT, e, NULL, edit_nothing);
            break;
        }
        case FIELD_ADDRESS:
        case FIELD_DNS: {
            static sheet_action actions[2];
            actions[0] = (sheet_action) {ICON_REFRESH, field == FIELD_ADDRESS ? _("Automatic (DHCP)") : _("Automatic"), false, edit_mode_auto};
            actions[1] = (sheet_action) {ICON_PENCIL, _("Typed by hand"), false, edit_mode_manual};
            sheet_display_choice(edit_label(field), actions, 2, e, NULL, edit_nothing);
            break;
        }
        default:
            break;
    }
}

// ---- saving and leaving ----

static void edit_close(ui_view* view, edit_data* e) {
    ui_pop();
    list_destroy(view);
    for(u32 f = 0; f < FIELD_COUNT; f++) {
        free(e->items[f]);
    }

    free(e);
}

static ui_view* edit_view;

static void edit_save_answer(ui_view* view, void* data, u32 response) {
    edit_data* e = (edit_data*) data;
    if(response != PROMPT_YES) {
        return;
    }

    wifislots_slot slot = e->slot;
    const char* problem = wifislots_set_network(&slot, e->ssid, e->security, e->password);
    if(problem != NULL) {
        error_display(NULL, NULL, problem);
        return;
    }

    Result res;
    const char* step = "";
    bool inUse = e->connection >= 0 && wifislots_in_use((u32) e->connection);
    if(e->connection >= 0) {
        res = wifislots_write((u32) e->connection, &slot, &step);
    } else {
        res = wifislots_update_backup(e->backup, &slot);
    }

    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, e->connection >= 0 ? "Failed to write the connection (%s)." : "Failed to save the copy.", step);
        return;
    }

    if(e->saved != NULL) {
        e->saved(e->data);
    }

    sheet_confirmed();   // The sheet it was opened from goes too.
    edit_close(edit_view, e);
    if(e->connection >= 0 && !wifislots_online()) {
        wificonnections_connect((u32) e->connection, _("Changes saved."));
    } else if(inUse) {
        char message[400];
        snprintf(message, sizeof(message), "%s\n\n%s", _("Changes saved."), _("The console stays on the network it is connected to until it connects again: put it in sleep mode and wake it, or restart it."));
        prompt_display_notify("Success", message, COLOR_TEXT, NULL, NULL, NULL);
    } else {
        prompt_display_notify("Success", "Changes saved.", COLOR_TEXT, NULL, NULL, NULL);
    }
}

static void edit_leave_answer(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        edit_close(edit_view, (edit_data*) data);
    }
}

static void edit_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    edit_data* e = (edit_data*) data;
    view->name = _("Edit Wi-Fi connection");
    view->info = e->showPassword ? _(" change     hide password     back") : _(" change     show password     back");

    if(e->rebuild || linked_list_size(items) == 0) {
        e->rebuild = false;
        edit_rebuild(e, items);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        if(e->changed) {
            prompt_display_yes_no("Confirmation", "Leave without saving the changes?", COLOR_TEXT, e, NULL, edit_leave_answer);
        } else {
            edit_close(view, e);
        }

        return;
    }

    if(hidKeysDown() & KEY_Y) {
        e->showPassword = !e->showPassword;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        edit_field field = (edit_field) (u32) selected->data;
        if(field == FIELD_SAVE) {
            char message[160];
            if(e->connection >= 0) {
                snprintf(message, sizeof(message), _("Save the changes to connection %lu?"), (u32) e->connection + 1);
            } else {
                string_copy(message, _("Save the changes to this copy?"), sizeof(message));
            }

            prompt_display_yes_no("Confirmation", message, COLOR_TEXT, e, NULL, edit_save_answer);
        } else {
            edit_open_field(e, field);
        }
    }
}

void wifiedit_open(const wifislots_slot* start, int connection, wifislots_backup* backup, void (*saved)(void* data), void* data) {
    edit_data* e = (edit_data*) calloc(1, sizeof(edit_data));
    if(e == NULL) {
        error_display(NULL, NULL, "Failed to allocate Wi-Fi data.");
        return;
    }

    if(start != NULL && !wifislots_empty(start)) {
        e->slot = *start;
        wifislots_name(start, e->ssid, sizeof(e->ssid));
        e->security = wifislots_network_of(start)->security;
        wifislots_password(start, e->password, sizeof(e->password));
    } else {
        wifislots_new(&e->slot);
        e->security = WIFISLOTS_WPA2_AES;
    }

    e->connection = connection;
    e->backup = backup;
    e->saved = saved;
    e->data = data;
    // A connection set up by hand is new: it needs saving even untouched.
    e->changed = start == NULL || wifislots_empty(start);

    for(u32 f = 0; f < FIELD_COUNT; f++) {
        list_item* item = (list_item*) calloc(1, sizeof(list_item));
        if(item == NULL) {
            continue;
        }

        string_copy(item->name, edit_label((edit_field) f), sizeof(item->name));
        item->data = (void*) f;
        e->items[f] = item;
    }

    ui_view* view = list_display(_("Edit Wi-Fi connection"), "", e, edit_update, edit_draw_top);
    if(view == NULL) {
        for(u32 f = 0; f < FIELD_COUNT; f++) {
            free(e->items[f]);
        }

        free(e);
        return;
    }

    edit_view = view;
    list_style style = {.rowHeight = 38, .drawRow = edit_draw_row};
    list_set_style(view, &style);
    list_show_title_bar(view);
}
