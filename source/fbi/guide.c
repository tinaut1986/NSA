#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "guide.h"
#include "resources.h"
#include "section.h"
#include "settings.h"
#include "../core/core.h"

// The guide's chapters, its viewer and Settings -> Guide (guide.h says how chapters are offered).
//
// Adding a chapter for a new feature: append it to `chapters` (keep the order: a chapter comes after the ones it
// `needs`) with a new id and the version it ships in as `since`, and add its strings to i18n_es.c. Consoles that
// update are offered it once; ids are kept in settings.json, so never rename one.

#define GUIDE_VERSION(major, minor) (((major) << 8) | (minor))
// What a console that ran NSA before the guide existed is taken to know: the chapters of v0.8 are new to it.
#define GUIDE_BEFORE GUIDE_VERSION(0, 7)

typedef struct {
    icon_id icon;
    const char* title;
    const char* text;
} guide_page;

typedef struct {
    const char* id;          // Stable: kept in settings.json.
    u32 since;               // GUIDE_VERSION of the release that brought it.
    bool essential;          // Offered on a first start.
    const char* needs;       // A chapter to see first (its id), or NULL.
    icon_id icon;
    const char* name;
    const char* summary;     // One line for Settings -> Guide.
    const guide_page* pages;
    u32 pageCount;
} guide_chapter;

#define COUNT_OF(a) (sizeof(a) / sizeof(*(a)))

static const guide_page basics_pages[] = {
        {ICON_DEVICE_GAMEPAD_2, "Two screens", "You choose on the bottom screen, with the D-Pad or a finger. The top screen tells you about what is selected, and the bar at the bottom lists the buttons that work there."},
        {ICON_CHECK, " opens,  goes back", " or a tap opens what is selected.  goes back one screen, and so does the arrow at the top left. Slide a finger to scroll a long list."},
        {ICON_DOTS, "Hold for more", "Hold , or keep a finger on an item: a circle fills and its actions open (install, copy, delete...). Whatever deletes asks first, in red."},
        {ICON_FILTER, "Tools at the top", "Lists have tools at the top right:  filters and sorts,  searches and  switches between rows and icons. NSA remembers them next time."},
        {ICON_KEYBOARD, "Typing", "Searches, names and links use NSA's keyboard on the bottom screen:  deletes,  adds a space, SELECT changes the capitals and  or START accepts."},
};

static const guide_page menu_pages[] = {
        {ICON_LAYOUT_GRID, "Tiles and icons", "The first three entries are big tiles and the rest a row of icons. The one with the focus shows its name, and the top screen says what it does."},
        {ICON_PENCIL, "Make it yours", "SELECT, or holding an entry, edits the menu: drag the entries, or pick one up with  and move it with the D-Pad.  hides one,  puts it all back. SELECT again when done."},
};

static const guide_page titles_pages[] = {
        {ICON_APPS, "Everything installed", "Titles lists your games and apps with their icons. Updates and DLC wait under their game: a number on its icon says how many, and opening it shows them."},
        {ICON_DOTS, "A title's actions", " opens what you can do with a title: launch it, delete it, export it as a CIA, send it to another 3DS or look after its saves."},
        {ICON_FILTER, "Find it fast", " filters by kind, place and region and sorts;  searches by name;  shows a grid of icons and SELECT changes their size."},
};

static const guide_page files_pages[] = {
        {ICON_DEVICE_SD_CARD, "Browse", "SD card shows your folders and files, with an icon for each kind. The top screen shows the selected one: a CIA with its icon and sizes.  goes up a folder."},
        {ICON_PHOTO, "Open files", "Tap a picture to see it (3D photos too), a text to read or edit it, or a song to play it. A .3dsx starts from its actions."},
        {ICON_CLIPBOARD, "Copy, move, install", "Hold a file for its actions: install a CIA, copy, cut, rename, delete or send it.  has the folder's own: paste, or install all its CIAs."},
};

static const guide_page remote_pages[] = {
        {ICON_QRCODE, "From a link", "Scan a QR code or type a link to a CIA. NSA asks how: install it, save it first, or only save it in /nsa/received/."},
        {ICON_DEVICE_DESKTOP, "From a computer", "Receive from a computer takes the CIAs sent with servefiles.py or any tool made for FBI, and installs them as they arrive."},
        {ICON_HISTORY, "History", "Every link is kept in History, to install it again with one press.  repeats the last one."},
};

static const guide_page saves_pages[] = {
        {ICON_DEVICE_FLOPPY, "Back up a save", "A title's actions have Save data, Extra data and SpotPass data. Each opens a list: Back up now copies it to /nsa/saves/ on the SD card."},
        {ICON_HISTORY, "Restore", "Under it, every backup, newest first: NSA's and those of Checkpoint, JKSM and PKSM, each with its badge. Open one to restore it (it asks in red) or delete it."},
        {ICON_3DS, "Share it", "Send to another 3DS sends the save: the other console imports it at once or keeps it for later."},
};

