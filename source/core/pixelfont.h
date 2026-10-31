#pragma once

// The Pixel theme's font (see tools/make_pixelfont.py): fixed sizes, one atlas texture.

#define PIXELFONT_SIZE_COUNT 3

typedef struct {
    unsigned short code;   // Unicode code point.
    unsigned short x, y;   // Where it is in the atlas.
    unsigned char w, h;
    signed char xoff;      // From the pen position to the glyph's left, and from the line's top to its top.
    signed char yoff;
    unsigned char advance;
} pixelfont_glyph;

typedef struct {
    unsigned char size;
    unsigned char lineHeight;
    unsigned char ascent;
    unsigned short count;
    const pixelfont_glyph* glyphs;   // Sorted by code.
} pixelfont_size;

extern const pixelfont_size pixelfont_sizes[PIXELFONT_SIZE_COUNT];
extern const unsigned pixelfont_atlas_width;
extern const unsigned pixelfont_atlas_height;
