#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "settings.h"
#include "../core/core.h"

// The main menu: the three everyday entries as big tiles, then two groups
// (Cleanup and data, Advanced) and System and Settings. A group opens a list
// of its entries; Advanced asks once per session before opening. System shows
// the console at a glance and opens the list of its details.

typedef struct mainmenu_entry_s mainmenu_entry;

struct mainmenu_entry_s {
    icon_id icon;
    const char* name;
    const char* description;
    const char* summary;   // One short line for a group's list.
    void (*open)();
    int storage;   // The media it manages (FS_SystemMediaType), or -1.
    int storage2;
    // Draws the top screen instead of the card (System); NULL for the card.
    void (*drawTop)(float x1, float y1, float x2, float y2);
    // A group: the entries it holds, and whether to warn before opening it.
    const mainmenu_entry* children;
    u32 childCount;
    bool warn;
};

static const mainmenu_entry cleanup[] = {
        {ICON_TICKET, "Tickets", "Installed tickets: see which are in use and delete the ones you no longer need.", "Licences of titles", tickets_open, SYSTEM_MEDIATYPE_CTR_NAND, -1},
        {ICON_CLOCK, "Pending titles", "Titles left half installed. Deleting them frees their space.", "Half-finished installs", pendingtitles_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
        {ICON_DATABASE, "Extra save data", "Extra save data of titles: browse, back up or delete it.", "Games' extra data", extsavedata_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
};

static const mainmenu_entry advanced[] = {
        {ICON_CPU, "CTR NAND", "The console's system memory. Changes here can stop it from starting.", "System memory (3DS mode)", files_open_ctr_nand, SYSTEM_MEDIATYPE_CTR_NAND, -1},
        {ICON_CPU, "TWL NAND", "The DSi-mode system memory. Changes here can break DSi games and software.", "System memory (DSi mode)", files_open_twl_nand, SYSTEM_MEDIATYPE_TWL_NAND, -1},
        {ICON_PHOTO, "TWL Photo", "The DSi camera's photo storage.", "DSi camera photos", files_open_twl_photo, SYSTEM_MEDIATYPE_TWL_PHOTO, -1},
        {ICON_MUSIC, "TWL Sound", "The DSi sound recorder's storage.", "DSi sound recordings", files_open_twl_sound, SYSTEM_MEDIATYPE_TWL_NAND, -1},
        {ICON_DEVICE_FLOPPY, "System save data", "System save data: browse or delete it. Handle with care.", "System saves", systemsavedata_open, SYSTEM_MEDIATYPE_CTR_NAND, -1},
        {ICON_DOWNLOAD, "Dump NAND", "Copy the whole system memory to the SD card as a backup.", "Backup to the SD card", dumpnand_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
};

// System's details: nothing to open, each draws its page on the top screen.
static const mainmenu_entry details[] = {
        {ICON_3DS, "Console", "", "Model, serial and versions", NULL, -1, -1, systeminfo_draw_console},
        {ICON_WIFI, "Network", "", "Wi-Fi, addresses and servers", NULL, -1, -1, systeminfo_draw_network},
        {ICON_DEVICE_SD_CARD, "Storage", "", "SD card and system memories", NULL, -1, -1, systeminfo_draw_storage},
        {ICON_BATTERY_CHARGING, "Battery", "", "Charge, voltage, temperature, rate", NULL, -1, -1, systeminfo_draw_battery},
};

#define COUNT_OF(a) (sizeof(a) / sizeof(*(a)))

static const mainmenu_entry entries[] = {
        {ICON_DEVICE_SD_CARD, "SD card", "Browse the SD card: install CIAs and tickets, copy, rename and delete files.", NULL, files_open_sd, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_APPS, "Titles", "Installed titles: launch them, delete them, back up their saves or extract their icons.", NULL, titles_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
        {ICON_WORLD_DOWNLOAD, "Remote install", "Install from the network: scan a QR code, type a URL or receive URLs from a computer.", NULL, remoteinstall_open, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_PACKAGE, "Store", "Homebrew from Universal-DB: browse the catalogue and install or download apps.", NULL, store_open, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_STACK_2, "Cleanup and data", "Licences, half-finished installs and the games' extra data.", NULL, NULL, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND, NULL, cleanup, COUNT_OF(cleanup), false},
        {ICON_ALERT_TRIANGLE, "Advanced", "The console's own memories and system data. Handle with care.", NULL, NULL, SYSTEM_MEDIATYPE_CTR_NAND, SYSTEM_MEDIATYPE_TWL_NAND, NULL, advanced, COUNT_OF(advanced), true},
        {ICON_3DS, "System", "The console at a glance: model, versions, network, battery and space.", NULL, NULL, -1, -1, systeminfo_draw_top, details, COUNT_OF(details), false},
        {ICON_SETTINGS, "Settings", "Theme, colours, language, updates and the FTP server.", NULL, settingsmenu_open, -1, -1},
};

#define ENTRY_COUNT COUNT_OF(entries)
#define TILE_COUNT 3      // The first entries, as big tiles in one row.
#define TILE_HEIGHT 80
#define SMALL_ROW_HEIGHT 44   // The small entries: one row of icons...
#define TIP_HEIGHT 26         // ...and a tooltip under the one with the focus, with its name.
#define TIP_SLIDE 10          // The tooltip slides down this far while it appears.
#define TIP_TIME 200
#define TRAY_HEIGHT 38        // Editing: the hidden entries wait in a row at the bottom.
#define GAP 6
#define PAD 8

// Stable names for the layout kept in settings.json, in the order of entries[].
static const char* const entry_ids[] = {"sd", "titles", "remote", "store", "cleanup", "advanced", "system", "settings"};
_Static_assert(COUNT_OF(entry_ids) == ENTRY_COUNT, "an id for every entry");

typedef struct {
    u32 selected;   // A position in the layout (below), not an index of entries[].
    u64 nextMove;

    bool touching;
    widget_press press;

    // The top card's contents cross-fade when the selection changes.
    u32 shown;
    u32 leaving;
    u64 shownTime;

    // The tooltip of the selected small entry slides in again at every new selection.
    u32 lastSelected;
    u64 tipTime;

    // Editing the layout.
    bool grabbed;       // The selected entry is picked up: the D-Pad moves it.
    int dragFrom;       // A finger holds the entry at this position, or -1.
    bool dragMoved;
    float dragStartX, dragStartY, dragX, dragY;
} mainmenu_data;

// ---- the layout: which entries show and in which order ----
//
// A permutation of entries[]: the first `layout_visible` positions are shown (the first three as big tiles, the rest as
// icons), the others are hidden. Editing shows the hidden ones in a tray under the icons.

static u8 layout_perm[ENTRY_COUNT];
static u32 layout_visible = ENTRY_COUNT;
static bool layout_editing = false;

#define POS(p) (&entries[layout_perm[p]])

// System and Settings always stay in the menu: from them the layout can always be edited again.
static bool mainmenu_pinned(u32 entry) {
    return strcmp(entry_ids[entry], "system") == 0 || strcmp(entry_ids[entry], "settings") == 0;
}

static void mainmenu_layout_default() {
    for(u32 i = 0; i < ENTRY_COUNT; i++) {
        layout_perm[i] = (u8) i;
    }

    layout_visible = ENTRY_COUNT;
}

static void mainmenu_layout_load() {
    mainmenu_layout_default();

    const char* c = settings_get()->menu;
    if(c[0] == '\0') {
        return;
    }

    bool used[ENTRY_COUNT] = {false};
    u8 shown[ENTRY_COUNT];
    u8 hidden[ENTRY_COUNT];
    u32 shownCount = 0;
    u32 hiddenCount = 0;
    bool inHidden = false;
    while(*c != '\0') {
        if(*c == '|') {
            inHidden = true;
            c++;
            continue;
        }

        if(*c == ',') {
            c++;
            continue;
        }

        char id[16];
        size_t len = 0;
        while(*c != '\0' && *c != ',' && *c != '|') {
            if(len < sizeof(id) - 1) {
                id[len++] = *c;
            }

            c++;
        }

        id[len] = '\0';
        for(u32 e = 0; e < ENTRY_COUNT; e++) {
            if(!used[e] && strcmp(id, entry_ids[e]) == 0) {
                used[e] = true;
                if(inHidden && !mainmenu_pinned(e)) {
                    hidden[hiddenCount++] = (u8) e;
                } else {
                    shown[shownCount++] = (u8) e;
                }

                break;
            }
        }
    }

    // Entries the saved layout does not know (new ones) show at the end.
    for(u32 e = 0; e < ENTRY_COUNT; e++) {
        if(!used[e]) {
            shown[shownCount++] = (u8) e;
        }
    }

    memcpy(layout_perm, shown, shownCount);
    memcpy(layout_perm + shownCount, hidden, hiddenCount);
    layout_visible = shownCount;
}

static void mainmenu_layout_save() {
    char text[sizeof(settings_get()->menu)];
    size_t len = 0;
    bool isDefault = layout_visible == ENTRY_COUNT;
    for(u32 p = 0; p < ENTRY_COUNT; p++) {
        isDefault = isDefault && layout_perm[p] == p;
    }

    if(!isDefault) {
        for(u32 p = 0; p < ENTRY_COUNT; p++) {
            if(p > 0) {
                text[len++] = p == layout_visible ? '|' : ',';
            } else if(layout_visible == 0) {
                text[len++] = '|';
            }

            len += snprintf(text + len, sizeof(text) - len, "%s", entry_ids[layout_perm[p]]);
        }
    }

    text[len] = '\0';
    string_copy(settings_get()->menu, text, sizeof(settings_get()->menu));
    Result res = settings_save();
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to save settings."));
    }
}

// Moves the entry at `from` to `to`, both among the shown ones.
static void mainmenu_layout_reorder(u32 from, u32 to) {
    u8 e = layout_perm[from];
    if(from < to) {
        memmove(layout_perm + from, layout_perm + from + 1, to - from);
    } else if(from > to) {
        memmove(layout_perm + to + 1, layout_perm + to, from - to);
    }

    layout_perm[to] = e;
}

// Hides the shown entry at `from`; returns its new position (the first of the hidden).
static u32 mainmenu_layout_hide(u32 from) {
    u8 e = layout_perm[from];
    memmove(layout_perm + from, layout_perm + from + 1, layout_visible - 1 - from);
    layout_perm[layout_visible - 1] = e;
    layout_visible--;
    return layout_visible;
}

// Shows the hidden entry at `from` at position `to` (at most the end of the shown ones); returns `to`.
static u32 mainmenu_layout_show(u32 from, u32 to) {
    u8 e = layout_perm[from];
    memmove(layout_perm + to + 1, layout_perm + to, from - to);
    layout_perm[to] = e;
    layout_visible++;
    return to;
}

static u32 mainmenu_limit() {
    return layout_editing ? ENTRY_COUNT : layout_visible;
}

static bool mainmenu_can_open(const mainmenu_entry* entry) {
    return entry->open != NULL || entry->children != NULL;
}

// Advanced warns once per session.
static bool mainmenu_advanced_confirmed = false;

// The HOME theme: every entry a big tile, four to a row, its name under it.
#define HOME_COLUMNS 4
#define HOME_TILE 58

static bool mainmenu_home() {
    return theme()->def->menuLayout == THEME_MENU_TILES;
}

// Where the hidden entries are while editing, from the top of the bottom screen.
static float mainmenu_tray_top(float y1) {
    return y1 + PAD + TILE_HEIGHT + GAP + SMALL_ROW_HEIGHT + GAP + TIP_HEIGHT + 8;
}

// Position `p`'s place: tiles across the top, then one row of icons (or, in HOME, the grid of tiles); a hidden one, while
// editing, goes in the tray (or on in the grid).
static void mainmenu_button_rect(u32 p, float x1, float y1, float x2, float* x, float* y, float* w, float* h) {
    float area = x2 - x1 - 2 * PAD;
    if(mainmenu_home()) {
        float label = widget_text_height(WIDGET_SMALL_SCALE);
        *w = (area - (HOME_COLUMNS - 1) * GAP) / HOME_COLUMNS;
        *h = HOME_TILE + 4 + 2 * label;
        *x = x1 + PAD + (p % HOME_COLUMNS) * (*w + GAP);
        *y = y1 + PAD + (p / HOME_COLUMNS) * (*h + 5);
    } else if(p >= layout_visible) {
        u32 slots = ENTRY_COUNT - 2;
        *w = (area - (slots - 1) * GAP) / slots;
        *h = TRAY_HEIGHT;
        *x = x1 + PAD + (p - layout_visible) * (*w + GAP);
        *y = mainmenu_tray_top(y1);
    } else if(p < TILE_COUNT) {
        *w = (area - (TILE_COUNT - 1) * GAP) / TILE_COUNT;
        *h = TILE_HEIGHT;
        *x = x1 + PAD + p * (*w + GAP);
        *y = y1 + PAD;
    } else {
        u32 slots = ENTRY_COUNT - TILE_COUNT;
        *w = (area - (slots - 1) * GAP) / slots;
        *h = SMALL_ROW_HEIGHT;
        *x = x1 + PAD + (p - TILE_COUNT) * (*w + GAP);
        *y = y1 + PAD + TILE_HEIGHT + GAP;
    }
}

static bool mainmenu_hit(u32 p, const touchPosition* pos, float x1, float y1, float x2) {
    float x, y, w, h;
    mainmenu_button_rect(p, x1, y1, x2, &x, &y, &w, &h);
    return widget_touched(pos, x, y, w, h);
}

static float mainmenu_center(u32 p) {
    float x, y, w, h;
    mainmenu_button_rect(p, 0, 0, 320, &x, &y, &w, &h);
    return x + w / 2;
}

// The position in [from, to) whose centre is closest to `cx`.
static u32 mainmenu_nearest(u32 from, u32 to, float cx) {
    u32 best = from;
    float bestDistance = 1e9f;
    for(u32 p = from; p < to; p++) {
        float distance = mainmenu_center(p) - cx;
        distance = distance < 0 ? -distance : distance;
        if(distance < bestDistance) {
            bestDistance = distance;
            best = p;
        }
    }

    return best;
}

// The D-Pad over tiles and icons among the first `limit` positions: Left and Right go through them in order, Up and Down
// go between the rows (tiles, icons and, editing, the hidden ones), landing on the one closest across.
static u32 mainmenu_move(u32 selected, u32 key, u32 limit) {
    if(mainmenu_home()) {
        if(key == KEY_RIGHT) {
            return (selected + 1) % limit;
        } else if(key == KEY_LEFT) {
            return selected > 0 ? selected - 1 : limit - 1;
        } else if(key == KEY_DOWN) {
            if(selected + HOME_COLUMNS < limit) {
                return selected + HOME_COLUMNS;
            }

            // The last row has fewer: down from the row above lands on its last one.
            return selected < HOME_COLUMNS ? limit - 1 : selected % HOME_COLUMNS;
        } else {
            if(selected >= HOME_COLUMNS) {
                return selected - HOME_COLUMNS;
            }

            return selected + HOME_COLUMNS < limit ? selected + HOME_COLUMNS : limit - 1;
        }
    }

    if(key == KEY_RIGHT) {
        return (selected + 1) % limit;
    } else if(key == KEY_LEFT) {
        return selected > 0 ? selected - 1 : limit - 1;
    }

    // Rows: tiles [0, tileEnd), icons [TILE_COUNT, shown), hidden [shown, limit).
    u32 shown = layout_visible < limit ? layout_visible : limit;
    u32 tileEnd = shown < TILE_COUNT ? shown : TILE_COUNT;
    u32 starts[3] = {0, TILE_COUNT, shown};
    u32 ends[3] = {tileEnd, shown, limit};
    int row = selected >= shown ? 2 : selected >= TILE_COUNT ? 1 : 0;
    for(int step = 1; step <= 3; step++) {
        int next = ((row + (key == KEY_DOWN ? step : 3 - step)) % 3);
        if(starts[next] < ends[next]) {
            return mainmenu_nearest(starts[next], ends[next], mainmenu_center(selected));
        }
    }

    return selected;
}

static bool mainmenu_repeat(mainmenu_data* data, u32 key) {
    u32 down = hidKeysDown();
    if(down & key) {
        data->nextMove = osGetTime() + 400;
        return true;
    }

    if((hidKeysHeld() & key) && osGetTime() >= data->nextMove) {
        data->nextMove = osGetTime() + 120;
        return true;
    }

    return false;
}

// ---- the top screen: an entry's card ----

// The card for an entry (or System's own screen), and for a group what it holds.
static void mainmenu_draw_entry(const mainmenu_entry* entry, float x1, float y1, float x2, float y2) {
    const theme_current* t = theme();
    if(entry->drawTop != NULL) {
        entry->drawTop(x1, y1, x2, y2);
        return;
    }

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = 96;
    widget_card(cardX, cardY, cardW, cardH);

    float tile = 64;
    float tileX = cardX + 16;
    float tileY = cardY + (cardH - tile) / 2;
    float cardDepth = screen_get_depth();
    screen_set_depth(cardDepth + 1);
    // What warns before opening shows its tile in a soft red.
    screen_fill_round_rect(tileX, tileY, tile, tile, t->def->radius, entry->warn ? theme_mix(t->palette->surface, t->palette->danger, 0x26) : t->accentSoft);
    screen_draw_icon(entry->icon, tileX + 14, tileY + 14, 36, entry->warn ? t->palette->danger : t->accent);
    screen_set_depth(cardDepth);

    float textX = tileX + tile + 14;
    float wrap = cardX + cardW - 14;
    screen_draw_text(_(entry->name), textX, cardY + 14, WIDGET_TITLE_SCALE, t->palette->text);
    screen_draw_text_wrap(_(entry->description), textX, cardY + 14 + widget_text_height(WIDGET_TITLE_SCALE) + 4, WIDGET_TEXT_SCALE, t->palette->textMuted, wrap);

    // A group: what is inside, as badges along the bottom of the card.
    if(entry->children != NULL) {
        float bx = textX;
        float by = cardY + cardH - 12 - widget_text_height(WIDGET_SMALL_SCALE);
        for(u32 i = 0; i < entry->childCount; i++) {
            const char* name = _(entry->children[i].name);
            float w = widget_text_width(name, WIDGET_SMALL_SCALE) + 12;
            if(bx + w > wrap) {
                break;
            }

            widget_badge(bx, by, name, entry->warn ? theme_mix(t->palette->surface, t->palette->danger, 0x26) : t->accentSoft, entry->warn ? t->palette->danger : t->accent);
            bx += w + 5;
        }
    }
}

// The space it manages, under the card.
static void mainmenu_draw_storage(const mainmenu_entry* entry, float x1, float y2, float x2) {
    float cardX = x1 + 12;
    float cardW = x2 - x1 - 24;
    if(entry->storage >= 0) {
        bool two = entry->storage2 >= 0;
        widget_storage((FS_SystemMediaType) entry->storage, cardX + 4, y2 - (two ? 78 : 52), cardW - 8);
        if(two) {
            widget_storage((FS_SystemMediaType) entry->storage2, cardX + 4, y2 - 52, cardW - 8);
        }
    }
}

// ---- a group: its entries in a list ----

static void mainmenu_group_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    const mainmenu_entry* entry = (const mainmenu_entry*) item->data;
    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    u32 muted = selected ? THEME_ALPHA(t->onAccent, 0xC0) : t->palette->textMuted;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    screen_draw_icon(entry->icon, x + 9, y + (height - 16) / 2, 16, selected ? t->onAccent : t->accent);
    float tx = x + 33;
    float right = x + width - 9;
    // What opens another screen says so; System's details only show.
    if(entry->open != NULL) {
        right = widget_row_chevron(right, y, height, selected);
    }

    float textY = y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2;
    const char* name = _(entry->name);
    widget_text_fit(name, tx, textY, WIDGET_TEXT_SCALE, ink, right - tx);

    // The short line on the right, where it fits.
    const char* summary = _(entry->summary);
    float nameW = widget_text_width(name, WIDGET_TEXT_SCALE);
    float summaryW = widget_text_width(summary, WIDGET_SMALL_SCALE);
    if(tx + nameW + 12 + summaryW <= right) {
        screen_draw_text(summary, right - summaryW, y + (height - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, muted);
    }
}

static void mainmenu_group_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL) {
        return;
    }

    const mainmenu_entry* entry = (const mainmenu_entry*) selected->data;
    mainmenu_draw_entry(entry, x1, y1, x2, y2);
    mainmenu_draw_storage(entry, x1, y2, x2);
}

static void mainmenu_group_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    const mainmenu_entry* group = (const mainmenu_entry*) data;
    view->name = _(group->name);
    // System's details only show: nothing to open.
    bool opens = selected == NULL || ((const mainmenu_entry*) selected->data)->open != NULL;
    view->info = opens ? _(" open    back") : _(" back");

    if(hidKeysDown() & KEY_B) {
        ui_pop();

        linked_list_iter iter;
        linked_list_iterate(items, &iter);
        while(linked_list_iter_has_next(&iter)) {
            free(linked_list_iter_next(&iter));
            linked_list_iter_remove(&iter);
        }

        list_destroy(view);
        return;
    }

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < group->childCount; i++) {
            list_item* item = (list_item*) calloc(1, sizeof(list_item));
            if(item != NULL) {
                string_copy(item->name, _(group->children[i].name), sizeof(item->name));
                item->color = COLOR_TEXT;
                item->data = (void*) &group->children[i];
                linked_list_add(items, item);
            }
        }
    }

    if(selected != NULL && ((const mainmenu_entry*) selected->data)->open != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        ((const mainmenu_entry*) selected->data)->open();
    }
}

