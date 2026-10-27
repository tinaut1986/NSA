#pragma once

// Pure parsing of GitHub's release list for the updater. No 3DS dependencies:
// tests/update_manifest_test.c builds it on the host.
// Ported from ../zelda-tmc-3ds platform/3ds/source/update_manifest.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/appinfo.h"

#define UPDATE_MAX_FILE (32u * 1024u * 1024u)
#define UPDATE_NOTES_MAX 16384
#define UPDATE_LINE_MAX 96

// The release workflow puts docs/release-notes/<tag>.md between these markers.
#define UPDATE_NOTES_OPEN "<!-- nsa-notes -->"
#define UPDATE_NOTES_CLOSE "<!-- /nsa-notes -->"

typedef struct {
    char version[48];
    char notes[UPDATE_NOTES_MAX];
    char url[512];
    uint32_t size;
    bool prerelease;
} update_release;

// The channel's newest release out of GitHub's list (/releases): `betas` also
// counts pre-releases, else only stable ones. The notes cover every release of
// the channel newer than `installed`, newest first; when nothing is newer
// (or `installed` is "0.0.0"), the channel's whole history.
// -1: invalid response, 0: nothing in this channel, 1: valid candidate.
int update_parse_releases(const char* data, size_t size, bool betas, bool homebrew, const char* installed, update_release* out);

// Versions: "X.Y.Z" (optionally with a leading 'v'), "X.Y.Z-beta" for a
// published pre-release build, "X.Y.Z-dev.N+hash" for an unpublished one.
// Order of one number: dev < beta < plain.
bool update_is_newer(const char* candidate, const char* installed);
bool update_valid_version(const char* version);
bool update_allowed_download_url(const char* url);

// Plain-text, word-wrapped notes, at most `width` (< UPDATE_LINE_MAX) columns.
unsigned update_format_notes(const char* markdown, char lines[][UPDATE_LINE_MAX], unsigned capacity, unsigned width);
