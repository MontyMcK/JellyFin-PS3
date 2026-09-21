#pragma once
#include <ppu-types.h>
#include <sys/mutex.h>

#define TS_PACKET_SIZE   188
// ALL video decodes to planar YUV420P (1.5 bpp) buffered in the jitter ring;
// the RSX converts to RGB at display via the BT.709/BT.601 fragment shaders
// (Movian-style).  A 1280×720 slot is 1.32 MB (vs 3.52 MB for the old ARGB
// path), a 1920×1088 slot is 3.13 MB — so the ring holds MORE frames in LESS
// memory than the shipped 12-slot ARGB buffer ever did.
#define JBUF_PREFILL      12   // frames to decode before display starts
// 8 slots, was 10.  These come from the SAME heap the compressed
// read-ahead ring draws from, so every slot removed is ~3.1 MB the ring
// gains -- and a compressed byte buys far more runway than a decoded one.
//
// Safe because the jitter buffer is never the constraint: `q=` in the
// heartbeat sits pinned at its cap in every healthy sample, i.e. the
// decoder always outruns the display and the extra slots were idle.
// Every measured stall had ring=0, not q=0.  8 slots is still a third of
// a second of decode jitter at 24fps, and the display only ever needs two
// (current + next, for the blend).
#define JBUF_1080_SLOTS   8    // ring slots on the 1080p path (~25 MB)
// Was 16 (~50 MB).  Hardware logging showed where that memory is better
// spent: with direct play the console decodes 1080p at a full 24 fps
// whenever the compressed ring has data, and stalls only when it runs
// dry -- and the ring was refilling to 72% before draining again, so
// average delivery MATCHES playback and it is the swings that hurt.
// Compressed bytes buy roughly 75x the runway per byte that decoded
// frames do, so six slots (18 MB of decoded video, 0.25 s) moved into
// the ring is worth several SECONDS of burst tolerance.  Ten slots is
// still comfortably above JBUF_PREFILL.
#define JBUF_SD_SLOTS     24   // ring slots on the 720p path       (~32 MB)
// The static ring arrays are sized for the LARGER of the two active counts;
// jbuf_cap() picks how many are actually used per path.
#define JBUF_MAX_SLOTS    24

// ---- VDEC lifecycle ----
bool vdec_open(void);
void vdec_close(void);
// Boot-time reservations: the VDEC arena and jitter-buffer slots are grabbed
// once at startup (main.cpp) and cached forever — allocated per-session they
// raced the UI for a heap with only a few MB of slack and lost at random.
void vdec_reserve_mem(void);
bool jbuf_reserve(u32 fw, u32 fh);
// Free the cached decoder arena + AU buffers (normally never called; the
// reservation is permanent by design).
void vdec_release_mem(void);

// EndSequence + StartSequence without close/reopen — faster seek flush.
void vdec_flush(void);

// Reset counter variables only (used after vdec_close/vdec_open on seek).
void vdec_reset_counters(void);

// Reset all video + TS demux state for a new playback session.
void video_reset(void);

// Reset only the TS demux state (leaves VDEC and jbuf intact).
void video_reset_demux(void);

// Feed one raw 188-byte TS packet: demux → submit H.264 AU to VDEC →
// try to pull a decoded frame into the jitter buffer.
// Returns true if a frame was added to the jitter buffer.
bool video_feed_ts(const u8 *pkt);

// Feed only the audio/PSI packets of the stream, leaving video untouched.
// Returns false when the packet was a video one (the caller must hold it and
// feed it through video_feed_ts() later, in order).  Never submits to VDEC
// and never touches the jitter buffer, so it is safe to call precisely when
// the jitter buffer is full — which is the point: see the hold-back ring in
// player_threads.cpp.
bool video_feed_ts_audio_only(const u8 *pkt);

// ---- Jitter buffer ----
bool         jbuf_alloc(u32 fw, u32 fh);
int          jbuf_cap(void);            // active ring capacity (<= JBUF_MAX_SLOTS)
int          jbuf_prefill_target(void); // frames to prefill (<= capacity)
// Bytes per decoded planar YUV420P frame (fw * pad16(fh) * 3/2).  All
// slot/texture sizing uses this.
u32          vid_frame_bytes(u32 fw, u32 fh);
void         jbuf_free(void);
void         jbuf_clear(void);     // drain all frames without freeing memory
const u8    *jbuf_peek(void);
void         jbuf_pop(void);
u32          jbuf_fw(void);
u32          jbuf_fh(void);
int          jbuf_count(void);
int          jbuf_rd(void);
u64          jbuf_peek_pts_us(void);  // PTS (us, 0=unknown) of current front slot
u32          jbuf_peek_seq(void);  // decode sequence number of current front slot
const u8    *jbuf_slot_ptr(int i); // raw pointer to slot i's frame buffer

// ---- Jitter buffer duration API ----
s64          jbuf_peek_dur(void);       // remaining duration of front slot (us)
s64          jbuf_peek_next_dur(void);  // natural duration of next slot (us)
const u8    *jbuf_peek_next(void);      // pointer to next slot, or NULL if count<2
void         jbuf_consume_dur(s64 us);  // subtract us from front slot's remaining duration
void         jbuf_advance(void);        // pop front only if its duration <= 0

// ---- Jitter buffer mutex (guards s_jb_n; lock before jbuf_peek/pop) ----
extern sys_mutex_t s_jbuf_mtx;

// ---- Observable VDEC state ----
extern volatile bool s_vdec_error;
extern int           s_au_submitted;
extern volatile int  s_au_done;
extern volatile int  s_frames_ready;

// Peak (sent - AUDONE) seen since last heartbeat; reaching AU_BUF_COUNT (4)
// means an AU buffer was overwritten while VDEC could still be reading it.
// The heartbeat logs and resets it.
extern volatile int  s_au_inflight_max;

// Set true once fps detection completes and timing_init has been called
// with the detected rate; display loop must not pop frames until then.
extern volatile bool s_timing_ready;

// Pull one decoded frame into the jitter buffer; returns false if none available.
bool vdec_pull_frame(void);
