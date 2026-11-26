#pragma once

// Sound files decoded for playing (the audio player, the background music): MP3, WAV, FLAC and Ogg Vorbis, read
// from the SD card through a buffer, as 16-bit frames.

#include <tremor/ivorbisfile.h>

#include "../libs/dr_libs/dr_flac.h"
#include "../libs/dr_libs/dr_mp3.h"
#include "../libs/dr_libs/dr_wav.h"

#define AUDIODEC_FILE_BUFFER (64 * 1024)

// The file, read through a buffer (a decoder asks for a few KB at a time).
typedef struct {
    Handle handle;
    u64 size;
    u64 pos;
    u8* buf;
    u64 bufStart;
    u32 bufLen;
} audio_file;

u32 audio_file_read(audio_file* f, void* out, u32 n);
// origin: 0 from the start, 1 from here, 2 from the end.
bool audio_file_seek(audio_file* f, s64 offset, int origin);

// Decoders, behind one interface.
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

// The kind of a file by its name; false when it is not one of them.
bool audio_kind_of(const char* path, audio_kind* kind);
// NULL and a translated `error` when it cannot be played (mono or stereo, 8-96 kHz).
audio_dec* audio_dec_open(FS_Archive archive, const char* path, const char** error);
void audio_dec_close(audio_dec* d);
// Frames of 16-bit sound into `out`; fewer than asked at the end.
u64 audio_dec_read(audio_dec* d, s16* out, u64 frames);
bool audio_dec_seek(audio_dec* d, u64 frame);
