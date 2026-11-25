#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <3ds.h>

// Rehid (hax0kartik/rehid): a HID module that Luma3DS loads instead of the console's own (as an external module), and
// that remaps buttons in every game from a JSON file on the SD card. NSA does not carry it (its repository has no
// licence): it downloads its release from GitHub, and writes the files Rehid reads. What Rehid accepts, as its
// Remapper.cpp reads them (v5.0):
//
//   /rehid/rehid.json                    every title that has no file of its own (HOME Menu too)
//   /rehid/<title ID, 16 upper hex>/rehid.json   that title only: the global file is not read at all then
//
//   "keys":  [{"get":"L+R", "press":"SELECT"}]      pressing `press` gives `get` (combos with '+')
//   "turbo": [{"get":"A", "press":"ZL", "framedelay":5}]   `press` turns a turbo on `get` on or off
//   "touch": [{"get":[x,y], "press":"ZR"}]          `press` touches the bottom screen at x,y
//   "touchtokeys": [{"get":"A", "press":[x,y,h,w]}] touching that box presses `get`
//   "cpad":  [{"get":[x,y], "press":"L"}]           `press` holds the Circle Pad at x,y
//   "cpadtodpad": true, "dpadtocpad": true, "overridecpadpro": true, "homebutton":"L+R+START"
//
// Rehid reads the file each time a title opens HID (a game starting): a change applies the next time the game starts.
// Its limits, which NSA keeps because Rehid itself does not check them:
// - at most 10 entries in each list: an 11th writes past Rehid's arrays (every game can lose its buttons);
// - "keys", "touch", "touchtokeys" and "cpad" entries with exactly two fields (others are skipped);
// - a title's file that is not JSON stops that title with an error screen (the global one is just ignored).

#define REHID_TITLE_ID 0x0004013000001D02ULL
#define REHID_MODULE_PATH "sdmc:/luma/sysmodules/0004013000001D02.cxi"
// Where rehidhelper puts the module and its IR patch while Rehid is off (the same names, so either app can turn it back).
#define REHID_MODULE_OFF_PATH "sdmc:/luma/sysmodules/rehid.cxi"
#define REHID_IR_PATH "sdmc:/luma/sysmodules/0004013000003302.ips"
#define REHID_IR_OFF_PATH "sdmc:/luma/sysmodules/rehid_ir.ips"
#define REHID_DIR "sdmc:/rehid"
#define REHID_GLOBAL_PATH "sdmc:/rehid/rehid.json"

#define REHID_ENTRIES_MAX 10

// NSA's own title (the CIA): its file is kept as "{}" so that NSA always has the console's buttons, whatever the global
// file does, and can always take a remapping back.
#define REHID_NSA_TITLE_ID 0x000400000FB1A000ULL

// ---- the module ----

typedef enum {
    REHID_NOT_INSTALLED,     // No module in /luma/sysmodules.
    REHID_OFF,               // Turned off (rehid.cxi): not loaded at the next start.
    REHID_NEEDS_LUMA,        // On the SD card, but Luma3DS does not load external modules.
    REHID_NEEDS_RESTART,     // On the SD card and Luma3DS would load it: the console has not started with it yet.
    REHID_RUNNING            // Loaded: HID answers Rehid's own command.
} rehid_state;

// Luma3DS's "Enable loading external FIRMs and modules" (/luma/config.ini): 1 on, 0 off, -1 not found.
int rehid_luma_modules();
bool rehid_set_luma_modules(bool on);

// Asks HID whether it is Rehid (a command only Rehid answers), and looks at the SD card.
rehid_state rehid_get_state();
// Off: moves the module and its IR patch aside (rehidhelper's names); on: puts them back. Takes effect at the next start.
bool rehid_set_enabled(bool on);

// Downloads Rehid's latest release from GitHub into /luma/sysmodules, checked against the SHA-256 GitHub gives; the
// module that was there before goes to sdmc:/nsa/rehid/ (rehid_restore_previous). `version` gets the release's tag.
// Blocking (a few seconds): run it on a thread. `why` gets a reason worth telling (a literal), or NULL.
Result rehid_install_latest(char* version, size_t versionSize, const char** why);
bool rehid_has_previous();
bool rehid_restore_previous();
// The tag of the release last installed by NSA ("v5.0"), or "" when not known.
void rehid_installed_version(char* out, size_t size);

