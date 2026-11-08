#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "wifislots.h"
#include "../core/core.h"

// Wi-Fi connections (System -> Network, A): the console's three connections and the copies saved on the SD card, in
// WifiManager's folder and format. A connection can be saved as a copy; a copy can be written back to any connection,
// which the console uses at once (wifislots.c writes it through ac:i, as System Settings does). Y shows the passwords.

typedef enum {
    ENTRY_SLOT,
    ENTRY_BACKUP
} entry_kind;

typedef struct {
    entry_kind kind;
    u32 index;   // The connection (0-2) or the copy.
} wifi_entry;

typedef struct {
    wifislots_slot slots[WIFISLOTS_COUNT];
    bool slotRead[WIFISLOTS_COUNT];
    wifislots_backup* backups;
    u32 backupCount;
    char connected[33];

    wifi_entry* entries;
    bool populated;
    bool showPasswords;

    linked_list* items;
    list_item* actionSelected;
    u32 target;            // The connection a copy goes to.
    u32 source;            // The copy that goes there.
} wifi_data;

#define ROW_HEIGHT 40
#define HEADER_HEIGHT 52
#define CELL_GAP 6

static void wifi_clear(wifi_data* data, linked_list* items) {
    if(items != NULL) {
        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            free(linked_list_iter_next(&iter));
            linked_list_iter_remove(&iter);
        }
    }

    free(data->backups);
    data->backups = NULL;
    data->backupCount = 0;
    free(data->entries);
    data->entries = NULL;
}

static void wifi_populate(wifi_data* data, linked_list* items) {
    wifi_clear(data, items);

    data->connected[0] = '\0';
    u32 status = 0;
    if(R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0) {
        ACU_GetSSID(data->connected);
    }

    for(u32 i = 0; i < WIFISLOTS_COUNT; i++) {
        data->slotRead[i] = R_SUCCEEDED(wifislots_read(i, &data->slots[i]));
    }

    data->backupCount = wifislots_list_backups(&data->backups);
    data->entries = (wifi_entry*) calloc(WIFISLOTS_COUNT + data->backupCount, sizeof(wifi_entry));
    if(data->entries == NULL) {
        return;
    }

    u32 count = 0;
    for(u32 i = 0; i < WIFISLOTS_COUNT + data->backupCount; i++) {
        wifi_entry* entry = &data->entries[count];
        entry->kind = i < WIFISLOTS_COUNT ? ENTRY_SLOT : ENTRY_BACKUP;
        entry->index = i < WIFISLOTS_COUNT ? i : i - WIFISLOTS_COUNT;

        list_item* item = (list_item*) calloc(1, sizeof(list_item));
        if(item == NULL) {
            break;
        }

        const wifislots_slot* slot = entry->kind == ENTRY_SLOT ? &data->slots[entry->index] : &data->backups[entry->index].slot;
        char name[33] = "";
        if(entry->kind == ENTRY_BACKUP || (data->slotRead[entry->index] && !wifislots_empty(slot))) {
            wifislots_name(slot, name, sizeof(name));
        }

        if(entry->kind == ENTRY_BACKUP && data->backups[entry->index].alias[0] != '\0') {
            string_copy(item->name, data->backups[entry->index].alias, sizeof(item->name));
        } else {
            string_copy(item->name, name[0] != '\0' ? name : _("Empty"), sizeof(item->name));
        }

        item->data = entry;
        linked_list_add(items, item);
        count++;
    }
}

static const wifislots_slot* wifi_slot_of(wifi_data* data, const wifi_entry* entry) {
    return entry->kind == ENTRY_SLOT ? &data->slots[entry->index] : &data->backups[entry->index].slot;
}

static bool wifi_slot_used(wifi_data* data, u32 index) {
    return data->slotRead[index] && !wifislots_empty(&data->slots[index]);
}

