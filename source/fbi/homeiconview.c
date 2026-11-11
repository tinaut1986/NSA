#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "homeicon.h"
#include "resources.h"
#include "section.h"
#include "../core/core.h"

// SD card -> a .3dsx's actions -> "Install in the HOME Menu": the top screen shows the banner the title will have (drawn
// from its icon and name, or a picture the user picks from the SD card), the list installs it or changes the banner.
// Installing runs on a thread of its own (a big .3dsx takes a while: its RomFS is read three times) behind a progress
// view that B cancels.

#define HV_STACK 0x10000
#define HV_BANNER_BYTES (HOMEICON_BANNER_WIDTH * HOMEICON_BANNER_HEIGHT * 4)

typedef enum {
    HV_INSTALL,
    HV_PICTURE,
    HV_ICON,
} hv_item;

typedef struct {
    char path[HOMEICON_PATH_MAX];
    char name[128];
    char publisher[128];
    SMDH smdh;
    bool hasSmdh;
    u32 iconTexture;
    u32 color;              // The icon's colour, 0xRRGGBB.

    u8* picture;            // A picked picture made a banner (R G B A), or NULL: drawn from the icon.
    char pictureName[FILE_NAME_MAX];
    bool itemsChanged;

    // The banner as it will be installed (R G B A), and as a texture for the top screen: the preview is that picture,
    // not a second drawing of it.
    u8* banner;
    u32 bannerTexture;
    bool bannerDirty;

    // The install.
    homeicon_job job;
    Thread thread;
    volatile bool finished;
    Result result;
    u64 started;
} homeicon_view;

static u32 hv_abgr(u32 rgb) {
    return 0xFF000000 | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

static u32 hv_shade(u32 rgb, u32 percent) {
    u32 r = ((rgb >> 16) & 0xFF) * percent / 100;
    u32 g = ((rgb >> 8) & 0xFF) * percent / 100;
    u32 b = (rgb & 0xFF) * percent / 100;
    return ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}

// The banner drawn from the icon, its top left at (x, y): the icon's colour going darker downwards, the icon at twice its
// size on a soft plate, the name (as big as fits, up to three lines) and the publisher beside it.
static void hv_draw_generated(homeicon_view* v, float x, float y) {
    const float w = HOMEICON_BANNER_WIDTH;
    const float h = HOMEICON_BANNER_HEIGHT;
    for(u32 band = 0; band < 16; band++) {
        screen_fill_rect(x, y + band * (h / 16), w, h / 16, hv_abgr(hv_shade(v->color, 74 - band * 38 / 16)));
    }

    float iconX = x + 14;
    float iconY = y + (h - 96) / 2;
    screen_fill_round_rect(iconX - 6, iconY - 6, 108, 108, 16, 0x30FFFFFF);
    if(v->iconTexture != 0) {
        screen_draw_texture(v->iconTexture, iconX, iconY, 96, 96);
    } else {
        screen_draw_icon(ICON_APPS, iconX + 16, iconY + 16, 64, 0xFFFFFFFF);
    }

    float textX = iconX + 96 + 16;
    float textW = x + w - 10 - textX;
    float scale = 0.85f;
    float nameW = 0;
    float nameH = 0;
    for(; scale > 0.5f; scale -= 0.05f) {
        screen_get_string_size_wrap(&nameW, &nameH, v->name, scale, scale, textW);
        if(nameH <= screen_get_font_height(scale) * 3 + 1 && nameW <= textW + 1) {
            break;
        }
    }

    float pubH = v->publisher[0] != '\0' ? screen_get_font_height(0.5f) + 6 : 0;
    float textY = y + (h - nameH - pubH) / 2;
    screen_draw_text_wrap(v->name, textX + 1, textY + 1, scale, 0x60000000, textX + textW + 1);
    screen_draw_text_wrap(v->name, textX, textY, scale, 0xFFFFFFFF, textX + textW);
    if(v->publisher[0] != '\0') {
        screen_draw_text_wrap(v->publisher, textX, textY + nameH + 6, 0.5f, 0xC8FFFFFF, textX + textW);
    }
}

static void hv_draw_offscreen(void* data) {
    hv_draw_generated((homeicon_view*) data, 0, 0);
}

static void hv_draw_banner(homeicon_view* v, float x, float y) {
    if(v->bannerTexture != 0) {
        screen_draw_texture(v->bannerTexture, x, y, HOMEICON_BANNER_WIDTH, HOMEICON_BANNER_HEIGHT);
    }
}

static void hv_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    homeicon_view* v = (homeicon_view*) data;
    const theme_current* t = theme();

    float bx = x1 + (x2 - x1 - HOMEICON_BANNER_WIDTH) / 2;
    float by = y1 + 12;
    screen_fill_round_rect(bx - 4, by - 4, HOMEICON_BANNER_WIDTH + 8, HOMEICON_BANNER_HEIGHT + 8, 8, t->palette->border);
    hv_draw_banner(v, bx, by);

    float y = by + HOMEICON_BANNER_HEIGHT + 16;
    const char* about = v->picture != NULL ? v->pictureName : _("Banner made from the icon and the name.");
    widget_text_fit(about, x1 + 16, y, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - x1 - 32);
    y += widget_text_height(WIDGET_SMALL_SCALE) + 8;
    screen_draw_text_wrap(_("A title of its own: it starts from the HOME Menu and goes back to it. The program is copied into it, so after updating the .3dsx, install it again."),
                          x1 + 16, y, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - 16);
}

