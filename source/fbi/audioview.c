// Audio player: MP3, WAV, FLAC and Ogg Vorbis, streamed from the card and played through the
// DSP. A thread decodes into a ring of buffers; the screens show the track and its controls.

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <tremor/ivorbisfile.h>

#include "viewers.h"
#include "../core/core.h"
#include "../libs/dr_libs/dr_flac.h"
#include "../libs/dr_libs/dr_mp3.h"
#include "../libs/dr_libs/dr_wav.h"

#define AUDIO_CHANNEL 0
#define AUDIO_BUFFERS 4
#define AUDIO_FRAMES 4096
#define AUDIO_FILE_BUFFER (64 * 1024)
#define AUDIO_STACK 0x10000
#define AUDIO_PRIORITY 0x30
#define AUDIO_SEEK_STEP 10

// ---- the file, read through a buffer (a decoder asks for a few KB at a time) ----

typedef struct {
    Handle handle;
    u64 size;
    u64 pos;
    u8* buf;
    u64 bufStart;
    u32 bufLen;
} audio_file;

static u32 audio_file_read(audio_file* f, void* out, u32 n) {
    u32 total = 0;
    while(total < n && f->pos < f->size) {
        if(f->bufLen == 0 || f->pos < f->bufStart || f->pos >= f->bufStart + f->bufLen) {
            u32 got = 0;
            if(R_FAILED(FSFILE_Read(f->handle, &got, f->pos, f->buf, AUDIO_FILE_BUFFER)) || got == 0) {
                break;
            }

            f->bufStart = f->pos;
            f->bufLen = got;
        }

        u32 off = (u32) (f->pos - f->bufStart);
        u32 copy = f->bufLen - off;
        if(copy > n - total) {
            copy = n - total;
        }

        memcpy((u8*) out + total, f->buf + off, copy);
        total += copy;
        f->pos += copy;
    }

    return total;
}

// origin: 0 from the start, 1 from here, 2 from the end.
static bool audio_file_seek(audio_file* f, s64 offset, int origin) {
    s64 base = origin == 0 ? 0 : origin == 1 ? (s64) f->pos : (s64) f->size;
    s64 target = base + offset;
    if(target < 0 || target > (s64) f->size) {
        return false;
    }

    f->pos = (u64) target;
    return true;
}

// ---- decoders, behind one interface ----

typedef enum {
    AUDIO_WAV,
    AUDIO_MP3,
    AUDIO_FLAC,
    AUDIO_OGG
} audio_kind;

typedef struct {
    audio_kind kind;
    audio_file file;
    bool fileOpen;

    drwav wav;
    drmp3 mp3;
    drflac* flac;
    OggVorbis_File ogg;
    bool decoderOpen;

    u32 channels;
    u32 rate;
    u64 total;   // In frames; 0 when unknown.
} audio_dec;

static size_t audio_cb_read(void* user, void* out, size_t n) { return audio_file_read((audio_file*) user, out, (u32) n); }
static drmp3_bool32 audio_cb_seek_mp3(void* user, int offset, drmp3_seek_origin origin) { return audio_file_seek((audio_file*) user, offset, (int) origin); }
static drmp3_bool32 audio_cb_tell_mp3(void* user, drmp3_int64* cursor) { *cursor = (drmp3_int64) ((audio_file*) user)->pos; return 1; }
static drflac_bool32 audio_cb_seek_flac(void* user, int offset, drflac_seek_origin origin) { return audio_file_seek((audio_file*) user, offset, (int) origin); }
static drflac_bool32 audio_cb_tell_flac(void* user, drflac_int64* cursor) { *cursor = (drflac_int64) ((audio_file*) user)->pos; return 1; }
static drwav_bool32 audio_cb_seek_wav(void* user, int offset, drwav_seek_origin origin) { return audio_file_seek((audio_file*) user, offset, (int) origin); }
static drwav_bool32 audio_cb_tell_wav(void* user, drwav_int64* cursor) { *cursor = (drwav_int64) ((audio_file*) user)->pos; return 1; }

