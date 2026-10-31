#include <string.h>
#include <time.h>

#include <3ds.h>

#include "screen.h"
#include "theme.h"

static const theme_def themes[] = {
        {
                .id = "cards",
                .name = "Cards",
                .day = {
                        .background = THEME_RGB(0xEEF2F1),
                        .surface = THEME_RGB(0xFFFFFF),
                        .border = THEME_RGB(0xD5DEDB),
                        .text = THEME_RGB(0x1E2B28),
                        .textMuted = THEME_RGB(0x5F6B67),
                        .scrim = THEME_ALPHA(THEME_RGB(0x1E2B28), 0x70),
                        .success = THEME_RGB(0x0F6E56),
                        .successSoft = THEME_RGB(0xE1F5EE),
                        .danger = THEME_RGB(0xA32D2D),
                        .warning = THEME_RGB(0x854F0B)
                },
                .night = {
                        .background = THEME_RGB(0x141917),
                        .surface = THEME_RGB(0x1F2624),
                        .border = THEME_RGB(0x34403C),
                        .text = THEME_RGB(0xE6ECEA),
                        .textMuted = THEME_RGB(0x97A39F),
                        .scrim = THEME_ALPHA(THEME_RGB(0x000000), 0xA0),
                        .success = THEME_RGB(0x5DCAA5),
                        .successSoft = THEME_RGB(0x085041),
                        .danger = THEME_RGB(0xF09595),
                        .warning = THEME_RGB(0xEF9F27)
                },
                .radius = 10,
                .radiusSmall = 8,
                .border = 1,
                .menuLayout = THEME_MENU_GRID2
        },
        {
                // Square corners, thick outlines, a pixel font and bars in blocks.
                .id = "pixel",
                .name = "Pixel",
                .day = {
                        .background = THEME_RGB(0xF2EEDC),
                        .surface = THEME_RGB(0xFFFCF0),
                        .border = THEME_RGB(0x3C3858),
                        .text = THEME_RGB(0x181624),
                        .textMuted = THEME_RGB(0x645F7A),
                        .scrim = THEME_ALPHA(THEME_RGB(0x181624), 0x80),
                        .success = THEME_RGB(0x0B7A43),
                        .successSoft = THEME_RGB(0xD6F2E2),
                        .danger = THEME_RGB(0xB3261E),
                        .warning = THEME_RGB(0x8A5A00)
                },
                .night = {
                        .background = THEME_RGB(0x0E1016),
                        .surface = THEME_RGB(0x1A1E2A),
                        .border = THEME_RGB(0x586078),
                        .text = THEME_RGB(0xECECE4),
                        .textMuted = THEME_RGB(0x969CB0),
                        .scrim = THEME_ALPHA(THEME_RGB(0x000000), 0xB0),
                        .success = THEME_RGB(0x50DC8C),
                        .successSoft = THEME_RGB(0x0C3A24),
                        .danger = THEME_RGB(0xFF6E6E),
                        .warning = THEME_RGB(0xFFC63C)
                },
                .radius = 0,
                .radiusSmall = 0,
                .border = 2,
                .pixelFont = true,
                .blockBars = true,
                .menuLayout = THEME_MENU_GRID2
        }
};

static const theme_accent accents[] = {
        {"green", "Green", THEME_RGB(0x0F6E56), THEME_RGB(0x1D9E75)},
        {"blue", "Blue", THEME_RGB(0x185FA5), THEME_RGB(0x378ADD)},
        {"violet", "Violet", THEME_RGB(0x534AB7), THEME_RGB(0x7F77DD)},
        {"pink", "Pink", THEME_RGB(0x993556), THEME_RGB(0xD4537E)},
        {"coral", "Coral", THEME_RGB(0xB04A24), THEME_RGB(0xD85A30)},
        {"amber", "Amber", THEME_RGB(0x8A5A10), THEME_RGB(0xBA7517)},
        {"graphite", "Graphite", THEME_RGB(0x5F5E5A), THEME_RGB(0x888780)},
        {"red", "Red", THEME_RGB(0xA32D2D), THEME_RGB(0xE24B4A)},
        {"yellow", "Yellow", THEME_RGB(0xC79000), THEME_RGB(0xFFCC00)}
};

