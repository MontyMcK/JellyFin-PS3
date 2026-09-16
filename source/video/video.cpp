#include "video.h"
#include "video_internal.h"
#include "ts_demux.h"
#include "adec.h"
#include "plog.h"

#include <stdio.h>
#include <string.h>

// -------------------------------------------------------
// Per-session glue: TS demux (ts_demux.cpp) → H.264 AUs to the decoder
// (vdec.cpp) / MP3 PES to the audio decoder → decoded frames into the
// jitter buffer (jbuf.cpp).
// -------------------------------------------------------

static TSState s_ts;
static u8      s_pes_out[TS_VPES_BUF_SIZE];
static u8      s_audio_pes_out[TS_APES_BUF_SIZE];
// Last PMT audio codec pushed into adec_set_codec() — TS_AUDIO_NONE until
// the PMT selects a stream.  Reset with the demux so a seek's PMT re-parse
// re-applies it (a no-op in adec when nothing changed).
static u8      s_codec_applied = TS_AUDIO_NONE;

void video_reset(void) {
    memset(&s_ts, 0, sizeof(s_ts));
    s_codec_applied = TS_AUDIO_NONE;
    vdec_reset_counters();
    s_au_inflight_max = 0;
    s_timing_ready    = false;
}

void video_reset_demux(void) {
    memset(&s_ts, 0, sizeof(s_ts));
    s_codec_applied = TS_AUDIO_NONE;
}

bool video_feed_ts(const u8 *pkt) {
    int vlen = 0, alen = 0;
    int ready = ts_process(&s_ts, pkt,
                           s_pes_out,       &vlen,
                           s_audio_pes_out, &alen);
    if (ready & 1) {
        const u8 *h264; int h264_len; u64 pts;
        if (pes_payload(s_pes_out, vlen, &h264, &h264_len, &pts))
            vdec_submit(h264, h264_len, pts);
    }
    // Route the PES queue to the decoder the PMT selected, BEFORE the first
    // audio PES is pushed.  The selection is runtime data, not a compile
    // flag: a server that refuses DTS or AC-3 and sends MP3 lands here with
    // TS_AUDIO_MP3 and plays stereo exactly as shipped.
    if (s_ts.audio_pid && s_codec_applied != s_ts.audio_codec) {
        adec_codec_t want = ADEC_CODEC_MP3;
        if (s_ts.audio_codec == TS_AUDIO_AC3)         want = ADEC_CODEC_AC3;
        else if (s_ts.audio_codec == TS_AUDIO_DTS)    want = ADEC_CODEC_DTS;
        else if (s_ts.audio_codec == TS_AUDIO_TRUEHD) want = ADEC_CODEC_TRUEHD;
        adec_set_codec(want);
        s_codec_applied = s_ts.audio_codec;
    }

    if (ready & 2) {
        static bool s_logged_pes = false;
        if (!s_logged_pes && alen >= 4) {
            s_logged_pes = true;
            char buf[64];
            snprintf(buf, sizeof(buf), "adec_pes: bytes=%02x %02x %02x %02x len=%d",
                s_audio_pes_out[0], s_audio_pes_out[1],
                s_audio_pes_out[2], s_audio_pes_out[3], alen);
            plog(buf);
        }
        adec_push_pes(s_audio_pes_out, alen);
    }

    if (s_frames_ready > 0)
        return vdec_pull_frame();
    return false;
}
