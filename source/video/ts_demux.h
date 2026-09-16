#pragma once
#include <ppu-types.h>
#include "video.h"   // TS_PACKET_SIZE

// MPEG-TS demuxer: PAT/PMT discovery plus PES reassembly for one H.264
// video stream and one audio stream (MPEG Layer 1/2/3, AC-3 or DTS).

#define TS_VPES_BUF_SIZE (512 * 1024)
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
