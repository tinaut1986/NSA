#pragma once

// The keys of NSA's on-screen keyboards: the search and name keyboard (osk.c) and the text editor's (textview.c).
// Both have the same three pages of keys under a toolbar of their own.

typedef enum {
    K_CHAR,
    K_SHIFT,
    K_BACK,
    K_PAGE_LETTERS,
    K_PAGE_SYMBOLS,
    K_PAGE_EXTRA,
    K_SPACE,
    K_OK,
    K_ENTER,
    K_CANCEL,
    K_LEFT,
    K_RIGHT,
    K_UP,
    K_DOWN,
    K_CLEAR,
    K_SAVE,
    K_DONE
} key_action;

typedef struct {
    const char* lower;
    const char* upper;
    float width;   // In key units: ten make a row.
    key_action action;
} key_def;

#define KC(l, u) {l, u, 1, K_CHAR}
#define KS(s) {s, s, 1, K_CHAR}
#define KA(act, w) {"", "", w, act}

#define KB_ROW_MAX 11

typedef struct {
    u32 count;
    key_def keys[KB_ROW_MAX];
} key_row;

typedef struct {
    float x, y, w, h;
    const key_def* def;
} key_rect;

#define KEYS_MAX 48

// Lays out `toolbar` over page `page` (0 letters, 1 symbols, 2 accents and more) into `out` (up to `max`); the key at
// the right of the bottom row is `confirm` (K_OK, or K_ENTER for a new line). Returns how many keys.
u32 keys_build(key_rect* out, u32 max, const key_row* toolbar, u32 page, key_action confirm);
// Draws a key; `shift` 0 off, 1 the next letter, 2 caps lock.
void keys_draw(const key_rect* key, bool pressed, u32 shift);
// The key a finger is on, or -1.
int keys_hit(const key_rect* keys, u32 count, const touchPosition* pos);
// A key held by a finger (`pressed`, -1 for none) repeats: true when it is due again (after *repeatAt, which the press
// set, 0 for a key that does not repeat) and the finger is still on it; *repeatAt then moves 60 ms on.
bool keys_touch_repeat(const key_rect* keys, int pressed, u64* repeatAt, const touchPosition* pos);
// A button held on a keyboard: true on the press, then every 60 ms after 400 ms. `holdAt` is the button's own.
bool keys_hold_repeat(u64* holdAt, u32 mask);
