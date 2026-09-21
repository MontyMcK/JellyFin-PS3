#pragma once

// TrueHD/MLP elementary-stream reader: finds access units in a byte stream,
// decodes them with the vendored MLP decoder, and hands PS3-ordered float
// frames to a callback.
//
// Pure C, no PS3 headers, output through a callback — so
// tests/test_truehd_stream.c can drive the whole state machine on the host
// with awkward feed boundaries, the same way test_dts_stream.c does.
//
// adec_truehd.cpp owns the PS3 side: decoder lifetime, port width, logging,
// and pushing frames into the PCM ring.

#include <stdint.h>
#include <stdbool.h>

#include "mlp_api.h"
#include "truehd_map.h"

// Largest MLP access unit: the 12-bit length field counts 16-bit words, so
// 4095*2 bytes is the ceiling.  8 KB carries a whole one plus a partial
// successor.
#define TRUEHD_CARRY_BYTES 16384

// Called with n interleaved frames of out_ch floats, in PS3 channel order.
typedef void (*truehd_emit_fn)(void *user, const float *frames, int n);

typedef struct {
    mlp_dec       *dec;          // caller-owned decoder instance
    int            out_ch;       // 8, 6 or 2
    truehd_emit_fn emit;
    void          *user;

    uint8_t  carry[TRUEHD_CARRY_BYTES];
    int      carry_len;
    bool     synced;             // a major sync has been seen

    truehd_map_t map;            // rebuilt whenever the layout changes
    uint64_t     map_mask;       // mask the current map was built for
    int          map_nb_ch;

    // Counters/observations for the caller's logs.
    uint32_t fed_bytes;          // elementary-stream bytes fed in
    uint32_t units;              // access units decoded
    uint32_t bad_units;          // units the decoder rejected
    uint32_t resyncs;            // bytes skipped hunting for a major sync
    bool     have_first;
    int      first_channels, first_rate;
    uint64_t first_mask;

    // One access unit of decoded PCM: interleaved int32 from the decoder,
    // then the PS3-ordered float frames handed to emit().
    int32_t  pcm[MLP_API_MAX_SAMPLES * 8];
    float    stage[MLP_API_MAX_SAMPLES * 8];
} truehd_stream_t;

// Initialise the reader AND the decoder inside it.  dec_mem must be at least
// mlp_api_instance_size() bytes and outlive the reader; the reader points the
// decoder at its own pcm block, so the caller never has to get that ordering
// right.  Returns false if the decoder refused to initialise.
bool truehd_stream_init(truehd_stream_t *s, void *dec_mem, int out_ch,
                        truehd_emit_fn emit, void *user);

// Drop the partial-unit carry and require a new major sync (seek/flush).
void truehd_stream_reset(truehd_stream_t *s);

// Feed elementary-stream bytes.  Any amount, any boundary.
void truehd_stream_feed(truehd_stream_t *s, const uint8_t *es, int len);
