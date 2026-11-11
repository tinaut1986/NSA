// The colour picker of the theme editor (themeedit.c): HSV, for a finger first.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "../core/core.h"
#include "colorpick.h"
#include "../core/ui/keys.h"

#define CP_TEX 64          // The textures' side: drawn larger, linear filtering makes them smooth gradients.
#define CP_BAR_W 22
#define CP_GAP 10

typedef enum {
    CP_NONE,
    CP_SQUARE,
    CP_HUE,
    CP_ALPHA
} cp_area;

typedef struct {
    float h, s, v;         // 0..1 each.
    u32 alpha;             // 0..255.
    bool hasAlpha;
    u32 original;

    u32 squareTexture;
    u32 hueTexture;
    u32 alphaTexture;
    float squareHue;       // The hue the square was made for.

    cp_area dragging;
    int pressedButton;
    u64 holdAt[6];

    void* data;
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2);
    void (*changed)(void* data, u32 color);
    void (*done)(void* data, bool accepted, u32 color);
} colorpick_data;

typedef struct {
    float sqX, sqY, sqW, sqH;
    float hueX, alphaX;
    float panelX, panelW;
} cp_layout;

static void cp_hsv_to_rgb(float h, float s, float v, u8* r, u8* g, u8* b) {
    float i = floorf(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    float rf, gf, bf;
    switch(((int) i) % 6) {
        case 0: rf = v; gf = t; bf = p; break;
        case 1: rf = q; gf = v; bf = p; break;
        case 2: rf = p; gf = v; bf = t; break;
        case 3: rf = p; gf = q; bf = v; break;
        case 4: rf = t; gf = p; bf = v; break;
        default: rf = v; gf = p; bf = q; break;
    }

    *r = (u8) (rf * 255 + 0.5f);
    *g = (u8) (gf * 255 + 0.5f);
    *b = (u8) (bf * 255 + 0.5f);
}

static void cp_rgb_to_hsv(u8 r, u8 g, u8 b, float* h, float* s, float* v) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float max = fmaxf(rf, fmaxf(gf, bf));
    float min = fminf(rf, fminf(gf, bf));
    float d = max - min;
    *v = max;
    *s = max > 0 ? d / max : 0;
    if(d <= 0) {
        // Grey: keep the hue it had, so the square does not jump to red.
        return;
    }

    float hue;
    if(max == rf) {
        hue = (gf - bf) / d + (gf < bf ? 6 : 0);
    } else if(max == gf) {
        hue = (bf - rf) / d + 2;
    } else {
        hue = (rf - gf) / d + 4;
    }

    *h = hue / 6;
}

static u32 cp_color(colorpick_data* d) {
    u8 r, g, b;
    cp_hsv_to_rgb(d->h, d->s, d->v, &r, &g, &b);
    return ((d->hasAlpha ? d->alpha : 0xFF) << 24) | ((u32) b << 16) | ((u32) g << 8) | r;
}

// A texture of CP_TEX x CP_TEX texels from `texel` (x, y -> 0xAABBGGRR); the GPU wants A, B, G, R in memory.
static void cp_upload(u32* id, colorpick_data* d, u32 (*texel)(colorpick_data* d, u32 x, u32 y)) {
    u8* texels = (u8*) malloc(CP_TEX * CP_TEX * 4);
    if(texels == NULL) {
        return;
    }

    for(u32 y = 0; y < CP_TEX; y++) {
        for(u32 x = 0; x < CP_TEX; x++) {
            u32 c = texel(d, x, y);
            u8* p = texels + (y * CP_TEX + x) * 4;
            p[0] = (u8) (c >> 24);
            p[1] = (u8) (c >> 16);
            p[2] = (u8) (c >> 8);
            p[3] = (u8) c;
        }
    }

    if(*id == 0) {
        *id = screen_allocate_free_texture();
    }

    screen_load_texture_untiled(*id, texels, CP_TEX * CP_TEX * 4, CP_TEX, CP_TEX, GPU_RGBA8, true);
    free(texels);
}

static u32 cp_square_texel(colorpick_data* d, u32 x, u32 y) {
    u8 r, g, b;
    cp_hsv_to_rgb(d->h, x / (float) (CP_TEX - 1), 1 - y / (float) (CP_TEX - 1), &r, &g, &b);
    return 0xFF000000u | ((u32) b << 16) | ((u32) g << 8) | r;
}

