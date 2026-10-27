#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "../core/core.h"

// The main menu: buttons in two columns, D-Pad or touch; one tap opens.

typedef struct {
    icon_id icon;
    const char* name;
    const char* description;
    void (*open)();
    int storage;   // The media it manages (FS_SystemMediaType), or -1.
    int storage2;
    // Draws the top screen instead of the card (System); NULL for the card.
    void (*drawTop)(float x1, float y1, float x2, float y2);
} mainmenu_entry;

static const mainmenu_entry entries[] = {
        {ICON_DEVICE_SD_CARD, "SD card", "Browse the SD card: install CIAs and tickets, copy, rename and delete files.", files_open_sd, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_APPS, "Titles", "Installed titles: launch them, delete them, back up their saves or extract their icons.", titles_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
        {ICON_WORLD_DOWNLOAD, "Remote install", "Install from the network: scan a QR code, type a URL or receive URLs from a computer.", remoteinstall_open, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_TICKET, "Tickets", "Installed tickets: see which are in use and delete the ones you no longer need.", tickets_open, SYSTEM_MEDIATYPE_CTR_NAND, -1},
        {ICON_CLOCK, "Pending titles", "Titles left half installed. Deleting them frees their space.", pendingtitles_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
        {ICON_DATABASE, "Extra save data", "Extra save data of titles: browse, back up or delete it.", extsavedata_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
        {ICON_DEVICE_FLOPPY, "System save data", "System save data: browse or delete it. Handle with care.", systemsavedata_open, SYSTEM_MEDIATYPE_CTR_NAND, -1},
        {ICON_CPU, "CTR NAND", "The console's system memory. Changes here can stop it from starting.", files_open_ctr_nand, SYSTEM_MEDIATYPE_CTR_NAND, -1},
        {ICON_CPU, "TWL NAND", "The DSi-mode system memory. Changes here can break DSi games and software.", files_open_twl_nand, SYSTEM_MEDIATYPE_TWL_NAND, -1},
        {ICON_PHOTO, "TWL Photo", "The DSi camera's photo storage.", files_open_twl_photo, SYSTEM_MEDIATYPE_TWL_PHOTO, -1},
        {ICON_MUSIC, "TWL Sound", "The DSi sound recorder's storage.", files_open_twl_sound, SYSTEM_MEDIATYPE_TWL_NAND, -1},
        {ICON_DOWNLOAD, "Dump NAND", "Copy the whole system memory to the SD card as a backup.", dumpnand_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
        {ICON_DEVICE_NINTENDO, "System", "The console at a glance: model, versions, network, battery and space.", NULL, -1, -1, systeminfo_draw_top},
        {ICON_SETTINGS, "Settings", "Theme, colours, language, updates and the FTP server.", settingsmenu_open, -1, -1},
};

#define ENTRY_COUNT (sizeof(entries) / sizeof(*entries))
#define COLUMNS 2
#define BUTTON_HEIGHT 34
#define GAP 6
#define PAD 8
#define PITCH (BUTTON_HEIGHT + GAP)

typedef struct {
    u32 selected;
    float scroll;
    u64 nextMove;

    bool touching;
    bool dragged;
    widget_press press;
    u16 touchStartY;
    u16 lastTouchY;

    // The top card's contents cross-fade when the selection changes.
    u32 shown;
    u32 leaving;
    u64 shownTime;
} mainmenu_data;

static float mainmenu_total_height() {
    u32 rows = (ENTRY_COUNT + COLUMNS - 1) / COLUMNS;
    return rows * PITCH - GAP;
}

static void mainmenu_button_rect(u32 index, float x1, float y1, float x2, float scroll, float* x, float* y, float* w) {
    float area = x2 - x1 - 2 * PAD - WIDGET_SCROLLBAR_WIDTH - 4;
    *w = (area - GAP) / COLUMNS;
    *x = x1 + PAD + (index % COLUMNS) * (*w + GAP);
    *y = y1 + PAD + (index / COLUMNS) * PITCH - scroll;
}

static void mainmenu_clamp_scroll(mainmenu_data* data, float viewHeight) {
    float max = mainmenu_total_height() - viewHeight;
    if(data->scroll > max) {
        data->scroll = max;
    }

    if(data->scroll < 0) {
        data->scroll = 0;
    }
}

// Scrolls just enough to show the selected button.
static void mainmenu_reveal(mainmenu_data* data, float viewHeight) {
    float top = (data->selected / COLUMNS) * PITCH;
    if(top < data->scroll) {
        data->scroll = top;
    } else if(top + BUTTON_HEIGHT > data->scroll + viewHeight) {
        data->scroll = top + BUTTON_HEIGHT - viewHeight;
    }

    mainmenu_clamp_scroll(data, viewHeight);
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

static void mainmenu_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    mainmenu_data* menu = (mainmenu_data*) data;
    float viewHeight = by2 - by1 - 2 * PAD;

    // Set every frame: the language can change in Settings.
    view->name = _("Main menu");
    // System only shows: nothing to open.
    view->info = entries[menu->selected].open != NULL ? _("\uE000 open   START exit") : _("START exit");

    if(hidKeysDown() & KEY_START) {
        ui_pop();
        free(menu);
        ui_destroy(view);
        return;
    }

    u32 last = menu->selected;
    if(mainmenu_repeat(menu, KEY_DOWN)) {
        menu->selected = menu->selected + COLUMNS < ENTRY_COUNT ? menu->selected + COLUMNS : menu->selected % COLUMNS;
    } else if(mainmenu_repeat(menu, KEY_UP)) {
        if(menu->selected >= COLUMNS) {
            menu->selected -= COLUMNS;
        } else {
            u32 lastRowStart = ((ENTRY_COUNT - 1) / COLUMNS) * COLUMNS;
            menu->selected = lastRowStart + menu->selected < ENTRY_COUNT ? lastRowStart + menu->selected : ENTRY_COUNT - 1;
        }
    } else if(mainmenu_repeat(menu, KEY_RIGHT)) {
        menu->selected = (menu->selected + 1) % ENTRY_COUNT;
    } else if(mainmenu_repeat(menu, KEY_LEFT)) {
        menu->selected = menu->selected > 0 ? menu->selected - 1 : ENTRY_COUNT - 1;
    }

    if(menu->selected != last) {
        mainmenu_reveal(menu, viewHeight);
    }

    // Touch: a tap opens the button under it; a drag scrolls.
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        menu->touching = true;
        menu->dragged = false;
        menu->touchStartY = menu->lastTouchY = pos.py;
        widget_press_end(&menu->press);
        for(u32 i = 0; i < ENTRY_COUNT; i++) {
            float x, y, w;
            mainmenu_button_rect(i, bx1, by1, bx2, menu->scroll, &x, &y, &w);
            if(widget_touched(&pos, x, y, w, BUTTON_HEIGHT) && pos.py >= by1 && pos.py < by2) {
                widget_press_begin(&menu->press, (int) i, true, pos.px, pos.py);
                menu->selected = i;
            }
        }
    } else if(menu->touching && (hidKeysHeld() & KEY_TOUCH)) {
        if(abs((int) pos.py - (int) menu->touchStartY) > 6) {
            menu->dragged = true;
            menu->press.cancelled = true;
        }

        // Off the button: no longer pressed.
        if(menu->press.index >= 0) {
            float x, y, w;
            mainmenu_button_rect((u32) menu->press.index, bx1, by1, bx2, menu->scroll, &x, &y, &w);
            if(!widget_touched(&pos, x, y, w, BUTTON_HEIGHT)) {
                menu->press.cancelled = true;
            }
        }

        if(menu->dragged) {
            menu->scroll -= (int) pos.py - (int) menu->lastTouchY;
            mainmenu_clamp_scroll(menu, viewHeight);
        }

        menu->lastTouchY = pos.py;
    } else if(menu->touching && (hidKeysUp() & KEY_TOUCH)) {
        menu->touching = false;
        int index = menu->press.index;
        bool open = widget_press_on(&menu->press, index);
        widget_press_end(&menu->press);
        if(open) {
            if(entries[index].open != NULL) {
                entries[index].open();
            }
            return;
        }
    }

    if(hidKeysDown() & KEY_A) {
        if(entries[menu->selected].open != NULL) {
            entries[menu->selected].open();
        }
    }
}

typedef struct {
    mainmenu_data* menu;
    float x1, y1, x2, y2;
} mainmenu_top_context;

// The selected entry's card; `leaving`, the one selected before.
static void mainmenu_draw_card(void* data, bool leaving) {
    mainmenu_top_context* c = (mainmenu_top_context*) data;
    const theme_current* t = theme();
    const mainmenu_entry* entry = &entries[leaving ? c->menu->leaving : c->menu->selected];
    if(entry->drawTop != NULL) {
        entry->drawTop(c->x1, c->y1, c->x2, c->y2);
        return;
    }

    float cardX = c->x1 + 12;
    float cardY = c->y1 + 14;
    float cardW = c->x2 - c->x1 - 24;
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
    screen_draw_text(_(entry->name), textX, cardY + 16, WIDGET_TITLE_SCALE, t->palette->text);
    screen_draw_text_wrap(_(entry->description), textX, cardY + 16 + widget_text_height(WIDGET_TITLE_SCALE) + 4, WIDGET_TEXT_SCALE, t->palette->textMuted, wrap);
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

    float cardX = x1 + 12;
    float cardW = x2 - x1 - 24;

    // The space left where it installs or works, for entries that manage one.
    if(entry->storage >= 0) {
        bool two = entry->storage2 >= 0;
        widget_storage((FS_SystemMediaType) entry->storage, cardX + 4, y2 - (two ? 78 : 52), cardW - 8);
        if(two) {
            widget_storage((FS_SystemMediaType) entry->storage2, cardX + 4, y2 - 52, cardW - 8);
        }
    }

    // Under it, what the app is (System fills the screen).
    if(entry->drawTop != NULL) {
        return;
    }

    char about[128];
    snprintf(about, sizeof(about), "%s, %s %s", APP_NAME, _("based on"), APP_UPSTREAM_VERSION);
    widget_text_centered(about, x1, x2, y2 - 22, WIDGET_SMALL_SCALE, t->palette->textMuted);
}

static void mainmenu_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    mainmenu_data* menu = (mainmenu_data*) data;
    float viewHeight = y2 - y1 - 2 * PAD;
    mainmenu_clamp_scroll(menu, viewHeight);

    screen_set_clip(x1, y1 + PAD / 2, x2 - x1, y2 - y1 - PAD);
    for(u32 i = 0; i < ENTRY_COUNT; i++) {
        float x, y, w;
        mainmenu_button_rect(i, x1, y1, x2, menu->scroll, &x, &y, &w);

        if(y + BUTTON_HEIGHT < y1 || y > y2) {
            continue;
        }

        widget_button(x, y, w, BUTTON_HEIGHT, entries[i].icon, _(entries[i].name), i == menu->selected);
        widget_press_draw(&menu->press, (int) i, x, y, w, BUTTON_HEIGHT, theme()->def->radiusSmall, i == menu->selected ? theme()->onAccent : theme()->palette->text, false);
    }

    screen_clear_clip();

    widget_scrollbar(x2 - PAD - WIDGET_SCROLLBAR_WIDTH, y1 + PAD, viewHeight, menu->scroll, viewHeight, mainmenu_total_height());
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
    view->info = _("\uE000 open   START exit");
    view->data = data;
    view->update = mainmenu_update;
    view->drawTop = mainmenu_draw_top;
    view->drawBottom = mainmenu_draw_bottom;
    ui_push(view);
}
