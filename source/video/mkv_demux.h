#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  Matroska demuxer: tracks, the seek index, and frames in file order
// -------------------------------------------------------------------------
//  Pure C++ over a read-at callback (no PS3 headers), so tests/test_mkv_demux.cpp
//  compiles the same file the console runs.  It knows the EBML structure, not
//  what a codec's bytes mean; mkv_ts.cpp turns the frames into the MPEG-TS the
//  existing player consumes.
//
//  Covered: EBML header, Segment (known or unknown size), SeekHead, Info
//  (TimestampScale, Duration), Tracks (the fields the player needs, header-
//  stripping compression), Cues (the video track's, thinned to a bounded
//  array), Clusters of known or unknown size, SimpleBlock and BlockGroup (a
//  frame is a key frame when its group has no ReferenceBlock), and all three
//  lacing modes.  Encrypted or otherwise compressed tracks are marked
//  unsupported, never decoded wrongly.
//
//  Memory is bounded: a sliding read window, one block buffer the caller
//  supplies, at most MKV_MAX_CUES cue points.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// Reads up to len bytes at off.  Returns the count (0 at the end of the file) or < 0 on an error.
typedef int (*MkvReadAt)(void *ctx, uint64_t off, uint8_t *buf, int len);

#define MKV_MAX_TRACKS   24
#define MKV_MAX_CUES     8192
#define MKV_CP_MAX       2048          // CodecPrivate kept per track (avcC is ~50 bytes)
#define MKV_STRIP_MAX    16            // header-stripped bytes kept per track
#define MKV_MAX_LACE     256

enum { MKV_TRACK_VIDEO = 1, MKV_TRACK_AUDIO = 2, MKV_TRACK_SUBTITLE = 17 };

typedef struct {
    int      number;                   // TrackNumber: what blocks name
    int      type;                     // MKV_TRACK_*
    char     codec_id[40];             // "V_MPEG4/ISO/AVC", "A_AC3", "S_TEXT/UTF8" ...
    char     language[8];              // "eng", "en"; "und" when absent
    char     name[96];
    bool     enabled, is_default, is_forced;
    uint64_t default_duration_ns;      // per frame; 0 = unknown
    uint8_t  codec_private[MKV_CP_MAX];
    int      cp_len;                   // bytes kept; cp_total says how many there were
    int      cp_total;
    int      width, height;            // video: PixelWidth/Height
    double   sample_rate;              // audio
    int      channels, bit_depth;
    uint8_t  strip[MKV_STRIP_MAX];     // header-stripping compression: bytes put back before every frame
    int      strip_len;
    bool     unsupported_encoding;     // encrypted, or a compression other than header stripping
} MkvTrack;

typedef struct {
    uint64_t time_ns;
    uint64_t cluster_pos;              // offset of the cluster, from the Segment's data start
} MkvCue;

typedef struct {
    MkvReadAt rd;
    void     *ctx;
    uint64_t  file_size;

    uint64_t  seg_data;                // file offset of the Segment's data
    uint64_t  seg_end;                 // file offset where it ends (the file's end when its size is unknown)
    uint64_t  timescale_ns;            // nanoseconds per timestamp tick
    double    duration_ns;             // 0 = not stated and not estimated
    uint64_t  first_cluster;           // file offset of the first Cluster element
    bool      is_webm;

    MkvTrack  tracks[MKV_MAX_TRACKS];
    int       n_tracks;

    int       cue_track;               // the video track the cues index (0 = none)
    MkvCue   *cues;                    // sorted by time
    int       n_cues, cap_cues;

    // sliding read window
    uint8_t  *win;
    uint64_t  win_off;
    int       win_len;

    char      err[64];                 // why mkv_open failed
} MkvFile;

// Opens a Matroska/WebM file: parses everything up to the first Cluster (and the Cues and any Tracks or
// Info that the SeekHead puts after it).  0 on success, -1 on failure with f->err set.
int  mkv_open(MkvFile *f, MkvReadAt rd, void *ctx, uint64_t file_size);
void mkv_close(MkvFile *f);

