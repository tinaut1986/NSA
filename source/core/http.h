#pragma once

// Where downloads say how they went (the app's transfer log); NULL for nowhere.
void http_set_log(void (*log)(const char* format, ...));

// Stops new transfers, aborts the running ones and waits (up to 10 s) for them; before the sockets close.
void http_shutdown();

// Resumes: `*from` (in) where an earlier try's bytes end, `expected` the file's size then. When the server serves ranges and
// the size is the same, it starts at the 1 MB segment `*from` falls in, else at 0; `*from` (out) is where, set before
// the first byte reaches `callback` (which gets the file from there on). `progress` counts from the file's start.
Result http_download_callback_from(const char* url, u32 bufferSize, u64* from, u64 expected, void* userData,
                                   Result (*callback)(void* userData, void* buffer, size_t size), Result (*checkRunning)(void* userData),
                                   Result (*progress)(void* userData, u64 total, u64 curr, u32 connections));
Result http_download_callback(const char* url, u32 bufferSize, void* userData, Result (*callback)(void* userData, void* buffer, size_t size),
                                                                               Result (*checkRunning)(void* userData),
                                                                               Result (*progress)(void* userData, u64 total, u64 curr, u32 connections));
// For a caller that can write anywhere in its file (saving): a big file comes over several connections, each piece written
// where it goes (`writeAt`, from several threads but one at a time). `open` gets the first `firstSize` bytes (fewer for a
// smaller file) and the size (0 when not known) before anything is written. Not for an Old 3DS (installurl.c): writing far
// into a big file takes seconds there.
Result http_download_file(const char* url, u32 firstSize, void* userData, Result (*open)(void* userData, void* first, size_t size, u64 total),
                          Result (*writeAt)(void* userData, u64 offset, void* buffer, size_t size),
                          Result (*checkRunning)(void* userData), Result (*progress)(void* userData, u64 total, u64 curr, u32 connections));
// The name the server gave the file (Content-Disposition) in the download under way, for its first write on: false when
// it gave none.
bool http_file_name(char* out, size_t size);
Result http_download_buffer(const char* url, u32* downloadedSize, void* buf, size_t size);
Result http_download_json(const char* url, json_t** json, size_t maxSize);
Result http_download_seed(u64 titleId);