static const guide_page console_pages[] = {
        {ICON_3DS, "Send", "A file, a folder, a title or a save can go to another 3DS running NSA: choose Send to another 3DS in its actions, then the console in the list."},
        {ICON_WIFI, "With or without a router", "Two consoles on the same Wi-Fi find each other. Away from it,  switches to the local wireless, console to console."},
        {ICON_DOWNLOAD, "Receive", "On the other one: Remote install, Receive from another 3DS. It asks whether to install, save or reject what comes. A cut transfer goes on where it stopped."},
};

static const guide_page store_pages[] = {
        {ICON_PACKAGE, "Homebrew catalogue", "Store lists the apps of Universal-DB with their icons. The top screen shows the author, version and description, and a mark says what you already have or can update."},
        {ICON_DOWNLOAD, "Get an app", " gets the selected one: a CIA installs, a .3dsx goes to /3ds/.  searches;  filters by kind, category and what you have."},
};

static const guide_page computer_pages[] = {
        {ICON_SERVER, "FTP", "The FTP server is on: an FTP program on your computer copies files to and from the SD card. Its address is in the top bar and in Settings."},
        {ICON_DEVICE_DESKTOP, "Web page", "Turn on Settings, Web server, and open the address it shows in your computer's browser with the PIN: files, titles, saves, installs and the screens live."},
        {ICON_SHIELD_LOCK, "At home only", "Both are plain, unencrypted connections: use them on your own Wi-Fi. The PIN changes every time the web server starts."},
};

static const guide_page look_pages[] = {
        {ICON_PALETTE, "The look", "Settings has three themes (Cards, Pixel and HOME), accent colours and a night mode, by hand or from 20:00 to 8:00."},
        {ICON_MUSIC, "Sound", "Interface sounds and the background music each have their volume. The console makes the music itself: Lift, Calm or Shop."},
        {ICON_LANGUAGE, "Language", "NSA speaks 11 languages. System follows the console's own."},
};

static const guide_page cleanup_pages[] = {
        {ICON_STACK_2, "Cleanup and data", "Tickets, half-finished installs and the games' extra data: free space or back it up."},
        {ICON_3DS, "System", "The console at a glance: model, versions, network, storage and battery, in detail."},
        {ICON_ALERT_TRIANGLE, "Advanced", "The console's own memories. NSA asks before going in: a wrong change there can stop the console from starting."},
};

static const guide_page updates_pages[] = {
        {ICON_REFRESH, "Updates", "NSA looks for a new version when it starts and asks before installing it. Stable has tested releases; Beta has them first."},
        {ICON_INFO_CIRCLE, "This guide", "Settings, Guide keeps every chapter. When an update adds something, NSA offers only what is new."},
};

static const guide_chapter chapters[] = {
        {"basics", GUIDE_VERSION(0, 2), true, NULL, ICON_DEVICE_GAMEPAD_2, "Getting around", "Screens, buttons, touch and the keyboard", basics_pages, COUNT_OF(basics_pages)},
        {"menu", GUIDE_VERSION(0, 8), true, "basics", ICON_LAYOUT_GRID, "Your main menu", "Tiles and icons, in your order", menu_pages, COUNT_OF(menu_pages)},
        {"titles", GUIDE_VERSION(0, 2), true, "basics", ICON_APPS, "Titles", "Your games and apps", titles_pages, COUNT_OF(titles_pages)},
        {"files", GUIDE_VERSION(0, 2), true, "basics", ICON_DEVICE_SD_CARD, "SD card", "Files, pictures, music and CIAs", files_pages, COUNT_OF(files_pages)},
        {"remote", GUIDE_VERSION(0, 2), true, "basics", ICON_WORLD_DOWNLOAD, "Remote install", "Install from a QR code, a link or a computer", remote_pages, COUNT_OF(remote_pages)},
        {"saves", GUIDE_VERSION(0, 5), false, "titles", ICON_DEVICE_FLOPPY, "Saves and backups", "Back up, restore and share saves", saves_pages, COUNT_OF(saves_pages)},
        {"console", GUIDE_VERSION(0, 3), false, "basics", ICON_3DS, "Console to console", "Send anything to another 3DS", console_pages, COUNT_OF(console_pages)},
        {"store", GUIDE_VERSION(0, 8), false, "basics", ICON_PACKAGE, "Store", "Homebrew from Universal-DB", store_pages, COUNT_OF(store_pages)},
        {"computer", GUIDE_VERSION(0, 5), false, "basics", ICON_DEVICE_DESKTOP, "From your computer", "FTP and the web page", computer_pages, COUNT_OF(computer_pages)},
        {"look", GUIDE_VERSION(0, 4), false, NULL, ICON_PALETTE, "Look and sound", "Themes, colours, music and language", look_pages, COUNT_OF(look_pages)},
        {"cleanup", GUIDE_VERSION(0, 2), false, "basics", ICON_STACK_2, "Cleanup and System", "Tickets, extra data, System and Advanced", cleanup_pages, COUNT_OF(cleanup_pages)},
        {"updates", GUIDE_VERSION(0, 3), false, NULL, ICON_REFRESH, "Updates and this guide", "New versions, and what each brings", updates_pages, COUNT_OF(updates_pages)},
};