static size_t audio_ogg_read(void* ptr, size_t size, size_t nmemb, void* user) {
    return size == 0 ? 0 : audio_file_read((audio_file*) user, ptr, (u32) (size * nmemb)) / size;
}

static int audio_ogg_seek(void* user, ogg_int64_t offset, int whence) { return audio_file_seek((audio_file*) user, (s64) offset, whence) ? 0 : -1; }
static int audio_ogg_close(void* user) { return 0; }
static long audio_ogg_tell(void* user) { return (long) ((audio_file*) user)->pos; }

static void audio_dec_close(audio_dec* d) {
    if(d->decoderOpen) {
        switch(d->kind) {
            case AUDIO_WAV: drwav_uninit(&d->wav); break;
            case AUDIO_MP3: drmp3_uninit(&d->mp3); break;
            case AUDIO_FLAC: drflac_close(d->flac); break;
            case AUDIO_OGG: ov_clear(&d->ogg); break;
        }
    }

    if(d->fileOpen) {
        FSFILE_Close(d->file.handle);
    }

    free(d->file.buf);
    free(d);
}

static bool audio_kind_of(const char* path, audio_kind* kind) {
    const char* dot = strrchr(path, '.');
    if(dot == NULL) {
        return false;
    }

    if(strcasecmp(dot, ".mp3") == 0) { *kind = AUDIO_MP3; return true; }
    if(strcasecmp(dot, ".wav") == 0) { *kind = AUDIO_WAV; return true; }
    if(strcasecmp(dot, ".flac") == 0) { *kind = AUDIO_FLAC; return true; }
    if(strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".oga") == 0) { *kind = AUDIO_OGG; return true; }
    return false;
}

static audio_dec* audio_dec_open(FS_Archive archive, const char* path, const char** error) {
    audio_dec* d = (audio_dec*) calloc(1, sizeof(audio_dec));
    if(d == NULL) {
        *error = _("Not enough memory.");
        return NULL;
    }

    if(!audio_kind_of(path, &d->kind)) {
        *error = _("This file cannot be played.");
        free(d);
        return NULL;
    }

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        *error = _("Not enough memory.");
        free(d);
        return NULL;
    }

    Result res = FSUSER_OpenFile(&d->file.handle, archive, *fsPath, FS_OPEN_READ, 0);
    fs_free_path_utf8(fsPath);
    if(R_FAILED(res) || R_FAILED(FSFILE_GetSize(d->file.handle, &d->file.size))) {
        *error = _("The file cannot be read.");
        if(R_SUCCEEDED(res)) {
            FSFILE_Close(d->file.handle);
        }

        free(d);
        return NULL;
    }

    d->fileOpen = true;
    d->file.buf = (u8*) malloc(AUDIO_FILE_BUFFER);
    if(d->file.buf == NULL) {
        *error = _("Not enough memory.");
        audio_dec_close(d);
        return NULL;
    }

    bool ok = false;
    switch(d->kind) {
        case AUDIO_WAV:
            if(drwav_init(&d->wav, audio_cb_read, audio_cb_seek_wav, audio_cb_tell_wav, &d->file, NULL)) {
                d->decoderOpen = ok = true;
                d->channels = d->wav.channels;
                d->rate = d->wav.sampleRate;
                d->total = d->wav.totalPCMFrameCount;
            }

            break;
        case AUDIO_MP3:
            if(drmp3_init(&d->mp3, audio_cb_read, audio_cb_seek_mp3, audio_cb_tell_mp3, NULL, &d->file, NULL)) {
                d->decoderOpen = ok = true;
                d->channels = d->mp3.channels;
                d->rate = d->mp3.sampleRate;
                d->total = d->mp3.totalPCMFrameCount == DRMP3_UINT64_MAX ? 0 : d->mp3.totalPCMFrameCount;
            }

            break;
        case AUDIO_FLAC:
            d->flac = drflac_open(audio_cb_read, audio_cb_seek_flac, audio_cb_tell_flac, &d->file, NULL);
            if(d->flac != NULL) {
                d->decoderOpen = ok = true;
                d->channels = d->flac->channels;
                d->rate = d->flac->sampleRate;
                d->total = d->flac->totalPCMFrameCount;
            }

            break;
        case AUDIO_OGG: {
            ov_callbacks callbacks = {audio_ogg_read, audio_ogg_seek, audio_ogg_close, audio_ogg_tell};
            if(ov_open_callbacks(&d->file, &d->ogg, NULL, 0, callbacks) == 0) {
                d->decoderOpen = ok = true;
                vorbis_info* info = ov_info(&d->ogg, -1);
                d->channels = (u32) info->channels;
                d->rate = (u32) info->rate;
                ogg_int64_t total = ov_pcm_total(&d->ogg, -1);
                d->total = total > 0 ? (u64) total : 0;
            }

            break;
        }
    }

    if(!ok) {
        *error = _("This file cannot be played.");
        audio_dec_close(d);
        return NULL;
    }

    if(d->channels < 1 || d->channels > 2 || d->rate < 8000 || d->rate > 96000) {
        *error = _("Only mono and stereo sound is supported.");
        audio_dec_close(d);
        return NULL;
    }

    return d;
}

