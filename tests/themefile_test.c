// Themes from the SD card: themefile_parse and themefile_write (source/core/themefile.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/core/themefile.h"

static int failures = 0;

#define CHECK(cond) do { if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while(0)

static theme_def bases[2];

static void make_bases() {
    memset(bases, 0, sizeof(bases));
    bases[0].id = "cards";
    bases[0].name = "Cards";
    bases[0].day.background = THEME_RGB(0xEEF2F1);
    bases[0].day.text = THEME_RGB(0x1E2B28);
    bases[0].night.background = THEME_RGB(0x141917);
    bases[0].radius = 10;
    bases[0].radiusSmall = 8;
    bases[0].border = 1;
    bases[0].menuLayout = THEME_MENU_GRID2;

    bases[1] = bases[0];
    bases[1].id = "home";
    bases[1].name = "HOME";
    bases[1].radius = 16;
    bases[1].sheetPrompts = true;
    bases[1].pagedGrids = true;
    bases[1].menuLayout = THEME_MENU_TILES;
}

static bool parse(const char* text, theme_def* out, char* name, char* error) {
    return themefile_parse(text, bases, 2, out, name, 48, error, 96);
}

int main() {
    make_bases();
    theme_def def;
    char name[48];
    char error[96];

    // Colours: #RRGGBB is opaque, #RRGGBBAA keeps its alpha; the renderer takes 0xAABBGGRR.
    CHECK(parse("{\"name\": \"Ocean\", \"day\": {\"background\": \"#102030\", \"scrim\": \"#11223380\"}}", &def, name, error));
    CHECK(strcmp(name, "Ocean") == 0);
    CHECK(def.day.background == THEME_RGB(0x102030));
    CHECK(def.day.background == 0xFF302010u);
    CHECK(def.day.scrim == 0x80332211u);
    // What the file leaves out comes from the base (Cards by default).
    CHECK(def.day.text == THEME_RGB(0x1E2B28));
    CHECK(def.night.background == THEME_RGB(0x141917));
    CHECK(def.radius == 10 && def.border == 1 && def.menuLayout == THEME_MENU_GRID2);

    // Another base, and the shapes.
    CHECK(parse("{\"base\": \"home\", \"radius\": 4, \"border\": 2, \"font\": \"pixel\", \"menu\": \"grid\"}", &def, name, error));
    CHECK(name[0] == '\0');
    // Corners given alone: those of rows and buttons follow (three quarters).
    CHECK(def.radius == 4 && def.radiusSmall == 3 && def.border == 2 && def.pixelFont && def.sheetPrompts);
    CHECK(def.menuLayout == THEME_MENU_GRID2);
    CHECK(def.pagedGrids);
    // A file may still give the corners of rows and buttons; 0 is square everywhere.
    CHECK(parse("{\"radius\": 16, \"radiusSmall\": 2}", &def, name, error));
    CHECK(def.radius == 16 && def.radiusSmall == 2);
    CHECK(parse("{\"radius\": 0}", &def, name, error));
    CHECK(def.radius == 0 && def.radiusSmall == 0);
    CHECK(parse("{\"base\": \"home\", \"grid\": \"scroll\", \"font\": \"system\"}", &def, name, error));
    CHECK(!def.pagedGrids && !def.pixelFont);
    CHECK(!parse("{\"grid\": \"down\"}", &def, name, error));
    CHECK(strstr(error, "grid") != NULL);
    CHECK(!parse("{\"font\": \"comic\"}", &def, name, error));

    // Unknown keys are left alone.
    CHECK(parse("{\"author\": \"me\", \"day\": {\"sparkle\": 3}}", &def, name, error));

    // Wrong values say which.
    CHECK(!parse("{\"day\": {\"text\": \"red\"}}", &def, name, error));
    CHECK(strstr(error, "day.text") != NULL);
    CHECK(!parse("{\"night\": {\"text\": \"#12345\"}}", &def, name, error));
    CHECK(strstr(error, "night.text") != NULL);
    CHECK(!parse("{\"day\": {\"text\": \"#12345G\"}}", &def, name, error));
    CHECK(!parse("{\"radius\": 99}", &def, name, error));
    CHECK(strstr(error, "radius") != NULL);
    CHECK(!parse("{\"border\": \"2\"}", &def, name, error));
    CHECK(!parse("{\"menu\": \"pages\"}", &def, name, error));
    CHECK(!parse("{\"base\": \"neon\"}", &def, name, error));
    CHECK(!parse("{\"blockBars\": 1}", &def, name, error));
    CHECK(!parse("[1, 2]", &def, name, error));
    CHECK(!parse("{\"name\": \"x\",", &def, name, error));
    CHECK(strstr(error, "not JSON") != NULL);

    // Written and read back: the same theme.
    theme_def source = bases[1];
    source.day.surface = THEME_ALPHA(THEME_RGB(0xABCDEF), 0x40);
    source.night.danger = THEME_RGB(0xF09595);
    source.blockBars = true;
    char* text = themefile_write(&source, "Mine 2");
    CHECK(text != NULL);
    CHECK(text != NULL && strstr(text, "\"#ABCDEF40\"") != NULL);
    CHECK(text != NULL && strstr(text, "\"tiles\"") != NULL);
    CHECK(text != NULL && strstr(text, "\"pages\"") != NULL);
    theme_def back;
    CHECK(text != NULL && parse(text, &back, name, error));
    CHECK(strcmp(name, "Mine 2") == 0);
    CHECK(memcmp(&back.day, &source.day, sizeof(back.day)) == 0);
    CHECK(memcmp(&back.night, &source.night, sizeof(back.night)) == 0);
    CHECK(back.radius == source.radius && back.radiusSmall == source.radiusSmall && back.border == source.border);
    CHECK(back.pixelFont == source.pixelFont && back.blockBars == source.blockBars && back.sheetPrompts == source.sheetPrompts);
    CHECK(back.menuLayout == source.menuLayout && back.pagedGrids == source.pagedGrids);
    free(text);

    printf("themefile_test: %s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