#define CHAPTER_COUNT COUNT_OF(chapters)
_Static_assert(CHAPTER_COUNT <= 32, "a queue holds every chapter");

// ---- what has been seen (settings.json) ----

static int guide_find(const char* id) {
    for(u32 i = 0; i < CHAPTER_COUNT; i++) {
        if(strcmp(chapters[i].id, id) == 0) {
            return (int) i;
        }
    }

    return -1;
}

// guide_seen is "id,id,...".
static bool guide_seen(u32 chapter) {
    const char* list = settings_get()->guideSeen;
    size_t len = strlen(chapters[chapter].id);
    for(const char* p = list; *p != '\0';) {
        const char* end = strchr(p, ',');
        size_t tokenLen = end != NULL ? (size_t) (end - p) : strlen(p);
        if(tokenLen == len && strncmp(p, chapters[chapter].id, len) == 0) {
            return true;
        }

        if(end == NULL) {
            break;
        }

        p = end + 1;
    }

    return false;
}

static void guide_save() {
    Result res = settings_save();
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, _("Failed to save settings."));
    }
}

static void guide_mark_seen(u32 chapter) {
    if(guide_seen(chapter)) {
        return;
    }

    settings_data* settings = settings_get();
    size_t used = strlen(settings->guideSeen);
    size_t len = strlen(chapters[chapter].id);
    if(used + len + 2 > sizeof(settings->guideSeen)) {
        return;
    }

    if(used > 0) {
        settings->guideSeen[used++] = ',';
    }

    memcpy(settings->guideSeen + used, chapters[chapter].id, len + 1);
}

void guide_progress(u32* seen, u32* total) {
    u32 count = 0;
    for(u32 i = 0; i < CHAPTER_COUNT; i++) {
        if(guide_seen(i)) {
            count++;
        }
    }

    *seen = count;
    *total = CHAPTER_COUNT;
}

static u32 guide_newest() {
    u32 newest = 0;
    for(u32 i = 0; i < CHAPTER_COUNT; i++) {
        if(chapters[i].since > newest) {
            newest = chapters[i].since;
        }
    }

    return newest;
}

// ---- a queue of chapters to play ----

typedef struct {
    u8 items[CHAPTER_COUNT];
    u32 count;
} guide_queue;

static bool guide_queue_has(const guide_queue* queue, u32 chapter) {
    for(u32 i = 0; i < queue->count; i++) {
        if(queue->items[i] == chapter) {
            return true;
        }
    }

    return false;
}

// Adds a chapter and, before it, what it needs that was not seen. Kept in the table's order, where what a chapter
// needs always comes first.
static void guide_queue_add(guide_queue* queue, u32 chapter) {
    if(guide_queue_has(queue, chapter) || queue->count >= CHAPTER_COUNT) {
        return;
    }

    const char* needs = chapters[chapter].needs;
    int need = needs != NULL ? guide_find(needs) : -1;
    if(need >= 0 && (u32) need != chapter && !guide_seen((u32) need)) {
        guide_queue_add(queue, (u32) need);
    }

    u32 at = queue->count;
    while(at > 0 && queue->items[at - 1] > chapter) {
        queue->items[at] = queue->items[at - 1];
        at--;
    }

    queue->items[at] = (u8) chapter;
    queue->count++;
}

static void guide_queue_unseen(guide_queue* queue) {
    for(u32 i = 0; i < CHAPTER_COUNT; i++) {
        if(!guide_seen(i)) {
            guide_queue_add(queue, i);
        }
    }
}

// ---- the viewer ----

#define BUTTON_HEIGHT 34
#define PAD 8
#define ROW_HEIGHT 24

typedef struct {
    guide_queue queue;
    u32 position;      // In the queue.
    u32 page;
    void (*onDone)();

    // The page's contents cross-fade when it changes: (position << 8) | page.
    u32 shown;
    u32 leaving;
    u64 shownTime;

    widget_press press;   // 0: previous, 1: next.
    float bx1, by1, bx2, by2;
} guide_view_data;

typedef struct {
    guide_view_data* data;
    float x1, y1, x2, y2;
} guide_top_context;

static const guide_chapter* guide_chapter_at(const guide_view_data* data, u32 position) {
    return &chapters[data->queue.items[position]];
}

static bool guide_at_start(const guide_view_data* data) {
    return data->position == 0 && data->page == 0;
}

static bool guide_at_end(const guide_view_data* data) {
    return data->position + 1 >= data->queue.count && data->page + 1 >= guide_chapter_at(data, data->position)->pageCount;
}

