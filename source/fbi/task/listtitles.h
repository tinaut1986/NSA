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

    // Grouping (titles.c): a game counts the updates and DLC that share its
    // unique ID; those are `linked` when their game is in the list.
    u32 linkedCount;
    bool linked;
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

typedef struct populate_titles_data_s {
    linked_list* items;

    void* userData;
    bool (*filter)(void* data, u64 titleId, FS_MediaType mediaType);
    int (*compare)(void* data, const void* p1, const void* p2);

    volatile bool finished;
    Result result;
    Handle cancelEvent;
} populate_titles_data;

void task_free_title(list_item* item);
void task_clear_titles(linked_list* items);
Result task_populate_titles(populate_titles_data* data);