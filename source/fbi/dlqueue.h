#pragma once

// The download queue (issue #41): downloads that need no one in the middle (a Store CIA or 3DSX, URLs from a remote
// install, a QR code or the web page) wait their turn and run one at a time in the background, installing as they come.
// The queue screen pauses and resumes the whole queue (now, or when the current one ends) and each item, cancels,
// reorders and retries them. An FTP or web transfer that finds the queue using the connection asks the user whether to
// pause it until that transfer ends.

// Adds a download: `urls` one per line, `name` what the screen calls it; `archives` as a remote install (a .zip or .7z is
// offered to be extracted). `finishedURL` / `finishedAll` with `userData` are the caller's, as for action_install_url:
// finishedAll runs once the item is done, cancelled or removed. False (and nothing kept) when the queue is full.
bool dlqueue_add(const char* name, const char* urls, bool archives, void* userData, void (*finishedURL)(void* data, u32 index),
                 void (*finishedAll)(void* data));
// A Store script asked beforehand (storescript.c): the queue runs it with its answers. False when the queue is full.
bool dlqueue_add_script(const char* name, void* script);
// The main loop: starts the next item, ends the running one's turn, asks about a transfer waiting for the connection.
void dlqueue_tick();
// The toast while the queue has work (or failures nobody has seen); a tap on it opens the queue (dlqueue_open_any).
bool dlqueue_notice(char* out, size_t size, float* progress);
bool dlqueue_open_any();
// The queue screen.
void dlqueue_open();
