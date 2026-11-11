#pragma once

// Guided tours on the real screens: a bubble points at a button or an area ("anchor") and waits for the user to do
// what it says. Everything else is dimmed and its input is held back, so a tour cannot change anything by mistake.
//
// Anchors: screens say where they draw what a tour may point at, every frame, with coach_anchor (ids such as
// "menu.titles", "tool.filter", "list.selected", "sheet", "back", "hints", "top"; a sheet's rows use their label).
// ui.c calls the coach around every frame; nothing else is needed from a screen.

#define COACH_PASS_DPAD (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)

typedef struct {
    const char* title;   // Shown small above the text (the chapter): set by the caller, translated by it.
    const char* anchor;  // What it points at (an English label is translated before matching), or NULL.
    const char* text;    // English: translated when drawn.
    // What the user must do. With neither, it is an explanation: A, or a tap on the bubble, goes on (not a tap elsewhere).
    u32 keys;            // Buttons that do it. A only counts while the anchor is the selected one.
    bool tap;            // A tap on the anchor does it.
    u32 pass;            // Also let through, without going on (the D-Pad, to bring the focus to the anchor).
    // After doing it, wait for: "v:<view id or English name>" on top, "<anchor>" drawn, "!<anchor>" gone, "@<anchor>"
    // with the focus; several separated by "|" wait for any of them. NULL: the step is done as soon as it is done.
    const char* until;
    // Only when this anchor is on the screen as the step comes (a branch the last step may have taken); else skipped.
    const char* only;
    bool home;           // Before it, go back to the main menu (B until only it is open).
    u32 tag;             // For the caller (the chapter).
    u32 since;           // For the caller: the version that brought the step (the guide's), 0 for its chapter's.
    bool fresh;          // A "New" badge by the title: what the step says came in the last version.
} coach_step;

// Plays `steps` (copied). `onStep` runs as each one is done (`last`: the last of its tag), `onEnd` when the tour ends,
// finished or left with START (`finished`).
void coach_start(const coach_step* steps, u32 count, void (*onStep)(u32 tag, bool last), void (*onEnd)(bool finished));
bool coach_active();
// Whether the step on screen points at `id`: a screen can show then what is usually hidden (the list's jump arrows).
bool coach_points_at(const char* id);

// Where something a tour may point at was drawn this frame, on the screen being drawn; `selected`: it has the focus.
void coach_anchor(const char* id, float x, float y, float width, float height, bool selected);

// ---- for ui.c and input.c ----

// Before the view's update: reads the keys, decides what may go through (input_scan applied coach_filter).
void coach_update();
// Called by input_scan with the keys held and the touch: returns what the view may see.
u32 coach_filter(u32 held, const touchPosition* touch);
// Drawing: a frame begins, a screen begins (anchors drawn after it are on it), the overlay over a screen.
void coach_frame_begin();
void coach_screen_begin(gfxScreen_t screen, bool record);
void coach_draw(gfxScreen_t screen);
