#pragma once

typedef struct ticket_info_s ticket_info;
typedef struct title_info_s title_info;
typedef struct export_cia_data_s export_cia_data;
typedef struct linked_list_s linked_list;
typedef struct list_item_s list_item;
typedef struct ui_view_s ui_view;

#define INSTALL_URLS_MAX 128

void action_browse_boss_ext_save_data(linked_list* items, list_item* selected);
void action_browse_user_ext_save_data(linked_list* items, list_item* selected);
void action_delete_ext_save_data(linked_list* items, list_item* selected);

void action_browse_system_save_data(linked_list* items, list_item* selected);
void action_delete_system_save_data(linked_list* items, list_item* selected);

void action_install_cia(linked_list* items, list_item* selected);
void action_install_cia_delete(linked_list* items, list_item* selected);
void action_install_cia_delete_now(linked_list* items, list_item* selected);
void action_install_cias(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData);
void action_install_cias_delete(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData);
void action_install_ticket(linked_list* items, list_item* selected);
void action_install_ticket_delete(linked_list* items, list_item* selected);
void action_install_tickets(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData);
void action_install_tickets_delete(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData);
void action_delete_file(linked_list* items, list_item* selected);
void action_delete_dir(linked_list* items, list_item* selected);
void action_delete_dir_contents(linked_list* items, list_item* selected);
void action_delete_dir_cias(linked_list* items, list_item* selected);
void action_delete_dir_tickets(linked_list* items, list_item* selected);
void action_new_folder(linked_list* items, list_item* selected);
void action_paste_contents(linked_list* items, list_item* selected);
// Like it, but a cut item on the same medium (`archiveId`) is renamed into the folder instead of copied.
void action_paste_contents_in(linked_list* items, list_item* selected, FS_ArchiveID archiveId);
void action_rename(linked_list* items, list_item* selected);

void action_delete_pending_title(linked_list* items, list_item* selected);
void action_delete_all_pending_titles(linked_list* items, list_item* selected);

void action_delete_ticket(linked_list* items, list_item* selected);
void action_delete_tickets_unused(linked_list* items, list_item* selected);

void action_delete_title(linked_list* items, list_item* selected);
void action_delete_title_ticket(linked_list* items, list_item* selected);
void action_launch_title(linked_list* items, list_item* selected);
void action_extract_smdh(linked_list* items, list_item* selected);
void action_export_cia(linked_list* items, list_item* selected);
void action_export_cia_send(linked_list* items, list_item* selected);

// Sending a title without making a CIA first: its contents go over the network as
// they are read (a "title package"), and the receiver builds the CIA.
Result action_export_cia_stream_open(title_info* title, export_cia_data** out, u64* streamSize, char* name, size_t nameSize);
const u8* action_export_cia_stream_header(export_cia_data* data, u32* size);
u32 action_export_cia_stream_content_count(export_cia_data* data);
u64 action_export_cia_stream_content_size(export_cia_data* data, u32 index);
Result action_export_cia_stream_read(export_cia_data* data, u32 index, u64 offset, void* buffer, u32 size);
void action_export_cia_stream_free(export_cia_data* data);
Result action_export_cia_sink_open(const char* path, u64 size, bool resume, export_cia_data** out);
Result action_export_cia_sink_write_at(export_cia_data* data, u64 offset, const void* buffer, u32 size);
void action_export_cia_sink_progress(export_cia_data* data, u64* done, u64* total);
Result action_export_cia_sink_finish(export_cia_data* data);
Result action_export_cia_sink_close(export_cia_data* data, bool succeeded, bool keepPartial);
void action_import_seed(linked_list* items, list_item* selected);
void action_erase_twl_save(linked_list* items, list_item* selected);
void action_export_twl_save(linked_list* items, list_item* selected);
void action_import_twl_save(linked_list* items, list_item* selected);
void action_browse_title_save_data(linked_list* items, list_item* selected);

// Save data and extra data as plain folders on the SD card (savetransfer.c). `extId` is for SAVE_KIND_EXT.
typedef enum { SAVE_KIND_SAVE, SAVE_KIND_EXT } save_kind;
void action_export_save_files(title_info* title, const char* name, save_kind kind, u32 extId);
void action_import_save_files(title_info* title, const char* name, save_kind kind, u32 extId);
void action_import_secure_value(linked_list* items, list_item* selected);
void action_export_secure_value(linked_list* items, list_item* selected);
void action_delete_secure_value(linked_list* items, list_item* selected);

void action_install_url(const char* confirmMessage, const char* urls, const char* paths, void* userData,
                        void (*finishedURL)(void* data, u32 index),
                        void (*finishedAll)(void* data),
                        void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index));