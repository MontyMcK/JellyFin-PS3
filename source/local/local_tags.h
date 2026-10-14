#pragma once
#include <stdbool.h>
#include <stdint.h>

// -------------------------------------------------------------------------
//  What a music file says about itself: tags, stream properties, where the audio is
// -------------------------------------------------------------------------
//  FLAC (Vorbis comments, STREAMINFO, the front cover), MP3 (ID3v2.2 / 2.3 / 2.4 and ID3v1, the first
//  frame, a Xing / Info / LAME header's frame count, seek table and gapless delay) and WAVE (the fmt
//  chunk, the data chunk, LIST INFO).  Only the start of the file and, for ID3v1, its last 128 bytes are
//  read, through a callback, so this works on a file of any size on any drive (local/lfs).
//
//  Pure (no PS3 headers): tests/test_local_tags.cpp compiles this file.  Every length read from a file
//  is checked against the file's size and a cap; a damaged file gives a short or empty answer, not a
//  crash.  Strings come out as UTF-8 with control characters replaced by spaces.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// Reads up to len bytes at off.  Returns the bytes read (fewer than len only at the end of the file),
// 0 at the end, or < 0 on an error.  The same shape as the probe's reader.
typedef int (*LaRead)(void *ctx, uint64_t off, uint8_t *buf, int len);

typedef enum { LAF_NONE = 0, LAF_FLAC, LAF_MP3, LAF_WAV } LaKind;

// By file name extension, case-insensitively (.flac .mp3 .wav).
LaKind la_kind_of(const char *name);

typedef struct {
    LaKind   kind;
    char     title[128];
    char     artist[96];
    char     album[128];
    int      track_no;            // 0 = not tagged
    int      disc_no;             // 0 = not tagged
    uint32_t duration_secs;       // 0 = unknown
    int      sample_rate, channels, bits;
    int      bitrate_kbps;        // MP3: the average when known; 0 otherwise

    // where the decoder starts and what it decodes
    uint64_t stream_off;          // FLAC: the "fLaC" marker
    uint64_t data_off;            // FLAC: the first frame | MP3: the first audio frame | WAVE: the first sample
    uint64_t data_len;            // bytes of audio from data_off (MP3: without an ID3v1 tag at the end)
    uint64_t total_frames;        // samples in each channel; 0 = unknown
    int      flac_min_block, flac_max_block;   // FLAC: STREAMINFO's block sizes
    int      wav_format;          // WAVE: 1 = integer PCM, 3 = IEEE float
    int      block_align;         // WAVE: bytes in a frame of all channels
    int      mp3_spf;             // MP3: samples per frame (1152, or 576 for MPEG 2 / 2.5)
    int      mp3_skip;            // MP3: samples to drop from the start of the decoder's output (0 = no gapless info)
    bool     mp3_gapless;         // the file carries the encoder's delay and padding: mp3_skip and total_frames are exact
    bool     mp3_has_toc;         // MP3: toc[] holds a Xing seek table
    uint8_t  mp3_toc[100];

    // the front cover, as bytes of the file
    uint64_t pic_off;
    uint32_t pic_len;             // 0 = none
    char     pic_mime[24];        // "image/jpeg", "image/png", or "" when the tag does not say
} LaMeta;

// Fills m for a file of `size` bytes of the given kind.  False when the file is not one (no STREAMINFO,
// no frame, no "fmt " chunk) or its format is outside what la_open decodes (WAVE other than 8-32 bit
// integer or 32/64 bit float PCM, more than 8 channels).
bool la_read_meta(LaRead rd, void *ctx, uint64_t size, LaKind kind, LaMeta *m);

// The line that says what the file is: "FLAC 44.1 kHz / 16-bit", "WAV 48 kHz / 24-bit", "192 kbps MP3" ("MP3" when the
// bit rate is unknown).  The browser shows it under a track and the player on its meta line.
void la_format_line(const LaMeta *m, char *out, int cap);

#ifdef __cplusplus
}
#endif
