#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "audiodec.h"
#include "core.h"

// ---- the file, read through a buffer (a decoder asks for a few KB at a time) ----

u32 audio_file_read(audio_file* f, void* out, u32 n) {
    u32 total = 0;
    while(total < n && f->pos < f->size) {
        if(f->bufLen == 0 || f->pos < f->bufStart || f->pos >= f->bufStart + f->bufLen) {
            u32 got = 0;
            if(R_FAILED(FSFILE_Read(f->handle, &got, f->pos, f->buf, AUDIODEC_FILE_BUFFER)) || got == 0) {
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
bool audio_file_seek(audio_file* f, s64 offset, int origin) {
    s64 base = origin == 0 ? 0 : origin == 1 ? (s64) f->pos : (s64) f->size;
    s64 target = base + offset;
    if(target < 0 || target > (s64) f->size) {
        return false;
    }

    f->pos = (u64) target;
    return true;
}

// ---- decoders, behind one interface ----

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

void audio_dec_close(audio_dec* d) {
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

bool audio_kind_of(const char* path, audio_kind* kind) {
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

audio_dec* audio_dec_open(FS_Archive archive, const char* path, const char** error) {
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
    d->file.buf = (u8*) malloc(AUDIODEC_FILE_BUFFER);
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
u64 audio_dec_read(audio_dec* d, s16* out, u64 frames) {
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

bool audio_dec_seek(audio_dec* d, u64 frame) {
    switch(d->kind) {
        case AUDIO_WAV: return drwav_seek_to_pcm_frame(&d->wav, frame);
        case AUDIO_MP3: return drmp3_seek_to_pcm_frame(&d->mp3, frame);
        case AUDIO_FLAC: return drflac_seek_to_pcm_frame(d->flac, frame);
        case AUDIO_OGG: return ov_pcm_seek(&d->ogg, (ogg_int64_t) frame) == 0;
    }

    return false;
}