static void hv_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    const theme_current* t = theme();
    u32 ink = 0;
    widget_item_frame(x, y, width, height, selected, &ink, NULL);

    static const u32 icons[] = {ICON_DOWNLOAD, ICON_PHOTO, ICON_REFRESH};
    hv_item kind = (hv_item) (uintptr_t) item->data;
    screen_draw_icon(icons[kind], x + 9, y + (height - 16) / 2, 16, selected ? t->onAccent : t->accent);
    widget_text_fit(item->name, x + 33, y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink, width - 42);
}

static void hv_free_items(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }
}

static void hv_add_item(linked_list* items, hv_item kind, const char* name) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    if(item != NULL) {
        string_copy(item->name, name, sizeof(item->name));
        item->color = COLOR_TEXT;
        item->data = (void*) (uintptr_t) kind;
        linked_list_add(items, item);
    }
}

static void hv_free(homeicon_view* v) {
    if(v->iconTexture != 0) {
        screen_unload_texture(v->iconTexture);
    }

    if(v->bannerTexture != 0) {
        screen_unload_texture(v->bannerTexture);
    }

    free(v->picture);
    free(v->banner);
    free(v);
}

// The banner as a texture for the preview: the GPU wants A, B, G, R.
static void hv_load_banner_texture(homeicon_view* v) {
    u8* texels = (u8*) malloc(HV_BANNER_BYTES);
    if(texels == NULL) {
        return;
    }

    for(u32 i = 0; i < HV_BANNER_BYTES; i += 4) {
        texels[i] = v->banner[i + 3];
        texels[i + 1] = v->banner[i + 2];
        texels[i + 2] = v->banner[i + 1];
        texels[i + 3] = v->banner[i];
    }

    if(v->bannerTexture == 0) {
        v->bannerTexture = screen_allocate_free_texture();
    }

    screen_load_texture_untiled(v->bannerTexture, texels, HV_BANNER_BYTES, HOMEICON_BANNER_WIDTH, HOMEICON_BANNER_HEIGHT, GPU_RGBA8, true);
    free(texels);
}

static void hv_picked(void* data, const char* path) {
    homeicon_view* v = (homeicon_view*) data;
    u8* picture = (u8*) malloc(HV_BANNER_BYTES);
    if(picture == NULL) {
        error_display(NULL, NULL, "Failed to allocate program data.");
        return;
    }

    Result res = homeicon_picture_banner(path, picture);
    if(R_FAILED(res)) {
        free(picture);
        if(res == R_APP_OUT_OF_RANGE) {
            prompt_display_notify("Failure", "That picture is too big for a banner: up to 12 MB and 5 million pixels.", COLOR_TEXT, NULL, NULL, NULL);
        } else {
            error_display_res(NULL, NULL, res, "Failed to read the picture.");
        }

        return;
    }

    free(v->picture);
    v->picture = picture;
    string_get_path_file(v->pictureName, path, sizeof(v->pictureName));
    v->bannerDirty = true;
    v->itemsChanged = true;
}

// The banner again (between frames: drawing it uses the GPU): the picture, or the drawing made into one (the plain one
// if that fails).
static void hv_refresh_banner(homeicon_view* v) {
    v->bannerDirty = false;
    if(v->banner == NULL && (v->banner = (u8*) malloc(HV_BANNER_BYTES)) == NULL) {
        return;
    }

    if(v->picture != NULL) {
        memcpy(v->banner, v->picture, HV_BANNER_BYTES);
    } else if(!screen_render_offscreen(HOMEICON_BANNER_WIDTH, HOMEICON_BANNER_HEIGHT, hv_draw_offscreen, v, v->banner)) {
        homeicon_plain_banner(v->hasSmdh ? &v->smdh : NULL, v->banner);
    }

    hv_load_banner_texture(v);
}

static void hv_install_thread(void* arg) {
    homeicon_view* v = (homeicon_view*) arg;
    v->result = homeicon_install(&v->job);
    v->finished = true;
}