static void mainmenu_group_show(const mainmenu_entry* group) {
    ui_view* view = list_display(_(group->name), _(" open    back"), (void*) group, mainmenu_group_update, mainmenu_group_draw_top);
    if(view == NULL) {
        return;
    }

    list_style style = {
            .rowHeight = 30,
            .drawRow = mainmenu_group_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
}

static void mainmenu_advanced_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        mainmenu_advanced_confirmed = true;
        mainmenu_group_show((const mainmenu_entry*) data);
    }
}

static void mainmenu_open_entry(const mainmenu_entry* entry) {
    if(entry->children == NULL) {
        if(entry->open != NULL) {
            entry->open();
        }

        return;
    }

    if(entry->warn && !mainmenu_advanced_confirmed) {
        prompt_display_warning(_(entry->name), _("Here you change the console's own memory and system data. A wrong change can stop it from starting.\n\nGo in?"), COLOR_TEXT, (void*) entry, NULL, mainmenu_advanced_onresponse);
        return;
    }

    mainmenu_group_show(entry);
}

// ---- the menu ----

// While an entry is dragged, the layout follows the finger at once: the entry sits where it would land if let go, the
// others make way (they ease into their new places when drawn), and it becomes a tile over the tiles, an icon over the icons
// and a hidden one over the tray.
static void mainmenu_drag_apply(mainmenu_data* menu, float x1, float y1, float x2) {
    u32 cur = (u32) menu->dragFrom;
    bool fromShown = cur < layout_visible;
    bool intoHidden = false;
    u32 target = cur;
    float fx = menu->dragX - x1 - PAD;

    if(mainmenu_home()) {
        float cx, cy, cw, ch;
        mainmenu_button_rect(0, x1, y1, x2, &cx, &cy, &cw, &ch);
        int col = (int) (fx / (cw + GAP));
        col = col < 0 ? 0 : col >= HOME_COLUMNS ? HOME_COLUMNS - 1 : col;
        int row = (int) ((menu->dragY - y1 - PAD) / (ch + 5));
        row = row < 0 ? 0 : row;
        u32 index = (u32) row * HOME_COLUMNS + (u32) col;
        if(index >= layout_visible) {
            intoHidden = fromShown;
            target = layout_visible;
        } else {
            target = index;
        }
    } else if(menu->dragY >= mainmenu_tray_top(y1) - 4) {
        intoHidden = true;
    } else if(menu->dragY < y1 + PAD + TILE_HEIGHT + GAP / 2) {
        // Over the tiles.
        float tileW = (x2 - x1 - 2 * PAD - (TILE_COUNT - 1) * GAP) / TILE_COUNT;
        int slot = (int) (fx / (tileW + GAP));
        target = (u32) (slot < 0 ? 0 : slot >= TILE_COUNT ? TILE_COUNT - 1 : slot);
    } else {
        // Over the icons.
        u32 slots = ENTRY_COUNT - TILE_COUNT;
        float slotW = (x2 - x1 - 2 * PAD - (slots - 1) * GAP) / slots;
        int slot = (int) (fx / (slotW + GAP));
        target = TILE_COUNT + (u32) (slot < 0 ? 0 : slot >= (int) slots ? (int) slots - 1 : slot);
    }

    // A hidden entry can come in at the end of the shown ones; a shown one only moves among them.
    u32 last = fromShown ? layout_visible - 1 : layout_visible;
    if(target > last) {
        target = last;
    }

    if(intoHidden) {
        if(fromShown && !mainmenu_pinned(layout_perm[cur])) {
            menu->dragFrom = (int) mainmenu_layout_hide(cur);
        }
    } else if(fromShown) {
        if(target != cur) {
            mainmenu_layout_reorder(cur, target);
            menu->dragFrom = (int) target;
        }
    } else {
        menu->dragFrom = (int) mainmenu_layout_show(cur, target);
    }

    menu->selected = (u32) menu->dragFrom;
}

