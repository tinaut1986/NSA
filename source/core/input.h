#pragma once

// Input as the UI sees it: the console's buttons and touch screen, plus what
// the FTP server's remote control injects (SITE PRESS / SITE TAP). The hid*
// calls below are redirected here for every file that includes this header.

void input_scan();

u32 input_keys_down();
u32 input_keys_held();
u32 input_keys_up();
void input_touch_read(touchPosition* pos);

// From any thread; applied on the next scans. A press is held for `frames`.
void input_inject_keys(u32 keys, u32 frames);
void input_inject_tap(u16 x, u16 y, u32 frames);
// A finger that goes from y1 to y2 at x over `frames` frames.
void input_inject_drag(u16 x, u16 y1, u16 y2, u32 frames);

#define hidKeysDown() input_keys_down()
#define hidKeysHeld() input_keys_held()
#define hidKeysUp() input_keys_up()
#define hidTouchRead(pos) input_touch_read(pos)
