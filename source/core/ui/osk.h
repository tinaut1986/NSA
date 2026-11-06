#pragma once

typedef struct ui_view_s ui_view;

// NSA's own on-screen keyboard for one line of text: the system's (kbd.h) takes seconds to start, this one is a screen like any other.
// `onResponse` gets SWKBD_BUTTON_CONFIRM with the text, or SWKBD_BUTTON_LEFT when it was cancelled (as kbd_display would answer).
ui_view* osk_display(const char* hint, const char* initialText, u32 maxSize, void* data, void (*onResponse)(ui_view* view, void* data, SwkbdButton button, const char* response));
