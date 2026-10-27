#pragma once

typedef struct linked_list_s linked_list;
typedef struct list_item_s list_item;

typedef struct ext_save_data_info_s {
    FS_MediaType mediaType;
    u64 extSaveDataId;
    bool shared;
    bool hasMeta;
    meta_info meta;

    // The list screen shows a row at once and fills in its name and icon
    // afterwards: `ready` once it has (`readyTime`, for the fade-in).
    bool ready;
    u64 readyTime;

    // The installed title with the same ID, if there is one (uitask.c finds it
    // when first shown): where it is installed and its banner.
    bool looked;
    bool hasTitle;
    FS_MediaType titleMedia;
    bool hasTitleMeta;
    meta_info titleMeta;
} ext_save_data_info;

typedef struct populate_ext_save_data_data_s {
    linked_list* items;

    void* userData;

    // List a placeholder for every one first and fill the details in after
    // (the list screen); without, each is complete when added, sorted by `compare`.
    bool placeholders;

    bool (*filter)(void* data, u64 extSaveDataId, FS_MediaType mediaType);
    int (*compare)(void* data, const void* p1, const void* p2);

    volatile bool finished;
    Result result;
    Handle cancelEvent;
} populate_ext_save_data_data;

void task_ext_save_data_load(list_item* item);
void task_free_ext_save_data(list_item* item);
void task_clear_ext_save_data(linked_list* items);
Result task_populate_ext_save_data(populate_ext_save_data_data* data);