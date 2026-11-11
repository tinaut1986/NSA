#pragma once

#include <stdbool.h>

// Scrollable release notes on the top screen.

typedef struct changelog_text_s changelog_text;

// Wraps the notes (Markdown from update_parse_releases / update_parse_history) to the top screen.
changelog_text* changelog_text_create(const char* notes);
// Adds more notes below, after a blank line. False when out of memory (the text stays as it was).
bool changelog_text_append(changelog_text* text, const char* notes);
// The last draw showed the last line.
bool changelog_text_at_end(changelog_text* text);
void changelog_text_free(changelog_text* text);

// Draws the visible lines and scrolls with the D-Pad / Circle Pad (up/down)
// and L/R (page). Usable as part of any view's drawTop.
void changelog_text_draw(changelog_text* text, float x1, float y1, float x2, float y2);

// Where a paged changelog gets more. Every callback is optional.
typedef struct {
    void* data;
    // Each frame the end of the text is on screen: load more if it can (retry: A was pressed).
    void (*more)(void* data, bool retry);
    // A line for the bottom screen instead of the usual help ("Loading..."), or NULL.
    const char* (*status)(void* data);
    // The view closed: the text it was given is gone.
    void (*closed)(void* data);
} changelog_source;

// A view of its own: the notes on top, B to close. Takes ownership of `text`.
void changelog_display(const char* name, changelog_text* text);
// The same, asking `source` for more when the end is reached (source is copied).
void changelog_display_paged(const char* name, changelog_text* text, const changelog_source* source);
