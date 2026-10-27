#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "../core/core.h"

// The main menu: the three everyday entries as big tiles, then two groups
// (Cleanup and data, Advanced) and System and Settings. A group opens a list
// of its entries; Advanced asks once per session before opening.

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

#define COUNT_OF(a) (sizeof(a) / sizeof(*(a)))

static const mainmenu_entry entries[] = {
        {ICON_DEVICE_SD_CARD, "SD card", "Browse the SD card: install CIAs and tickets, copy, rename and delete files.", NULL, files_open_sd, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_APPS, "Titles", "Installed titles: launch them, delete them, back up their saves or extract their icons.", NULL, titles_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
        {ICON_WORLD_DOWNLOAD, "Remote install", "Install from the network: scan a QR code, type a URL or receive URLs from a computer.", NULL, remoteinstall_open, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_STACK_2, "Cleanup and data", "Licences, half-finished installs and the games' extra data.", NULL, NULL, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND, NULL, cleanup, COUNT_OF(cleanup), false},
        {ICON_ALERT_TRIANGLE, "Advanced", "The console's own memories and system data. Handle with care.", NULL, NULL, SYSTEM_MEDIATYPE_CTR_NAND, SYSTEM_MEDIATYPE_TWL_NAND, NULL, advanced, COUNT_OF(advanced), true},
        {ICON_DEVICE_NINTENDO, "System", "The console at a glance: model, versions, network, battery and space.", NULL, NULL, -1, -1, systeminfo_draw_top},
        {ICON_SETTINGS, "Settings", "Theme, colours, language, updates and the FTP server.", NULL, settingsmenu_open, -1, -1},
};

#define ENTRY_COUNT COUNT_OF(entries)
#define TILE_COUNT 3      // The first entries, as big tiles in one row.
#define TILE_HEIGHT 80
#define BUTTON_HEIGHT 36
#define GAP 6
#define PAD 8

typedef struct {
    u32 selected;
    u64 nextMove;

    bool touching;
    widget_press press;

    // The top card's contents cross-fade when the selection changes.
    u32 shown;
    u32 leaving;
    u64 shownTime;
} mainmenu_data;

// Advanced warns once per session.
static bool mainmenu_advanced_confirmed = false;

static bool mainmenu_can_open(const mainmenu_entry* entry) {
    return entry->open != NULL || entry->children != NULL;
}

// Tiles across the top, then two columns of buttons.
static void mainmenu_button_rect(u32 index, float x1, float y1, float x2, float* x, float* y, float* w, float* h) {
    float area = x2 - x1 - 2 * PAD;
    if(index < TILE_COUNT) {
        *w = (area - (TILE_COUNT - 1) * GAP) / TILE_COUNT;
        *h = TILE_HEIGHT;
        *x = x1 + PAD + index * (*w + GAP);
        *y = y1 + PAD;
    } else {
        u32 i = index - TILE_COUNT;
        *w = (area - GAP) / 2;
        *h = BUTTON_HEIGHT;
        *x = x1 + PAD + (i % 2) * (*w + GAP);
        *y = y1 + PAD + TILE_HEIGHT + GAP + (i / 2) * (BUTTON_HEIGHT + GAP);
    }
}

// The D-Pad over tiles and buttons: Up and Down move between rows (a tile
// row of three over rows of two), Left and Right go through them in order.
static u32 mainmenu_move(u32 selected, u32 key) {
    u32 buttons = ENTRY_COUNT - TILE_COUNT;
    u32 lastRow = (buttons - 1) / 2;
    if(key == KEY_RIGHT) {
        return (selected + 1) % ENTRY_COUNT;
    } else if(key == KEY_LEFT) {
        return selected > 0 ? selected - 1 : ENTRY_COUNT - 1;
    } else if(key == KEY_DOWN) {
        if(selected < TILE_COUNT) {
            return TILE_COUNT + (selected == TILE_COUNT - 1 ? 1 : 0);
        }

        u32 i = selected - TILE_COUNT;
        if(i / 2 < lastRow) {
            return i + 2 < buttons ? selected + 2 : ENTRY_COUNT - 1;
        }

        return (i % 2) == 0 ? 0 : TILE_COUNT - 1;
    } else {
        if(selected < TILE_COUNT) {
            u32 target = TILE_COUNT + lastRow * 2 + (selected == TILE_COUNT - 1 ? 1 : 0);
            return target < ENTRY_COUNT ? target : ENTRY_COUNT - 1;
        }

        u32 i = selected - TILE_COUNT;
        if(i < 2) {
            return i == 0 ? 0 : TILE_COUNT - 1;
        }

        return selected - 2;
    }
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
    screen_fill_round_rect(tileX, tileY, tile, tile, t->def->radius, t->accentSoft);
    screen_draw_icon(entry->icon, tileX + 14, tileY + 14, 36, t->accent);
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

            widget_badge(bx, by, name, t->accentSoft, t->accent);
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
    view->info = _(" open    back");

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

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
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
        prompt_display_yes_no(_(entry->name), _("Here you change the console's own memory and system data. A wrong change can stop it from starting.\n\nGo in?"), COLOR_TEXT, (void*) entry, NULL, mainmenu_advanced_onresponse);
        return;
    }

    mainmenu_group_show(entry);
}

