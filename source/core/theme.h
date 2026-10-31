#pragma once

// Themes: how every widget looks. A theme gives the palette for day and night,
// the shapes (radii, border width) and the layout of menus; the accent colour
// and night mode are picked separately, so any theme works with any of them.
// Colours are 0xAABBGGRR, as the renderer takes them.

#define THEME_RGB(rgb) (0xFF000000u | (((rgb) & 0xFF) << 16) | ((rgb) & 0xFF00) | (((rgb) >> 16) & 0xFF))
#define THEME_ALPHA(color, alpha) (((u32) (alpha) << 24) | ((color) & 0x00FFFFFF))

typedef struct {
    u32 background;     // Behind everything.
    u32 surface;        // Cards, buttons, bars.
    u32 border;         // Hairlines and outlines.
    u32 text;
    u32 textMuted;      // Secondary text, hints.
    u32 scrim;          // Behind a modal.
    u32 success;
    u32 successSoft;    // Background of a success badge.
    u32 danger;
    u32 warning;
} theme_palette;

typedef enum {
    THEME_MENU_GRID2,  // Main menu as buttons in two columns.
    THEME_MENU_TILES   // Main menu as big icon tiles, four to a row, with the name under each.
} theme_menu_layout;

typedef struct {
    const char* id;       // Saved in settings.json.
    const char* name;     // English; shown through the translation layer.
    theme_palette day;
    theme_palette night;
    float radius;         // Cards and modals.
    float radiusSmall;    // Buttons and rows.
    float border;         // Outline width.
    bool pixelFont;       // Text in the bundled pixel font (pixelfont.c) instead of the system's.
    bool blockBars;       // Progress bars as blocks instead of a smooth fill.
    bool sheetPrompts;    // Dialogs rise from the bottom as a full-width sheet.
    theme_menu_layout menuLayout;
} theme_def;

typedef struct {
    const char* id;
    const char* name;
    u32 day;      // Accent on the day palette (white text on it).
    u32 night;    // Accent on the night palette.
} theme_accent;

typedef enum {
    THEME_NIGHT_OFF,
    THEME_NIGHT_ON,
    THEME_NIGHT_AUTO   // 20:00 to 7:59 by the console's clock.
} theme_night_mode;

// What the widgets draw with: the chosen theme and accent, resolved for day or night.
typedef struct {
    const theme_def* def;
    const theme_palette* palette;
    u32 accent;
    u32 onAccent;      // Text and icons on the accent.
    u32 accentSoft;    // A light wash of the accent (selected backgrounds).
    bool night;
} theme_current;

u32 theme_count();
const theme_def* theme_get(u32 index);
u32 theme_find(const char* id);

u32 theme_accent_count();
const theme_accent* theme_accent_get(u32 index);
u32 theme_accent_find(const char* id);

// Picks the theme, accent and night mode; call again whenever one changes.
void theme_select(u32 theme, u32 accent, theme_night_mode night);
// Re-evaluates THEME_NIGHT_AUTO against the clock; cheap, call once per frame.
void theme_update();
const theme_current* theme();

// Mixes two opaque colours: weight 0 is all `a`, 255 all `b`.
u32 theme_mix(u32 a, u32 b, u32 weightB);
