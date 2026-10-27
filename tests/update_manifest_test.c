// Host test for source/fbi/update_manifest.c. Build and run with `make -C tests`.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "../source/fbi/update_manifest.h"

static json_t* make(const char* tag, bool pre, const char* notes) {
    char name[100];
    char url[512];
    char body[256];
    snprintf(name, sizeof(name), APP_NAME "-%s.cia", tag);
    snprintf(url, sizeof(url), "https://github.com/" APP_REPOSITORY "/releases/download/%s/%s", tag, name);
    snprintf(body, sizeof(body), "intro\n" UPDATE_NOTES_OPEN "\n%s\n" UPDATE_NOTES_CLOSE "\nfooter", notes);
    return json_pack("{s:s,s:b,s:b,s:s,s:[{s:s,s:s,s:i}]}", "tag_name", tag, "draft", 0, "prerelease", pre, "body", body,
                     "assets", "name", name, "browser_download_url", url, "size", 1000000);
}

static int parse(json_t* list, bool betas, bool homebrew, const char* installed, update_release* out) {
    char* s = json_dumps(list, JSON_COMPACT);
    int result = update_parse_releases(s, strlen(s), betas, homebrew, installed, out);
    free(s);
    return result;
}

static void test_versions() {
    assert(update_is_newer("v2.7.0", "2.6.1"));
    assert(update_is_newer("v2.10.0", "2.9.9"));
    assert(update_is_newer("v2.7.0", "2.7.0-beta"));
    assert(update_is_newer("v2.7.0", "2.7.0-dev.3+abc1234"));
    assert(update_is_newer("v2.7.0-beta", "2.7.0-dev.3.2+abc1234"));
    assert(update_is_newer("v2.7.1-beta", "2.7.0"));
    assert(!update_is_newer("v2.7.0-beta", "2.7.0-beta"));
    assert(!update_is_newer("v2.7.0-dev.5+abc1234", "2.7.0-beta"));
    assert(!update_is_newer("v2.7.0", "2.7.1-dev.1+abc1234"));
    assert(!update_valid_version("2.7"));
    assert(!update_valid_version("2.7.0.1"));
    assert(!update_valid_version("2..7"));
    assert(!update_valid_version("2.7.0-betax"));
    assert(!update_valid_version("2.7.0-dev/../x"));
    assert(update_allowed_download_url("https://github.com/" APP_REPOSITORY "/releases/download/v1.0.0/" APP_NAME "-v1.0.0.cia"));
    assert(!update_allowed_download_url("https://github.com/Steveice10/FBI/releases/download/2.6.1/FBI.cia"));
}

static void test_format() {
    static char lines[512][UPDATE_LINE_MAX];
    unsigned count = update_format_notes("## Changelog\n\n- **Fixed** [colors](https://example.com).\n![QR](https://example.com/qr.png)", lines, 512, 50);
    assert(count == 3 && !strcmp(lines[0], "Changelog") && !lines[1][0] && !strcmp(lines[2], "- Fixed colors."));

    count = update_format_notes("> Beta\n" UPDATE_NOTES_OPEN "\n## What's new\n\n- Faster\n" UPDATE_NOTES_CLOSE "\n## Changelog\n- x (abc)", lines, 512, 50);
    assert(count == 3 && !strcmp(lines[0], "What's new") && !strcmp(lines[2], "- Faster"));

    assert(update_format_notes("", lines, 512, 50) == 1 && !strcmp(lines[0], "No changelog provided."));
    assert(update_format_notes("ignored", lines, 0, 50) == 0);

    static char long_notes[UPDATE_NOTES_MAX];
    memset(long_notes, 'x', sizeof(long_notes) - 1);
    long_notes[sizeof(long_notes) - 1] = '\0';
    count = update_format_notes(long_notes, lines, 512, 50);
    assert(count == 328);
    for(unsigned i = 0; i < count; i++) {
        assert(strlen(lines[i]) <= 50);
    }

    count = update_format_notes("one two three four", lines, 512, 9);
    assert(count == 2 && !strcmp(lines[0], "one two") && !strcmp(lines[1], "three four"));
}

