#pragma once

#include "metainfo.h"

typedef struct linked_list_s linked_list;
typedef struct list_item_s list_item;

// Where Luma3DS looks for a game's mods (LayeredFS, code patches, locale): one folder per title ID.
#define MODS_ROOT "/luma/titles/"

// Whether the game a mods folder is for is still on the console.
typedef enum {
    MOD_STATE_INSTALLED,       // Installed on the SD card or the NAND.
    MOD_STATE_CARD,            // The game card in the slot.
    MOD_STATE_RELATED,         // Not installed, but its update or DLC is: most likely played from a game card.
    MOD_STATE_NOT_INSTALLED
} mod_state;

// What a mods folder holds (bits).
#define MOD_CONTENT_ROMFS 1      // romfs/: files that replace the game's own (LayeredFS).
#define MOD_CONTENT_CODE 2       // code.ips, code.bps or code.bin: a patch of the game's code.
#define MOD_CONTENT_EXHEADER 4   // exheader.bin.
#define MOD_CONTENT_LOCALE 8     // locale.txt: the region and language Luma gives the game.
#define MOD_CONTENT_OTHER 16

typedef struct mod_info_s {
    u64 titleId;
    char folder[17];   // The folder's name: the title ID in 16 hex digits.

    // The row shows at once; the rest is found afterwards, in place: `ready` once it has (`readyTime`, for the fade-in).
    bool ready;
    u64 readyTime;

    mod_state state;
    FS_MediaType mediaType;   // Where the game (or its update) is, unless not installed.
    bool hasMeta;
    meta_info meta;

    u64 size;
    u32 files;
    u32 contents;

    // Not a folder: the screen's own row that stands for the empty ones while they are hidden (mods.c).
    bool summary;
} mod_info;

typedef struct populate_mods_data_s {
    linked_list* items;

    volatile bool finished;
    Result result;
    Handle cancelEvent;
} populate_mods_data;

// Whether the mods of this title can be deleted along with others: games, demos and the like whose game is gone, never a
// system title's (Luma patches some of those, and a region's own may not be installed).
bool task_mod_unused(const mod_info* info);
// "romfs, code patch" from the contents bits.
void task_mod_contents_text(char* out, size_t size, u32 contents);
// An empty folder holds no mods: Luma3DS finds nothing in it. Something other than Luma makes them.
#define MOD_EMPTY(info) ((info)->ready && !(info)->summary && (info)->files == 0)

// Whether the SD card has mods for this title (MODS_ROOT<ID>/ with something in it).
bool task_mods_exist(u64 titleId);
// Deletes the mods folder of this title.
Result task_mods_delete(u64 titleId);

void task_free_mod(list_item* item);
void task_clear_mods(linked_list* items);
Result task_populate_mods(populate_mods_data* data);