static void mainmenu_edit_exit(mainmenu_data* menu) {
    layout_editing = false;
    menu->grabbed = false;
    menu->dragFrom = -1;
    if(menu->selected >= layout_visible) {
        menu->selected = layout_visible - 1;
    }

    mainmenu_layout_save();
}

static void mainmenu_reset_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        mainmenu_data* menu = (mainmenu_data*) data;
        mainmenu_layout_default();
        menu->selected = 0;
        menu->grabbed = false;
        menu->dragFrom = -1;
        mainmenu_layout_save();
    }
}

// Editing: A picks an entry up (and the D-Pad moves it) or brings a hidden one back, X hides, Y puts everything back,
// SELECT or B finish. By touch, drag an entry to a place or down to the hidden ones; tap a hidden one to bring it back.
static void mainmenu_edit_update(mainmenu_data* menu, float bx1, float by1, float bx2) {
    u32 down = hidKeysDown();
    if((down & KEY_SELECT) || ((down & KEY_B) && !menu->grabbed)) {
        mainmenu_edit_exit(menu);
        return;
    }

    if(down & KEY_B) {
        menu->grabbed = false;
    }

    static const u32 keys[] = {KEY_DOWN, KEY_UP, KEY_RIGHT, KEY_LEFT};
    for(u32 i = 0; i < COUNT_OF(keys); i++) {
        if(mainmenu_repeat(menu, keys[i])) {
            if(menu->grabbed) {
                u32 target = mainmenu_move(menu->selected, keys[i], layout_visible);
                if(target != menu->selected) {
                    mainmenu_layout_reorder(menu->selected, target);
                    menu->selected = target;
                }
            } else {
                menu->selected = mainmenu_move(menu->selected, keys[i], ENTRY_COUNT);
            }

            break;
        }
    }

    if(down & KEY_A) {
        if(menu->selected >= layout_visible) {
            menu->selected = mainmenu_layout_show(menu->selected, layout_visible);
        } else {
            menu->grabbed = !menu->grabbed;
        }
    }

    if((down & KEY_X) && !menu->grabbed && menu->selected < layout_visible && !mainmenu_pinned(layout_perm[menu->selected])) {
        menu->selected = mainmenu_layout_hide(menu->selected);
    }

    if(down & KEY_Y) {
        prompt_display_yes_no(_("Edit the menu"), _("Put the menu back as it came?"), COLOR_TEXT, menu, NULL, mainmenu_reset_onresponse);
        return;
    }

    touchPosition pos;
    hidTouchRead(&pos);
    if(down & KEY_TOUCH) {
        menu->dragFrom = -1;
        menu->dragMoved = false;
        for(u32 p = 0; p < ENTRY_COUNT; p++) {
            if(mainmenu_hit(p, &pos, bx1, by1, bx2)) {
                menu->dragFrom = (int) p;
                menu->selected = p;
                menu->grabbed = false;
                menu->dragStartX = menu->dragX = pos.px;
                menu->dragStartY = menu->dragY = pos.py;
                break;
            }
        }
    } else if(menu->dragFrom >= 0 && (hidKeysHeld() & KEY_TOUCH)) {
        menu->dragX = pos.px;
        menu->dragY = pos.py;
        float dx = menu->dragX - menu->dragStartX;
        float dy = menu->dragY - menu->dragStartY;
        if(dx * dx + dy * dy > 64) {
            menu->dragMoved = true;
        }

        if(menu->dragMoved) {
            mainmenu_drag_apply(menu, bx1, by1, bx2);
        }
    } else if(menu->dragFrom >= 0 && (hidKeysUp() & KEY_TOUCH)) {
        u32 from = (u32) menu->dragFrom;
        menu->dragFrom = -1;
        if(!menu->dragMoved) {
            // A tap: a hidden entry comes back at the end.
            menu->selected = from >= layout_visible ? mainmenu_layout_show(from, layout_visible) : from;
        }

        // A drag was applied as it went.
    }
}

