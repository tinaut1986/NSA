#pragma once

typedef struct ticket_info_s ticket_info;
typedef struct title_info_s title_info;
typedef struct export_cia_data_s export_cia_data;
typedef struct linked_list_s linked_list;
typedef struct list_item_s list_item;
typedef struct ui_view_s ui_view;

#define INSTALL_URLS_MAX 128

// Deleting one item, asked in red: "Deleting..." while `run` deletes it, then it leaves `items` (NULL: `selected` is a
// copy made for the action, freed in any case) and a notice says so (deleteone.c). The texts are English (translated).
typedef struct {
    const char* question;
    const char* progress;
    const char* failure;
    const char* success;
    const char* allocError;
    void (*drawInfo)(ui_view* view, void* info, float x1, float y1, float x2, float y2);   // The item's info (item->data).
    Result (*run)(list_item* item);
    void (*freeItem)(list_item* item);
} action_delete_one_def;

void action_delete_one(const action_delete_one_def* def, linked_list* items, list_item* selected);

void action_browse_boss_ext_save_data(linked_list* items, list_item* selected);
void action_browse_user_ext_save_data(linked_list* items, list_item* selected);
void action_delete_ext_save_data(linked_list* items, list_item* selected);

void action_browse_system_save_data(linked_list* items, list_item* selected);
void action_delete_system_save_data(linked_list* items, list_item* selected);

void action_install_cia(linked_list* items, list_item* selected);
void action_install_cia_delete(linked_list* items, list_item* selected);
void action_install_cia_delete_now(linked_list* items, list_item* selected);
// Installs CIAs on the SD card one after another with no question (Remote Install's "Save and install"):
// `paths` one a line; `delete` deletes each once it is installed.
void action_install_cia_paths_now(const char* paths, bool delete);
// Starts installing a CIA of `titleId` / `version` on `dest`: over the installed title when this is the same version or
// a higher one (nothing deleted, the save kept), else deleting the installed one first.
Result action_install_cia_start(FS_MediaType dest, u64 titleId, u16 version, Handle* handle);
// installsave.c: a lower version than the installed one. Asks, backs up the save (an error, or R_APP_SKIPPED when the
// user says no: nothing deleted then); `ended` once the install is over puts the save back, or says where it is kept.
Result action_install_cia_lower(FS_MediaType dest, u64 titleId, u16 version, u16 installedVersion);
void action_install_cia_ended(u64 titleId, bool installed);
// Copies the title's save to sdmc:/nsa/saves/<ID>/<date>-<why>/ (a backup of NSA's, listed with the others) and puts that
// folder in `dir`; `dir` is left empty when the title has no save (never started), which is no error.
Result action_save_backup(u64 titleId, FS_MediaType media, u16 version, const char* why, char* dir, size_t size);
void action_install_cias(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData);
void action_install_cias_delete(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData);
// The CIAs of the folder `selected` that `filter` keeps (the Files screen's selection).
void action_install_cias_selected(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData, bool delete);
void action_install_ticket(linked_list* items, list_item* selected);
void action_install_ticket_delete(linked_list* items, list_item* selected);
void action_install_tickets(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData);
void action_install_tickets_delete(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData);
void action_delete_file(linked_list* items, list_item* selected);
void action_delete_dir(linked_list* items, list_item* selected);
void action_delete_dir_contents(linked_list* items, list_item* selected);
void action_delete_dir_cias(linked_list* items, list_item* selected);
void action_delete_dir_tickets(linked_list* items, list_item* selected);
// The items of the folder `selected` that `filter` keeps (the Files screen's selection), folders with all they hold.
void action_delete_selected(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData);
void action_new_folder(linked_list* items, list_item* selected);
// Everything in the .zip or .7z at `path` (on the SD card) into its folder, or into a new folder named after it;
// `done` once it ends (to list the folder again), before the message.
void action_extract_archive(const char* path, bool intoFolder, void (*done)(void* userData), void* userData);
// What of the archive at `path` matches `pattern` (unarchive.h) into `output` ("sdmc:/..."; `shown` is how the message
// names it), with no question first, refusing the whole archive if anything would go where the console starts from or
// NSA's data (see extractarchive.c); the archive is deleted afterwards if `deleteAfter` and it all came out. With
// `sdLayoutTop` (not NULL), only the entries in the SD card's own folders (action_extract_sd_place) under that top folder
// of the archive ("" for its root) come out, the loose files beside them (a README) staying in it.
void action_extract_archive_to(const char* path, const char* pattern, const char* output, const char* shown, bool deleteAfter,
                               const char* sdLayoutTop, void (*done)(void* userData), void* userData);
// Where homebrew archives put their files on the SD card's root: "3ds/...", "cias/...", "luma/plugins/...", and a game's
// mods in "luma/titles/<ID>/..." (a path inside the archive, with no leading slash).
bool action_extract_sd_place(const char* path);
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
void action_extract_exefs(linked_list* items, list_item* selected);
// The same without the UI: how many files it copied in *extracted.
Result action_extract_exefs_to_sd(u64 titleId, FS_MediaType mediaType, u32* extracted);
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

