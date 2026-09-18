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
    // The truncation counters live in ts_demux.cpp, not in TSState, so the
    // memset above does not reach them.  Zero them per playback session so
    // "pes=" in the heartbeat means this stream, not everything since boot.
    ts_pes_stats_reset();
    s_codec_applied = TS_AUDIO_NONE;
    vdec_reset_counters();
    s_au_inflight_max = 0;
    s_timing_ready    = false;
}

void video_reset_demux(void) {
    memset(&s_ts, 0, sizeof(s_ts));
    s_codec_applied = TS_AUDIO_NONE;
}

// Apply the PMT's audio codec choice and push any completed audio PES.
// Shared by the full feed and the audio-only one below.
static void video_route_audio(int ready, int alen) {
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
}

// Feed ONLY the audio and PSI packets of a TS stream; tell the caller when a
// packet was not one of those and so was left untouched.
//
// This exists for the decode thread's hold-back path (player_threads.cpp):
// when the jitter buffer is full it must stop handing video to VDEC, but the
// audio in the same stream still needs to flow or the PCM ring runs dry.
// Nothing here can reach VDEC or the jitter buffer — no vdec_submit, no
// vdec_pull_frame — so a full jitter buffer cannot be made worse by calling
// it.  Reordering is safe because ts_process keeps one reassembly state per
// PID: audio read ahead of the video packets around it still reassembles
// correctly, and each stream keeps its own PTS.
bool video_feed_ts_audio_only(const u8 *pkt) {
    if (pkt[0] != 0x47) return false;          // TS sync byte
    u16 pid = ((u16)(pkt[1] & 0x1F) << 8) | pkt[2];
    bool psi = (pid == 0x0000) || (s_ts.pmt_pid && pid == s_ts.pmt_pid);
    bool aud = (s_ts.audio_pid && pid == s_ts.audio_pid);
    if (!psi && !aud) return false;            // video (or an unknown PID)

    int vlen = 0, alen = 0;
    int ready = ts_process(&s_ts, pkt,
                           s_pes_out,       &vlen,
                           s_audio_pes_out, &alen);
    // ready&1 cannot be set: a video PES only completes on a video PID, and
    // those never reach here.
    video_route_audio(ready, alen);
    return true;
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

    video_route_audio(ready, alen);

    if (s_frames_ready > 0)
        return vdec_pull_frame();
    return false;
}