static void mainmenu_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    mainmenu_data* menu = (mainmenu_data*) data;

    // Set every frame: the language can change in Settings.
    view->name = _("Main menu");
    if(layout_editing) {
        view->info = _(" move    hide    reset   SELECT done");
    } else {
        view->info = mainmenu_can_open(POS(menu->selected)) ? _(" open   START exit") : _("START exit");
    }

    if(layout_editing) {
        ui_wake();   // The entries jiggle: keep drawing.
        mainmenu_edit_update(menu, bx1, by1, bx2);
        if(menu->selected != menu->lastSelected) {
            menu->tipTime = osGetTime();
            menu->lastSelected = menu->selected;
        }

        return;
    }

    if(hidKeysDown() & KEY_START) {
        ui_pop();
        free(menu);
        ui_destroy(view);
        return;
    }

    if(hidKeysDown() & KEY_SELECT) {
        layout_editing = true;
        menu->grabbed = false;
        return;
    }

    static const u32 keys[] = {KEY_DOWN, KEY_UP, KEY_RIGHT, KEY_LEFT};
    for(u32 i = 0; i < COUNT_OF(keys); i++) {
        if(mainmenu_repeat(menu, keys[i])) {
            menu->selected = mainmenu_move(menu->selected, keys[i], layout_visible);
            break;
        }
    }

    // Touch: a button looks pressed while held and opens on release over it; held long, it starts editing the menu.
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        menu->touching = true;
        widget_press_end(&menu->press);
        for(u32 i = 0; i < layout_visible; i++) {
            if(mainmenu_hit(i, &pos, bx1, by1, bx2)) {
                widget_press_begin(&menu->press, (int) i, true, pos.px, pos.py);
                menu->selected = i;
            }
        }
    } else if(menu->touching && (hidKeysHeld() & KEY_TOUCH)) {
        // A finger sliding over the buttons moves the focus to the one under it; letting go there opens nothing.
        for(u32 i = 0; i < layout_visible; i++) {
            if(i != menu->selected && mainmenu_hit(i, &pos, bx1, by1, bx2)) {
                menu->selected = i;
                menu->press.cancelled = true;
                break;
            }
        }

        // Off the button: no longer pressed.
        if(menu->press.index >= 0) {
            if(!mainmenu_hit((u32) menu->press.index, &pos, bx1, by1, bx2)) {
                menu->press.cancelled = true;
            } else if(widget_press_long_due(&menu->press)) {
                menu->press.cancelled = true;
                layout_editing = true;
                menu->grabbed = false;
                menu->dragFrom = -1;
                return;
            }
        }
    } else if(menu->touching && (hidKeysUp() & KEY_TOUCH)) {
        menu->touching = false;
        int index = menu->press.index;
        bool open = widget_press_on(&menu->press, index);
        widget_press_end(&menu->press);
        if(open) {
            mainmenu_open_entry(POS(index));
            return;
        }
    }

    if(hidKeysDown() & KEY_A) {
        mainmenu_open_entry(POS(menu->selected));
    }

    // A new selection: its tooltip slides in.
    if(menu->selected != menu->lastSelected) {
        menu->tipTime = osGetTime();
        menu->lastSelected = menu->selected;
    }
}

