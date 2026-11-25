#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "foldersize.h"
#include "settings.h"
#include "datetime.h"
#include "dlqueue.h"
#include "../core/core.h"

// The main menu: the three everyday entries as big tiles, then three groups
// (Cleanup and data, Advanced, Utilities) and System and Settings. A group opens a list
// of its entries; Advanced asks once per session before opening. System shows
// the console at a glance and opens its own groups: Information (the details),
// Console settings and Hardware tests.

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
    // An entry that is on or off, changed in place (Utilities): its value, drawn as Settings draws one, and the change.
    const char* (*value)();
    void (*toggle)();
};

static const mainmenu_entry cleanup[] = {
        {ICON_TICKET, "Tickets", "Installed tickets: see which are in use and delete the ones you no longer need.", "Licences of titles", tickets_open, SYSTEM_MEDIATYPE_CTR_NAND, -1},
        {ICON_CLOCK, "Pending titles", "Titles left half installed. Deleting them frees their space.", "Half-finished installs", pendingtitles_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
        {ICON_DATABASE, "Extra save data", "Extra save data of titles: browse, back up or delete it.", "Games' extra data", extsavedata_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
        {ICON_FOLDER, "Game mods", "Mods in /luma/titles (LayeredFS): see which games are no longer installed and delete their mods.", "LayeredFS in /luma/titles", mods_open, SYSTEM_MEDIATYPE_SD, -1},
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
        {ICON_3DS, "Console", "", "Model, versions and region", NULL, -1, -1, systeminfo_draw_console},
        {ICON_WIFI, "Network", "", "Wi-Fi, addresses and servers", NULL, -1, -1, systeminfo_draw_network},
        {ICON_DEVICE_SD_CARD, "Storage", "", "SD card and system memories", NULL, -1, -1, systeminfo_draw_storage},
        {ICON_BATTERY_CHARGING, "Battery", "", "Charge, voltage, temperature, rate", NULL, -1, -1, systeminfo_draw_battery},
};

// What System lets you change on the console.
static const mainmenu_entry console_settings[] = {
        {ICON_CALENDAR, "Date and time", "The console's clock: set it by hand or from the Internet, with your time zone and summer time.", "Clock, time zone, format", datetime_open, -1, -1},
        {ICON_WIFI, "Wi-Fi connections", "The console's three connections and the copies on the SD card: save, edit and restore them.", "Save and restore them", wificonnections_open, -1, -1},
        {ICON_SUN, "Screen brightness", "The console's five brightness levels between a lowest and a highest you choose, in every game, auto-brightness too.", "Lowest and highest, in every game", brightnessview_open, -1, -1},
};

// What Luma3DS's Rosalina menu or GodMode9 were opened for (utilities.c).
static const mainmenu_entry utilities[] = {
        {ICON_VOLUME, "DSP firmware", "Homebrew needs it to play sound. On: Nintendo's, whole; OpenDSP: the open source one. A or a tap: dump it or put OpenDSP.", NULL, NULL, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND, NULL, NULL, 0, false, utilities_dsp_value, utilities_dsp_toggle},
        {ICON_PUZZLE, "Game patching", "Luma3DS's game patching, for LayeredFS mods and IPS patches. A change takes effect when the console restarts.", NULL, NULL, SYSTEM_MEDIATYPE_SD, -1, NULL, NULL, 0, false, utilities_luma_value, utilities_luma_toggle},
        {ICON_DATABASE, "What takes the room", "Which folders and files take the most room on the SD card, biggest first, each with its share.", "Biggest first", foldersize_open_sd, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_DEVICE_GAMEPAD_2, "Button remapping", "Change what the buttons do in every game, or in one, with Rehid (a HID module Luma3DS loads). NSA installs it and edits its remappings.", "Rehid, in every game", rehidview_open, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_REFRESH, "Restart", "Restart the console. NSA closes first.", "Restart the console", utilities_restart_open, -1, -1},
        {ICON_POWER, "Power off", "Turn the console off. NSA closes first.", "Turn the console off", utilities_power_off_open, -1, -1},
};

// Checks of the console's hardware (hwtest.c).
static const mainmenu_entry hardware_tests[] = {
        {ICON_DEVICE_GAMEPAD_2, "Buttons and touch", "Every button, the Circle Pad, the C-Stick, the sliders and the touch screen, live as you use them.", "Every input, live", hwtest_buttons_open, -1, -1},
        {ICON_DEVICE_MOBILE, "Motion sensors", "The accelerometer and the gyroscope, live, and a recording of both to a CSV file on the SD card.", "Accelerometer and gyroscope", hwtest_motion_open, -1, -1},
};

#define COUNT_OF(a) (sizeof(a) / sizeof(*(a)))

// System's own list: three groups.
static const mainmenu_entry system_groups[] = {
        {ICON_INFO_CIRCLE, "Information", "", "Console, network, battery", NULL, -1, -1, systeminfo_draw_top, details, COUNT_OF(details), false},
        {ICON_SETTINGS, "Console settings", "What NSA can change on the console itself.", "Date and time, Wi-Fi, brightness", NULL, -1, -1, NULL, console_settings, COUNT_OF(console_settings), false},
        {ICON_FLASK, "Hardware tests", "Check that the buttons, the touch screen and the motion sensors work.", "Buttons, touch, motion", NULL, -1, -1, NULL, hardware_tests, COUNT_OF(hardware_tests), false},
};

static const mainmenu_entry entries[] = {
        {ICON_DEVICE_SD_CARD, "SD card", "Browse the SD card: install CIAs and tickets, copy, rename and delete files.", NULL, files_open_sd, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_APPS, "Titles", "Installed titles: launch them, delete them, back up their saves or extract their icons.", NULL, titles_open, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND},
        {ICON_WORLD_DOWNLOAD, "Remote install", "Install from the network: scan a QR code, type a URL or receive URLs from a computer.", NULL, remoteinstall_open, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_PACKAGE, "Store", "Homebrew from Universal-DB and the stores you add: browse them and install or download apps.", NULL, store_open, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_DOWNLOAD, "Download queue", "What the console downloads and installs in the background, one after the other: pause, reorder or cancel it.", NULL, dlqueue_open, SYSTEM_MEDIATYPE_SD, -1},
        {ICON_STACK_2, "Cleanup and data", "Licences, half-finished installs, the games' extra data and mods.", NULL, NULL, SYSTEM_MEDIATYPE_SD, SYSTEM_MEDIATYPE_CTR_NAND, NULL, cleanup, COUNT_OF(cleanup), false},
        {ICON_ALERT_TRIANGLE, "Advanced", "The console's own memories and system data. Handle with care.", NULL, NULL, SYSTEM_MEDIATYPE_CTR_NAND, SYSTEM_MEDIATYPE_TWL_NAND, NULL, advanced, COUNT_OF(advanced), true},
        {ICON_BOLT, "Utilities", "What Luma3DS's Rosalina menu and GodMode9 were for.", NULL, NULL, -1, -1, NULL, utilities, COUNT_OF(utilities), false},
        {ICON_3DS, "System", "The console at a glance: model, versions, network, battery and space.", NULL, NULL, -1, -1, systeminfo_draw_top, system_groups, COUNT_OF(system_groups), false},
        {ICON_SETTINGS, "Settings", "Theme, colours, language, updates and the FTP server.", NULL, settingsmenu_open, -1, -1},
};

#define ENTRY_COUNT COUNT_OF(entries)
#define TILE_COUNT 3          // The default layout: the first entries as big tiles, the rest as icons.
#define TIP_HEIGHT 26         // A tooltip by the small entry with the focus, with its name.
#define TIP_SLIDE 10          // The tooltip slides this far while it appears.
#define TIP_TIME 200
#define TIP_CONE 6           // The tooltip's cone, in rows of pixels.
#define TRAY_HEIGHT 30        // Editing: the hidden entries wait in a row at the bottom.
#define GAP 6
#define PAD 8

// The grid: six cells across, four down. A small entry takes one cell, a big one two by two. Editing, the cells get
// shorter to make room for the tray.
#define GRID_COLS 6
#define GRID_ROWS 4
#define GRID_GAP 5
#define GRID_PAD 6
#define SHAKE_TIME 360        // An entry that has no room to grow, or to come back, shakes this long.

// Stable names for the layout kept in settings.json, in the order of entries[].
static const char* const entry_ids[] = {"sd", "titles", "remote", "store", "queue", "cleanup", "advanced", "utilities", "system", "settings"};
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

    // The entry that had no room, shaking since `shakeTime`.
    int shakeEntry;
    u64 shakeTime;
} mainmenu_data;

/// ---- the layout: which entries show, in which order and where ----
//
// A permutation of entries[]: the first `layout_visible` positions are shown, the others are hidden. Editing shows the
// hidden ones in a tray at the bottom. In the grid (every theme but HOME's) each shown entry also has a cell, its top
// left one, and a size; the shown positions are kept in reading order (by row, then column), so the D-Pad's Left and
// Right and HOME's own order follow the grid.

static u8 layout_perm[ENTRY_COUNT];
static u32 layout_visible = ENTRY_COUNT;
static bool layout_editing = false;

// By entry, not by position.
static s8 grid_col[ENTRY_COUNT];
static s8 grid_row[ENTRY_COUNT];
static bool grid_big[ENTRY_COUNT];

#define POS(p) (&entries[layout_perm[p]])

// System and Settings always stay in the menu: from them the layout can always be edited again.
static bool mainmenu_pinned(u32 entry) {
    return strcmp(entry_ids[entry], "system") == 0 || strcmp(entry_ids[entry], "settings") == 0;
}

// The position of entry `e`.
static u32 mainmenu_pos_of(u8 e) {
    for(u32 p = 0; p < ENTRY_COUNT; p++) {
        if(layout_perm[p] == e) {
            return p;
        }
    }

    return 0;
}

static int grid_span(u8 e) {
    return grid_big[e] ? 2 : 1;
}

static bool grid_in_bounds(int col, int row, bool big) {
    int span = big ? 2 : 1;
    return col >= 0 && row >= 0 && col + span <= GRID_COLS && row + span <= GRID_ROWS;
}

static bool grid_free(u8 occ[GRID_ROWS][GRID_COLS], int col, int row, bool big) {
    if(!grid_in_bounds(col, row, big)) {
        return false;
    }

    int span = big ? 2 : 1;
    for(int r = 0; r < span; r++) {
        for(int c = 0; c < span; c++) {
            if(occ[row + r][col + c] != 0xFF) {
                return false;
            }
        }
    }

    return true;
}

static void grid_mark(u8 occ[GRID_ROWS][GRID_COLS], int col, int row, bool big, u8 e) {
    int span = big ? 2 : 1;
    for(int r = 0; r < span; r++) {
        for(int c = 0; c < span; c++) {
            occ[row + r][col + c] = e;
        }
    }
}

// The cells the shown entries take (0xFF: free).
static void grid_occupancy(u8 occ[GRID_ROWS][GRID_COLS]) {
    memset(occ, 0xFF, GRID_ROWS * GRID_COLS);
    for(u32 p = 0; p < layout_visible; p++) {
        u8 e = layout_perm[p];
        if(grid_col[e] >= 0) {
            grid_mark(occ, grid_col[e], grid_row[e], grid_big[e], e);
        }
    }
}

// The free place for a big (or small) entry closest to (col, row); false when there is none.
static bool grid_nearest_free(u8 occ[GRID_ROWS][GRID_COLS], int col, int row, bool big, int* outCol, int* outRow) {
    int best = -1;
    for(int r = 0; r < GRID_ROWS; r++) {
        for(int c = 0; c < GRID_COLS; c++) {
            if(!grid_free(occ, c, r, big)) {
                continue;
            }

            // Up or down counts a little more than sideways: entries rather slide along their row.
            int distance = (c - col) * (c - col) * 2 + (r - row) * (r - row) * 3;
            if(best < 0 || distance < best) {
                best = distance;
                *outCol = c;
                *outRow = r;
            }
        }
    }

    return best >= 0;
}

// The first free cell in reading order; false when the grid is full.
static bool grid_first_free(u8 occ[GRID_ROWS][GRID_COLS], int* outCol, int* outRow) {
    for(int r = 0; r < GRID_ROWS; r++) {
        for(int c = 0; c < GRID_COLS; c++) {
            if(occ[r][c] == 0xFF) {
                *outCol = c;
                *outRow = r;
                return true;
            }
        }
    }

    return false;
}

// Keeps the shown positions in reading order: by the row, then the column, of each entry's top left cell.
static void grid_sort() {
    for(u32 i = 1; i < layout_visible; i++) {
        u8 e = layout_perm[i];
        int key = grid_row[e] * GRID_COLS + grid_col[e];
        u32 j = i;
        while(j > 0 && grid_row[layout_perm[j - 1]] * GRID_COLS + grid_col[layout_perm[j - 1]] > key) {
            layout_perm[j] = layout_perm[j - 1];
            j--;
        }

        layout_perm[j] = e;
    }
}

// Puts the shown entry `e` at (col, row) in that size; the entries in the way move to the free places closest to theirs.
// False, and nothing changed, when they do not all fit.
static bool grid_put(u8 e, int col, int row, bool big) {
    if(!grid_in_bounds(col, row, big)) {
        return false;
    }

    s8 cols[ENTRY_COUNT];
    s8 rows[ENTRY_COUNT];
    memcpy(cols, grid_col, sizeof(cols));
    memcpy(rows, grid_row, sizeof(rows));

    u8 occ[GRID_ROWS][GRID_COLS];
    memset(occ, 0xFF, sizeof(occ));
    grid_mark(occ, col, row, big, e);
    cols[e] = (s8) col;
    rows[e] = (s8) row;

    // The others keep their place while it is still free (in reading order); the rest move, the big ones first: they
    // are the hardest to fit.
    u8 moved[ENTRY_COUNT];
    u32 movedCount = 0;
    for(u32 p = 0; p < layout_visible; p++) {
        u8 o = layout_perm[p];
        if(o == e) {
            continue;
        }

        if(grid_col[o] >= 0 && grid_free(occ, grid_col[o], grid_row[o], grid_big[o])) {
            grid_mark(occ, grid_col[o], grid_row[o], grid_big[o], o);
        } else {
            moved[movedCount++] = o;
        }
    }

    for(int pass = 0; pass < 2; pass++) {
        for(u32 i = 0; i < movedCount; i++) {
            u8 o = moved[i];
            if(grid_big[o] != (pass == 0)) {
                continue;
            }

            int c = 0, r = 0;
            int fromCol = grid_col[o] >= 0 ? grid_col[o] : 0;
            int fromRow = grid_row[o] >= 0 ? grid_row[o] : 0;
            if(!grid_nearest_free(occ, fromCol, fromRow, grid_big[o], &c, &r)) {
                return false;
            }

            grid_mark(occ, c, r, grid_big[o], o);
            cols[o] = (s8) c;
            rows[o] = (s8) r;
        }
    }

    memcpy(grid_col, cols, sizeof(cols));
    memcpy(grid_row, rows, sizeof(rows));
    grid_big[e] = big;
    grid_sort();
    return true;
}

// Shown entries without a place, or in one taken or out of the grid (a layout of an older NSA, an entry shown again in
// HOME's theme, a new entry), get the first free cell; with none left, the last big one is made small.
static void grid_validate() {
    u8 occ[GRID_ROWS][GRID_COLS];
    memset(occ, 0xFF, sizeof(occ));
    u8 lost[ENTRY_COUNT];
    u32 lostCount = 0;
    for(u32 p = 0; p < layout_visible; p++) {
        u8 e = layout_perm[p];
        if(grid_col[e] >= 0 && grid_free(occ, grid_col[e], grid_row[e], grid_big[e])) {
            grid_mark(occ, grid_col[e], grid_row[e], grid_big[e], e);
        } else {
            lost[lostCount++] = e;
        }
    }

    for(u32 i = 0; i < lostCount; i++) {
        u8 e = lost[i];
        int c = 0, r = 0;
        grid_big[e] = false;
        while(!grid_first_free(occ, &c, &r)) {
            int shrink = -1;
            for(u32 p = 0; p < layout_visible; p++) {
                u8 o = layout_perm[p];
                if(grid_big[o] && grid_col[o] >= 0 && occ[grid_row[o]][grid_col[o]] == o) {
                    shrink = o;
                }
            }

            if(shrink < 0) {
                break;
            }

            grid_big[shrink] = false;
            occ[grid_row[shrink]][grid_col[shrink] + 1] = 0xFF;
            occ[grid_row[shrink] + 1][grid_col[shrink]] = 0xFF;
            occ[grid_row[shrink] + 1][grid_col[shrink] + 1] = 0xFF;
        }

        grid_mark(occ, c, r, false, e);
        grid_col[e] = (s8) c;
        grid_row[e] = (s8) r;
    }

    grid_sort();
}

// The layout NSA comes with, and the one a list without places (an older NSA's) gets: the first three shown as big tiles
// across the top, the rest as icons in the two rows under them.
static void grid_flow() {
    u32 small = 0;
    for(u32 p = 0; p < ENTRY_COUNT; p++) {
        u8 e = layout_perm[p];
        if(p >= layout_visible) {
            grid_col[e] = grid_row[e] = -1;
            grid_big[e] = false;
        } else if(p < TILE_COUNT) {
            grid_big[e] = true;
            grid_col[e] = (s8) (p * 2);
            grid_row[e] = 0;
        } else {
            grid_big[e] = false;
            grid_col[e] = (s8) (small % GRID_COLS);
            grid_row[e] = (s8) (2 + small / GRID_COLS);
            small++;
        }
    }

    grid_validate();
}

static void mainmenu_layout_default() {
    for(u32 i = 0; i < ENTRY_COUNT; i++) {
        layout_perm[i] = (u8) i;
    }

    layout_visible = ENTRY_COUNT;
    grid_flow();
}

// settings.c keeps the layout as one line, "sd:00B,titles:20B,...|advanced": the shown entries in order, each with its column
// and row (and B when big), then the hidden ones. A list without places (NSA 0.16 and older) gets the default places.
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
    bool anyPlace = false;
    s8 cols[ENTRY_COUNT];
    s8 rows[ENTRY_COUNT];
    bool bigs[ENTRY_COUNT];
    memset(cols, -1, sizeof(cols));
    memset(rows, -1, sizeof(rows));
    memset(bigs, 0, sizeof(bigs));
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
        while(*c != '\0' && *c != ',' && *c != '|' && *c != ':') {
            if(len < sizeof(id) - 1) {
                id[len++] = *c;
            }

            c++;
        }

        id[len] = '\0';

        // ":CR", then B when big.
        int col = -1, row = -1;
        bool big = false;
        if(*c == ':') {
            c++;
            if(c[0] >= '0' && c[0] <= '9' && c[1] >= '0' && c[1] <= '9') {
                col = c[0] - '0';
                row = c[1] - '0';
                c += 2;
            }

            if(*c == 'B') {
                big = true;
                c++;
            }

            while(*c != '\0' && *c != ',' && *c != '|') {
                c++;
            }
        }

        for(u32 e = 0; e < ENTRY_COUNT; e++) {
            if(!used[e] && strcmp(id, entry_ids[e]) == 0) {
                used[e] = true;
                if(inHidden && !mainmenu_pinned(e)) {
                    hidden[hiddenCount++] = (u8) e;
                } else {
                    shown[shownCount++] = (u8) e;
                    if(col >= 0) {
                        cols[e] = (s8) col;
                        rows[e] = (s8) row;
                        bigs[e] = big;
                        anyPlace = true;
                    }
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

    if(!anyPlace) {
        grid_flow();
        return;
    }

    memcpy(grid_col, cols, sizeof(cols));
    memcpy(grid_row, rows, sizeof(rows));
    memcpy(grid_big, bigs, sizeof(bigs));
    grid_validate();
}

// The layout as that line (above).
static void mainmenu_layout_text(char* text, size_t size) {
    size_t len = 0;
    text[0] = '\0';
    for(u32 p = 0; p < ENTRY_COUNT && len + 16 < size; p++) {
        u8 e = layout_perm[p];
        if(p > 0) {
            text[len++] = p == layout_visible ? '|' : ',';
        } else if(layout_visible == 0) {
            text[len++] = '|';
        }

        len += snprintf(text + len, size - len, "%s", entry_ids[e]);
        if(p < layout_visible && grid_col[e] >= 0) {
            len += snprintf(text + len, size - len, ":%d%d%s", grid_col[e], grid_row[e], grid_big[e] ? "B" : "");
        }
    }

    text[len] = '\0';
}

// The whole layout, to try a change and take it back.
typedef struct {
    u8 perm[ENTRY_COUNT];
    u32 visible;
    s8 col[ENTRY_COUNT];
    s8 row[ENTRY_COUNT];
    bool big[ENTRY_COUNT];
} mainmenu_layout_copy;

static void mainmenu_layout_keep(mainmenu_layout_copy* copy) {
    memcpy(copy->perm, layout_perm, sizeof(copy->perm));
    copy->visible = layout_visible;
    memcpy(copy->col, grid_col, sizeof(copy->col));
    memcpy(copy->row, grid_row, sizeof(copy->row));
    memcpy(copy->big, grid_big, sizeof(copy->big));
}

static void mainmenu_layout_restore(const mainmenu_layout_copy* copy) {
    memcpy(layout_perm, copy->perm, sizeof(copy->perm));
    layout_visible = copy->visible;
    memcpy(grid_col, copy->col, sizeof(copy->col));
    memcpy(grid_row, copy->row, sizeof(copy->row));
    memcpy(grid_big, copy->big, sizeof(copy->big));
}

static void mainmenu_layout_save() {
    char text[sizeof(settings_get()->menu)];
    mainmenu_layout_text(text, sizeof(text));

    // The default layout is kept as nothing, so a later NSA's default applies.
    mainmenu_layout_copy mine;
    mainmenu_layout_keep(&mine);
    mainmenu_layout_default();
    char defaultText[sizeof(settings_get()->menu)];
    mainmenu_layout_text(defaultText, sizeof(defaultText));
    mainmenu_layout_restore(&mine);
    if(strcmp(text, defaultText) == 0) {
        text[0] = '\0';
    }

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

// The HOME theme: every entry a big tile, five to a row (two rows), its name under it.
#define HOME_COLUMNS 5
#define HOME_TILE 58

static bool mainmenu_home() {
    return theme()->def->menuLayout == THEME_MENU_TILES;
}

/// The bottom screen's content, under the title bar and over the hint bar.
#define CONTENT_HEIGHT (BOTTOM_SCREEN_HEIGHT - WIDGET_TITLE_BAR_HEIGHT - WIDGET_HINT_BAR_HEIGHT)

// Where the hidden entries are while editing, from the top of the content.
static float mainmenu_tray_top(float y1) {
    return y1 + CONTENT_HEIGHT - GRID_PAD - TRAY_HEIGHT;
}

// The grid's cells: their size and where the first one starts. Editing, they are shorter: the tray goes under them.
static void mainmenu_cell(float x1, float y1, float x2, float* gx, float* gy, float* cw, float* ch) {
    float bottom = layout_editing ? mainmenu_tray_top(y1) - GRID_GAP : y1 + CONTENT_HEIGHT - GRID_PAD;
    *gx = x1 + GRID_PAD;
    *gy = y1 + GRID_PAD;
    *cw = (x2 - x1 - 2 * GRID_PAD - (GRID_COLS - 1) * GRID_GAP) / GRID_COLS;
    *ch = (bottom - *gy - (GRID_ROWS - 1) * GRID_GAP) / GRID_ROWS;
}

// Position `p`'s place: its cells in the grid (or, in HOME, the flow of tiles); a hidden one, while editing, goes in the
// tray (or on in HOME's flow).
static void mainmenu_button_rect(u32 p, float x1, float y1, float x2, float* x, float* y, float* w, float* h) {
    if(mainmenu_home()) {
        float area = x2 - x1 - 2 * PAD;
        float label = widget_text_height(WIDGET_SMALL_SCALE);
        *w = (area - (HOME_COLUMNS - 1) * GAP) / HOME_COLUMNS;
        *h = HOME_TILE + 4 + 2 * label;
        *x = x1 + PAD + (p % HOME_COLUMNS) * (*w + GAP);
        *y = y1 + PAD + (p / HOME_COLUMNS) * (*h + 5);
        return;
    }

    if(p >= layout_visible) {
        float area = x2 - x1 - 2 * GRID_PAD;
        u32 slots = ENTRY_COUNT - 2;
        *w = (area - (slots - 1) * GRID_GAP) / slots;
        *h = TRAY_HEIGHT;
        *x = x1 + GRID_PAD + (p - layout_visible) * (*w + GRID_GAP);
        *y = mainmenu_tray_top(y1);
        return;
    }

    float gx, gy, cw, ch;
    mainmenu_cell(x1, y1, x2, &gx, &gy, &cw, &ch);
    u8 e = layout_perm[p];
    int span = grid_span(e);
    *x = gx + grid_col[e] * (cw + GRID_GAP);
    *y = gy + grid_row[e] * (ch + GRID_GAP);
    *w = span * cw + (span - 1) * GRID_GAP;
    *h = span * ch + (span - 1) * GRID_GAP;
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

// The rows position `p` spans in the grid; the tray is one more row under it.
static void mainmenu_rows_of(u32 p, int* top, int* bottom) {
    if(p >= layout_visible) {
        *top = *bottom = GRID_ROWS;
        return;
    }

    u8 e = layout_perm[p];
    *top = grid_row[e];
    *bottom = grid_row[e] + grid_span(e) - 1;
}

// The row Left and Right go along: the one the D-Pad came in on, for a big entry (two rows). It holds for `nav_entry`
// only: a touch or another entry starts again from the top row.
static int nav_row = -1;
static int nav_entry = -1;

// Left or Right along a row of the grid: the next entry on that row that way (a big one, then on past it on the same
// row); at the end of the row, the first one of the next row (the last of the one before). In the tray, along it.
static u32 mainmenu_move_along(u32 selected, bool right, u32 limit) {
    if(selected >= layout_visible) {
        if(right) {
            return selected + 1 < limit ? selected + 1 : 0;
        }

        return selected > layout_visible ? selected - 1 : (layout_visible > 0 ? layout_visible - 1 : selected);
    }

    int top, bottom;
    mainmenu_rows_of(selected, &top, &bottom);
    int line = nav_entry == (int) layout_perm[selected] && nav_row >= top && nav_row <= bottom ? nav_row : top;
    float cx = mainmenu_center(selected);

    for(int step = 0; step <= GRID_ROWS; step++) {
        // Past the last row, going right: the tray first, when editing shows it.
        if(right && step > 0 && line + step >= GRID_ROWS && limit > layout_visible) {
            return layout_visible;
        }

        int row = step == 0 ? line : right ? (line + step) % GRID_ROWS : (line - step + GRID_ROWS) % GRID_ROWS;
        u32 best = selected;
        float bestX = 0;
        for(u32 q = 0; q < layout_visible; q++) {
            int qTop, qBottom;
            mainmenu_rows_of(q, &qTop, &qBottom);
            if(q == selected || row < qTop || row > qBottom) {
                continue;
            }

            float qx = mainmenu_center(q);
            // On its own row, the closest that way; on the next one, the first from its start (or its end).
            if(step == 0 && (right ? qx <= cx : qx >= cx)) {
                continue;
            }

            bool better = best == selected || (right ? qx < bestX : qx > bestX);
            if(better) {
                best = q;
                bestX = qx;
            }
        }

        if(best != selected) {
            nav_entry = layout_perm[best];
            nav_row = row;
            return best;
        }

    }

    return selected;
}

// The D-Pad among the first `limit` positions: Left and Right go through them in reading order, Up and Down to the row
// above or under (the tray, editing, is the last), landing on the one closest across; past the edge they go round.
static u32 mainmenu_move(u32 selected, u32 key, u32 limit) {
    if(limit == 0) {
        return selected;
    }

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

    if(key == KEY_RIGHT || key == KEY_LEFT) {
        return mainmenu_move_along(selected, key == KEY_RIGHT, limit);
    }

    bool down = key == KEY_DOWN;
    int top, bottom;
    mainmenu_rows_of(selected, &top, &bottom);
    float cx = mainmenu_center(selected);
    for(int wrap = 0; wrap < 2; wrap++) {
        u32 best = selected;
        float bestScore = 1e9f;
        for(u32 q = 0; q < limit; q++) {
            if(q == selected) {
                continue;
            }

            int qTop, qBottom;
            mainmenu_rows_of(q, &qTop, &qBottom);
            int rows;
            if(wrap == 0) {
                rows = down ? qTop - bottom : top - qBottom;
                if(rows <= 0) {
                    continue;
                }
            } else {
                // Round: from the top row (going down) or the bottom one (going up).
                rows = down ? qTop + 1 : GRID_ROWS + 1 - qBottom;
            }

            float dx = mainmenu_center(q) - cx;
            float score = (float) rows * 1000 + (dx < 0 ? -dx : dx);
            if(score < bestScore) {
                bestScore = score;
                best = q;
            }
        }

        if(best != selected) {
            if(best < layout_visible) {
                int bTop, bBottom;
                mainmenu_rows_of(best, &bTop, &bBottom);
                nav_entry = layout_perm[best];
                nav_row = down ? bTop : bBottom;
            }

            return best;
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

// The download queue's entry carries how many downloads it still has (none: no badge).
static void mainmenu_draw_badge(const mainmenu_entry* entry, float right, float top) {
    if(entry->open != dlqueue_open) {
        return;
    }

    u32 pending = dlqueue_pending();
    if(pending > 0) {
        widget_count_badge(pending, right, top);
    }
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

static void mainmenu_open_entry(const mainmenu_entry* entry);

// What opens another screen or a group; System's details only show.
static bool mainmenu_entry_opens(const mainmenu_entry* entry) {
    return entry->open != NULL || entry->children != NULL;
}

static void mainmenu_group_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    const mainmenu_entry* entry = (const mainmenu_entry*) item->data;
    if(entry->value != NULL) {
        widget_row(x, y, width, height, entry->icon, _(entry->name), t->palette->text, entry->value(), selected);
        return;
    }

    u32 ink = 0;
    u32 muted = 0;
    widget_item_frame(x, y, width, height, selected, &ink, &muted);

    screen_draw_icon(entry->icon, x + 9, y + (height - 16) / 2, 16, selected ? t->onAccent : t->accent);
    float tx = x + 33;
    float right = x + width - 9;
    // What opens another screen says so; System's details only show.
    if(mainmenu_entry_opens(entry)) {
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
    // System's details only show: nothing to open. An entry that is on or off changes.
    const mainmenu_entry* current = selected != NULL ? (const mainmenu_entry*) selected->data : NULL;
    bool opens = current == NULL || mainmenu_entry_opens(current);
    view->info = current != NULL && current->toggle != NULL ? _(" change    back") : opens ? _(" open    back") : _(" back");

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
        utilities_refresh();
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

    if(current != NULL && current->toggle != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        current->toggle();
        return;
    }

    if(selected != NULL && mainmenu_entry_opens((const mainmenu_entry*) selected->data) && (selectedTouched || (hidKeysDown() & KEY_A))) {
        mainmenu_open_entry((const mainmenu_entry*) selected->data);
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

/// ---- the menu ----

// The layout as a drag started, and the last one the drag could have.
static mainmenu_layout_copy drag_base;
static mainmenu_layout_copy drag_good;

// An entry with no room shakes, with the error tone.
static void mainmenu_shake(mainmenu_data* menu, u8 e) {
    menu->shakeEntry = e;
    menu->shakeTime = osGetTime();
    sound_play(SOUND_ERROR);
}

// Shows the hidden entry `e` as an icon in the first free cell; false, and still hidden, when the grid is full.
static bool grid_show(u8 e) {
    mainmenu_layout_show(mainmenu_pos_of(e), layout_visible);
    grid_big[e] = false;
    grid_col[e] = grid_row[e] = -1;

    u8 occ[GRID_ROWS][GRID_COLS];
    grid_occupancy(occ);
    int c = 0, r = 0;
    if(!grid_first_free(occ, &c, &r)) {
        mainmenu_layout_hide(mainmenu_pos_of(e));
        return false;
    }

    grid_col[e] = (s8) c;
    grid_row[e] = (s8) r;
    grid_sort();
    return true;
}

// Big to small keeps the top left cell; small to big takes the two by two square around it that moves the fewest others.
// False when no square has room.
static bool grid_toggle(u8 e) {
    if(grid_big[e]) {
        return grid_put(e, grid_col[e], grid_row[e], false);
    }

    u8 occ[GRID_ROWS][GRID_COLS];
    grid_occupancy(occ);
    int anchors[4][2] = {{grid_col[e], grid_row[e]}, {grid_col[e] - 1, grid_row[e]}, {grid_col[e], grid_row[e] - 1}, {grid_col[e] - 1, grid_row[e] - 1}};
    int taken[4];
    for(int i = 0; i < 4; i++) {
        taken[i] = 99;
        if(grid_in_bounds(anchors[i][0], anchors[i][1], true)) {
            taken[i] = 0;
            for(int r = 0; r < 2; r++) {
                for(int c = 0; c < 2; c++) {
                    u8 o = occ[anchors[i][1] + r][anchors[i][0] + c];
                    taken[i] += o != 0xFF && o != e;
                }
            }
        }
    }

    for(int tries = 0; tries < 4; tries++) {
        int pick = -1;
        for(int i = 0; i < 4; i++) {
            if(taken[i] < 99 && (pick < 0 || taken[i] < taken[pick])) {
                pick = i;
            }
        }

        if(pick < 0) {
            break;
        }

        if(grid_put(e, anchors[pick][0], anchors[pick][1], true)) {
            return true;
        }

        taken[pick] = 99;
    }

    return false;
}

// While an entry is dragged, the layout follows the finger at once: the entry sits where it would land if let go, the
// others make way (they ease into their new places when drawn). Every step starts again from the layout the drag began
// with, so entries pushed aside come back when the finger moves on; where there is no room, the last good layout stays.
static void mainmenu_drag_apply(mainmenu_data* menu, float x1, float y1, float x2) {
    u32 cur = (u32) menu->dragFrom;
    bool fromShown = cur < layout_visible;

    if(mainmenu_home()) {
        bool intoHidden = false;
        u32 target = cur;
        float fx = menu->dragX - x1 - PAD;
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
        return;
    }

    u8 e = layout_perm[cur];
    mainmenu_layout_restore(&drag_base);
    bool wasShown = mainmenu_pos_of(e) < layout_visible;
    bool ok = true;
    if(menu->dragY >= mainmenu_tray_top(y1) - 4) {
        // Over the tray: hidden (System and Settings stay).
        if(wasShown && !mainmenu_pinned(e)) {
            mainmenu_layout_hide(mainmenu_pos_of(e));
        } else if(wasShown) {
            ok = false;
        }
    } else {
        // The cells it would take, centred on the finger (drawn a little above it).
        bool big = wasShown && grid_big[e];
        int span = big ? 2 : 1;
        float gx, gy, cw, ch;
        mainmenu_cell(x1, y1, x2, &gx, &gy, &cw, &ch);
        float w = span * cw + (span - 1) * GRID_GAP;
        float h = span * ch + (span - 1) * GRID_GAP;
        int col = (int) floorf((menu->dragX - w / 2 - gx) / (cw + GRID_GAP) + 0.5f);
        int row = (int) floorf((menu->dragY - 8 - h / 2 - gy) / (ch + GRID_GAP) + 0.5f);
        col = col < 0 ? 0 : col > GRID_COLS - span ? GRID_COLS - span : col;
        row = row < 0 ? 0 : row > GRID_ROWS - span ? GRID_ROWS - span : row;
        if(!wasShown) {
            mainmenu_layout_show(mainmenu_pos_of(e), layout_visible);
            grid_big[e] = false;
            grid_col[e] = grid_row[e] = -1;
        }

        ok = grid_put(e, col, row, big);
    }

    if(ok) {
        mainmenu_layout_keep(&drag_good);
    } else {
        mainmenu_layout_restore(&drag_good);
    }

    menu->dragFrom = (int) mainmenu_pos_of(e);
    menu->selected = (u32) menu->dragFrom;
}

static void mainmenu_edit_exit(mainmenu_data* menu) {
    layout_editing = false;
    menu->grabbed = false;
    menu->dragFrom = -1;
    if(menu->selected >= layout_visible) {
        menu->selected = 0;
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

// Editing, the grid's entry with the focus has a button at its top right corner: small to big, or back.
#define SIZE_BUTTON 20

static bool mainmenu_size_button(const mainmenu_data* menu, float x1, float y1, float x2, float* bx, float* by) {
    if(mainmenu_home() || !layout_editing || menu->selected >= layout_visible) {
        return false;
    }

    float x, y, w, h;
    mainmenu_button_rect(menu->selected, x1, y1, x2, &x, &y, &w, &h);
    *bx = x + w - SIZE_BUTTON + 4;
    *by = y - 4;
    if(*by < y1 + 1) {
        *by = y1 + 1;
    }

    if(*bx + SIZE_BUTTON > x2 - 1) {
        *bx = x2 - 1 - SIZE_BUTTON;
    }

    return true;
}

// Makes the selected entry big or small; one that has no room to grow shakes.
static void mainmenu_toggle_size(mainmenu_data* menu) {
    if(mainmenu_home() || menu->selected >= layout_visible) {
        return;
    }

    u8 e = layout_perm[menu->selected];
    if(grid_toggle(e)) {
        sound_play(SOUND_TICK);
    } else {
        mainmenu_shake(menu, e);
    }

    menu->selected = mainmenu_pos_of(e);
}

// Brings the hidden entry at position `p` back; in the grid, one that has no room shakes.
static void mainmenu_bring_back(mainmenu_data* menu, u32 p) {
    if(mainmenu_home()) {
        menu->selected = mainmenu_layout_show(p, layout_visible);
        return;
    }

    u8 e = layout_perm[p];
    if(!grid_show(e)) {
        mainmenu_shake(menu, e);
    }

    menu->selected = mainmenu_pos_of(e);
}

// Editing: A picks an entry up (and the D-Pad moves it) or brings a hidden one back, R makes it big or small, X hides,
// Y puts everything back, SELECT or B finish. By touch, drag an entry to a place or down to the hidden ones, tap the
// corner button of the one with the focus to change its size, and tap a hidden one to bring it back.
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
            if(menu->grabbed && mainmenu_home()) {
                u32 target = mainmenu_move(menu->selected, keys[i], layout_visible);
                if(target != menu->selected) {
                    mainmenu_layout_reorder(menu->selected, target);
                    menu->selected = target;
                }
            } else if(menu->grabbed) {
                // One cell that way; the others make room.
                u8 e = layout_perm[menu->selected];
                int dc = keys[i] == KEY_RIGHT ? 1 : keys[i] == KEY_LEFT ? -1 : 0;
                int dr = keys[i] == KEY_DOWN ? 1 : keys[i] == KEY_UP ? -1 : 0;
                if(grid_in_bounds(grid_col[e] + dc, grid_row[e] + dr, grid_big[e]) && !grid_put(e, grid_col[e] + dc, grid_row[e] + dr, grid_big[e])) {
                    mainmenu_shake(menu, e);
                }

                menu->selected = mainmenu_pos_of(e);
            } else {
                menu->selected = mainmenu_move(menu->selected, keys[i], ENTRY_COUNT);
            }

            break;
        }
    }

    if(down & KEY_A) {
        if(menu->selected >= layout_visible) {
            mainmenu_bring_back(menu, menu->selected);
        } else {
            menu->grabbed = !menu->grabbed;
        }
    }

    if(down & KEY_R) {
        mainmenu_toggle_size(menu);
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

        float sx, sy;
        if(mainmenu_size_button(menu, bx1, by1, bx2, &sx, &sy) && widget_touched(&pos, sx - 3, sy - 3, SIZE_BUTTON + 6, SIZE_BUTTON + 6)) {
            mainmenu_toggle_size(menu);
            return;
        }

        for(u32 p = 0; p < ENTRY_COUNT; p++) {
            if(mainmenu_hit(p, &pos, bx1, by1, bx2)) {
                menu->dragFrom = (int) p;
                menu->selected = p;
                menu->grabbed = false;
                menu->dragStartX = menu->dragX = pos.px;
                menu->dragStartY = menu->dragY = pos.py;
                mainmenu_layout_keep(&drag_base);
                mainmenu_layout_keep(&drag_good);
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
            // A tap: a hidden entry comes back.
            if(from >= layout_visible) {
                mainmenu_bring_back(menu, from);
            } else {
                menu->selected = from;
            }
        }

        // A drag was applied as it went.
    }
}


static void mainmenu_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    mainmenu_data* menu = (mainmenu_data*) data;

    // A layout changed in HOME's theme comes back whole into the grid.
    if(!mainmenu_home() && menu->dragFrom < 0) {
        u8 e = layout_perm[menu->selected < ENTRY_COUNT ? menu->selected : 0];
        grid_validate();
        menu->selected = mainmenu_pos_of(e);
    }

    // Set every frame: the language can change in Settings.
    view->name = _("Main menu");
    if(layout_editing) {
        view->info = mainmenu_home() ? _(" move    hide    reset   SELECT done") : _(" move   size   hide   reset  SELECT done");
    } else {
        view->info = mainmenu_can_open(POS(menu->selected)) ? _("\uE000 open   START exit") : _("START exit");
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
    const char* help = mainmenu_home() ? _("Drag an entry to move it, or pick it up with A and use the D-Pad. Drop it on the row at the bottom, or press X, to hide it; tap a hidden one to bring it back. The first three are big tiles, the rest icons. System and Settings always stay.")
                                       : _("Drag an entry to move it, or pick it up with A and use the D-Pad: the others make room. R, or the button on its corner, makes it big or small, if there is room. Drop it on the row at the bottom, or press X, to hide it; tap a hidden one to bring it back. System and Settings always stay.");
    screen_draw_text_wrap(help, textX, y, WIDGET_TEXT_SCALE, t->palette->textMuted, wrap);
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
        } else if(best == NULL && len < sizeof(line1)) {
            // No space (Japanese, Chinese): split in the middle, between two characters.
            size_t half = len / 2;
            while(half > 0 && ((u8) label[half] & 0xC0) == 0x80) {
                half--;
            }

            if(half > 0) {
                memcpy(line1, label, half);
                line1[half] = '\0';
                string_copy(line2, label + half, sizeof(line2));
            }
        }
    }

    u32 textColor = danger ? t->palette->danger : t->palette->text;
    float lineH = widget_text_height(WIDGET_SMALL_SCALE);
    mainmenu_tile_text(line1, x - 2, x + w + 2, y + HOME_TILE + 4, WIDGET_SMALL_SCALE, textColor);
    if(line2[0] != '\0') {
        mainmenu_tile_text(line2, x - 2, x + w + 2, y + HOME_TILE + 4 + lineH, WIDGET_SMALL_SCALE, textColor);
    }
}

/// A small entry: just its icon, filled when it has the focus (smaller while editing makes the cells short).
static void mainmenu_draw_small(const mainmenu_entry* entry, float x, float y, float w, float h, bool selected) {
    const theme_current* t = theme();
    bool danger = entry->warn;
    u32 onDanger = t->night ? t->palette->background : t->onAccent;
    u32 fill = selected ? (danger ? t->palette->danger : t->accent) : danger ? theme_mix(t->palette->surface, t->palette->danger, 0x26) : t->palette->surface;
    u32 edge = selected ? fill : danger ? t->palette->danger : t->palette->border;
    u32 ink = selected ? (danger ? onDanger : t->onAccent) : danger ? t->palette->danger : t->accent;
    screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, edge, fill);

    float iconSize = h >= 38 && w >= 38 ? 32 : 24;
    screen_draw_icon(entry->icon, x + (float) (int) ((w - iconSize) / 2), y + (float) (int) ((h - iconSize) / 2), iconSize, ink);
}

// The name of the small entry with the focus, in a bubble under it (over it near the bottom, and over the tray for a
// hidden one) that slides out and fades in.
static void mainmenu_draw_tip(const mainmenu_data* menu, float x1, float y1, float x2, float y2) {
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

    // Centred on the icon, kept inside the screen.
    float x = ix + (iw - width) / 2;
    if(x < x1 + PAD) {
        x = x1 + PAD;
    } else if(x + width > x2 - PAD) {
        x = x2 - PAD - width;
    }

    bool above = hidden || iy + ih + GAP + TIP_HEIGHT > y2 - 2;
    float p = widget_ease(menu->tipTime, TIP_TIME);
    float restY = above ? iy - GAP - TIP_HEIGHT : iy + ih + GAP;
    float y = (float) (int) (restY + (above ? 1 : -1) * (1 - p) * TIP_SLIDE);
    u32 alpha = (u32) (p * 255);

    u32 fill = danger ? t->palette->danger : t->accent;
    u32 ink = danger ? (t->night ? t->palette->background : t->onAccent) : t->onAccent;

    // The theme's corners and outline: a bubble with its card's radius, and the outline round the cone too.
    float radius = t->def->radius;
    int border = (int) t->def->border;
    u32 edge = THEME_ALPHA(t->palette->border, alpha);
    screen_frame_round_rect(x, y, width, TIP_HEIGHT, radius, (float) border, edge, THEME_ALPHA(fill, alpha));

    // A cone pointing at the icon, one row of pixels at a time: two wide at the tip, wider towards the bubble. With an
    // outline, the cone is drawn in its colour and filled inside, the fill going on through the bubble's outline so the
    // two are one shape.
    float cx = (float) (int) (ix + iw / 2);
    for(int pass = border > 0 ? 0 : 1; pass < 2; pass++) {
        int rows = pass == 0 ? TIP_CONE : TIP_CONE + border;
        for(int row = 0; row < rows; row++) {
            float half = (float) (row + 1) - (pass == 1 ? border * 1.4f : 0);
            if(half <= 0) {
                continue;
            }

            float rowY = above ? y + TIP_HEIGHT + TIP_CONE - 1 - row : y - TIP_CONE + row;
            screen_fill_rect(cx - half, rowY, 2 * half, 1, pass == 0 ? edge : THEME_ALPHA(fill, alpha));
        }
    }

    widget_text_fit(name, x + 12, y + (TIP_HEIGHT - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, THEME_ALPHA(ink, alpha), width - 24);
}

// Where each entry is drawn (by entry): it eases towards its place, so while one is dragged the others make way and an
// entry that changes size grows or shrinks on the way.
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

// The corner button of the entry with the focus while editing: arrows out to grow it, in to shrink it.
static void mainmenu_draw_size_button(const mainmenu_data* menu, float x1, float y1, float x2) {
    float bx, by;
    if(!mainmenu_size_button(menu, x1, y1, x2, &bx, &by)) {
        return;
    }

    const theme_current* t = theme();
    bool big = grid_big[layout_perm[menu->selected]];
    screen_frame_round_rect(bx, by, SIZE_BUTTON, SIZE_BUTTON, SIZE_BUTTON / 2, 2, t->palette->surface, t->accent);
    screen_draw_icon(big ? ICON_ARROWS_DIAGONAL_MINIMIZE_2 : ICON_ARROWS_DIAGONAL, bx + 2, by + 2, 16, t->onAccent);
    coach_anchor("menu.size", bx, by, SIZE_BUTTON, SIZE_BUTTON, false);
}

static void mainmenu_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    mainmenu_data* menu = (mainmenu_data*) data;
    const theme_current* t = theme();
    bool dragging = menu->dragFrom >= 0 && menu->dragMoved;
    bool moving = false;

    // A rectangle wider than a cell and a half is drawn as a tile, so a change of size shows as it grows.
    float gx, gy, cw, ch;
    mainmenu_cell(x1, y1, x2, &gx, &gy, &cw, &ch);
    float asTile = cw * 1.5f;

    float shake = 0;
    if(menu->shakeEntry >= 0) {
        u64 since = osGetTime() - menu->shakeTime;
        if(since < SHAKE_TIME) {
            shake = roundf(sinf((float) since * 0.07f) * 4 * (1 - (float) since / SHAKE_TIME));
            moving = true;
        } else {
            menu->shakeEntry = -1;
        }
    }

    // Editing, the tray for the hidden ones; empty, it says what it is for.
    if(layout_editing && !mainmenu_home()) {
        float trayY = mainmenu_tray_top(y1);
        screen_fill_round_rect(x1 + GRID_PAD - 2, trayY - 2, x2 - x1 - 2 * GRID_PAD + 4, TRAY_HEIGHT + 4, t->def->radiusSmall, THEME_ALPHA(t->palette->border, 0x50));
        if(layout_visible == ENTRY_COUNT) {
            widget_text_centered(_("Drop an entry here to hide it"), x1, x2, trayY + (TRAY_HEIGHT - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE, t->palette->textMuted);
        }

        coach_anchor("menu.tray", x1 + GRID_PAD, trayY, x2 - x1 - 2 * GRID_PAD, TRAY_HEIGHT, false);
    }

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

            if((int) layout_perm[p] == menu->shakeEntry) {
                x += shake;
            }

            float radius = t->def->radiusSmall;
            if(mainmenu_home()) {
                mainmenu_draw_home_tile(entry, x, y, w, selected, &menu->press, (int) p);
                radius = t->def->radius;
                mainmenu_draw_badge(entry, x + (w + HOME_TILE) / 2 - 2, y + 2);
            } else if(w > asTile && !hidden) {
                mainmenu_draw_tile(x, y, w, h, entry->icon, _(entry->name), selected, entry->warn);
                radius = t->def->radius;
                mainmenu_draw_badge(entry, x + w - 4, y + 4);
            } else {
                mainmenu_draw_small(entry, x, y, w, h, selected);
                mainmenu_draw_badge(entry, x + w - 2, y + 2);
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

            // Where a guided tour finds it (coach.h): "menu.titles"...
            if(!hidden && !isDragged) {
                char id[32];
                snprintf(id, sizeof(id), "menu.%s", entry_ids[layout_perm[p]]);
                coach_anchor(id, slot[0], slot[1], slot[2], slot[3], selected);
            }
        }
    }

    // The grid as a whole.
    if(!mainmenu_home()) {
        coach_anchor("menu.grid", gx, gy, GRID_COLS * cw + (GRID_COLS - 1) * GRID_GAP, GRID_ROWS * ch + (GRID_ROWS - 1) * GRID_GAP, false);
    }

    if(layout_editing) {
        coach_anchor("menu.editing", x1, y1, x2 - x1, y2 - y1, false);
        if(!dragging) {
            mainmenu_draw_size_button(menu, x1, y1, x2);
        }
    }

    // The name of the focused icon (a big tile shows its own).
    bool smallSelected = menu->selected >= layout_visible || !grid_big[layout_perm[menu->selected]];
    if(!mainmenu_home() && !dragging && smallSelected && mainmenu_limit() > 0) {
        mainmenu_draw_tip(menu, x1, y1, x2, y2);
    }

    // Keep drawing while things are still gliding into place.
    if(moving) {
        ui_wake();
    }
}


void mainmenu_open() {
    // Anything a data operation changes makes the folder sizes counted out of date.
    task_data_op_set_end_hook(foldersize_forget_all);

    mainmenu_data* data = (mainmenu_data*) calloc(1, sizeof(mainmenu_data));
    if(data == NULL) {
        error_panic("Failed to allocate main menu data.");
        return;
    }

    data->press.index = -1;
    data->dragFrom = -1;
    data->shakeEntry = -1;
    data->lastSelected = data->selected;
    layout_editing = false;
    mainmenu_layout_load();

    ui_view* view = ui_create();
    view->name = _("Main menu");
    view->info = _("\uE000 open   START exit");
    view->data = data;
    view->update = mainmenu_update;
    view->drawTop = mainmenu_draw_top;
    view->drawBottom = mainmenu_draw_bottom;
    ui_push(view);
}
