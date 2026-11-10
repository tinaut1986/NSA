#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "guide.h"
#include "resources.h"
#include "section.h"
#include "settings.h"
#include "../core/core.h"

// The guide's chapters, played as guided tours on the real screens (core/ui/coach.h: a bubble points at a button and
// waits for the user to press it), the question at start and Settings -> Guide. guide.h says how chapters are offered.
//
// Adding a chapter for a new feature: append it to `chapters` (a chapter comes after the one it `needs`) with a new id
// and the version it ships in as `since`; its steps point at anchors the screens draw (coach_anchor: "menu.<entry>",
// "tool.<name>", "list", "list.selected", "sheet", a sheet row's label, "popover", "settings.<option>", "remote.<tile>",
// "back", "hints", "top"); add its Spanish to i18n_es.c. Never rename an id: settings.json keeps them.
//
// A chapter that a release changes enough to show again (a feature it explains changed): set `updated` to that release.
// It is offered as new again, also to whoever saw it, and keeps its `since` for Settings -> Guide.
//
// Better, when a release only adds to a chapter: give the new steps their own version (the *_SINCE macros). The chapter
// is offered again with only what someone who saw it needs: the new steps, marked "New", and the steps that move between
// screens to reach them (any step that waits for a key, a tap or a screen; old explanations are left out). Settings ->
// Guide plays it whole, with "New" on the steps of the newest version.

#define GUIDE_VERSION(major, minor) (((major) << 8) | (minor))

typedef struct {
    const char* id;          // Stable: kept in settings.json.
    u32 since;               // GUIDE_VERSION of the release that brought it.
    bool essential;          // Offered on a first start.
    const char* needs;       // A chapter to see first (its id), or NULL.
    icon_id icon;
    const char* name;
    const char* summary;     // One line for Settings -> Guide.
    const coach_step* steps;
    u32 stepCount;
    u32 updated;             // GUIDE_VERSION of the last release that changed it enough to show it again; 0: none.
} guide_chapter;

#define COUNT_OF(a) (sizeof(a) / sizeof(*(a)))

// An explanation pointing at `anchor`: A goes on.
#define INFO(a_, t_) {.anchor = (a_), .text = (t_)}
// Press `k_` (or tap the anchor when `tap_`), then wait for `u_`.
#define DO(a_, t_, k_, tap_, u_) {.anchor = (a_), .text = (t_), .keys = (k_), .tap = (tap_), .until = (u_)}
// Bring the focus to `a_` (a literal) with the D-Pad.
#define REACH(a_, t_) {.anchor = (a_), .text = (t_), .pass = COACH_PASS_DPAD, .until = "@" a_}
// Open an entry of the main menu (a tap, or the D-Pad and A) and wait for its screen.
#define OPEN(a_, t_, u_) {.anchor = (a_), .text = (t_), .keys = KEY_A, .tap = true, .pass = COACH_PASS_DPAD, .until = (u_)}
#define BACK_TO_MENU(t_) {.anchor = NULL, .text = (t_), .keys = KEY_B, .until = "v:Main menu"}
// The same, for a step a later release added to its chapter.
#define INFO_SINCE(v_, a_, t_) {.anchor = (a_), .text = (t_), .since = (v_)}

// A game with updates or DLC opens their list first (titles.c draws "titles.chain" there): these steps run only then.
#define CHAIN_OPEN_GAME {.anchor = "list.selected", .text = "This game has updates or DLC: they wait here, under it. Open the game itself, the first one, with \uE000.", \
                         .keys = KEY_A, .tap = true, .pass = COACH_PASS_DPAD, .until = "sheet", .only = "titles.chain"}
#define CHAIN_BACK {.anchor = "back", .text = "\uE001 goes back to the list of titles.", .keys = KEY_B, .tap = true, .until = "!titles.chain", .only = "titles.chain"}

