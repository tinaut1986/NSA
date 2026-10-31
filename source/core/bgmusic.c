#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "bgmusic.h"
#include "sound.h"

#define BG_RATE 22050
#define BG_CHANNEL 14          // 0 is the audio player's, 8 to 11 the interface sounds'.
#define BG_PI 3.14159265f

typedef struct {
    int root;
    int count;
    int tones[5];
} bg_chord;

typedef struct {
    float bpm;
    bg_chord chords[4];
    int scale[8];
    u32 seed;
    float density;   // How often a melody note falls on a beat.
    float wet;       // How much echo.
} bg_style;

static const bg_style bg_styles[BGMUSIC_COUNT - 1] = {
        {66, {{60, 4, {0, 4, 7, 11}}, {57, 4, {0, 3, 7, 10}}, {53, 4, {0, 4, 7, 11}}, {55, 4, {0, 4, 7, 9}}}, {60, 62, 64, 67, 69, 72, 74, 76}, 7, 0.55f, 0.32f},
        {52, {{53, 5, {0, 4, 7, 11, 14}}, {50, 5, {0, 3, 7, 10, 14}}, {58, 4, {0, 4, 7, 11}}, {48, 4, {0, 4, 7, 9}}}, {53, 55, 57, 60, 62, 65, 67, 69}, 3, 0.30f, 0.40f}
};

typedef enum {
    BG_IDLE,
    BG_RENDERING,
    BG_DONE
} bg_state;

static bgmusic_style bg_wanted = BGMUSIC_OFF;
static u32 bg_volume = 50;

static volatile bg_state bg_render_state = BG_IDLE;
static bgmusic_style bg_render_style = BGMUSIC_OFF;   // What the thread is making.
static s16* bg_data = NULL;                           // What it made (linear memory).
static u32 bg_frames = 0;

static bgmusic_style bg_playing = BGMUSIC_OFF;
static ndspWaveBuf bg_wave;
static bool bg_channel_ready = false;
static bool bg_ducked = false;

// ---- the synthesis (cheap operations only: adds, multiplies, one recurrence per partial) ----

