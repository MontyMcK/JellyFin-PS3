#pragma once
#include <ppu-types.h>

// Interfaces shared between the video modules (ts_demux.cpp, vdec.cpp,
// jbuf.cpp, video.cpp).  Nothing here is part of the public video.h API.

// ---- VDEC (vdec.cpp) ----
// Submit one H.264 access unit (annex-B) with its 90 kHz PTS to the decoder.
void vdec_submit(const u8 *data, int len, u64 pts);

// ---- Jitter buffer producer side (jbuf.cpp) ----
// Used only by the decode thread (vdec_pull_frame): fill jbuf_write_ptr(),
// then jbuf_push() to stamp and publish the slot.
bool jbuf_full(void);                    // no free slot (locks s_jbuf_mtx)
u8  *jbuf_write_ptr(void);               // frame buffer of the write slot (allocates one)
int  jbuf_write_idx(void);               // index of the write slot (diagnostics)
void jbuf_set_dims(u32 fw, u32 fh);      // adopt actual stream dimensions
// Stamp + publish the write slot.  `order` is the decoder's display-order key
// (from the H.264 picture order count, see vdec_pull_frame): the slot is held
// until a later-ordered picture arrives, so decode-order output comes out in
// display order.  JBUF_ORDER_NONE publishes in arrival order.
#define JBUF_ORDER_NONE  ((s64)0x8000000000000000LL)
void jbuf_push(u64 pts_us, s64 dur_us, s64 order);
