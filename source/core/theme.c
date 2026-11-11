#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <3ds.h>

#include "appinfo.h"
#include "screen.h"
#include "stringutil.h"
#include "theme.h"
#include "themefile.h"

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
        },
        {
                // Big rounded icon tiles and dialogs as a sheet from the bottom.
                .id = "home",
                .name = "HOME",
                .day = {
                        .background = THEME_RGB(0xE2E6EE),
                        .surface = THEME_RGB(0xFFFFFF),
                        .border = THEME_RGB(0xC8CFDC),
                        .text = THEME_RGB(0x1E2432),
                        .textMuted = THEME_RGB(0x646E82),
                        .scrim = THEME_ALPHA(THEME_RGB(0x1E2432), 0x70),
                        .success = THEME_RGB(0x0F7A56),
                        .successSoft = THEME_RGB(0xDDF3EA),
                        .danger = THEME_RGB(0xC83232),
                        .warning = THEME_RGB(0x9A6200)
                },
                .night = {
                        .background = THEME_RGB(0x12151C),
                        .surface = THEME_RGB(0x1E232E),
                        .border = THEME_RGB(0x363E50),
                        .text = THEME_RGB(0xE6EAF2),
                        .textMuted = THEME_RGB(0x98A2B8),
                        .scrim = THEME_ALPHA(THEME_RGB(0x000000), 0xA0),
                        .success = THEME_RGB(0x5DCAA5),
                        .successSoft = THEME_RGB(0x0C3A2C),
                        .danger = THEME_RGB(0xF09595),
                        .warning = THEME_RGB(0xEF9F27)
                },
                .radius = 16,
                .radiusSmall = 12,
                .border = 1,
                .sheetPrompts = true,
                .pagedGrids = true,
                .menuLayout = THEME_MENU_TILES
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

// Themes from the SD card, after the built-in ones: "sd:<file name without .json>".
#define THEME_SD_MAX 32
#define THEME_SD_TEXT 48
#define THEME_SD_FILE_MAX (32 * 1024)

typedef struct {
    theme_def def;
    char id[THEME_SD_TEXT + 4];
    char name[THEME_SD_TEXT];
} theme_sd;

static theme_sd sd_themes[THEME_SD_MAX];
static u32 sd_count = 0;
static char sd_errors[256] = "";

static u32 selected_theme = 0;
static u32 selected_accent = 0;
static theme_night_mode selected_night = THEME_NIGHT_OFF;
static theme_current current;

u32 theme_count() {
    return THEME_COUNT + sd_count;
}

const theme_def* theme_get(u32 index) {
    if(index >= THEME_COUNT && index < THEME_COUNT + sd_count) {
        return &sd_themes[index - THEME_COUNT].def;
    }

    return &themes[index < THEME_COUNT ? index : 0];
}

u32 theme_find(const char* id) {
    for(u32 i = 0; id != NULL && i < theme_count(); i++) {
        if(strcmp(theme_get(i)->id, id) == 0) {
            return i;
        }
    }

    return 0;
}

bool theme_is_custom(u32 index) {
    return index >= THEME_COUNT && index < THEME_COUNT + sd_count;
}

u32 theme_custom_count() {
    return sd_count;
}

const char* theme_custom_errors() {
    return sd_errors;
}

static int theme_sd_compare(const void* a, const void* b) {
    return strcasecmp(((const theme_sd*) a)->name, ((const theme_sd*) b)->name);
}

static void theme_note_error(const char* file, const char* error) {
    size_t used = strlen(sd_errors);
    snprintf(sd_errors + used, sizeof(sd_errors) - used, "%s%s: %s", used > 0 ? "\n" : "", file, error);
}

static void theme_load_sd() {
    sd_count = 0;
    sd_errors[0] = '\0';

    DIR* dir = opendir("sdmc:" APP_DATA_DIR "themes");
    if(dir == NULL) {
        return;
    }

    struct dirent* entry;
    while((entry = readdir(dir)) != NULL && sd_count < THEME_SD_MAX) {
        size_t len = strlen(entry->d_name);
        if(len <= 5 || strcasecmp(entry->d_name + len - 5, ".json") != 0 || entry->d_name[0] == '.') {
            continue;
        }

        char path[320];
        snprintf(path, sizeof(path), "sdmc:" APP_DATA_DIR "themes/%s", entry->d_name);
        FILE* fd = fopen(path, "rb");
        if(fd == NULL) {
            theme_note_error(entry->d_name, "cannot be opened");
            continue;
        }

        char* text = (char*) malloc(THEME_SD_FILE_MAX + 1);
        size_t size = text != NULL ? fread(text, 1, THEME_SD_FILE_MAX + 1, fd) : 0;
        fclose(fd);
        if(text == NULL || size > THEME_SD_FILE_MAX) {
            theme_note_error(entry->d_name, "too big");
            free(text);
            continue;
        }

        text[size] = '\0';
        theme_sd* sd = &sd_themes[sd_count];
        char error[96];
        bool ok = themefile_parse(text, themes, THEME_COUNT, &sd->def, sd->name, sizeof(sd->name), error, sizeof(error));
        free(text);
        if(!ok) {
            theme_note_error(entry->d_name, error);
            continue;
        }

        char stem[THEME_SD_TEXT];
        snprintf(stem, sizeof(stem), "%.*s", (int) (len - 5), entry->d_name);
        snprintf(sd->id, sizeof(sd->id), "sd:%s", stem);
        if(sd->name[0] == '\0') {
            string_copy(sd->name, stem, sizeof(sd->name));
        }

        sd->def.id = sd->id;
        sd->def.name = sd->name;
        sd_count++;
    }

    closedir(dir);
    qsort(sd_themes, sd_count, sizeof(*sd_themes), theme_sd_compare);
    // Sorting moved the names: point at them again.
    for(u32 i = 0; i < sd_count; i++) {
        sd_themes[i].def.id = sd_themes[i].id;
        sd_themes[i].def.name = sd_themes[i].name;
    }
}

void theme_load_custom() {
    theme_load_sd();
}

static void theme_resolve();

u32 theme_reload_custom(u32 selected) {
    char id[THEME_SD_TEXT + 4];
    string_copy(id, theme_get(selected)->id, sizeof(id));
    bool wasCurrent = selected == selected_theme;
    theme_load_sd();
    u32 found = theme_find(id);
    if(wasCurrent) {
        selected_theme = found;
        theme_resolve();
    }

    return found;
}

// A file name from a theme's name: its letters and digits, in lower case, with dashes between words.
static void theme_slug(const char* name, char* stem, size_t stemSize) {
    size_t n = 0;
    for(const char* c = name; *c != '\0' && n + 1 < stemSize; c++) {
        if(isalnum((unsigned char) *c)) {
            stem[n++] = (char) tolower((unsigned char) *c);
        } else if(n > 0 && stem[n - 1] != '-') {
            stem[n++] = '-';
        }
    }

    while(n > 0 && stem[n - 1] == '-') {
        n--;
    }

    stem[n] = '\0';
    if(n == 0) {
        string_copy(stem, "theme", stemSize);
    }
}

static bool theme_file_exists(const char* path) {
    FILE* fd = fopen(path, "rb");
    if(fd != NULL) {
        fclose(fd);
    }

    return fd != NULL;
}

static bool theme_write_file(const theme_def* def, const char* name, const char* path) {
    mkdir("sdmc:" APP_DATA_DIR, 0777);
    mkdir("sdmc:" APP_DATA_DIR "themes", 0777);

    char* text = themefile_write(def, name);
    if(text == NULL) {
        return false;
    }

    FILE* fd = fopen(path, "wb");
    bool ok = fd != NULL && fwrite(text, 1, strlen(text), fd) == strlen(text);
    if(fd != NULL) {
        ok = fclose(fd) == 0 && ok;
    }

    free(text);
    return ok;
}

bool theme_export(u32 index, char* path, size_t pathSize) {
    const theme_def* def = theme_get(index);
    char stem[THEME_SD_TEXT];
    theme_slug(def->name, stem, sizeof(stem));

    // A name that is not taken: "Cards 2", "Cards 3"...
    char base[THEME_SD_TEXT];
    string_copy(base, def->name, sizeof(base));
    for(u32 number = 2; number < 1000; number++) {
        snprintf(path, pathSize, "sdmc:" APP_DATA_DIR "themes/%s-%lu.json", stem, number);
        if(!theme_file_exists(path)) {
            char name[THEME_SD_TEXT + 8];
            snprintf(name, sizeof(name), "%s %lu", base, number);
            return theme_write_file(def, name, path);
        }
    }

    return false;
}

bool theme_save_custom(const theme_def* def, const char* name, u32 editing, u32* saved, char* path, size_t pathSize) {
    char stem[THEME_SD_TEXT];
    if(theme_is_custom(editing)) {
        // A theme from the SD card is written over its own file.
        string_copy(stem, theme_get(editing)->id + 3, sizeof(stem));
        snprintf(path, pathSize, "sdmc:" APP_DATA_DIR "themes/%s.json", stem);
    } else {
        // A built-in one becomes a new file named after the name given, with a number when that is taken.
        char base[THEME_SD_TEXT];
        theme_slug(name, base, sizeof(base));
        string_copy(stem, base, sizeof(stem));
        snprintf(path, pathSize, "sdmc:" APP_DATA_DIR "themes/%s.json", stem);
        for(u32 number = 2; theme_file_exists(path) && number < 1000; number++) {
            snprintf(stem, sizeof(stem), "%.*s-%lu", THEME_SD_TEXT - 8, base, number);
            snprintf(path, pathSize, "sdmc:" APP_DATA_DIR "themes/%s.json", stem);
        }
    }

    if(!theme_write_file(def, name, path)) {
        return false;
    }

    theme_load_sd();
    char id[THEME_SD_TEXT + 4];
    snprintf(id, sizeof(id), "sd:%s", stem);
    *saved = theme_find(id);
    return true;
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

// The theme editor's copy, drawn in place of the selected theme while it edits (theme_preview).
static theme_def preview_def;
static bool previewing = false;
static int preview_night = -1;

static void theme_resolve() {
    current.def = previewing ? &preview_def : theme_get(selected_theme);
    current.night = previewing && preview_night >= 0 ? preview_night == 1 : theme_is_night();
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
    selected_theme = theme < theme_count() ? theme : 0;
    selected_accent = accent < ACCENT_COUNT ? accent : 0;
    selected_night = night;
    theme_resolve();
}

void theme_preview(const theme_def* def, int night) {
    preview_def = *def;
    previewing = true;
    preview_night = night;
    theme_resolve();
}

void theme_preview_end() {
    previewing = false;
    preview_night = -1;
    theme_resolve();
}

void theme_update() {
    if(!(previewing && preview_night >= 0) && selected_night == THEME_NIGHT_AUTO && theme_is_night() != current.night) {
        theme_resolve();
    }
}

const theme_current* theme() {
    if(current.def == NULL) {
        theme_resolve();
    }

    return &current;
}
