// Text viewer and editor. The system keyboard covers the screen the text is on, so editing
// uses its own: the text stays on the top screen and the keys are on the bottom one.
// D-Pad moves the cursor, X deletes, Y types a space, A starts a new line, L and R turn pages.

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "viewers.h"
#include "../core/core.h"

#define TV_SCALE WIDGET_SMALL_SCALE
#define TV_PAD 5
#define TV_STATUS_HEIGHT 16
#define TV_EDIT_MAX (TEXTVIEW_MAX_SIZE * 2)
#define TV_NO_LINE 0xFFFFFFFF

// ---- the on-screen keyboard ----

typedef enum {
    K_CHAR,
    K_SHIFT,
    K_BACK,
    K_PAGE_LETTERS,
    K_PAGE_SYMBOLS,
    K_PAGE_EXTRA,
    K_SPACE,
    K_ENTER,
    K_LEFT,
    K_RIGHT,
    K_UP,
    K_DOWN,
    K_SAVE,
    K_DONE
} key_action;

typedef struct {
    const char* lower;
    const char* upper;
    float width;
    key_action action;
} key_def;

#define KC(l, u) {l, u, 1, K_CHAR}
#define KS(s) {s, s, 1, K_CHAR}
#define KA(act, w) {"", "", w, act}

#define KB_ROW_MAX 11

typedef struct {
    u32 count;
    key_def keys[KB_ROW_MAX];
} key_row;

static const key_row kb_toolbar = {6, {KA(K_SAVE, 3), KA(K_DONE, 3), KA(K_LEFT, 1), KA(K_UP, 1), KA(K_DOWN, 1), KA(K_RIGHT, 1)}};

static const key_row kb_letters[4] = {
        {10, {KC("q", "Q"), KC("w", "W"), KC("e", "E"), KC("r", "R"), KC("t", "T"), KC("y", "Y"), KC("u", "U"), KC("i", "I"), KC("o", "O"), KC("p", "P")}},
        {10, {KC("a", "A"), KC("s", "S"), KC("d", "D"), KC("f", "F"), KC("g", "G"), KC("h", "H"), KC("j", "J"), KC("k", "K"), KC("l", "L"), KC("ñ", "Ñ")}},
        {9, {KA(K_SHIFT, 1.5f), KC("z", "Z"), KC("x", "X"), KC("c", "C"), KC("v", "V"), KC("b", "B"), KC("n", "N"), KC("m", "M"), KA(K_BACK, 1.5f)}},
        {5, {KA(K_PAGE_SYMBOLS, 2), KS(","), KA(K_SPACE, 4), KS("."), KA(K_ENTER, 2)}}
};

static const key_row kb_symbols[4] = {
        {10, {KS("1"), KS("2"), KS("3"), KS("4"), KS("5"), KS("6"), KS("7"), KS("8"), KS("9"), KS("0")}},
        {10, {KS("@"), KS("#"), KS("$"), KS("%"), KS("&"), KS("*"), KS("-"), KS("+"), KS("("), KS(")")}},
        {9, {KA(K_PAGE_EXTRA, 1.5f), KS("!"), KS("\""), KS("'"), KS(":"), KS(";"), KS("/"), KS("?"), KA(K_BACK, 1.5f)}},
        {5, {KA(K_PAGE_LETTERS, 2), KS(","), KA(K_SPACE, 4), KS("."), KA(K_ENTER, 2)}}
};

static const key_row kb_extra[4] = {
        {10, {KC("á", "Á"), KC("é", "É"), KC("í", "Í"), KC("ó", "Ó"), KC("ú", "Ú"), KC("ü", "Ü"), KC("ñ", "Ñ"), KS("¿"), KS("¡"), KS("€")}},
        {10, {KS("="), KS("<"), KS(">"), KS("["), KS("]"), KS("{"), KS("}"), KS("\\"), KS("|"), KS("~")}},
        {9, {KA(K_PAGE_SYMBOLS, 1.5f), KS("_"), KS("^"), KS("`"), KC("ç", "Ç"), KS("°"), KS("§"), KS("´"), KA(K_BACK, 1.5f)}},
        {5, {KA(K_PAGE_LETTERS, 2), KS(","), KA(K_SPACE, 4), KS("."), KA(K_ENTER, 2)}}
};

#define KB_KEYS_MAX 48

typedef struct {
    float x, y, w, h;
    const key_def* def;
} key_rect;

// ---- the viewer ----

