#pragma once
// Audio-only playback engine for the Music tab.  Streams each queued track
// as 48 kHz MP3 from Jellyfin's audio transcode endpoint, decodes it with
// minimp3 into its own PCM ring, and feeds the shared audio port through
// audio_set_source().  One instance, two threads (stream+decode, DMA pump);
// the Now Playing screen drives it through the command API below.

#include <ppu-types.h>

#define MUSIC_QUEUE_MAX 100

typedef struct {
    char id[64];
    char name[128];
    char artist[64];
    char art_id[64];      // AlbumId when known, else the track id — the
                          // Now Playing art follows this per track
    u32  duration_secs;   // from RunTimeTicks (0 if unknown)
    int  track_num;       // IndexNumber, 0 if unknown
} MusicTrack;

// Fetch an album's audio tracks in disc/track order.  Returns the count.
int music_fetch_album_tracks(const char *album_id, MusicTrack *out, int max);

// Fetch a playlist's tracks in the playlist's own order.
int music_fetch_playlist_tracks(const char *playlist_id,
                                MusicTrack *out, int max);

// Start playing tracks[start_idx..] — opens the audio port and spawns the
// engine threads.  The array is copied.  Returns false if already running
// or the queue is empty.
bool music_start(const MusicTrack *tracks, int count, int start_idx);

// Stop playback and tear everything down (joins threads, closes the audio
// port, restores the video pipeline's PCM source).  Safe to call twice.
void music_stop(void);

// ---- controls (UI thread, non-blocking) ----
void music_toggle_pause(void);
void music_next(void);
void music_prev(void);              // restarts the track when >3 s in
void music_seek(int delta_secs);    // relative seek within the track
void music_jump(int queue_pos);     // jump to a PLAY-ORDER position

// Shuffle rewrites the play order in place (current track keeps playing);
// the queue overlay / Up Next read the live order via music_track_at().
void music_set_shuffle(bool on);
bool music_is_shuffle(void);

// ---- state (UI thread) ----
bool        music_is_active(void);      // false once the queue finishes
bool        music_is_paused(void);
int         music_current_index(void);  // ORIGINAL index of playing track
int         music_current_pos(void);    // position in the play order
int         music_track_at(int pos);    // play order -> original index (-1 OOB)
u32         music_elapsed_secs(void);
u32         music_duration_secs(void);  // current track (server-reported)
const char *music_source_info(void);    // "320 kbps FLAC" ("" until known)
