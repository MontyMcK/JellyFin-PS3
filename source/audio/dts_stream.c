// DTS elementary-stream reader.  See dts_stream.h.

#include "dts_stream.h"
#include "dts_map.h"
#include "dts_exss.h"

#include <string.h>

// Smallest window that lets both sync tests run safely: dca_syncinfo() looks
// at 6 bytes before deciding, then reads a ~13-byte frame header, and an
// extension substream header needs DTS_EXSS_MIN_HEADER (10).
#define DTS_MIN_SCAN 16

void dts_stream_init(dts_stream_t *s, dca_state_t *state, int out_ch,
                     dts_emit_fn emit, void *user)
{
    memset(s, 0, sizeof(*s));
    s->state  = state;
    s->out_ch = (out_ch == 6) ? 6 : 2;
    // DCA_ADJUST_LEVEL lets libdca scale a downmix into range instead of
    // clipping (dca_downmix.c:96-185 adjusts *level per conversion).
    s->req_flags = (s->out_ch == 6) ? (DCA_3F2R | DCA_LFE | DCA_ADJUST_LEVEL)
                                    : (DCA_STEREO | DCA_ADJUST_LEVEL);
    s->emit = emit;
    s->user = user;
}

void dts_stream_reset(dts_stream_t *s)
{
    s->carry_len = 0;
    s->skip      = 0;
}

// Decode one complete core frame at carry[off].
static void decode_core_frame(dts_stream_t *s, int off, int fsize,
                              int stream_flags, int srate, int brate, int flen)
{
    int     f     = s->req_flags;
    level_t level = 1.0f;
    if (dca_frame(s->state, s->carry + off, &f, &level, 0.0f)) {
        s->bad_frames++;
        return;
    }
    // NB: no dca_dynrng() call here, deliberately.  The AC-3 path calls
    // a52_dynrng(state, NULL, NULL) to switch dynamic-range compression off,
    // and the obvious analogue is wrong: libdca's dca_dynrng() zeroes
    // state->dynrange (dca_parse.c:1289), which is not an "apply DRC" switch
    // but the BITSTREAM FLAG the subframe parser reads at dca_parse.c:708 to
    // decide whether an 8-bit dynamic-range coefficient is present in the
    // stream.  Clearing it desyncs the bit reader on any stream that carries
    // one.  libdca never applies dynrange_coef to the samples anyway (it is
    // parsed and dropped), so there is nothing to disable.

    // A DTS core frame carries a variable number of 256-sample blocks
    // (dca_blocks_num = sample_blocks/8; 2 blocks for the common 512-sample
    // frame, up to 16).  One block is exactly one PS3 DMA block.
    int nblocks = dca_blocks_num(s->state);
    if (nblocks < 1 || nblocks > 16) {
        s->bad_frames++;
        return;
    }
    if (!s->have_first) {
        s->have_first    = true;
        s->first_srate   = srate;
        s->first_brate   = brate;
        s->first_amode   = stream_flags & DCA_CHANNEL_MASK;
        s->first_lfe     = (stream_flags & DCA_LFE) ? 1 : 0;
        s->first_granted = f;
        s->first_fsize   = fsize;
        s->first_flen    = flen;
        s->first_blocks  = nblocks;
    }
    for (int blk = 0; blk < nblocks; blk++) {
        if (dca_block(s->state)) {
            s->bad_frames++;
            break;
        }
        dts_map_block(dca_samples(s->state), f, s->stage, s->out_ch);
        if (s->emit) s->emit(s->user, s->stage, 256);
    }
    s->core_frames++;
}

// Consume everything decodable in the carry buffer; keep the tail.
static void decode_carry(dts_stream_t *s)
{
    int off = 0;
    while (s->carry_len - off >= DTS_MIN_SCAN) {
        const uint8_t *p = s->carry + off;

        // ---- DTS-HD / DTS:X extension substream: locate and skip ----
        if (p[0] == 0x64 && p[1] == 0x58 && p[2] == 0x20 && p[3] == 0x25) {
            uint32_t ext_size = 0;
            if (!dts_exss_size(p, s->carry_len - off, &ext_size)) {
                off++;                       // false sync
                continue;
            }
            s->ext_count++;
            s->ext_bytes += ext_size;
            uint32_t avail = (uint32_t)(s->carry_len - off);
            if (ext_size <= avail) {
                off += (int)ext_size;
            } else {
                // Skip the remainder across the following feeds without ever
                // buffering it.
                s->skip = ext_size - avail;
                off     = s->carry_len;
            }
            continue;
        }

        // ---- Core substream ----
        int stream_flags = 0, srate = 0, brate = 0, flen = 0;
        int fsize = dca_syncinfo(s->state, s->carry + off, &stream_flags,
                                 &srate, &brate, &flen);
        if (fsize <= 0) { off++; continue; }         // not a core sync here
        if (s->carry_len - off < fsize) break;       // incomplete — wait
        decode_core_frame(s, off, fsize, stream_flags, srate, brate, flen);
        off += fsize;
    }
    if (off > 0) {
        memmove(s->carry, s->carry + off, (size_t)(s->carry_len - off));
        s->carry_len -= off;
    }
}

void dts_stream_feed(dts_stream_t *s, const uint8_t *es, int len)
{
    if (!s->state || len <= 0) return;
    while (len > 0) {
        // Finish any extension-substream skip first — those bytes never
        // reach the carry buffer.
        if (s->skip > 0) {
            uint32_t take = (s->skip < (uint32_t)len) ? s->skip : (uint32_t)len;
            es      += take;
            len     -= (int)take;
            s->skip -= take;
            continue;
        }
        int space = DTS_CARRY_BYTES - s->carry_len;
        if (space == 0) {
            // Should not happen (decode_carry drains every complete frame);
            // a full carry with no decodable frame means garbage — resync.
            s->carry_len = 0;
            s->bad_frames++;
            space = DTS_CARRY_BYTES;
        }
        int take = (len < space) ? len : space;
        memcpy(s->carry + s->carry_len, es, (size_t)take);
        s->carry_len += take;
        es  += take;
        len -= take;
        decode_carry(s);
    }
}
