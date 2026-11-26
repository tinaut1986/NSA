#pragma once

// Background music made at run time (no files): soft chords, a sparse melody of bell notes and a
// little echo, rendered once into a loop that has no seam (what rings past the end is added to the
// start) and played from memory through the DSP. Off by default.
// Or the user's own songs (issue #63): MP3, WAV, FLAC or Ogg files in BGMUSIC_SONGS_DIR, streamed from the SD card.

typedef enum {
    BGMUSIC_OFF,
    BGMUSIC_LIFT,   // Cmaj7, Am7, Fmaj7, G6: the classic lift.
    BGMUSIC_CALM,   // Slower and sparser, in F.
    BGMUSIC_SHOP,   // Soft bossa: marimba, electric piano, bass and a light beat, like a shop channel.
    BGMUSIC_SONGS,  // The user's songs: one (bgmusic_set_song) over and over, or all of them in a random order.
    BGMUSIC_COUNT
} bgmusic_style;

void bgmusic_set(bgmusic_style style, u32 volumePercent);

// The user's songs.
#define BGMUSIC_SONGS_DIR "/nsa/music/"
#define BGMUSIC_SONG_NAME_MAX 128
// Reads BGMUSIC_SONGS_DIR again (the files it can play, sorted by name); returns how many. From the UI thread.
u32 bgmusic_scan_songs();
u32 bgmusic_song_count();
// The file name of a song of the last scan ("" past the end).
void bgmusic_song_name(u32 index, char* out, size_t size);
// Which song BGMUSIC_SONGS plays: a file name in BGMUSIC_SONGS_DIR, or "" for all of them, shuffled.
void bgmusic_set_song(const char* name);
// While held the music fades out and stops (an FTP transfer on an Old 3DS goes faster without it); released,
// the last song comes back from the SD card. The setting is left as it is. From the UI thread.
void bgmusic_hold(bool held);
// From the UI loop, once a frame: starts, stops and ducks the music.
void bgmusic_update();
void bgmusic_exit();

// What the music is doing, for the animation: a song is being made (progress 0..100) and/or one is playing.
typedef struct {
    bool generating;
    u32 progress;
    bool playing;
    char song[BGMUSIC_SONG_NAME_MAX];   // The user's song playing, or "".
    bool noSongs;                       // BGMUSIC_SONGS, and nothing it can play (no files, or the one chosen is gone).
} bgmusic_status;

void bgmusic_get_status(bgmusic_status* out);
