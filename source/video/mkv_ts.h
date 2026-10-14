#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "mkv_demux.h"

// -------------------------------------------------------------------------
//  Matroska as MPEG-TS: what the player's decode path already reads
// -------------------------------------------------------------------------
//  The player's whole pipeline (stream_read -> ts_process -> VDEC and the audio
//  decoders) speaks MPEG-TS.  Rather than teach it a second container, a Matroska
//  file is presented as the TS an ffmpeg muxer would write for it:
//
//    PAT and PMT before the first picture and before every key frame (a seek
//    resets the demuxer, so the stream can be entered at any key frame);
//    video: one PES per access unit, PTS only, H.264 in Annex B (the length prefixes
//    become start codes, an access-unit delimiter leads, and SPS and PPS from the avcC
//    come before every key frame);
//    audio: one PES per frame, the codec's bytes as stored (AC-3, DTS and TrueHD
//    frames are identical in both containers), the stream type the demuxer knows it by.
//
//  Time starts at the first key frame: stream PTS 0 (plus MKV_TS_PTS_BASE_90K) is
//  that frame, the way the server's transcode restarts at a seek, so the audio
//  clock and play_base_us mean what they always mean.  The caller gets the key
//  frame's position in the file back to put in play_base_us.
//
//  One video and at most one audio track are written; picking the audio track is the
//  caller's, and a track change is a reopen at the current position, as on the server path.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// The first frame's PTS, in 90 kHz ticks.  Not zero: a picture with PTS 0 reads as "no PTS".
#define MKV_TS_PTS_BASE_90K  3600
// The same, in microseconds, for the caller's play_base_us.
#define MKV_TS_PTS_BASE_US   40000

// Audio the player has a decoder for (mkv_ts_open refuses a track whose codec is not one of these).
bool mkv_ts_audio_supported(MkvAudioCodec c);
// Whether this track can be carried: its codec, and what that codec needs of the track (an AAC config the ADTS
// header can be made from, a FLAC STREAMINFO, a PCM layout and a sample rate the resampler converts).  When not,
// `reason` says why in a few words ("unsupported AAC setup", "no decoder for this audio", ...).
bool mkv_ts_audio_track_supported(const MkvTrack *t, char *reason, int reason_cap);
// The video can be carried: H.264 with 4-byte NAL lengths... or any length size 1..4, avcC well-formed.
bool mkv_ts_video_supported(const MkvFile *f, int video_track, char *reason, int reason_cap);

typedef struct MkvTs MkvTs;

// Opens a TS view of the file from the key frame at or before `start_ns` (0 = the start).  `audio_track`
// 0 = no audio.  *actual_start_ns is the key frame's time in the file.  NULL on failure, with the reason in
// `err` (may be NULL).  The one frame buffer is allocated on first use and kept for the session.
MkvTs *mkv_ts_open(MkvFile *f, int video_track, int audio_track, uint64_t start_ns,
                   uint64_t *actual_start_ns, char *err, int err_cap);
// The same, and the blocks of one subtitle track (`sub_track`, 0 = none) go to `sink` as the stream is read: every block
// the reader passes, in file order, from the cluster the entry's key frame is in (blocks before the key frame included, since
// a cue can span it).  The frame's data is valid only during the call.  The sink runs on the thread that reads the stream.
typedef void (*MkvSubSink)(void *ctx, const MkvFrame *frame);
MkvTs *mkv_ts_open_subs(MkvFile *f, int video_track, int audio_track, int sub_track, MkvSubSink sink, void *sink_ctx,
                        uint64_t start_ns, uint64_t *actual_start_ns, char *err, int err_cap);
// Writes up to n bytes of whole 188-byte packets.  Returns the byte count, 0 at the end of the file,
// -1 on a read error.
int  mkv_ts_read(MkvTs *t, uint8_t *out, int n);
void mkv_ts_close(MkvTs *t);
// Frees the shared frame buffer (the end of a playback session).
void mkv_ts_release(void);

typedef struct { uint32_t video_frames, audio_frames, dropped_oversize, dropped_damaged; } MkvTsStats;
void mkv_ts_stats(const MkvTs *t, MkvTsStats *out);

#ifdef __cplusplus
}
#endif
