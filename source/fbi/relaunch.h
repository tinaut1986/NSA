#pragma once

// Restarting NSA into a build of itself that was just installed (by the
// updater or the FTP server's SITE INSTALL).
//
// Jumping into the new build at once crashed the system loader in the ports
// (zelda-tmc-3ds issue #4): wait RELAUNCH_DELAY_MS, then exit normally and let
// libctru's chainloader start the title again.

#define RELAUNCH_DELAY_MS 10000

// Any thread. Only a CIA build can restart itself; a 3DSX returns false (the
// Homebrew Launcher has to start it again).
bool relaunch_request();
bool relaunch_pending();

// The countdown for the bottom screen's toast; false when none is running.
bool relaunch_notice(char* out, size_t size, float* progress);

// Main thread, once per frame: true when NSA must exit now.
bool relaunch_poll();