// Save data and extra data as plain folders on the SD card (savetransfer.c). `extId` is for SAVE_KIND_EXT and SAVE_KIND_BOSS (SpotPass).
typedef enum { SAVE_KIND_SAVE, SAVE_KIND_EXT, SAVE_KIND_BOSS } save_kind;
void action_export_save_files(title_info* title, const char* name, save_kind kind, u32 extId);
void action_import_save_files(title_info* title, const char* name, save_kind kind, u32 extId);
// Sent as one file (a package, see savetransfer.c) over the console to console transfer.
void action_send_save_files(title_info* title, const char* name, save_kind kind, u32 extId);
// The same with one of the title's backups (as listed by action_save_backups) instead of the data on the console.
void action_send_save_backup(title_info* title, const char* name, save_kind kind, const char* path);
bool action_find_ext_data(title_info* info, u32* extId);
// DEBUG (SITE EXTIDS): every game on the SD card, the ext data ID its exheader names against the guesses.
bool action_ext_data_debug(const char* path);
typedef struct {
    char path[512];      // The backup's folder, with the trailing slash.
    char label[96];      // "[NSA] 2026-10-31 14:25:30", "[Checkpoint] name", "[JKSM] name", "[PKSM] date".
    bool checkpoint;     // The tree right in the folder, no manifest (Checkpoint's layout, and JKSM's: both set for JKSM's).
    bool pksm;           // PKSM's: <sd>/3ds/PKSM/backups/0x<unique ID>/<date>/main (a single file; checkpoint is set too).
    bool jksm;           // JKSM's: <sd>/JKSM/<kind>/<game>/<backup>/.
    u8 nameAt;           // Where the name starts in label: after "[NSA] ", "[Checkpoint] ", "[JKSM] " or "[PKSM] " (lists show a badge instead).
} save_backup;
// The backups of a title's data, NSA's, Checkpoint's, JKSM's then PKSM's (free the array); the count is the return value.
u32 action_save_backups(title_info* title, save_kind kind, save_backup** out);
// Asks on the console (red) whether to replace the data with the backup in `path`; false when it is not one of those listed.
bool action_save_import_backup(title_info* title, const char* name, save_kind kind, u32 extId, const char* path);
// Deletes one of the backups listed for the title (only that kind's part of an NSA export; the folder once empty).
Result action_save_delete_backup(title_info* title, save_kind kind, const char* path);
// Goes up whenever a title's backups change, for the screens that list them.
u32 action_save_generation();
// For the web server: the package of a save, made on the console behind its progress screen (B cancels).
// claim (any thread) reserves the one slot, false while one is being made; start runs on the main thread.
typedef struct {
    bool running;
    bool finished;      // res, path (inside the SD archive) and size are the last package's.
    bool counting;      // Still counting what to pack.
    Result res;
    u64 processed;
    u64 total;
    char path[512];
    u64 size;
} save_pack_status;
bool action_save_pack_web_claim();
// Gives the slot back when the claim could not be handed to the main thread.
void action_save_pack_web_release();
void action_save_pack_web_start(title_info* title, const char* name, save_kind kind, u32 extId);
void action_save_pack_web_status(save_pack_status* out);
void action_save_pack_web_cancel();
bool action_save_data_exists(title_info* title, save_kind kind, u32* extId);
bool action_find_boss_data(title_info* info, u32* extId);
// The receiving side: a package's file name tells what it is; `path` is inside the SD archive.
bool action_save_package_name(const char* fileName, u64* titleId, save_kind* kind, char* name, size_t nameSize);
bool action_save_package_can_import(u64 titleId, save_kind kind);
void action_save_package_received(const char* path, bool importNow);
void action_import_secure_value(linked_list* items, list_item* selected);
void action_export_secure_value(linked_list* items, list_item* selected);
void action_delete_secure_value(linked_list* items, list_item* selected);

// Tests (SITE SAVEORDER, DEBUG): false makes "Save" take the file in long pieces written where they go (http_download_file)
// instead of in order, as an install does (the default).
void action_install_url_save_in_order(bool inOrder);
void action_install_url(const char* confirmMessage, const char* urls, const char* paths, void* userData,
                        void (*finishedURL)(void* data, u32 index),
                        void (*finishedAll)(void* data),
                        void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index));
// A remote install (QR code, a typed URL, URLs from a computer or the web page): the same, and a .zip or .7z that arrives
// is saved to /nsa/received/ and offered to be taken out onto the SD card (issue #51).
void action_install_url_remote(const char* confirmMessage, const char* urls, void* userData, void (*finishedAll)(void* data));
// The same with the choice made for the user (>= 0, one of installurl.c's INSTALL_URL_CHOICE_*), or -1 to ask.
void action_install_url_choice(const char* confirmMessage, const char* urls, const char* paths, void* userData,
                               void (*finishedURL)(void* data, u32 index),
                               void (*finishedAll)(void* data),
                               void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index),
                               int choice);
// The Store's scripts: each URL saved to its path in `paths` (one a line, "/..." on the SD card; folders made, a file there
// replaced), with the progress screen but no question and nothing said when it ends (errors are). finishedURL runs for
// each file that came, finishedAll at the end.
void action_download_urls_to(const char* urls, const char* paths, void* userData, void (*finishedURL)(void* data, u32 index),
                             void (*finishedAll)(void* data));
// Only downloads the file(s) to /nsa/received/, to install later from Files.
void action_save_url(const char* urls, void* userData, void (*finishedURL)(void* data, u32 index), void (*finishedAll)(void* data));