static bool wifi_in_use(wifi_data* data, u32 index) {
    if(!wifi_slot_used(data, index) || data->connected[0] == '\0') {
        return false;
    }

    char name[33];
    wifislots_name(&data->slots[index], name, sizeof(name));
    return strcmp(name, data->connected) == 0;
}

static void wifi_saved_text(const wifislots_backup* backup, char* out, size_t size) {
    if(backup->saved == 0) {
        string_copy(out, "-", size);
        return;
    }

    time_t saved = backup->saved;
    struct tm* tm = gmtime(&saved);
    if(tm == NULL || strftime(out, size, "%Y-%m-%d %H:%M", tm) == 0) {
        string_copy(out, "-", size);
    }
}

static void wifi_password_text(wifi_data* data, const wifislots_slot* slot, char* out, size_t size) {
    const wifislots_network* net = wifislots_network_of(slot);
    char password[0x41];
    wifislots_password(slot, password, sizeof(password));
    if(net->security == WIFISLOTS_OPEN) {
        string_copy(out, _("No password"), size);
    } else if(password[0] == '\0') {
        string_copy(out, _("Not stored"), size);
    } else if(data->showPasswords) {
        string_copy(out, password, size);
    } else {
        string_copy(out, "********", size);   // Always 8: the length does not show either.
    }
}

// ---- drawing ----

static void wifi_draw_row(void* listData, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    wifi_data* data = (wifi_data*) listData;
    wifi_entry* entry = (wifi_entry*) item->data;

    u32 ink = 0;
    u32 muted = 0;
    widget_item_frame(x, y, width, height, selected, &ink, &muted);

    float icon = height - 10;
    bool used = entry->kind == ENTRY_BACKUP || wifi_slot_used(data, entry->index);
    u32 iconId = entry->kind == ENTRY_BACKUP ? ICON_DEVICE_FLOPPY : used ? ICON_WIFI : ICON_WIFI_OFF;
    widget_icon_placeholder(x + 5, y + 5, icon, iconId, selected ? THEME_ALPHA(t->onAccent, 0x40) : used ? t->accentSoft : t->palette->background,
                            selected ? t->onAccent : used ? t->accent : t->palette->textMuted);

    float tx = x + 5 + icon + 8;
    float right = x + width - 8;
    float tagW = 0;
    if(entry->kind == ENTRY_SLOT && wifi_in_use(data, entry->index)) {
        tagW = widget_badge_right(right, y + 4, _("In use"), selected ? THEME_ALPHA(t->onAccent, 0x40) : t->accentSoft, selected ? t->onAccent : t->accent);
    }

    widget_text_fit(item->name, tx, y + 4, WIDGET_TEXT_SCALE, used ? ink : muted, right - tagW - 6 - tx);

    char line[96];
    if(entry->kind == ENTRY_SLOT) {
        char number[48];
        snprintf(number, sizeof(number), _("Connection %lu"), entry->index + 1);
        if(used) {
            snprintf(line, sizeof(line), "%s \xC2\xB7 %s", number, wifislots_security_name(wifislots_network_of(wifi_slot_of(data, entry))->security));
        } else {
            string_copy(line, number, sizeof(line));
        }
    } else {
        char saved[32];
        wifi_saved_text(&data->backups[entry->index], saved, sizeof(saved));
        snprintf(line, sizeof(line), "%s \xC2\xB7 %s", _("Saved copy"), saved);
    }

    widget_text_fit(line, tx, y + height - 4 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, muted, right - tx);
}