static u32 cp_hue_texel(colorpick_data* d, u32 x, u32 y) {
    u8 r, g, b;
    cp_hsv_to_rgb(y / (float) CP_TEX, 1, 1, &r, &g, &b);
    return 0xFF000000u | ((u32) b << 16) | ((u32) g << 8) | r;
}

static u32 cp_alpha_texel(colorpick_data* d, u32 x, u32 y) {
    u32 rgb = cp_color(d) & 0x00FFFFFF;
    return ((u32) ((1 - y / (float) (CP_TEX - 1)) * 255) << 24) | rgb;
}

static void cp_refresh(colorpick_data* d, bool notify) {
    if(d->squareTexture == 0 || d->squareHue != d->h) {
        d->squareHue = d->h;
        cp_upload(&d->squareTexture, d, cp_square_texel);
    }

    if(d->hasAlpha) {
        cp_upload(&d->alphaTexture, d, cp_alpha_texel);
    }

    if(notify && d->changed != NULL) {
        d->changed(d->data, cp_color(d));
    }
}

static void cp_set(colorpick_data* d, u32 color) {
    cp_rgb_to_hsv((u8) color, (u8) (color >> 8), (u8) (color >> 16), &d->h, &d->s, &d->v);
    d->alpha = color >> 24;
}

static cp_layout cp_get_layout(colorpick_data* d, float x1, float y1, float x2) {
    cp_layout l;
    float bx, by, bw, bh;
    widget_button_row_rect(0, 2, &bx, &by, &bw, &bh);
    l.sqX = x1 + 12;
    l.sqY = y1 + 10;
    l.sqH = by - 10 - l.sqY;
    l.sqW = 150;
    l.hueX = l.sqX + l.sqW + CP_GAP;
    l.alphaX = l.hueX + CP_BAR_W + CP_GAP;
    l.panelX = (d->hasAlpha ? l.alphaX + CP_BAR_W : l.hueX + CP_BAR_W) + CP_GAP;
    l.panelW = x2 - 12 - l.panelX;
    return l;
}

static float cp_clamp(float v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static void cp_finish(ui_view* view, colorpick_data* d, bool accepted) {
    u32 color = accepted ? cp_color(d) : d->original;
    ui_pop();
    if(d->squareTexture != 0) {
        screen_unload_texture(d->squareTexture);
    }

    if(d->hueTexture != 0) {
        screen_unload_texture(d->hueTexture);
    }

    if(d->alphaTexture != 0) {
        screen_unload_texture(d->alphaTexture);
    }

    if(d->done != NULL) {
        d->done(d->data, accepted, color);
    }

    free(d);
    ui_destroy(view);
}

static void cp_hex_typed(ui_view* kbd, void* data, SwkbdButton button, const char* response) {
    colorpick_data* d = (colorpick_data*) data;
    if(button != SWKBD_BUTTON_CONFIRM || response == NULL) {
        return;
    }

    const char* text = response[0] == '#' ? response + 1 : response;
    size_t len = strlen(text);
    if(len != 6 && len != 8) {
        return;
    }

    u32 bytes[4] = {0, 0, 0, d->hasAlpha ? d->alpha : 0xFF};
    for(size_t i = 0; i < len / 2; i++) {
        unsigned value;
        char pair[3] = {text[i * 2], text[i * 2 + 1], '\0'};
        if(sscanf(pair, "%2x", &value) != 1 || strspn(pair, "0123456789abcdefABCDEF") != 2) {
            return;
        }

        bytes[i] = value;
    }

    cp_set(d, (bytes[3] << 24) | (bytes[2] << 16) | (bytes[1] << 8) | bytes[0]);
    if(!d->hasAlpha) {
        d->alpha = 0xFF;
    }

    cp_refresh(d, true);
}

static void cp_hex(colorpick_data* d, u32 color, char* out, size_t size) {
    if(d->hasAlpha) {
        snprintf(out, size, "#%02X%02X%02X%02X", (unsigned) (color & 0xFF), (unsigned) ((color >> 8) & 0xFF), (unsigned) ((color >> 16) & 0xFF),
                 (unsigned) (color >> 24));
    } else {
        snprintf(out, size, "#%02X%02X%02X", (unsigned) (color & 0xFF), (unsigned) ((color >> 8) & 0xFF), (unsigned) ((color >> 16) & 0xFF));
    }
}

static void cp_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    colorpick_data* d = (colorpick_data*) data;
    if(d->drawTop != NULL) {
        d->drawTop(view, d->data, x1, y1, x2, y2);
    }
}

