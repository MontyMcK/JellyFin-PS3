// TrueHD/MLP elementary-stream reader.  See truehd_stream.h.

#include "truehd_stream.h"

#include <string.h>

// ---- Access unit framing (derived from ffmpeg's libavcodec/mlp_parser.c) ----
//
//   * Every access unit starts with a 4-byte header whose first 12 bits after
//     the check nibble are the unit length in 16-bit WORDS:
//         length_bytes = ((buf[0] << 8 | buf[1]) & 0x0FFF) * 2
//     (mlp_parser.c: `mp->bytes_left = (... & 0xfff) * 2`).
//   * A unit that carries a major sync has the sync word at offset 4:
//         (AV_RB32(buf + 4) & 0xFFFFFFFE) == 0xF8726FBA
//     (mlp_parser.c: `sync_present = ... (AV_RB32(buf + 4) & 0xfffffffe) ==
//     0xf8726fba`).  The masked low bit is what distinguishes MLP from
//     TrueHD, and both decode through the same path here.
//
// Joining a stream mid-flight (which is what a seek does) therefore means
// hunting for that sync word at offset 4 of a candidate unit, and from then
// on chaining unit to unit by length.  A decode error drops sync and starts
// hunting again rather than walking further into misaligned data.

#define MLP_SYNC_MASK 0xFFFFFFFEu
#define MLP_SYNC_WORD 0xF8726FBAu

// Consecutive failed units before the reader stops trusting its framing.
#define TRUEHD_MAX_BAD_RUN 4

static uint32_t rb32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | (uint32_t)p[3];
}

bool truehd_stream_init(truehd_stream_t *s, void *dec_mem, int out_ch,
                        truehd_emit_fn emit, void *user)
{
    memset(s, 0, sizeof(*s));
    s->out_ch = (out_ch == 8) ? 8 : (out_ch == 2) ? 2 : 6;
    s->emit   = emit;
    s->user   = user;
    // The decoder writes into the reader's own block, so it must be opened
    // AFTER the memset above.
    s->dec = mlp_api_open(dec_mem, s->pcm);
    return s->dec != NULL;
}

void truehd_stream_reset(truehd_stream_t *s)
{
    s->carry_len = 0;
    s->synced    = false;
}

// True if a major sync sits at offset 4 of the unit starting at p.
static bool has_major_sync(const uint8_t *p, int avail)
{
    return avail >= 8 && (rb32(p + 4) & MLP_SYNC_MASK) == MLP_SYNC_WORD;
}

static void decode_unit(truehd_stream_t *s, const uint8_t *unit, int len,
                        int *bad_run)
{
    int      channels = 0, rate = 0;
    uint64_t mask     = 0;
    int n = mlp_api_decode(s->dec, unit, len, &channels, &rate, &mask);
    if (n < 0) {
        s->bad_units++;
        (*bad_run)++;
        return;
    }
    *bad_run = 0;
    s->units++;
    if (n == 0) return;                       // unit carried no samples

    if (!s->have_first) {
        s->have_first     = true;
        s->first_channels = channels;
        s->first_rate     = rate;
        s->first_mask     = mask;
    }
    if (mask != s->map_mask || channels != s->map_nb_ch) {
        truehd_map_build(mask, channels, s->out_ch, &s->map);
        s->map_mask  = mask;
        s->map_nb_ch = channels;
    }
    if (n > MLP_API_MAX_SAMPLES) n = MLP_API_MAX_SAMPLES;   // defensive
    truehd_map_block(s->pcm, channels, n, &s->map, s->stage);
    if (s->emit) s->emit(s->user, s->stage, n);
}

// Consume everything decodable in the carry buffer; keep the tail.
static void decode_carry(truehd_stream_t *s)
{
    int off      = 0;
    int bad_run  = 0;
    while (s->carry_len - off >= 8) {
        const uint8_t *p = s->carry + off;

        if (!s->synced) {
            if (!has_major_sync(p, s->carry_len - off)) {
                off++;
                s->resyncs++;
                continue;
            }
            s->synced = true;
        }

        int len = (((int)p[0] << 8 | p[1]) & 0x0FFF) * 2;
        if (len < 8 || len > TRUEHD_CARRY_BYTES) {
            // Not a plausible unit header — the framing is wrong, hunt again.
            s->synced = false;
            off++;
            s->resyncs++;
            continue;
        }
        if (s->carry_len - off < len) break;       // incomplete unit — wait

        decode_unit(s, p, len, &bad_run);
        off += len;
        if (bad_run >= TRUEHD_MAX_BAD_RUN) {
            // Several units in a row failed: the length chain is probably
            // walking through misaligned data, so re-hunt for a major sync.
            s->synced = false;
            bad_run   = 0;
        }
    }
    if (off > 0) {
        memmove(s->carry, s->carry + off, (size_t)(s->carry_len - off));
        s->carry_len -= off;
    }
}

void truehd_stream_feed(truehd_stream_t *s, const uint8_t *es, int len)
{
    if (!s->dec || len <= 0) return;
    s->fed_bytes += (uint32_t)len;
    while (len > 0) {
        int space = TRUEHD_CARRY_BYTES - s->carry_len;
        if (space == 0) {
            // decode_carry drains every complete unit, so a full carry means
            // no framing was found in 16 KB — drop it and hunt again.
            s->carry_len = 0;
            s->synced    = false;
            s->resyncs++;
            space = TRUEHD_CARRY_BYTES;
        }
        int take = (len < space) ? len : space;
        memcpy(s->carry + s->carry_len, es, (size_t)take);
        s->carry_len += take;
        es  += take;
        len -= take;
        decode_carry(s);
    }
}
