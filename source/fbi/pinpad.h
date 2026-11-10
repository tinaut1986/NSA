#pragma once

typedef struct ui_view_s ui_view;

// NSA's numeric keypad for a six-digit PIN (the web page's): `onResponse` gets SWKBD_BUTTON_CONFIRM with the six digits,
// or SWKBD_BUTTON_LEFT when it was cancelled (as osk_display answers). It has a key that makes a random PIN.
ui_view* pinpad_display(const char* hint, const char* initialPin, void* data, void (*onResponse)(ui_view* view, void* data, SwkbdButton button, const char* response));
