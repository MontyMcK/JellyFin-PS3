#pragma once
#include <ppu-types.h>
#include "video.h"   // TS_PACKET_SIZE

// MPEG-TS demuxer: PAT/PMT discovery plus PES reassembly for one H.264
// video stream and one audio stream (MPEG Layer 1/2/3, AC-3 or DTS).

// Video PES.  512 KB was not enough and failed SILENTLY: a Blu-ray remux at
// 53 Mbps / 23.976 fps averages ~276 KB per frame, so an I-frame routinely
// ran past the cap and the AU was truncated rather than dropped -- it still
// passed vdec's own size check, so VDEC got a corrupt AU and stopped emitting
// frames entirely.  That is the 30-second freeze in the 05:39 session of
// player_log.txt: PES_TRUNC x20 (the log cap), then jbuf q=0 and 1037
// "audio: decoder stall" lines until the server session was torn down.
//
// Size it from the standard instead of from a guess.  H.264 Level 4.1 (what
// these remuxes are) has MaxFS = 8192 macroblocks and MinCR = 2, so the
// largest legal coded frame is MaxFS * 384 / MinCR = 1,572,864 bytes.  A
// conforming Level 4.1 AU cannot exceed that, so AU_BUF_SIZE is exactly it.
//
// The PES buffer is deliberately one packet LARGER than AU_BUF_SIZE so the
// two failure modes stay distinguishable: an AU over the level ceiling now
// reaches vdec_submit() at its true length and is logged as AU_DROP, instead
// of being quietly clipped to the buffer size and passed off as valid.
#define TS_VPES_AU_MAX   (1536 * 1024)          /* H.264 L4.1 MaxFS*384/MinCR */
#define TS_VPES_BUF_SIZE (TS_VPES_AU_MAX + 64 * 1024)
// Audio PES: MP3 frames are small; AC-3 syncframes are <=3840 bytes and
// ffmpeg's mpegts muxer PES-packs a handful at a time — 32 KB remains ample.
// DTS is the biggest: a stream-copied DTS-HD MA frame is core (<=16 KB) plus
// extension substream, and at ~5 Mbps the muxer emits ~32 KB per PES in the
// worst case.  A PES that does not fit is truncated (PES_TRUNC in the log)
// and the decoder resyncs at the next frame, so size for the worst case:
// 64 KB, +32 KB of static demux state, once.
#define TS_APES_BUF_SIZE (64 * 1024)

// What the PMT said the selected audio stream is (TSState.audio_codec).
#define TS_AUDIO_NONE 0
#define TS_AUDIO_MP3  1
#define TS_AUDIO_AC3  2
#define TS_AUDIO_DTS  3
#define TS_AUDIO_TRUEHD 4

typedef struct {
    u16  pmt_pid;
    u16  video_pid;
    u16  audio_pid;
    u8   audio_codec;   // TS_AUDIO_* — valid once audio_pid != 0
    // Video PES reassembly
    u8   pes_buf[TS_VPES_BUF_SIZE];
    int  pes_len;
    bool pes_started;
    // Audio PES reassembly
    u8   a_pes_buf[TS_APES_BUF_SIZE];
    int  a_pes_len;
    bool a_pes_started;
} TSState;

// PES truncation telemetry.  The PES_TRUNC log line stops after 20 lines, so
// on its own it can say "this happened" but never "how often" or "how much
// bigger the buffer needed to be" -- reading 20 in a log tells you only that
// the cap was reached.  max_want is the true assembled size INCLUDING the
// bytes that did not fit, which is what to size the buffer from.
typedef struct {
    u32 trunc_count;   // PESes clipped to the buffer since playback started
    u32 max_want;      // largest PES seen, counting what was dropped
} PesStats;

void ts_pes_stats(PesStats *video_out, PesStats *audio_out);
void ts_pes_stats_reset(void);

// Feed one raw 188-byte TS packet.
// Returns bitmask: bit 0 = video PES ready (copied to out_vpes/out_vlen),
// bit 1 = audio PES ready (copied to out_apes/out_alen).
int ts_process(TSState *ts, const u8 *pkt,
               u8 *out_vpes, int *out_vlen,
               u8 *out_apes, int *out_alen);

// Strip PES header and return pointer into the elementary-stream payload.
// Sets *pts_out to the 90 kHz PTS when pes[7]&0x80, else VDEC_TS_INVALID.
bool pes_payload(const u8 *pes, int pes_len,
                 const u8 **es, int *es_len, u64 *pts_out);
