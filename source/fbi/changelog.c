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
    float area;     // The height the last draw had for the lines.
    u32 inputFrame; // The ui_frame whose input was read (3D draws the top screen twice).

    // The releases: their heading lines.
    u32* heads;
    u32 headCount;
    u32 headCapacity;
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

    if(kind == LINE_HEAD) {
        if(text->headCount >= text->headCapacity) {
            u32 capacity = text->headCapacity > 0 ? text->headCapacity * 2 : 32;
            u32* grown = (u32*) realloc(text->heads, capacity * sizeof(u32));
            if(grown == NULL) {
                text->count--;
                text->height -= line->height;
                return NULL;
            }

            text->heads = grown;
            text->headCapacity = capacity;
        }

        text->heads[text->headCount++] = text->count - 1;
    }

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
    u32 headCount = text->headCount;
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
        text->headCount = headCount;
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
        free(text->heads);
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

static u32 changelog_max_scroll(changelog_text* text, float area);

u32 changelog_text_release_count(changelog_text* text) {
    return text != NULL ? text->headCount : 0;
}

const char* changelog_text_release(changelog_text* text, u32 index, bool* beta) {
    changelog_line* line = &text->lines[text->heads[index]];
    if(beta != NULL) {
        *beta = line->beta;
    }

    return line->text;
}

u32 changelog_text_current_release(changelog_text* text) {
    // The last release whose heading is at or above the first line; at the end of the text, the last one on screen
    // (those below the top can never reach it).
    float limit = text->atEnd && text->count > 0 ? text->lines[text->scroll].top + text->area : -1;
    u32 current = 0;
    for(u32 i = 0; i < text->headCount; i++) {
        changelog_line* head = &text->lines[text->heads[i]];
        if(text->heads[i] <= text->scroll || head->top + head->height <= limit) {
            current = i;
        }
    }

    return current;
}

