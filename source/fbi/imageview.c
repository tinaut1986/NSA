// Picture viewer: top screen the picture (in 3D for a camera's MPO), bottom screen its
// details and buttons; drag or D-Pad to move, A to zoom, L and R for the folder's others.

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "viewers.h"
#include "../core/core.h"
#include "../libs/stb_image/stb_image.h"

#define IMAGE_MAX_FILE (12 * 1024 * 1024)
#define IMAGE_MAX_PIXELS 5000000
#define IMAGE_MAX_TEXTURE 1024

static const float image_zooms[] = {0, 1, 2, 4};   // 0 is "fit".
#define IMAGE_ZOOM_COUNT 4

enum { IMAGE_PENDING, IMAGE_READY, IMAGE_FAILED };

typedef struct {
    FS_Archive archive;
    char** paths;
    u32 count;
    u32 index;

    ui_view* view;
    char name[FILE_NAME_MAX];

    int state;
    u32 delay;
    char error[96];

    bool hasTexture[2];
    u32 texture[2];
    bool stereo;
    u32 width[2];
    u32 height[2];
    u32 sourceWidth;
    u32 sourceHeight;
    u64 fileSize;

    u32 zoom;
    float panX;
    float panY;

    int pressed;       // The button a finger went down on, or -1.
    bool dragging;
    int lastX;
    int lastY;
} image_data;

#define IMAGE_BUTTONS 5

static void image_free_textures(image_data* data) {
    for(int i = 0; i < 2; i++) {
        if(data->hasTexture[i]) {
            screen_unload_texture(data->texture[i]);
            data->hasTexture[i] = false;
        }
    }

    data->stereo = false;
}

static u16 image_be16(const u8* p) { return (u16) ((p[0] << 8) | p[1]); }

