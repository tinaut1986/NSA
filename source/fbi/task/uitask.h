#pragma once

typedef struct ui_view_s ui_view;

#include "metainfo.h"

// "Game", "Update", "DLC"... from a title ID, and a version as "1.2.3 (4800)".
const char* task_kind_name(u64 titleId);
void task_version_text(char* out, size_t size, u16 version);

// The card of a game's mods folder (mods.c).
void task_draw_mod_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_meta_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_ext_save_data_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_file_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_pending_title_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_system_save_data_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_ticket_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_title_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);

// ---- loading a list on a thread (the list*.c tasks) ----

typedef struct linked_list_s linked_list;
typedef struct list_item_s list_item;

// Takes every item out of `items` and frees it with `freeItem`.
void task_clear_list(linked_list* items, void (*freeItem)(list_item* item));
// Starts `thread` on `data`, with a new cancel event in *cancelEvent, *result 0 and *finished false. If it cannot start,
// *finished is true and no event is left open.
Result task_start_populate(ThreadFunc thread, void* data, Handle* cancelEvent, volatile bool* finished, Result* result);
// Asks a running task to stop (its cancel event) and waits until it has.
void task_stop_populate(Handle cancelEvent, volatile bool* finished);

#include "listextsavedata.h"
#include "listmods.h"
#include "listpendingtitles.h"
#include "listsystemsavedata.h"
#include "listtickets.h"
#include "listtitles.h"
#include "listfiles.h"