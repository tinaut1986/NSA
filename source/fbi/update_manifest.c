#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "update_manifest.h"

// rank orders the builds of one number: dev (1) < beta (2) < plain X.Y.Z.
typedef struct {
    unsigned part[3];
    unsigned rank;
    bool pre;
} version;

static bool parse_version(const char* s, version* v) {
    memset(v, 0, sizeof(*v));
    if(s == NULL) {
        return false;
    }

    if(*s == 'v') {
        s++;
    }

    for(int i = 0; i < 3; i++) {
        if(!isdigit((unsigned char) *s)) {
            return false;
        }

        unsigned n = 0;
        int digits = 0;
        while(isdigit((unsigned char) *s)) {
            n = n * 10 + (*s++ - '0');
            if(++digits > 5 || n > 65535) {
                return false;
            }
        }

        v->part[i] = n;
        if(i < 2 && *s++ != '.') {
            return false;
        }
    }

    if(*s == '-') {
        s++;
        if(strcmp(s, "beta") == 0) {
            v->pre = true;
            v->rank = 2;
            return true;
        }

        if(strncmp(s, "dev", 3) != 0) {
            return false;
        }

        for(s += 3; *s; s++) {
            if(!isalnum((unsigned char) *s) && *s != '.' && *s != '+') {
                return false;
            }
        }

        v->pre = true;
        v->rank = 1;
        return true;
    }

    return *s == '\0';
}

bool update_valid_version(const char* s) {
    version v;
    return parse_version(s, &v);
}

bool update_is_newer(const char* candidate, const char* installed) {
    version a;
    version b;
    if(!parse_version(candidate, &a) || !parse_version(installed, &b)) {
        return false;
    }

    for(int i = 0; i < 3; i++) {
        if(a.part[i] != b.part[i]) {
            return a.part[i] > b.part[i];
        }
    }

    if(a.pre != b.pre) {
        return !a.pre;
    }

    return a.rank > b.rank;
}

#define DOWNLOAD_PREFIX "https://github.com/" APP_REPOSITORY "/releases/download/"

bool update_allowed_download_url(const char* url) {
    return url != NULL && strncmp(url, DOWNLOAD_PREFIX, strlen(DOWNLOAD_PREFIX)) == 0;
}

static bool get_text(json_t* o, const char* key, char* dst, size_t cap) {
    json_t* v = json_object_get(o, key);
    if(!json_is_string(v) || json_string_length(v) >= cap || strlen(json_string_value(v)) != json_string_length(v)) {
        return false;
    }

    strcpy(dst, json_string_value(v));
    return true;
}

// A release this build can install: published, a vX.Y.Z tag, and the asset for
// this build type (CIA or 3DSX) at the expected URL with a sane size.
static bool candidate(json_t* o, bool homebrew, update_release* out) {
    if(!json_is_object(o) || !json_is_false(json_object_get(o, "draft")) || !json_is_boolean(json_object_get(o, "prerelease"))) {
        return false;
    }

    char tag[48];
    if(!get_text(o, "tag_name", tag, sizeof(tag)) || tag[0] != 'v' || !update_valid_version(tag) || strchr(tag, '-') != NULL) {
        return false;
    }

    char name[96];
    char expected[512];
    snprintf(name, sizeof(name), APP_NAME "-%s.%s", tag, homebrew ? "3dsx" : "cia");
    snprintf(expected, sizeof(expected), DOWNLOAD_PREFIX "%s/%s", tag, name);

    json_t* assets = json_object_get(o, "assets");
    if(!json_is_array(assets)) {
        return false;
    }

    size_t i;
    json_t* a;
    json_array_foreach(assets, i, a) {
        char n[96];
        char url[512];
        if(!get_text(a, "name", n, sizeof(n)) || strcmp(n, name) != 0) {
            continue;
        }

        if(!get_text(a, "browser_download_url", url, sizeof(url)) || strcmp(url, expected) != 0) {
            return false;
        }

        json_t* s = json_object_get(a, "size");
        if(!json_is_integer(s) || json_integer_value(s) < 4096 || json_integer_value(s) > UPDATE_MAX_FILE) {
            return false;
        }

        memset(out, 0, sizeof(*out));
        strcpy(out->version, tag);
        strcpy(out->url, url);
        out->size = (uint32_t) json_integer_value(s);
        out->prerelease = json_is_true(json_object_get(o, "prerelease"));

        json_t* body = json_object_get(o, "body");
        if(json_is_string(body)) {
            snprintf(out->notes, sizeof(out->notes), "%s", json_string_value(body));
        }

        return true;
    }

    return false;
}