static void guide_close(ui_view* view, guide_view_data* data) {
    guide_save();

    void (*onDone)() = data->onDone;
    ui_pop();
    ui_destroy(view);
    free(data);

    if(onDone != NULL) {
        onDone();
    }
}

// A chapter counts as seen once its last page is shown (or it is skipped).
static void guide_note_page(guide_view_data* data) {
    if(data->page + 1 >= guide_chapter_at(data, data->position)->pageCount) {
        guide_mark_seen(data->queue.items[data->position]);
    }
}

static void guide_next(ui_view* view, guide_view_data* data) {
    if(guide_at_end(data)) {
        guide_close(view, data);
        return;
    }

    if(data->page + 1 < guide_chapter_at(data, data->position)->pageCount) {
        data->page++;
    } else {
        data->position++;
        data->page = 0;
    }

    guide_note_page(data);
}

static void guide_previous(guide_view_data* data) {
    if(data->page > 0) {
        data->page--;
    } else if(data->position > 0) {
        data->position--;
        data->page = guide_chapter_at(data, data->position)->pageCount - 1;
    }
}

static void guide_skip(ui_view* view, guide_view_data* data) {
    guide_mark_seen(data->queue.items[data->position]);
    if(data->position + 1 >= data->queue.count) {
        guide_close(view, data);
        return;
    }

    data->position++;
    data->page = 0;
    guide_note_page(data);
}

static void guide_button_rect(const guide_view_data* data, u32 index, float* x, float* y, float* w, float* h) {
    float width = (data->bx2 - data->bx1 - 3 * PAD) / 2;
    *x = data->bx1 + PAD + index * (width + PAD);
    *y = data->by2 - PAD - BUTTON_HEIGHT;
    *w = width;
    *h = BUTTON_HEIGHT;
}

static bool guide_button_hit(const guide_view_data* data, u32 index, const touchPosition* pos) {
    float x, y, w, h;
    guide_button_rect(data, index, &x, &y, &w, &h);
    return widget_touched(pos, x, y, w, h);
}

static void guide_update(ui_view* view, void* v, float bx1, float by1, float bx2, float by2) {
    guide_view_data* data = (guide_view_data*) v;
    data->bx1 = bx1;
    data->by1 = by1;
    data->bx2 = bx2;
    data->by2 = by2;

    view->name = _("Guide");
    view->info = data->queue.count > 1 ? _(" next    skip chapter    close") : _(" next    close");

    u32 down = hidKeysDown();
    if(down & KEY_B) {
        guide_close(view, data);
        return;
    }

    if(down & (KEY_A | KEY_RIGHT | KEY_R)) {
        guide_next(view, data);
        return;
    }

    if(down & (KEY_LEFT | KEY_L)) {
        guide_previous(data);
    }

    if((down & KEY_X) && data->queue.count > 1) {
        guide_skip(view, data);
        return;
    }

    touchPosition pos;
    hidTouchRead(&pos);
    if(down & KEY_TOUCH) {
        for(u32 i = 0; i < 2; i++) {
            if(guide_button_hit(data, i, &pos)) {
                widget_press_begin(&data->press, (int) i, true, pos.px, pos.py);
            }
        }
    } else if((hidKeysHeld() & KEY_TOUCH) && data->press.index >= 0) {
        if(!guide_button_hit(data, (u32) data->press.index, &pos)) {
            data->press.cancelled = true;
        }
    } else if((hidKeysUp() & KEY_TOUCH) && data->press.index >= 0) {
        int index = data->press.index;
        bool act = !data->press.cancelled;
        widget_press_end(&data->press);
        if(act && index == 1) {
            guide_next(view, data);
            return;
        }

        if(act && index == 0) {
            guide_previous(data);
        }
    }
}

static void guide_draw_page(void* context, bool leaving) {
    guide_top_context* c = (guide_top_context*) context;
    guide_view_data* data = c->data;
    const theme_current* t = theme();

    u32 key = leaving ? data->leaving : data->shown;
    u32 position = key >> 8;
    u32 pageIndex = key & 0xFF;
    if(position >= data->queue.count) {
        return;
    }

    const guide_chapter* chapter = guide_chapter_at(data, position);
    if(pageIndex >= chapter->pageCount) {
        return;
    }

    const guide_page* page = &chapter->pages[pageIndex];

    float x = c->x1 + 16;
    float right = c->x2 - 16;
    float y = c->y1 + 12;

    // The chapter, and where it is in the queue.
    screen_draw_icon(chapter->icon, x, y, 16, t->accent);
    char where[48] = "";
    if(data->queue.count > 1) {
        snprintf(where, sizeof(where), _("Chapter %lu of %lu"), position + 1, data->queue.count);
    }

    float whereW = where[0] != '\0' ? widget_text_width(where, WIDGET_SMALL_SCALE) : 0;
    float smallY = y + (16 - widget_text_height(WIDGET_SMALL_SCALE)) / 2;
    widget_text_fit(_(chapter->name), x + 22, smallY, WIDGET_SMALL_SCALE, t->palette->textMuted, right - whereW - 12 - (x + 22));
    if(where[0] != '\0') {
        screen_draw_text(where, right - whereW, smallY, WIDGET_SMALL_SCALE, t->palette->textMuted);
    }

    // The page: its icon and title, then the text.
    y += 30;
    screen_draw_icon(page->icon, x, y, 32, t->accent);
    float titleY = y + (32 - widget_text_height(WIDGET_TITLE_SCALE)) / 2;
    widget_text_fit(_(page->title), x + 44, titleY, WIDGET_TITLE_SCALE, t->palette->text, right - (x + 44));

    y += 46;
    screen_draw_text_wrap(_(page->text), x, y, WIDGET_TEXT_SCALE, t->palette->text, right);
}