static const coach_step basics_steps[] = {
        INFO("top", "The top screen tells you about what has the focus on the bottom one."),
        INFO("hints", "This bar lists the buttons that work on each screen."),
        OPEN("menu.titles", "Open Titles: tap it, or bring the focus to it with the D-Pad and press \uE000.", "v:Titles"),
        INFO("list", "Every game and app installed. The top screen shows the one with the focus."),
        DO("list.selected", "Press \uE000, or tap the one with the focus: its actions open.", KEY_A, true, "sheet|titles.chain"),
        CHAIN_OPEN_GAME,
        DO("sheet", "Its actions. Whatever deletes asks first, in red. Close them with \uE001.", KEY_B, false, "!sheet"),
        CHAIN_BACK,
        DO("back", "\uE001, or this arrow, goes back.", KEY_B, true, "v:Main menu"),
        INFO(NULL, "That is all it takes. START leaves the guide at any time, and Settings, Guide keeps every chapter."),
};

static const coach_step menu_steps[] = {
        INFO("menu.tiles", "The first three entries are big tiles."),
        INFO("menu.icons", "The rest wait in this row of icons. The one with the focus shows its name."),
        DO(NULL, "Press SELECT to edit the menu (holding an entry does it too).", KEY_SELECT, false, "menu.editing"),
        INFO("menu.editing", "Now you can drag the entries, or pick one up with \uE000 and move it with the D-Pad. \uE002 hides one, \uE003 puts it all back as it came."),
        DO(NULL, "Press SELECT again to finish.", KEY_SELECT, false, "!menu.editing"),
};

static const coach_step titles_steps[] = {
        OPEN("menu.titles", "Open Titles.", "v:Titles"),
        DO("tool.filter", "\uE004, or this funnel, filters by kind, place and region, and sorts. Open it.", KEY_L, true, "popover"),
        DO("popover", "Choose what to show; NSA remembers it. Close it with \uE001.", KEY_B, false, "!popover"),
        DO("tool.grid", "\uE003 switches between rows and a grid of icons. Try it.", KEY_Y, true, NULL),
        DO("tool.rows", "\uE003 again goes back.", KEY_Y, true, NULL),
        INFO("tool.search", "\uE005 searches by name, with NSA's keyboard."),
        INFO("list.selected", "Updates and DLC wait under their game: a number on its icon says how many, and opening it shows them."),
        DO("back", "\uE001 goes back to the menu.", KEY_B, true, "v:Main menu"),
};

static const coach_step files_steps[] = {
        OPEN("menu.sd", "Open SD card.", "tool.clipboard"),
        INFO("top", "The top screen shows the file with the focus: a CIA with its icon and sizes."),
        INFO("list.selected", "\uE000 opens a folder, and a picture, a text or a song in its viewer. Hold \uE000 on a file (or keep your finger on it) to open its actions: install, copy, cut, rename, delete, send."),
        INFO_SINCE(GUIDE_VERSION(0, 11), "list.selected", "A .zip or a .7z can be extracted from its actions, on the SD card itself."),
        DO("tool.more", "\uE002 has the actions of the folder itself, such as installing all its CIAs. Open them.", KEY_X, true, "sheet"),
        DO("sheet", "Close them with \uE001.", KEY_B, false, "!sheet"),
        INFO("tool.clipboard", "What you copy or cut waits here until you paste it, even if you leave the SD card: you can paste it somewhere else, such as the CTR NAND."),
        DO(NULL, "\uE001 goes up a folder, and from the top back to the menu.", KEY_B, false, "v:Main menu"),
};

static const coach_step remote_steps[] = {
        OPEN("menu.remote", "Open Remote install.", "v:Remote install"),
        INFO("remote.qr", "Scan a QR code that holds links to CIAs."),
        INFO("remote.url", "Or type the links. NSA asks how: install, save it first, or only save it."),
        INFO("remote.pc", "Receive the CIAs a computer sends with servefiles.py or any tool made for FBI."),
        INFO("remote.history", "Every link is kept here, to install it again. \uE003 repeats the last one."),
        INFO_SINCE(GUIDE_VERSION(0, 11), "remote.url", "A download you chose to save goes on where it stopped if it was cut: Retry, or the same link again."),
        INFO_SINCE(GUIDE_VERSION(0, 11), NULL, "Installing a game that is already there keeps its save. An older version asks first, and NSA backs up the save and puts it back."),
        INFO_SINCE(GUIDE_VERSION(0, 11), NULL, "When a download, an install or a transfer ends, the notification light blinks. Any button turns it off."),
        DO("back", "\uE001 goes back to the menu.", KEY_B, true, "v:Main menu"),
};

