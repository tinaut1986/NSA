#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "changelog.h"
#include "resources.h"
#include "update_manifest.h"
#include "../core/core.h"

#define CHANGELOG_TEXT_SCALE 0.5f
#define CHANGELOG_HEAD_SCALE 0.6f
#define CHANGELOG_MARGIN 6.0f
#define CHANGELOG_INDENT 12.0f        // A bullet's text, and the lines it wraps to.
#define CHANGELOG_HEAD_SPACE 8.0f     // Above a version: the hairline between releases.
#define CHANGELOG_WRAP_WIDTH (TOP_SCREEN_WIDTH - 2 * CHANGELOG_MARGIN - 8)
#define CHANGELOG_LINE_MAX 192        // Bytes of one line on screen (Japanese: about 3 a character).

typedef enum {
    LINE_HEAD,     // A release: "v0.9.9", with a BETA badge for a pre-release.
    LINE_BULLET,   // The first line of a bullet.
    LINE_CONT,     // A bullet's next lines, under its text.
    LINE_PARA      // Anything else.
} changelog_line_kind;

typedef struct {
    u8 kind;
    bool beta;
    float top;     // From the top of the whole text.
    float height;
    char text[CHANGELOG_LINE_MAX];
} changelog_line;

struct changelog_text_s {
    changelog_line* lines;
    u32 count;
    u32 capacity;
    float height;   // Of all the lines.
    u32 scroll;     // The first line on screen.
    bool atEnd;     // The last draw showed the last line.
};

static bool utf8_continuation(char c) {
    return ((unsigned char) c & 0xC0) == 0x80;
}

static float changelog_measure(const char* p, size_t len, float scale) {
    char buf[CHANGELOG_LINE_MAX];
    memcpy(buf, p, len);
    buf[len] = '\0';

    float width = 0;
    screen_get_string_size(&width, NULL, buf, scale, scale);
    return width;
}

// How many bytes of p go on a line maxWidth wide: whole words while they fit; when the last space that fits leaves
// the line short (Japanese and Chinese have few or no spaces), whole characters instead. Never 0 while p has any.
static size_t changelog_fit(const char* p, size_t len, float maxWidth, float scale) {
    size_t best = 0;
    float bestWidth = 0;
    for(size_t i = 0; i < len;) {
        size_t j = i;
        while(j < len && p[j] == ' ') {
            j++;
        }

        while(j < len && p[j] != ' ') {
            j++;
        }

        if(j >= CHANGELOG_LINE_MAX) {
            break;
        }

        float width = changelog_measure(p, j, scale);
        if(width > maxWidth) {
            break;
        }

        best = j;
        bestWidth = width;
        i = j;
    }

    if(best == len || (best > 0 && bestWidth >= maxWidth * 0.6f)) {
        return best;
    }

    size_t k = best;
    while(k < len) {
        size_t next = k + 1;
        while(next < len && utf8_continuation(p[next])) {
            next++;
        }

        if(next >= CHANGELOG_LINE_MAX || changelog_measure(p, next, scale) > maxWidth) {
            break;
        }

        k = next;
    }

    if(k == 0) {
        k = 1;
        while(k < len && utf8_continuation(p[k])) {
            k++;
        }
    }

    return k;
}

static changelog_line* changelog_add(changelog_text* text, u8 kind, const char* p, size_t len) {
    if(text->count >= text->capacity) {
        u32 capacity = text->capacity > 0 ? text->capacity * 2 : 256;
        changelog_line* grown = (changelog_line*) realloc(text->lines, capacity * sizeof(changelog_line));
        if(grown == NULL) {
            return NULL;
        }

        text->lines = grown;
        text->capacity = capacity;
    }

    changelog_line* line = &text->lines[text->count++];
    line->kind = kind;
    line->beta = false;
    if(len >= CHANGELOG_LINE_MAX) {
        len = CHANGELOG_LINE_MAX - 1;
    }

    memcpy(line->text, p, len);
    line->text[len] = '\0';

    float body = screen_get_font_height(CHANGELOG_TEXT_SCALE);
    line->height = kind == LINE_HEAD ? CHANGELOG_HEAD_SPACE + screen_get_font_height(CHANGELOG_HEAD_SCALE) + 2 : body;
    line->top = text->height;
    text->height += line->height;
    return line;
}

