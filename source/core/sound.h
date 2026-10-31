#pragma once

// Interface sounds: short tones made at run time (no files) and played through the DSP.
// Anything may ask for a sound from any thread; they are played from the UI loop.

typedef enum {
    SOUND_TICK,      // A direction, a touch.
    SOUND_CLICK,     // A confirmed.
    SOUND_BACK,      // B.
    SOUND_SUCCESS,   // A task ended well (transfers, installs).
    SOUND_ERROR,     // Something failed.
    SOUND_COUNT
} sound_id;

typedef enum {
    SOUNDS_OFF,
    SOUNDS_ALERTS,   // Only SUCCESS and ERROR.
    SOUNDS_ALL
} sound_mode;

void sound_set(sound_mode mode, u32 volumePercent);
void sound_play(sound_id id);
// From the UI loop, once a frame: plays what was asked for.
void sound_update();
void sound_exit();

// The DSP is shared with the audio player: starts it on first use; false without its firmware.
bool sound_dsp_start();
