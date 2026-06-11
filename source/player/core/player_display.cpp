// Per-vblank display step — Movian-style duration-consumption gate, frame
// swap, GPU draw, HUD overlay, and the playback diagnostics.

#include <stdio.h>

#include <ppu-types.h>
#include <sysutil/video.h>

#include "player_internal.h"
#include "audio.h"
#include "video.h"
#include "timing.h"
#include "plog.h"
#include "rsxutil.h"

// A/B test knob: define to disable the temporal crossfade entirely (every
// frame displays as pure-A, Bresenham pulldown only).  Ghosting on flat-color
// content (cartoons) that disappears with this set implicates the blend.

// fps detection timeout — needed only to initialise the Bresenham fallback.
static void check_fps_fallback(PlayerState *ps) {
    if (s_timing_ready) return;
    if (ps->det_timeout_start == 0) ps->det_timeout_start = timing_get_us();
    if (timing_get_us() - ps->det_timeout_start >= 5000000ULL) {
        plog("fps_detect: timeout, fallback 30fps");
        timing_init(30, 1);
        s_timing_ready = true;
    }
}

// Refresh-rate stability diagnostic: poll videoGetState every 300 vblanks.
// Fuck Ninja
static void log_refresh_rate(void) {
    static u64  s_rr_vblank   = 0;
    static u16  s_last_rr     = 0;
    static int  s_rr_changes  = 0;
    static int  s_rr_polls    = 0;
    s_rr_vblank++;
    if (s_rr_vblank % 300 != 0) return;
    videoState vs;
    if (videoGetState(0, 0, &vs) != 0) return;
    u16 rr = vs.displayMode.refreshRates;
    s_rr_polls++;
    if (s_last_rr != 0 && rr != s_last_rr) {
        s_rr_changes++;
        char buf[96];
        snprintf(buf, sizeof(buf),
            "rr_change: poll#%d prev=0x%04x new=0x%04x changes=%d",
            s_rr_polls, (unsigned)s_last_rr,
            (unsigned)rr, s_rr_changes);
        plog(buf);
    }
    s_last_rr = rr;
    if (s_rr_polls % 12 == 0) {
        char buf[80];
        snprintf(buf, sizeof(buf),
            "rr_stats: polls=%d changes=%d cur=0x%04x",
            s_rr_polls, s_rr_changes, (unsigned)rr);
        plog(buf);
    }
}

