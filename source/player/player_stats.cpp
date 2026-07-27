// Player stats — live playback instrumentation.  Public surface: player_stats.h.
//
// The whole file is compiled out when ENABLE_PLAYER_STATS is 0; the header
// turns every hook into an inline no-op, so no call site needs an #if.

#include "player_stats.h"

#if ENABLE_PLAYER_STATS

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "adec.h"        // PCM_RING_HIGHWATER
#include "meminfo.h"     // meminfo_get
#include "player_rsx.h"  // rsx_draw_overlay_quad
#include "rsxutil.h"     // display_width / display_height
#include "statsovl.h"    // runtime on/off
#include "timing.h"      // timing_get_us, timing_vblank_period_us, avsync_*
#include "overscan.h"
#include "plog.h"
#include "ui.h"          // cpu_rt_begin/end, drawTTF, drawRect, ttf_text_width
#include "ui_visuals.h"  // XMB_* palette

// =========================================================================
//  Sample window sizing
// =========================================================================
//  The reference implementation this borrows its math from (VshFpsCounter)
//  keeps a session-wide histogram that never resets.  That is right for a
//  5-minute game benchmark and wrong here: our sessions are 2-hour movies.
//  With an unbounded pool a single hitch during startup buffering pins the
//  1% low for the rest of the film, and by the 90-minute mark no amount of
//  real stuttering can move the number — the metric stops reporting on the
//  present and starts reporting on ancient history.
//
//  So the histogram is a SLIDING WINDOW over the last STAT_WINDOW presented
//  frames, kept exact by decrementing the outgoing sample's bin as the new
//  one is added.  Fixed memory, O(1) per frame, no growth over a long movie.
//
//  STAT_WINDOW = 8192 presented frames.  At 24fps that is ~5.7 minutes —
//  long enough that the tails are meaningful (81 samples in the 1% tail, 8
//  in the 0.1% tail) and short enough that the reading still describes the
//  scene you are watching.  Smaller windows starve the 0.1% low: at 4096 it
//  is the 4th-worst frame, which is noise.
//
//  Bin width is 250us, NOT the reference's 1ms.  That project measures games
//  whose frame times sprawl across 16-50ms, where 1ms bins are fine.  Ours
//  sit on two tight spikes (16.68ms vblank, 41.71ms at 24fps), and 1ms bins
//  would quantise the 1% low to +/-0.6fps at 24fps — coarser than the effect
//  being hunted.  256 bins x 250us covers 0-63.75ms; bin 255 is the overflow
//  catch-all for anything slower.
#define STAT_WINDOW   8192
#define STAT_BINS     256
#define STAT_BIN_US   250
#define STAT_OVERFLOW (STAT_BINS - 1)

// Overlay recompose interval.  The numbers do not need to update at 60Hz to
// be readable, and a recompose costs a full text layout pass.
#define STAT_REFRESH_US 250000ULL

// Overlay panel geometry (main-RAM compose + RSX texture, both this size).
// OVL_W must be a multiple of 16: bind_texture() sets pitch = width * 4 and
// RSX linear textures need a 64-byte-aligned pitch.  384*4 = 1536 = 64*24.
// The HUD never has to think about this because it is display-width sized
// (1280/1920, both already aligned); a small panel does.
// Height covers a header + 12 stat lines (last baseline at y=225) plus a
// bottom margin matching the top pad.  Only the WIDTH has an alignment rule.
#define OVL_W 384
#define OVL_H 256

// =========================================================================
//  State
// =========================================================================
// Written by the vblank handler (RSX interrupt context) and read by the
// display thread — volatile so neither side caches them in a register.  The
// handler only ever INCREMENTS these; nothing clears them behind its back,
// so a reader can never observe a torn or rewound value.  64-bit loads and
// stores are single instructions on the Cell PPU, so no tearing there either.
static volatile u64 s_vblanks       = 0;
static volatile u64 s_vb_last_us    = 0;
static volatile u32 s_missed_vsync  = 0;
static volatile u32 s_fifo_samples  = 0;
static volatile u32 s_fifo_busy     = 0;
static u64          s_vb_nominal_us = 16683;   // refreshed on reset

// The RSX control register block.  gcmGetControlRegister() is resolved ONCE
// on the display thread and cached; the vblank handler only dereferences the
// pointer, never calls into libgcm.
static gcmControlRegister *s_ctrl = NULL;

