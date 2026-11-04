#pragma once

// The font for Simplified Chinese (see tools/make_hanfont.py): one size, one atlas texture, drawn by
// screen.c in place of the system font while that language is selected.

typedef struct {
    unsigned short code;   // Unicode code point.
    unsigned short x, y;   // Where it is in the atlas.
    unsigned char w, h;
    signed char xoff;      // From the pen position to the glyph's left, and from the baseline to its top.
    signed char yoff;
    unsigned char advance;
} hanfont_glyph;

extern const hanfont_glyph hanfont_glyphs[];   // Sorted by code.
extern const unsigned hanfont_count;
extern const unsigned hanfont_atlas_width;
extern const unsigned hanfont_atlas_height;
extern const unsigned hanfont_line;            // The line feed the glyphs are made for.
