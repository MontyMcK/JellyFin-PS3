#pragma once

// DTS elementary-stream reader: finds core substream frames in a byte stream,
// decodes them with libdca, and skips DTS-HD / DTS:X extension substreams.
//
// Pure C, no PS3 headers, output delivered through a callback — so
// tests/test_dts_stream.c can drive the whole state machine on the host with
// awkward feed boundaries and synthetic extension substreams, which is the
// only way to be sure of it: on the console this code is the difference
// between a DTS-HD track playing and a track that resyncs on garbage every
// few frames, and both sound like "something is wrong with the network".
//
// adec_dts.cpp owns the PS3 side: the libdca state, the output port width,
// logging, and pushing frames into the PCM ring.

#include <stdint.h>
#include <stdbool.h>

#include "dca/dca.h"

// Partial-frame carry.  A DTS core frame is at most 16384 bytes (FSIZE is a
// 14-bit field, dca_parse.c:236), so 32 KB holds a full frame plus a partial
// successor.  Extension substreams are never buffered whole — they can reach
// a megabyte and are only ever skipped, so a byte counter carries a skip
// across feeds instead.
#define DTS_CARRY_BYTES 32768

// Called with n interleaved frames of out_ch floats, in PS3 channel order.
typedef void (*dts_emit_fn)(void *user, const float *frames, int n);

typedef struct {
    dca_state_t *state;        // caller-owned libdca state
    int          out_ch;       // 6 (5.1) or 2
    int          req_flags;    // DCA_* request passed to dca_frame()
    dts_emit_fn  emit;
    void        *user;

    uint8_t  carry[DTS_CARRY_BYTES];
    int      carry_len;
    uint32_t skip;             // extension-substream bytes still to discard

    // Counters/observations, for the caller's logs and diagnostics.
    uint32_t core_frames;      // core frames successfully decoded
    uint32_t bad_frames;       // frames libdca rejected, plus block errors
    uint32_t ext_count;        // extension substreams seen
    uint32_t ext_bytes;        // their total size
    bool     have_first;       // first_* below are populated
    int      first_srate, first_brate, first_amode, first_lfe;
    int      first_granted, first_fsize, first_flen, first_blocks;

    float    stage[256 * 6];   // one block, interleaved, before emit()
} dts_stream_t;

// state must already be dca_init()ed and outlive the reader.
// out_ch 6 requests DCA_3F2R|DCA_LFE, anything else requests DCA_STEREO.
void dts_stream_init(dts_stream_t *s, dca_state_t *state, int out_ch,
                     dts_emit_fn emit, void *user);

// Drop the partial-frame carry and any pending skip (seek/flush).
void dts_stream_reset(dts_stream_t *s);

// Feed elementary-stream bytes.  Any amount, any boundary.
void dts_stream_feed(dts_stream_t *s, const uint8_t *es, int len);