static u32 bg_random(u32* state) {
    u32 x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float bg_hz(float midi) {
    return 440.0f * exp2f((midi - 69.0f) / 12.0f);
}

// Adds amplitude * sin(2 pi freq t + phase) to `out` with a recurrence: two multiplies a sample.
static void bg_add_sine(float* out, u32 n, float freq, float amplitude, float phase) {
    float w = 2.0f * BG_PI * freq / BG_RATE;
    float c = 2.0f * cosf(w);
    float previous2 = sinf(phase - w);
    float previous1 = sinf(phase);
    for(u32 i = 0; i < n; i++) {
        out[i] += amplitude * previous1;
        float next = c * previous1 - previous2;
        previous2 = previous1;
        previous1 = next;
    }
}

// Adds gain * `sig` at `start`, wrapping past the end of the loop: the join cannot be heard.
static void bg_put(float* mix, u32 length, u32 start, const float* sig, u32 n, float gain) {
    start %= length;
    for(u32 i = 0; i < n; i++) {
        u32 at = start + i;
        mix[at >= length ? at % length : at] += gain * sig[i];
    }
}

static void bg_add_circular(float* mix, u32 length, u32 start, const float* sig, u32 n) {
    bg_put(mix, length, start, sig, n, 1.0f);
}

static void bg_pad(float* mix, u32 length, float* tmp, u32 start, const float* freqs, u32 count, float seconds, float gain) {
    u32 n = (u32) (seconds * BG_RATE);
    memset(tmp, 0, n * sizeof(float));
    for(u32 k = 0; k < count; k++) {
        bg_add_sine(tmp, n, freqs[k], 1.0f, 0.0f);
        bg_add_sine(tmp, n, freqs[k] * 2.0f, 0.21f, 0.5f);
        bg_add_sine(tmp, n, freqs[k] * 1.003f, 0.5f, 0.0f);   // A slight detune: it breathes.
    }

    float attack = 1.8f * BG_RATE;
    float release = 2.4f * BG_RATE;
    for(u32 i = 0; i < n; i++) {
        float e = 1.0f;
        if(i < attack) {
            float x = i / attack;
            e = x * x;
        }

        if(i + release > n) {
            float x = (n - i) / release;
            e *= x * x;
        }

        tmp[i] *= e * gain / (float) count;
    }

    bg_add_circular(mix, length, start, tmp, n);
}

static void bg_bell(float* mix, u32 length, float* tmp, u32 start, float freq, float gain) {
    u32 n = (u32) (3.2f * BG_RATE);
    memset(tmp, 0, n * sizeof(float));
    bg_add_sine(tmp, n, freq, 1.0f, 0.0f);
    bg_add_sine(tmp, n, freq * 2.0f, 0.25f, 0.0f);
    bg_add_sine(tmp, n, freq * 3.01f, 0.08f, 0.0f);

    float decay = expf(-2.2f / BG_RATE);
    float level = gain;
    for(u32 i = 0; i < n; i++) {
        tmp[i] *= level * (i < BG_RATE / 120 ? (float) i / (BG_RATE / 120) : 1.0f);
        level *= decay;
    }

    bg_add_circular(mix, length, start, tmp, n);
}

// Echo (three circular taps), a one-pole low-pass that has already seen the end of the loop when it
// starts (so the start and the end agree), a level and the conversion to 16 bits.
static bool bg_finish(const float* mix, u32 length, float wet, float alpha, float peakTarget, float* filtered, s16* data) {
    static const u32 tapMs[3] = {283, 467, 719};
    static const float tapGain[3] = {0.45f, 0.33f, 0.22f};
    u32 tapFrames[3];
    for(u32 t = 0; t < 3; t++) {
        tapFrames[t] = tapMs[t] * BG_RATE / 1000;
    }

    #define BG_SAMPLE(i) ((mix)[i] + wet * (tapGain[0] * mix[((i) + length - tapFrames[0]) % length] + tapGain[1] * mix[((i) + length - tapFrames[1]) % length] + tapGain[2] * mix[((i) + length - tapFrames[2]) % length]))

    float state = 0;
    for(u32 i = length - 4096; i < length; i++) {
        state += alpha * (BG_SAMPLE(i) - state);
    }

    float peak = 0.0001f;
    for(u32 i = 0; i < length; i++) {
        state += alpha * (BG_SAMPLE(i) - state);
        filtered[i] = state;
        float magnitude = fabsf(state);
        if(magnitude > peak) {
            peak = magnitude;
        }
    }

    #undef BG_SAMPLE

    float scale = peakTarget / peak * 32767.0f;
    for(u32 i = 0; i < length; i++) {
        data[i] = (s16) (filtered[i] * scale);
    }

    DSP_FlushDataCache(data, length * sizeof(s16));
    return true;
}

// Renders one whole loop into linear memory; false if there is no memory for it.
static bool bg_render(const bg_style* style, s16** outData, u32* outFrames) {
    float beat = 60.0f / style->bpm;
    u32 chordFrames = (u32) (2 * 4 * beat * BG_RATE);
    u32 length = chordFrames * 4;

    float* mix = (float*) calloc(length, sizeof(float));
    float* tmp = (float*) malloc((chordFrames + 4 * BG_RATE) * sizeof(float));
    float* filtered = (float*) malloc(length * sizeof(float));
    s16* data = (s16*) linearAlloc(length * sizeof(s16));
    if(mix == NULL || tmp == NULL || filtered == NULL || data == NULL) {
        free(mix);
        free(tmp);
        free(filtered);
        if(data != NULL) {
            linearFree(data);
        }

        return false;
    }

    u32 seed = style->seed * 2654435761u + 1;
    u32 lastNote = bg_random(&seed) % 8;
    static const int steps[7] = {-2, -1, -1, 0, 1, 1, 2};
    u32 beatsInChord = (u32) (chordFrames / (beat * BG_RATE / 2.0f) + 0.5f);

    for(u32 c = 0; c < 4; c++) {
        const bg_chord* chord = &style->chords[c];
        u32 start = c * chordFrames;

        float freqs[5];
        for(int t = 0; t < chord->count; t++) {
            freqs[t] = bg_hz((float) (chord->root + chord->tones[t]));
        }

        bg_pad(mix, length, tmp, start, freqs, (u32) chord->count, 2 * 4 * beat + 3.0f, 0.9f);
        float bass = bg_hz((float) (chord->root - 12));
        bg_pad(mix, length, tmp, start, &bass, 1, 2 * 4 * beat + 2.0f, 0.55f);

        for(u32 k = 0; k < beatsInChord; k++) {
            float chance = style->density * (k % 2 == 0 ? 1.0f : 0.45f);
            if((bg_random(&seed) & 0xFFFF) / 65536.0f < chance) {
                int note = (int) lastNote + steps[bg_random(&seed) % 7];
                lastNote = (u32) (note < 0 ? 0 : note > 7 ? 7 : note);
                bg_bell(mix, length, tmp, start + (u32) (k * beat * BG_RATE / 2.0f), bg_hz((float) (style->scale[lastNote] + 12)), 0.22f);
            }
        }
    }

    bool ok = bg_finish(mix, length, style->wet, 0.28f, 0.42f, filtered, data);
    free(mix);
    free(tmp);
    free(filtered);
    if(!ok) {
        linearFree(data);
        return false;
    }

    *outData = data;
    *outFrames = length;
    return true;
}

// ---- the shop style: a bossa with a marimba melody ----

typedef struct {
    float ratio;
    float amplitude;
    float decay;   // Per second, on top of the note's own.
} bg_partial;

// One note: partials that each fade by themselves, an overall fade and a short attack.
static void bg_note(float* out, u32 n, float freq, const bg_partial* parts, u32 count, float decay, float attack) {
    memset(out, 0, n * sizeof(float));
    for(u32 k = 0; k < count; k++) {
        float w = 2.0f * BG_PI * freq * parts[k].ratio / BG_RATE;
        float c = 2.0f * cosf(w);
        float previous2 = sinf(-w);
        float previous1 = 0.0f;
        float level = parts[k].amplitude;
        float fade = expf(-(parts[k].decay + decay) / BG_RATE);
        for(u32 i = 0; i < n; i++) {
            out[i] += level * previous1;
            level *= fade;
            float next = c * previous1 - previous2;
            previous2 = previous1;
            previous1 = next;
        }
    }

    u32 ramp = (u32) (attack * BG_RATE);
    for(u32 i = 0; i < ramp && i < n; i++) {
        out[i] *= (float) i / (float) ramp;
    }
}

#define SHOP_BPM 104.0f
#define SHOP_SWING 0.58f

// Where a beat of a bar falls, in frames, with the swing on the off-beat eighths.
static u32 bg_when(u32 bar, float beat) {
    float whole = floorf(beat);
    float frac = beat - whole;
    if(fabsf(frac - 0.5f) < 0.001f) {
        frac = SHOP_SWING;
    }

    return (u32) (((float) bar * 4.0f + whole + frac) * 60.0f / SHOP_BPM * BG_RATE);
}

typedef struct {
    int bass;
    int comp[5];
} bg_shop_chord;

// C maj9, A min9, D min9, G 13, E min7, A 7, D min9, G 13.
static const bg_shop_chord bg_shop_chords[8] = {
        {48, {59, 62, 64, 67, 71}}, {45, {55, 60, 62, 64, 67}}, {50, {60, 64, 65, 69, 72}}, {43, {59, 62, 64, 65, 69}},
        {52, {59, 62, 64, 67, 71}}, {45, {55, 59, 61, 64, 67}}, {50, {60, 64, 65, 69, 72}}, {43, {59, 62, 64, 65, 69}}
};

static const int bg_shop_scale[12] = {60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79};

static bool bg_render_shop(s16** outData, u32* outFrames) {
    const u32 passes = 2;
    const u32 bars = 8 * passes;
    u32 length = (u32) ((float) bars * 4.0f * 60.0f / SHOP_BPM * BG_RATE + 0.5f);

    float* mix = (float*) calloc(length, sizeof(float));
    float* tmp = (float*) malloc(BG_RATE * 2 * sizeof(float));
    float* filtered = (float*) malloc(length * sizeof(float));
    s16* data = (s16*) linearAlloc(length * sizeof(s16));
    if(mix == NULL || tmp == NULL || filtered == NULL || data == NULL) {
        free(mix);
        free(tmp);
        free(filtered);
        if(data != NULL) {
            linearFree(data);
        }

        return false;
    }

    static const bg_partial marimba[] = {{1.0f, 1.0f, 1.0f}, {3.98f, 0.35f, 9.0f}, {9.9f, 0.12f, 22.0f}};
    static const bg_partial epiano[] = {{1.0f, 1.0f, 2.2f}, {2.0f, 0.28f, 4.0f}};
    static const bg_partial bassNote[] = {{1.0f, 1.0f, 3.5f}, {2.0f, 0.35f, 8.0f}, {3.0f, 0.08f, 14.0f}};
    static const bg_partial click[] = {{1.0f, 1.0f, 90.0f}, {1.4727f, 0.5f, 90.0f}};

    u32 seed = 0x5EED1234u;

    // The two-bar-long idea, as (beat, step in the scale, unused length): said, varied, turned over, resolved.
    static const float motifBeat[4] = {0.0f, 1.5f, 2.0f, 3.5f};
    static const int motifStep[4] = {0, 2, 1, -1};
    static const int pairBase[4] = {7, 7, 5, 4};

    for(u32 p = 0; p < passes; p++) {
        for(u32 b = 0; b < 8; b++) {
            u32 bar = p * 8 + b;
            const bg_shop_chord* chord = &bg_shop_chords[b];
            int next = bg_shop_chords[(b + 1) % 8].bass;

            // The bass: a bossa figure.
            const float bassBeat[4] = {0.0f, 1.5f, 2.5f, 3.5f};
            const int bassMidi[4] = {chord->bass, chord->bass + 7, chord->bass, (b % 2) ? next - 1 : chord->bass + 7};
            const float bassGain[4] = {0.9f, 0.6f, 0.7f, 0.5f};
            for(u32 k = 0; k < 4; k++) {
                u32 n = (u32) (0.55f * BG_RATE);
                bg_note(tmp, n, bg_hz((float) bassMidi[k]), bassNote, 3, 0.0f, 0.006f);
                bg_put(mix, length, bg_when(bar, bassBeat[k]), tmp, n, bassGain[k] * 0.55f);
            }

            // The comp: electric piano stabs on 1+, 2+ and 4, the chord rolled a little.
            static const float compBeat[3] = {0.5f, 1.5f, 3.0f};
            for(u32 k = 0; k < 3; k++) {
                for(u32 i = 0; i < 5; i++) {
                    u32 n = (u32) (0.9f * BG_RATE);
                    bg_note(tmp, n, bg_hz((float) chord->comp[i]), epiano, 2, 0.2f, 0.006f);
                    bg_put(mix, length, bg_when(bar, compBeat[k]) + (u32) (i * 0.012f * BG_RATE), tmp, n, 0.13f);
                }
            }

            // A soft kick on 1 and 3.
            for(u32 k = 0; k < 2; k++) {
                u32 n = (u32) (0.22f * BG_RATE);
                float level = 1.0f;
                float drop = 60.0f;
                float phase = 0.0f;
                for(u32 i = 0; i < n; i++) {
                    phase += 2.0f * BG_PI * (52.0f + drop) / BG_RATE;
                    tmp[i] = sinf(phase) * level;
                    level *= expf(-16.0f / BG_RATE);
                    drop *= expf(-28.0f / BG_RATE);
                }

                bg_put(mix, length, bg_when(bar, k * 2.0f), tmp, n, 0.35f);
            }

            // The cross-stick on the clave.
            static const float clickBeat[4] = {0.0f, 1.5f, 2.5f, 3.0f};
            for(u32 k = 0; k < 4; k++) {
                u32 n = (u32) (0.05f * BG_RATE);
                bg_note(tmp, n, 1650.0f, click, 2, 0.0f, 0.0f);
                bg_put(mix, length, bg_when(bar, clickBeat[k]), tmp, n, 0.14f);
            }

            // The shaker on every eighth, the off-beats a little louder.
            for(u32 k = 0; k < 8; k++) {
                u32 n = (u32) (0.06f * BG_RATE);
                float previous = 0.0f;
                float level = 1.0f;
                for(u32 i = 0; i < n; i++) {
                    float x = (float) (bg_random(&seed) & 0xFFFF) / 32768.0f - 1.0f;
                    tmp[i] = (x - previous) * level;
                    previous = x;
                    level *= expf(-55.0f / BG_RATE);
                }

                bg_put(mix, length, bg_when(bar, k / 2.0f), tmp, n, k % 2 == 0 ? 0.06f : 0.10f);
            }
        }

        // The melody, in marimba: the motif, the motif varied, turned over, and a resolution.
        for(u32 pair = 0; pair < 4; pair++) {
            u32 bar = p * 8 + pair * 2;
            u32 notes = pair == 3 ? 3 : 4;
            for(u32 k = 0; k < notes; k++) {
                float beat = motifBeat[k];
                int step = motifStep[k];
                if(pair == 1 && k == 1) {
                    step += 1;
                } else if(pair == 2) {
                    step = -step;
                } else if(pair == 3) {
                    static const float endBeat[3] = {0.0f, 2.0f, 3.0f};
                    static const int endStep[3] = {0, -1, -2};
                    beat = endBeat[k];
                    step = endStep[k];
                }

                int index = pairBase[pair] + step;
                index = index < 0 ? 0 : index > 11 ? 11 : index;
                u32 n = (u32) (1.4f * BG_RATE);
                bg_note(tmp, n, bg_hz((float) bg_shop_scale[index]), marimba, 3, 0.6f, 0.002f);
                bg_put(mix, length, bg_when(bar, beat), tmp, n, 0.30f);
            }
        }
    }

    bool ok = bg_finish(mix, length, 0.62f, 0.45f, 0.45f, filtered, data);
    free(mix);
    free(tmp);
    free(filtered);
    if(!ok) {
        linearFree(data);
        return false;
    }

    *outData = data;
    *outFrames = length;
    return true;
}

static void bg_render_thread(void* arg) {
    bgmusic_style style = (bgmusic_style) (uintptr_t) arg;
    s16* data = NULL;
    u32 frames = 0;
    u64 started = osGetTime();
    bool ok = style == BGMUSIC_SHOP ? bg_render_shop(&data, &frames) : bg_render(&bg_styles[style - 1], &data, &frames);
#if NSA_DEBUG
    // How long the loop took to make, for tuning on each console.
    FILE* log = fopen("sdmc:/nsa/music-log.txt", "a");
    if(log != NULL) {
        fprintf(log, "style %d: %s in %lu ms (%lu frames)\n", (int) style, ok ? "rendered" : "FAILED", (u32) (osGetTime() - started), frames);
        fclose(log);
    }

    // The loop itself, to listen to it on a computer.
    if(ok) {
        char path[64];
        snprintf(path, sizeof(path), "sdmc:/nsa/music-style-%d.wav", (int) style);
        FILE* wav = fopen(path, "wb");
        if(wav != NULL) {
            u32 bytes = frames * 2;
            u32 header[11] = {0x46464952u, 36 + bytes, 0x45564157u, 0x20746D66u, 16, 0x00010001u, BG_RATE, BG_RATE * 2, 0x00100002u, 0x61746164u, bytes};
            fwrite(header, sizeof(header), 1, wav);
            fwrite(data, 2, frames, wav);
            fclose(wav);
        }
    }
#endif
    if(ok) {
        bg_data = data;
        bg_frames = frames;
    } else {
        bg_data = NULL;
        bg_frames = 0;
    }

    bg_render_state = BG_DONE;
}

// ---- playing ----

static void bg_apply_volume() {
    float gain = bg_ducked ? 0.0f : (float) bg_volume / 100.0f * 0.8f;
    float mix[12] = {gain, gain};
    ndspChnSetMix(BG_CHANNEL, mix);
}

static void bg_stop() {
    if(bg_playing != BGMUSIC_OFF) {
        ndspChnWaveBufClear(BG_CHANNEL);
        bg_playing = BGMUSIC_OFF;
    }

    if(bg_data != NULL && bg_render_state != BG_RENDERING) {
        linearFree(bg_data);
        bg_data = NULL;
    }
}

static void bg_play(bgmusic_style style) {
    if(!bg_channel_ready) {
        ndspChnReset(BG_CHANNEL);
        ndspChnSetInterp(BG_CHANNEL, NDSP_INTERP_LINEAR);
        ndspChnSetFormat(BG_CHANNEL, NDSP_FORMAT_MONO_PCM16);
        ndspChnSetRate(BG_CHANNEL, (float) BG_RATE);
        bg_channel_ready = true;
    }

    bg_apply_volume();
    memset(&bg_wave, 0, sizeof(bg_wave));
    bg_wave.data_vaddr = bg_data;
    bg_wave.nsamples = bg_frames;
    bg_wave.looping = true;
    ndspChnWaveBufAdd(BG_CHANNEL, &bg_wave);
    bg_playing = style;
}

void bgmusic_set(bgmusic_style style, u32 volumePercent) {
    bg_wanted = style < BGMUSIC_COUNT ? style : BGMUSIC_OFF;
    bg_volume = volumePercent > 100 ? 100 : volumePercent;
    if(bg_channel_ready) {
        bg_apply_volume();
    }
}

void bgmusic_update() {
    // Another style or off: stop what plays.
    if(bg_playing != BGMUSIC_OFF && bg_playing != bg_wanted) {
        bg_stop();
    }

    if(bg_render_state == BG_DONE) {
        bg_render_state = BG_IDLE;
        if(bg_data != NULL && bg_render_style == bg_wanted && bg_playing == BGMUSIC_OFF && sound_dsp_start()) {
            bg_play(bg_render_style);
        } else if(bg_data != NULL) {
            linearFree(bg_data);
            bg_data = NULL;
        }
    }

    if(bg_wanted != BGMUSIC_OFF && bg_playing == BGMUSIC_OFF && bg_render_state == BG_IDLE && bg_data == NULL && sound_dsp_start()) {
        bg_render_state = BG_RENDERING;
        bg_render_style = bg_wanted;
        // The lowest priority, on the system core: it must never get in the UI's way.
        if(threadCreate(bg_render_thread, (void*) (uintptr_t) bg_wanted, 0x10000, 0x3F, 1, true) == NULL) {
            bg_render_state = BG_IDLE;
            bg_wanted = BGMUSIC_OFF;
        }
    }

    // The audio player (channel 0) has the speakers: the music waits quietly.
    if(bg_playing != BGMUSIC_OFF) {
        bool player = ndspChnIsPlaying(0);
        if(player != bg_ducked) {
            bg_ducked = player;
            bg_apply_volume();
        }
    }
}

void bgmusic_exit() {
    bg_wanted = BGMUSIC_OFF;
    for(u32 waited = 0; bg_render_state == BG_RENDERING && waited < 8000; waited += 10) {
        svcSleepThread(10 * 1000000LL);
    }

    bg_stop();
    if(bg_data != NULL) {
        linearFree(bg_data);
        bg_data = NULL;
    }

    bg_render_state = BG_IDLE;
    bg_channel_ready = false;
}
