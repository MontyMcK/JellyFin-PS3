#pragma once
#include <stdint.h>

// -------------------------------------------------------------------------
//  Local MPEG-TS files -- index and seek for offline playback
// -------------------------------------------------------------------------
//  Online, a seek asks the server for a new transcode at StartTimeTicks and
//  the stream starts clean at a keyframe with PTS ~0.  A downloaded media.ts
//  is one continuous stream, so a seek has to find its own way in:
//
//    * where:  a byte offset whose video PTS is at (or just before) the
//              target -- interpolation search over the file, a handful of
//              small reads, no index file;
//    * how:    at a keyframe (random_access_indicator) and at the PAT that
//              ffmpeg's mpegts muxer writes right before every keyframe.
//              The player resets its demuxer on every seek, so without that
//              PAT/PMT the keyframe itself would be dropped and the picture
//              would come back only at the next one.
//
//  The clock is kept honest the same way as online: the entry's PTS minus the
//  file's first PTS is the absolute position the player's play_base_us
//  anchors to (its avsync re-latches on the first frame after a seek).
//
//  Pure, over a read-at callback, so tests/test_offline.cpp drives it on
//  real TS bytes.  PTS parsing is dl_ts's (offline/dl_ts.h), not a copy.
// -------------------------------------------------------------------------

// Reads up to len bytes at offset.  Returns bytes read (0 at EOF), -1 error.
typedef int (*StreamReadAtFn)(void *ctx, uint64_t offset, uint8_t *buf, int len);

typedef struct {
    uint64_t size;            // file bytes
    uint64_t first_pts;       // 90 kHz, at the first keyframe
    uint64_t end_pts;         // highest video PTS near the end
    uint32_t duration_secs;   // (end - first), from the file itself
    bool     has_rai;         // keyframes are flagged (ffmpeg: always)
    bool     ok;
} StreamLocalIndex;

// How far a keyframe search reads forward before settling for any video
// PES start.  Blu-ray copies have ~1 s GOPs; x264's default is up to 10 s,
// which at 25 Mbps is ~31 MB.
#define STREAM_LOCAL_SCAN_MAX (48u * 1024u * 1024u)
// Smallest scratch buffer the functions below accept (they return false
// below it): the search looks back 16 packets from where it starts, and a
// window must cover more than that to make progress.  The player passes
// the stream's 256 KB buffer.
#define STREAM_LOCAL_MIN_SCRATCH (64u * 188u)

// The clock of a transport stream entered at entry_us (what stream_local_entry / stream_local_seek
// return).  The audio clock is the PES PTS as the file wrote it, and the HUD's position is
// play_base_us plus that clock, so a stream entered part way would count the entry time twice.
// *origin_us is the PTS to take off every picture and sound so the stream's time starts at the
// entry -- lead_us before it, because a picture whose PTS is 0 reads as "no PTS"; *play_base_us is
// the absolute position of stream time 0.  Stream time t then reads as position play_base + t, and
// the entry's key frame is at t = lead_us (less where the entry is nearer the start than that).
// A PTS that wraps (33 bits, 26.5 hours) in the middle of the file is not followed.
void stream_local_clock(const StreamLocalIndex *idx, uint64_t entry_us, uint64_t lead_us,
                        uint64_t *origin_us, uint64_t *play_base_us);

// Measures the file: first keyframe PTS and the last video PTS.  scratch is
// the caller's buffer (the stream's own socket buffer: nothing is allocated).
bool stream_local_index(StreamReadAtFn rd, void *ctx, uint64_t size,
                        uint8_t *scratch, int scratch_len, StreamLocalIndex *out);

// The entry point at or after `from`: offset of the PAT preceding the first
// keyframe found (or of the keyframe itself / any video PES start as
// fallbacks), and that frame's position in microseconds from the file start.
bool stream_local_entry(StreamReadAtFn rd, void *ctx, const StreamLocalIndex *idx,
                        uint64_t from, uint8_t *scratch, int scratch_len,
                        uint64_t *entry_off, uint64_t *entry_us);

// Seek: the entry whose position is closest to target_us without passing
// it by more than half a second.  target 0 (or before the first frame) is
// the file start.  Returns false only if nothing in the file can be entered.
bool stream_local_seek(StreamReadAtFn rd, void *ctx, const StreamLocalIndex *idx,
                       uint64_t target_us, uint8_t *scratch, int scratch_len,
                       uint64_t *entry_off, uint64_t *entry_us);