// ---- the menu ----

static void mainmenu_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    mainmenu_data* menu = (mainmenu_data*) data;

    // Set every frame: the language can change in Settings.
    view->name = _("Main menu");
    // System only shows: nothing to open.
    view->info = mainmenu_can_open(&entries[menu->selected]) ? _(" open   START exit") : _("START exit");

    if(hidKeysDown() & KEY_START) {
        ui_pop();
        free(menu);
        ui_destroy(view);
        return;
    }

    static const u32 keys[] = {KEY_DOWN, KEY_UP, KEY_RIGHT, KEY_LEFT};
    for(u32 i = 0; i < COUNT_OF(keys); i++) {
        if(mainmenu_repeat(menu, keys[i])) {
            menu->selected = mainmenu_move(menu->selected, keys[i]);
            break;
        }
    }

    // Touch: a button looks pressed while held and opens on release over it.
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        menu->touching = true;
        widget_press_end(&menu->press);
        for(u32 i = 0; i < ENTRY_COUNT; i++) {
            float x, y, w, h;
            mainmenu_button_rect(i, bx1, by1, bx2, &x, &y, &w, &h);
            if(widget_touched(&pos, x, y, w, h)) {
                widget_press_begin(&menu->press, (int) i, true, pos.px, pos.py);
                menu->selected = i;
            }
        }
    } else if(menu->touching && (hidKeysHeld() & KEY_TOUCH)) {
        // Off the button: no longer pressed.
        if(menu->press.index >= 0) {
            float x, y, w, h;
            mainmenu_button_rect((u32) menu->press.index, bx1, by1, bx2, &x, &y, &w, &h);
            if(!widget_touched(&pos, x, y, w, h)) {
                menu->press.cancelled = true;
            }
        }
    } else if(menu->touching && (hidKeysUp() & KEY_TOUCH)) {
        menu->touching = false;
        int index = menu->press.index;
        bool open = widget_press_on(&menu->press, index);
        widget_press_end(&menu->press);
        if(open) {
            mainmenu_open_entry(&entries[index]);
            return;
        }
    }

    if(hidKeysDown() & KEY_A) {
        mainmenu_open_entry(&entries[menu->selected]);
    }
}

typedef struct {
    mainmenu_data* menu;
    float x1, y1, x2, y2;
} mainmenu_top_context;

static void mainmenu_draw_card(void* data, bool leaving) {
    mainmenu_top_context* c = (mainmenu_top_context*) data;
    mainmenu_draw_entry(&entries[leaving ? c->menu->leaving : c->menu->selected], c->x1, c->y1, c->x2, c->y2);
}

static void mainmenu_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    mainmenu_data* menu = (mainmenu_data*) data;
    const theme_current* t = theme();
    const mainmenu_entry* entry = &entries[menu->selected];

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

// A big tile: the icon over the label, which takes two lines when it must.
static void mainmenu_draw_tile(float x, float y, float w, float h, u32 icon, const char* label, bool selected) {
    const theme_current* t = theme();
    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
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
    screen_draw_icon(icon, x + (w - iconSize) / 2, top, iconSize, selected ? t->onAccent : t->accent);
    widget_text_centered(line1, x, x + w, top + iconSize + 6, WIDGET_TEXT_SCALE, ink);
    if(lines == 2) {
        widget_text_centered(line2, x, x + w, top + iconSize + 6 + textH, WIDGET_TEXT_SCALE, ink);
    }
}

static void mainmenu_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    mainmenu_data* menu = (mainmenu_data*) data;
    const theme_current* t = theme();

    for(u32 i = 0; i < ENTRY_COUNT; i++) {
        float x, y, w, h;
        mainmenu_button_rect(i, x1, y1, x2, &x, &y, &w, &h);

        bool selected = i == menu->selected;
        if(i < TILE_COUNT) {
            mainmenu_draw_tile(x, y, w, h, entries[i].icon, _(entries[i].name), selected);
        } else {
            widget_button(x, y, w, h, entries[i].icon, _(entries[i].name), selected);
        }

        widget_press_draw(&menu->press, (int) i, x, y, w, h, i < TILE_COUNT ? t->def->radius : t->def->radiusSmall, selected ? t->onAccent : t->palette->text, false);
    }
}

void mainmenu_open() {
    mainmenu_data* data = (mainmenu_data*) calloc(1, sizeof(mainmenu_data));
    if(data == NULL) {
        error_panic("Failed to allocate main menu data.");
        return;
    }

    data->press.index = -1;

    ui_view* view = ui_create();
    view->name = _("Main menu");
    view->info = _(" open   START exit");
    view->data = data;
    view->update = mainmenu_update;
    view->drawTop = mainmenu_draw_top;
    view->drawBottom = mainmenu_draw_bottom;
    ui_push(view);
}
