#pragma once
#include <ppu-types.h>
#include "jellyfin_api.h"   // JFMediaSource, JFTracks, url_encode_query
#include "vquality.h"

// -------------------------------------------------------------------------
//  Stream request -- the ONE place a Jellyfin stream.ts request is decided
// -------------------------------------------------------------------------
//  Playback (build_stream_url, player_session.cpp) and offline downloads
//  (dl_request.cpp) both call this.  Neither has a codec/quality engine of
//  its own, so a download cannot pick a different video codec, quality,
//  audio track, container or copy/transcode mode than pressing Play with the
//  same selection would.
//
//  Three steps, all pure (no globals, no I/O) so tests/test_offline.cpp
//  compiles this exact file on the host:
//
//    StreamPrefs      the live settings (quality ladder, 1080p toggle,
//                     surround mode, display size) -- snapshotted by
//                     stream_prefs_current() in player_session.cpp
//    StreamSelection  what is being played: item, version, tracks, and the
//                     chosen audio/subtitle positions
//         |
//         v  stream_request_resolve()
//    StreamRequest    the decision: frame ceiling, profile/level, video
//                     bitrate ceiling (0 = direct copy), audio codec or HD
//                     stream copy, stream indices, source/live ids
//         |
//         v  stream_url_build()   + server, device id, and the two fields
//    URL                          that DO differ: PlaySessionId, StartTimeTicks
//
//  Output format: MPEG-TS (stream.ts), H.264 video, always -- it is what
//  video/ts_demux.cpp consumes.
// -------------------------------------------------------------------------

typedef struct {
    vquality_t quality;       // vquality_get() (already the per-title value
                              // when the info screen restored it)
    bool       hd1080;        // hd1080_enabled()
    bool       surround;      // surround_enabled()
    bool       surround_hd;   // surround_hd_preferred()
    u32        display_w;     // output size, clamps the sub-1080p steps
    u32        display_h;
    bool       budget;        // a quality step is the whole stream, audio and
                              // TS framing included (stream_budget.h)
    bool       passthrough;   // Dolby Digital output: the receiver decodes, so
                              // the stream carries AC-3 and never an HD copy
    bool       no_video_copy; // Settings > Direct Stream = Off: always re-encode
                              // the video (directstream.h)
} StreamPrefs;

// Snapshot of the live settings.  Console only (player_session.cpp): the
// player and the offline downloader both take the same snapshot.
void stream_prefs_current(StreamPrefs *out);

typedef struct {
    const char          *item_id;
    const JFMediaSource *source;   // chosen version; NULL/"" id = item id
    const JFTracks      *tracks;   // that version's tracks (may be NULL)
    int                  cur_audio;   // position in tracks->audio, -1 = default
    int                  cur_sub;     // position in tracks->subs,  -1 = off
    bool                 sub_on_console;   // cur_sub is drawn by this app (text
                                           // or PGS): no server burn-in
    bool                 live;        // a Live TV channel: item_id is the channel
} StreamSelection;

// A live channel has no source to copy from and no length: the video is
// always transcoded, and "Original" quality means this ceiling.
#define STREAM_LIVE_VIDEO_BPS 10000000u

typedef struct {
    // video
    u32         max_w, max_h;      // frame ceiling (the jitter-buffer size)
    const char *profile;           // "baseline" / "high"
    const char *level;             // "31" / "42"
    unsigned    vbitrate;          // quality step in bit/s; 0 = direct copy
    unsigned    vreq;              // VideoBitrate asked for: the step less the
                                   // audio and framing when budgeted; 0 = none
    bool        budgeted;          // vreq came from stream_video_budget()
    unsigned    audio_cost;        // what the budget charged for the audio
    unsigned    audio_reported;    // the track bitrate that charge started from
    // audio
    const char *acodec;            // transcode target: "mp3" / "ac3"
    unsigned    abitrate;
    int         achans;
    const char *hd_codec;          // "dts" / "truehd" when stream-copied, else NULL
    bool        ac3_copy;          // passthrough on a Dolby Digital track: the
                                   // track is copied, with no AudioBitrate
    bool        live;              // a Live TV channel (see StreamSelection)
    bool        no_video_copy;     // StreamPrefs.no_video_copy
    // selection, as Jellyfin indices / ids
    int         audio_idx;         // MediaStream Index, -1 = server default
    int         sub_idx;           // -1 = none; else the track to burn in
    char        item_id[64];
    char        source_id[96];     // MediaSourceId (item id when none chosen)
// Jellyfin names a live stream "<tuner>_<provider>_<channel>", three 32-digit ids:
// 98 characters.  Every copy of it needs room for all of them (a 96-byte copy
// loses the last three and the server answers the stream request with a 500).
    char        live_stream_id[128];
} StreamRequest;

// The player's rule for the tracks a title starts with: the source's
// default audio track, subtitles off.  Downloads use the same rule.
void stream_select_initial(const JFTracks *tracks, bool have_tracks,
                           int *cur_audio, int *cur_sub);

void stream_request_resolve(const StreamPrefs *prefs,
                            const StreamSelection *sel, StreamRequest *out);

// play_session_id NULL/"" = no PlaySessionId parameter (downloads, and
// playback when PlaybackInfo failed).  Returns the URL length (as snprintf).
int stream_url_build(char *url, int url_sz, const StreamRequest *rq,
                     const char *server, const char *device_id,
                     const char *play_session_id,
                     unsigned long long start_ticks);