// One item of update_notes_items, wrapped to the screen.
static bool changelog_add_item(changelog_text* text, char kind, const char* p, size_t len) {
    if(kind == 'H') {
        bool beta = len > 5 && strncmp(p + len - 5, " BETA", 5) == 0;
        if(beta) {
            len -= 5;
        }

        changelog_line* line = changelog_add(text, LINE_HEAD, p, len);
        if(line == NULL) {
            return false;
        }

        line->beta = beta;
        return true;
    }

    bool bullet = kind == 'B';
    float width = CHANGELOG_WRAP_WIDTH - (bullet ? CHANGELOG_INDENT : 0);
    bool first = true;
    while(len > 0) {
        size_t take = changelog_fit(p, len, width, CHANGELOG_TEXT_SCALE);
        size_t shown = take;
        while(shown > 0 && p[shown - 1] == ' ') {
            shown--;
        }

        if(changelog_add(text, bullet ? (first ? LINE_BULLET : LINE_CONT) : LINE_PARA, p, shown) == NULL) {
            return false;
        }

        first = false;
        p += take;
        len -= take;
        while(len > 0 && *p == ' ') {
            p++;
            len--;
        }
    }

    return true;
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

    char* items = update_notes_items(notes);
    if(items == NULL) {
        return false;
    }

    u32 count = text->count;
    float height = text->height;
    bool ok = true;
    for(const char* p = items; *p && ok;) {
        const char* eol = strchr(p, '\n');
        size_t len = eol != NULL ? (size_t) (eol - p) : strlen(p);
        if(len > 1) {
            ok = changelog_add_item(text, p[0], p + 1, len - 1);
        }

        p += len + (eol != NULL ? 1 : 0);
    }

    if(ok && text->count == 0) {
        const char* none = "No changelog provided.";
        ok = changelog_add_item(text, 'P', none, strlen(none));
    }

    free(items);
    if(!ok) {
        // As it was.
        text->count = count;
        text->height = height;
        return false;
    }

    text->atEnd = false;
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

// The last first line: from it on, everything fits in `area`.
static u32 changelog_max_scroll(changelog_text* text, float area) {
    u32 s = text->count;
    while(s > 0 && text->height - text->lines[s - 1].top <= area) {
        s--;
    }

    return s;
}

static void changelog_page(changelog_text* text, float area, bool down, u32 maxScroll) {
    if(down) {
        // The first line that was not wholly on screen.
        float bottom = text->count > 0 ? text->lines[text->scroll].top + area : 0;
        u32 s = text->scroll;
        while(s < text->count && text->lines[s].top + text->lines[s].height <= bottom) {
            s++;
        }

        text->scroll = s > text->scroll ? (s < maxScroll ? s : maxScroll) : text->scroll;
    } else {
        float top = text->scroll < text->count ? text->lines[text->scroll].top - area : 0;
        u32 s = text->scroll;
        while(s > 0 && text->lines[s - 1].top >= top) {
            s--;
        }

        text->scroll = s;
    }
}

void changelog_text_draw(changelog_text* text, float x1, float y1, float x2, float y2) {
    if(text == NULL || text->count == 0) {
        return;
    }

    const theme_current* t = theme();
    float area = y2 - y1 - 2 * CHANGELOG_MARGIN;
    u32 maxScroll = changelog_max_scroll(text, area);

    u32 down = hidKeysDown();
    u32 held = hidKeysHeld();

    // One line per press; after a short hold, one line every other frame.
    static u32 repeat = 0;
    if(held & (KEY_UP | KEY_DOWN)) {
        repeat++;
    } else {
        repeat = 0;
    }

    bool step = (down & (KEY_UP | KEY_DOWN)) || (repeat > 20 && repeat % 2 == 0);
    if(step && (held & KEY_UP) && text->scroll > 0) {
        text->scroll--;
    } else if(step && (held & KEY_DOWN) && text->scroll < maxScroll) {
        text->scroll++;
    }

    // A page per press of L / R; held, a page every few frames.
    static u32 pageRepeat = 0;
    if(held & (KEY_L | KEY_R)) {
        pageRepeat++;
    } else {
        pageRepeat = 0;
    }

    bool page = (down & (KEY_L | KEY_R)) || (pageRepeat > 20 && pageRepeat % 8 == 0);
    if(page && (held & KEY_L)) {
        changelog_page(text, area, false, maxScroll);
    } else if(page && (held & KEY_R)) {
        changelog_page(text, area, true, maxScroll);
    }

    if(text->scroll > maxScroll) {
        text->scroll = maxScroll;
    }

    text->atEnd = text->scroll >= maxScroll;

    float x = x1 + CHANGELOG_MARGIN;
    float right = x2 - 6 - WIDGET_SCROLLBAR_WIDTH - 4;
    float bodyHeight = screen_get_font_height(CHANGELOG_TEXT_SCALE);
    float y = y1 + CHANGELOG_MARGIN;
    for(u32 i = text->scroll; i < text->count; i++) {
        changelog_line* line = &text->lines[i];
        if(y + line->height > y2 - CHANGELOG_MARGIN + 0.5f) {
            break;
        }

        switch(line->kind) {
            case LINE_HEAD: {
                if(i > 0) {
                    screen_fill_rect(x, y + CHANGELOG_HEAD_SPACE / 2 - 1, right - x, 1, t->palette->border);
                }

                float ty = y + CHANGELOG_HEAD_SPACE;
                screen_draw_text(line->text, x, ty, CHANGELOG_HEAD_SCALE, t->accent);
                if(line->beta) {
                    float w = widget_text_width(line->text, CHANGELOG_HEAD_SCALE);
                    float bh = widget_text_height(WIDGET_SMALL_SCALE) + 2;
                    float hh = screen_get_font_height(CHANGELOG_HEAD_SCALE);
                    widget_badge(x + w + 6, ty + (hh - bh) / 2, "BETA", theme_mix(t->palette->surface, t->palette->warning, 0x30), t->palette->warning);
                }
                break;
            }
            case LINE_BULLET:
                screen_fill_circle(x + 4, y + bodyHeight / 2 + 1, 2, t->accent);
                screen_draw_text(line->text, x + CHANGELOG_INDENT, y, CHANGELOG_TEXT_SCALE, t->palette->text);
                break;
            case LINE_CONT:
                screen_draw_text(line->text, x + CHANGELOG_INDENT, y, CHANGELOG_TEXT_SCALE, t->palette->text);
                break;
            default:
                screen_draw_text(line->text, x, y, CHANGELOG_TEXT_SCALE, t->palette->text);
                break;
        }

        y += line->height;
    }

    widget_scrollbar(x2 - 6 - WIDGET_SCROLLBAR_WIDTH, y1 + CHANGELOG_MARGIN, area, text->lines[text->scroll].top, area, text->height);
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