void changelog_text_go_to_release(changelog_text* text, u32 index) {
    if(text == NULL || index >= text->headCount) {
        return;
    }

    u32 maxScroll = changelog_max_scroll(text, text->area);
    text->scroll = text->heads[index] < maxScroll ? text->heads[index] : maxScroll;
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
    text->area = area;
    u32 maxScroll = changelog_max_scroll(text, area);

    // Input once a frame: in 3D this draws once for each eye.
    bool input = text->inputFrame != ui_frame();
    text->inputFrame = ui_frame();
    u32 down = input ? hidKeysDown() : 0;
    u32 held = input ? hidKeysHeld() : 0;

    // One line per press; after a short hold, one line every other frame.
    static u32 repeat = 0;
    if(held & (KEY_UP | KEY_DOWN)) {
        repeat++;
    } else if(input) {
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
    } else if(input) {
        pageRepeat = 0;
    }

    bool page = (down & (KEY_L | KEY_R)) || (pageRepeat > 20 && pageRepeat % 8 == 0);
    if(page && (held & KEY_L)) {
        changelog_page(text, area, false, maxScroll);
    } else if(page && (held & KEY_R)) {
        changelog_page(text, area, true, maxScroll);
    }

    // ZL / ZR (a New 3DS): the start and the end, as in the lists.
    if(down & KEY_ZL) {
        text->scroll = 0;
    } else if(down & KEY_ZR) {
        text->scroll = maxScroll;
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

// The bottom screen: every release as a chip, grouped by minor version (v0.9, v0.8...), the one on screen
// highlighted, the ones not loaded yet hollow. A drag scrolls it; a tap goes to a release (loading the pages up to it).
#define INDEX_COLUMNS 4
#define INDEX_MAX 512
#define INDEX_MARGIN 8.0f
#define INDEX_GAP 5.0f
#define INDEX_CHIP_HEIGHT 22.0f
#define INDEX_GROUP_HEIGHT 18.0f
#define INDEX_HELP_HEIGHT 20.0f
#define INDEX_DRAG_SLOP 6

typedef struct {
    const char* tag;
    bool beta;
    s32 release;     // Its heading in the text, -1 while it is not loaded.
    float x;
    float y;         // In the index, from its top.
    bool groupStart; // The first of its group: a heading above it.
} changelog_index_entry;

typedef struct {
    changelog_text* text;
    changelog_source source;   // All NULL: nothing more to load.

    float scrollY;             // Of the index.
    s32 lastCurrent;           // The release the index last followed.
    bool touching;
    bool dragged;
    u16 touchX;
    u16 touchY;
    float touchScroll;
    char target[16];           // A release tapped before it was loaded: loading up to it.

    changelog_index_entry entries[INDEX_MAX];
    u32 entryCount;
    float contentHeight;
} changelog_view_data;

typedef struct {
    float labelY;
    float top;        // Of the scrolling part.
    float bottom;
    float chipWidth;
    float helpY;
} changelog_index_layout;

static changelog_index_layout changelog_index(float x1, float y1, float x2, float y2) {
    changelog_index_layout l;
    l.labelY = y1 + 5;
    l.top = l.labelY + widget_text_height(WIDGET_SMALL_SCALE) + 5;
    l.helpY = y2 - INDEX_HELP_HEIGHT;
    l.bottom = l.helpY - 2;
    l.chipWidth = (x2 - x1 - 2 * INDEX_MARGIN - (INDEX_COLUMNS - 1) * INDEX_GAP) / INDEX_COLUMNS;
    return l;
}

static s32 changelog_find_release(changelog_text* text, const char* tag) {
    u32 count = changelog_text_release_count(text);
    for(u32 i = 0; i < count; i++) {
        if(strcmp(changelog_text_release(text, i, NULL), tag) == 0) {
            return (s32) i;
        }
    }

    return -1;
}

// "v0.9.3" and "v0.9.0" are one group, "v0.8.4" another.
static bool changelog_same_group(const char* a, const char* b) {
    const char* da = strchr(a, '.');
    const char* db = strchr(b, '.');
    da = da != NULL ? strchr(da + 1, '.') : NULL;
    db = db != NULL ? strchr(db + 1, '.') : NULL;
    return da != NULL && db != NULL && da - a == db - b && strncmp(a, b, (size_t) (da - a)) == 0;
}

// The releases (every one the pages list, else the ones loaded) and where each chip goes.
static void changelog_index_build(changelog_view_data* d, float chipWidth) {
    d->entryCount = 0;
    u32 loaded = changelog_text_release_count(d->text);
    u32 total = d->source.catalogCount > 0 ? d->source.catalogCount : loaded;
    for(u32 i = 0; i < total && d->entryCount < INDEX_MAX; i++) {
        changelog_index_entry* e = &d->entries[d->entryCount++];
        if(d->source.catalogCount > 0) {
            e->tag = d->source.catalog[i].tag;
            e->beta = d->source.catalog[i].prerelease;
            e->release = changelog_find_release(d->text, e->tag);
        } else {
            e->tag = changelog_text_release(d->text, i, &e->beta);
            e->release = (s32) i;
        }
    }

    float y = 0;
    u32 column = 0;
    for(u32 i = 0; i < d->entryCount; i++) {
        changelog_index_entry* e = &d->entries[i];
        e->groupStart = i == 0 || !changelog_same_group(d->entries[i - 1].tag, e->tag);
        if(e->groupStart) {
            if(i > 0) {
                y += INDEX_CHIP_HEIGHT + INDEX_GAP + 2;
            }

            y += INDEX_GROUP_HEIGHT;
            column = 0;
        } else if(column == INDEX_COLUMNS) {
            y += INDEX_CHIP_HEIGHT + INDEX_GAP;
            column = 0;
        }

        e->x = INDEX_MARGIN + column * (chipWidth + INDEX_GAP);
        e->y = y;
        column++;
    }

    d->contentHeight = d->entryCount > 0 ? y + INDEX_CHIP_HEIGHT : 0;
}

static float changelog_index_max_scroll(changelog_view_data* d, changelog_index_layout* l) {
    float view = l->bottom - l->top;
    return d->contentHeight > view ? d->contentHeight - view : 0;
}

static s32 changelog_index_current(changelog_view_data* d) {
    if(changelog_text_release_count(d->text) == 0) {
        return -1;
    }

    const char* tag = changelog_text_release(d->text, changelog_text_current_release(d->text), NULL);
    for(u32 i = 0; i < d->entryCount; i++) {
        if(strcmp(d->entries[i].tag, tag) == 0) {
            return (s32) i;
        }
    }

    return -1;
}

static void changelog_index_tap(changelog_view_data* d, changelog_index_layout* l, float bx1, u16 px, u16 py) {
    float x = px - bx1;
    float y = py - l->top + d->scrollY;
    if(py < l->top || py >= l->bottom) {
        return;
    }

    for(u32 i = 0; i < d->entryCount; i++) {
        changelog_index_entry* e = &d->entries[i];
        if(x >= e->x && x < e->x + l->chipWidth && y >= e->y && y < e->y + INDEX_CHIP_HEIGHT) {
            if(e->release >= 0) {
                changelog_text_go_to_release(d->text, (u32) e->release);
                d->target[0] = '\0';
            } else {
                // Not loaded yet: load the pages up to it, then go there.
                string_copy(d->target, e->tag, sizeof(d->target));
            }

            return;
        }
    }
}

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

    changelog_index_layout l = changelog_index(bx1, by1, bx2, by2);
    changelog_index_build(d, l.chipWidth);
    float maxScroll = changelog_index_max_scroll(d, &l);

    // A finger: dragged, it scrolls the index; lifted without moving, a tap.
    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        d->touching = pos.py >= l.top && pos.py < l.bottom;
        d->dragged = false;
        d->touchX = pos.px;
        d->touchY = pos.py;
        d->touchScroll = d->scrollY;
    } else if(d->touching && (hidKeysHeld() & KEY_TOUCH)) {
        int dy = (int) pos.py - (int) d->touchY;
        if(dy > INDEX_DRAG_SLOP || dy < -INDEX_DRAG_SLOP) {
            d->dragged = true;
        }

        if(d->dragged) {
            d->scrollY = d->touchScroll - dy;
        }
    } else if(d->touching) {
        d->touching = false;
        if(!d->dragged) {
            changelog_index_tap(d, &l, bx1, d->touchX, d->touchY);
        }
    }

    // A release tapped before it was loaded: once it is, go to it; until then, more pages.
    if(d->target[0] != '\0') {
        s32 release = changelog_find_release(d->text, d->target);
        if(release >= 0) {
            changelog_text_go_to_release(d->text, (u32) release);
            d->target[0] = '\0';
        } else if(d->source.more != NULL) {
            d->source.more(d->source.data, false);
            const char* status = d->source.status != NULL ? d->source.status(d->source.data) : NULL;
            if(d->source.finished != NULL && d->source.finished(d->source.data) && status == NULL) {
                d->target[0] = '\0';   // The list ended without it.
            }
        } else {
            d->target[0] = '\0';
        }
    }

    // The index follows the release on screen (not while a finger is on it).
    s32 current = changelog_index_current(d);
    if(current >= 0 && current != d->lastCurrent && !d->touching) {
        d->lastCurrent = current;
        float view = l.bottom - l.top;
        changelog_index_entry* e = &d->entries[current];
        float top = e->y - (e->groupStart ? INDEX_GROUP_HEIGHT : 0);
        if(top < d->scrollY) {
            d->scrollY = top;
        } else if(e->y + INDEX_CHIP_HEIGHT > d->scrollY + view) {
            d->scrollY = e->y + INDEX_CHIP_HEIGHT - view;
        }
    }

    if(d->scrollY > maxScroll) {
        d->scrollY = maxScroll;
    }

    if(d->scrollY < 0) {
        d->scrollY = 0;
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
    const theme_current* t = theme();
    changelog_index_layout l = changelog_index(x1, y1, x2, y2);

    if(d->entryCount > 0) {
        screen_draw_text(_("Versions"), x1 + INDEX_MARGIN, l.labelY, WIDGET_SMALL_SCALE, t->palette->textMuted);

        // How many are loaded, and what a hollow chip means.
        bool anyMissing = false;
        for(u32 i = 0; i < d->entryCount && !anyMissing; i++) {
            anyMissing = d->entries[i].release < 0;
        }

        if(anyMissing) {
            const char* legend = _("Hollow: not loaded yet, tap to load");
            float w = widget_text_width(legend, WIDGET_SMALL_SCALE);
            screen_draw_text(legend, x2 - INDEX_MARGIN - w, l.labelY, WIDGET_SMALL_SCALE, t->palette->textMuted);
        }

        s32 current = changelog_index_current(d);
        float textHeight = widget_text_height(WIDGET_TEXT_SCALE);
        float smallHeight = widget_text_height(WIDGET_SMALL_SCALE);
        screen_set_clip(x1, l.top, x2 - x1, l.bottom - l.top);
        for(u32 i = 0; i < d->entryCount; i++) {
            changelog_index_entry* e = &d->entries[i];
            float cy = l.top + e->y - d->scrollY;
            if(e->groupStart) {
                // The group: "v0.9" and a hairline.
                float gy = cy - INDEX_GROUP_HEIGHT;
                if(gy + INDEX_GROUP_HEIGHT >= l.top && gy < l.bottom) {
                    const char* second = strchr(e->tag, '.');
                    second = second != NULL ? strchr(second + 1, '.') : NULL;
                    char group[16];
                    size_t len = second != NULL ? (size_t) (second - e->tag) : strlen(e->tag);
                    if(len >= sizeof(group)) {
                        len = sizeof(group) - 1;
                    }

                    memcpy(group, e->tag, len);
                    group[len] = '\0';
                    float ty = gy + (INDEX_GROUP_HEIGHT - smallHeight) / 2 - 1;
                    screen_draw_text(group, x1 + INDEX_MARGIN, ty, WIDGET_SMALL_SCALE, t->accent);
                    float after = x1 + INDEX_MARGIN + widget_text_width(group, WIDGET_SMALL_SCALE) + 6;
                    screen_fill_rect(after, gy + INDEX_GROUP_HEIGHT / 2, x2 - INDEX_MARGIN - after, 1, t->palette->border);
                }
            }

            if(cy + INDEX_CHIP_HEIGHT < l.top || cy >= l.bottom) {
                continue;
            }

            float cx = x1 + e->x;
            bool selected = (s32) i == current;
            bool loaded = e->release >= 0;
            bool waiting = !loaded && strcmp(d->target, e->tag) == 0;
            u32 ink;
            if(selected) {
                screen_fill_round_rect(cx, cy, l.chipWidth, INDEX_CHIP_HEIGHT, 6, t->accent);
                ink = t->onAccent;
            } else if(loaded || waiting) {
                screen_fill_round_rect(cx, cy, l.chipWidth, INDEX_CHIP_HEIGHT, 6, waiting ? t->accent : t->palette->border);
                screen_fill_round_rect(cx + 1, cy + 1, l.chipWidth - 2, INDEX_CHIP_HEIGHT - 2, 5, waiting ? t->accentSoft : t->palette->surface);
                ink = waiting ? t->accent : t->palette->text;
            } else {
                // Not loaded: hollow, the text muted.
                screen_fill_round_rect(cx, cy, l.chipWidth, INDEX_CHIP_HEIGHT, 6, t->palette->border);
                screen_fill_round_rect(cx + 1, cy + 1, l.chipWidth - 2, INDEX_CHIP_HEIGHT - 2, 5, t->palette->background);
                ink = t->palette->textMuted;
            }

            widget_text_centered(e->tag, cx, cx + l.chipWidth, cy + (INDEX_CHIP_HEIGHT - textHeight) / 2, WIDGET_TEXT_SCALE, ink);
            if(e->beta) {
                // A pre-release: a dot in the warning colour.
                screen_fill_circle(cx + l.chipWidth - 6, cy + 6, 2.5f, t->palette->warning);
            }
        }

        screen_clear_clip();

        float maxScroll = changelog_index_max_scroll(d, &l);
        if(maxScroll > 0) {
            float view = l.bottom - l.top;
            widget_scrollbar(x2 - 5, l.top, view, d->scrollY, view, d->contentHeight);
        }
    }

    const char* status = d->source.status != NULL ? d->source.status(d->source.data) : NULL;
    const char* help = status != NULL ? status : _("/: page   ZL/ZR: start, end   Tap: a version");
    widget_text_centered(help, x1, x2, l.helpY + (INDEX_HELP_HEIGHT - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE,
                         status != NULL ? t->palette->text : t->palette->textMuted);
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
    d->lastCurrent = -1;
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