// Display-thread only (presented-frame cadence).
static u64 s_ft_last_us   = 0;
static u64 s_ft_ema_us    = 0;
static u64 s_vb_base      = 0;    // hardware vblank count at session start
static u64 s_vb_at_frame  = 0;    // hardware vblank count at the previous frame
static u32 s_frames_shown = 0;
static u32 s_hold2 = 0, s_hold3 = 0, s_hold_other = 0, s_long_holds = 0;

// Sliding sample window + its histogram.
static u32 s_win_ft_us[STAT_WINDOW];
static u32 s_hist[STAT_BINS];
static int s_win_head  = 0;
static int s_win_count = 0;

// Main memory, sampled on the display thread (see on_frame_shown).
#define RAM_SAMPLE_US 500000ULL
#define RAM_LOW_KB    (20 * 1024)   // below this the free figure turns accent
static u32 s_ram_total_kb = 0;
static u32 s_ram_avail_kb = 0;
static u32 s_ram_min_kb   = 0xFFFFFFFFu;
static u64 s_ram_next_us  = 0;

// Audio thread.
static volatile u32 s_audio_blocks  = 0;
static volatile u32 s_audio_starves = 0;
static volatile u32 s_pcm_avail     = 0;
static volatile u32 s_dma_ahead     = 0;
static volatile u32 s_dma_total     = 0;

// Overlay buffers.
static u32 *s_ovl_stage   = NULL;
static u32 *s_ovl_tex     = NULL;
static u32  s_ovl_tex_off = 0;
static u64  s_ovl_next_us = 0;

static inline int bin_of(u32 ft_us) {
    u32 b = ft_us / STAT_BIN_US;
    return (b >= STAT_OVERFLOW) ? STAT_OVERFLOW : (int)b;
}

// =========================================================================
//  Hooks
// =========================================================================

void player_stats_reset(void) {
    s_vblanks = 0; s_vb_last_us = 0; s_missed_vsync = 0;
    s_fifo_samples = 0; s_fifo_busy = 0;
    s_ft_last_us = 0; s_ft_ema_us = 0;
    s_vb_base = gcmGetVBlankCount();
    s_vb_at_frame = s_vb_base;
    s_frames_shown = 0;
    s_hold2 = s_hold3 = s_hold_other = s_long_holds = 0;
    s_win_head = 0; s_win_count = 0;
    memset(s_hist, 0, sizeof(s_hist));
    s_audio_blocks = s_audio_starves = 0;
    s_pcm_avail = s_dma_ahead = s_dma_total = 0;
    s_ram_total_kb = s_ram_avail_kb = 0;
    s_ram_min_kb = 0xFFFFFFFFu;
    s_ram_next_us = 0;
    s_ovl_next_us = 0;

    s_vb_nominal_us = (u64)timing_vblank_period_us();
    s_ctrl = gcmGetControlRegister();
}

// RSX interrupt context.  Integer only — no floats, no allocation, no
// logging, no libgcm calls.  Everything expensive is deferred to the 250ms
// recompose on the display thread.
void player_stats_on_vblank(void) {
    u64 now  = timing_get_us();
    u64 prev = s_vb_last_us;
    s_vb_last_us = now;
    s_vblanks++;

    if (prev != 0 && s_vb_nominal_us != 0) {
        u64 d = now - prev;
        // A vblank interval at or past 1.5x nominal means the handler did not
        // run on one or more edges — the display pipeline stalled through
        // them.  Round to the nearest whole period and count the ones lost.
        if (d >= s_vb_nominal_us + s_vb_nominal_us / 2) {
            u32 periods = (u32)((d + s_vb_nominal_us / 2) / s_vb_nominal_us);
            if (periods > 1) s_missed_vsync += periods - 1;
        }
    }

    // RSX command-buffer occupancy.  get == put means the GPU has drained
    // everything the CPU queued; get != put means work is still outstanding.
    // Sampling that once per vblank gives the fraction of vblanks that ended
    // with the FIFO non-empty.
    //
    // This is FIFO BACKLOG, not true GPU utilisation.  The RSX exposes no
    // busy counter to a PSL1GHT process — there is no equivalent of a
    // performance-monitor read — so a real "GPU busy %" is not obtainable
    // here.  Reported and labelled as what it actually measures; the overlay
    // prints "fifo", not "gpu".
    {
        gcmControlRegister *c = s_ctrl;
        if (c != NULL) {
            s_fifo_samples++;
            if (c->get != c->put) s_fifo_busy++;
        }
    }
}