// Frames of 16-bit sound into `out`; fewer than asked at the end.
static u64 audio_dec_read(audio_dec* d, s16* out, u64 frames) {
    switch(d->kind) {
        case AUDIO_WAV: return drwav_read_pcm_frames_s16(&d->wav, frames, out);
        case AUDIO_MP3: return drmp3_read_pcm_frames_s16(&d->mp3, frames, out);
        case AUDIO_FLAC: return drflac_read_pcm_frames_s16(d->flac, frames, out);
        case AUDIO_OGG: {
            u32 frameBytes = d->channels * 2;
            u32 want = (u32) frames * frameBytes;
            u32 got = 0;
            u32 failures = 0;
            while(got < want) {
                int bitstream = 0;
                long n = ov_read(&d->ogg, (char*) out + got, (int) (want - got), &bitstream);
                if(n == 0) {
                    break;
                }

                if(n < 0) {
                    // A damaged page: skip it, but not forever.
                    if(++failures > 50) {
                        break;
                    }

                    continue;
                }

                got += (u32) n;
            }

            return got / frameBytes;
        }
    }

    return 0;
}

static bool audio_dec_seek(audio_dec* d, u64 frame) {
    switch(d->kind) {
        case AUDIO_WAV: return drwav_seek_to_pcm_frame(&d->wav, frame);
        case AUDIO_MP3: return drmp3_seek_to_pcm_frame(&d->mp3, frame);
        case AUDIO_FLAC: return drflac_seek_to_pcm_frame(d->flac, frame);
        case AUDIO_OGG: return ov_pcm_seek(&d->ogg, (ogg_int64_t) frame) == 0;
    }

    return false;
}

// ---- the player: a thread that keeps the DSP's buffers full ----

typedef struct {
    audio_dec* dec;
    s16* pcm[AUDIO_BUFFERS];
    ndspWaveBuf wave[AUDIO_BUFFERS];
    u32 frames[AUDIO_BUFFERS];
    bool pending[AUDIO_BUFFERS];   // Queued and not counted as played yet.

    Thread thread;
    volatile bool quit;
    volatile bool eof;
    volatile bool ended;
    volatile s64 seekTo;           // A frame to go to, or -1.
    volatile u64 played;           // Frames whose buffer has finished.
} audio_player;