static const coach_step saves_steps[] = {
        OPEN("menu.titles", "Open Titles.", "v:Titles"),
        DO("list.selected", "Open the actions of a game.", KEY_A, true, "sheet|titles.chain"),
        CHAIN_OPEN_GAME,
        INFO("Save data", "Save data, Extra data and SpotPass data each open a list: back it up, restore any backup (NSA's, Checkpoint's, JKSM's, PKSM's) or send it to another 3DS."),
        DO("sheet", "Close the actions with \uE001.", KEY_B, false, "!sheet"),
        CHAIN_BACK,
        DO("back", "\uE001 goes back to the menu.", KEY_B, true, "v:Main menu"),
};

static const coach_step console_steps[] = {
        OPEN("menu.remote", "Open Remote install.", "v:Remote install"),
        INFO("remote.3ds", "Here you receive what another console running NSA sends: a title, a file, a folder or a save."),
        INFO(NULL, "To send, choose Send to another 3DS in the actions of a title, a file or a save. Same Wi-Fi or none: \uE002 switches to the local wireless."),
        DO("back", "\uE001 goes back to the menu.", KEY_B, true, "v:Main menu"),
};

static const coach_step store_steps[] = {
        OPEN("menu.store", "Open Store.", "v:Store"),
        INFO("list", "Your stores: Universal-DB, and any other you add with the last row, by its QR code or its link. Holding one of yours changes or deletes it."),
        DO("list.selected", "Open Universal-DB with \uE000.", KEY_A, true, "v:Universal-DB"),
        INFO("list", "Its homebrew. The top screen shows the one with the focus, and a mark says what is already on the console."),
        INFO("list.selected", "\uE000 gets it: a CIA installs, a .3dsx goes to /3ds/."),
        DO("tool.filter", "\uE004 filters by kind, category and what you have. Open it.", KEY_L, true, "popover"),
        DO("popover", "Close it with \uE001.", KEY_B, false, "!popover"),
        DO("back", "\uE001 goes back to your stores.", KEY_B, true, "v:Store"),
        DO("back", "\uE001 again goes back to the menu.", KEY_B, true, "v:Main menu"),
};

static const coach_step computer_steps[] = {
        OPEN("menu.settings", "Open Settings.", "v:Settings"),
        REACH("settings.ftp", "Go down to FTP server with the D-Pad."),
        INFO("settings.ftp", "An FTP program on your computer copies files to and from the SD card. The address is on the top screen."),
        INFO("settings.ftp", "Anything that goes up or down over FTP or the web page shows its progress and speed at the bottom of the screen."),
        REACH("settings.web", "Now go to Web server."),
        INFO("settings.web", "Turn it on and open the address on the top screen in your computer's browser, with the PIN: files, titles, saves, installs and the screens live. Only on your own Wi-Fi."),
        INFO("settings.web", "While the web server runs, its row shows a QR code: scan it with your phone and the page opens, already signed in."),
        REACH("settings.web_pin", "Go down to Web PIN."),
        INFO_SINCE(GUIDE_VERSION(0, 12), "settings.web_pin", "Web PIN: Random makes a new PIN each time the server starts, Fixed keeps one between launches (you type it, or make a random one on the keypad), and None opens the page without a PIN, only for a network you trust."),
        INFO_SINCE(GUIDE_VERSION(0, 12), "settings.web_pin", "When a browser asks for the PIN, the console shows it at the bottom of the screen."),
        BACK_TO_MENU("\uE001 goes back to the menu."),
};

static const coach_step look_steps[] = {
        OPEN("menu.settings", "Open Settings.", "v:Settings"),
        INFO("settings.theme", "Three themes: Cards, Pixel and HOME. Left and Right change an option."),
        INFO("settings.accent", "The colour of buttons and selections."),
        INFO("settings.night", "Dark colours, by hand or from 20:00 to 8:00."),
        INFO("settings.language", "NSA speaks 11 languages. System follows the console's own."),
        REACH("settings.music", "Go down to Background music."),
        INFO("settings.music", "Music the console makes itself, with its own volume. The sounds above have theirs."),
        BACK_TO_MENU("\uE001 goes back to the menu."),
};