// Display thread — called when a decoded frame is actually presented.
void player_stats_on_frame_shown(void) {
    u64 now = timing_get_us();

    // Vblanks this frame was held.  Taken from the HARDWARE counter
    // (gcmGetVBlankCount, safe to call off the display thread) rather than
    // the handler's own tally: if handler dispatch is ever delayed or an edge
    // is skipped, the hardware count is still exact, and the cadence figures
    // below are the whole point of this metric.  Diffed rather than cleared —
    // a vblank landing between a read and a clear would lose a count.
    u64 vc   = gcmGetVBlankCount();
    u32 held = (u32)(vc - s_vb_at_frame);
    s_vb_at_frame = vc;

    if (s_ft_last_us != 0) {
        u32 ft_us = (u32)(now - s_ft_last_us);

        // Sliding window: when full, the head slot holds the oldest sample,
        // so drop it from the histogram before overwriting.
        if (s_win_count == STAT_WINDOW)
            s_hist[bin_of(s_win_ft_us[s_win_head])]--;
        s_win_ft_us[s_win_head] = ft_us;
        s_hist[bin_of(ft_us)]++;
        s_win_head = (s_win_head + 1) & (STAT_WINDOW - 1);
        if (s_win_count < STAT_WINDOW) s_win_count++;

        // Integer EMA, same 9:1 weighting timing.cpp uses for the A/V diff.
        s_ft_ema_us = (s_ft_ema_us == 0) ? ft_us : (s_ft_ema_us * 9 + ft_us) / 10;

        // 2:3 pulldown cadence.  24fps on a 59.94Hz panel must alternate
        // 3,2,3,2,... — anything else is the pulldown failing to hold.
        if      (held == 2) s_hold2++;
        else if (held == 3) s_hold3++;
        else if (held != 0) {
            s_hold_other++;
            if (held > 3) s_long_holds++;
        }
    }

    s_ft_last_us = now;
    s_frames_shown++;

    // Main-memory sample, throttled to ~2Hz.  meminfo_get() is an LV2 syscall
    // (352), so it must NOT go anywhere near the vblank handler; the display
    // thread is fine, and the codebase already calls it from log lines.
    // Sampled HERE rather than in player_stats_get() so the low-water mark
    // keeps tracking while the overlay is switched off — memory pressure on
    // the 1080p path is worth catching whether or not anyone is looking.
    // Note this only covers steady-state playback: no frames present during
    // vdec_open's big transient alloc, so that dip belongs to vdec.cpp's own
    // meminfo log line, not to this figure.
    if (now >= s_ram_next_us) {
        s_ram_next_us = now + RAM_SAMPLE_US;
        u32 total = 0, avail = 0;
        if (meminfo_get(&total, &avail)) {
            s_ram_total_kb = total / 1024;
            s_ram_avail_kb = avail / 1024;
            if (s_ram_avail_kb < s_ram_min_kb) s_ram_min_kb = s_ram_avail_kb;
        }
    }

    // Keep the handler's nominal period current.  timing_init() only learns
    // the real display refresh once fps detection runs, which is AFTER
    // player_stats_reset() — on a 50Hz PAL set the cached 59.94Hz default
    // would otherwise skew the missed-vsync threshold for the whole session.
    // A 64-bit store is a single instruction here, so the handler can never
    // read a half-updated value.
    s_vb_nominal_us = (u64)timing_vblank_period_us();
}

// Audio thread — one call per DMA block written.
void player_stats_on_audio_write(int pcm_avail_pairs, int dma_ahead,
                                 int dma_total, bool starved) {
    s_audio_blocks++;
    if (starved) s_audio_starves++;
    s_pcm_avail = (u32)(pcm_avail_pairs < 0 ? 0 : pcm_avail_pairs);
    s_dma_ahead = (u32)(dma_ahead < 0 ? 0 : dma_ahead);
    s_dma_total = (u32)(dma_total < 0 ? 0 : dma_total);
}

// =========================================================================
//  Aggregation (display thread, once per recompose)
// =========================================================================