static void audio_player_thread(void* arg) {
    audio_player* p = (audio_player*) arg;
    audio_dec* d = p->dec;

    while(!p->quit) {
        if(p->seekTo >= 0) {
            u64 target = (u64) p->seekTo;
            ndspChnWaveBufClear(AUDIO_CHANNEL);
            // Wait for the DSP to let go of the buffers.
            for(u32 waited = 0; waited < 300; waited++) {
                bool busy = false;
                for(u32 i = 0; i < AUDIO_BUFFERS; i++) {
                    busy = busy || p->wave[i].status == NDSP_WBUF_QUEUED || p->wave[i].status == NDSP_WBUF_PLAYING;
                }

                if(!busy) {
                    break;
                }

                svcSleepThread(1000000);
            }

            for(u32 i = 0; i < AUDIO_BUFFERS; i++) {
                p->pending[i] = false;
                p->wave[i].status = NDSP_WBUF_FREE;
            }

            if(d->total > 0 && target >= d->total) {
                target = d->total > 0 ? d->total - 1 : 0;
            }

            audio_dec_seek(d, target);
            p->played = target;
            p->eof = false;
            p->ended = false;
            p->seekTo = -1;
        }

        for(u32 i = 0; i < AUDIO_BUFFERS && !p->quit && p->seekTo < 0; i++) {
            ndspWaveBuf* wave = &p->wave[i];
            if(p->pending[i] && wave->status == NDSP_WBUF_DONE) {
                p->played += p->frames[i];
                p->pending[i] = false;
            }

            if(!p->pending[i] && !p->eof && (wave->status == NDSP_WBUF_FREE || wave->status == NDSP_WBUF_DONE)) {
                u64 got = audio_dec_read(d, p->pcm[i], AUDIO_FRAMES);
                if(got == 0) {
                    p->eof = true;
                    continue;
                }

                p->frames[i] = (u32) got;
                wave->data_pcm16 = p->pcm[i];
                wave->nsamples = (u32) got;
                DSP_FlushDataCache(p->pcm[i], (u32) got * d->channels * 2);
                p->pending[i] = true;
                ndspChnWaveBufAdd(AUDIO_CHANNEL, wave);
            }
        }

        if(p->eof && p->seekTo < 0) {
            bool any = false;
            for(u32 i = 0; i < AUDIO_BUFFERS; i++) {
                any = any || p->pending[i];
            }

            p->ended = !any;
        }

        svcSleepThread(4000000);
    }
}

static Thread audio_player_thread_start(audio_player* p) {
    bool isNew = false;
    Thread thread = NULL;
    if(R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew) {
        thread = threadCreate(audio_player_thread, p, AUDIO_STACK, AUDIO_PRIORITY, 2, false);
    }

    if(thread == NULL) {
        thread = threadCreate(audio_player_thread, p, AUDIO_STACK, AUDIO_PRIORITY, -2, false);
    }

    return thread;
}

static void audio_player_stop(audio_player* p) {
    if(p == NULL) {
        return;
    }

    p->quit = true;
    if(p->thread != NULL) {
        threadJoin(p->thread, U64_MAX);
        threadFree(p->thread);
    }

    ndspChnWaveBufClear(AUDIO_CHANNEL);
    ndspChnReset(AUDIO_CHANNEL);
    for(u32 i = 0; i < AUDIO_BUFFERS; i++) {
        if(p->pcm[i] != NULL) {
            linearFree(p->pcm[i]);
        }
    }

    if(p->dec != NULL) {
        audio_dec_close(p->dec);
    }

    free(p);
}

static audio_player* audio_player_start(FS_Archive archive, const char* path, bool paused, u64 startFrame, const char** error) {
    audio_dec* dec = audio_dec_open(archive, path, error);
    if(dec == NULL) {
        return NULL;
    }

    audio_player* p = (audio_player*) calloc(1, sizeof(audio_player));
    if(p == NULL) {
        *error = _("Not enough memory.");
        audio_dec_close(dec);
        return NULL;
    }

    p->dec = dec;
    p->seekTo = -1;
    for(u32 i = 0; i < AUDIO_BUFFERS; i++) {
        p->pcm[i] = (s16*) linearAlloc(AUDIO_FRAMES * dec->channels * sizeof(s16));
        if(p->pcm[i] == NULL) {
            *error = _("Not enough memory.");
            audio_player_stop(p);
            return NULL;
        }
    }

    ndspChnReset(AUDIO_CHANNEL);
    ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CHANNEL, (float) dec->rate);
    ndspChnSetFormat(AUDIO_CHANNEL, dec->channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {1.0f, 1.0f};
    ndspChnSetMix(AUDIO_CHANNEL, mix);
    ndspChnSetPaused(AUDIO_CHANNEL, paused);

    if(startFrame > 0) {
        p->seekTo = (s64) startFrame;
    }

    p->thread = audio_player_thread_start(p);
    if(p->thread == NULL) {
        *error = _("Not enough memory.");
        audio_player_stop(p);
        return NULL;
    }

    return p;
}

