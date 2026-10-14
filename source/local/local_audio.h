#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "local_tags.h"

// -------------------------------------------------------------------------
//  Decoding a music file to the port's format: 48 kHz stereo floats
// -------------------------------------------------------------------------
//  FLAC (audio/flac_dec), MP3 (minimp3) and WAVE (integer 8/16/24/32 bit, float 32/64 bit), any channel
//  count up to 8 (mixed down to two as chan_map does) and any rate resample.h converts, read through
//  the same callback local_tags.h uses.  Seeking is by sample for FLAC and WAVE; for MP3 by the Xing
//  table, else by the average bit rate (a seek lands within a frame or two of the byte it computed, and
//  the position is the one asked for, not measured).
//
//  The pairs la_decode makes are the +-1.0 floats the music engine's ring holds.  A 48 kHz 16-bit file
//  comes out as exactly sample / 32768.  An MP3 with a LAME header comes out gapless: the encoder's delay
//  and padding are cut, so la_gapless_trimmed says the engine need not trim silence at the joins.
//
//  Pure apart from audio/resample.c, audio/chan_map.c, audio/flac_dec.c and minimp3: tests/
//  test_local_audio.cpp compiles it on the host.  One decoder is used from one thread.
// -------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LaDecoder LaDecoder;

// Whether la_open can decode the file the metadata describes (the sample size, the channels and the rate).
bool la_can_decode(const LaMeta *m);

// A decoder for the file (size bytes through rd), positioned at its start.  NULL when it cannot decode
// it or memory runs out.  The callback and its context must outlive the decoder.
LaDecoder *la_open(LaRead rd, void *ctx, uint64_t size, const LaMeta *m);
void       la_close(LaDecoder *d);

// Up to max_pairs frames of 48 kHz stereo, interleaved L R.  Returns the pairs made (> 0), 0 at the end
// of the file, or < 0 when a read failed (the drive was pulled) and everything decoded before it has been
// handed out.  Any max_pairs >= 1 works.
int la_decode(LaDecoder *d, float *lr, int max_pairs);

// Moves to `secs` into the file (clamped to its length).  False when the file cannot be read there.
bool la_seek(LaDecoder *d, uint32_t secs);

// True when the file's own gapless information is applied (see above).
bool la_gapless_trimmed(const LaDecoder *d);

#ifdef __cplusplus
}
#endif