void player_stats_get(PlayerStats *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    // Hardware truth for the count; s_vblanks (handler-observed) is only used
    // for the interval timing above.  A gap between the two would itself mean
    // the handler is missing edges.
    { u64 hw = gcmGetVBlankCount(); out->vblanks = (hw > s_vb_base) ? hw - s_vb_base : 0; }
    out->frames_shown = s_frames_shown;
    out->missed_vsync = s_missed_vsync;
    out->hold2        = s_hold2;
    out->hold3        = s_hold3;
    out->hold_other   = s_hold_other;
    out->long_holds   = s_long_holds;

    out->fps_ema  = (s_ft_ema_us > 0) ? 1000000.0f / (float)s_ft_ema_us : 0.0f;
    int count     = s_win_count;
    out->window_samples = (u32)count;

    if (count > 0) {
        // Single pass for mean / min / max / sum-of-squares.  Milliseconds
        // as double keeps the variance well-conditioned.
        double sum = 0.0, sum_sq = 0.0;
        u32 mn = 0xFFFFFFFFu, mx = 0;
        for (int i = 0; i < count; i++) {
            u32 v = s_win_ft_us[i];
            double ms = (double)v / 1000.0;
            sum    += ms;
            sum_sq += ms * ms;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        double mean = sum / (double)count;
        out->ft_mean_ms = (float)mean;
        out->ft_min_ms  = (float)mn / 1000.0f;
        out->ft_max_ms  = (float)mx / 1000.0f;
        out->ft_cur_ms  = (float)s_win_ft_us[(s_win_head - 1 + STAT_WINDOW)
                                             & (STAT_WINDOW - 1)] / 1000.0f;

        double var = (sum_sq / (double)count) - (mean * mean);
        out->ft_stdev_ms = (var > 0.0) ? sqrtf((float)var) : 0.0f;

        // 1% / 0.1% lows: walk the histogram low-to-high until the cumulative
        // count crosses the percentile, giving the frame time that 99% (99.9%)
        // of the window beat.  The bin's UPPER edge is reported, so the fps is
        // the conservative reading.
        //
        // NOTE for 2:3 pulldown content: the presented-interval distribution
        // is bimodal BY DESIGN (33.4ms and 50.1ms alternating at 24fps), and
        // the 50ms frames are 40% of the window.  A healthy 24fps movie
        // therefore reads ~20fps here, and that is correct, not a fault.  The
        // signal is a reading BELOW that baseline; for a straight yes/no on
        // cadence, read hold2/hold3/hold_other instead.
        if (count >= 10) {
            u32 t99  = (u32)(((u64)99  * count + 99)  / 100);
            u32 t999 = (u32)(((u64)999 * count + 999) / 1000);
            u32 cum = 0;
            u32 p99_us = 0, p999_us = 0;
            for (int b = 0; b < STAT_BINS; b++) {
                cum += s_hist[b];
                if (!p99_us  && cum >= t99)  p99_us  = (u32)(b + 1) * STAT_BIN_US;
                if (!p999_us && cum >= t999) p999_us = (u32)(b + 1) * STAT_BIN_US;
                if (p99_us && p999_us) break;
            }
            out->fps_1_low  = p99_us  ? 1000000.0f / (float)p99_us  : 0.0f;
            out->fps_01_low = p999_us ? 1000000.0f / (float)p999_us : 0.0f;
        }
    }

    {
        u32 n = s_fifo_samples;
        out->rsx_fifo_busy_pct =
            n ? (100.0f * (float)s_fifo_busy / (float)n) : -1.0f;
    }

    out->ram_total_kb     = s_ram_total_kb;
    out->ram_avail_kb     = s_ram_avail_kb;
    out->ram_min_avail_kb = (s_ram_min_kb == 0xFFFFFFFFu) ? 0 : s_ram_min_kb;

    out->audio_blocks  = s_audio_blocks;
    out->audio_starves = s_audio_starves;
    out->pcm_ring_pct  = 100.0f * (float)s_pcm_avail / (float)PCM_RING_HIGHWATER;
    if (out->pcm_ring_pct > 999.0f) out->pcm_ring_pct = 999.0f;
    out->dma_ring_pct  = s_dma_total
        ? (100.0f * (float)s_dma_ahead / (float)s_dma_total) : 0.0f;

    // A/V delta is already tracked by timing.cpp's EMA — read it, do not
    // recompute it.  Duplicating that logic would give a second number that
    // drifts from the one actually steering avsync_biased_period().
    out->avsync_us     = avsync_get_smoothed_diff();
    out->avsync_locked = avsync_is_locked();
}

// =========================================================================
//  Overlay
// =========================================================================

void player_stats_overlay_alloc(void) {
    if (s_ovl_stage && s_ovl_tex) return;
    if (!statsovl_enabled()) return;   // never enabled: pay nothing

    u32 bytes = OVL_W * OVL_H * 4;
    if (!s_ovl_stage) s_ovl_stage = (u32*)memalign(128, bytes);
    if (!s_ovl_tex) {
        s_ovl_tex = (u32*)rsxMemalign(64, bytes);
        if (s_ovl_tex) rsxAddressToOffset(s_ovl_tex, &s_ovl_tex_off);
    }
    if (s_ovl_stage) memset(s_ovl_stage, 0, bytes);
    if (s_ovl_tex)   memset((void*)s_ovl_tex, 0, bytes);
    if (!s_ovl_stage || !s_ovl_tex) plog("stats_ovl: alloc FAILED");
    else                            plog("stats_ovl: buffers ready");
}

// Translucent backdrop written straight into the staging buffer.
static void ovl_dim(int rx, int ry, int rw, int rh, u8 alpha) {
    if (rx < 0 || ry < 0 || rx >= OVL_W || ry >= OVL_H) return;
    int x2 = (rx + rw > OVL_W) ? OVL_W : rx + rw;
    int y2 = (ry + rh > OVL_H) ? OVL_H : ry + rh;
    u32 px = (u32)alpha << 24;
    for (int y = ry; y < y2; y++) {
        u32 *row = s_ovl_stage + y * OVL_W;
        for (int x = rx; x < x2; x++) row[x] = px;
    }
}

#define ST_PAD    12
#define ST_LINE   19
#define ST_PX   14.0f
#define ST_HDR  15.0f

// One "label: value" line.  Label in faint ink at the left margin, value in
// the given colour at a fixed column so the numbers stay in a straight edge.
static void stat_line(int y, const char *label, const char *value, u32 color) {
    drawTTF((u32)ST_PAD, (u32)y, label, ST_PX, XMB_TEXT_FAINT);
    drawTTF((u32)(ST_PAD + 62), (u32)y, value, ST_PX, color);
}

static void stats_compose(const PlayerStats *s) {
    cpu_rt_begin(s_ovl_stage, OVL_W, OVL_H);

    // Covers every pixel, so it doubles as the wipe of the previous compose —
    // no separate memset needed (unlike the HUD, which only dirties bands of
    // a display-sized buffer and has to track spans).
    ovl_dim(0, 0, OVL_W, OVL_H, 190);
    drawRect(0, 0, OVL_W, 1, XMB_HAIRLINE);
    drawRect(0, OVL_H - 1, OVL_W, 1, XMB_HAIRLINE);
    drawRect(0, 0, 1, OVL_H, XMB_HAIRLINE);
    drawRect(OVL_W - 1, 0, 1, OVL_H, XMB_HAIRLINE);
    drawRect(0, 0, 3, OVL_H, XMB_ACCENT);

    int y = ST_PAD;
    drawTTF(ST_PAD, (u32)y, "PLAYER STATS", ST_HDR, XMB_WHITE, true);
    y += ST_LINE + 4;

    char v[96];

    // ---- presented-frame cadence ----
    snprintf(v, sizeof(v), "%.2f  (%.2f ms)", (double)s->fps_ema,
             (double)s->ft_cur_ms);
    stat_line(y, "fps", v, XMB_TEXT); y += ST_LINE;

    snprintf(v, sizeof(v), "%.1f / %.1f / %.1f ms", (double)s->ft_min_ms,
             (double)s->ft_mean_ms, (double)s->ft_max_ms);
    stat_line(y, "lo/av/hi", v, XMB_TEXT_DIM); y += ST_LINE;

    snprintf(v, sizeof(v), "%.2f ms   n=%u", (double)s->ft_stdev_ms,
             (unsigned)s->window_samples);
    stat_line(y, "stdev", v, XMB_TEXT_DIM); y += ST_LINE;

    snprintf(v, sizeof(v), "1%%  %.1f    0.1%%  %.1f",
             (double)s->fps_1_low, (double)s->fps_01_low);
    stat_line(y, "lows", v, XMB_TEXT_DIM); y += ST_LINE;

    // ---- pulldown cadence ----
    // hold_other is the honest verdict on the 2:3 gate: nonzero means frames
    // are landing on a vblank count the cadence never asks for.
    snprintf(v, sizeof(v), "2x %u  3x %u  other %u",
             (unsigned)s->hold2, (unsigned)s->hold3, (unsigned)s->hold_other);
    stat_line(y, "pulldown", v,
              s->hold_other ? XMB_ACCENT : XMB_TEXT_DIM); y += ST_LINE;

    snprintf(v, sizeof(v), "%u shown  %u long  %u missed",
             (unsigned)s->frames_shown, (unsigned)s->long_holds,
             (unsigned)s->missed_vsync);
    stat_line(y, "vsync", v,
              (s->missed_vsync || s->long_holds) ? XMB_ACCENT : XMB_TEXT_DIM);
    y += ST_LINE;

    // ---- RSX ----
    if (s->rsx_fifo_busy_pct < 0.0f)
        snprintf(v, sizeof(v), "n/a (no counter)");
    else
        snprintf(v, sizeof(v), "%.0f%% backlog", (double)s->rsx_fifo_busy_pct);
    stat_line(y, "rsx fifo", v, XMB_TEXT_DIM); y += ST_LINE;

    // ---- main memory ----
    // Free rather than used: free is the number that decides whether the next
    // allocation succeeds, and it is how the rest of the codebase talks about
    // this pool (meminfo_avail_kb).  "low" is the session's tightest moment.
    if (s->ram_total_kb)
        snprintf(v, sizeof(v), "%u / %u MB   low %u",
                 (unsigned)(s->ram_avail_kb     / 1024),
                 (unsigned)(s->ram_total_kb     / 1024),
                 (unsigned)(s->ram_min_avail_kb / 1024));
    else
        snprintf(v, sizeof(v), "n/a (syscall failed)");
    stat_line(y, "ram free", v,
              (s->ram_avail_kb && s->ram_avail_kb < RAM_LOW_KB)
                  ? XMB_ACCENT : XMB_TEXT_DIM);
    y += ST_LINE;

    // ---- audio ----
    snprintf(v, sizeof(v), "pcm %.0f%%  dma %.0f%%",
             (double)s->pcm_ring_pct, (double)s->dma_ring_pct);
    stat_line(y, "aud buf", v,
              (s->pcm_ring_pct < 25.0f) ? XMB_ACCENT : XMB_TEXT_DIM);
    y += ST_LINE;

    snprintf(v, sizeof(v), "%u  (of %u blocks)",
             (unsigned)s->audio_starves, (unsigned)s->audio_blocks);
    stat_line(y, "starves", v,
              s->audio_starves ? XMB_ACCENT : XMB_TEXT_DIM); y += ST_LINE;

    // ---- A/V sync ----
    snprintf(v, sizeof(v), "%+.1f ms  %s", (double)s->avsync_us / 1000.0,
             s->avsync_locked ? "LOCK" : "----");
    stat_line(y, "a/v", v, s->avsync_locked ? XMB_TEXT_DIM : XMB_ACCENT);

    cpu_rt_end();

    memcpy((void*)s_ovl_tex, s_ovl_stage, OVL_W * OVL_H * 4);
}

void player_stats_render_overlay(void) {
    // Runtime switch: off means zero rendering cost.  No compose, no text
    // layout, no draw call — the counters above keep running so the panel is
    // already populated the moment it is switched on.
    if (!statsovl_enabled()) return;

    if (!s_ovl_stage || !s_ovl_tex) {
        player_stats_overlay_alloc();          // enabled mid-session
        if (!s_ovl_stage || !s_ovl_tex) return;
    }

    // Recompose at ~4Hz.  The GPU quad below still goes out every frame —
    // it is a few FIFO words, unlike the text layout it presents.
    u64 now = timing_get_us();
    if (now >= s_ovl_next_us) {
        s_ovl_next_us = now + STAT_REFRESH_US;
        PlayerStats s;
        player_stats_get(&s);
        stats_compose(&s);
    }

    // Top-RIGHT, inside the overscan-safe area so a CRT does not eat it.
    // Right rather than left because the HUD puts the paused item title at
    // top-left and the two would overlap.
    int px = (int)display_width - OVL_W - 24 - overscan_x();
    int py = overscan_y() + 24;
    if (px < 0) px = 0;
    rsx_draw_overlay_quad(s_ovl_tex_off, OVL_W, OVL_H, px, py, OVL_W, OVL_H);
}

#endif  // ENABLE_PLAYER_STATS
