#include <math.h>
#include <string.h>

#include <3ds.h>

#include "sound.h"

#define SOUND_RATE 22050
#define SOUND_CHANNEL_FIRST 8
#define SOUND_CHANNELS 4
#define SOUND_FRAMES_MAX (SOUND_RATE / 2)

typedef struct {
    s16* data;
    u32 frames;
} sound_buffer;

static sound_mode sound_current_mode = SOUNDS_ALL;
static u32 sound_volume = 60;
static volatile u32 sound_pending = 0;

static bool sound_dsp_tried = false;
static bool sound_dsp_ok = false;
static bool sound_ready = false;
static sound_buffer sound_buffers[SOUND_COUNT];
static ndspWaveBuf sound_waves[SOUND_CHANNELS];

bool sound_dsp_start() {
    if(!sound_dsp_tried) {
        sound_dsp_tried = true;
        sound_dsp_ok = R_SUCCEEDED(ndspInit());
        if(sound_dsp_ok) {
            ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        }
    }

    return sound_dsp_ok;
}

// ---- making the tones ----

typedef struct {
    float from;      // Hz at the start and at the end of the tone.
    float to;
    u32 ms;
    float amplitude;
    float decay;     // Per second, exponential.
    float gapMs;     // Silence after it.
} sound_tone;

static u32 sound_synth(s16* out, const sound_tone* tones, u32 count) {
    u32 pos = 0;
    float phase = 0;
    for(u32 t = 0; t < count; t++) {
        u32 frames = (u32) tones[t].ms * SOUND_RATE / 1000;
        for(u32 i = 0; i < frames && pos < SOUND_FRAMES_MAX; i++, pos++) {
            float x = (float) i / (float) frames;
            float freq = tones[t].from + (tones[t].to - tones[t].from) * x;
            phase += 2.0f * 3.14159265f * freq / SOUND_RATE;
            float seconds = (float) i / SOUND_RATE;
            float envelope = expf(-tones[t].decay * seconds);
            // A short rise, so the start does not click.
            if(i < 80) {
                envelope *= (float) i / 80.0f;
            }

            out[pos] = (s16) (sinf(phase) * envelope * tones[t].amplitude * 32000.0f);
        }

        u32 gap = (u32) (tones[t].gapMs * SOUND_RATE / 1000);
        for(u32 i = 0; i < gap && pos < SOUND_FRAMES_MAX; i++, pos++) {
            out[pos] = 0;
        }
    }

    return pos;
}

static bool sound_make(sound_id id, const sound_tone* tones, u32 count) {
    s16* data = (s16*) linearAlloc(SOUND_FRAMES_MAX * sizeof(s16));
    if(data == NULL) {
        return false;
    }

    u32 frames = sound_synth(data, tones, count);
    DSP_FlushDataCache(data, frames * sizeof(s16));
    sound_buffers[id].data = data;
    sound_buffers[id].frames = frames;
    return true;
}

static void sound_apply_volume() {
    float gain = (float) sound_volume / 100.0f;
    float mix[12] = {gain, gain};
    for(u32 i = 0; i < SOUND_CHANNELS; i++) {
        ndspChnSetMix(SOUND_CHANNEL_FIRST + (int) i, mix);
    }
}

static bool sound_prepare() {
    if(sound_ready) {
        return true;
    }

    if(!sound_dsp_start()) {
        return false;
    }

    static const sound_tone tick[] = {{2400, 2000, 14, 0.30f, 140, 0}};
    static const sound_tone click[] = {{1700, 1300, 28, 0.40f, 70, 0}};
    static const sound_tone back[] = {{1100, 650, 55, 0.35f, 30, 0}};
    static const sound_tone success[] = {{784, 784, 90, 0.40f, 12, 10}, {988, 988, 90, 0.40f, 12, 10}, {1319, 1319, 220, 0.40f, 8, 0}};
    static const sound_tone error[] = {{300, 300, 120, 0.40f, 6, 60}, {220, 220, 200, 0.40f, 6, 0}};

    bool ok = sound_make(SOUND_TICK, tick, 1) && sound_make(SOUND_CLICK, click, 1) && sound_make(SOUND_BACK, back, 1)
              && sound_make(SOUND_SUCCESS, success, 3) && sound_make(SOUND_ERROR, error, 2);
    if(!ok) {
        return false;
    }

    for(u32 i = 0; i < SOUND_CHANNELS; i++) {
        int channel = SOUND_CHANNEL_FIRST + (int) i;
        ndspChnReset(channel);
        ndspChnSetInterp(channel, NDSP_INTERP_LINEAR);
        ndspChnSetFormat(channel, NDSP_FORMAT_MONO_PCM16);
        ndspChnSetRate(channel, (float) SOUND_RATE);
    }

    sound_apply_volume();
    sound_ready = true;
    return true;
}

// ---- playing ----

void sound_set(sound_mode mode, u32 volumePercent) {
    sound_current_mode = mode;
    sound_volume = volumePercent > 100 ? 100 : volumePercent;
    if(sound_ready) {
        sound_apply_volume();
    }
}

void sound_play(sound_id id) {
    if(id < SOUND_COUNT) {
        __sync_fetch_and_or(&sound_pending, 1u << id);
    }
}

static void sound_start(sound_id id) {
    for(u32 i = 0; i < SOUND_CHANNELS; i++) {
        ndspWaveBuf* wave = &sound_waves[i];
        if(wave->status == NDSP_WBUF_QUEUED || wave->status == NDSP_WBUF_PLAYING) {
            continue;
        }

        memset(wave, 0, sizeof(*wave));
        wave->data_vaddr = sound_buffers[id].data;
        wave->nsamples = sound_buffers[id].frames;
        ndspChnWaveBufAdd(SOUND_CHANNEL_FIRST + (int) i, wave);
        return;
    }
}

void sound_update() {
    u32 pending = __sync_fetch_and_and(&sound_pending, 0);
    if(pending == 0 || sound_current_mode == SOUNDS_OFF) {
        return;
    }

    // What the settings allow, and the most important of what was asked for this frame.
    static const sound_id order[SOUND_COUNT] = {SOUND_ERROR, SOUND_SUCCESS, SOUND_BACK, SOUND_CLICK, SOUND_TICK};
    for(u32 i = 0; i < SOUND_COUNT; i++) {
        sound_id id = order[i];
        if(!(pending & (1u << id))) {
            continue;
        }

        bool alert = id == SOUND_SUCCESS || id == SOUND_ERROR;
        if(!alert && sound_current_mode != SOUNDS_ALL) {
            continue;
        }

        if(sound_prepare()) {
            sound_start(id);
        }

        // One sound per frame: an alert covers the clicks that came with it.
        return;
    }
}

void sound_exit() {
    if(sound_ready) {
        for(u32 i = 0; i < SOUND_CHANNELS; i++) {
            ndspChnWaveBufClear(SOUND_CHANNEL_FIRST + (int) i);
        }

        for(u32 i = 0; i < SOUND_COUNT; i++) {
            if(sound_buffers[i].data != NULL) {
                linearFree(sound_buffers[i].data);
                sound_buffers[i].data = NULL;
            }
        }

        sound_ready = false;
    }

    if(sound_dsp_ok) {
        ndspExit();
        sound_dsp_ok = false;
        sound_dsp_tried = false;
    }
}