static const coach_step cleanup_steps[] = {
        INFO("menu.cleanup", "Tickets, half-finished installs and the games' extra data: free space or back it up."),
        INFO("menu.system", "The console at a glance: model, versions, network, storage and battery."),
        INFO_SINCE(GUIDE_VERSION(0, 11), "menu.system", "Inside, \uE000 on Console opens Date and time, and on Network your Wi-Fi connections."),
        INFO("menu.advanced", "The console's own memories. NSA asks before going in: a wrong change there can stop the console from starting."),
};

static const coach_step mods_steps[] = {
        INFO("menu.cleanup", "Game mods, in Cleanup: the mods in /luma/titles, and which games are no longer installed."),
        INFO("menu.cleanup", "Deleting a game that has mods asks whether to delete them too."),
};

static const coach_step select_steps[] = {
        OPEN("menu.sd", "Open SD card.", "tool.select"),
        INFO("tool.select", "This box (or ) ticks several files and folders at once:  or a tap on each one."),
        INFO("tool.select", "Then  copies, moves, installs or deletes all the ticked ones together, and  leaves."),
        DO(NULL, " goes up a folder, and from the top back to the menu.", KEY_B, false, "v:Main menu"),
};

static const coach_step player_steps[] = {
        INFO("menu.sd", "Music you play from the SD card goes on when you leave the player with ;  in the player stops it."),
        INFO("menu.sd", "While it plays, a play icon shows in the top bar, and  in any folder of the SD card has Back to the player."),
        INFO("menu.sd", "In the player, \uE003 or the button beside the volume repeats the whole list or the same song, and the bar jumps to any point at once."),
        INFO("menu.sd", "Choosing again the song that plays opens its player where it is, without starting it over."),
};

static const coach_step updates_steps[] = {
        OPEN("menu.settings", "Open Settings.", "v:Settings"),
        REACH("settings.channel", "Go down to Update channel."),
        INFO("settings.channel", "Stable has tested releases; Beta has them first."),
        INFO("settings.auto_update", "On: NSA looks for an update when it starts, and asks before installing it."),
        REACH("settings.guide", "Go down to Guide."),
        INFO("settings.guide", "Every chapter of this guide. When an update brings something new, NSA offers only that."),
        BACK_TO_MENU("\uE001 goes back to the menu."),
};

static const coach_step wifi_steps[] = {
        OPEN("menu.system", "Open System.", "v:System"),
        OPEN("list.selected", "Go down to Network and open it with \uE000.", "v:Wi-Fi connections"),
        INFO("list", "Your console's three connections, then the copies saved on the SD card: the same ones WifiManager keeps in /3ds/WifiManager."),
        INFO("list.selected", "\uE000 on a connection: save a copy (with an alias, if you like), edit it, delete it, or write a saved copy there."),
        INFO("list.selected", "\uE000 on a copy: write it to a connection, edit it, rename its alias or delete it."),
        INFO("list.selected", "Not connected, NSA connects with what you write at once (or with Connect). The network the console is on changes only after sleep mode."),
        INFO("hints", "\uE003 shows the passwords, to type one on another device."),
        DO("back", "\uE001 goes back to System, and again to the menu.", KEY_B, true, "v:Main menu"),
};

static const coach_step datetime_steps[] = {
        OPEN("menu.system", "Open System.", "v:System"),
        OPEN("list.selected", "Console has the focus: open it with \uE000.", "v:Date and time"),
        INFO("list.selected", "\uE000 here opens a calendar, then wheels for the hours, minutes and seconds: drag them or turn them with the D-Pad."),
        INFO("list", "Set the time from the Internet puts the clock right, now or each time NSA opens. The console's clock has no time zone: NSA adds yours and summer time."),
        INFO("list", "Clock and Date order: 24 or 12 hours, day or month first. By region, they follow your console's region."),
        DO("back", "\uE001 goes back to System, and again to the menu.", KEY_B, true, "v:Main menu"),
};

