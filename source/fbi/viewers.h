#pragma once

// Opening files in the Files screen: pictures (PNG, JPEG, BMP, GIF, TGA and the
// 3DS camera's 3D MPO) and text.

typedef enum {
    VIEWER_NONE,
    VIEWER_IMAGE,
    VIEWER_TEXT
} viewer_kind;

// What a file is opened with by its name's extension, or VIEWER_NONE.
viewer_kind viewers_kind_of(const char* name);

// Shows the picture at `paths[index]` of `count` (the pictures of a folder: L and R
// go through them). The paths are copied; `paths` need not outlive the call.
void imageview_open(FS_Archive archive, const char* const* paths, u32 count, u32 index);

// Shows the text file, with Edit in the viewer unless `readOnly`. Any file can be opened
// this way; up to TEXTVIEW_MAX_SIZE bytes are read.
#define TEXTVIEW_MAX_SIZE (256 * 1024)
void textview_open(FS_Archive archive, const char* path, const char* name, bool readOnly);

// Reads up to `maxSize` bytes of a file into a new buffer (NUL after the data, free() it).
// `fullSize` is the file's real size when longer than what was read.
Result viewers_read_file(FS_Archive archive, const char* path, u32 maxSize, u8** data, u32* size, u64* fullSize);