typedef struct {
    mainmenu_data* menu;
    float x1, y1, x2, y2;
} mainmenu_top_context;

static void mainmenu_draw_card(void* data, bool leaving) {
    mainmenu_top_context* c = (mainmenu_top_context*) data;
    mainmenu_draw_entry(POS(leaving ? c->menu->leaving : c->menu->selected), c->x1, c->y1, c->x2, c->y2);
}

// Editing: what to do, in place of the entry's card.
static void mainmenu_draw_edit_help(float x1, float y1, float x2, float y2) {
    const theme_current* t = theme();
    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    widget_card(cardX, cardY, cardW, y2 - y1 - 28);

    float textX = cardX + 16;
    float wrap = cardX + cardW - 14;
    screen_draw_icon(ICON_SETTINGS, textX, cardY + 12, 16, t->accent);
    screen_draw_text(_("Edit the menu"), textX + 24, cardY + 12, WIDGET_TITLE_SCALE, t->palette->text);
    float y = cardY + 12 + widget_text_height(WIDGET_TITLE_SCALE) + 10;
    screen_draw_text_wrap(_("Drag an entry to move it, or pick it up with A and use the D-Pad. Drop it on the row at the bottom, or press X, to hide it; tap a hidden one to bring it back. The first three are big tiles, the rest icons. System and Settings always stay."),
                          textX, y, WIDGET_TEXT_SCALE, t->palette->textMuted, wrap);
}

