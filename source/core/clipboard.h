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
