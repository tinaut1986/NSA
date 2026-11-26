#pragma once

// An animated background for the top screen (issue #60), drawn by NSA behind the content in the theme's accent colour:
// each particle at a depth of its own behind the screen, so with the 3D slider up they float at different depths; flat
// without it. While one runs, the top screen is drawn every frame (NSA otherwise draws only when something moves), so it
// keeps few particles and stands still while a transfer has the turn. Off by default.

typedef enum {
    BACKDROP_OFF,
    BACKDROP_BUBBLES,     // Rising and swaying.
    BACKDROP_SNOW,        // Falling slowly, with some wind.
    BACKDROP_STARS,       // Twinkling, drifting slowly sideways (the far ones slower).
    BACKDROP_FIREFLIES,   // Wandering lights that glow and fade.
    BACKDROP_COUNT
} backdrop_style;

void backdrop_set(backdrop_style style);
// True while it moves: the UI then draws every frame.
bool backdrop_active();
// Once a frame, before the top screen is drawn (both eyes see the same frame).
void backdrop_update();
// Right after the top screen's background, for each eye.
void backdrop_draw(float width, float height);