const MkvTrack *mkv_track(const MkvFile *f, int number);
// The first enabled track of a type, or NULL.
const MkvTrack *mkv_first_track(const MkvFile *f, int type);

// ---- codec classes (what the player can do with a track) ----------------------------------
typedef enum {
    MKV_VC_OTHER = 0, MKV_VC_AVC, MKV_VC_HEVC, MKV_VC_VC1, MKV_VC_MPEG2, MKV_VC_MPEG4, MKV_VC_VP9, MKV_VC_AV1
} MkvVideoCodec;
typedef enum {
    MKV_AC_OTHER = 0, MKV_AC_AC3, MKV_AC_EAC3, MKV_AC_DTS, MKV_AC_TRUEHD, MKV_AC_MP3, MKV_AC_MP2,
    MKV_AC_AAC, MKV_AC_FLAC, MKV_AC_PCM, MKV_AC_VORBIS, MKV_AC_OPUS
} MkvAudioCodec;
typedef enum {
    MKV_SC_OTHER = 0, MKV_SC_SRT, MKV_SC_ASS, MKV_SC_PGS, MKV_SC_VOBSUB
} MkvSubCodec;

MkvVideoCodec mkv_video_codec(const MkvTrack *t);
MkvAudioCodec mkv_audio_codec(const MkvTrack *t);
MkvSubCodec   mkv_sub_codec(const MkvTrack *t);

// ---- the AVC decoder configuration (CodecPrivate of V_MPEG4/ISO/AVC) ---------------------
typedef struct {
    int      nal_length_size;          // 1..4
    uint8_t  sps[4][256];  int sps_len[4];  int n_sps;
    uint8_t  pps[4][256];  int pps_len[4];  int n_pps;
} MkvAvcConfig;
// False when the CodecPrivate is not a well-formed avcC.
bool mkv_parse_avcc(const uint8_t *cp, int len, MkvAvcConfig *out);

// ---- frames ----------------------------------------------------------------------------
typedef struct {
    int           track;               // TrackNumber
    int64_t       pts_ns;              // may be negative (pre-roll); tick * timescale
    uint64_t      duration_ns;         // 0 = unknown
    bool          key;
    bool          discardable;
    const uint8_t *data;               // valid until the next mkv_next_frame
    uint32_t      size;
    const uint8_t *prefix;             // header-stripping bytes to put in front (NULL when none)
    int           prefix_len;
} MkvFrame;

typedef struct {
    MkvFile  *f;
    uint8_t  *buf;                     // one block's payload (caller-supplied)
    uint32_t  cap;
    uint64_t  pos;                     // file offset of the next element
    uint64_t  cluster_end;             // UINT64_MAX while the cluster's size is unknown
    int64_t   cluster_tc;              // the cluster's Timecode, in ticks
    bool      in_cluster;
    bool      eof;
    // the block being unpacked
    int       lace_n, lace_i;
    uint32_t  lace_size[MKV_MAX_LACE];
    uint32_t  lace_off;
    int       blk_track;
    int64_t   blk_pts_ns;
    uint64_t  blk_dur_ns;
    bool      blk_key, blk_discardable;
    // counters
    uint32_t  oversize;                // blocks skipped because they did not fit the buffer
    uint32_t  damaged;                 // elements skipped as unreadable
} MkvReader;

void mkv_reader_init(MkvReader *r, MkvFile *f, uint8_t *buf, uint32_t cap);
// Starts reading at the Cluster whose element begins at `cluster_pos` (from the Segment's data start).
bool mkv_reader_at_cluster(MkvReader *r, uint64_t cluster_pos);
// Next frame: 1 with *out filled, 0 at the end, -1 on a read error.
int  mkv_next_frame(MkvReader *r, MkvFrame *out);

// ---- seeking ---------------------------------------------------------------------------
// Positions the reader at the cluster to start from for `target_ns`: the last cue at or before it, or
// (no cues) the last cluster found by a bounded search.  *cluster_time_ns is that cue's time or the
// cluster's timecode: the earliest a key frame there can be.  True on success.
bool mkv_seek(MkvFile *f, MkvReader *r, uint64_t target_ns, uint64_t *cluster_time_ns);

#ifdef __cplusplus
}
#endif
