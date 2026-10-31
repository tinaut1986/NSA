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
    float density;   // How often a melody note falls on a beat.
    float wet;       // How much echo.
} bg_style;

static const bg_style bg_styles[BGMUSIC_COUNT - 1] = {
        {66, {{60, 4, {0, 4, 7, 11}}, {57, 4, {0, 3, 7, 10}}, {53, 4, {0, 4, 7, 11}}, {55, 4, {0, 4, 7, 9}}}, {60, 62, 64, 67, 69, 72, 74, 76}, 0.55f, 0.32f},
        {52, {{53, 5, {0, 4, 7, 11, 14}}, {50, 5, {0, 3, 7, 10, 14}}, {58, 4, {0, 4, 7, 11}}, {48, 4, {0, 4, 7, 9}}}, {53, 55, 57, 60, 62, 65, 67, 69}, 0.30f, 0.40f}
};

typedef enum {
    BG_IDLE,
    BG_RENDERING,
    BG_DONE
} bg_state;

#define BG_CHANNELS 2
static const int bg_channel_of[BG_CHANNELS] = {14, 15};   // 0 is the audio player's, 8 to 11 the interface sounds'.

// A song that plays, or fades in or out, on one DSP channel. Two of them make the crossfade.
typedef struct {
    s16* data;
    u32 frames;
    bgmusic_style style;
    bool playing;
    ndspWaveBuf wave;
    float gain;      // 0..1 now,
    float target;    // and where it is going,
    float fadeTime;  // in this many seconds from 0 to 1.
} bg_slot;

static bg_slot bg_slots[BG_CHANNELS];

static bgmusic_style bg_wanted = BGMUSIC_OFF;
static u32 bg_volume = 50;
static bgmusic_style bg_session = BGMUSIC_OFF;   // The style a new song was already asked for in this go.

// The thread that makes a song.
static volatile bg_state bg_render_state = BG_IDLE;
static bgmusic_style bg_render_style = BGMUSIC_OFF;
static s16* bg_data = NULL;                      // What it made (linear memory).
static u32 bg_frames = 0;
static volatile u32 bg_progress = 0;             // 0..100, for the animation.
static volatile bool bg_cancel = false;

static bool bg_ducked = false;
static bool bg_channels_ready = false;
static u64 bg_last_update = 0;

#define BG_CACHE_MAGIC 0x4D41534Eu   // "NSAM"

// ---- the synthesis (cheap operations only: adds, multiplies, one recurrence per partial) ----