typedef struct {
    FS_Archive archive;
    char path[FILE_PATH_MAX];
    char name[FILE_NAME_MAX];
    char title[FILE_NAME_MAX + 4];

    char* buf;
    u32 len;
    u32 cap;
    bool crlf;
    bool truncated;
    bool converted;
    bool readOnly;
    bool modified;
    bool editing;
    u64 fileSize;
    u32 changes;

    u32 cursor;
    float desiredX;

    u32* lines;
    u32 lineCount;
    u32 lineCap;
    u32 top;
    float dragRemainder;

    // Cursor position as the status shows it.
    u32 statChanges;
    u32 statCursor;
    u32 statLine;
    u32 statColumn;

    ui_view* view;

    u32 page;
    u32 shift;      // 0 off, 1 next letter, 2 caps lock.
    key_rect keys[KB_KEYS_MAX];
    u32 keyCount;
    int pressedKey;
    int pressedButton;
    bool dragging;
    int lastY;
    u64 repeatAt;

    u64 holdAt[5];

    char message[64];
    u64 messageAt;
} text_data;

static float tv_ascii[128];
static bool tv_ascii_ready = false;

static float tv_measure(const char* s) {
    return widget_text_width(s, TV_SCALE);
}

static u32 tv_char_len(const char* p, u32 left) {
    u8 c = (u8) *p;
    u32 n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return n > left ? left : n;
}

static float tv_char_width(const char* p, u32 n) {
    if(!tv_ascii_ready) {
        for(int i = 0; i < 128; i++) {
            char s[2] = {(char) i, '\0'};
            tv_ascii[i] = i < 32 ? 0 : tv_measure(s);
        }

        tv_ascii['\t'] = 4 * tv_ascii[' '];
        tv_ascii_ready = true;
    }

    if(n == 1 && (u8) *p < 128) {
        return tv_ascii[(u8) *p];
    }

    // A few non-ASCII letters repeat all through a text.
    static char cacheKey[64][5];
    static float cacheWidth[64];
    u32 hash = 0;
    for(u32 i = 0; i < n; i++) {
        hash = hash * 31 + (u8) p[i];
    }

    hash &= 63;
    if(strncmp(cacheKey[hash], p, n) == 0 && cacheKey[hash][n] == '\0' && cacheKey[hash][0] != '\0') {
        return cacheWidth[hash];
    }

    char s[5] = {0};
    memcpy(s, p, n);
    float w = tv_measure(s);
    memcpy(cacheKey[hash], s, 5);
    cacheWidth[hash] = w;
    return w;
}

static float tv_line_height() {
    return widget_text_height(TV_SCALE) + 1;
}

static float tv_text_width() {
    return TOP_SCREEN_WIDTH - 2 * TV_PAD - WIDGET_SCROLLBAR_WIDTH - 2;
}

static void tv_push_line(text_data* d, u32 start) {
    if(d->lineCount == d->lineCap) {
        u32 cap = d->lineCap == 0 ? 256 : d->lineCap * 2;
        u32* grown = (u32*) realloc(d->lines, cap * sizeof(u32));
        if(grown == NULL) {
            return;
        }

        d->lines = grown;
        d->lineCap = cap;
    }

    d->lines[d->lineCount++] = start;
}

// Where each line on screen starts: lines of the text cut at the width, at a space when there is one.
static void tv_layout(text_data* d) {
    float maxW = tv_text_width();
    d->lineCount = 0;
    tv_push_line(d, 0);

    u32 i = 0;
    float x = 0;
    u32 lastSpace = TV_NO_LINE;
    u32 lineStart = 0;
    while(i < d->len) {
        char c = d->buf[i];
        if(c == '\n') {
            i++;
            tv_push_line(d, i);
            lineStart = i;
            x = 0;
            lastSpace = TV_NO_LINE;
            continue;
        }

        u32 n = tv_char_len(d->buf + i, d->len - i);
        float w = tv_char_width(d->buf + i, n);
        if(x + w > maxW && x > 0) {
            u32 brk = lastSpace != TV_NO_LINE && lastSpace + 1 > lineStart ? lastSpace + 1 : i;
            tv_push_line(d, brk);
            lineStart = brk;
            x = 0;
            lastSpace = TV_NO_LINE;
            for(u32 j = brk; j < i;) {
                u32 m = tv_char_len(d->buf + j, i - j);
                x += tv_char_width(d->buf + j, m);
                if(d->buf[j] == ' ') {
                    lastSpace = j;
                }

                j += m;
            }

            continue;
        }

        if(c == ' ') {
            lastSpace = i;
        }

        x += w;
        i += n;
    }
}

