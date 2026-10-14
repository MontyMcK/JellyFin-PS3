#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  What a local video file is: container, picture, tracks
// -------------------------------------------------------------------------
//  Pure (no PS3 headers) over a read-at callback, so tests/test_local_probe.cpp
//  compiles the same file the console runs.  The Media browser's details page
//  and the player's start-up both read a LocalInfo; the rules for which track to
//  start with live here too.
//
//  MKV is read with video/mkv_demux; MPEG-TS and Blu-ray .m2ts (192-byte packets:
//  a 4-byte header before each 188) by walking the PAT, the PMT and the first
//  video access units.  Nothing here decodes anything.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// Reads up to len bytes at off: the count (0 at the end), or < 0.
typedef int (*LocalReadAt)(void *ctx, uint64_t off, uint8_t *buf, int len);

typedef enum { LM_NONE = 0, LM_TS, LM_M2TS, LM_MKV } LocalContainer;

typedef enum {
    LA_OTHER = 0, LA_AC3, LA_EAC3, LA_DTS, LA_DTS_HD, LA_TRUEHD, LA_MP3, LA_MP2, LA_AAC, LA_FLAC, LA_PCM, LA_VORBIS, LA_OPUS
} LocalAudioCodec;

typedef enum { LS_OTHER = 0, LS_SRT, LS_ASS, LS_PGS } LocalSubKind;

#define LOCAL_MAX_TRACKS 8

typedef struct {
    int      id;              // the MKV track number, or the TS PID
    char     lang[8];         // as the file says it ("eng")
    char     label[72];       // "English - Dolby Digital - 5.1 - Default", the server's DisplayTitle form
    LocalAudioCodec codec;
    bool     decodable;       // the player has a decoder for it
    bool     is_default;
    bool     commentary;      // the name says so
    int      channels;        // 0 = not known
} LocalAudio;

typedef struct {
    int      id;
    char     lang[8];
    char     label[72];
    LocalSubKind kind;
    bool     forced, is_default;
    bool     usable;          // kind the player can draw (SRT, ASS, PGS)
} LocalSub;

typedef struct {
    LocalContainer container;
    uint64_t size;
    // the picture
    bool     video_ok;        // the console can play it
    char     video_reason[48]; // when not: "HEVC", "10-bit", "4K", ... shown in the file row
    char     video_desc[48];  // "H.264 High 1920x1080 23.976 fps"
    int      width, height;
    double   fps;             // 0 = not known
    int      video_id;        // MKV track number / TS PID
    uint32_t duration_secs;   // 0 = not known
    LocalAudio audio[LOCAL_MAX_TRACKS]; int n_audio;
    LocalSub   subs[LOCAL_MAX_TRACKS];  int n_subs;
} LocalInfo;

// Probes the file.  `name` decides the container together with the first bytes (".mkv" with an EBML
// header, ".m2ts"/".mts" and ".ts" by where the 0x47 sync bytes fall).  False with `err` for a file that
// is none of them or cannot be read.  A file that is one of them but cannot be played (HEVC, ...) is a
// success with video_ok false.
bool local_probe(LocalReadAt rd, void *ctx, uint64_t size, const char *name, LocalInfo *out, char *err, int err_cap);

// How many bytes of s (at most max) a "%.*s" may copy without cutting a UTF-8 sequence in half.
int local_clip_utf8(const char *s, int max);

// ---- track choice -----------------------------------------------------------------------------

typedef struct {
    bool surround;            // Audio Output is 5.1/7.1 (not stereo)
    bool passthrough;         // Audio Output is Dolby Digital: the receiver decodes AC-3
} LocalAudioPrefs;

// The audio track to start with, or -1 when none can be decoded.  Best decodable first (TrueHD, DTS-HD
// MA, DTS, Dolby Digital, MPEG audio); with Dolby Digital output a Dolby Digital track outranks them
// (it goes to the receiver untouched, the rest are decoded to PCM); commentary tracks only when nothing
// else decodes; ties go to the file's default flag, then the first.
int local_pick_audio(const LocalInfo *info, const LocalAudioPrefs *prefs);

// The subtitle track to start with: a forced, usable one in the chosen audio track's language, else -1.
int local_pick_sub(const LocalInfo *info, int audio_index);

// Whether the player can play the file: a picture the console can decode and an audio track it can
// decode (the audio is the player's clock, so a file with none cannot play silent).  When not, `why`
// says so in a sentence fit for the screen.  `why` may be NULL.
bool local_can_play(const LocalInfo *info, const LocalAudioPrefs *prefs, char *why, int why_cap);

// ---- Blu-ray .m2ts: 192-byte source packets viewed as 188-byte TS -----------------------------------
// A read-at over an .m2ts file that returns the stream with each packet's 4-byte header removed, so
// offsets are 188-aligned TS offsets.  ctx must point to a LocalM2tsView.
typedef struct { LocalReadAt rd; void *ctx; } LocalM2tsView;
int  local_m2ts_read_at(void *view, uint64_t off, uint8_t *buf, int len);
uint64_t local_m2ts_ts_size(uint64_t file_size);          // the stripped stream's length
// Strips the 4-byte header of every whole 192-byte packet of buf[0..len) in place, so the 188-byte
// packets follow each other.  Returns the byte count of the stripped stream (a trailing partial packet is dropped).
int  local_m2ts_compact(uint8_t *buf, int len);

#ifdef __cplusplus
}
#endif
