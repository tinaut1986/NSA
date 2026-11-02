#pragma once

// A folder sent to another console (transfer.c, kind "folder"). The payload is the folder's
// manifest (foldermanifest.h) and then the bytes of each file, in the manifest's order. The
// sender reads the files as the chunks ask for them; the receiver writes each chunk straight
// into the file it belongs to, inside a <name>.part folder that is renamed when all is here.

#include <3ds.h>

#include "transferconn.h"

// ---- sending ----

typedef struct transfer_folder_send transfer_folder_send;

// Lists `path` (a folder of `archive`) with everything in it. Fails with R_APP_OUT_OF_RANGE
// when there are too many files or names for one list.
Result transfer_folder_send_scan(transfer_folder_send** out, FS_Archive archive, const char* path);
void transfer_folder_send_free(transfer_folder_send* folder);

u64 transfer_folder_send_total(const transfer_folder_send* folder);
u32 transfer_folder_send_files(const transfer_folder_send* folder);
// The manifest, the payload's first bytes.
const u8* transfer_folder_send_manifest(const transfer_folder_send* folder, u32* size);
// The segment lengths for transfer_chunks_build: the manifest, then each file (malloc'd; `count` of them).
u64* transfer_folder_send_segments(const transfer_folder_send* folder, u32* count);
// Fills the chunk's bytes.
Result transfer_folder_send_read(const transfer_folder_send* folder, FS_Archive archive, const transfer_chunk* chunk, void* buffer);

// ---- receiving ----

typedef struct transfer_folder_recv transfer_folder_recv;

// Starts receiving into `root` (a folder that ends with a slash, made if it is missing). `head` is
// the manifest's size, which the hello said.
Result transfer_folder_recv_open(transfer_folder_recv** out, FS_Archive sd, const char* root, u32 head);
// The payload's bytes at `offset`, from any connection. A file's bytes wait for the manifest.
Result transfer_folder_recv_write(transfer_folder_recv* folder, u64 offset, const void* buffer, u32 size);
// Everything is here: makes the files that have no bytes.
Result transfer_folder_recv_finish(transfer_folder_recv* folder);
void transfer_folder_recv_free(transfer_folder_recv* folder);
// How many files the manifest listed (0 until it has arrived).
u32 transfer_folder_recv_files(transfer_folder_recv* folder);