// ---- the view ----

typedef struct {
    FS_Archive archive;
    char** paths;
    u32 count;
    u32 index;
    ui_view* view;
    char name[FILE_NAME_MAX];

    bool dspReady;
    audio_player* player;
    char error[96];
    bool paused;
    bool finished;      // The last track ended.
    float volume;

    int pressed;
    int dragging;       // 1 the seek bar, 2 the volume bar.
    int touchX;         // Where the finger was last.
} audio_data;

#define AUDIO_BUTTONS 5

static void audio_play_index(audio_data* data, u32 index);

static void audio_stop(audio_data* data) {
    audio_player_stop(data->player);
    data->player = NULL;
}

static void audio_play_index(audio_data* data, u32 index) {
    audio_stop(data);
    data->index = index;
    data->finished = false;
    data->error[0] = '\0';
    string_get_path_file(data->name, data->paths[index], sizeof(data->name));

    if(!data->dspReady) {
        return;
    }

    const char* error = NULL;
    data->player = audio_player_start(data->archive, data->paths[index], false, 0, &error);
    data->paused = false;
    if(data->player == NULL) {
        snprintf(data->error, sizeof(data->error), "%s", error != NULL ? error : _("This file cannot be played."));
    }
}

static void audio_close(ui_view* view, audio_data* data) {
    ui_pop();
    audio_stop(data);
    if(data->dspReady) {
        ndspExit();
    }

    for(u32 i = 0; i < data->count; i++) {
        free(data->paths[i]);
    }

    free(data->paths);
    free(data);
    ui_destroy(view);
}

static u64 audio_position(audio_data* data) {
    return data->player != NULL ? data->player->played : 0;
}

static void audio_seek_by(audio_data* data, int seconds) {
    if(data->player == NULL) {
        return;
    }

    audio_dec* d = data->player->dec;
    s64 target = (s64) audio_position(data) + (s64) seconds * d->rate;
    if(target < 0) {
        target = 0;
    }

    if(d->total > 0 && target >= (s64) d->total) {
        return;
    }

    data->player->seekTo = target;
}

static void audio_seek_fraction(audio_data* data, float fraction) {
    if(data->player == NULL || data->player->dec->total == 0) {
        return;
    }

    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    data->player->seekTo = (s64) (fraction * (float) (data->player->dec->total - 1));
}

static void audio_set_volume(audio_data* data, float volume) {
    data->volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
    if(data->dspReady) {
        ndspSetMasterVol(data->volume);
    }
}

static void audio_toggle(audio_data* data) {
    if(data->finished || (data->player == NULL && data->error[0] == '\0')) {
        audio_play_index(data, data->index);
        return;
    }

    if(data->player == NULL) {
        return;
    }

    data->paused = !data->paused;
    ndspChnSetPaused(AUDIO_CHANNEL, data->paused);
}

static void audio_press(audio_data* data, int button) {
    switch(button) {
        case 0: audio_play_index(data, (data->index + data->count - 1) % data->count); break;
        case 1: audio_seek_by(data, -AUDIO_SEEK_STEP); break;
        case 2: audio_toggle(data); break;
        case 3: audio_seek_by(data, AUDIO_SEEK_STEP); break;
        case 4: audio_play_index(data, (data->index + 1) % data->count); break;
        default: break;
    }
}

