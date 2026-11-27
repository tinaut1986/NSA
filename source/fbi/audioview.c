// Audio player: MP3, WAV, FLAC and Ogg Vorbis, streamed from the card and played through the
// DSP. A thread decodes into a ring of buffers; the screens show the track and its controls.

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "settings.h"
#include "viewers.h"
#include "../core/core.h"
#include "../core/audiodec.h"

#define AUDIO_CHANNEL 0
#define AUDIO_BUFFERS 4
#define AUDIO_FRAMES 4096
#define AUDIO_STACK 0x10000
#define AUDIO_PRIORITY 0x30
#define AUDIO_SEEK_STEP 10

// ---- an MP3's seek table, made on a thread of its own ----

// dr_mp3 finds a place in an MP3 by reading it from the start: 4-5 s into a long track on a console, with the screen
// frozen meanwhile on an Old 3DS (its player thread shares the app core). A second decoder reads the whole file once at
// the lowest priority and notes where the frames start; once the player binds that table a seek is immediate.
typedef struct {
    FS_Archive archive;
    char path[FILE_PATH_MAX];
    audio_file file;
    drmp3 mp3;
    volatile bool cancel;   // Reading stops (the reads return nothing) and the thread ends soon.
    volatile bool done;     // `points` and `count` are ready.
    drmp3_seek_point* points;
    drmp3_uint32 count;
} audio_seek_job;

static size_t audio_seek_job_read(void* user, void* out, size_t n) {
    audio_seek_job* job = (audio_seek_job*) user;
    return job->cancel ? 0 : audio_file_read(&job->file, out, (u32) n);
}

static drmp3_bool32 audio_seek_job_seek(void* user, int offset, drmp3_seek_origin origin) {
    return audio_file_seek(&((audio_seek_job*) user)->file, offset, (int) origin);
}

static drmp3_bool32 audio_seek_job_tell(void* user, drmp3_int64* cursor) {
    *cursor = (drmp3_int64) ((audio_seek_job*) user)->file.pos;
    return 1;
}

static void audio_seek_thread(void* arg) {
    audio_seek_job* job = (audio_seek_job*) arg;

    FS_Path* fsPath = fs_make_path_utf8(job->path);
    if(fsPath == NULL) {
        return;
    }

    Result res = FSUSER_OpenFile(&job->file.handle, job->archive, *fsPath, FS_OPEN_READ, 0);
    fs_free_path_utf8(fsPath);
    if(R_FAILED(res)) {
        return;
    }

    job->file.buf = (u8*) malloc(AUDIODEC_FILE_BUFFER);
    if(job->file.buf != NULL && R_SUCCEEDED(FSFILE_GetSize(job->file.handle, &job->file.size))
       && drmp3_init(&job->mp3, audio_seek_job_read, audio_seek_job_seek, audio_seek_job_tell, NULL, job, NULL)) {
        // About one point a second (dr_mp3 keeps fewer when the file has fewer frames).
        u64 total = job->mp3.totalPCMFrameCount != DRMP3_UINT64_MAX ? job->mp3.totalPCMFrameCount : 0;
        u32 rate = job->mp3.sampleRate > 0 ? job->mp3.sampleRate : 44100;
        u64 wanted = total > 0 ? total / rate + 1 : 1024;
        drmp3_uint32 count = (drmp3_uint32) (wanted < 16 ? 16 : wanted > 4096 ? 4096 : wanted);
        drmp3_seek_point* points = (drmp3_seek_point*) malloc(count * sizeof(drmp3_seek_point));
        if(points != NULL && drmp3_calculate_seek_points(&job->mp3, &count, points) && !job->cancel && count > 0) {
            job->points = points;
            job->count = count;
            job->done = true;
        } else {
            free(points);
        }

        drmp3_uninit(&job->mp3);
    }

    FSFILE_Close(job->file.handle);
    free(job->file.buf);
    job->file.buf = NULL;
}

// ---- the player: a thread that keeps the DSP's buffers full ----

