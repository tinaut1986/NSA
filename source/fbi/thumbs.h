#pragma once

// Pictures' previews for the Files screen: a worker thread decodes each picture into a small
// square (cropped from the centre) and the UI thread makes the texture.

// A .3dsx carries its icon, name and author (its SMDH): previews of those are made too.
typedef struct {
    char title[128];
    char publisher[128];
} thumbs_info;

// The texture of the preview of the picture at `path` (a file of `size` bytes), or 0 while
// it is being made, when it cannot be (too large, unreadable) or when it is not a picture.
// `info`, when given, is filled for a .3dsx that has one. The first call asks for it; keep asking while drawing. Main thread only.
u32 thumbs_get(FS_Archive archive, const char* path, u64 size, thumbs_info* info);

// Stops the worker and frees the textures.
void thumbs_exit();