static void audio_time_text(char* out, size_t size, u64 frames, u32 rate) {
    u32 seconds = rate > 0 ? (u32) (frames / rate) : 0;
    snprintf(out, size, "%lu:%02lu", seconds / 60, seconds % 60);
}

static void audio_line(audio_data* data, char* out, size_t size) {
    audio_dec* d = data->player != NULL ? data->player->dec : NULL;
    if(d == NULL) {
        out[0] = '\0';
        return;
    }

    static const char* kinds[] = {"WAV", "MP3", "FLAC", "OGG"};
    snprintf(out, size, "%s   %lu Hz   %s", kinds[d->kind], d->rate, d->channels == 1 ? _("mono") : _("stereo"));
}

static void audio_times(audio_data* data, char* out, size_t size) {
    out[0] = '\0';
    if(data->player == NULL) {
        return;
    }

    audio_dec* d = data->player->dec;
    char now[16];
    audio_time_text(now, sizeof(now), audio_position(data), d->rate);
    if(d->total > 0) {
        char total[16];
        audio_time_text(total, sizeof(total), d->total, d->rate);
        snprintf(out, size, "%s / %s", now, total);
    } else {
        snprintf(out, size, "%s", now);
    }
}

static float audio_progress(audio_data* data) {
    if(data->player == NULL || data->player->dec->total == 0) {
        return 0;
    }

    return (float) audio_position(data) / (float) data->player->dec->total;
}

static void audio_draw_top(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    audio_data* data = (audio_data*) v;
    const theme_current* t = theme();

    float cardX = x1 + 12;
    float cardY = y1 + 14;
    float cardW = x2 - x1 - 24;
    float cardH = y2 - y1 - 28;
    widget_card(cardX, cardY, cardW, cardH);

    float tile = 72;
    float tileX = cardX + (cardW - tile) / 2;
    float tileY = cardY + 12;
    screen_fill_round_rect(tileX, tileY, tile, tile, t->def->radius, t->accentSoft);
    screen_draw_icon(ICON_MUSIC, tileX + 20, tileY + 20, 32, t->accent);

    float y = tileY + tile + 10;
    widget_text_centered(data->name, cardX + 8, cardX + cardW - 8, y, WIDGET_TITLE_SCALE, t->palette->text);
    y += widget_text_height(WIDGET_TITLE_SCALE) + 3;

    char line[128];
    if(data->player == NULL && data->error[0] != '\0') {
        widget_text_centered(data->error, cardX + 8, cardX + cardW - 8, y, WIDGET_TEXT_SCALE, t->palette->danger);
        return;
    }

    audio_line(data, line, sizeof(line));
    widget_text_centered(line, cardX + 8, cardX + cardW - 8, y, WIDGET_SMALL_SCALE, t->palette->textMuted);
    y += widget_text_height(WIDGET_SMALL_SCALE) + 8;

    audio_times(data, line, sizeof(line));
    widget_text_centered(line, cardX + 8, cardX + cardW - 8, y, WIDGET_TEXT_SCALE, t->palette->text);
    y += widget_text_height(WIDGET_TEXT_SCALE) + 6;
    widget_progress(cardX + 20, y, cardW - 40, 6, audio_progress(data));

    if(data->count > 1) {
        snprintf(line, sizeof(line), "%lu / %lu", data->index + 1, data->count);
        screen_draw_text(line, cardX + cardW - 10 - widget_text_width(line, WIDGET_SMALL_SCALE), cardY + 8, WIDGET_SMALL_SCALE, t->palette->textMuted);
    }
}

static void audio_button_rect(int i, float* x, float* y, float* w, float* h) {
    float gap = 6;
    *w = (BOTTOM_SCREEN_WIDTH - 16 - gap * (AUDIO_BUTTONS - 1)) / AUDIO_BUTTONS;
    *h = 40;
    *x = 8 + i * (*w + gap);
    *y = BOTTOM_SCREEN_HEIGHT - WIDGET_HINT_BAR_HEIGHT - 8 - *h;
}