// The comparable version of a release: a pre-release tag vX.Y.Z is "X.Y.Z-beta".
static void comparable(const update_release* r, char* out, size_t size) {
    snprintf(out, size, "%s%s", r->version, r->prerelease ? "-beta" : "");
}

// The user-facing part of one release body: its notes block, or all of it.
static void notes_block(const char* body, const char** start, size_t* len) {
    const char* a = strstr(body, UPDATE_NOTES_OPEN);
    if(a == NULL) {
        *start = body;
        *len = strlen(body);
        return;
    }

    a += strlen(UPDATE_NOTES_OPEN);
    const char* b = strstr(a, UPDATE_NOTES_CLOSE);
    *start = a;
    *len = b != NULL ? (size_t) (b - a) : strlen(a);
}

int update_parse_history(const char* data, size_t size, const char* installed, char* out, size_t cap, unsigned* pages) {
    *pages = 0;
    if(cap > 0) {
        out[0] = '\0';
    }

    json_error_t err;
    json_t* root = json_loadb(data, size, JSON_REJECT_DUPLICATES, &err);
    json_t* releases = root != NULL ? json_object_get(root, "releases") : NULL;
    json_t* total = root != NULL ? json_object_get(root, "pages") : NULL;
    if(!json_is_object(root) || json_integer_value(json_object_get(root, "format")) != UPDATE_HISTORY_FORMAT || !json_is_array(releases) ||
       !json_is_integer(total) || json_integer_value(total) < 0 || json_integer_value(total) > 100000) {
        json_decref(root);
        return -1;
    }

    *pages = (unsigned) json_integer_value(total);

    int count = 0;
    size_t used = 0;
    size_t i;
    json_t* o;
    json_array_foreach(releases, i, o) {
        char tag[48];
        json_t* notes = json_object_get(o, "notes");
        json_t* pre = json_object_get(o, "prerelease");
        if(!json_is_object(o) || !get_text(o, "tag", tag, sizeof(tag)) || tag[0] != 'v' || strchr(tag, '-') != NULL || !update_valid_version(tag) ||
           !json_is_boolean(pre) || !json_is_string(notes)) {
            continue;
        }

        // Betas and stables alike, up to the installed build: a beta's notes are not repeated by the stable after it.
        char cmp[64];
        snprintf(cmp, sizeof(cmp), "%s%s", tag, json_is_true(pre) ? "-beta" : "");
        if(installed != NULL && update_is_newer(cmp, installed)) {
            continue;
        }

        // A release that does not fit is left out with the ones after it, never cut in half.
        size_t len = json_string_length(notes);
        int head = snprintf(out + used, cap - used, "%s## %s%s\n", used ? "\n" : "", tag, json_is_true(pre) ? " BETA" : "");
        if(head < 0 || (size_t) head + len + 1 > cap - used) {
            out[used] = '\0';
            break;
        }

        used += (size_t) head;
        memcpy(out + used, json_string_value(notes), len);
        used += len;
        out[used] = '\0';
        count++;
    }

    json_decref(root);
    return count;
}