static u32 image_rd32(const u8* p, bool little) {
    return little ? (u32) (p[0] | (p[1] << 8) | (p[2] << 16) | ((u32) p[3] << 24)) : (u32) (((u32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

static u16 image_rd16(const u8* p, bool little) {
    return little ? (u16) (p[0] | (p[1] << 8)) : image_be16(p);
}

// A camera's MPO is two JPEGs one after the other; its "MP Index" says where the second starts.
static bool image_mpo_second(const u8* d, size_t n, size_t* offset, size_t* length) {
    size_t p = 2;
    while(p + 4 <= n && d[p] == 0xFF) {
        u8 marker = d[p + 1];
        if(marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            p += 2;
            continue;
        }

        if(marker == 0xDA || marker == 0xD9) {
            break;
        }

        size_t len = image_be16(d + p + 2);
        if(marker == 0xE2 && len >= 8 && p + 2 + len <= n && memcmp(d + p + 4, "MPF\0", 4) == 0) {
            size_t base = p + 8;
            if(base + 8 > n) {
                return false;
            }

            bool little = d[base] == 'I';
            size_t ifd = base + image_rd32(d + base + 4, little);
            if(ifd + 2 > n) {
                return false;
            }

            u16 entries = image_rd16(d + ifd, little);
            for(u16 i = 0; i < entries; i++) {
                size_t e = ifd + 2 + (size_t) i * 12;
                if(e + 12 > n) {
                    return false;
                }

                if(image_rd16(d + e, little) == 0xB002) {
                    u32 count = image_rd32(d + e + 4, little) / 16;
                    size_t list = base + image_rd32(d + e + 8, little);
                    if(count < 2 || list + 32 > n) {
                        return false;
                    }

                    size_t size = image_rd32(d + list + 16 + 4, little);
                    size_t off = base + image_rd32(d + list + 16 + 8, little);
                    if(off + 4 > n || d[off] != 0xFF || d[off + 1] != 0xD8) {
                        return false;
                    }

                    *offset = off;
                    *length = off + size <= n ? size : n - off;
                    return true;
                }
            }

            return false;
        }

        p += 2 + len;
    }

    return false;
}

// RGBA of the picture, shrunk to fit a texture (box filter); NULL with `error` filled on failure.
static u8* image_decode(const u8* data, size_t size, u32* outW, u32* outH, u32* sourceW, u32* sourceH, char* error, size_t errorSize) {
    int w = 0;
    int h = 0;
    int comp = 0;
    if(!stbi_info_from_memory(data, (int) size, &w, &h, &comp)) {
        snprintf(error, errorSize, "%s", _("This picture cannot be read."));
        return NULL;
    }

    if(w <= 0 || h <= 0 || (u64) w * (u64) h > IMAGE_MAX_PIXELS) {
        snprintf(error, errorSize, "%s", _("Too large (5 megapixels at most)."));
        return NULL;
    }

    u8* pixels = stbi_load_from_memory(data, (int) size, &w, &h, &comp, 4);
    if(pixels == NULL) {
        snprintf(error, errorSize, "%s", _("This picture cannot be read."));
        return NULL;
    }

    *sourceW = (u32) w;
    *sourceH = (u32) h;

    if(w <= IMAGE_MAX_TEXTURE && h <= IMAGE_MAX_TEXTURE) {
        *outW = (u32) w;
        *outH = (u32) h;
        return pixels;
    }

    float f = (float) IMAGE_MAX_TEXTURE / (float) (w > h ? w : h);
    int dw = (int) (w * f);
    int dh = (int) (h * f);
    dw = dw < 1 ? 1 : dw;
    dh = dh < 1 ? 1 : dh;

    u8* out = (u8*) malloc((size_t) dw * dh * 4);
    if(out == NULL) {
        free(pixels);
        snprintf(error, errorSize, "%s", _("Not enough memory for this picture."));
        return NULL;
    }

    for(int dy = 0; dy < dh; dy++) {
        int y0 = (int) ((long long) dy * h / dh);
        int y1 = (int) ((long long) (dy + 1) * h / dh);
        y1 = y1 <= y0 ? y0 + 1 : y1;
        for(int dx = 0; dx < dw; dx++) {
            int x0 = (int) ((long long) dx * w / dw);
            int x1 = (int) ((long long) (dx + 1) * w / dw);
            x1 = x1 <= x0 ? x0 + 1 : x1;

            u32 sum[4] = {0, 0, 0, 0};
            for(int y = y0; y < y1; y++) {
                const u8* row = pixels + ((size_t) y * w + x0) * 4;
                for(int x = x0; x < x1; x++, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                }
            }

            u32 n = (u32) ((y1 - y0) * (x1 - x0));
            u8* dst = out + ((size_t) dy * dw + dx) * 4;
            for(int c = 0; c < 4; c++) {
                dst[c] = (u8) (sum[c] / n);
            }
        }
    }

    free(pixels);
    *outW = (u32) dw;
    *outH = (u32) dh;
    return out;
}

static bool image_upload(image_data* data, int slot, u8* pixels, u32 w, u32 h) {
    // The GPU wants the bytes the other way round (A, B, G, R).
    for(u32 i = 0; i < w * h; i++) {
        u8* px = pixels + (size_t) i * 4;
        u8 r = px[0];
        u8 g = px[1];
        px[0] = px[3];
        px[1] = px[2];
        px[2] = g;
        px[3] = r;
    }

    u32 id = screen_allocate_free_texture();
    screen_load_texture_untiled(id, pixels, w * h * 4, w, h, GPU_RGBA8, true);
    free(pixels);

    data->texture[slot] = id;
    data->hasTexture[slot] = true;
    data->width[slot] = w;
    data->height[slot] = h;
    return true;
}

static void image_load(image_data* data) {
    image_free_textures(data);
    data->error[0] = '\0';
    data->zoom = 0;
    data->panX = 0;
    data->panY = 0;

    const char* path = data->paths[data->index];
    string_get_path_file(data->name, path, sizeof(data->name));

    u8* file = NULL;
    u32 size = 0;
    u64 fullSize = 0;
    Result res = viewers_read_file(data->archive, path, IMAGE_MAX_FILE, &file, &size, &fullSize);
    if(R_FAILED(res)) {
        snprintf(data->error, sizeof(data->error), "%s", _("The file cannot be read."));
        data->state = IMAGE_FAILED;
        return;
    }

    data->fileSize = fullSize;
    if(fullSize > size) {
        free(file);
        snprintf(data->error, sizeof(data->error), "%s", _("Too large (12 MB at most)."));
        data->state = IMAGE_FAILED;
        return;
    }

    u32 w = 0;
    u32 h = 0;
    u8* left = image_decode(file, size, &w, &h, &data->sourceWidth, &data->sourceHeight, data->error, sizeof(data->error));
    if(left == NULL) {
        free(file);
        data->state = IMAGE_FAILED;
        return;
    }

    image_upload(data, 0, left, w, h);

    size_t offset = 0;
    size_t length = 0;
    if(size > 4 && file[0] == 0xFF && file[1] == 0xD8 && image_mpo_second(file, size, &offset, &length)) {
        char ignored[8];
        u32 sw = 0;
        u32 sh = 0;
        u32 srcW = 0;
        u32 srcH = 0;
        u8* right = image_decode(file + offset, length, &sw, &sh, &srcW, &srcH, ignored, sizeof(ignored));
        if(right != NULL) {
            image_upload(data, 1, right, sw, sh);
            data->stereo = true;
        }
    }

    free(file);
    data->state = IMAGE_READY;
}

static void image_go(image_data* data, int step) {
    if(data->count < 2) {
        return;
    }

    data->index = (data->index + data->count + (u32) step) % data->count;
    image_free_textures(data);
    string_get_path_file(data->name, data->paths[data->index], sizeof(data->name));
    data->state = IMAGE_PENDING;
    data->delay = 2;
}

static void image_close(ui_view* view, image_data* data) {
    ui_pop();
    image_free_textures(data);
    for(u32 i = 0; i < data->count; i++) {
        free(data->paths[i]);
    }

    free(data->paths);
    free(data);
    ui_destroy(view);
}

// Where the picture is drawn in the top screen's viewport, `slot` being the eye's picture.
static void image_layout(image_data* data, int slot, float vw, float vh, float* x, float* y, float* w, float* h) {
    float tw = (float) data->width[slot];
    float th = (float) data->height[slot];
    float scale = data->zoom == 0 ? (vw / tw < vh / th ? vw / tw : vh / th) : image_zooms[data->zoom];

    *w = tw * scale;
    *h = th * scale;

    float maxX = *w > vw ? (*w - vw) / 2 : 0;
    float maxY = *h > vh ? (*h - vh) / 2 : 0;
    float px = data->panX < -maxX ? -maxX : data->panX > maxX ? maxX : data->panX;
    float py = data->panY < -maxY ? -maxY : data->panY > maxY ? maxY : data->panY;
    *x = (vw - *w) / 2 + px;
    *y = (vh - *h) / 2 + py;
}

static void image_clamp_pan(image_data* data) {
    float x, y, w, h;
    image_layout(data, 0, TOP_SCREEN_WIDTH, TOP_SCREEN_HEIGHT - WIDGET_TOP_BAR_HEIGHT, &x, &y, &w, &h);
    float maxX = w > TOP_SCREEN_WIDTH ? (w - TOP_SCREEN_WIDTH) / 2 : 0;
    float maxY = h > TOP_SCREEN_HEIGHT - WIDGET_TOP_BAR_HEIGHT ? (h - (TOP_SCREEN_HEIGHT - WIDGET_TOP_BAR_HEIGHT)) / 2 : 0;
    data->panX = data->panX < -maxX ? -maxX : data->panX > maxX ? maxX : data->panX;
    data->panY = data->panY < -maxY ? -maxY : data->panY > maxY ? maxY : data->panY;
}

static void image_draw_top(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    image_data* data = (image_data*) v;
    const theme_current* t = theme();

    float vw = x2 - x1;
    float vh = y2 - y1;
    screen_fill_rect(x1, y1, vw, vh, 0xFF101010);

    if(data->state != IMAGE_READY) {
        widget_text_centered(data->state == IMAGE_FAILED ? data->error : _("Loading..."), x1, x2, y1 + (vh - widget_text_height(WIDGET_TEXT_SCALE)) / 2,
                             WIDGET_TEXT_SCALE, data->state == IMAGE_FAILED ? t->palette->danger : 0xFFFFFFFF);
        return;
    }

    // The picture sits on the screen's plane: a pair already has its own depth.
    screen_set_depth(0);
    int slot = data->stereo && screen_get_eye() == GFX_RIGHT ? 1 : 0;

    float x, y, w, h;
    image_layout(data, slot, vw, vh, &x, &y, &w, &h);
    screen_set_clip(x1, y1, vw, vh);
    screen_draw_texture(data->texture[slot], x1 + x, y1 + y, w, h);
    screen_clear_clip();
}

static void image_button_rect(int i, float* x, float* y, float* w, float* h) {
    float gap = 6;
    *w = (BOTTOM_SCREEN_WIDTH - 16 - gap * (IMAGE_BUTTONS - 1)) / IMAGE_BUTTONS;
    *h = 40;
    *x = 8 + i * (*w + gap);
    *y = BOTTOM_SCREEN_HEIGHT - WIDGET_HINT_BAR_HEIGHT - 8 - *h;
}

static const u32 image_button_icons[IMAGE_BUTTONS] = {ICON_CHEVRON_LEFT, ICON_ZOOM_OUT, ICON_ARROWS_MAXIMIZE, ICON_ZOOM_IN, ICON_CHEVRON_RIGHT};

static void image_zoom_to(image_data* data, int zoom) {
    if(zoom < 0) {
        zoom = 0;
    }

    if(zoom >= IMAGE_ZOOM_COUNT) {
        zoom = IMAGE_ZOOM_COUNT - 1;
    }

    data->zoom = (u32) zoom;
    image_clamp_pan(data);
}

static void image_press(image_data* data, int button) {
    switch(button) {
        case 0: image_go(data, -1); break;
        case 1: image_zoom_to(data, (int) data->zoom - 1); break;
        case 2: data->zoom = 0; data->panX = 0; data->panY = 0; break;
        case 3: image_zoom_to(data, (int) data->zoom + 1); break;
        case 4: image_go(data, 1); break;
        default: break;
    }
}

static void image_draw_bottom(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    image_data* data = (image_data*) v;
    const theme_current* t = theme();

    widget_card(8, y1 + 8, x2 - x1 - 16, 84);
    float tx = 18;
    float ty = y1 + 16;
    widget_text_fit(data->name, tx, ty, WIDGET_TEXT_SCALE, t->palette->text, x2 - tx - 18);
    ty += widget_text_height(WIDGET_TEXT_SCALE) + 4;

    char line[128];
    if(data->state == IMAGE_READY) {
        char size[32];
        snprintf(size, sizeof(size), data->fileSize < 1024 ? "%.0f %s" : "%.1f %s", ui_get_display_size(data->fileSize), ui_get_display_size_units(data->fileSize));
        snprintf(line, sizeof(line), "%lu x %lu   %s", data->sourceWidth, data->sourceHeight, size);
        widget_text_fit(line, tx, ty, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - tx - 18);
        ty += widget_text_height(WIDGET_SMALL_SCALE) + 3;

        const char* zoom = data->zoom == 0 ? _("Fit") : data->zoom == 1 ? "100%" : data->zoom == 2 ? "200%" : "400%";
        snprintf(line, sizeof(line), "%s: %s", _("Zoom"), zoom);
        widget_text_fit(line, tx, ty, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - tx - 18);
        ty += widget_text_height(WIDGET_SMALL_SCALE) + 3;

        if(data->stereo) {
            widget_badge(tx, ty, _("3D picture: raise the 3D slider"), t->accentSoft, t->accent);
        }
    } else if(data->state == IMAGE_FAILED) {
        widget_text_fit(data->error, tx, ty, WIDGET_SMALL_SCALE, t->palette->danger, x2 - tx - 18);
    }

    if(data->count > 1) {
        snprintf(line, sizeof(line), "%lu / %lu", data->index + 1, data->count);
        widget_text_centered(line, x1, x2, y2 - 8 - 40 - 8 - widget_text_height(WIDGET_SMALL_SCALE), WIDGET_SMALL_SCALE, t->palette->textMuted);
    }

    for(int i = 0; i < IMAGE_BUTTONS; i++) {
        float x, y, w, h;
        image_button_rect(i, &x, &y, &w, &h);
        bool pressed = data->pressed == i;
        bool disabled = (i == 0 || i == 4) && data->count < 2;
        u32 fill = pressed ? t->accent : t->palette->surface;
        u32 edge = pressed ? t->accent : t->palette->border;
        screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, edge, fill);
        screen_draw_icon(image_button_icons[i], x + (w - 16) / 2, y + (h - 16) / 2, 16, pressed ? t->onAccent : disabled ? t->palette->textMuted : t->palette->text);
    }
}

static void image_update(ui_view* view, void* v, float bx1, float by1, float bx2, float by2) {
    image_data* data = (image_data*) v;

    view->name = data->name;
    view->info = _(" zoom     others    back");

    if(hidKeysDown() & KEY_B) {
        image_close(view, data);
        return;
    }

    if(data->state == IMAGE_PENDING) {
        if(data->delay > 0) {
            data->delay--;
        } else {
            image_load(data);
        }
    }

    if(hidKeysDown() & KEY_L) {
        image_go(data, -1);
    }

    if(hidKeysDown() & KEY_R) {
        image_go(data, 1);
    }

    if(data->state == IMAGE_READY) {
        if(hidKeysDown() & KEY_A) {
            if(data->zoom + 1 >= IMAGE_ZOOM_COUNT) {
                data->zoom = 0;
                data->panX = 0;
                data->panY = 0;
            } else {
                image_zoom_to(data, (int) data->zoom + 1);
            }
        }

        if(hidKeysDown() & KEY_X) {
            data->zoom = 0;
            data->panX = 0;
            data->panY = 0;
        }

        u32 held = hidKeysHeld();
        float step = data->zoom == 0 ? 0 : 5;
        if(held & (KEY_DLEFT | KEY_CPAD_LEFT)) data->panX += step;
        if(held & (KEY_DRIGHT | KEY_CPAD_RIGHT)) data->panX -= step;
        if(held & (KEY_DUP | KEY_CPAD_UP)) data->panY += step;
        if(held & (KEY_DDOWN | KEY_CPAD_DOWN)) data->panY -= step;
        image_clamp_pan(data);
    }

    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        data->pressed = -1;
        data->dragging = false;
        for(int i = 0; i < IMAGE_BUTTONS; i++) {
            float x, y, w, h;
            image_button_rect(i, &x, &y, &w, &h);
            if(widget_touched(&pos, x, y, w, h)) {
                data->pressed = i;
            }
        }

        data->dragging = data->pressed < 0;
        data->lastX = pos.px;
        data->lastY = pos.py;
    } else if(hidKeysHeld() & KEY_TOUCH) {
        if(data->dragging && data->state == IMAGE_READY) {
            // The picture follows the finger, on the top screen at its own scale.
            data->panX += (pos.px - data->lastX) * 1.5f;
            data->panY += (pos.py - data->lastY) * 1.5f;
            image_clamp_pan(data);
        }

        if(data->pressed >= 0) {
            float x, y, w, h;
            image_button_rect(data->pressed, &x, &y, &w, &h);
            if(!widget_touched(&pos, x, y, w, h)) {
                data->pressed = -1;
            }
        }

        data->lastX = pos.px;
        data->lastY = pos.py;
    } else if(hidKeysUp() & KEY_TOUCH) {
        if(data->pressed >= 0) {
            int button = data->pressed;
            data->pressed = -1;
            image_press(data, button);
        }

        data->dragging = false;
    }
}

void imageview_open(FS_Archive archive, const char* const* paths, u32 count, u32 index) {
    if(count == 0 || index >= count) {
        return;
    }

    image_data* data = (image_data*) calloc(1, sizeof(image_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate picture viewer data.");
        return;
    }

    data->paths = (char**) calloc(count, sizeof(char*));
    if(data->paths == NULL) {
        free(data);
        error_display(NULL, NULL, "Failed to allocate picture viewer data.");
        return;
    }

    for(u32 i = 0; i < count; i++) {
        data->paths[i] = strdup(paths[i]);
        if(data->paths[i] == NULL) {
            for(u32 j = 0; j < i; j++) {
                free(data->paths[j]);
            }

            free(data->paths);
            free(data);
            error_display(NULL, NULL, "Failed to allocate picture viewer data.");
            return;
        }
    }

    data->archive = archive;
    data->count = count;
    data->index = index;
    data->pressed = -1;
    data->state = IMAGE_PENDING;
    data->delay = 2;
    string_get_path_file(data->name, data->paths[index], sizeof(data->name));

    ui_view* view = ui_create();
    view->name = data->name;
    view->info = "";
    view->data = data;
    view->update = image_update;
    view->drawTop = image_draw_top;
    view->drawBottom = image_draw_bottom;
    data->view = view;
    ui_push(view);
}
