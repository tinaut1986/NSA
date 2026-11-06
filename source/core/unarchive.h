#pragma once

// Takes files out of a .zip (stored or deflated, through zlib) or a .7z (the LZMA SDK in source/libs/lzma), as
// Universal-DB's scripts ask: every entry whose path inside the archive matches `pattern` (minire.h) goes to `output` plus
// what follows the match (so "^3ds/app" to "/3ds/app" takes a whole folder, and "^app.cia" to "/app.cia" one file).
// Paths are "sdmc:/..."; folders are made as needed and files already there are replaced.

typedef struct {
    volatile bool* cancel;   // Set (by another thread) to stop: R_APP_CANCELLED.
    volatile u64 done;       // Bytes written so far, of
    volatile u64 total;      // what the matching entries hold (known once the archive is read).
    void* userData;
    void (*created)(void* userData, const char* path);   // Each file written, as it is closed.
} unarchive_progress;

// The archive's kind from its first bytes: 'z' zip, '7' 7z, 0 neither.
char unarchive_kind(const char* path);
// How many files it wrote in *count (0 is not an error: nothing matched).
Result unarchive_extract(const char* archive, const char* pattern, const char* output, unarchive_progress* progress, u32* count);