typedef struct {
    audio_dec* dec;
    s16* pcm[AUDIO_BUFFERS];
    ndspWaveBuf wave[AUDIO_BUFFERS];
    u32 frames[AUDIO_BUFFERS];
    bool pending[AUDIO_BUFFERS];   // Queued and not counted as played yet.

    Thread thread;
    audio_seek_job* seekJob;       // An MP3's seek table being made (NULL for the other kinds).
    Thread seekThread;
    bool seekBound;                // The table is in the decoder.
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
        if(p->seekJob != NULL && !p->seekBound && p->seekJob->done) {
            drmp3_bind_seek_table(&d->mp3, p->seekJob->count, p->seekJob->points);
            p->seekBound = true;
        }

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

    if(p->seekThread != NULL) {
        p->seekJob->cancel = true;
        threadJoin(p->seekThread, U64_MAX);
        threadFree(p->seekThread);
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

    // After the decoder: it held on to the table.
    if(p->seekJob != NULL) {
        free(p->seekJob->points);
        free(p->seekJob);
    }

    free(p);
}

static audio_player* audio_player_start(FS_Archive archive, const char* path, bool paused, u64 startFrame, float volume, const char** error) {
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
    float mix[12] = {volume, volume};
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

    // An MP3's seek table, at the lowest priority (without it each seek reads the track from its start).
    if(dec->kind == AUDIO_MP3) {
        p->seekJob = (audio_seek_job*) calloc(1, sizeof(audio_seek_job));
        if(p->seekJob != NULL) {
            p->seekJob->archive = archive;
            string_copy(p->seekJob->path, path, sizeof(p->seekJob->path));
            bool isNew = false;
            APT_CheckNew3DS(&isNew);
            p->seekThread = threadCreate(audio_seek_thread, p->seekJob, AUDIO_STACK, 0x3F, isNew ? 2 : -2, false);
        }
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
    u32 repeat;         // settings' playerRepeat: 0 off, 1 the list, 2 the track.

    int pressed;
    int dragging;       // 1 the seek bar, 2 the volume bar.
    int touchX;         // Where the finger was last.
} audio_data;

#define AUDIO_BUTTONS 5

// The player left with B while it played: it goes on behind the other screens (audioview_tick moves it to the next
// track) until it ends, is stopped, or its screen is opened again (audioview_resume). Its view is kept for that.
static audio_data* audio_background = NULL;

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
    data->player = audio_player_start(data->archive, data->paths[index], false, 0, data->volume, &error);
    data->paused = false;
    if(data->player == NULL) {
        snprintf(data->error, sizeof(data->error), "%s", error != NULL ? error : _("This file cannot be played."));
    }
}

// Stops the player and frees its screen (no longer on the stack).
static void audio_free(audio_data* data) {
    ui_view* view = data->view;
    audio_stop(data);
    for(u32 i = 0; i < data->count; i++) {
        free(data->paths[i]);
    }

    free(data->paths);
    free(data);
    if(view != NULL) {
        ui_destroy(view);
    }
}

// B: a track that plays goes on in the background; anything else closes. X (`stop`) always closes.
static void audio_close(ui_view* view, audio_data* data, bool stop) {
    ui_pop();
    if(!stop && data->player != NULL && !data->paused && !data->finished) {
        audio_background = data;
        return;
    }

    audio_free(data);
}

// A track that ends goes on to the next; the last one stops, unless the list or the track repeats.
static void audio_advance(audio_data* data) {
    if(data->player != NULL && data->player->ended && !data->finished) {
        if(data->repeat == 2) {
            audio_play_index(data, data->index);
        } else if(data->index + 1 < data->count) {
            audio_play_index(data, data->index + 1);
        } else if(data->repeat == 1) {
            audio_play_index(data, 0);
        } else {
            data->finished = true;
            data->paused = true;
        }
    }
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
        float mix[12] = {data->volume, data->volume};
        ndspChnSetMix(AUDIO_CHANNEL, mix);
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

// Off, the list, the track: kept in settings.json for the next time.
static void audio_cycle_repeat(audio_data* data) {
    data->repeat = (data->repeat + 1) % 3;
    settings_get()->playerRepeat = data->repeat;
    settings_save();
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

// The seek bar's touch area, then the volume bar's (it leaves room for the repeat button at its right).
static void audio_bar_rect(int which, float* x, float* y, float* w, float* h) {
    *x = 16;
    *w = BOTTOM_SCREEN_WIDTH - 32 - (which == 1 ? 36 : 0);
    *h = 24;
    *y = which == 1 ? 128 : 98;
}

// The repeat button, at the right of the volume bar.
static void audio_repeat_rect(float* x, float* y, float* w, float* h) {
    *w = 28;
    *h = 24;
    *x = BOTTOM_SCREEN_WIDTH - 16 - *w;
    *y = 128;
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

    // Repeat: off (muted), the list, the track.
    audio_repeat_rect(&x, &y, &w, &h);
    u32 repeatIcon = data->repeat == 1 ? ICON_REPEAT : data->repeat == 2 ? ICON_REPEAT_ONCE : ICON_REPEAT_OFF;
    screen_draw_icon(repeatIcon, x + (w - 16) / 2, y + (h - 16) / 2, 16, data->repeat != 0 ? t->accent : t->palette->textMuted);

    for(int i = 0; i < AUDIO_BUTTONS; i++) {
        u32 icon = i == 0 ? ICON_PLAYER_TRACK_PREV : i == 1 ? ICON_REWIND_BACKWARD_10 : i == 2 ? (data->paused || data->finished || data->player == NULL ? ICON_PLAYER_PLAY : ICON_PLAYER_PAUSE) : i == 3 ? ICON_REWIND_FORWARD_10 : ICON_PLAYER_TRACK_NEXT;
        widget_row_button(i, AUDIO_BUTTONS, icon, NULL, data->pressed == i, (i == 0 || i == 4) && data->count < 2, i == 2);
    }
}

static void audio_update(ui_view* view, void* v, float bx1, float by1, float bx2, float by2) {
    audio_data* data = (audio_data*) v;

    view->name = data->name;
    view->info = _("\uE000 pause   \uE004 \uE005 track   \uE003 repeat   \uE002 stop   \uE001 back");

    if(hidKeysDown() & (KEY_B | KEY_X)) {
        audio_close(view, data, (hidKeysDown() & KEY_X) != 0);
        return;
    }

    audio_advance(data);

    if(hidKeysDown() & KEY_A) audio_toggle(data);
    if(hidKeysDown() & KEY_Y) audio_cycle_repeat(data);
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
        data->pressed = widget_button_row_hit(&pos, AUDIO_BUTTONS);

        float rx, ry, rw, rh;
        audio_repeat_rect(&rx, &ry, &rw, &rh);
        if(data->pressed < 0 && widget_touched(&pos, rx, ry, rw, rh)) {
            audio_cycle_repeat(data);
            data->pressed = -1;
            data->dragging = 3;   // Nothing more for this touch.
        }

        for(int bar = 0; bar < 2 && data->pressed < 0 && data->dragging == 0; bar++) {
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
    } else if(hidKeysHeld() & KEY_TOUCH) {
        widget_button_row_hold(&data->pressed, &pos, AUDIO_BUTTONS);
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

void audioview_tick() {
    audio_data* data = audio_background;
    if(data == NULL) {
        return;
    }

    audio_advance(data);
    if(data->finished || data->player == NULL) {
        audio_background = NULL;
        audio_free(data);
    }
}

bool audioview_background(char* name, size_t size) {
    if(audio_background == NULL) {
        return false;
    }

    if(name != NULL) {
        snprintf(name, size, "%s", audio_background->name);
    }

    return true;
}

void audioview_resume() {
    audio_data* data = audio_background;
    if(data == NULL) {
        return;
    }

    audio_background = NULL;
    ui_push(data->view);
}

void audioview_exit() {
    audio_data* data = audio_background;
    audio_background = NULL;
    if(data != NULL) {
        audio_free(data);
    }
}

void audioview_open(FS_Archive archive, const char* const* paths, u32 count, u32 index) {
    if(count == 0 || index >= count) {
        return;
    }

    // The track playing in the background chosen again: its player as it is, not that track from its start.
    audio_data* playing = audio_background;
    if(playing != NULL && playing->archive == archive && strcmp(playing->paths[playing->index], paths[index]) == 0) {
        audioview_resume();
        return;
    }

    // Another list replaces what plays in the background.
    audioview_exit();

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
    data->repeat = settings_get()->playerRepeat <= 2 ? settings_get()->playerRepeat : 0;

    // The DSP needs its firmware (sdmc:/3ds/dspfirm.cdc) dumped from the console.
    data->dspReady = sound_dsp_start();
    if(!data->dspReady) {
        snprintf(data->error, sizeof(data->error), "%s", _("No DSP firmware: dump it with DSP1 to play sound."));
    } else {
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