static void wifi_header(float x, float y, float w, u32 icon, const char* title, const char* subtitle) {
    const theme_current* t = theme();
    widget_card(x, y, w, HEADER_HEIGHT);
    float tile = 36;
    float tileY = y + (HEADER_HEIGHT - tile) / 2;
    screen_fill_round_rect(x + 8, tileY, tile, tile, t->def->radius, t->accentSoft);
    screen_draw_icon(icon, x + 10, tileY + 2, 32, t->accent);
    float tx = x + 8 + tile + 10;
    float titleH = widget_text_height(WIDGET_TITLE_SCALE);
    float textY = y + (HEADER_HEIGHT - titleH - 2 - widget_text_height(WIDGET_SMALL_SCALE)) / 2;
    widget_text_fit(title, tx, textY, WIDGET_TITLE_SCALE, t->palette->text, x + w - 10 - tx);
    widget_text_fit(subtitle, tx, textY + titleH + 2, WIDGET_SMALL_SCALE, t->palette->textMuted, x + w - 10 - tx);
}

static void wifi_draw_top(ui_view* view, void* listData, float x1, float y1, float x2, float y2, list_item* selected) {
    const theme_current* t = theme();
    wifi_data* data = (wifi_data*) listData;
    float x = x1 + 10;
    float w = x2 - x1 - 20;
    float y = y1 + 10;

    if(selected == NULL || selected->data == NULL) {
        return;
    }

    wifi_entry* entry = (wifi_entry*) selected->data;
    const wifislots_slot* slot = wifi_slot_of(data, entry);
    bool used = entry->kind == ENTRY_BACKUP || wifi_slot_used(data, entry->index);

    char subtitle[96];
    if(entry->kind == ENTRY_SLOT) {
        snprintf(subtitle, sizeof(subtitle), _("Connection %lu"), entry->index + 1);
        if(wifi_in_use(data, entry->index)) {
            size_t len = strlen(subtitle);
            snprintf(subtitle + len, sizeof(subtitle) - len, " \xC2\xB7 %s", _("In use"));
        }
    } else {
        const char* file = strrchr(data->backups[entry->index].path, '/');
        snprintf(subtitle, sizeof(subtitle), "%s \xC2\xB7 %s", _("Saved copy"), file != NULL ? file + 1 : data->backups[entry->index].path);
    }

    wifi_header(x, y, w, entry->kind == ENTRY_BACKUP ? ICON_DEVICE_FLOPPY : used ? ICON_WIFI : ICON_WIFI_OFF, selected->name, subtitle);
    y += HEADER_HEIGHT + CELL_GAP;

    if(!used) {
        screen_draw_text_wrap(_("This connection is empty:  writes one of the saved copies to it."),
                              x + 2, y + 4, WIDGET_SMALL_SCALE + 0.04f, t->palette->textMuted, x + w - 2);
        return;
    }

    float half = (w - CELL_GAP) / 2;
    char name[33];
    wifislots_name(slot, name, sizeof(name));
    widget_cell(x, y, half, _("Network"), name);
    widget_cell(x + half + CELL_GAP, y, half, _("Security"), wifislots_security_name(wifislots_network_of(slot)->security));
    y += WIDGET_CELL_HEIGHT + CELL_GAP;

    char password[0x41];
    wifi_password_text(data, slot, password, sizeof(password));
    widget_cell(x, y, w, _("Password"), password);
    y += WIDGET_CELL_HEIGHT + CELL_GAP;

    char address[32];
    const wifislots_settings* s = &slot->settings;
    if(s->dhcp) {
        string_copy(address, _("Automatic (DHCP)"), sizeof(address));
    } else {
        snprintf(address, sizeof(address), "%u.%u.%u.%u", s->ip[0], s->ip[1], s->ip[2], s->ip[3]);
    }

    if(entry->kind == ENTRY_BACKUP) {
        char saved[32];
        wifi_saved_text(&data->backups[entry->index], saved, sizeof(saved));
        widget_cell(x, y, half, _("Address"), address);
        widget_cell(x + half + CELL_GAP, y, half, _("Saved"), saved);
    } else {
        widget_cell(x, y, w, _("Address"), address);
    }
}

// ---- actions ----

