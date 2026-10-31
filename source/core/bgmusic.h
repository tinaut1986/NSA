#pragma once

// Background music made at run time (no files): soft chords, a sparse melody of bell notes and a
// little echo, rendered once into a loop that has no seam (what rings past the end is added to the
// start) and played from memory through the DSP. Off by default.

typedef enum {
    BGMUSIC_OFF,
    BGMUSIC_LIFT,   // Cmaj7, Am7, Fmaj7, G6: the classic lift.
    BGMUSIC_CALM,   // Slower and sparser, in F.
    BGMUSIC_SHOP,   // Soft bossa: marimba, electric piano, bass and a light beat, like a shop channel.
    BGMUSIC_COUNT
} bgmusic_style;

void bgmusic_set(bgmusic_style style, u32 volumePercent);
// From the UI loop, once a frame: starts, stops and ducks the music.
void bgmusic_update();
void bgmusic_exit();
