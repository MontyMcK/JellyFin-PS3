#pragma once
#include <ppu-types.h>
#include <sys/mutex.h>

#define TS_PACKET_SIZE   188
#define JBUF_SIZE        16   // max buffered decoded frames (~56 MB at 1280×720)
#define JBUF_PREFILL     12   // frames to decode before display starts

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
int          jbuf_rd(void);

// ---- Jitter buffer mutex (guards s_jb_n; lock before jbuf_peek/pop) ----
extern sys_mutex_t s_jbuf_mtx;

// ---- Observable VDEC state ----
extern volatile bool s_vdec_error;
extern int           s_au_submitted;
extern volatile int  s_au_done;
extern volatile int  s_frames_ready;

// Set true once fps detection completes and timing_init has been called
// with the detected rate; display loop must not pop frames until then.
extern volatile bool s_timing_ready;

// Pull one decoded frame into the jitter buffer; returns false if none available.
bool vdec_pull_frame(void);
