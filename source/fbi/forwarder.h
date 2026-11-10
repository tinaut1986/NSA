#pragma once

// A HOME Menu icon for a .3dsx (issue #46): a small CIA whose only job is to start that .3dsx, which stays where it is
// on the SD card. It is NSA's forwarder template (romfs:/forwarder.cia, built from forwarder/) with the .3dsx's path,
// its icon and names and a title ID of its own; starting it needs Luma3DS's 3DSX loader and the Homebrew Launcher
// title, as "Run program" does.

// Whether one can be made and started on this console; when not, `why` says what is missing (translated).
bool forwarder_available(char* why, size_t size);

// Makes the forwarder for the .3dsx at `path` (on the SD card, "/3ds/foo.3dsx") and installs it. `name` gets the name
// the HOME Menu shows for it.
Result forwarder_install(const char* path, char* name, size_t nameSize);

// The same CIA written to `out` ("sdmc:/..."), not installed: for the tests (DEBUG "SITE FORWARDER").
Result forwarder_save(const char* path, const char* out, char* name, size_t nameSize, u64* titleId);