static void guide_draw_top(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    guide_view_data* data = (guide_view_data*) v;
    const theme_current* t = theme();

    float cardX = x1 + 12;
    float cardY = y1 + 12;
    float cardW = x2 - x1 - 24;
    float cardH = y2 - y1 - 24;
    widget_card(cardX, cardY, cardW, cardH);

    u32 key = (data->position << 8) | data->page;
    if(key != data->shown) {
        data->leaving = data->shown;
        data->shown = key;
        data->shownTime = osGetTime();
    }

    guide_top_context context = {data, cardX, cardY, cardX + cardW, cardY + cardH};
    widget_crossfade(widget_ease(data->shownTime, WIDGET_CROSSFADE_TIME), guide_draw_page, &context, data->leaving != data->shown);

    // The chapter's pages as dots, the current one longer.
    u32 pages = guide_chapter_at(data, data->position)->pageCount;
    if(pages > 1) {
        float dotsW = (pages - 1) * 10 + 16;
        float dx = cardX + (cardW - dotsW) / 2;
        float dy = cardY + cardH - 14;
        for(u32 i = 0; i < pages; i++) {
            bool current = i == data->page;
            screen_fill_round_rect(dx, dy, current ? 12 : 6, 6, 3, current ? t->accent : t->palette->border);
            dx += current ? 16 : 10;
        }
    }
}

static void guide_draw_bottom(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    guide_view_data* data = (guide_view_data*) v;
    const theme_current* t = theme();

    // The chapters of this run (or, for one chapter, its pages): the ones done ticked, the current one marked.
    bool pagesOnly = data->queue.count == 1;
    const guide_chapter* only = guide_chapter_at(data, 0);
    u32 count = pagesOnly ? only->pageCount : data->queue.count;
    u32 at = pagesOnly ? data->page : data->position;

    float listTop = y1 + PAD;
    float listBottom = y2 - 2 * PAD - BUTTON_HEIGHT;
    u32 fits = (u32) ((listBottom - listTop) / ROW_HEIGHT);
    if(fits == 0) {
        fits = 1;
    }

    u32 first = 0;
    if(count > fits && at >= fits / 2) {
        first = at - fits / 2;
        if(first + fits > count) {
            first = count - fits;
        }
    }

    float rowX = x1 + PAD;
    float rowW = x2 - x1 - 2 * PAD;
    for(u32 i = first; i < count && i < first + fits; i++) {
        icon_id icon = pagesOnly ? only->pages[i].icon : guide_chapter_at(data, i)->icon;
        const char* name = pagesOnly ? only->pages[i].title : guide_chapter_at(data, i)->name;
        float y = listTop + (i - first) * ROW_HEIGHT;
        bool current = i == at;
        bool done = i < at || (!pagesOnly && guide_seen(data->queue.items[i]));
        if(current) {
            screen_fill_round_rect(rowX, y, rowW, ROW_HEIGHT - 2, t->def->radiusSmall, t->accentSoft);
        }

        u32 ink = current ? t->palette->text : t->palette->textMuted;
        screen_draw_icon(icon, rowX + 6, y + (ROW_HEIGHT - 2 - 16) / 2, 16, current ? t->accent : t->palette->textMuted);
        float textY = y + (ROW_HEIGHT - 2 - widget_text_height(WIDGET_TEXT_SCALE)) / 2;
        widget_text_fit(_(name), rowX + 28, textY, WIDGET_TEXT_SCALE, ink, rowW - 28 - 26);
        if(done && !current) {
            screen_draw_icon(ICON_CHECK, rowX + rowW - 22, y + (ROW_HEIGHT - 2 - 16) / 2, 16, t->palette->success);
        }
    }

    // Previous and next.
    const char* nextLabel = guide_at_end(data) ? _("Finish")
                            : data->page + 1 >= guide_chapter_at(data, data->position)->pageCount ? _("Next chapter")
                                                                                                    : _("Next");
    for(u32 i = 0; i < 2; i++) {
        float bx, by, bw, bh;
        guide_button_rect(data, i, &bx, &by, &bw, &bh);
        if(i == 0) {
            widget_button(bx, by, bw, bh, ICON_CHEVRON_LEFT, _("Previous"), false);
        } else {
            widget_button(bx, by, bw, bh, guide_at_end(data) ? ICON_CHECK : ICON_CHEVRON_RIGHT, nextLabel, true);
        }

        bool dim = i == 0 && guide_at_start(data);
        if(dim) {
            screen_fill_round_rect(bx, by, bw, bh, t->def->radiusSmall, THEME_ALPHA(t->palette->background, 0xA0));
        }

        widget_press_draw(&data->press, (int) i, bx, by, bw, bh, t->def->radiusSmall, i == 1 ? t->onAccent : t->palette->text, false);
    }
}

