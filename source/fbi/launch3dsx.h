#pragma once

// Starting a .3dsx from NSA, through Luma3DS's 3DSX loader (the "hb:ldr" service, the one the
// Homebrew Launcher itself uses): the file is set as the target, then the title the loader
// replaces (the Homebrew Launcher placeholder) is started, and it loads the file instead of
// hbmenu. From a 3DSX build that title is this very process, so it just starts itself again.

// Whether it can be done on this console; when not, `why` says what is missing (translated).
bool launch3dsx_available(char* why, size_t size);

// Sets `path` (on the SD card, "/3ds/foo.3dsx") as the target and asks NSA to exit into it.
Result launch3dsx_start(const char* path);

// Main thread, once per frame: true when NSA must exit now.
bool launch3dsx_poll();
