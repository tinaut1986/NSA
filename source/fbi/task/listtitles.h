#pragma once

typedef struct linked_list_s linked_list;
typedef struct list_item_s list_item;

typedef struct title_info_s {
    FS_MediaType mediaType;
    u64 titleId;
    char productCode[0x10];
    u16 version;
    u64 installedSize;
    bool twl;
    bool hasMeta;
    meta_info meta;

    // Details are filled in after the title is listed (listtitles.c).
    bool loaded;
    u64 loadedTime;
    bool infoKnown; // version and installedSize already set

    // Grouping (titles.c): a game counts the updates and DLC that share its
    // unique ID; those are `linked` when their game is in the list.
    u32 linkedCount;
    bool linked;

    // Marked in Titles' picking mode, to be sent with others to another 3DS (issue #53).
    bool picked;
} title_info;

typedef enum {
    TITLE_KIND_GAME,
    TITLE_KIND_UPDATE,
    TITLE_KIND_DLC,
    TITLE_KIND_DEMO,
    TITLE_KIND_DSIWARE,
    TITLE_KIND_SYSTEM
} title_kind;

// From the upper half of a title ID; the lower half is shared by a game, its
// update and its DLC.
title_kind title_kind_of(u64 titleId);
// Part of the console's own software (system apps, applets, modules, data; the DSi system apps too): deleting one can
// stop the console from starting or break a feature.
bool title_is_system(u64 titleId);

// A CTR title's banner from its SMDH; false when it has none. The icon texture
// is allocated and belongs to the caller.
bool task_title_load_meta(FS_MediaType mediaType, u64 titleId, meta_info* meta);

typedef struct populate_titles_data_s {
    linked_list* items;

    void* userData;
    bool (*filter)(void* data, u64 titleId, FS_MediaType mediaType);
    int (*compare)(void* data, const void* p1, const void* p2);

    // Set before a populate to read every SMDH again instead of using the cache.
    bool refresh;

    volatile bool finished;
    Result result;
    Handle cancelEvent;
} populate_titles_data;

// What is in the game card slot.
typedef enum {
    TASK_CARD_NONE,
    TASK_CARD_UNKNOWN,   // Inserted, but the console does not say what yet.
    TASK_CARD_CTR,
    TASK_CARD_TWL
} task_card_state;

task_card_state task_titles_card_state(void);
// Takes the game card's titles out of `items` (they are freed), and puts in the ones of the card now in the slot
// (`filter` as in a populate: NULL lists them). Fails while the system has not read the card yet: try again a little later.
Result task_titles_sync_card(linked_list* items, void* userData, bool (*filter)(void* data, u64 titleId, FS_MediaType mediaType));

// One CTR title, loaded at once, as a list item (free it with task_free_title).
list_item* task_title_item(FS_MediaType mediaType, u64 titleId);
void task_free_title(list_item* item);
void task_clear_titles(linked_list* items);
Result task_populate_titles(populate_titles_data* data);