// A checkerboard under what may be see-through.
static void cp_checker(float x, float y, float w, float h) {
    const theme_current* t = theme();
    const float cell = 6;
    screen_fill_rect(x, y, w, h, t->palette->surface);
    for(float cy = 0; cy < h; cy += cell) {
        for(float cx = ((int) (cy / cell) % 2) * cell; cx < w; cx += 2 * cell) {
            screen_fill_rect(x + cx, y + cy, fminf(cell, w - cx), fminf(cell, h - cy), t->palette->border);
        }
    }
}

static void cp_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    colorpick_data* d = (colorpick_data*) data;
    const theme_current* t = theme();
    cp_layout l = cp_get_layout(d, x1, y1, x2);
    u32 color = cp_color(d);

    // The square, with a ring where the colour is.
    screen_frame_round_rect(l.sqX - 1, l.sqY - 1, l.sqW + 2, l.sqH + 2, 2, 1, t->palette->border, t->palette->border);
    screen_draw_texture(d->squareTexture, l.sqX, l.sqY, l.sqW, l.sqH);
    float rx = l.sqX + d->s * l.sqW;
    float ry = l.sqY + (1 - d->v) * l.sqH;
    screen_fill_circle(rx, ry, 7, t->palette->surface);
    screen_fill_circle(rx, ry, 5, t->palette->text);
    screen_fill_circle(rx, ry, 4, color | 0xFF000000u);

    // Where a guided tour points (guide.c, "Your own themes").
    coach_anchor("colorpick.square", l.sqX, l.sqY, l.sqW, l.sqH, false);
    coach_anchor("colorpick.hue", l.hueX - 2, l.sqY, CP_BAR_W + 4, l.sqH, false);

    // The hues, and a bar across where it is.
    screen_frame_round_rect(l.hueX - 1, l.sqY - 1, CP_BAR_W + 2, l.sqH + 2, 2, 1, t->palette->border, t->palette->border);
    screen_draw_texture(d->hueTexture, l.hueX, l.sqY, CP_BAR_W, l.sqH);
    float hy = l.sqY + d->h * l.sqH;
    screen_fill_rect(l.hueX - 3, hy - 2, CP_BAR_W + 6, 4, t->palette->surface);
    screen_fill_rect(l.hueX - 3, hy - 1, CP_BAR_W + 6, 2, t->palette->text);

    if(d->hasAlpha) {
        cp_checker(l.alphaX, l.sqY, CP_BAR_W, l.sqH);
        screen_draw_texture(d->alphaTexture, l.alphaX, l.sqY, CP_BAR_W, l.sqH);
        float ay = l.sqY + (1 - d->alpha / 255.0f) * l.sqH;
        screen_fill_rect(l.alphaX - 3, ay - 2, CP_BAR_W + 6, 4, t->palette->surface);
        screen_fill_rect(l.alphaX - 3, ay - 1, CP_BAR_W + 6, 2, t->palette->text);
    }

    // New over old, and the new one's code.
    float half = (l.sqH - 20) / 2;
    screen_fill_rect(l.panelX - 1, l.sqY - 1, l.panelW + 2, half * 2 + 2, t->palette->border);
    cp_checker(l.panelX, l.sqY, l.panelW, half * 2);
    screen_fill_rect(l.panelX, l.sqY, l.panelW, half, color);
    screen_fill_rect(l.panelX, l.sqY + half, l.panelW, half, d->original);
    char hex[16];
    cp_hex(d, color, hex, sizeof(hex));
    widget_text_centered(hex, l.panelX - 6, l.panelX + l.panelW + 6, l.sqY + half * 2 + 6, WIDGET_SMALL_SCALE, t->palette->text);

    widget_row_button(0, 2, ICON_X, _("Cancel"), d->pressedButton == 0, false, false);
    widget_row_button(1, 2, ICON_CHECK, _("OK"), d->pressedButton == 1, false, true);
}

