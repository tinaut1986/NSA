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
    u32 capacity;
    u32 scroll;
    bool atEnd;   // The last draw showed the last line.
};

static bool utf8_continuation(char c) {
    return ((unsigned char) c & 0xC0) == 0x80;
}

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

                // A space early in the line is not worth breaking at (Japanese and Chinese have none past "- ").
                take = space > 0 && space >= take / 2 ? space : take - 1;
                // Never in the middle of a character (Japanese and Chinese have no spaces to break at).
                while(take > 0 && utf8_continuation(p[take])) {
                    take--;
                }
            }

            if(take == 0 && len > 0) {
                take = 1;
                while(take < len && utf8_continuation(p[take])) {
                    take++;
                }
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
    if(text != NULL && !changelog_text_append(text, notes)) {
        changelog_text_free(text);
        return NULL;
    }

    return text;
}

bool changelog_text_append(changelog_text* text, const char* notes) {
    if(text == NULL) {
        return false;
    }

    char (*raw)[UPDATE_LINE_MAX] = calloc(CHANGELOG_LINES_MAX, UPDATE_LINE_MAX);
    if(raw == NULL) {
        return false;
    }

    u32 rawCount = update_format_notes(notes, raw, CHANGELOG_LINES_MAX, UPDATE_LINE_MAX - 1);

    // A blank line between what was there and what comes; a long line rewraps to a few.
    u32 need = text->count + 1 + rawCount * 3 + 16;
    if(need > text->capacity) {
        char (*grown)[UPDATE_LINE_MAX] = realloc(text->lines, (size_t) need * UPDATE_LINE_MAX);
        if(grown == NULL) {
            free(raw);
            return false;
        }

        text->lines = grown;
        text->capacity = need;
    }

    if(text->count > 0) {
        text->lines[text->count++][0] = '\0';
    }

    text->count += changelog_rewrap(raw, rawCount, text->lines + text->count, text->capacity - text->count);
    text->atEnd = false;
    free(raw);
    return true;
}

bool changelog_text_at_end(changelog_text* text) {
    return text != NULL && text->atEnd;
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

    text->atEnd = text->scroll >= maxScroll;

    float y = y1 + CHANGELOG_MARGIN;
    for(u32 i = text->scroll; i < text->count && i < text->scroll + visible; i++) {
        screen_draw_text(text->lines[i], x1 + CHANGELOG_MARGIN, y, CHANGELOG_TEXT_SCALE, theme()->palette->text);
        y += lineHeight;
    }

    widget_scrollbar(x2 - 6 - WIDGET_SCROLLBAR_WIDTH, y1 + CHANGELOG_MARGIN, y2 - y1 - 2 * CHANGELOG_MARGIN, text->scroll, visible, text->count);
}

typedef struct {
    changelog_text* text;
    changelog_source source;   // All NULL: nothing more to load.
} changelog_view_data;

static void changelog_view_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    changelog_view_data* d = (changelog_view_data*) data;

    if(hidKeysDown() & KEY_B) {
        if(d->source.closed != NULL) {
            d->source.closed(d->source.data);
        }

        ui_pop();
        changelog_text_free(d->text);
        free(d);
        ui_destroy(view);
        return;
    }

    if(d->source.more != NULL && changelog_text_at_end(d->text)) {
        d->source.more(d->source.data, (hidKeysDown() & KEY_A) != 0);
    }
}

static void changelog_view_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    changelog_text_draw(((changelog_view_data*) data)->text, x1, y1, x2, y2);
}

static void changelog_view_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    changelog_view_data* d = (changelog_view_data*) data;

    const char* status = d->source.status != NULL ? d->source.status(d->source.data) : NULL;
    const char* help = status != NULL ? status : _("Up/Down: scroll   /: page");
    widget_text_centered(help, x1, x2, y1 + (y2 - y1 - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE,
                         status != NULL ? theme()->palette->text : theme()->palette->textMuted);
}

void changelog_display_paged(const char* name, changelog_text* text, const changelog_source* source) {
    changelog_view_data* d = text != NULL ? (changelog_view_data*) calloc(1, sizeof(changelog_view_data)) : NULL;
    if(d == NULL) {
        changelog_text_free(text);
        if(source != NULL && source->closed != NULL) {
            source->closed(source->data);
        }

        error_display(NULL, NULL, _("Failed to allocate changelog."));
        return;
    }

    d->text = text;
    if(source != NULL) {
        d->source = *source;
    }

    ui_view* view = ui_create();
    view->name = name;
    view->backArrow = true;
    view->info = _(" back");
    view->data = d;
    view->update = changelog_view_update;
    view->drawTop = changelog_view_draw_top;
    view->drawBottom = changelog_view_draw_bottom;
    ui_push(view);
}

void changelog_display(const char* name, changelog_text* text) {
    changelog_display_paged(name, text, NULL);
}
