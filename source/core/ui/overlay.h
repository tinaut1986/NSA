#pragma once

// Floating layers over the current view, which stays drawn underneath.

typedef struct ui_view_s ui_view;

// ---- Popover: groups of options under the title bar's tools ----

#define POPOVER_GROUPS_MAX 4
#define POPOVER_OPTIONS_MAX 6

typedef struct {
    const char* label;      // Translated by the caller.
    bool multiple;          // Chips that toggle each, or one of a segmented choice.
    u32 count;
    const char* options[POPOVER_OPTIONS_MAX];
    int icons[POPOVER_OPTIONS_MAX];   // -1 for none.
} popover_group;

typedef struct {
    u32 groupCount;
    popover_group groups[POPOVER_GROUPS_MAX];
    bool (*isOn)(void* data, u32 group, u32 option);
    void (*select)(void* data, u32 group, u32 option);   // Toggle, or choose.
} popover_def;

void popover_display(const popover_def* def, void* data);

// ---- Sheet: actions for one item, rising from the bottom ----

#define SHEET_ACTIONS_MAX 12

typedef struct {
    u32 icon;
    const char* label;      // Translated by the caller.
    bool danger;            // Drawn in red; the action must still confirm.
    void (*run)(void* data);
} sheet_action;

// `title` (or NULL for none, when the top screen already shows the item) and
// `texture`, an icon to show by it (or 0), make the sheet's header.
void sheet_display(const char* title, u32 texture, const sheet_action* actions, u32 count, void* data);
// The same as a list: one row per action with its whole label; scrolls.
void sheet_display_list(const char* title, u32 texture, const sheet_action* actions, u32 count, void* data);
