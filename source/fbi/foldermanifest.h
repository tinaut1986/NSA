#pragma once

// The list of what is in a folder sent to another console (transfer.c, kind "folder").
// The payload is this manifest followed by the bytes of each file, in the order listed:
//   "NSAF", u32 entries, then per entry: u8 type (0 folder, 1 file), u16 path length,
//   the path (relative to the folder, '/' between names, no leading slash), u64 size
//   (0 for a folder). Numbers are big endian.
// Pure C (no 3DS headers), so the host tests in tests/ can build it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FOLDER_MANIFEST_MAX_ENTRIES 20000
#define FOLDER_MANIFEST_MAX_SIZE (4 * 1024 * 1024)
#define FOLDER_PATH_MAX 400

typedef struct {
    char* path;           // Owned by the layout.
    uint64_t size;
    bool isDir;
    uint64_t start;       // Where a file's bytes begin in the payload.
} folder_entry;

typedef struct {
    folder_entry* entries;
    uint32_t count;
    uint32_t* files;      // Indexes into `entries` of the files, in payload order.
    uint32_t fileCount;
    uint64_t head;        // Size of the manifest: the payload's first bytes.
    uint64_t total;       // Size of the whole payload.
} folder_layout;

// Does `rel` stay inside the folder it is written in? No '..', no empty names, no
// ':' or '\\', no leading slash.
bool folder_path_ok(const char* rel);

// Adds an entry, copying `path`. False when there is no room or the path is not ok.
bool folder_layout_add(folder_layout* layout, const char* path, bool isDir, uint64_t size);

// Works out `head`, `total`, `files` and each file's `start`. False when the manifest
// would be too big.
bool folder_layout_finish(folder_layout* layout);

// The manifest bytes of a finished layout (malloc'd; NULL when out of memory).
uint8_t* folder_manifest_build(const folder_layout* layout, uint32_t* size);

// Reads and checks a manifest into a finished layout. False when it is damaged or unsafe.
bool folder_manifest_parse(const uint8_t* data, size_t size, folder_layout* out);

// The file that holds payload byte `offset` (NULL for the manifest or past the end).
const folder_entry* folder_layout_find(const folder_layout* layout, uint64_t offset);

void folder_layout_free(folder_layout* layout);