static u32 tv_line_of(text_data* d, u32 offset) {
    u32 lo = 0;
    u32 hi = d->lineCount;
    while(hi - lo > 1) {
        u32 mid = (lo + hi) / 2;
        if(d->lines[mid] <= offset) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    return lo;
}

static u32 tv_line_end(text_data* d, u32 line) {
    u32 end = line + 1 < d->lineCount ? d->lines[line + 1] : d->len;
    if(line + 1 < d->lineCount && end > d->lines[line] && d->buf[end - 1] == '\n') {
        end--;
    }

    return end;
}

static float tv_width_between(text_data* d, u32 from, u32 to) {
    float x = 0;
    for(u32 i = from; i < to;) {
        u32 n = tv_char_len(d->buf + i, to - i);
        x += tv_char_width(d->buf + i, n);
        i += n;
    }

    return x;
}

static u32 tv_rows(text_data* d) {
    float h = (TOP_SCREEN_HEIGHT - WIDGET_TOP_BAR_HEIGHT) - 2 * 3 - (d->editing ? TV_STATUS_HEIGHT : 0);
    u32 rows = (u32) (h / tv_line_height());
    return rows < 1 ? 1 : rows;
}

static void tv_clamp_top(text_data* d) {
    u32 rows = tv_rows(d);
    u32 maxTop = d->lineCount > rows ? d->lineCount - rows : 0;
    if(d->top > maxTop) {
        d->top = maxTop;
    }
}

static void tv_show_cursor(text_data* d) {
    u32 rows = tv_rows(d);
    u32 line = tv_line_of(d, d->cursor);
    if(line < d->top) {
        d->top = line;
    } else if(line >= d->top + rows) {
        d->top = line + 1 - rows;
    }

    tv_clamp_top(d);
}

static void tv_message(text_data* d, const char* text) {
    snprintf(d->message, sizeof(d->message), "%s", text);
    d->messageAt = osGetTime();
}

// ---- editing ----

static bool tv_insert(text_data* d, const char* s, u32 n) {
    if(d->readOnly || n == 0) {
        return false;
    }

    if(d->len + n > TV_EDIT_MAX) {
        tv_message(d, _("The file is too large to grow more."));
        return false;
    }

    if(d->len + n + 1 > d->cap) {
        u32 cap = d->cap * 2 > d->len + n + 1 ? d->cap * 2 : d->len + n + 1;
        char* grown = (char*) realloc(d->buf, cap);
        if(grown == NULL) {
            tv_message(d, _("Not enough memory."));
            return false;
        }

        d->buf = grown;
        d->cap = cap;
    }

    memmove(d->buf + d->cursor + n, d->buf + d->cursor, d->len - d->cursor);
    memcpy(d->buf + d->cursor, s, n);
    d->len += n;
    d->cursor += n;
    d->buf[d->len] = '\0';
    d->modified = true;
    d->changes++;
    d->desiredX = -1;
    tv_layout(d);
    tv_show_cursor(d);
    return true;
}

static void tv_backspace(text_data* d) {
    if(d->readOnly || d->cursor == 0) {
        return;
    }

    u32 start = d->cursor - 1;
    while(start > 0 && ((u8) d->buf[start] & 0xC0) == 0x80) {
        start--;
    }

    memmove(d->buf + start, d->buf + d->cursor, d->len - d->cursor);
    d->len -= d->cursor - start;
    d->cursor = start;
    d->buf[d->len] = '\0';
    d->modified = true;
    d->changes++;
    d->desiredX = -1;
    tv_layout(d);
    tv_show_cursor(d);
}

static void tv_move_horizontal(text_data* d, int step) {
    if(step < 0) {
        if(d->cursor == 0) {
            return;
        }

        d->cursor--;
        while(d->cursor > 0 && ((u8) d->buf[d->cursor] & 0xC0) == 0x80) {
            d->cursor--;
        }
    } else {
        if(d->cursor >= d->len) {
            return;
        }

        d->cursor += tv_char_len(d->buf + d->cursor, d->len - d->cursor);
    }

    d->desiredX = -1;
    tv_show_cursor(d);
}

static void tv_move_vertical(text_data* d, int step) {
    u32 line = tv_line_of(d, d->cursor);
    if((step < 0 && line == 0) || (step > 0 && line + 1 >= d->lineCount)) {
        return;
    }

    if(d->desiredX < 0) {
        d->desiredX = tv_width_between(d, d->lines[line], d->cursor);
    }

    u32 target = (u32) ((int) line + step);
    u32 start = d->lines[target];
    u32 end = tv_line_end(d, target);
    // A line cut at the width ends where the next begins: stay on this line.
    if(target + 1 < d->lineCount && d->lines[target + 1] == end && end > start) {
        u32 p = end - 1;
        while(p > start && ((u8) d->buf[p] & 0xC0) == 0x80) {
            p--;
        }

        end = p;
    }

    float x = 0;
    u32 p = start;
    while(p < end) {
        u32 n = tv_char_len(d->buf + p, end - p);
        float w = tv_char_width(d->buf + p, n);
        if(x + w / 2 > d->desiredX) {
            break;
        }

        x += w;
        p += n;
    }

    d->cursor = p;
    tv_show_cursor(d);
}

static void tv_update_stats(text_data* d) {
    if(d->statChanges == d->changes && d->statCursor == d->cursor && d->statLine != 0) {
        return;
    }

    u32 line = 1;
    u32 lineStart = 0;
    for(u32 i = 0; i < d->cursor && i < d->len; i++) {
        if(d->buf[i] == '\n') {
            line++;
            lineStart = i + 1;
        }
    }

    u32 column = 1;
    for(u32 i = lineStart; i < d->cursor;) {
        i += tv_char_len(d->buf + i, d->cursor - i);
        column++;
    }

    d->statChanges = d->changes;
    d->statCursor = d->cursor;
    d->statLine = line;
    d->statColumn = column;
}

// ---- loading and saving ----

static bool tv_valid_utf8(const u8* p, u32 n) {
    for(u32 i = 0; i < n;) {
        u8 c = p[i];
        u32 len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if(len == 0 || i + len > n) {
            return false;
        }

        for(u32 j = 1; j < len; j++) {
            if((p[i + j] & 0xC0) != 0x80) {
                return false;
            }
        }

        i += len;
    }

    return true;
}

static bool tv_load(text_data* d) {
    u8* file = NULL;
    u32 size = 0;
    u64 full = 0;
    Result res = viewers_read_file(d->archive, d->path, TEXTVIEW_MAX_SIZE, &file, &size, &full);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to read the file.");
        return false;
    }

    d->fileSize = full;
    for(u32 i = 0; i < size && i < 4096; i++) {
        if(file[i] == 0) {
            free(file);
            prompt_display_notify("Failure", "This does not look like a text file.", COLOR_TEXT, NULL, NULL, NULL);
            return false;
        }
    }

    // Anything that is not UTF-8 is read as Latin-1.
    u8* text = file;
    u32 textSize = size;
    if(!tv_valid_utf8(file, size)) {
        text = (u8*) malloc((size_t) size * 2 + 1);
        if(text == NULL) {
            free(file);
            error_display(NULL, NULL, "Failed to allocate text viewer data.");
            return false;
        }

        textSize = 0;
        for(u32 i = 0; i < size; i++) {
            if(file[i] < 0x80) {
                text[textSize++] = file[i];
            } else {
                text[textSize++] = (u8) (0xC0 | (file[i] >> 6));
                text[textSize++] = (u8) (0x80 | (file[i] & 0x3F));
            }
        }

        free(file);
        d->converted = true;
    }

    // Line endings: kept as they were when the file is saved.
    u32 crlf = 0;
    u32 lf = 0;
    for(u32 i = 0; i < textSize; i++) {
        if(text[i] == '\n') {
            if(i > 0 && text[i - 1] == '\r') {
                crlf++;
            } else {
                lf++;
            }
        }
    }

    d->crlf = crlf > lf;
    u32 out = 0;
    for(u32 i = 0; i < textSize; i++) {
        if(text[i] == '\r' && i + 1 < textSize && text[i + 1] == '\n') {
            continue;
        }

        text[out++] = text[i];
    }

    d->cap = out + 4096;
    char* sized = (char*) realloc(text, d->cap);
    if(sized == NULL) {
        free(text);
        error_display(NULL, NULL, "Failed to allocate text viewer data.");
        return false;
    }

    d->buf = sized;

    d->buf[out] = '\0';
    d->len = out;
    d->truncated = full > size;
    d->readOnly = d->readOnly || d->truncated;
    return true;
}

