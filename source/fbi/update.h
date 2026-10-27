#pragma once

// The updater. Checks run on a worker thread; update_poll(), called once per
// frame from the main loop, shows the result.

// silent: the automatic check at start-up. It shows nothing unless an update
// is found (no "checking" screen, no errors, no "up to date").
void update_check_start(bool silent);
void update_poll();

// Downloads the channel's whole history and shows it.
void update_show_history();