int update_parse_releases(const char* data, size_t size, bool betas, bool homebrew, const char* installed, update_release* out) {
    memset(out, 0, sizeof(*out));

    json_error_t err;
    json_t* root = json_loadb(data, size, JSON_REJECT_DUPLICATES, &err);
    if(root == NULL) {
        return -1;
    }

    if(!json_is_array(root)) {
        json_decref(root);
        return -1;
    }

    update_release* r = (update_release*) malloc(sizeof(update_release));
    char* acc = (char*) malloc(UPDATE_NOTES_MAX);
    if(r == NULL || acc == NULL) {
        free(r);
        free(acc);
        json_decref(root);
        return -1;
    }

    int result = 0;
    char best[64] = "";

    // A release this build cannot use (no asset, bad URL) is skipped, not
    // fatal: the list holds every old release too.
    size_t i;
    json_t* o;
    json_array_foreach(root, i, o) {
        if(!json_is_object(o) || (!betas && json_is_true(json_object_get(o, "prerelease"))) || !candidate(o, homebrew, r)) {
            continue;
        }

        char cmp[64];
        comparable(r, cmp, sizeof(cmp));
        if(result == 0 || update_is_newer(cmp, best)) {
            *out = *r;
            snprintf(best, sizeof(best), "%s", cmp);
            result = 1;
        }
    }

    if(result == 1 && installed != NULL) {
        // Up to date: nothing is newer, so the notes are the channel's history.
        const char* since = update_is_newer(best, installed) ? installed : "0.0.0";
        size_t used = 0;
        unsigned count = 0;
        acc[0] = '\0';

        // On the stable channel what changed on the way to the newest stable
        // includes the betas before it, so their notes count too. A beta newer
        // than that stable, or one the install already has, does not.
        json_array_foreach(root, i, o) {
            if(!candidate(o, homebrew, r)) {
                continue;
            }

            char cmp[64];
            comparable(r, cmp, sizeof(cmp));
            if(!update_is_newer(cmp, since) || (!betas && r->prerelease && update_is_newer(cmp, best))) {
                continue;
            }

            const char* block;
            size_t len;
            notes_block(r->notes, &block, &len);

            // The block's own "## What's new": under the version heading it would read as another release.
            while(len > 0 && (*block == '\n' || *block == '\r' || *block == ' ')) {
                block++;
                len--;
            }

            if(len > 3 && strncmp(block, "## ", 3) == 0) {
                const char* eol = memchr(block, '\n', len);
                size_t skip = eol != NULL ? (size_t) (eol - block) + 1 : len;
                block += skip;
                len -= skip;
            }

            int head = snprintf(acc + used, UPDATE_NOTES_MAX - used, "%s## %s%s\n", count ? "\n" : "", r->version, r->prerelease ? " BETA" : "");
            if(head < 0 || (size_t) head >= UPDATE_NOTES_MAX - used) {
                break;
            }

            used += (size_t) head;
            if(len > UPDATE_NOTES_MAX - used - 1) {
                len = UPDATE_NOTES_MAX - used - 1;
            }

            memcpy(acc + used, block, len);
            used += len;
            acc[used] = '\0';
            count++;
        }

        snprintf(out->notes, sizeof(out->notes), UPDATE_NOTES_OPEN "\n%s\n" UPDATE_NOTES_CLOSE "\n", acc);
    }

    free(r);
    free(acc);
    json_decref(root);
    return result;
}

