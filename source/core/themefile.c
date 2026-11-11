#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "themefile.h"

typedef struct {
    const char* key;
    size_t offset;
} themefile_color;

static const themefile_color colors[] = {
        {"background", offsetof(theme_palette, background)},
        {"surface", offsetof(theme_palette, surface)},
        {"border", offsetof(theme_palette, border)},
        {"text", offsetof(theme_palette, text)},
        {"textMuted", offsetof(theme_palette, textMuted)},
        {"scrim", offsetof(theme_palette, scrim)},
        {"success", offsetof(theme_palette, success)},
        {"successSoft", offsetof(theme_palette, successSoft)},
        {"danger", offsetof(theme_palette, danger)},
        {"warning", offsetof(theme_palette, warning)}
};

#define COLOR_COUNT (sizeof(colors) / sizeof(*colors))

static int themefile_hex(char c) {
    if(c >= '0' && c <= '9') {
        return c - '0';
    }

    if(c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    if(c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

// "#RRGGBB" or "#RRGGBBAA" into the renderer's 0xAABBGGRR.
static bool themefile_parse_color(const char* text, u32* out) {
    size_t len = text != NULL ? strlen(text) : 0;
    if((len != 7 && len != 9) || text[0] != '#') {
        return false;
    }

    u32 bytes[4] = {0, 0, 0, 0xFF};
    for(size_t i = 0; i < (len - 1) / 2; i++) {
        int hi = themefile_hex(text[1 + i * 2]);
        int lo = themefile_hex(text[2 + i * 2]);
        if(hi < 0 || lo < 0) {
            return false;
        }

        bytes[i] = (u32) (hi * 16 + lo);
    }

    *out = (bytes[3] << 24) | (bytes[2] << 16) | (bytes[1] << 8) | bytes[0];
    return true;
}

static bool themefile_parse_palette(json_t* object, const char* which, theme_palette* palette, char* error, size_t errorSize) {
    if(object == NULL) {
        return true;
    }

    if(!json_is_object(object)) {
        snprintf(error, errorSize, "\"%s\" is not an object", which);
        return false;
    }

    for(size_t i = 0; i < COLOR_COUNT; i++) {
        json_t* value = json_object_get(object, colors[i].key);
        if(value == NULL) {
            continue;
        }

        u32 color;
        if(!json_is_string(value) || !themefile_parse_color(json_string_value(value), &color)) {
            snprintf(error, errorSize, "%s.%s is not a colour like #RRGGBB", which, colors[i].key);
            return false;
        }

        *(u32*) ((char*) palette + colors[i].offset) = color;
    }

    return true;
}

static bool themefile_parse_number(json_t* json, const char* key, float min, float max, float* out, char* error, size_t errorSize) {
    json_t* value = json_object_get(json, key);
    if(value == NULL) {
        return true;
    }

    if(!json_is_number(value) || json_number_value(value) < min || json_number_value(value) > max) {
        snprintf(error, errorSize, "%s is not a number from %d to %d", key, (int) min, (int) max);
        return false;
    }

    *out = (float) json_number_value(value);
    return true;
}

static bool themefile_parse_bool(json_t* json, const char* key, bool* out, char* error, size_t errorSize) {
    json_t* value = json_object_get(json, key);
    if(value == NULL) {
        return true;
    }

    if(!json_is_boolean(value)) {
        snprintf(error, errorSize, "%s is not true or false", key);
        return false;
    }

    *out = json_is_true(value);
    return true;
}

// One of two words: `second` sets *out, `first` clears it.
static bool themefile_parse_choice(json_t* json, const char* key, const char* first, const char* second, bool* out, char* error,
                                   size_t errorSize) {
    json_t* value = json_object_get(json, key);
    if(value == NULL) {
        return true;
    }

    const char* text = json_is_string(value) ? json_string_value(value) : "";
    if(strcmp(text, first) == 0 || strcmp(text, second) == 0) {
        *out = strcmp(text, second) == 0;
        return true;
    }

    snprintf(error, errorSize, "%s is not \"%s\" or \"%s\"", key, first, second);
    return false;
}

bool themefile_parse(const char* text, const theme_def* bases, size_t baseCount, theme_def* out, char* name, size_t nameSize,
                     char* error, size_t errorSize) {
    if(nameSize > 0) {
        name[0] = '\0';
    }

    json_error_t jsonError;
    json_t* json = json_loads(text, 0, &jsonError);
    if(json == NULL) {
        snprintf(error, errorSize, "not JSON (line %d: %s)", jsonError.line, jsonError.text);
        return false;
    }

    bool ok = false;
    if(!json_is_object(json)) {
        snprintf(error, errorSize, "not a JSON object");
        goto done;
    }

    const theme_def* base = baseCount > 0 ? &bases[0] : NULL;
    json_t* baseId = json_object_get(json, "base");
    if(baseId != NULL) {
        base = NULL;
        for(size_t i = 0; json_is_string(baseId) && i < baseCount; i++) {
            if(strcmp(bases[i].id, json_string_value(baseId)) == 0) {
                base = &bases[i];
            }
        }

        if(base == NULL) {
            snprintf(error, errorSize, "base is not one of the built-in themes (cards, pixel, home)");
            goto done;
        }
    }

    if(base != NULL) {
        *out = *base;
    } else {
        memset(out, 0, sizeof(*out));
    }

    json_t* jsonName = json_object_get(json, "name");
    if(jsonName != NULL) {
        if(!json_is_string(jsonName)) {
            snprintf(error, errorSize, "name is not text");
            goto done;
        }

        snprintf(name, nameSize, "%s", json_string_value(jsonName));
    }

    if(!themefile_parse_palette(json_object_get(json, "day"), "day", &out->day, error, errorSize)
       || !themefile_parse_palette(json_object_get(json, "night"), "night", &out->night, error, errorSize)
       || !themefile_parse_number(json, "radius", 0, 24, &out->radius, error, errorSize)
       || !themefile_parse_number(json, "radiusSmall", 0, 24, &out->radiusSmall, error, errorSize)
       || !themefile_parse_number(json, "border", 0, 4, &out->border, error, errorSize)
       || !themefile_parse_choice(json, "font", "system", "pixel", &out->pixelFont, error, errorSize)
       || !themefile_parse_choice(json, "grid", "scroll", "pages", &out->pagedGrids, error, errorSize)
       || !themefile_parse_bool(json, "blockBars", &out->blockBars, error, errorSize)
       || !themefile_parse_bool(json, "sheetPrompts", &out->sheetPrompts, error, errorSize)) {
        goto done;
    }

    // One value for the corners: those of rows and buttons follow it, unless the file gives them too.
    if(json_object_get(json, "radius") != NULL && json_object_get(json, "radiusSmall") == NULL) {
        out->radiusSmall = theme_small_radius(out->radius);
    }

    json_t* menu = json_object_get(json, "menu");
    if(menu != NULL) {
        const char* value = json_is_string(menu) ? json_string_value(menu) : "";
        if(strcmp(value, "grid") == 0) {
            out->menuLayout = THEME_MENU_GRID2;
        } else if(strcmp(value, "tiles") == 0) {
            out->menuLayout = THEME_MENU_TILES;
        } else {
            snprintf(error, errorSize, "menu is not \"grid\" or \"tiles\"");
            goto done;
        }
    }

    ok = true;

done:
    json_decref(json);
    return ok;
}

static json_t* themefile_palette_json(const theme_palette* palette) {
    json_t* object = json_object();
    for(size_t i = 0; object != NULL && i < COLOR_COUNT; i++) {
        u32 c = *(const u32*) ((const char*) palette + colors[i].offset);
        u32 a = c >> 24;
        char text[16];
        if(a == 0xFF) {
            snprintf(text, sizeof(text), "#%02X%02X%02X", (unsigned) (c & 0xFF), (unsigned) ((c >> 8) & 0xFF), (unsigned) ((c >> 16) & 0xFF));
        } else {
            snprintf(text, sizeof(text), "#%02X%02X%02X%02X", (unsigned) (c & 0xFF), (unsigned) ((c >> 8) & 0xFF), (unsigned) ((c >> 16) & 0xFF),
                     (unsigned) a);
        }

        json_object_set_new(object, colors[i].key, json_string(text));
    }

    return object;
}

char* themefile_write(const theme_def* def, const char* name) {
    json_t* json = json_object();
    if(json == NULL) {
        return NULL;
    }

    json_object_set_new(json, "name", json_string(name));
    json_object_set_new(json, "day", themefile_palette_json(&def->day));
    json_object_set_new(json, "night", themefile_palette_json(&def->night));
    json_object_set_new(json, "radius", json_integer((json_int_t) def->radius));
    json_object_set_new(json, "radiusSmall", json_integer((json_int_t) def->radiusSmall));
    json_object_set_new(json, "border", json_integer((json_int_t) def->border));
    json_object_set_new(json, "font", json_string(def->pixelFont ? "pixel" : "system"));
    json_object_set_new(json, "blockBars", json_boolean(def->blockBars));
    json_object_set_new(json, "sheetPrompts", json_boolean(def->sheetPrompts));
    json_object_set_new(json, "menu", json_string(def->menuLayout == THEME_MENU_TILES ? "tiles" : "grid"));
    json_object_set_new(json, "grid", json_string(def->pagedGrids ? "pages" : "scroll"));

    char* text = json_dumps(json, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    json_decref(json);
    return text;
}
