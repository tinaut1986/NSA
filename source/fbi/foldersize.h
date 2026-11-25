#pragma once

// What a folder holds. foldersize_open counts everything in the folder at `path` of `archive` (`archiveId`: what it is,
// ARCHIVE_SDMC...) on a thread, the total growing on the screen (B stops it), then shows what takes its room: its
// contents biggest first, each with its share, and its subfolders to go into. `name` is what the screen calls it. A
// folder counted before (or inside one counted) needs no count: the sizes are kept for the session.
void foldersize_open(FS_Archive archive, u32 archiveId, const char* path, const char* name);
// The same for the whole SD card (Utilities).
void foldersize_open_sd();

// A folder's size from the counts made (`path` with or without its last '/'); false when it has not been counted.
// Any pointer may be NULL.
bool foldersize_cached(u32 archiveId, const char* path, u64* bytes, u32* files, u32* folders);
// Forgets every size: something changed files.
void foldersize_forget_all();
// Changes each time the sizes are forgotten.
u32 foldersize_version();
