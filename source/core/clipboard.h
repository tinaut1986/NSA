#pragma once

bool clipboard_has_contents();
FS_Archive clipboard_get_archive();
char* clipboard_get_path();
bool clipboard_is_contents_only();
Result clipboard_set_contents(FS_Archive archive, const char* path, bool contentsOnly);
void clipboard_clear();
// A cut item is moved when pasted: renamed when it stays on the same medium,
// else copied and then deleted. `archiveId` tells whether two archives are the same medium.
bool clipboard_is_cut();
FS_ArchiveID clipboard_get_archive_id();
Result clipboard_set_cut(FS_Archive archive, FS_ArchiveID archiveId, const char* path);

// Several items of one folder (the Files screen's selection): the clipboard holds that folder (`dir`, ending in '/')
// as "contents only" plus the names chosen in it, `count` strings one after another, each ended by '\0'. A paste
// copies (or moves, when `cut`) only those, folders with all they hold. `archiveId` as for clipboard_set_cut.
Result clipboard_set_names(FS_Archive archive, FS_ArchiveID archiveId, const char* dir, const char* names, u32 count, bool cut);
// How many names the clipboard holds (0: a whole file or folder, as before), and each of them.
u32 clipboard_get_name_count();
const char* clipboard_get_name(u32 index);
// A populate_files filter (with filterTopOnly) that keeps the clipboard's names.
bool clipboard_filter_names(void* data, const char* name, u32 attributes);
