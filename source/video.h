#pragma once
#include <ppu-types.h>

#define TS_PACKET_SIZE   188
#define JBUF_SIZE         8   // max buffered decoded frames (~28 MB at 1280×720)
#define JBUF_PREFILL      4   // frames to decode before display starts

// ---- VDEC lifecycle ----
bool vdec_open(void);
void vdec_close(void);

// Reset all video + TS demux state for a new playback session.
void video_reset(void);

// Feed one raw 188-byte TS packet: demux → submit H.264 AU to VDEC →
// try to pull a decoded frame into the jitter buffer.
// Returns true if a frame was added to the jitter buffer.
bool video_feed_ts(const u8 *pkt);

// ---- Jitter buffer ----
bool         jbuf_alloc(u32 fw, u32 fh);
void         jbuf_free(void);
const u8    *jbuf_peek(void);
void         jbuf_pop(void);
u32          jbuf_fw(void);
u32          jbuf_fh(void);
int          jbuf_count(void);

// ---- Observable VDEC state ----
extern volatile bool s_vdec_error;
extern int           s_au_submitted;
extern volatile int  s_au_done;
