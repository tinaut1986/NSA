#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "changelog.h"
#include "resources.h"
#include "update_manifest.h"
#include "../core/core.h"

#define CHANGELOG_LINES_MAX 1024
#define CHANGELOG_TEXT_SCALE 0.5f
#define CHANGELOG_MARGIN 6.0f
#define CHANGELOG_WRAP_WIDTH (TOP_SCREEN_WIDTH - 2 * CHANGELOG_MARGIN - 8)

struct changelog_text_s {
    char (*lines)[UPDATE_LINE_MAX];
    u32 count;
    u32 scroll;
};

// update_format_notes wraps by columns; the system font is proportional, so
// lines that are still too wide on screen are split again by measured width.
static u32 changelog_rewrap(char (*in)[UPDATE_LINE_MAX], u32 inCount, char (*out)[UPDATE_LINE_MAX], u32 capacity) {
    u32 count = 0;
    for(u32 i = 0; i < inCount && count < capacity; i++) {
        const char* p = in[i];
        do {
            size_t len = strlen(p);
            char buf[UPDATE_LINE_MAX];
            size_t take = len;
            while(take > 0) {
                memcpy(buf, p, take);
                buf[take] = '\0';

                float width;
                screen_get_string_size(&width, NULL, buf, CHANGELOG_TEXT_SCALE, CHANGELOG_TEXT_SCALE);
                if(width <= CHANGELOG_WRAP_WIDTH) {
                    break;
                }

                size_t space = take - 1;
                while(space > 0 && p[space] != ' ') {
                    space--;
                }

                take = space > 0 ? space : take - 1;
            }

            if(take == 0) {
                take = len > 0 ? 1 : 0;
            }

            memcpy(out[count], p, take);
            out[count++][take] = '\0';
            p += take;
            while(*p == ' ') {
                p++;
            }
        } while(*p && count < capacity);
    }

    return count;
}

changelog_text* changelog_text_create(const char* notes) {
    changelog_text* text = (changelog_text*) calloc(1, sizeof(changelog_text));
    char (*raw)[UPDATE_LINE_MAX] = calloc(CHANGELOG_LINES_MAX, UPDATE_LINE_MAX);
    if(text != NULL) {
        text->lines = calloc(CHANGELOG_LINES_MAX, UPDATE_LINE_MAX);
    }

    if(text == NULL || raw == NULL || text->lines == NULL) {
        free(raw);
        changelog_text_free(text);
        return NULL;
    }

    u32 rawCount = update_format_notes(notes, raw, CHANGELOG_LINES_MAX, UPDATE_LINE_MAX - 1);
    text->count = changelog_rewrap(raw, rawCount, text->lines, CHANGELOG_LINES_MAX);
    free(raw);

    return text;
}

void changelog_text_free(changelog_text* text) {
    if(text != NULL) {
        free(text->lines);
        free(text);
    }
}

void changelog_text_draw(changelog_text* text, float x1, float y1, float x2, float y2) {
    if(text == NULL) {
        return;
    }

    float lineHeight = screen_get_font_height(CHANGELOG_TEXT_SCALE);
    u32 visible = (u32) ((y2 - y1 - 2 * CHANGELOG_MARGIN) / lineHeight);
    u32 maxScroll = text->count > visible ? text->count - visible : 0;

    u32 down = hidKeysDown();
    u32 held = hidKeysHeld();
    static u32 repeat = 0;
    if(held & (KEY_UP | KEY_DOWN)) {
        repeat++;
    } else {
        repeat = 0;
    }

    // One line per press; after a short hold, one line every other frame.
    bool step = (down & (KEY_UP | KEY_DOWN)) || (repeat > 20 && repeat % 2 == 0);
    if(step && (held & KEY_UP) && text->scroll > 0) {
        text->scroll--;
    } else if(step && (held & KEY_DOWN) && text->scroll < maxScroll) {
        text->scroll++;
    }

    if(down & KEY_L) {
        text->scroll = text->scroll > visible ? text->scroll - visible : 0;
    } else if(down & KEY_R) {
        text->scroll = text->scroll + visible < maxScroll ? text->scroll + visible : maxScroll;
    }

    if(text->scroll > maxScroll) {
        text->scroll = maxScroll;
    }

    float y = y1 + CHANGELOG_MARGIN;
    for(u32 i = text->scroll; i < text->count && i < text->scroll + visible; i++) {
        screen_draw_string(text->lines[i], x1 + CHANGELOG_MARGIN, y, CHANGELOG_TEXT_SCALE, CHANGELOG_TEXT_SCALE, COLOR_TEXT, false);
        y += lineHeight;
    }

    if(maxScroll > 0) {
        u32 barWidth;
        u32 barHeight;
        screen_get_texture_size(&barWidth, &barHeight, TEXTURE_SCROLL_BAR);

        float trackHeight = y2 - y1;
        float thumbHeight = trackHeight * visible / text->count;
        float thumbY = y1 + (trackHeight - thumbHeight) * text->scroll / maxScroll;
        screen_draw_texture(TEXTURE_SCROLL_BAR, x2 - barWidth, thumbY, barWidth, thumbHeight);
    }
}

static void changelog_view_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    if(hidKeysDown() & KEY_B) {
        ui_pop();
        changelog_text_free((changelog_text*) data);
        ui_destroy(view);
    }
}

static void changelog_view_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    changelog_text_draw((changelog_text*) data, x1, y1, x2, y2);
}

static void changelog_view_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    static const char* help = "Up/Down: Scroll\nL/R: Page";

    float textWidth;
    float textHeight;
    screen_get_string_size(&textWidth, &textHeight, help, 0.5f, 0.5f);
    screen_draw_string(help, x1 + (x2 - x1 - textWidth) / 2, y1 + (y2 - y1 - textHeight) / 2, 0.5f, 0.5f, COLOR_TEXT, true);
}

void changelog_display(const char* name, changelog_text* text) {
    if(text == NULL) {
        error_display(NULL, NULL, "Failed to allocate changelog.");
        return;
    }

    ui_view* view = ui_create();
    view->name = name;
    view->info = "B: Back";
    view->data = text;
    view->update = changelog_view_update;
    view->drawTop = changelog_view_draw_top;
    view->drawBottom = changelog_view_draw_bottom;
    ui_push(view);
}
