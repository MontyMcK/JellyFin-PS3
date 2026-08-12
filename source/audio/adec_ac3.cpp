// AC-3 decode path (liba52) — see adec_ac3.h for the module contract.

#include "adec_ac3.h"
#include "adec.h"
#include "ac3_map.h"
#include "plog.h"

#include <stdio.h>
#include <string.h>

extern "C" {
#include <inttypes.h>   /* a52.h uses uint*_t but does not include this itself */
#include "a52/a52.h"
}

// Partial-frame carry across PES payloads.  The largest AC-3 syncframe is
// 3840 bytes (38.4 kbit per frame at 640 kbps/48 kHz); 8 KB holds a full
// frame plus a partial successor with room to spare.  Static — audio path,
// no malloc (liba52's one-time a52_init() allocation is the documented
// exception, see docs/surround-5.1.md).
#define AC3_CARRY_BYTES 8192

static a52_state_t *s_state     = NULL;
static int          s_out_ch    = 0;
static int          s_req_flags = 0;
static u8           s_carry[AC3_CARRY_BYTES];
static int          s_carry_len = 0;
static bool         s_logged_frame = false;
static u32          s_bad_frames   = 0;

bool adec_ac3_open(int out_channels) {
    adec_ac3_close();
    s_state = a52_init(0);   // no ASM accel: plain C on the PPU
    if (!s_state) {
        plog("adec_ac3: a52_init failed");
        return false;
    }
    s_out_ch = (out_channels == 6) ? 6 : 2;
    // A52_ADJUST_LEVEL lets liba52 scale a downmix into [-1,1] instead of
    // clipping (downmix.c:70-116 adjusts *level per conversion).
    s_req_flags = (s_out_ch == 6) ? (A52_3F2R | A52_LFE | A52_ADJUST_LEVEL)
                                  : (A52_STEREO | A52_ADJUST_LEVEL);
    s_carry_len = 0;
    s_logged_frame = false;
    s_bad_frames   = 0;
    char b[64];
    snprintf(b, sizeof(b), "adec_ac3: open out_ch=%d req=0x%x",
             s_out_ch, s_req_flags);
    plog(b);
    return true;
}

void adec_ac3_close(void) {
    if (s_state) {
        a52_free(s_state);
        s_state = NULL;
    }
    s_carry_len = 0;
}

void adec_ac3_reset(void) {
    s_carry_len = 0;
}

// Decode every complete syncframe in the carry buffer; keep the tail.
static void decode_carry(void) {
    int off = 0;
    while (s_carry_len - off >= 7) {
        if (s_carry[off] != 0x0B || s_carry[off + 1] != 0x77) {
            off++;   // scan to the 0x0B77 syncword
            continue;
        }
        int flags = 0, srate = 0, brate = 0;
        int fl = a52_syncinfo(s_carry + off, &flags, &srate, &brate);
        if (fl <= 0) { off++; continue; }         // false sync
        if (s_carry_len - off < fl) break;        // incomplete frame — wait

        int      f     = s_req_flags;
        sample_t level = 1.0f;
        if (a52_frame(s_state, s_carry + off, &f, &level, 0.0f)) {
            // Corrupt frame: skip the syncword and rescan.
            if ((s_bad_frames++ % 128) == 0) {
                char b[64];
                snprintf(b, sizeof(b), "adec_ac3: bad frame, total=%u",
                         (unsigned)s_bad_frames);
                plog(b);
            }
            off += 2;
            continue;
        }
        // Dynamic range compression OFF (a52_frame re-enables it per frame —
        // parse.c:167; NULL callback disables — parse.c:203-206).
        a52_dynrng(s_state, NULL, NULL);

        if (!s_logged_frame) {
            s_logged_frame = true;
            char b[96];
            snprintf(b, sizeof(b),
                     "adec_ac3: hz=%d acmod=0x%x lfe=%d bitrate=%d granted=0x%x",
                     srate, flags & A52_CHANNEL_MASK,
                     (flags & A52_LFE) ? 1 : 0, brate, f);
            plog(b);
            if (srate != 48000) {
                // The stream URL pins AudioSampleRate=48000; if the server
                // ignored that, the CellAudio port (fixed 48 kHz) will play
                // this pitched.  Loud log instead of silent wrongness.
                snprintf(b, sizeof(b),
                         "adec_ac3: WARNING stream is %d Hz, port is 48000 Hz",
                         srate);
                plog(b);
            }
        }

        // 6 audio blocks of 256 samples per syncframe (1536 samples).
        for (int blk = 0; blk < 6; blk++) {
            if (a52_block(s_state)) {
                if ((s_bad_frames++ % 128) == 0)
                    plog("adec_ac3: a52_block error");
                break;
            }
            static float stage[256 * 6];   // audio thread only — no reentry
            ac3_map_block(a52_samples(s_state), f, stage, s_out_ch);
            adec_push_frames(stage, 256);
        }
        off += fl;
    }
    if (off > 0) {
        memmove(s_carry, s_carry + off, s_carry_len - off);
        s_carry_len -= off;
    }
}

void adec_ac3_decode_payload(const u8 *es, int len) {
    if (!s_state || len <= 0) return;
    while (len > 0) {
        int space = AC3_CARRY_BYTES - s_carry_len;
        if (space == 0) {
            // Should not happen (decode_carry drains every complete frame);
            // a full carry with no decodable frame means garbage — resync.
            plog("adec_ac3: carry overflow, resync");
            s_carry_len = 0;
            space = AC3_CARRY_BYTES;
        }
        int take = (len < space) ? len : space;
        memcpy(s_carry + s_carry_len, es, take);
        s_carry_len += take;
        es  += take;
        len -= take;
        decode_carry();
    }
}