static void cp_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    colorpick_data* d = (colorpick_data*) data;
    view->info = _(" hue    type    ok    cancel");

    if(hidKeysDown() & KEY_B) {
        cp_finish(view, d, false);
        return;
    }

    if(hidKeysDown() & KEY_A) {
        cp_finish(view, d, true);
        return;
    }

    if(hidKeysDown() & KEY_X) {
        char hex[16];
        cp_hex(d, cp_color(d), hex, sizeof(hex));
        osk_display(_("A colour, as #RRGGBB"), hex, 9, 0, d, cp_hex_typed);
        return;
    }

    bool moved = false;
    const float step = 1.0f / 64;
    if(keys_hold_repeat(&d->holdAt[0], KEY_LEFT)) { d->s = cp_clamp(d->s - step); moved = true; }
    if(keys_hold_repeat(&d->holdAt[1], KEY_RIGHT)) { d->s = cp_clamp(d->s + step); moved = true; }
    if(keys_hold_repeat(&d->holdAt[2], KEY_UP)) { d->v = cp_clamp(d->v + step); moved = true; }
    if(keys_hold_repeat(&d->holdAt[3], KEY_DOWN)) { d->v = cp_clamp(d->v - step); moved = true; }
    if(keys_hold_repeat(&d->holdAt[4], KEY_L)) { d->h = d->h - 1.0f / 128 < 0 ? d->h - 1.0f / 128 + 1 : d->h - 1.0f / 128; moved = true; }
    if(keys_hold_repeat(&d->holdAt[5], KEY_R)) { d->h = d->h + 1.0f / 128 >= 1 ? d->h + 1.0f / 128 - 1 : d->h + 1.0f / 128; moved = true; }

    touchPosition pos;
    hidTouchRead(&pos);
    cp_layout l = cp_get_layout(d, bx1, by1, bx2);
    if(hidKeysDown() & KEY_TOUCH) {
        d->dragging = CP_NONE;
        d->pressedButton = widget_button_row_hit(&pos, 2);
        // A little room around each, for a finger.
        if(d->pressedButton < 0 && pos.py >= l.sqY - 6 && pos.py < l.sqY + l.sqH + 6) {
            if(pos.px >= l.sqX - 6 && pos.px < l.sqX + l.sqW + 4) {
                d->dragging = CP_SQUARE;
            } else if(pos.px >= l.hueX - 5 && pos.px < l.hueX + CP_BAR_W + 5) {
                d->dragging = CP_HUE;
            } else if(d->hasAlpha && pos.px >= l.alphaX - 5 && pos.px < l.alphaX + CP_BAR_W + 5) {
                d->dragging = CP_ALPHA;
            }
        }
    }

    if(hidKeysHeld() & KEY_TOUCH) {
        if(d->dragging == CP_SQUARE) {
            d->s = cp_clamp((pos.px - l.sqX) / l.sqW);
            d->v = 1 - cp_clamp((pos.py - l.sqY) / l.sqH);
            moved = true;
        } else if(d->dragging == CP_HUE) {
            float h = cp_clamp((pos.py - l.sqY) / l.sqH);
            d->h = h >= 1 ? 0.999f : h;
            moved = true;
        } else if(d->dragging == CP_ALPHA) {
            d->alpha = (u32) ((1 - cp_clamp((pos.py - l.sqY) / l.sqH)) * 255 + 0.5f);
            moved = true;
        } else if(d->pressedButton != -1) {
            widget_button_row_hold(&d->pressedButton, &pos, 2);
        }
    } else if(hidKeysUp() & KEY_TOUCH) {
        int button = d->pressedButton;
        d->pressedButton = -1;
        d->dragging = CP_NONE;
        if(button == 0 || button == 1) {
            cp_finish(view, d, button == 1);
            return;
        }
    }

    if(moved) {
        cp_refresh(d, true);
    }
}

void colorpick_open(const char* title, u32 color, bool alpha, void* data,
                    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2),
                    void (*changed)(void* data, u32 color), void (*done)(void* data, bool accepted, u32 color)) {
    colorpick_data* d = (colorpick_data*) calloc(1, sizeof(colorpick_data));
    if(d == NULL) {
        error_display(NULL, NULL, "Failed to allocate colour picker data.");
        return;
    }

    d->hasAlpha = alpha;
    d->original = alpha ? color : color | 0xFF000000u;
    cp_set(d, d->original);
    d->pressedButton = -1;
    d->data = data;
    d->drawTop = drawTop;
    d->changed = changed;
    d->done = done;
    cp_upload(&d->hueTexture, d, cp_hue_texel);
    cp_refresh(d, false);

    ui_view* view = ui_create();
    view->name = title;
    view->info = "";
    view->data = d;
    view->backArrow = true;
    view->update = cp_update;
    view->drawTop = cp_draw_top;
    view->drawBottom = cp_draw_bottom;
    ui_push(view);
}
