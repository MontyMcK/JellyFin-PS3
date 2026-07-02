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

void video_reset(void) {
    memset(&s_ts, 0, sizeof(s_ts));
    vdec_reset_counters();
    s_au_inflight_max = 0;
    s_timing_ready    = false;
}

void video_reset_demux(void) {
    memset(&s_ts, 0, sizeof(s_ts));
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