// ---- remappings ----

typedef struct {
    u32 press;   // KEY_* bits the player presses (several: all of them together).
    u32 get;     // KEY_* bits the game sees instead.
} rehid_key;

typedef struct {
    u32 press;   // Turns the turbo on or off.
    u32 get;     // The keys the turbo presses and lets go.
    u8 frames;   // Frames between two changes ("framedelay").
} rehid_turbo;

typedef struct {
    u32 press;
    s16 x;
    s16 y;
} rehid_point;   // "touch" (a point of the bottom screen) and "cpad" (a Circle Pad position).

typedef struct {
    u32 get;
    u16 x;
    u16 y;
    u16 w;
    u16 h;
} rehid_box;     // "touchtokeys": a box of the bottom screen.

typedef struct rehid_config_s {
    rehid_key keys[REHID_ENTRIES_MAX];
    u32 keyCount;
    rehid_turbo turbo[REHID_ENTRIES_MAX];
    u32 turboCount;
    rehid_point touch[REHID_ENTRIES_MAX];
    u32 touchCount;
    rehid_box boxes[REHID_ENTRIES_MAX];
    u32 boxCount;
    rehid_point cpad[REHID_ENTRIES_MAX];
    u32 cpadCount;
    bool cpadToDpad;
    bool dpadToCpad;
    bool overrideCpadPro;
    u32 home;                // Keys that act as HOME, 0 for none.
    // What NSA does not edit, written back as it was (a JSON object, or NULL).
    void* extra;
    // Found when reading: entries past the 10th or that Rehid would not read (dropped when saved).
    u32 dropped;
} rehid_config;

// Rehid's names of the keys it knows ("A", "ZL", "CUP"...).
#define REHID_KEYS_ALL (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_SELECT | KEY_START | KEY_DRIGHT | KEY_DLEFT | KEY_DUP | KEY_DDOWN | KEY_R | KEY_L \
                        | KEY_ZL | KEY_ZR | KEY_CPAD_RIGHT | KEY_CPAD_LEFT | KEY_CPAD_UP | KEY_CPAD_DOWN \
                        | KEY_CSTICK_RIGHT | KEY_CSTICK_LEFT | KEY_CSTICK_UP | KEY_CSTICK_DOWN)
// "A+B", in Rehid's names; "" for 0.
void rehid_keys_name(u32 keys, char* out, size_t size);
// The same for people: "A + B", the D-Pad and the sticks spelled out (translated).
void rehid_keys_label(u32 keys, char* out, size_t size);
// Rehid's names back to bits (unknown names are skipped, as Rehid does).
u32 rehid_keys_parse(const char* text);

void rehid_config_init(rehid_config* config);
void rehid_config_free(rehid_config* config);
bool rehid_config_empty(const rehid_config* config);

// 0 for the global file. The path of a title's file.
void rehid_config_path(u64 titleId, char* out, size_t size);
bool rehid_config_exists(u64 titleId);
// Reads the file (none: an empty remapping). `why` gets a reason when it is not one NSA can edit.
Result rehid_config_load(u64 titleId, rehid_config* config, const char** why);
// Writes it whole: into a file of its own first, read back, then put in place.
Result rehid_config_save(u64 titleId, const rehid_config* config);
bool rehid_config_delete(u64 titleId);
// The JSON Rehid gets for `config` (malloc'd), for the save and the tests.
char* rehid_config_dump(const rehid_config* config);
// Reads JSON text as rehid_config_load does.
Result rehid_config_parse(const char* text, rehid_config* config, const char** why);

// Writes "{}" for NSA's own title when it has no file yet (Rehid gives NSA the console's buttons then).
void rehid_protect_nsa();

// The title IDs that have a file in /rehid (up to `max`); how many.
u32 rehid_list_titles(u64* out, u32 max);
