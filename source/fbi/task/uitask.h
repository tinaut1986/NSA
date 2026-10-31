#pragma once

typedef struct ui_view_s ui_view;

#include "metainfo.h"

// "Game", "Update", "DLC"... from a title ID, and a version as "1.2.3 (4800)".
const char* task_kind_name(u64 titleId);
void task_version_text(char* out, size_t size, u16 version);

void task_draw_meta_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_ext_save_data_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_file_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_pending_title_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_system_save_data_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_ticket_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);
void task_draw_title_info(ui_view* view, void* data, float x1, float y1, float x2, float y2);

#include "listextsavedata.h"
#include "listpendingtitles.h"
#include "listsystemsavedata.h"
#include "listtickets.h"
#include "listtitles.h"
#include "listfiles.h"