#pragma once

// Picks a colour (0xAABBGGRR, as the theme keeps them) on the bottom screen: a square of saturation and brightness, a
// bar of hues and, with `alpha`, a bar of opacity, all for a finger; the D-Pad moves in the square, L and R through the
// hues, X types "#RRGGBB". `changed` runs on every change (to preview it), `done` once, when it is accepted (A or OK) or
// not (B or Cancel). `drawTop` draws the top screen meanwhile.
void colorpick_open(const char* title, u32 color, bool alpha, void* data,
                    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2),
                    void (*changed)(void* data, u32 color), void (*done)(void* data, bool accepted, u32 color));
