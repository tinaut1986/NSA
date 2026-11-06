#pragma once

// Floating layers over the current view, which stays drawn underneath.

typedef struct ui_view_s ui_view;

// ---- Popover: groups of options under the title bar's tools ----

#define POPOVER_GROUPS_MAX 6
#define POPOVER_OPTIONS_MAX 8

typedef struct {
    const char* label;      // Translated by the caller.
    bool multiple;          // Chips that toggle each, or one of a segmented choice.
    u32 count;
    const char* options[POPOVER_OPTIONS_MAX];
    int icons[POPOVER_OPTIONS_MAX];   // -1 for none.
    // Only the icons show (a small chip each), and the one with the focus (or, with none, the one chosen) shows its
    // label in a tooltip above it. Every option needs an icon.
    bool iconOnly;
    // A sort: the chosen option shows which way it goes (a chevron beside its icon), and choosing it again turns it
    // round (the caller's select does that; isReversed says which way it is).
    bool directional;
} popover_group;

typedef struct {
    u32 groupCount;
    popover_group groups[POPOVER_GROUPS_MAX];
    bool (*isOn)(void* data, u32 group, u32 option);
    void (*select)(void* data, u32 group, u32 option);   // Toggle, or choose.
    bool (*isReversed)(void* data, u32 group, u32 option);   // Only for directional groups; NULL: never.
} popover_def;

void popover_display(const popover_def* def, void* data);

// ---- Sheet: actions for one item, rising from the bottom ----

#define SHEET_ACTIONS_MAX 24

typedef struct {
    u32 icon;
    const char* label;      // Translated by the caller.
    bool danger;            // Drawn in red; the action must still confirm.
    void (*run)(void* data);
    bool stay;              // Runs with the sheet still open: B in what it opens comes back to it.
    const char* detail;     // List only: a second, smaller line under the label (translated by the caller), or NULL.
} sheet_action;

// An action leaves the sheet under what it opens: backing out of that (B, No)
// shows the sheet again, while anything confirmed on the way closes it.
// Prompts call sheet_confirmed() for an answer that goes ahead.
void sheet_confirmed();

// `title` (or NULL for none, when the top screen already shows the item) and
// `texture`, an icon to show by it (or 0), make the sheet's header.
void sheet_display(const char* title, u32 texture, const sheet_action* actions, u32 count, void* data);
// The same as a list: one row per action with its whole label; scrolls.
void sheet_display_list(const char* title, u32 texture, const sheet_action* actions, u32 count, void* data);

// A question answered from a list: the rows are the answers, each with its detail line.
// The sheet closes before the chosen action runs. B and a touch above the list call `cancel` instead (the caller's "no"), so nothing is left unanswered.
// `drawTop` draws the top screen (NULL: the screen underneath).
void sheet_display_choice(const char* title, const sheet_action* actions, u32 count, void* data,
                          void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2),
                          void (*cancel)(void* data));