// Saving asks for an alias first, as WifiManager does: it names the copy in the list (and its file); empty, the network does.
static void wifi_save_typed(ui_view* view, void* listData, SwkbdButton button, const char* response) {
    if(button != SWKBD_BUTTON_CONFIRM) {
        return;
    }

    sheet_confirmed();
    wifi_data* data = (wifi_data*) listData;
    wifi_entry* entry = (wifi_entry*) data->actionSelected->data;
    char path[256];
    Result res = wifislots_save_backup(&data->slots[entry->index], response, path, sizeof(path));
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to save the copy.");
        return;
    }

    data->populated = false;
    char message[320];
    snprintf(message, sizeof(message), _("Saved as %s."), path);
    prompt_display_notify("Success", message, COLOR_TEXT, NULL, NULL, NULL);
}

static void wifi_save(void* listData) {
    osk_display(_("Optional alias (empty = network name)"), "", 96, 0, listData, wifi_save_typed);
}

static void wifi_rename_typed(ui_view* view, void* listData, SwkbdButton button, const char* response) {
    if(button != SWKBD_BUTTON_CONFIRM) {
        return;
    }

    sheet_confirmed();
    wifi_data* data = (wifi_data*) listData;
    wifi_entry* entry = (wifi_entry*) data->actionSelected->data;
    Result res = wifislots_set_alias(&data->backups[entry->index], response);
    data->populated = false;
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to change the alias.");
        return;
    }

    prompt_display_notify("Success", "Alias changed.", COLOR_TEXT, NULL, NULL, NULL);
}

static void wifi_rename(void* listData) {
    wifi_data* data = (wifi_data*) listData;
    wifi_entry* entry = (wifi_entry*) data->actionSelected->data;
    osk_display(_("Alias (empty = network name)"), data->backups[entry->index].alias, 96, 0, listData, wifi_rename_typed);
}

static void wifi_restore_answer(ui_view* view, void* listData, u32 response) {
    wifi_data* data = (wifi_data*) listData;
    if(response != PROMPT_YES) {
        return;
    }

    sheet_confirmed();
    wifislots_slot slot = data->backups[data->source].slot;
    const char* step = "";
    Result res = wifislots_write(data->target, &slot, &step);
    data->populated = false;
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to write the connection (%s).", step);
        return;
    }

    char name[33];
    wifislots_name(&slot, name, sizeof(name));
    char message[160];
    snprintf(message, sizeof(message), _("Connection %lu is now %s."), data->target + 1, name);
    prompt_display_notify("Success", message, COLOR_TEXT, NULL, NULL, NULL);
}

// Copy `data->source` to connection `data->target`, after asking.
static void wifi_restore_ask(wifi_data* data) {
    char name[33];
    wifislots_name(&data->backups[data->source].slot, name, sizeof(name));
    char message[256];
    if(wifi_slot_used(data, data->target)) {
        char current[33];
        wifislots_name(&data->slots[data->target], current, sizeof(current));
        snprintf(message, sizeof(message), _("Replace connection %lu (%s) with %s?"), data->target + 1, current, name);
        prompt_display_warning("Confirmation", message, COLOR_TEXT, data, NULL, wifi_restore_answer);
    } else {
        snprintf(message, sizeof(message), _("Write %s to connection %lu?"), name, data->target + 1);
        prompt_display_yes_no("Confirmation", message, COLOR_TEXT, data, NULL, wifi_restore_answer);
    }
}

#define WIFI_RESTORE_TO(n)                                       \
    static void wifi_restore_to_##n(void* listData) {            \
        wifi_data* data = (wifi_data*) listData;                 \
        data->source = ((wifi_entry*) data->actionSelected->data)->index; \
        data->target = (n);                                      \
        wifi_restore_ask(data);                                  \
    }

WIFI_RESTORE_TO(0)
WIFI_RESTORE_TO(1)
WIFI_RESTORE_TO(2)

// A connection: which copy goes there.
#define WIFI_PICK_MAX 32
static wifi_data* wifi_pick_data;
static u32 wifi_pick_count;

