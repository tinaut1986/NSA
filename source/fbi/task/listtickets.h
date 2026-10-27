#pragma once

#include "metainfo.h"

typedef struct linked_list_s linked_list;
typedef struct list_item_s list_item;

typedef struct ticket_info_s {
    bool loaded;

    u64 titleId;
    bool inUse;

    // Which installed title it belongs to, looked up when first shown
    // (uitask.c): where it is installed and its banner, if it has one.
    bool looked;
    FS_MediaType mediaType;
    bool hasMeta;
    meta_info meta;

    // The list screen shows a ticket's row at once and fills in its title
    // afterwards: `ready` once it has (`readyTime`, for the fade-in).
    bool ready;
    u64 readyTime;

    // Grouping (tickets.c): a game's ticket counts the update and DLC tickets
    // that share its unique ID; those are `linked` when the game's is listed.
    u32 linkedCount;
    bool linked;
} ticket_info;

typedef struct populate_tickets_data_s {
    linked_list* items;

    // Also find each ticket's title (name and icon): for the list screen.
    bool withTitles;

    volatile bool finished;
    Result result;
    Handle cancelEvent;
} populate_tickets_data;

// Which installed title the ticket belongs to: where it is installed and its
// banner. Done once per ticket (`looked`).
void task_ticket_lookup(ticket_info* info);
void task_populate_tickets_update_use(list_item* item);
void task_free_ticket(list_item* item);
void task_clear_tickets(linked_list* items);
Result task_populate_tickets(populate_tickets_data* data);