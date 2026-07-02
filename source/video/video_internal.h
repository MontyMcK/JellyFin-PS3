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
u8  *jbuf_write_ptr(void);               // frame buffer of the next write slot
int  jbuf_write_idx(void);               // index of the next write slot (diagnostics)
void jbuf_set_dims(u32 fw, u32 fh);      // adopt actual stream dimensions
void jbuf_push(u64 pts_us, s64 dur_us);  // stamp + publish the write slot