// The seek bar's touch area, then the volume bar's.
static void audio_bar_rect(int which, float* x, float* y, float* w, float* h) {
    *x = 16;
    *w = BOTTOM_SCREEN_WIDTH - 32;
    *h = 24;
    *y = which == 1 ? 128 : 98;
}

static void audio_draw_bottom(ui_view* view, void* v, float x1, float y1, float x2, float y2) {
    audio_data* data = (audio_data*) v;
    const theme_current* t = theme();

    float x, y, w, h;
    float tx = 16;

    widget_text_fit(data->name, tx, y1 + 8, WIDGET_TEXT_SCALE, t->palette->text, x2 - 2 * tx);
    char line[128];
    if(data->player == NULL && data->error[0] != '\0') {
        widget_text_fit(data->error, tx, y1 + 8 + widget_text_height(WIDGET_TEXT_SCALE) + 4, WIDGET_SMALL_SCALE, t->palette->danger, x2 - 2 * tx);
    } else {
        audio_line(data, line, sizeof(line));
        widget_text_fit(line, tx, y1 + 8 + widget_text_height(WIDGET_TEXT_SCALE) + 4, WIDGET_SMALL_SCALE, t->palette->textMuted, x2 - 2 * tx);
    }

    // Seek bar with the time.
    audio_bar_rect(0, &x, &y, &w, &h);
    audio_times(data, line, sizeof(line));
    widget_text_centered(line, x1, x2, y1 + 50, WIDGET_TEXT_SCALE, t->palette->text);
    widget_progress(x, y + 8, w, 8, audio_progress(data));

    // Volume.
    audio_bar_rect(1, &x, &y, &w, &h);
    screen_draw_icon(ICON_VOLUME, x, y + 4, 16, t->palette->textMuted);
    widget_progress(x + 24, y + 8, w - 24, 8, data->volume);

    for(int i = 0; i < AUDIO_BUTTONS; i++) {
        audio_button_rect(i, &x, &y, &w, &h);
        bool pressed = data->pressed == i;
        bool disabled = (i == 0 || i == 4) && data->count < 2;
        u32 icon = i == 0 ? ICON_PLAYER_TRACK_PREV : i == 1 ? ICON_REWIND_BACKWARD_10 : i == 2 ? (data->paused || data->finished || data->player == NULL ? ICON_PLAYER_PLAY : ICON_PLAYER_PAUSE) : i == 3 ? ICON_REWIND_FORWARD_10 : ICON_PLAYER_TRACK_NEXT;
        u32 fill = pressed ? t->accent : i == 2 ? t->accentSoft : t->palette->surface;
        u32 edge = pressed ? t->accent : i == 2 ? t->accent : t->palette->border;
        screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, edge, fill);
        u32 ink = pressed ? t->onAccent : disabled ? t->palette->textMuted : i == 2 ? t->accent : t->palette->text;
        screen_draw_icon(icon, x + (w - 16) / 2, y + (h - 16) / 2, 16, ink);
    }
}