// Inline Markdown and HTML out of one line of notes, appended to out at *n: tags, images, link destinations (the text
// stays), emphasis and code marks, and four-byte UTF-8 (emoji: not in the system font). Literal brackets stay.
static void notes_inline(const char* src, size_t len, char* out, size_t* n) {
    for(size_t i = 0; i < len;) {
        unsigned char c = (unsigned char) src[i];
        if(c == '<') {
            const char* end = memchr(src + i, '>', len - i);
            if(end != NULL) {
                i = (size_t) (end - src) + 1;
                continue;
            }
        }

        if(c == '!' && i + 1 < len && src[i + 1] == '[') {
            const char* mid = memchr(src + i, ']', len - i);
            const char* end = mid != NULL && (size_t) (mid - src) + 1 < len && mid[1] == '(' ? memchr(mid, ')', len - (size_t) (mid - src)) : NULL;
            if(end != NULL) {
                i = (size_t) (end - src) + 1;
                continue;
            }
        }

        if(c == '[') {
            // [text](url): the text only.
            const char* mid = memchr(src + i, ']', len - i);
            const char* end = mid != NULL && (size_t) (mid - src) + 1 < len && mid[1] == '(' ? memchr(mid, ')', len - (size_t) (mid - src)) : NULL;
            if(end != NULL) {
                notes_inline(src + i + 1, (size_t) (mid - src) - i - 1, out, n);
                i = (size_t) (end - src) + 1;
                continue;
            }
        }

        if(c == '*' || c == '`' || c == '\r') {
            i++;
            continue;
        }

        if(c >= 0xF0) {
            i++;
            while(i < len && ((unsigned char) src[i] & 0xC0) == 0x80) {
                i++;
            }

            continue;
        }

        out[(*n)++] = (char) c;
        i++;
    }
}

char* update_notes_items(const char* md) {
    // The notes block when there is one, else the commit list, else everything.
    size_t limit = md != NULL ? strlen(md) : 0;
    const char* section = md != NULL ? strstr(md, UPDATE_NOTES_OPEN) : NULL;
    if(section != NULL) {
        md = section + strlen(UPDATE_NOTES_OPEN);
        const char* stop = strstr(md, UPDATE_NOTES_CLOSE);
        limit = stop != NULL ? (size_t) (stop - md) : strlen(md);
    } else if(md != NULL && (section = strstr(md, "## Changelog")) != NULL) {
        md = section;
        limit = strlen(md);
    }

    // Each item gains a kind and a newline at most; a line's own text never grows.
    char* out = (char*) malloc(limit * 2 + 4);
    if(out == NULL) {
        return NULL;
    }

    size_t n = 0;
    bool open = false;   // The last item can take more text (a bullet or a paragraph, not a heading).
    for(size_t i = 0; i < limit;) {
        const char* line = md + i;
        const char* eol = memchr(line, '\n', limit - i);
        size_t len = eol != NULL ? (size_t) (eol - line) : limit - i;
        i += len + (eol != NULL ? 1 : 0);

        while(len > 0 && (line[0] == ' ' || line[0] == '\t')) {
            line++;
            len--;
        }

        while(len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r' || line[len - 1] == '\t')) {
            len--;
        }

        // A blank line ends an item; a quote is the beta notice of a release body.
        if(len == 0 || line[0] == '>') {
            open = false;
            continue;
        }

        char kind = 0;
        if(line[0] == '#') {
            while(len > 0 && line[0] == '#') {
                line++;
                len--;
            }

            kind = 'H';
        } else if(len >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
            line += 2;
            len -= 2;
            kind = 'B';
        }

        while(len > 0 && line[0] == ' ') {
            line++;
            len--;
        }

        size_t start = n;
        if(kind == 0 && open) {
            // A line the notes broke by hand: the same item goes on.
            n--;   // Its newline.
            out[n++] = ' ';
            size_t mark = n;
            notes_inline(line, len, out, &n);
            if(n == mark) {
                n--;   // Nothing but tags: no stray space.
            }

            out[n++] = '\n';
            continue;
        }

        out[n++] = kind != 0 ? kind : 'P';
        size_t mark = n;
        notes_inline(line, len, out, &n);
        if(n == mark) {
            // Only tags or markers (an HTML comment, <details>): no item.
            n = start;
            continue;
        }

        out[n++] = '\n';
        open = kind != 'H';
    }

    out[n] = '\0';
    return out;
}
