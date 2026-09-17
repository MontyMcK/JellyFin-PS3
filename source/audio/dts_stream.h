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
// Packet mode (see dts_packet_fn) needs the WHOLE frame resident -- core plus
// its extension substreams -- not just the core, so the carry is sized for
// that instead. DTS-HD MA peaks around 24.5 Mbps, which at a 512-sample frame
// is roughly 32 KB; 64 KB leaves headroom without being extravagant.
#define DTS_CARRY_BYTES 65536

// Called with n interleaved frames of out_ch floats, in PS3 channel order.
typedef void (*dts_emit_fn)(void *user, const float *frames, int n);

// Called with one COMPLETE DTS packet: a core frame plus every extension
// substream that follows it, or a lone extension substream on a core-less
// DTS-HD MA track.  Setting this switches the reader into packet mode, where
// it does no libdca decoding at all and hands whole frames to a caller that
// has its own decoder -- which is how the lossless path works, since the XLL
// extension libdca skips is exactly the part that carries the lossless audio.
typedef void (*dts_packet_fn)(void *user, const uint8_t *pkt, int len);

typedef struct {
    dca_state_t *state;        // caller-owned libdca state
    int          out_ch;       // 6 (5.1) or 2
    int          req_flags;    // DCA_* request passed to dca_frame()
    dts_emit_fn  emit;
    dts_packet_fn packet;      // non-NULL => packet mode, emit unused
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

// Switch to packet mode.  Call after dts_stream_init().  `state` is still
// required: nothing DECODES here, but the frame walker still calls
// dca_syncinfo() to find core frames and measure them.
void dts_stream_set_packet_mode(dts_stream_t *s, dts_packet_fn packet,
                                void *user);

// Drop the partial-frame carry and any pending skip (seek/flush).
void dts_stream_reset(dts_stream_t *s);

// Feed elementary-stream bytes.  Any amount, any boundary.
void dts_stream_feed(dts_stream_t *s, const uint8_t *es, int len);
