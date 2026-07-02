#pragma once
#include <ppu-types.h>
#include "video.h"   // TS_PACKET_SIZE

// MPEG-TS demuxer: PAT/PMT discovery plus PES reassembly for one H.264
// video stream and one MPEG audio (Layer 1/2/3) stream.

#define TS_VPES_BUF_SIZE (512 * 1024)
#define TS_APES_BUF_SIZE (32 * 1024)   // MP3 frames are small; 32 KB is ample

typedef struct {
    u16  pmt_pid;
    u16  video_pid;
    u16  audio_pid;
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