#define WIFI_PICK(n)                                             \
    static void wifi_pick_##n(void* listData) {                  \
        wifi_data* data = (wifi_data*) listData;                 \
        data->source = (n);                                      \
        wifi_restore_ask(data);                                  \
    }

WIFI_PICK(0) WIFI_PICK(1) WIFI_PICK(2) WIFI_PICK(3) WIFI_PICK(4) WIFI_PICK(5) WIFI_PICK(6) WIFI_PICK(7)
WIFI_PICK(8) WIFI_PICK(9) WIFI_PICK(10) WIFI_PICK(11) WIFI_PICK(12) WIFI_PICK(13) WIFI_PICK(14) WIFI_PICK(15)
WIFI_PICK(16) WIFI_PICK(17) WIFI_PICK(18) WIFI_PICK(19) WIFI_PICK(20) WIFI_PICK(21) WIFI_PICK(22) WIFI_PICK(23)
WIFI_PICK(24) WIFI_PICK(25) WIFI_PICK(26) WIFI_PICK(27) WIFI_PICK(28) WIFI_PICK(29) WIFI_PICK(30) WIFI_PICK(31)

static void (*const wifi_picks[WIFI_PICK_MAX])(void*) = {
        wifi_pick_0, wifi_pick_1, wifi_pick_2, wifi_pick_3, wifi_pick_4, wifi_pick_5, wifi_pick_6, wifi_pick_7,
        wifi_pick_8, wifi_pick_9, wifi_pick_10, wifi_pick_11, wifi_pick_12, wifi_pick_13, wifi_pick_14, wifi_pick_15,
        wifi_pick_16, wifi_pick_17, wifi_pick_18, wifi_pick_19, wifi_pick_20, wifi_pick_21, wifi_pick_22, wifi_pick_23,
        wifi_pick_24, wifi_pick_25, wifi_pick_26, wifi_pick_27, wifi_pick_28, wifi_pick_29, wifi_pick_30, wifi_pick_31,
};

static void wifi_pick_cancel(void* listData) {
}

static void wifi_restore_here(void* listData) {
    wifi_data* data = (wifi_data*) listData;
    data->target = ((wifi_entry*) data->actionSelected->data)->index;

    static sheet_action actions[WIFI_PICK_MAX];
    static char labels[WIFI_PICK_MAX][64];
    static char details[WIFI_PICK_MAX][64];
    u32 count = data->backupCount < WIFI_PICK_MAX ? data->backupCount : WIFI_PICK_MAX;
    for(u32 i = 0; i < count; i++) {
        const wifislots_backup* backup = &data->backups[i];
        char name[33];
        wifislots_name(&backup->slot, name, sizeof(name));
        string_copy(labels[i], backup->alias[0] != '\0' ? backup->alias : name, sizeof(labels[i]));
        char saved[32];
        wifi_saved_text(backup, saved, sizeof(saved));
        snprintf(details[i], sizeof(details[i]), "%s \xC2\xB7 %s", wifislots_security_name(wifislots_network_of(&backup->slot)->security), saved);
        actions[i] = (sheet_action) {ICON_DEVICE_FLOPPY, labels[i], false, wifi_picks[i], false, details[i]};
    }

    wifi_pick_data = data;
    wifi_pick_count = count;
    sheet_display_choice(_("Which copy?"), actions, count, data, NULL, wifi_pick_cancel);
}

static void wifi_delete_answer(ui_view* view, void* listData, u32 response) {
    wifi_data* data = (wifi_data*) listData;
    if(response != PROMPT_YES) {
        return;
    }

    sheet_confirmed();
    Result res = wifislots_delete_backup(&data->backups[data->source]);
    data->populated = false;
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to delete the copy.");
        return;
    }

    prompt_display_notify("Success", "Copy deleted.", COLOR_TEXT, NULL, NULL, NULL);
}