static void mainmenu_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    mainmenu_data* menu = (mainmenu_data*) data;
    const theme_current* t = theme();

    if(layout_editing) {
        mainmenu_draw_edit_help(x1, y1, x2, y2);
        return;
    }

    const mainmenu_entry* entry = POS(menu->selected);

    if(menu->shown != menu->selected) {
        menu->leaving = menu->shown;
        menu->shown = menu->selected;
        menu->shownTime = osGetTime();
    }

    mainmenu_top_context context = {menu, x1, y1, x2, y2};
    widget_crossfade(widget_ease(menu->shownTime, WIDGET_CROSSFADE_TIME), mainmenu_draw_card, &context, menu->leaving != menu->shown);

    mainmenu_draw_storage(entry, x1, y2, x2);

    // Under it, what the app is (System fills the screen).
    if(entry->drawTop != NULL) {
        return;
    }

    char about[128];
    snprintf(about, sizeof(about), "%s, %s %s", APP_NAME, _("based on"), APP_UPSTREAM_VERSION);
    widget_text_centered(about, x1, x2, y2 - 22, WIDGET_SMALL_SCALE, t->palette->textMuted);
}

// A label centred in a tile; one that does not fit and has no space to split at (Japanese) is made smaller.
static void mainmenu_tile_text(const char* text, float x1, float x2, float y, float scale, u32 color) {
    float room = x2 - x1 - 8;
    float width = widget_text_width(text, scale);
    if(width > room && width > 0) {
        scale *= room / width;
    }

    widget_text_centered(text, x1, x2, y, scale, color);
}