static void test_channels() {
    static update_release out;
    json_t* l = json_array();
    json_array_append_new(l, make("v0.3.0", false, "- stable three"));
    json_array_append_new(l, make("v0.2.1", true, "- beta two one"));
    json_array_append_new(l, make("v0.2.0", false, "- stable two"));
    json_array_append_new(l, make("v0.1.2", true, "- beta one two"));
    json_array_append_new(l, make("v0.1.1", false, "- stable one one"));

    // Betas sees the newest of both; stable ignores the betas.
    assert(parse(l, true, false, "0.2.0", &out) == 1 && !strcmp(out.version, "v0.3.0") && !out.prerelease);
    assert(parse(l, false, false, "0.1.1", &out) == 1 && !strcmp(out.version, "v0.3.0"));
    assert(!strcmp(out.url, "https://github.com/" APP_REPOSITORY "/releases/download/v0.3.0/" APP_NAME "-v0.3.0.cia"));

    json_array_insert_new(l, 0, make("v0.4.0", true, "- beta four"));
    assert(parse(l, true, false, "0.3.0", &out) == 1 && !strcmp(out.version, "v0.4.0") && out.prerelease);
    assert(parse(l, false, false, "0.3.0", &out) == 1 && !strcmp(out.version, "v0.3.0"));

    // Stable: every stable newer than the install and the betas on the way to
    // the newest stable, newest first; a beta past that stable stays out.
    assert(parse(l, false, false, "0.1.0", &out) == 1);
    assert(strstr(out.notes, "stable three") && strstr(out.notes, "stable two") && strstr(out.notes, "stable one one"));
    assert(strstr(out.notes, "beta two one") && strstr(out.notes, "beta one two") && !strstr(out.notes, "beta four"));
    assert(strstr(out.notes, "stable three") < strstr(out.notes, "beta two one"));
    assert(strstr(out.notes, "beta two one") < strstr(out.notes, "stable two"));

    assert(parse(l, false, false, "0.2.0", &out) == 1);
    assert(strstr(out.notes, "## v0.2.1 BETA") && strstr(out.notes, "stable three") && !strstr(out.notes, "beta one two"));

    // Betas: every release in between, relative to the installed build.
    assert(parse(l, true, false, "0.1.1", &out) == 1);
    assert(strstr(out.notes, "beta four") && strstr(out.notes, "beta two one") && strstr(out.notes, "beta one two"));
    assert(strstr(out.notes, "stable two") && !strstr(out.notes, "stable one one"));

    // Up to date: the channel's whole history.
    assert(parse(l, false, false, "0.3.0", &out) == 1);
    assert(strstr(out.notes, "stable three") && strstr(out.notes, "stable one one") && !strstr(out.notes, "beta four"));
    assert(parse(l, true, false, "0.4.0", &out) == 1 && strstr(out.notes, "beta four") && strstr(out.notes, "stable one one"));

    // A dev build is offered the beta of its own number, a beta build is not.
    json_t* only = json_array();
    json_array_append_new(only, make("v0.5.0", true, "- beta five"));
    assert(parse(only, true, false, "0.5.0-dev.3+abc1234", &out) == 1 && update_is_newer("v0.5.0-beta", "0.5.0-dev.3+abc1234"));
    assert(!update_is_newer("v0.5.0-beta", "0.5.0-beta"));
    assert(parse(only, false, false, "0.4.0", &out) == 0);

    // The 3DSX build looks for its own asset; these releases have none.
    assert(parse(l, true, true, "0.1.0", &out) == 0);

    // Unusable releases are skipped: no asset, upstream-style tag, foreign URL.
    json_array_append_new(l, json_pack("{s:s,s:b,s:b}", "tag_name", "v0.0.9", "draft", 0, "prerelease", 0));
    json_t* foreign = make("v9.9.9", false, "- evil");
    json_object_set_new(json_array_get(json_object_get(foreign, "assets"), 0), "browser_download_url", json_string("https://evil.example/x.cia"));
    json_array_append_new(l, foreign);
    json_array_append_new(l, make("2.6.1", false, "- upstream"));
    assert(parse(l, false, false, "0.1.0", &out) == 1 && !strcmp(out.version, "v0.3.0") && !strstr(out.notes, "evil"));

    json_decref(l);
    json_decref(only);

    assert(update_parse_releases("{}", 2, true, false, "0.1.0", &out) == -1);
    assert(update_parse_releases("[]garbage", 9, true, false, "0.1.0", &out) == -1);
    assert(update_parse_releases("[]", 2, true, false, "0.1.0", &out) == 0);
}

int main() {
    test_versions();
    test_format();
    test_channels();
    puts("PASS: update_manifest");
    return 0;
}