#define THEME_COUNT (sizeof(themes) / sizeof(*themes))
#define ACCENT_COUNT (sizeof(accents) / sizeof(*accents))

static u32 selected_theme = 0;
static u32 selected_accent = 0;
static theme_night_mode selected_night = THEME_NIGHT_OFF;
static theme_current current;

u32 theme_count() {
    return THEME_COUNT;
}

const theme_def* theme_get(u32 index) {
    return &themes[index < THEME_COUNT ? index : 0];
}

u32 theme_find(const char* id) {
    for(u32 i = 0; id != NULL && i < THEME_COUNT; i++) {
        if(strcmp(themes[i].id, id) == 0) {
            return i;
        }
    }

    return 0;
}

u32 theme_accent_count() {
    return ACCENT_COUNT;
}

const theme_accent* theme_accent_get(u32 index) {
    return &accents[index < ACCENT_COUNT ? index : 0];
}

u32 theme_accent_find(const char* id) {
    for(u32 i = 0; id != NULL && i < ACCENT_COUNT; i++) {
        if(strcmp(accents[i].id, id) == 0) {
            return i;
        }
    }

    return 0;
}

u32 theme_mix(u32 a, u32 b, u32 weightB) {
    u32 out = 0xFF000000;
    for(u32 shift = 0; shift < 24; shift += 8) {
        u32 ca = (a >> shift) & 0xFF;
        u32 cb = (b >> shift) & 0xFF;
        out |= ((ca * (255 - weightB) + cb * weightB) / 255) << shift;
    }

    return out;
}

static bool theme_is_night() {
    if(selected_night != THEME_NIGHT_AUTO) {
        return selected_night == THEME_NIGHT_ON;
    }

    time_t t = time(NULL);
    struct tm* local = gmtime(&t);  // libctru's time() is already local time.
    return local != NULL && (local->tm_hour >= 20 || local->tm_hour < 8);
}

// The legacy colour IDs (FBI's textcolor.cfg) follow the theme, so screens not
// yet redrawn with widgets stay readable in both palettes.
static void theme_apply_legacy_colors() {
    const theme_palette* p = current.palette;
    screen_set_color(COLOR_TEXT, p->text);
    for(u32 id = 1; id <= 10; id++) {
        screen_set_color(id, p->text);
    }

    screen_set_color(2, current.accent);      // SD
    screen_set_color(6, current.accent);      // Directory
    screen_set_color(7, p->success);          // Enabled
    screen_set_color(8, p->danger);           // Disabled
    screen_set_color(9, p->success);          // Ticket in use
    screen_set_color(10, p->textMuted);       // Ticket not in use
}

static void theme_resolve() {
    current.def = &themes[selected_theme];
    current.night = theme_is_night();
    current.palette = current.night ? &current.def->night : &current.def->day;
    current.accent = current.night ? accents[selected_accent].night : accents[selected_accent].day;
    // White on the accent, or near black when the accent is a light one (yellow).
    u32 r = current.accent & 0xFF;
    u32 g = (current.accent >> 8) & 0xFF;
    u32 b = (current.accent >> 16) & 0xFF;
    current.onAccent = (r * 299 + g * 587 + b * 114) / 1000 > 160 ? THEME_RGB(0x14120A) : THEME_RGB(0xFFFFFF);
    current.accentSoft = theme_mix(current.palette->surface, current.accent, current.night ? 0x40 : 0x24);
    screen_set_pixel_font(current.def->pixelFont);
    screen_set_square_corners(current.def->radius < 1);
    theme_apply_legacy_colors();
}

void theme_select(u32 theme, u32 accent, theme_night_mode night) {
    selected_theme = theme < THEME_COUNT ? theme : 0;
    selected_accent = accent < ACCENT_COUNT ? accent : 0;
    selected_night = night;
    theme_resolve();
}

void theme_update() {
    if(selected_night == THEME_NIGHT_AUTO && theme_is_night() != current.night) {
        theme_resolve();
    }
}

const theme_current* theme() {
    if(current.def == NULL) {
        theme_resolve();
    }

    return &current;
}