static u32 bg_random(u32* state) {
    u32 x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// A seed that is different every time the music starts: the melody is never the same twice.
static u32 bg_fresh_seed() {
    u32 seed = (u32) osGetTime() * 2654435761u ^ (u32) svcGetSystemTick() ^ 0x9E3779B9u;
    return seed != 0 ? seed : 1;
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

    u32 seed = bg_fresh_seed();
    u32 lastNote = bg_random(&seed) % 8;
    static const int steps[7] = {-2, -1, -1, 0, 1, 1, 2};
    u32 beatsInChord = (u32) (chordFrames / (beat * BG_RATE / 2.0f) + 0.5f);

    for(u32 c = 0; c < 4; c++) {
        if(bg_cancel) {
            free(mix);
            free(tmp);
            free(filtered);
            linearFree(data);
            return false;
        }

        bg_progress = c * 85 / 4;
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

    bg_progress = 90;
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

    u32 seed = bg_fresh_seed();

    // The two-bar-long idea, as (beat, step in the scale, unused length): said, varied, turned over, resolved.
    static const float motifBeat[4] = {0.0f, 1.5f, 2.0f, 3.5f};
    static const int motifStep[4] = {0, 2, 1, -1};
    static const int pairBase[4] = {7, 7, 5, 4};

    for(u32 p = 0; p < passes; p++) {
        for(u32 b = 0; b < 8; b++) {
            if(bg_cancel) {
                free(mix);
                free(tmp);
                free(filtered);
                linearFree(data);
                return false;
            }

            u32 bar = p * 8 + b;
            bg_progress = (p * 8 + b) * 85 / bars;
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
            int pairShift = 0;
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

                // Never quite the same: the start of each phrase moves a step, and some notes move one.
                if(k == 0) {
                    pairShift = (int) (bg_random(&seed) % 3) - 1;
                } else if(bg_random(&seed) % 10 < 3) {
                    step += (int) (bg_random(&seed) % 3) - 1;
                }

                int index = pairBase[pair] + pairShift + step;
                index = index < 0 ? 0 : index > 11 ? 11 : index;
                u32 n = (u32) (1.4f * BG_RATE);
                bg_note(tmp, n, bg_hz((float) bg_shop_scale[index]), marimba, 3, 0.6f, 0.002f);
                bg_put(mix, length, bg_when(bar, beat), tmp, n, 0.30f);
            }
        }
    }

    bg_progress = 90;
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

// ---- the cache: the last song of each style, to play at once next time ----

typedef struct {
    u32 magic;
    u32 version;
    u32 style;
    u32 frames;
    u32 rate;
} bg_cache_header;

static void bg_cache_path(bgmusic_style style, char* out, size_t size) {
    snprintf(out, size, "sdmc:/nsa/music-cache-%d.pcm", (int) style);
}

static void bg_cache_save(bgmusic_style style, const s16* data, u32 frames) {
    char path[64];
    bg_cache_path(style, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if(f == NULL) {
        return;
    }

    bg_cache_header header = {BG_CACHE_MAGIC, 1, (u32) style, frames, BG_RATE};
    bool ok = fwrite(&header, sizeof(header), 1, f) == 1 && fwrite(data, sizeof(s16), frames, f) == frames;
    fclose(f);
    if(!ok) {
        remove(path);
    }
}

// The last song of this style, in linear memory; false when there is none or it is not whole.
static bool bg_cache_load(bgmusic_style style, s16** outData, u32* outFrames) {
    char path[64];
    bg_cache_path(style, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        return false;
    }

    bg_cache_header header;
    bool ok = fread(&header, sizeof(header), 1, f) == 1 && header.magic == BG_CACHE_MAGIC && header.version == 1
              && header.style == (u32) style && header.rate == BG_RATE && header.frames > BG_RATE && header.frames < BG_RATE * 120;
    s16* data = NULL;
    if(ok) {
        data = (s16*) linearAlloc(header.frames * sizeof(s16));
        ok = data != NULL && fread(data, sizeof(s16), header.frames, f) == header.frames;
    }

    fclose(f);
    if(!ok) {
        if(data != NULL) {
            linearFree(data);
        }

        remove(path);
        return false;
    }

    DSP_FlushDataCache(data, header.frames * sizeof(s16));
    *outData = data;
    *outFrames = header.frames;
    return true;
}

static void bg_render_thread(void* arg) {
    bgmusic_style style = (bgmusic_style) (uintptr_t) arg;
    s16* data = NULL;
    u32 frames = 0;
    u64 started = osGetTime();
    bool ok = style == BGMUSIC_SHOP ? bg_render_shop(&data, &frames) : bg_render(&bg_styles[style - 1], &data, &frames);
    if(ok) {
        // Kept for next time (not when it was cancelled half way: then there is nothing).
        bg_cache_save(style, data, frames);
    }

#if NSA_DEBUG
    // How long the loop took to make, for tuning on each console.
    FILE* log = fopen("sdmc:/nsa/music-log.txt", "a");
    if(log != NULL) {
        fprintf(log, "style %d: %s in %lu ms (%lu frames)\n", (int) style, ok ? "rendered" : "stopped", (u32) (osGetTime() - started), frames);
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
        bg_progress = 100;
    } else {
        bg_data = NULL;
        bg_frames = 0;
    }

    bg_render_state = BG_DONE;
}

// ---- playing and crossfading ----

static void bg_channels_init() {
    if(bg_channels_ready) {
        return;
    }

    for(u32 i = 0; i < BG_CHANNELS; i++) {
        ndspChnReset(bg_channel_of[i]);
        ndspChnSetInterp(bg_channel_of[i], NDSP_INTERP_LINEAR);
        ndspChnSetFormat(bg_channel_of[i], NDSP_FORMAT_MONO_PCM16);
        ndspChnSetRate(bg_channel_of[i], (float) BG_RATE);
    }

    bg_channels_ready = true;
}

static void bg_slot_mix(u32 i) {
    float gain = bg_ducked ? 0.0f : bg_slots[i].gain * (float) bg_volume / 100.0f * 0.8f;
    float mix[12] = {gain, gain};
    ndspChnSetMix(bg_channel_of[i], mix);
}

// Starts a song on a free slot, silent, and sets it fading towards `target`.
static bool bg_slot_start(bgmusic_style style, s16* data, u32 frames, float fadeSeconds) {
    for(u32 i = 0; i < BG_CHANNELS; i++) {
        bg_slot* slot = &bg_slots[i];
        if(slot->playing) {
            continue;
        }

        bg_channels_init();
        memset(slot, 0, sizeof(*slot));
        slot->data = data;
        slot->frames = frames;
        slot->style = style;
        slot->gain = 0.0f;
        slot->target = 1.0f;
        slot->fadeTime = fadeSeconds;
        slot->wave.data_vaddr = data;
        slot->wave.nsamples = frames;
        slot->wave.looping = true;
        bg_slot_mix(i);
        ndspChnWaveBufAdd(bg_channel_of[i], &slot->wave);
        slot->playing = true;
        return true;
    }

    return false;
}

static void bg_slot_stop(u32 i) {
    bg_slot* slot = &bg_slots[i];
    if(!slot->playing) {
        return;
    }

    ndspChnWaveBufClear(bg_channel_of[i]);
    if(slot->data != NULL) {
        linearFree(slot->data);
    }

    memset(slot, 0, sizeof(*slot));
}

static void bg_fade(u32 i, float seconds) {
    bg_slots[i].target = 0.0f;
    bg_slots[i].fadeTime = seconds;
}

void bgmusic_set(bgmusic_style style, u32 volumePercent) {
    bg_wanted = style < BGMUSIC_COUNT ? style : BGMUSIC_OFF;
    bg_volume = volumePercent > 100 ? 100 : volumePercent;
    if(bg_channels_ready) {
        for(u32 i = 0; i < BG_CHANNELS; i++) {
            if(bg_slots[i].playing) {
                bg_slot_mix(i);
            }
        }
    }
}

void bgmusic_get_status(bgmusic_status* out) {
    out->generating = bg_render_state == BG_RENDERING;
    out->progress = bg_progress > 100 ? 100 : bg_progress;
    out->playing = false;
    for(u32 i = 0; i < BG_CHANNELS; i++) {
        if(bg_slots[i].playing && bg_slots[i].gain > 0.05f) {
            out->playing = true;
        }
    }
}

void bgmusic_update() {
    u64 now = osGetTime();
    float dt = bg_last_update == 0 ? 0.0f : (float) (now - bg_last_update) / 1000.0f;
    bg_last_update = now;
    if(dt > 0.2f) {
        dt = 0.2f;
    }

    // A finished song: it replaces the one playing, with a crossfade, or starts the music.
    if(bg_render_state == BG_DONE) {
        bg_render_state = BG_IDLE;
        s16* data = bg_data;
        bg_data = NULL;
        if(data != NULL) {
            bool used = false;
            if(bg_render_style == bg_wanted && sound_dsp_start()) {
                bool another = false;
                for(u32 i = 0; i < BG_CHANNELS; i++) {
                    if(bg_slots[i].playing && bg_slots[i].style == bg_wanted && bg_slots[i].target > 0) {
                        another = true;
                    }
                }

                // The one that was playing goes out while the new one comes in.
                if(bg_slot_start(bg_render_style, data, bg_frames, another ? 3.5f : 2.0f)) {
                    used = true;
                    if(another) {
                        for(u32 i = 0; i < BG_CHANNELS; i++) {
                            if(bg_slots[i].playing && bg_slots[i].style == bg_wanted && bg_slots[i].data != data) {
                                bg_fade(i, 3.5f);
                            }
                        }
                    }
                }
            }

            if(!used) {
                linearFree(data);
            }
        }
    }

    if(bg_wanted == BGMUSIC_OFF) {
        bg_session = BGMUSIC_OFF;
        if(bg_render_state == BG_RENDERING) {
            bg_cancel = true;
        }

        for(u32 i = 0; i < BG_CHANNELS; i++) {
            if(bg_slots[i].playing && bg_slots[i].target > 0) {
                bg_fade(i, 0.8f);
            }
        }
    } else if(sound_dsp_start()) {
        // Songs of another style go out quickly.
        for(u32 i = 0; i < BG_CHANNELS; i++) {
            if(bg_slots[i].playing && bg_slots[i].style != bg_wanted && bg_slots[i].target > 0) {
                bg_fade(i, 1.2f);
            }
        }

        bool playingWanted = false;
        for(u32 i = 0; i < BG_CHANNELS; i++) {
            if(bg_slots[i].playing && bg_slots[i].style == bg_wanted && bg_slots[i].target > 0) {
                playingWanted = true;
            }
        }

        // The last song of this style, from the SD card, so there is music at once.
        if(!playingWanted && bg_session != bg_wanted) {
            s16* data = NULL;
            u32 frames = 0;
            if(bg_cache_load(bg_wanted, &data, &frames)) {
                if(!bg_slot_start(bg_wanted, data, frames, 2.0f)) {
                    linearFree(data);
                }
            }
        }

        // A new one is made meanwhile (once for each time the music is turned on or the style changes).
        if(bg_session != bg_wanted) {
            if(bg_render_state == BG_IDLE) {
                bg_session = bg_wanted;
                bg_cancel = false;
                bg_progress = 0;
                bg_render_state = BG_RENDERING;
                bg_render_style = bg_wanted;
                // The lowest priority, on the system core: it must never get in the UI's way.
                if(threadCreate(bg_render_thread, (void*) (uintptr_t) bg_wanted, 0x10000, 0x3F, 1, true) == NULL) {
                    bg_render_state = BG_IDLE;
                    bg_wanted = BGMUSIC_OFF;
                }
            } else if(bg_render_state == BG_RENDERING && bg_render_style != bg_wanted) {
                // A song of another style is being made: stop it, the right one starts after.
                bg_cancel = true;
            }
        }
    }

    // Fades, volume and ducking (the audio player on channel 0 has the speakers).
    bool player = bg_channels_ready && ndspChnIsPlaying(0);
    bool duckChanged = player != bg_ducked;
    bg_ducked = player;
    for(u32 i = 0; i < BG_CHANNELS; i++) {
        bg_slot* slot = &bg_slots[i];
        if(!slot->playing) {
            continue;
        }

        bool moved = false;
        if(slot->gain != slot->target) {
            float step = slot->fadeTime > 0 ? dt / slot->fadeTime : 1.0f;
            if(slot->gain < slot->target) {
                slot->gain = slot->gain + step > slot->target ? slot->target : slot->gain + step;
            } else {
                slot->gain = slot->gain - step < slot->target ? slot->target : slot->gain - step;
            }

            moved = true;
        }

        if(slot->target == 0.0f && slot->gain <= 0.0f) {
            bg_slot_stop(i);
        } else if(moved || duckChanged) {
            bg_slot_mix(i);
        }
    }
}

void bgmusic_exit() {
    bg_wanted = BGMUSIC_OFF;
    bg_cancel = true;
    for(u32 waited = 0; bg_render_state == BG_RENDERING && waited < 8000; waited += 10) {
        svcSleepThread(10 * 1000000LL);
    }

    for(u32 i = 0; i < BG_CHANNELS; i++) {
        bg_slot_stop(i);
    }

    if(bg_data != NULL) {
        linearFree(bg_data);
        bg_data = NULL;
    }

    bg_render_state = BG_IDLE;
    bg_channels_ready = false;
    bg_session = BGMUSIC_OFF;
}
