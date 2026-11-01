#pragma once

// Where downloads say how they went (the app's transfer log); NULL for nowhere.
void http_set_log(void (*log)(const char* format, ...));

// Stops new transfers, aborts the running ones and waits (up to 10 s) for them; before the sockets close.
void http_shutdown();

Result http_download_callback(const char* url, u32 bufferSize, void* userData, Result (*callback)(void* userData, void* buffer, size_t size),
                                                                               Result (*checkRunning)(void* userData),
                                                                               Result (*progress)(void* userData, u64 total, u64 curr));
Result http_download_buffer(const char* url, u32* downloadedSize, void* buf, size_t size);
Result http_download_json(const char* url, json_t** json, size_t maxSize);
Result http_download_seed(u64 titleId);