void player_display_frame(PlayerState *ps) {
    check_fps_fallback(ps);
    log_refresh_rate();

    // ---- Duration-consumption gate logic (Movian-style temporal blend) ----
    s64 vblank_period_us = avsync_biased_period(16683);
    static int  s_pure_count = 0;
    static int  s_mid_count  = 0;
    static s64  s_spill_max  = 0;

    bool  do_pop       = false;
    bool  render_blend = false;
    bool  silent_flip  = false;
    float blend_factor = 0.0f;

    // Log first 30 gate-logic vblanks after timing is ready
    {
        static int s_blend_init_n = 0;
        if (s_blend_init_n < 30 && s_timing_ready) {
            s_blend_init_n++;
            char buf[96];
            snprintf(buf, sizeof(buf),
                "blend_init: tick=%d n=%d dur=%lldus",
                s_blend_init_n, jbuf_count(),
                (long long)jbuf_peek_dur());
            plog(buf);
        }
    }

    if (!ps->paused && s_vid_frame_ready && s_timing_ready && jbuf_count() > 0) {
        // Measurement only — result discarded; EMA updated for logging.
        { u64 vpts = jbuf_peek_pts_us(); (void)avsync_compute_diff(vpts); }

        s64  dur_a = jbuf_peek_dur();
#ifdef VID_DISABLE_BLEND
        bool b_ok  = false;   // force pure-A: no crossfade ever
#else
        bool b_ok  = (jbuf_peek_next() != NULL) && s_vid_b_present;
#endif

        if (!b_ok || dur_a >= vblank_period_us) {
            // Pure-A: consume one vblank period, pop if frame exhausted
            jbuf_consume_dur(vblank_period_us);
            jbuf_advance();
            silent_flip = true;
            s_pure_count++;
        } else {
            // Crossfade: dur_a < vblank, B available — blend A→B
            {
                static u64 s_last_cf_us = 0;
                u64 now_cf = timing_get_us();
                u64 delta_cf = (s_last_cf_us != 0) ? (now_cf - s_last_cf_us) : 0;
                s_last_cf_us = now_cf;
                if (ps->frame_count < 200) {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                        "cf: t=%lluus delta=%lluus blend=%.3f fr=%d dur_a=%lld",
                        (unsigned long long)now_cf,
                        (unsigned long long)delta_cf,
                        (double)((float)dur_a / (float)vblank_period_us),
                        ps->frame_count,
                        (long long)dur_a);
                    plog(buf);
                }
            }
            blend_factor  = (float)dur_a / (float)vblank_period_us;
            s64 spill     = vblank_period_us - dur_a;
            jbuf_consume_dur(dur_a);   // exhaust A
            jbuf_advance();            // pop A (dur now <= 0)
            jbuf_consume_dur(spill);   // consume spill from new front (old B)
            do_pop        = true;
            render_blend  = true;
            s_mid_count++;
            if (spill > s_spill_max) s_spill_max = spill;
        }
    }

    if (do_pop) {
        u32 disp_seq = jbuf_peek_seq();
        ps->frame_count++;
        __asm__ volatile("sync" ::: "memory");
        s_vid_frame_ready = false;
        s_vid_disp_idx ^= 1;
        {
            static u64 s_fi_last_us = 0;
            static u64 s_fi_gaps[2] = {0, 0};
            u64 now_us = timing_get_us();
            if (s_fi_last_us != 0) {
                s_fi_gaps[0] = s_fi_gaps[1];
                s_fi_gaps[1] = now_us - s_fi_last_us;
            }
            s_fi_last_us = now_us;
            if (ps->frame_count % 60 == 0) {
                u64 audio_clk = audio_get_clock_us();
                char buf[128];
                snprintf(buf, sizeof(buf),
                    "frame_interval: fr=%d gap1=%lluus gap2=%lluus audio=%lluus",
                    ps->frame_count,
                    (unsigned long long)s_fi_gaps[0],
                    (unsigned long long)s_fi_gaps[1],
                    (unsigned long long)audio_clk);
                plog(buf);
                char buf2[128];
                snprintf(buf2, sizeof(buf2),
                    "blend_dist: fr=%d pure=%d mid=%d spill_max=%lldus",
                    ps->frame_count, s_pure_count, s_mid_count,
                    (long long)s_spill_max);
                plog(buf2);
                s64  smooth = avsync_get_smoothed_diff();
                bool locked = avsync_is_locked();
                s64  biased = avsync_biased_period(16683);
                char buf3[160];
                snprintf(buf3, sizeof(buf3),
                    "avsync: smooth=%lldus locked=%d audio=%lluus video=%lluus bias=%lldus",
                    (long long)smooth,
                    locked ? 1 : 0,
                    (unsigned long long)audio_get_clock_us(),
                    (unsigned long long)jbuf_peek_pts_us(),
                    (long long)(biased - 16683));
                plog(buf3);
            }
        }
        {
            int window_pos = ps->frame_count % 240;
            if (window_pos >= 1 && window_pos <= 24) {
                char buf[80];
                snprintf(buf, sizeof(buf),
                    "disp_seq: fr=%d seq=%u window=%d",
                    ps->frame_count, disp_seq, ps->frame_count / 240);
                plog(buf);
            }
        }
    } else if (silent_flip && s_vid_frame_ready) {
        __asm__ volatile("sync" ::: "memory");
        s_vid_frame_ready = false;
        s_vid_disp_idx ^= 1;
    } else if (ps->show_seek_frame && ps->paused && s_vid_frame_ready) {
        // Paused seek: display the target frame exactly once, staying paused.
        __asm__ volatile("sync" ::: "memory");
        s_vid_frame_ready = false;
        s_vid_disp_idx ^= 1;
        ps->show_seek_frame = false;
    }
    if (do_pop) ps->show_seek_frame = false;   // playing again; clear any leftover

    // Re-draw every vblank. s_vid_disp_idx advances on do_pop or silent_flip.
    if (ps->frame_count > 0) {
        if (do_pop) rsxSync();
        u32 fw = jbuf_fw(), fh = jbuf_fh();
        vid_gpu_draw(render_blend, blend_factor, fw, fh);
    }

    {
        static int s_vb_log_n = 0;
        static int s_vb_last_fr = -1;
        // Log at most 200 lines, one per DISTINCT frame, after startup.
        // Guard against frame_count freezing (e.g. a post-seek stall) — that
        // would otherwise spam an identical line every loop iteration and
        // thrash the HDD with log writes.
        if (ps->frame_count >= 60 && s_timing_ready &&
            s_vb_log_n < 200 && ps->frame_count != s_vb_last_fr) {
            s_vb_last_fr = ps->frame_count;
            u64 now_us = timing_get_us();
            char buf[160];
            snprintf(buf, sizeof(buf),
                "vb: t=%lluus fr=%d disp=%d dur_a=%lld do_pop=%d render_blend=%d blend=%.3f q=%d",
                (unsigned long long)now_us,
                ps->frame_count,
                s_vid_disp_idx,
                (long long)jbuf_peek_dur(),
                do_pop ? 1 : 0,
                render_blend ? 1 : 0,
                (double)blend_factor,
                jbuf_count());
            plog(buf);
            s_vb_log_n++;
        }
    }

    // Post-seek diagnostic: what position is the HUD actually showing?
    if (ps->seek_dbg_frames > 0) {
        ps->seek_dbg_frames--;
        if (ps->seek_dbg_frames % 15 == 0) {
            u64 clk = audio_get_clock_us();
            char buf[128];
            snprintf(buf, sizeof(buf),
                "seek_dbg: shown=%llus (base=%llus + clk=%llus) jbuf=%d paused=%d",
                (unsigned long long)((ps->play_base_us + clk) / 1000000ULL),
                (unsigned long long)(ps->play_base_us / 1000000ULL),
                (unsigned long long)(clk / 1000000ULL),
                jbuf_count(), (int)ps->paused);
            plog(buf);
        }
    }

    // HUD overlay.
    // The HUD's dim quad reprograms RSX vertex/fragment state.  If the video
    // draw above is still in flight when that happens, the RSX wedges (the dim
    // quad collides with the video path's TEX0 fetch).  When playing, do_pop
    // triggers the rsxSync() before vid_gpu_draw, so the pipeline is already
    // fenced.  When PAUSED, do_pop is always false (the consume gate requires
    // !paused), so that sync is skipped and the HUD would be the first thing
    // to touch RSX after an unfenced vid_gpu_draw -> hard freeze.  Fence
    // unconditionally here so the HUD never races the video commands.
    if (hud_is_visible() && ps->frame_count > 0) {
        static int s_hg = 0;
        if (s_hg < 12) {
            char b[80];
            snprintf(b, sizeof(b), "hud_gate: vis paused=%d fr=%d",
                     (int)ps->paused, ps->frame_count);
            plog(b); s_hg++;
        }
        rsxSync();  // ensure prior vid_gpu_draw is complete before HUD reprograms RSX
        // While a seek is armed, show where the accumulated skip will land so
        // the user can keep tapping toward the right spot before it fires.
        u64 hud_elapsed = ps->play_base_us + audio_get_clock_us();
        if (ps->seek.pending_secs != 0) {   // tap armed or scrubbing: preview target
            s64 prev = (s64)hud_elapsed + (s64)ps->seek.pending_secs * 1000000LL;
            hud_elapsed = prev < 0 ? 0 : (u64)prev;
        }
        hud_draw(hud_elapsed, ps->paused);
        { static int s_hg2 = 0; if (s_hg2 < 12) { plog("hud_gate: draw returned"); s_hg2++; } }
    }
}
