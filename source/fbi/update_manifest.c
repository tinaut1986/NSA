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

unsigned update_format_notes(const char* md, char lines[][UPDATE_LINE_MAX], unsigned capacity, unsigned width) {
    if(width == 0 || width >= UPDATE_LINE_MAX) {
        width = UPDATE_LINE_MAX - 1;
    }

    char* clean = (char*) malloc(UPDATE_NOTES_MAX);
    if(clean == NULL) {
        return 0;
    }

    // Show the notes block when there is one, else the commit list.
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

    // Image/link destinations and Markdown markers are presentation only.
    size_t n = 0;
    for(size_t i = 0; i < limit && md[i] && n < UPDATE_NOTES_MAX - 1;) {
        if(md[i] == '<') {
            const char* end = memchr(md + i, '>', limit - i);
            if(end != NULL) {
                i = (size_t) (end - md) + 1;
                continue;
            }
        }

        if(md[i] == '!' && md[i + 1] == '[') {
            const char* end = memchr(md + i, ')', limit - i);
            if(end != NULL) {
                i = (size_t) (end - md) + 1;
                continue;
            }
        }

        if(md[i] == '[') {
            i++;
            continue;
        }

        if(md[i] == ']' && md[i + 1] == '(') {
            const char* end = memchr(md + i + 2, ')', limit - i - 2);
            if(end != NULL) {
                i = (size_t) (end - md) + 1;
                continue;
            }
        }

        unsigned char c = (unsigned char) md[i++];
        if(c == '*' || c == '`' || c == '#' || c == '\r' || c >= 128) {
            continue;
        }

        clean[n++] = (char) c;
    }

    clean[n] = '\0';

    unsigned count = 0;
    const char* p = clean;
    while(*p && count < capacity) {
        // Drop leading spaces and runs of blank lines.
        while(*p == ' ' || (*p == '\n' && (count == 0 || lines[count - 1][0] == '\0'))) {
            p++;
        }

        if(!*p) {
            break;
        }

        const char* end = strchr(p, '\n');
        if(end == NULL) {
            end = p + strlen(p);
        }

        size_t take = (size_t) (end - p);
        if(take > width) {
            take = width;
            while(take > 0 && p[take] != ' ') {
                take--;
            }

            if(take == 0) {
                take = width;
            }
        }

        memcpy(lines[count], p, take);
        lines[count++][take] = '\0';
        p += take;
        if(*p == '\n') {
            p++;
        }
    }

    // A trailing blank line is noise.
    while(count > 0 && lines[count - 1][0] == '\0') {
        count--;
    }

    if(count == 0 && capacity > 0) {
        strcpy(lines[0], "No changelog provided.");
        count = 1;
    }

    free(clean);
    return count;
}
