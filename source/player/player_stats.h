#pragma once
#include <ppu-types.h>
#include "../build_config.h"   // relative: source/ is not on the -I path

// =========================================================================
//  Player stats — live playback instrumentation
// =========================================================================
//  Self-contained diagnostics for the video/audio pipeline.  The rest of the
//  codebase only ever calls the small hook surface at the bottom of this
//  header; no instrumentation is scattered through the render or audio code.
//
//  Everything is EVENT-DRIVEN off callbacks the app already has — there is no
//  polling thread:
//
//    player_stats_on_vblank()      <- timing.cpp   s_vblank_handler (RSX vblank)
//    player_stats_on_frame_shown() <- player_display.cpp, when a decoded frame
//                                     is actually presented (the do_pop path)
//    player_stats_on_audio_write() <- audio.cpp    audio_write_pcm (DMA block)
//
//  Two compile/runtime switches, deliberately separate:
//    ENABLE_PLAYER_STATS (build_config.h) — compile-time.  0 turns every hook
//      below into an inline no-op and drops the sample storage entirely.
//    statsovl_enabled()  (statsovl.h)     — runtime.  Controls DISPLAY only;
//      counters keep updating so the overlay is warm when switched on.
//
//  THREADING.  on_vblank runs in RSX interrupt context, on_frame_shown on the
//  display thread, on_audio_write on the audio thread.  The hooks are
//  integer-only, lock-free, and never allocate or log.  All floating-point
//  aggregation (EMA, stdev, percentiles) is deferred to the display thread
//  inside player_stats_render_overlay().  Readers may observe counters that
//  are a sample or two stale; these are diagnostics, not control inputs, and
//  nothing in the playback path reads them back.
// =========================================================================

// Snapshot of every tracked metric.  Filled by player_stats_get().
// Float fields are computed lazily on the display thread, so they only
// advance while the overlay is being rendered (see the header note above).
struct PlayerStats {
    // ---- presented-frame cadence -------------------------------------
    // "Frame" here means a DECODED VIDEO FRAME actually put on screen, not
    // an RSX flip.  At 24fps on a 60Hz panel this ticks 24x/sec while
    // vblanks tick ~60x/sec.
    float fps_ema;          // EMA-smoothed instantaneous fps
    float ft_cur_ms;        // most recent presented-frame interval
    float ft_mean_ms;       // mean over the bounded window
    float ft_min_ms;        // window min
    float ft_max_ms;        // window max
    float ft_stdev_ms;      // window stdev
    float fps_1_low;        // 1%   low fps  (see note on pulldown below)
    float fps_01_low;       // 0.1% low fps
    u32   window_samples;   // samples currently in the window

    // ---- vsync / pulldown health -------------------------------------
    u64   vblanks;          // hardware vblanks since session start
    u32   frames_shown;     // decoded frames presented
    u32   missed_vsync;     // vblank intervals longer than 1.5x nominal
    // 2:3 pulldown cadence — how many vblanks each presented frame was held.
    // Healthy 24fps-on-60Hz alternates 3,2,3,2,... so hold2 ~= hold3 and
    // hold_other stays at 0.  Any growth in hold_other means the pulldown is
    // NOT holding cadence.
    u32   hold2, hold3, hold_other;
    u32   long_holds;       // frames held >3 vblanks (visible judder)

    // ---- RSX ----------------------------------------------------------
    // Command-buffer occupancy sampled from the get/put registers.  This is
    // FIFO backlog, not true GPU utilisation — see the comment in
    // player_stats.cpp.  Negative means unsupported/unavailable.
    float rsx_fifo_busy_pct;

    // ---- main memory ----------------------------------------------------
    // LV2's view of the process's main-memory budget (meminfo.h, syscall
    // 352) — the pool malloc grows into, so it is what decides whether
    // jbuf_alloc / vdec_open can get their buffers.  All three are 0 if the
    // syscall failed.  RSX local memory (VRAM) is deliberately absent:
    // PSL1GHT's rsx/mm.h exposes only rsxMalloc/rsxMemalign/rsxFree with no
    // heap-stat query, so a live VRAM figure is not obtainable — same call as
    // rsx_fifo_busy_pct, report what exists rather than invent a number.
    u32   ram_total_kb;      // process container size
    u32   ram_avail_kb;      // free right now
    u32   ram_min_avail_kb;  // lowest free seen while frames were presenting

    // ---- audio ---------------------------------------------------------
    float pcm_ring_pct;     // decoder PCM ring fill (upstream of the DMA ring)
    float dma_ring_pct;     // hardware DMA ring fill
    u32   audio_blocks;     // DMA blocks written this session
    u32   audio_starves;    // blocks where the decoder had no PCM -> silence

    // ---- A/V sync -------------------------------------------------------
    // Read straight out of timing.cpp's existing EMA; not recomputed here.
    s64   avsync_us;        // smoothed video-minus-audio, microseconds
    bool  avsync_locked;
};

#if ENABLE_PLAYER_STATS

// Reset every counter and the sample window.  Call at playback session start.
void player_stats_reset(void);

// Allocate the overlay's compose buffer + RSX texture.  Call alongside
// hud_overlay_alloc() at session start so the allocation lands low in the
// heap; a no-op when the overlay is disabled or already allocated.
void player_stats_overlay_alloc(void);

// ---- hooks (see threading note above) ----
void player_stats_on_vblank(void);
void player_stats_on_frame_shown(void);
// pcm_avail_pairs: decoder ring occupancy (adec_pcm_available()).
// dma_ahead/dma_total: hardware DMA ring blocks queued ahead of the read
// cursor, and ring size.  starved: the decoder had no PCM and silence was
// written instead — the direct dropout counter for issue #16.
void player_stats_on_audio_write(int pcm_avail_pairs, int dma_ahead,
                                 int dma_total, bool starved);

// Snapshot the current metrics.  Safe to call from the display thread.
void player_stats_get(PlayerStats *out);

// Draw the overlay for this frame.  Returns immediately when the runtime
// toggle is off — no compose, no text layout, no draw call.  Otherwise the
// panel is recomposed at most every ~250ms and blitted as one alpha-blended
// GPU quad.  Call during playback, after hud_draw().
void player_stats_render_overlay(void);

#else   // stats stripped — every hook collapses to nothing

static inline void player_stats_reset(void) {}
static inline void player_stats_overlay_alloc(void) {}
static inline void player_stats_on_vblank(void) {}
static inline void player_stats_on_frame_shown(void) {}
static inline void player_stats_on_audio_write(int, int, int, bool) {}
static inline void player_stats_get(PlayerStats *) {}
static inline void player_stats_render_overlay(void) {}

#endif  // ENABLE_PLAYER_STATS