// A big tile: the icon over the label, which takes two lines when it must.
static void mainmenu_draw_tile(float x, float y, float w, float h, u32 icon, const char* label, bool selected, bool danger) {
    const theme_current* t = theme();
    // What warns before opening (Advanced) is red, as a tile and as an icon.
    u32 onDanger = t->night ? t->palette->background : t->onAccent;
    u32 fill = selected ? (danger ? t->palette->danger : t->accent) : danger ? theme_mix(t->palette->surface, t->palette->danger, 0x26) : t->palette->surface;
    u32 edge = selected ? fill : danger ? t->palette->danger : t->palette->border;
    u32 ink = selected ? (danger ? onDanger : t->onAccent) : danger ? t->palette->danger : t->palette->text;
    u32 iconInk = selected ? (danger ? onDanger : t->onAccent) : danger ? t->palette->danger : t->accent;
    screen_frame_round_rect(x, y, w, h, t->def->radius, t->def->border, edge, fill);

    char line1[64];
    char line2[64];
    string_copy(line1, label, sizeof(line1));
    line2[0] = '\0';
    if(widget_text_width(label, WIDGET_TEXT_SCALE) > w - 8) {
        // Split at the space closest to the middle.
        const char* best = NULL;
        size_t len = strlen(label);
        for(const char* c = label; *c; c++) {
            if(*c == ' ' && (best == NULL || abs((int) (c - label) - (int) len / 2) < abs((int) (best - label) - (int) len / 2))) {
                best = c;
            }
        }

        if(best != NULL && (size_t) (best - label) < sizeof(line1)) {
            memcpy(line1, label, best - label);
            line1[best - label] = '\0';
            string_copy(line2, best + 1, sizeof(line2));
        }
    }

    float textH = widget_text_height(WIDGET_TEXT_SCALE);
    u32 lines = line2[0] != '\0' ? 2 : 1;
    float iconSize = 26;
    float top = y + (h - iconSize - 6 - lines * textH) / 2;
    screen_draw_icon(icon, x + (w - iconSize) / 2, top, iconSize, iconInk);
    mainmenu_tile_text(line1, x, x + w, top + iconSize + 6, WIDGET_TEXT_SCALE, ink);
    if(lines == 2) {
        mainmenu_tile_text(line2, x, x + w, top + iconSize + 6 + textH, WIDGET_TEXT_SCALE, ink);
    }
}

// HOME: a big square tile with the icon, and the name in up to two lines under it.
static void mainmenu_draw_home_tile(const mainmenu_entry* entry, float x, float y, float w, bool selected, widget_press* press, int index) {
    const theme_current* t = theme();
    bool danger = entry->warn;
    float tx = x + (w - HOME_TILE) / 2;
    u32 fill = selected ? (danger ? t->palette->danger : t->accent) : danger ? theme_mix(t->palette->surface, t->palette->danger, 0x26) : t->palette->surface;
    u32 edge = selected ? fill : danger ? t->palette->danger : t->palette->border;
    u32 onDanger = t->night ? t->palette->background : t->onAccent;
    u32 ink = selected ? (danger ? onDanger : t->onAccent) : danger ? t->palette->danger : t->accent;
    screen_frame_round_rect(tx, y, HOME_TILE, HOME_TILE, t->def->radius, danger ? 2 : t->def->border, edge, fill);
    screen_draw_icon(entry->icon, tx + (HOME_TILE - 32) / 2, y + (HOME_TILE - 32) / 2, 32, ink);
    widget_press_draw(press, index, tx, y, HOME_TILE, HOME_TILE, t->def->radius, ink, false);

    // The name: one line, or two split at the space nearest the middle.
    const char* label = _(entry->name);
    char line1[64];
    char line2[64];
    string_copy(line1, label, sizeof(line1));
    line2[0] = '\0';
    if(widget_text_width(label, WIDGET_SMALL_SCALE) > w) {
        const char* best = NULL;
        size_t len = strlen(label);
        for(const char* c = label; *c; c++) {
            if(*c == ' ' && (best == NULL || abs((int) (c - label) - (int) len / 2) < abs((int) (best - label) - (int) len / 2))) {
                best = c;
            }
        }

        if(best != NULL && (size_t) (best - label) < sizeof(line1)) {
            memcpy(line1, label, best - label);
            line1[best - label] = '\0';
            string_copy(line2, best + 1, sizeof(line2));
        }
    }

    u32 textColor = danger ? t->palette->danger : t->palette->text;
    float lineH = widget_text_height(WIDGET_SMALL_SCALE);
    mainmenu_tile_text(line1, x - 2, x + w + 2, y + HOME_TILE + 4, WIDGET_SMALL_SCALE, textColor);
    if(line2[0] != '\0') {
        mainmenu_tile_text(line2, x - 2, x + w + 2, y + HOME_TILE + 4 + lineH, WIDGET_SMALL_SCALE, textColor);
    }
}

// A small entry: just its icon, filled when it has the focus.
static void mainmenu_draw_small(const mainmenu_entry* entry, float x, float y, float w, float h, bool selected) {
    const theme_current* t = theme();
    bool danger = entry->warn;
    u32 onDanger = t->night ? t->palette->background : t->onAccent;
    u32 fill = selected ? (danger ? t->palette->danger : t->accent) : danger ? theme_mix(t->palette->surface, t->palette->danger, 0x26) : t->palette->surface;
    u32 edge = selected ? fill : danger ? t->palette->danger : t->palette->border;
    u32 ink = selected ? (danger ? onDanger : t->onAccent) : danger ? t->palette->danger : t->accent;
    screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, edge, fill);

    float iconSize = 32;
    screen_draw_icon(entry->icon, x + (float) (int) ((w - iconSize) / 2), y + (float) (int) ((h - iconSize) / 2), iconSize, ink);
}