static void guide_play(const guide_queue* queue, void (*onDone)()) {
    if(queue->count == 0) {
        if(onDone != NULL) {
            onDone();
        }

        return;
    }

    guide_view_data* data = (guide_view_data*) calloc(1, sizeof(guide_view_data));
    if(data == NULL) {
        error_display(NULL, NULL, _("Failed to open the guide."));
        if(onDone != NULL) {
            onDone();
        }

        return;
    }

    data->queue = *queue;
    data->onDone = onDone;
    data->press.index = -1;
    data->shown = 0;
    data->leaving = 0;
    guide_note_page(data);

    ui_view* view = ui_create();
    view->name = _("Guide");
    view->info = "";
    view->data = data;
    view->update = guide_update;
    view->drawTop = guide_draw_top;
    view->drawBottom = guide_draw_bottom;
    view->backArrow = true;
    ui_push(view);
}

// ---- the question at start ----

static struct {
    guide_queue queue;     // What "Start" / "Show me" plays.
    guide_queue all;       // What "Everything" plays.
    bool everything;       // The question has an "Everything" option (the second).
    void (*next)();
    char text[512];
} guide_offer;

// The top screen while it asks: the chapters it would play, with how many pages each.
static void guide_offer_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    const theme_current* t = theme();

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = y2 - y1 - 28;
    widget_card(cardX, cardY, cardW, cardH);

    float x = cardX + 16;
    float right = cardX + cardW - 14;
    float y = cardY + 12;
    screen_draw_icon(ICON_INFO_CIRCLE, x, y, 16, t->accent);
    screen_draw_text(_("What you will see"), x + 24, y + (16 - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, t->palette->text);
    y += 28;

    const float rowH = 22;
    float bottom = cardY + cardH - (guide_offer.everything ? 44 : 10);
    u32 fits = (u32) ((bottom - y) / rowH);
    for(u32 i = 0; i < guide_offer.queue.count && i < fits; i++) {
        const guide_chapter* chapter = &chapters[guide_offer.queue.items[i]];
        screen_draw_icon(chapter->icon, x, y + (rowH - 16) / 2, 16, t->accent);

        char pages[32];
        snprintf(pages, sizeof(pages), _("%lu pages"), chapter->pageCount);
        float pagesW = widget_text_width(pages, WIDGET_SMALL_SCALE);
        screen_draw_text(pages, right - pagesW, y + (rowH - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, t->palette->textMuted);
        widget_text_fit(_(chapter->name), x + 24, y + (rowH - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, t->palette->text, right - pagesW - 12 - (x + 24));
        y += rowH;
    }

    // What "Everything" adds.
    if(guide_offer.everything) {
        char more[160];
        snprintf(more, sizeof(more), _(" Everything: also the other %lu chapters you have not seen."), guide_offer.all.count - guide_offer.queue.count);
        screen_draw_text_wrap(more, x, cardY + cardH - 40, WIDGET_SMALL_SCALE, t->palette->textMuted, right);
    }
}

static void guide_offer_onresponse(ui_view* view, void* data, u32 response) {
    settings_get()->guideKnown = guide_newest();
    guide_save();

    // Option 0 plays the offer, the second (when there are three) everything not seen; the last one is "Later".
    if(response == 0) {
        guide_play(&guide_offer.queue, guide_offer.next);
    } else if(response == 1 && guide_offer.everything) {
        guide_play(&guide_offer.all, guide_offer.next);
    } else if(guide_offer.next != NULL) {
        guide_offer.next();
    }
}

void guide_startup(void (*next)()) {
    settings_data* settings = settings_get();
    u32 newest = guide_newest();
    if(settings->guideKnown >= newest) {
        next();
        return;
    }

    memset(&guide_offer, 0, sizeof(guide_offer));
    guide_offer.next = next;
    guide_queue_unseen(&guide_offer.all);

    bool firstStart = settings->guideKnown == 0;
    size_t used = 0;
    if(firstStart) {
        for(u32 i = 0; i < CHAPTER_COUNT; i++) {
            if(chapters[i].essential && !guide_seen(i)) {
                guide_queue_add(&guide_offer.queue, i);
            }
        }

        snprintf(guide_offer.text, sizeof(guide_offer.text), _("Welcome to NSA!\n\nA short guide shows how it works: %lu chapters of a few pages.  stops it at any time, and Settings -> Guide keeps every chapter."),
                 guide_offer.queue.count);
    } else {
        used = (size_t) snprintf(guide_offer.text, sizeof(guide_offer.text), "%s\n", _("New in this version:"));
        for(u32 i = 0; i < CHAPTER_COUNT; i++) {
            if(chapters[i].since > settings->guideKnown && !guide_seen(i)) {
                guide_queue_add(&guide_offer.queue, i);
                if(used < sizeof(guide_offer.text)) {
                    used += (size_t) snprintf(guide_offer.text + used, sizeof(guide_offer.text) - used, "\n- %s", _(chapters[i].name));
                }
            }
        }

        // What they build on, chained before them.
        u32 added = 0;
        for(u32 i = 0; i < guide_offer.queue.count; i++) {
            if(chapters[guide_offer.queue.items[i]].since <= settings->guideKnown) {
                added++;
            }
        }

        // How many others "Everything" adds is on the top screen (guide_offer_draw_top).
        if(added > 0 && used < sizeof(guide_offer.text)) {
            snprintf(guide_offer.text + used, sizeof(guide_offer.text) - used, "\n\n%s", _("With what they build on, first."));
        }
    }

    if(guide_offer.queue.count == 0) {
        settings->guideKnown = newest;
        guide_save();
        next();
        return;
    }

    static const char* firstOptions[] = {"Start the guide", "Later"};
    static u32 firstButtons[] = {KEY_A, KEY_B};
    static const char* newOptions[] = {"Show me", "Everything", "Later"};
    static u32 newButtons[] = {KEY_A, KEY_X, KEY_B};
    static const char* newOptionsShort[] = {"Show me", "Later"};

    if(firstStart) {
        prompt_display_multi_choice("Guide", guide_offer.text, COLOR_TEXT, firstOptions, firstButtons, 2, NULL, guide_offer_draw_top, guide_offer_onresponse);
    } else if(guide_offer.all.count > guide_offer.queue.count) {
        guide_offer.everything = true;
        prompt_display_multi_choice("Guide", guide_offer.text, COLOR_TEXT, newOptions, newButtons, 3, NULL, guide_offer_draw_top, guide_offer_onresponse);
    } else {
        prompt_display_multi_choice("Guide", guide_offer.text, COLOR_TEXT, newOptionsShort, firstButtons, 2, NULL, guide_offer_draw_top, guide_offer_onresponse);
    }
}

// ---- Settings -> Guide ----

#define ROW_UNSEEN 0x100   // "Everything not seen yet".
#define ROW_RESET 0x101    // "Start again".

static u32 guide_row_of(const list_item* item) {
    return (u32) (uintptr_t) item->data;
}

static u32 guide_unseen_count() {
    u32 seen, total;
    guide_progress(&seen, &total);
    return total - seen;
}

static void guide_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    u32 row = guide_row_of(item);

    u32 fill = selected ? t->accent : t->palette->surface;
    u32 edge = selected ? t->accent : t->palette->border;
    u32 ink = selected ? t->onAccent : t->palette->text;
    screen_frame_round_rect(x, y, width, height, t->def->radiusSmall, t->def->border, edge, fill);

    icon_id icon = row == ROW_UNSEEN ? ICON_PLAYER_PLAY : row == ROW_RESET ? ICON_REFRESH : chapters[row].icon;
    screen_draw_icon(icon, x + 9, y + (height - 16) / 2, 16, selected ? t->onAccent : t->accent);

    float tx = x + 33;
    float right = x + width - 9;
    char label[96];
    if(row == ROW_UNSEEN) {
        u32 unseen = guide_unseen_count();
        if(unseen > 0) {
            snprintf(label, sizeof(label), _("Everything not seen yet (%lu)"), unseen);
        } else {
            snprintf(label, sizeof(label), "%s", _("You have seen every chapter"));
        }
    } else if(row == ROW_RESET) {
        snprintf(label, sizeof(label), "%s", _("Start again"));
    } else {
        snprintf(label, sizeof(label), "%s", _(chapters[row].name));
        if(guide_seen(row)) {
            screen_draw_icon(ICON_CHECK, right - 16, y + (float) (int) ((height - 16) / 2), 16, selected ? t->onAccent : t->palette->success);
            right -= 22;
        } else {
            const char* fresh = _("New");
            float badgeW = widget_text_width(fresh, WIDGET_SMALL_SCALE) + 12;
            widget_badge(right - badgeW, y + (height - widget_text_height(WIDGET_SMALL_SCALE) - 2) / 2, fresh, selected ? t->onAccent : t->accentSoft, t->accent);
            right -= badgeW + 6;
        }
    }

    float textY = y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2;
    widget_text_fit(label, tx, textY, WIDGET_TEXT_SCALE, ink, right - tx);
}

static void guide_list_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL) {
        return;
    }

    const theme_current* t = theme();
    u32 row = guide_row_of(selected);

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = y2 - y1 - 28;
    widget_card(cardX, cardY, cardW, cardH);

    float x = cardX + 16;
    float right = cardX + cardW - 14;
    float y = cardY + 14;

    icon_id icon = row == ROW_UNSEEN ? ICON_PLAYER_PLAY : row == ROW_RESET ? ICON_REFRESH : chapters[row].icon;
    screen_draw_icon(icon, x, y, 22, t->accent);

    char title[96];
    char text[256];
    char line[96] = "";
    if(row == ROW_UNSEEN) {
        snprintf(title, sizeof(title), "%s", _("Guide"));
        u32 seen, total;
        guide_progress(&seen, &total);
        snprintf(text, sizeof(text), "%s", seen < total ? _("Plays every chapter you have not seen, in order.  stops it; what you saw is kept.")
                                                        : _("You have seen every chapter. Pick one below to see it again."));
        snprintf(line, sizeof(line), _("%lu of %lu chapters seen"), seen, total);
    } else if(row == ROW_RESET) {
        snprintf(title, sizeof(title), "%s", _("Start again"));
        snprintf(text, sizeof(text), "%s", _("Marks every chapter as not seen, as on the first day."));
    } else {
        const guide_chapter* chapter = &chapters[row];
        snprintf(title, sizeof(title), "%s", _(chapter->name));
        snprintf(text, sizeof(text), "%s.", _(chapter->summary));
        snprintf(line, sizeof(line), _("%lu pages, since NSA %lu.%lu"), chapter->pageCount, chapter->since >> 8, chapter->since & 0xFF);
    }

    widget_text_fit(title, x + 30, y + 1, WIDGET_TITLE_SCALE, t->palette->text, right - (x + 30));
    y += 32;
    screen_draw_text_wrap(text, x, y, WIDGET_TEXT_SCALE, t->palette->textMuted, right);

    if(row < CHAPTER_COUNT) {
        // How long it is, then the titles of its pages.
        float py = cardY + cardH - 14 - chapters[row].pageCount * 20;
        screen_draw_text(line, x, py - 18, WIDGET_SMALL_SCALE, t->palette->textMuted);
        for(u32 i = 0; i < chapters[row].pageCount; i++) {
            const guide_page* page = &chapters[row].pages[i];
            screen_draw_icon(page->icon, x, py + 2, 16, t->accent);
            widget_text_fit(_(page->title), x + 24, py + (20 - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, t->palette->text, right - (x + 24));
            py += 20;
        }
    } else if(line[0] != '\0') {
        screen_draw_text(line, x, cardY + cardH - 30, WIDGET_TEXT_SCALE, t->palette->text);
    }
}