static void hv_progress_update(ui_view* view, void* data, float* progress, char* text) {
    homeicon_view* v = (homeicon_view*) data;
    if(hidKeysDown() & KEY_B) {
        v->job.cancel = true;
    }

    if(v->finished) {
        threadJoin(v->thread, U64_MAX);
        threadFree(v->thread);
        v->thread = NULL;

        ui_pop();
        info_destroy(view);

        if(R_SUCCEEDED(v->result)) {
            char done[300];
            snprintf(done, sizeof(done), _("\"%s\" is in the HOME Menu now. The .3dsx is no longer needed to start it."), v->job.name);
            prompt_display_notify("Success", done, COLOR_TEXT, NULL, NULL, NULL);
        } else if(v->result != R_APP_CANCELLED) {
            error_display_res(NULL, NULL, v->result, "Failed to install the program in the HOME Menu.");
        }

        return;
    }

    u64 total = v->job.total;
    *progress = total > 0 ? (float) ((double) v->job.done / (double) total) : 0;
    snprintf(text, PROGRESS_TEXT_MAX, "%.1f / %.1f MiB", v->job.done / 1048576.0, total / 1048576.0);
}

static void hv_progress_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    homeicon_view* v = (homeicon_view*) data;
    float bx = x1 + (x2 - x1 - HOMEICON_BANNER_WIDTH) / 2;
    hv_draw_banner(v, bx, y1 + (y2 - y1 - HOMEICON_BANNER_HEIGHT) / 2);
}

static void hv_install(homeicon_view* v) {
    if(v->banner == NULL) {
        error_display(NULL, NULL, "Failed to allocate program data.");
        return;
    }

    if(NSA_DEBUG) {
        // To look at on a PC: the banner as drawn, before it is packed into the title.
        FILE* file = fopen("sdmc:" APP_DATA_DIR "homeicon-banner.rgba", "wb");
        if(file != NULL) {
            fwrite(v->banner, 1, HV_BANNER_BYTES, file);
            fclose(file);
        }
    }

    memset(&v->job, 0, sizeof(v->job));
    string_copy(v->job.path, v->path, sizeof(v->job.path));
    v->job.banner = v->banner;
    v->finished = false;
    v->result = 0;

    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    v->thread = threadCreate(hv_install_thread, v, HV_STACK, priority + 1, -2, false);
    if(v->thread == NULL) {
        error_display(NULL, NULL, "Failed to create the install thread.");
        return;
    }

    v->started = osGetTime();
    info_display("Installing in the HOME Menu", " cancel", true, v, hv_progress_update, hv_progress_top);
}

static void hv_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    homeicon_view* v = (homeicon_view*) data;
    view->name = _("HOME Menu");
    view->info = _(" choose    back");

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        hv_free_items(items);
        list_destroy(view);
        hv_free(v);
        return;
    }

    if(v->bannerDirty) {
        hv_refresh_banner(v);
    }

    if(linked_list_size(items) == 0 || v->itemsChanged) {
        hv_free_items(items);
        hv_add_item(items, HV_INSTALL, _("Install in the HOME Menu"));
        hv_add_item(items, HV_PICTURE, _("Choose a picture for the banner"));
        if(v->picture != NULL) {
            hv_add_item(items, HV_ICON, _("Make the banner from the icon"));
        }

        v->itemsChanged = false;
    }

    if(selected == NULL || !(selectedTouched || (hidKeysDown() & KEY_A))) {
        return;
    }

    switch((hv_item) (uintptr_t) selected->data) {
        case HV_INSTALL:
            hv_install(v);
            break;
        case HV_PICTURE: {
            char dir[HOMEICON_PATH_MAX];
            string_get_parent_path(dir, v->path, sizeof(dir));
            files_pick_picture(dir, hv_picked, v);
            break;
        }
        case HV_ICON:
            free(v->picture);
            v->picture = NULL;
            v->bannerDirty = true;
            v->itemsChanged = true;
            break;
    }
}

void homeicon_open(const char* path) {
    homeicon_view* v = (homeicon_view*) calloc(1, sizeof(homeicon_view));
    if(v == NULL) {
        error_display(NULL, NULL, "Failed to allocate program data.");
        return;
    }

    string_copy(v->path, path, sizeof(v->path));
    v->bannerDirty = true;
    v->hasSmdh = homeicon_read_smdh(path, &v->smdh);
    homeicon_name(path, v->hasSmdh ? &v->smdh : NULL, v->name, sizeof(v->name));
    v->color = homeicon_icon_color(v->hasSmdh ? &v->smdh : NULL);
    if(v->hasSmdh) {
        SMDH_title* title = smdh_select_title(&v->smdh);
        utf16_to_utf8((uint8_t*) v->publisher, title->publisher, sizeof(v->publisher) - 1);
        v->iconTexture = screen_allocate_free_texture();
        screen_load_texture_tiled(v->iconTexture, v->smdh.largeIcon, sizeof(v->smdh.largeIcon), 48, 48, GPU_RGB565, false);
    }

    ui_view* view = list_display(_("HOME Menu"), _(" choose    back"), v, hv_update, hv_draw_top);
    if(view == NULL) {
        hv_free(v);
        return;
    }

    list_style style = {
            .rowHeight = 34,
            .drawRow = hv_draw_row
    };
    list_set_style(view, &style);
    list_show_title_bar(view);
}