static bool tv_save(text_data* d) {
    u32 extra = 0;
    if(d->crlf) {
        for(u32 i = 0; i < d->len; i++) {
            extra += d->buf[i] == '\n';
        }
    }

    u32 outLen = d->len + extra;
    char* out = (char*) malloc((size_t) outLen + 1);
    if(out == NULL) {
        error_display(NULL, NULL, "Failed to allocate text viewer data.");
        return false;
    }

    u32 o = 0;
    for(u32 i = 0; i < d->len; i++) {
        if(d->crlf && d->buf[i] == '\n') {
            out[o++] = '\r';
        }

        out[o++] = d->buf[i];
    }

    Result res = 0;
    FS_Path* fsPath = fs_make_path_utf8(d->path);
    if(fsPath != NULL) {
        Handle handle = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenFile(&handle, d->archive, *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
            if(R_SUCCEEDED(res = FSFILE_SetSize(handle, outLen))) {
                u32 done = 0;
                while(done < outLen && R_SUCCEEDED(res)) {
                    u32 chunk = outLen - done > 256 * 1024 ? 256 * 1024 : outLen - done;
                    u32 written = 0;
                    res = FSFILE_Write(handle, &written, done, out + done, chunk, FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME);
                    if(R_SUCCEEDED(res) && written == 0) {
                        res = R_APP_IO_ERROR;
                    }

                    done += written;
                }
            }

            FSFILE_Close(handle);
        }

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    free(out);

    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to save the file.");
        return false;
    }

    FSUSER_ControlArchive(d->archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
    d->modified = false;
    d->converted = false;
    d->fileSize = outLen;
    tv_message(d, _("Saved."));
    return true;
}

static void tv_close(text_data* d) {
    ui_view* view = d->view;
    ui_pop();
    free(d->buf);
    free(d->lines);
    free(d);
    ui_destroy(view);
}

// ---- drawing ----

static void tv_draw_top(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    text_data* d = (text_data*) v;
    const theme_current* t = theme();

    screen_set_depth(0);
    screen_fill_rect(x1, y1, x2 - x1, y2 - y1, t->palette->surface);

    float lineH = tv_line_height();
    u32 rows = tv_rows(d);
    float textTop = y1 + 3;

    for(u32 r = 0; r < rows && d->top + r < d->lineCount; r++) {
        u32 k = d->top + r;
        u32 start = d->lines[k];
        u32 end = k + 1 < d->lineCount ? d->lines[k + 1] : d->len;

        char seg[768];
        u32 o = 0;
        for(u32 i = start; i < end && o < sizeof(seg) - 6; i++) {
            char c = d->buf[i];
            if(c == '\n' || c == '\r') {
                continue;
            }

            if(c == '\t') {
                memcpy(seg + o, "    ", 4);
                o += 4;
            } else {
                seg[o++] = c;
            }
        }

        seg[o] = '\0';
        float y = textTop + r * lineH;
        if(o > 0) {
            screen_draw_text(seg, x1 + TV_PAD, y, TV_SCALE, t->palette->text);
        }

        if(d->editing && d->cursor >= start && (d->cursor < end || (k + 1 == d->lineCount && d->cursor == end))
           && !(d->cursor == end && k + 1 < d->lineCount && d->buf[end - 1] != '\n')) {
            if((osGetTime() / 500) % 2 == 0) {
                float cx = x1 + TV_PAD + tv_width_between(d, start, d->cursor);
                screen_fill_rect(cx, y, 1.5f, lineH - 1, t->accent);
            }
        }
    }

    if(d->lineCount > rows) {
        widget_scrollbar(x2 - WIDGET_SCROLLBAR_WIDTH - 2, y1 + 3, (rows * lineH), (float) d->top, (float) rows, (float) d->lineCount);
    }

    if(d->editing) {
        float sy = y2 - TV_STATUS_HEIGHT;
        screen_fill_rect(x1, sy, x2 - x1, TV_STATUS_HEIGHT, t->palette->background);
        screen_fill_rect(x1, sy, x2 - x1, 1, t->palette->border);
        tv_update_stats(d);

        char status[96];
        bool showMessage = d->message[0] != '\0' && osGetTime() - d->messageAt < 2500;
        if(showMessage) {
            snprintf(status, sizeof(status), "%s", d->message);
        } else {
            snprintf(status, sizeof(status), "%s %lu, %s %lu%s", _("Line"), d->statLine, _("Col"), d->statColumn, d->modified ? "   *" : "");
        }

        widget_text_fit(status, x1 + TV_PAD, sy + (TV_STATUS_HEIGHT - widget_text_height(WIDGET_SMALL_SCALE)) / 2, WIDGET_SMALL_SCALE,
                        showMessage ? t->accent : t->palette->textMuted, x2 - x1 - 2 * TV_PAD);
    }
}

static void tv_build_keys(text_data* d) {
    const key_row* rows = d->page == 0 ? kb_letters : d->page == 1 ? kb_symbols : kb_extra;
    float unit = (BOTTOM_SCREEN_WIDTH - 8) / 10.0f;
    float y = WIDGET_TITLE_BAR_HEIGHT + 4;
    d->keyCount = 0;

    for(int r = -1; r < 4; r++) {
        const key_row* row = r < 0 ? &kb_toolbar : &rows[r];
        float h = r < 0 ? 28 : 34;
        float x = 4;
        for(u32 i = 0; i < row->count && d->keyCount < KB_KEYS_MAX; i++) {
            const key_def* def = &row->keys[i];
            float w = def->width * unit;
            // The last two rows of keys are a little shorter than the others' unit grid: all rows share it.
            d->keys[d->keyCount++] = (key_rect) {x + 1, y, w - 2, h, def};
            x += w;
        }

        y += h + 3;
    }
}

static void tv_draw_key(text_data* d, const key_rect* k, bool pressed) {
    const theme_current* t = theme();
    bool special = k->def->action != K_CHAR;
    u32 fill = pressed ? t->accent : special ? t->palette->background : t->palette->surface;
    u32 edge = pressed ? t->accent : t->palette->border;
    u32 ink = pressed ? t->onAccent : t->palette->text;
    screen_frame_round_rect(k->x, k->y, k->w, k->h, t->def->radiusSmall, t->def->border, edge, fill);

    int icon = -1;
    const char* label = NULL;
    switch(k->def->action) {
        case K_CHAR: label = d->shift != 0 ? k->def->upper : k->def->lower; break;
        case K_SHIFT: icon = ICON_CHEVRON_UP; if(d->shift == 2) { ink = pressed ? t->onAccent : t->accent; } break;
        case K_BACK: icon = ICON_ARROW_LEFT; break;
        case K_PAGE_LETTERS: label = "abc"; break;
        case K_PAGE_SYMBOLS: label = "123"; break;
        case K_PAGE_EXTRA: label = "áé="; break;
        case K_SPACE: break;
        case K_ENTER: icon = ICON_ARROW_BACK_UP; break;
        case K_LEFT: icon = ICON_CHEVRON_LEFT; break;
        case K_RIGHT: icon = ICON_CHEVRON_RIGHT; break;
        case K_UP: icon = ICON_CHEVRON_UP; break;
        case K_DOWN: icon = ICON_CHEVRON_DOWN; break;
        case K_SAVE: icon = ICON_DEVICE_FLOPPY; label = _("Save"); break;
        case K_DONE: icon = ICON_CHECK; label = _("Done"); break;
    }

    if(icon >= 0 && label != NULL) {
        float tw = widget_text_width(label, WIDGET_TEXT_SCALE);
        float total = 16 + 6 + tw;
        float x = k->x + (k->w - total) / 2;
        screen_draw_icon((u32) icon, x, k->y + (k->h - 16) / 2, 16, ink);
        screen_draw_text(label, x + 22, k->y + (k->h - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink);
    } else if(icon >= 0) {
        screen_draw_icon((u32) icon, k->x + (k->w - 16) / 2, k->y + (k->h - 16) / 2, 16, ink);
    } else if(label != NULL) {
        widget_text_centered(label, k->x, k->x + k->w, k->y + (k->h - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink);
    }
}

#define TV_BUTTONS 3

static void tv_button_rect(int i, float* x, float* y, float* w, float* h) {
    float gap = 6;
    *w = (BOTTOM_SCREEN_WIDTH - 16 - gap * (TV_BUTTONS - 1)) / TV_BUTTONS;
    *h = 40;
    *x = 8 + i * (*w + gap);
    *y = BOTTOM_SCREEN_HEIGHT - WIDGET_HINT_BAR_HEIGHT - 8 - *h;
}

static void tv_draw_bottom(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    text_data* d = (text_data*) v;
    const theme_current* t = theme();

    if(d->editing) {
        for(u32 i = 0; i < d->keyCount; i++) {
            tv_draw_key(d, &d->keys[i], d->pressedKey == (int) i);
        }

        return;
    }

    widget_card(8, y1 + 8, x2 - x1 - 16, 84);
    float tx = 18;
    float ty = y1 + 16;
    widget_text_fit(d->name, tx, ty, WIDGET_TEXT_SCALE, t->palette->text, x2 - tx - 18);
    ty += widget_text_height(WIDGET_TEXT_SCALE) + 4;

    char line[128];
    char size[32];
    snprintf(size, sizeof(size), d->fileSize < 1024 ? "%.0f %s" : "%.1f %s", ui_get_display_size(d->fileSize), ui_get_display_size_units(d->fileSize));
    snprintf(line, sizeof(line), "%lu %s   %s", d->lineCount, _("lines"), size);
    widget_text_fit(line, tx, ty, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - tx - 18);
    ty += widget_text_height(WIDGET_SMALL_SCALE) + 3;

    if(d->truncated) {
        widget_text_fit(_("Only the start is shown (read only)."), tx, ty, WIDGET_SMALL_SCALE, t->palette->danger, x2 - tx - 18);
    } else if(d->converted) {
        widget_text_fit(_("Not UTF-8: read as Latin-1, saved as UTF-8."), tx, ty, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - tx - 18);
    } else if(d->crlf) {
        widget_text_fit(_("Windows line endings (kept)."), tx, ty, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - tx - 18);
    }

    if(d->modified) {
        widget_badge(tx, y1 + 8 + 84 - 22, _("Unsaved changes"), t->accentSoft, t->accent);
    }

    static const u32 icons[TV_BUTTONS] = {ICON_PENCIL, ICON_CHEVRON_UP, ICON_CHEVRON_DOWN};
    const char* labels[TV_BUTTONS] = {_("Edit"), _("Start"), _("End")};
    for(int i = 0; i < TV_BUTTONS; i++) {
        float x, y, w, h;
        tv_button_rect(i, &x, &y, &w, &h);
        bool pressed = d->pressedButton == i;
        bool disabled = i == 0 && d->readOnly;
        u32 fill = pressed ? t->accent : t->palette->surface;
        u32 edge = pressed ? t->accent : t->palette->border;
        u32 ink = pressed ? t->onAccent : disabled ? t->palette->textMuted : t->palette->text;
        screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, edge, fill);

        float tw = widget_text_width(labels[i], WIDGET_TEXT_SCALE);
        float bx = x + (w - (16 + 6 + tw)) / 2;
        screen_draw_icon(icons[i], bx, y + (h - 16) / 2, 16, ink);
        screen_draw_text(labels[i], bx + 22, y + (h - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, ink);
    }
}

// ---- input ----

static void tv_enter_edit(text_data* d) {
    if(d->readOnly) {
        return;
    }

    d->editing = true;
    d->page = 0;
    d->shift = 0;
    tv_build_keys(d);
    d->cursor = d->lines[d->top < d->lineCount ? d->top : 0];
    d->desiredX = -1;
    tv_show_cursor(d);
}

static void tv_leave_edit(text_data* d) {
    d->editing = false;
    d->pressedKey = -1;
    tv_clamp_top(d);
}

static void tv_type_char(text_data* d, const key_def* def) {
    const char* s = d->shift != 0 ? def->upper : def->lower;
    tv_insert(d, s, (u32) strlen(s));
    if(d->shift == 1) {
        d->shift = 0;
    }
}

// Runs a key; false when it is not one that repeats while held.
static bool tv_run_key(text_data* d, const key_def* def) {
    switch(def->action) {
        case K_CHAR: tv_type_char(d, def); return true;
        case K_SHIFT: d->shift = (d->shift + 1) % 3; return false;
        case K_BACK: tv_backspace(d); return true;
        case K_PAGE_LETTERS: d->page = 0; tv_build_keys(d); return false;
        case K_PAGE_SYMBOLS: d->page = 1; tv_build_keys(d); return false;
        case K_PAGE_EXTRA: d->page = 2; tv_build_keys(d); return false;
        case K_SPACE: tv_insert(d, " ", 1); return true;
        case K_ENTER: tv_insert(d, "\n", 1); return true;
        case K_LEFT: tv_move_horizontal(d, -1); return true;
        case K_RIGHT: tv_move_horizontal(d, 1); return true;
        case K_UP: tv_move_vertical(d, -1); return true;
        case K_DOWN: tv_move_vertical(d, 1); return true;
        case K_SAVE: tv_save(d); return false;
        case K_DONE: tv_leave_edit(d); return false;
    }

    return false;
}

// True on a press and, held, every so often after a pause.
static bool tv_repeat(text_data* d, int slot, u32 mask) {
    if(hidKeysDown() & mask) {
        d->holdAt[slot] = osGetTime() + 400;
        return true;
    }

    if((hidKeysHeld() & mask) && d->holdAt[slot] != 0 && osGetTime() >= d->holdAt[slot]) {
        d->holdAt[slot] = osGetTime() + 60;
        return true;
    }

    if(!(hidKeysHeld() & mask)) {
        d->holdAt[slot] = 0;
    }

    return false;
}

static void tv_close_response(ui_view* view, void* v, u32 response) {
    text_data* d = (text_data*) v;
    if(response == 0) {
        if(tv_save(d)) {
            tv_close(d);
        }
    } else if(response == 1) {
        tv_close(d);
    }
}

static void tv_try_close(text_data* d) {
    if(!d->modified) {
        tv_close(d);
        return;
    }

    static const char* options[3] = {"Save", "Discard", "Cancel"};
    static u32 buttons[3] = {KEY_A, KEY_X, KEY_B};
    prompt_display_multi_choice("Unsaved changes", "Save the changes before closing?", COLOR_TEXT, options, buttons, 3, d, tv_draw_top, tv_close_response);
}

static void tv_update(ui_view* view, void* v, float bx1, float by1, float bx2, float by2) {
    text_data* d = (text_data*) v;

    snprintf(d->title, sizeof(d->title), "%s%s", d->modified ? "* " : "", d->name);
    view->name = d->title;
    view->info = d->editing ? _(" done   START save    delete    space") : _(" edit     pages    back");

    if(hidKeysDown() & KEY_B) {
        if(d->editing) {
            tv_leave_edit(d);
        } else {
            tv_try_close(d);
        }

        return;
    }

    touchPosition pos;
    hidTouchRead(&pos);

    if(d->editing) {
        if(hidKeysDown() & KEY_START) {
            tv_save(d);
        }

        if(tv_repeat(d, 0, KEY_DLEFT)) tv_move_horizontal(d, -1);
        if(tv_repeat(d, 1, KEY_DRIGHT)) tv_move_horizontal(d, 1);
        if(tv_repeat(d, 2, KEY_DUP)) tv_move_vertical(d, -1);
        if(tv_repeat(d, 3, KEY_DDOWN)) tv_move_vertical(d, 1);
        if(tv_repeat(d, 4, KEY_X)) tv_backspace(d);
        if(hidKeysDown() & KEY_Y) tv_insert(d, " ", 1);
        if(hidKeysDown() & KEY_A) tv_insert(d, "\n", 1);
        if(hidKeysDown() & KEY_SELECT) d->shift = (d->shift + 1) % 3;

        u32 rows = tv_rows(d);
        if(hidKeysDown() & KEY_L) {
            for(u32 i = 0; i + 1 < rows; i++) tv_move_vertical(d, -1);
        }

        if(hidKeysDown() & KEY_R) {
            for(u32 i = 0; i + 1 < rows; i++) tv_move_vertical(d, 1);
        }

        if(hidKeysDown() & KEY_TOUCH) {
            d->pressedKey = -1;
            for(u32 i = 0; i < d->keyCount; i++) {
                const key_rect* k = &d->keys[i];
                if(widget_touched(&pos, k->x, k->y, k->w, k->h)) {
                    d->pressedKey = (int) i;
                    break;
                }
            }

            if(d->pressedKey >= 0) {
                const key_def* def = d->keys[d->pressedKey].def;
                bool repeats = tv_run_key(d, def);
                d->repeatAt = repeats ? osGetTime() + 450 : 0;
                // Keys may have been rebuilt (page change): do not keep a stale press.
                if(!repeats && def->action != K_SHIFT && def->action != K_SAVE) {
                    d->pressedKey = -1;
                }
            }
        } else if(hidKeysHeld() & KEY_TOUCH) {
            if(d->pressedKey >= 0 && d->repeatAt != 0 && osGetTime() >= d->repeatAt) {
                const key_rect* k = &d->keys[d->pressedKey];
                if(widget_touched(&pos, k->x, k->y, k->w, k->h)) {
                    tv_run_key(d, k->def);
                    d->repeatAt = osGetTime() + 60;
                }
            }
        } else if(hidKeysUp() & KEY_TOUCH) {
            d->pressedKey = -1;
            d->repeatAt = 0;
        }

        return;
    }

    // Viewing.
    u32 rows = tv_rows(d);
    if(tv_repeat(d, 2, KEY_DUP) && d->top > 0) d->top--;
    if(tv_repeat(d, 3, KEY_DDOWN)) { d->top++; tv_clamp_top(d); }
    if(hidKeysDown() & KEY_L) { d->top = d->top > rows - 1 ? d->top - (rows - 1) : 0; }
    if(hidKeysDown() & KEY_R) { d->top += rows - 1; tv_clamp_top(d); }
    if(hidKeysDown() & KEY_A) tv_enter_edit(d);

    if(hidKeysDown() & KEY_TOUCH) {
        d->pressedButton = -1;
        for(int i = 0; i < TV_BUTTONS; i++) {
            float x, y, w, h;
            tv_button_rect(i, &x, &y, &w, &h);
            if(widget_touched(&pos, x, y, w, h)) {
                d->pressedButton = i;
            }
        }

        d->dragging = d->pressedButton < 0;
        d->lastY = pos.py;
        d->dragRemainder = 0;
    } else if(hidKeysHeld() & KEY_TOUCH) {
        if(d->dragging) {
            // The text follows the finger.
            d->dragRemainder += (float) (d->lastY - pos.py) / tv_line_height() * 1.4f;
            int lines = (int) d->dragRemainder;
            d->dragRemainder -= lines;
            if(lines < 0 && d->top < (u32) -lines) {
                d->top = 0;
            } else {
                d->top = (u32) ((int) d->top + lines);
            }

            tv_clamp_top(d);
        }

        if(d->pressedButton >= 0) {
            float x, y, w, h;
            tv_button_rect(d->pressedButton, &x, &y, &w, &h);
            if(!widget_touched(&pos, x, y, w, h)) {
                d->pressedButton = -1;
            }
        }

        d->lastY = pos.py;
    } else if(hidKeysUp() & KEY_TOUCH) {
        int button = d->pressedButton;
        d->pressedButton = -1;
        d->dragging = false;
        if(button == 0) {
            tv_enter_edit(d);
        } else if(button == 1) {
            d->top = 0;
        } else if(button == 2) {
            d->top = d->lineCount;
            tv_clamp_top(d);
        }
    }
}

void textview_open(FS_Archive archive, const char* path, const char* name, bool readOnly) {
    text_data* d = (text_data*) calloc(1, sizeof(text_data));
    if(d == NULL) {
        error_display(NULL, NULL, "Failed to allocate text viewer data.");
        return;
    }

    d->archive = archive;
    string_copy(d->path, path, FILE_PATH_MAX);
    string_copy(d->name, name, FILE_NAME_MAX);
    d->readOnly = readOnly;
    d->pressedKey = -1;
    d->pressedButton = -1;
    d->desiredX = -1;

    if(!tv_load(d)) {
        free(d->buf);
        free(d);
        return;
    }

    tv_layout(d);

    ui_view* view = ui_create();
    view->name = d->name;
    view->info = "";
    view->data = d;
    view->update = tv_update;
    view->drawTop = tv_draw_top;
    view->drawBottom = tv_draw_bottom;
    d->view = view;
    ui_push(view);
}