static void guide_list_free(ui_view* view, linked_list* items) {
    ui_pop();

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }

    list_destroy(view);
}

static void guide_reset_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        settings_get()->guideSeen[0] = '\0';
        guide_save();
    }
}

static void guide_list_add(linked_list* items, u32 row, const char* name) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    if(item != NULL) {
        string_copy(item->name, name, sizeof(item->name));
        item->color = COLOR_TEXT;
        item->data = (void*) (uintptr_t) row;
        linked_list_add(items, item);
    }
}

static void guide_list_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    view->name = _("Guide");
    view->info = _(" play    back");

    if(hidKeysDown() & KEY_B) {
        guide_list_free(view, items);
        return;
    }

    if(linked_list_size(items) == 0) {
        guide_list_add(items, ROW_UNSEEN, _("Everything not seen yet"));
        for(u32 i = 0; i < CHAPTER_COUNT; i++) {
            guide_list_add(items, i, _(chapters[i].name));
        }

        guide_list_add(items, ROW_RESET, _("Start again"));
    }

    if(selected == NULL || !(selectedTouched || (hidKeysDown() & KEY_A))) {
        return;
    }

    u32 row = guide_row_of(selected);
    guide_queue queue = {.count = 0};
    if(row == ROW_UNSEEN) {
        guide_queue_unseen(&queue);
        if(queue.count == 0) {
            // Seen it all: play it all again.
            for(u32 i = 0; i < CHAPTER_COUNT; i++) {
                queue.items[queue.count++] = (u8) i;
            }
        }
    } else if(row == ROW_RESET) {
        prompt_display_yes_no(_("Start again"), _("Mark every chapter of the guide as not seen?"), COLOR_TEXT, NULL, NULL, guide_reset_onresponse);
        return;
    } else {
        queue.items[queue.count++] = (u8) row;
    }

    guide_play(&queue, NULL);
}

void guide_open() {
    ui_view* view = list_display(_("Guide"), _(" play    back"), NULL, guide_list_update, guide_list_draw_top);
    if(view == NULL) {
        return;
    }

    list_style style = {
            .rowHeight = 30,
            .drawRow = guide_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
}
