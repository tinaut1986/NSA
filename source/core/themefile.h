#pragma once

// Themes from the SD card (sdmc:/nsa/themes/<name>.json): data only, read on top of a built-in theme.
//
//   {
//     "name": "Ocean",                 // As it shows in Settings; the file's name when missing.
//     "base": "cards",                 // The built-in theme that gives what the file leaves out (cards by default).
//     "day":   {"background": "#EEF2F1", "surface": "#FFFFFF", "border": "#D5DEDB", "text": "#1E2B28",
//               "textMuted": "#5F6B67", "scrim": "#1E2B2870", "success": "#0F6E56", "successSoft": "#E1F5EE",
//               "danger": "#A32D2D", "warning": "#854F0B"},
//     "night": { ... the same keys ... },
//     "radius": 10, "radiusSmall": 8, "border": 1,   // Corners and outline, in pixels (0..24, 0..4).
//     "pixelFont": false, "blockBars": false, "sheetPrompts": false,
//     "menu": "grid"                   // Or "tiles": the main menu as big icon tiles (HOME).
//   }
//
// Colours are "#RRGGBB" or "#RRGGBBAA". Plain C with Jansson, so the host tests build it too.

#include <stdbool.h>
#include <stddef.h>

#if defined(_3DS) || defined(__3DS__)
#include <3ds.h>
#else
#include <stdint.h>
typedef uint32_t u32;
#endif

#include "theme.h"

// Reads `text` into `out`: first the built-in theme named by "base" (one of `bases`, the first when missing), then
// what the file says. `name` gets its "name" (empty when it has none). False, with a reason in English in `error`,
// when it is not JSON or a value is wrong; unknown keys are left alone. `out`'s id and name are the caller's.
bool themefile_parse(const char* text, const theme_def* bases, size_t baseCount, theme_def* out, char* name, size_t nameSize,
                     char* error, size_t errorSize);

// The JSON of `def` with every key, named `name` (malloc'd; NULL when out of memory).
char* themefile_write(const theme_def* def, const char* name);