static void audio_update(ui_view* view, void* v, float bx1, float by1, float bx2, float by2) {
    audio_data* data = (audio_data*) v;

    view->name = data->name;
    view->info = _(" pause     track    back");

    if(hidKeysDown() & KEY_B) {
        audio_close(view, data);
        return;
    }

    // A track that ends goes on to the next; the last one stops.
    if(data->player != NULL && data->player->ended && !data->finished) {
        if(data->index + 1 < data->count) {
            audio_play_index(data, data->index + 1);
        } else {
            data->finished = true;
            data->paused = true;
        }
    }

    if(hidKeysDown() & KEY_A) audio_toggle(data);
    if(hidKeysDown() & KEY_L) audio_press(data, 0);
    if(hidKeysDown() & KEY_R) audio_press(data, 4);
    if(hidKeysDown() & KEY_LEFT) audio_seek_by(data, -AUDIO_SEEK_STEP);
    if(hidKeysDown() & KEY_RIGHT) audio_seek_by(data, AUDIO_SEEK_STEP);
    if(hidKeysDown() & KEY_UP) audio_set_volume(data, data->volume + 0.1f);
    if(hidKeysDown() & KEY_DOWN) audio_set_volume(data, data->volume - 0.1f);

    touchPosition pos;
    hidTouchRead(&pos);
    if(hidKeysDown() & KEY_TOUCH) {
        data->pressed = -1;
        data->dragging = 0;
        for(int i = 0; i < AUDIO_BUTTONS; i++) {
            float x, y, w, h;
            audio_button_rect(i, &x, &y, &w, &h);
            if(widget_touched(&pos, x, y, w, h)) {
                data->pressed = i;
            }
        }

        for(int bar = 0; bar < 2 && data->pressed < 0; bar++) {
            float x, y, w, h;
            audio_bar_rect(bar, &x, &y, &w, &h);
            if(widget_touched(&pos, x, y, w, h)) {
                data->dragging = bar + 1;
            }
        }
    }

    if(hidKeysHeld() & KEY_TOUCH) {
        data->touchX = pos.px;
    }

    if((hidKeysHeld() & KEY_TOUCH) && data->dragging == 2) {
        float x, y, w, h;
        audio_bar_rect(1, &x, &y, &w, &h);
        audio_set_volume(data, (pos.px - (x + 24)) / (w - 24));
    } else if((hidKeysHeld() & KEY_TOUCH) && data->pressed >= 0) {
        float x, y, w, h;
        audio_button_rect(data->pressed, &x, &y, &w, &h);
        if(!widget_touched(&pos, x, y, w, h)) {
            data->pressed = -1;
        }
    }

    if(hidKeysUp() & KEY_TOUCH) {
        if(data->pressed >= 0) {
            int button = data->pressed;
            data->pressed = -1;
            audio_press(data, button);
        } else if(data->dragging == 1) {
            // A seek happens on release, so the track does not jump while the finger moves.
            float x, y, w, h;
            audio_bar_rect(0, &x, &y, &w, &h);
            audio_seek_fraction(data, ((float) data->touchX - x) / w);
        }

        data->dragging = 0;
    }
}

void audioview_open(FS_Archive archive, const char* const* paths, u32 count, u32 index) {
    if(count == 0 || index >= count) {
        return;
    }

    audio_data* data = (audio_data*) calloc(1, sizeof(audio_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate audio player data.");
        return;
    }

    data->paths = (char**) calloc(count, sizeof(char*));
    for(u32 i = 0; i < count && data->paths != NULL; i++) {
        data->paths[i] = strdup(paths[i]);
        if(data->paths[i] == NULL) {
            for(u32 j = 0; j < i; j++) {
                free(data->paths[j]);
            }

            free(data->paths);
            data->paths = NULL;
        }
    }

    if(data->paths == NULL) {
        free(data);
        error_display(NULL, NULL, "Failed to allocate audio player data.");
        return;
    }

    data->archive = archive;
    data->count = count;
    data->volume = 1.0f;
    data->pressed = -1;

    // The DSP needs its firmware (sdmc:/3ds/dspfirm.cdc) dumped from the console.
    Result res = ndspInit();
    data->dspReady = R_SUCCEEDED(res);
    if(!data->dspReady) {
        snprintf(data->error, sizeof(data->error), "%s", _("No DSP firmware: dump it with DSP1 to play sound."));
    } else {
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspSetMasterVol(data->volume);
    }

    ui_view* view = ui_create();
    view->name = data->name;
    view->info = "";
    view->data = data;
    view->update = audio_update;
    view->drawTop = audio_draw_top;
    view->drawBottom = audio_draw_bottom;
    data->view = view;
    ui_push(view);

    if(data->dspReady) {
        audio_play_index(data, index);
    } else {
        data->index = index;
        string_get_path_file(data->name, data->paths[index], sizeof(data->name));
    }
}