// The name of the entry with the focus, in a bubble under its row of icons that slides down and fades in.
static void mainmenu_draw_tip(const mainmenu_data* menu, float x1, float y1, float x2) {
    const theme_current* t = theme();
    const mainmenu_entry* entry = POS(menu->selected);
    bool danger = entry->warn;

    float ix, iy, iw, ih;
    mainmenu_button_rect(menu->selected, x1, y1, x2, &ix, &iy, &iw, &ih);
    bool hidden = menu->selected >= layout_visible;

    const char* name = _(entry->name);
    float width = widget_text_width(name, WIDGET_TEXT_SCALE) + 24;
    float maxWidth = x2 - x1 - 2 * PAD;
    if(width > maxWidth) {
        width = maxWidth;
    }

    // Centred under the icon, kept inside the screen.
    float x = ix + (iw - width) / 2;
    if(x < x1 + PAD) {
        x = x1 + PAD;
    } else if(x + width > x2 - PAD) {
        x = x2 - PAD - width;
    }

    float p = widget_ease(menu->tipTime, TIP_TIME);
    float restY = hidden ? y1 + PAD + TILE_HEIGHT + GAP + SMALL_ROW_HEIGHT + GAP : iy + ih + GAP;
    float y = (float) (int) (restY - (1 - p) * TIP_SLIDE);
    u32 alpha = (u32) (p * 255);

    u32 fill = danger ? t->palette->danger : t->accent;
    u32 ink = danger ? (t->night ? t->palette->background : t->onAccent) : t->onAccent;

    // A small notch pointing at the icon (a hidden one is further down: none).
    if(!hidden) {
        float notch = 10;
        screen_fill_round_rect(ix + iw / 2 - notch / 2, y - notch / 2, notch, notch, 2, THEME_ALPHA(fill, alpha));
    }

    screen_fill_round_rect(x, y, width, TIP_HEIGHT, t->def->radiusSmall, THEME_ALPHA(fill, alpha));
    widget_text_fit(name, x + 12, y + (TIP_HEIGHT - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, THEME_ALPHA(ink, alpha), width - 24);
}

// Where each entry is drawn (by entry): it eases towards its place, so while one is dragged the others make way and an
// entry that changes from tile to icon (or back) grows or shrinks on the way.
static float mainmenu_anim[ENTRY_COUNT][4];
static bool mainmenu_anim_valid[ENTRY_COUNT];

// Moves entry `e`'s drawn rectangle a step towards `target`; true while it is still on its way.
static bool mainmenu_anim_step(u32 e, const float target[4], float out[4]) {
    if(!mainmenu_anim_valid[e]) {
        memcpy(mainmenu_anim[e], target, sizeof(mainmenu_anim[e]));
        mainmenu_anim_valid[e] = true;
    }

    bool moving = false;
    for(int k = 0; k < 4; k++) {
        float d = target[k] - mainmenu_anim[e][k];
        if(d > -0.5f && d < 0.5f) {
            mainmenu_anim[e][k] = target[k];
        } else {
            mainmenu_anim[e][k] += d * 0.35f;
            moving = true;
        }
    }

    memcpy(out, mainmenu_anim[e], sizeof(mainmenu_anim[e]));
    return moving;
}

static void mainmenu_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    mainmenu_data* menu = (mainmenu_data*) data;
    const theme_current* t = theme();
    bool dragging = menu->dragFrom >= 0 && menu->dragMoved;
    bool moving = false;

    // A rectangle wider than this is drawn as a tile, so the change shows as it grows.
    float area = x2 - x1 - 2 * PAD;
    float tileWidth = (area - (TILE_COUNT - 1) * GAP) / TILE_COUNT;
    float iconWidth = (area - (ENTRY_COUNT - TILE_COUNT - 1) * GAP) / (ENTRY_COUNT - TILE_COUNT);
    float asTile = (tileWidth + iconWidth) / 2;

    // The one dragged is drawn last, over the rest.
    for(int pass = 0; pass < 2; pass++) {
        for(u32 p = 0; p < mainmenu_limit(); p++) {
            bool isDragged = dragging && p == (u32) menu->dragFrom;
            if(isDragged != (pass == 1)) {
                continue;
            }

            float slot[4];
            mainmenu_button_rect(p, x1, y1, x2, &slot[0], &slot[1], &slot[2], &slot[3]);

            // The dragged entry follows the finger, in the size of the place it would land in; its place stays marked.
            float target[4] = {slot[0], slot[1], slot[2], slot[3]};
            if(isDragged) {
                target[0] = menu->dragX - slot[2] / 2;
                target[1] = menu->dragY - slot[3] / 2 - 8;
                if(!mainmenu_home()) {
                    screen_fill_round_rect(slot[0], slot[1], slot[2], slot[3], t->def->radiusSmall, THEME_ALPHA(t->palette->border, 0x70));
                }
            }

            float cur[4];
            moving |= mainmenu_anim_step(layout_perm[p], target, cur);
            float x = cur[0], y = cur[1], w = cur[2], h = cur[3];

            bool selected = p == menu->selected;
            bool hidden = p >= layout_visible;
            const mainmenu_entry* entry = POS(p);
            // An entry picked up with the D-Pad is lifted a little; while editing the others jiggle, each out of step.
            if(isDragged) {
                // Nothing more: it is in the hand.
            } else if(selected && menu->grabbed) {
                y -= 4;
            } else if(layout_editing) {
                float phase = (float) (osGetTime() % 100000) * 0.022f + (float) p * 1.9f;
                x += roundf(sinf(phase) * 1.5f);
                y += roundf(cosf(phase * 1.3f));
            }

            float radius = t->def->radiusSmall;
            if(mainmenu_home()) {
                mainmenu_draw_home_tile(entry, x, y, w, selected, &menu->press, (int) p);
                radius = t->def->radius;
            } else if(w > asTile) {
                mainmenu_draw_tile(x, y, w, h, entry->icon, _(entry->name), selected, entry->warn);
                radius = t->def->radius;
            } else {
                mainmenu_draw_small(entry, x, y, w, h, selected);
            }

            // Hidden ones are dimmed.
            if(hidden && !selected) {
                screen_fill_round_rect(x, y, w, mainmenu_home() ? HOME_TILE : h, radius, THEME_ALPHA(t->palette->background, 0xA0));
            }

            if(!mainmenu_home()) {
                u32 pressInk = selected ? t->onAccent : t->palette->text;
                if(entry->warn && selected) {
                    pressInk = t->night ? t->palette->background : t->onAccent;
                }

                widget_press_draw(&menu->press, (int) p, x, y, w, h, radius, pressInk, !layout_editing);
            }
        }
    }

    // The name of the focused icon.
    if(!mainmenu_home() && !dragging && (menu->selected >= TILE_COUNT || menu->selected >= layout_visible)) {
        mainmenu_draw_tip(menu, x1, y1, x2);
    }

    // Keep drawing while things are still gliding into place.
    if(moving) {
        ui_wake();
    }
}

void mainmenu_open() {
    mainmenu_data* data = (mainmenu_data*) calloc(1, sizeof(mainmenu_data));
    if(data == NULL) {
        error_panic("Failed to allocate main menu data.");
        return;
    }

    data->press.index = -1;
    data->dragFrom = -1;
    data->lastSelected = data->selected;
    layout_editing = false;
    mainmenu_layout_load();

    ui_view* view = ui_create();
    view->name = _("Main menu");
    view->info = _(" open   START exit");
    view->data = data;
    view->update = mainmenu_update;
    view->drawTop = mainmenu_draw_top;
    view->drawBottom = mainmenu_draw_bottom;
    ui_push(view);
}
