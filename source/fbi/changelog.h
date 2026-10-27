#pragma once

// Scrollable release notes on the top screen.

typedef struct changelog_text_s changelog_text;

// Wraps the notes (Markdown from update_parse_releases) to the top screen.
changelog_text* changelog_text_create(const char* notes);
void changelog_text_free(changelog_text* text);

// Draws the visible lines and scrolls with the D-Pad / Circle Pad (up/down)
// and L/R (page). Usable as part of any view's drawTop.
void changelog_text_draw(changelog_text* text, float x1, float y1, float x2, float y2);

// A view of its own: the notes on top, B to close. Takes ownership of `text`.
void changelog_display(const char* name, changelog_text* text);