static const guide_chapter chapters[] = {
        {"basics", GUIDE_VERSION(0, 2), true, NULL, ICON_DEVICE_GAMEPAD_2, "Getting around", "Screens, buttons and touch", basics_steps, COUNT_OF(basics_steps)},
        {"menu", GUIDE_VERSION(0, 8), true, "basics", ICON_LAYOUT_GRID, "Your main menu", "Tiles and icons, in your order", menu_steps, COUNT_OF(menu_steps)},
        {"titles", GUIDE_VERSION(0, 2), true, "basics", ICON_APPS, "Titles", "Your games and apps", titles_steps, COUNT_OF(titles_steps)},
        {"files", GUIDE_VERSION(0, 2), true, "basics", ICON_DEVICE_SD_CARD, "SD card", "Files, pictures, music and CIAs", files_steps, COUNT_OF(files_steps)},
        {"remote", GUIDE_VERSION(0, 2), true, "basics", ICON_WORLD_DOWNLOAD, "Remote install", "Install from a QR code, a link or a computer", remote_steps, COUNT_OF(remote_steps)},
        {"saves", GUIDE_VERSION(0, 5), false, "titles", ICON_DEVICE_FLOPPY, "Saves and backups", "Back up, restore and share saves", saves_steps, COUNT_OF(saves_steps)},
        {"console", GUIDE_VERSION(0, 3), false, "basics", ICON_3DS, "Console to console", "Send anything to another 3DS", console_steps, COUNT_OF(console_steps)},
        {"store", GUIDE_VERSION(0, 8), false, "basics", ICON_PACKAGE, "Store", "Homebrew from Universal-DB", store_steps, COUNT_OF(store_steps), GUIDE_VERSION(0, 9)},
        {"computer", GUIDE_VERSION(0, 5), false, "basics", ICON_DEVICE_DESKTOP, "From your computer", "FTP and the web page", computer_steps, COUNT_OF(computer_steps), GUIDE_VERSION(0, 9)},
        {"look", GUIDE_VERSION(0, 4), false, "basics", ICON_PALETTE, "Look and sound", "Themes, colours, music and language", look_steps, COUNT_OF(look_steps)},
        {"cleanup", GUIDE_VERSION(0, 2), false, "basics", ICON_STACK_2, "Cleanup and System", "Tickets, extra data, System and Advanced", cleanup_steps, COUNT_OF(cleanup_steps)},
        {"updates", GUIDE_VERSION(0, 3), false, "basics", ICON_REFRESH, "Updates and this guide", "New versions, and what each brings", updates_steps, COUNT_OF(updates_steps)},
        {"select", GUIDE_VERSION(0, 9), false, "files", ICON_SQUARE_CHECK, "Selecting several", "Copy, move or delete many at once", select_steps, COUNT_OF(select_steps)},
        {"player", GUIDE_VERSION(0, 9), false, "files", ICON_MUSIC, "Music in the background", "Keep listening while you browse", player_steps, COUNT_OF(player_steps)},
        {"mods", GUIDE_VERSION(0, 9), false, "basics", ICON_FOLDER, "Game mods", "LayeredFS mods of games you deleted", mods_steps, COUNT_OF(mods_steps)},
        // 0.10 although it may ship in a 0.9.x: the guide tells minor versions apart only, and a console on 0.9 has been
        // offered everything marked 0.9 (a chapter marked so would never be offered to it, as happened with mods and 0.8).
        {"wifi", GUIDE_VERSION(0, 10), false, "basics", ICON_WIFI, "Wi-Fi connections", "Save and restore your Wi-Fi settings", wifi_steps, COUNT_OF(wifi_steps)},
        {"datetime", GUIDE_VERSION(0, 11), false, "basics", ICON_CALENDAR, "Date and time", "Its format, and the clock from the Internet", datetime_steps, COUNT_OF(datetime_steps)},
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

// The newest of a chapter's own steps (0: none newer than the chapter).
static u32 guide_steps_newest(u32 chapter) {
    u32 newest = 0;
    for(u32 s = 0; s < chapters[chapter].stepCount; s++) {
        if(chapters[chapter].steps[s].since > newest) {
            newest = chapters[chapter].steps[s].since;
        }
    }

    return newest;
}

static u32 guide_newest() {
    u32 newest = 0;
    for(u32 i = 0; i < CHAPTER_COUNT; i++) {
        if(chapters[i].since > newest) {
            newest = chapters[i].since;
        }

        if(chapters[i].updated > newest) {
            newest = chapters[i].updated;
        }

        if(guide_steps_newest(i) > newest) {
            newest = guide_steps_newest(i);
        }
    }

    return newest;
}

// New to someone who knew the guide up to `known`: a chapter that came after (unless seen already), one changed after,
// or one that has steps added after.
static bool guide_is_new(u32 chapter, u32 known) {
    return (chapters[chapter].since > known && !guide_seen(chapter)) || chapters[chapter].updated > known || guide_steps_newest(chapter) > known;
}

// Only its new steps are played to someone who saw the chapter: steps added after `known` were, it was not changed whole.
static bool guide_only_new(u32 chapter, u32 known) {
    return known > 0 && guide_seen(chapter) && chapters[chapter].since <= known && chapters[chapter].updated <= known && guide_steps_newest(chapter) > known;
}

// A step kept for someone who saw its chapter: a new one, or one that moves between screens (it waits for something).
static bool guide_step_kept(const coach_step* step, u32 known) {
    return step->since > known || step->keys != 0 || step->tap || step->until != NULL;
}

// How many steps a chapter plays.
static u32 guide_step_count(u32 chapter, u32 known) {
    if(!guide_only_new(chapter, known)) {
        return chapters[chapter].stepCount;
    }

    u32 count = 0;
    for(u32 s = 0; s < chapters[chapter].stepCount; s++) {
        if(guide_step_kept(&chapters[chapter].steps[s], known)) {
            count++;
        }
    }

    return count;
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

// ---- playing: the chapters' steps one after another, each chapter from the main menu ----

static void (*guide_done)() = NULL;

static void guide_on_step(u32 tag, bool last) {
    if(last && tag < CHAPTER_COUNT) {
        guide_mark_seen(tag);
        guide_save();
    }
}

static void guide_on_end(bool finished) {
    guide_save();

    void (*done)() = guide_done;
    guide_done = NULL;
    if(done != NULL) {
        done();
    }
}

// `known`: the version the user knew (the offer after an update), whose seen chapters play only what is new; 0 plays
// every step (Settings -> Guide), marking those of the newest version.
static void guide_play(const guide_queue* queue, u32 known, void (*onDone)()) {
    u32 newest = guide_newest();
    u32 total = 0;
    for(u32 i = 0; i < queue->count; i++) {
        total += chapters[queue->items[i]].stepCount;
    }

    coach_step* steps = total > 0 ? (coach_step*) calloc(total, sizeof(coach_step)) : NULL;
    if(steps == NULL) {
        if(onDone != NULL) {
            onDone();
        }

        return;
    }

    u32 n = 0;
    for(u32 i = 0; i < queue->count; i++) {
        u32 c = queue->items[i];
        bool onlyNew = guide_only_new(c, known);
        u32 first = n;
        for(u32 s = 0; s < chapters[c].stepCount; s++) {
            const coach_step* from = &chapters[c].steps[s];
            if(onlyNew && !guide_step_kept(from, known)) {
                continue;
            }

            coach_step* step = &steps[n++];
            *step = *from;
            step->title = _(chapters[c].name);
            step->home = n - 1 == first;
            step->tag = c;
            // New: added after what the user knew, or (playing everything) in the newest version, after its chapter.
            step->fresh = from->since > chapters[c].since && (known > 0 ? from->since > known : from->since >= newest);
        }
    }

    guide_done = onDone;
    coach_start(steps, n, guide_on_step, guide_on_end);
    free(steps);
}

// ---- the question at start ----

static struct {
    guide_queue queue;     // What "Start" / "Show me" plays.
    guide_queue all;       // What "Everything" plays.
    bool everything;       // The question has an "Everything" option (the second).
    u32 known;             // The version the user knew, for what plays.
    void (*next)();
    char text[512];
} guide_offer;

// The top screen while it asks: the chapters it would play, with how many steps each.
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

        char steps[32];
        snprintf(steps, sizeof(steps), _("%lu steps"), guide_step_count(guide_offer.queue.items[i], settings_get()->guideKnown));
        float stepsW = widget_text_width(steps, WIDGET_SMALL_SCALE);
        screen_draw_text(steps, right - stepsW, y + (rowH - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, t->palette->textMuted);
        widget_text_fit(_(chapter->name), x + 24, y + (rowH - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, t->palette->text, right - stepsW - 12 - (x + 24));
        y += rowH;
    }

    // What "Everything" adds.
    if(guide_offer.everything) {
        char more[160];
        snprintf(more, sizeof(more), _("\uE002 Everything: also the other %lu chapters you have not seen."), guide_offer.all.count - guide_offer.queue.count);
        screen_draw_text_wrap(more, x, cardY + cardH - 40, WIDGET_SMALL_SCALE, t->palette->textMuted, right);
    }
}

static void guide_offer_onresponse(ui_view* view, void* data, u32 response) {
    guide_offer.known = settings_get()->guideKnown;
    settings_get()->guideKnown = guide_newest();
    guide_save();

    // Option 0 plays the offer, the second (when there are three) everything not seen; the last one is "Later".
    if(response == 0) {
        guide_play(&guide_offer.queue, guide_offer.known, guide_offer.next);
    } else if(response == 1 && guide_offer.everything) {
        guide_play(&guide_offer.all, guide_offer.known, guide_offer.next);
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
    if(firstStart) {
        for(u32 i = 0; i < CHAPTER_COUNT; i++) {
            if(chapters[i].essential && !guide_seen(i)) {
                guide_queue_add(&guide_offer.queue, i);
            }
        }

        snprintf(guide_offer.text, sizeof(guide_offer.text), _("Welcome to NSA!\n\nA short guide shows how it works on its own screens: %lu chapters. START leaves it at any time, and Settings -> Guide keeps every chapter."),
                 guide_offer.queue.count);
    } else {
        size_t used = (size_t) snprintf(guide_offer.text, sizeof(guide_offer.text), "%s\n", _("New in this version:"));
        for(u32 i = 0; i < CHAPTER_COUNT; i++) {
            if(guide_is_new(i, settings->guideKnown)) {
                guide_queue_add(&guide_offer.queue, i);
                if(used < sizeof(guide_offer.text)) {
                    used += (size_t) snprintf(guide_offer.text + used, sizeof(guide_offer.text) - used, "\n- %s", _(chapters[i].name));
                }
            }
        }

        // What they build on, chained before them. How many others "Everything" adds is on the top screen.
        u32 added = 0;
        for(u32 i = 0; i < guide_offer.queue.count; i++) {
            if(!guide_is_new(guide_offer.queue.items[i], settings->guideKnown)) {
                added++;
            }
        }

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

    u32 ink = 0;
    widget_item_frame(x, y, width, height, selected, &ink, NULL);

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
        snprintf(text, sizeof(text), "%s", seen < total ? _("Shows every chapter you have not seen, in order, on NSA's own screens. START leaves it; what you saw is kept.")
                                                        : _("You have seen every chapter. Pick one below to see it again."));
        snprintf(line, sizeof(line), _("%lu of %lu chapters seen"), seen, total);
    } else if(row == ROW_RESET) {
        snprintf(title, sizeof(title), "%s", _("Start again"));
        snprintf(text, sizeof(text), "%s", _("Marks every chapter as not seen, as on the first day."));
    } else {
        const guide_chapter* chapter = &chapters[row];
        snprintf(title, sizeof(title), "%s", _(chapter->name));
        snprintf(text, sizeof(text), "%s.", _(chapter->summary));
        snprintf(line, sizeof(line), _("%lu steps, since NSA %lu.%lu"), chapter->stepCount, chapter->since >> 8, chapter->since & 0xFF);
    }

    widget_text_fit(title, x + 30, y + 1, WIDGET_TITLE_SCALE, t->palette->text, right - (x + 30));
    y += 32;
    screen_draw_text_wrap(text, x, y, WIDGET_TEXT_SCALE, t->palette->textMuted, right);

    if(line[0] != '\0') {
        screen_draw_text(line, x, cardY + cardH - 26, WIDGET_TEXT_SCALE, t->palette->text);
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
    view->info = _("\uE000 play   \uE001 back");

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
            // Seen it all: all of it again.
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

    // The tour goes back to the main menu first (it closes this list and Settings).
    guide_play(&queue, 0, NULL);
}

void guide_open() {
    ui_view* view = list_display(_("Guide"), _("\uE000 play   \uE001 back"), NULL, guide_list_update, guide_list_draw_top);
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