static void wifi_delete(void* listData) {
    wifi_data* data = (wifi_data*) listData;
    data->source = ((wifi_entry*) data->actionSelected->data)->index;
    char name[33];
    wifislots_name(&data->backups[data->source].slot, name, sizeof(name));
    char message[160];
    snprintf(message, sizeof(message), _("Delete the copy of %s? It cannot be undone."), name);
    prompt_display_warning("Confirmation", message, COLOR_TEXT, data, NULL, wifi_delete_answer);
}

static void wifi_actions_open(wifi_data* data, list_item* selected) {
    data->actionSelected = selected;
    wifi_entry* entry = (wifi_entry*) selected->data;

    static char labels[WIFISLOTS_COUNT][64];
    static char details[WIFISLOTS_COUNT][64];
    sheet_action actions[7];
    u32 count = 0;
    if(entry->kind == ENTRY_SLOT) {
        if(wifi_slot_used(data, entry->index)) {
            actions[count++] = (sheet_action) {ICON_DEVICE_FLOPPY, _("Save a copy"), false, wifi_save};
        }

        if(data->backupCount > 0) {
            actions[count++] = (sheet_action) {ICON_ARROW_BACK_UP, _("Restore a copy here"), false, wifi_restore_here};
        }

        if(count == 0) {
            return;
        }

        char number[48];
        snprintf(number, sizeof(number), _("Connection %lu"), entry->index + 1);
        sheet_display_list_for(number, selected->name, wifi_slot_used(data, entry->index) ? ICON_WIFI : ICON_WIFI_OFF, actions, count, data);
        return;
    }

    static void (*const restore[WIFISLOTS_COUNT])(void*) = {wifi_restore_to_0, wifi_restore_to_1, wifi_restore_to_2};
    for(u32 i = 0; i < WIFISLOTS_COUNT; i++) {
        snprintf(labels[i], sizeof(labels[i]), _("Restore to connection %lu"), i + 1);
        if(wifi_slot_used(data, i)) {
            wifislots_name(&data->slots[i], details[i], sizeof(details[i]));
        } else {
            string_copy(details[i], _("Empty"), sizeof(details[i]));
        }

        actions[count++] = (sheet_action) {ICON_ARROW_BACK_UP, labels[i], wifi_slot_used(data, i), restore[i], false, details[i]};
    }

    actions[count++] = (sheet_action) {ICON_PENCIL, _("Rename alias"), false, wifi_rename};
    actions[count++] = (sheet_action) {ICON_TRASH, _("Delete this copy"), true, wifi_delete};
    sheet_display_list_for(_("Saved copy"), selected->name, ICON_DEVICE_FLOPPY, actions, count, data);
}

// ---- the screen ----

static void wifi_update(ui_view* view, void* listData, linked_list* items, list_item* selected, bool selectedTouched) {
    wifi_data* data = (wifi_data*) listData;
    data->items = items;

    view->name = _("Wi-Fi connections");
    view->info = data->showPasswords ? _(" actions     hide passwords     reload     back") : _(" actions     show passwords     reload     back");

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        wifi_clear(data, items);
        list_destroy(view);
        free(data);
        return;
    }

    if(hidKeysDown() & KEY_Y) {
        data->showPasswords = !data->showPasswords;
    }

    if(!data->populated || (hidKeysDown() & KEY_X)) {
        wifi_populate(data, items);
        data->populated = true;
        return;
    }

    if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        wifi_actions_open(data, selected);
    }
}

void wificonnections_open() {
    wifi_data* data = (wifi_data*) calloc(1, sizeof(wifi_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate Wi-Fi data.");
        return;
    }

    ui_view* view = list_display(_("Wi-Fi connections"), "", data, wifi_update, wifi_draw_top);
    if(view == NULL) {
        free(data);
        return;
    }

    list_style style = {
            .rowHeight = ROW_HEIGHT,
            .drawRow = wifi_draw_row,
    };
    list_set_style(view, &style);
}
