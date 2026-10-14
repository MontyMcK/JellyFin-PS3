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
#include "player_stats.h"
#include "rsxutil.h"
#include "ui.h"          // drawTTF
#include "ui_visuals.h" // ttf_text_width
#include "subtitles.h"
#include "display_diag.h"
#include "subfont.h"
#include "subcolor.h"
#include "vblend.h"
#include <string.h>

// Longest single rendered line. Cues are capped well below this in
// subtitles.cpp; this is only the on-stack row buffer.
#define SUB_ROW_MAX 208

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
        display_diag_session(30, 1, DM_FPS_TIMEOUT, jbuf_fw(), jbuf_fh());
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

// Count a PRESENTATION, not a buffer swap.
//
// player_stats_on_frame_shown() derives `held` -- how many vblanks a frame
// stayed on screen -- from the hardware vblank counter, and the pulldown
// figures (pd= in the heartbeat) are built from it: 24fps on a 59.94Hz
// output should alternate 3,2,3,2, so hold2 and hold3 should be roughly
// equal and hold_other near zero.
//
// It used to be called from BOTH flip paths below, unconditionally, and
// that made the numbers meaningless.  The upload thread re-stages
// jbuf_peek() and raises s_vid_frame_ready again the moment the display
// consumes it, WITHOUT popping -- so the flag goes true about once per
// vblank even for 24fps content.  `held` was therefore almost always 1,
// which is neither 2 nor 3, and every sample fell into hold_other.  That
// is why pd read like 14/1/1905: not broken cadence, a broken counter.
//
// The upload thread now records WHICH frame it staged, so a re-upload of
// the picture already on screen is ignored and `held` becomes the number
// of vblanks that frame was actually displayed.
static void stats_on_picture_shown(void) {
    static u32 s_last_seq = 0xFFFFFFFFu;
    const u32  seq = s_vid_uploaded_seq;
    if (seq == s_last_seq) return;      // same picture, re-staged
    s_last_seq = seq;
    player_stats_on_frame_shown();
}
void player_display_frame(PlayerState *ps) {
    check_fps_fallback(ps);
    log_refresh_rate();

    // ---- Duration-consumption gate logic (Movian-style temporal blend) ----
    // Drain one true vblank of content per real vblank.  The nominal period MUST
    // track the actual display refresh (timing_vblank_period_us), NOT a hardcoded
    // 16683: that constant is the 59.94Hz value and undershoots ~17% on a 50Hz
    // PAL display (20000us), leaving the avsync loop to carry a permanent
    // ~+3.4ms bias that never locks — the reported CRT judder.  Applies to both
    // hardware and emulator; on a 59.94Hz display it evaluates to 16683 as before.
    s64 nominal_vblank_us = timing_vblank_period_us();
    s64 vblank_period_us  = avsync_biased_period(nominal_vblank_us);
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

    // ---- 1:1 (24p output of 24p film): frame-locked ----
    // Exactly one picture per vblank, never blended.  The duration gate below
    // is wrong here: its A/V bias nudges the per-vblank period by up to 5 ms,
    // which at 59.94 only shifts a crossfade but at 1:1 repeats or skips a
    // frame every few frames -- constant judder (hardware, 2026-09-27).  A/V
    // is corrected instead by one whole repeat/skip when the smoothed offset
    // passes 25 ms: that moves it by one frame (41.7 ms), landing inside
    // +/-17 ms, so it cannot oscillate.
    const bool one_to_one = timing_is_1to1();
    if (one_to_one) {
        static u64 s_d_t = 0; static int s_d_calls = 0, s_d_pops = 0, s_d_notready = 0, s_d_empty = 0;
        static int s_d_fr0 = 0;
        s_d_calls++;
        if (!s_vid_frame_ready) s_d_notready++;
        if (jbuf_count() == 0) s_d_empty++;
        const u64 now = timing_get_us();
        if (!s_d_t) { s_d_t = now; s_d_fr0 = ps->frame_count; }
        if (now - s_d_t >= 1000000ULL) {
            s_d_pops = ps->frame_count - s_d_fr0;
            char b[160];
            snprintf(b, sizeof b, "1to1: 1s calls=%d pops=%d notready=%d empty=%d q=%d paused=%d",
                     s_d_calls, s_d_pops, s_d_notready, s_d_empty, jbuf_count(), (int)ps->paused);
            plog(b);
            s_d_t = now; s_d_fr0 = ps->frame_count;
            s_d_calls = s_d_notready = s_d_empty = 0;
        }
    }
    if (one_to_one && !ps->paused && s_vid_frame_ready && s_timing_ready && jbuf_count() > 0) {
        // The RAW offset of the frame about to show decides the big moves;
        // the smoothed one lags ~10 frames, and acting on it after a reopen
        // (video ~1 s ahead of the new stream's sound) repeated a frame every
        // third vblank for two seconds -- stutter -- and overshot (2026-09-27).
        const s64 raw = avsync_compute_diff(jbuf_peek_pts_us(), ps->play_base_us);
        static int  s_settle = 0;         // vblanks before another SMALL correction
        static bool s_holding = false;
        const s64 sm = avsync_get_smoothed_diff();
        if (s_settle > 0) s_settle--;
        if (raw > 60000 && raw < 3000000 && (s_holding || raw > 150000)) {
            // Far ahead: hold this picture, cleanly, until the sound reaches
            // it (down to 60 ms, so it does not stop short and stutter on).
            if (!s_holding) {
                s_holding = true;
                char b[80]; snprintf(b, sizeof b, "1to1: hold (video +%lld us)", (long long)raw); plog(b);
            }
        } else {
            s_holding = false;
            if (raw < -150000 && raw > -10000000 && jbuf_count() > 1) {
                // Far behind -- a stall starved the picture while the sound
                // played on.  Drop every frame already late, at once: one
                // jump instead of seconds of frame-skipping judder.  Runs
                // every vblank until caught up.  Was capped at 3 s: a start
                // 4.9 s behind fell through to one skip per 2 s and stayed
                // 3+ s out of lip sync for the whole film (2026-09-27).
                int dropped = 0;
                while (jbuf_count() > 1) {
                    const s64 d = avsync_compute_diff(jbuf_peek_pts_us(), ps->play_base_us);
                    if (d > -40000) break;
                    jbuf_consume_dur(jbuf_peek_dur());
                    jbuf_advance();
                    dropped++;
                }
                s_settle = 24;
                char b[80]; snprintf(b, sizeof b, "1to1: caught up (video %lld us), dropped %d",
                                     (long long)raw, dropped); plog(b);
            } else if (s_settle == 0 && sm < -25000 && raw < -25000 && jbuf_count() > 1) {
                // A little behind: drop one.
                jbuf_consume_dur(jbuf_peek_dur());
                jbuf_advance();
                s_settle = 48;
                char b[80]; snprintf(b, sizeof b, "1to1: skip (video %lld us)", (long long)sm); plog(b);
            } else if (s_settle == 0 && sm > 25000 && raw > 25000) {
                // A little ahead: show this picture one more vblank.
                s_settle = 48;
                char b[80]; snprintf(b, sizeof b, "1to1: repeat (video +%lld us)", (long long)sm); plog(b);
                goto one_to_one_done;
            }
            jbuf_consume_dur(jbuf_peek_dur());
            jbuf_advance();
            do_pop = true;                // render_blend stays false: pure A
        }
one_to_one_done: ;
    } else if (!one_to_one && !ps->paused && s_vid_frame_ready && s_timing_ready && jbuf_count() > 0) {
        // Measurement only — result discarded; EMA updated for logging.
        // play_base lets avsync fold out an absolute (sub-burn) video PTS.
        const s64 raw60 = avsync_compute_diff(jbuf_peek_pts_us(), ps->play_base_us);
        // Far behind (a stall or a slow start starved the picture while the
        // sound played on): drop the frames that are already late at once,
        // as the 1:1 path does.  The duration gate's bias moves at most 5 ms
        // a vblank, so a second behind took seconds to close (2026-09-27).
        if (raw60 < -150000 && raw60 > -10000000 && jbuf_count() > 1) {
            int dropped = 0;
            while (jbuf_count() > 1) {
                const s64 d = avsync_compute_diff(jbuf_peek_pts_us(), ps->play_base_us);
                if (d > -40000) break;
                jbuf_consume_dur(jbuf_peek_dur());
                jbuf_advance();
                dropped++;
            }
            if (dropped) {
                char b[80]; snprintf(b, sizeof b, "avsync: caught up (video %lld us), dropped %d",
                                     (long long)raw60, dropped); plog(b);
            }
        }

        s64  dur_a = jbuf_peek_dur();
#ifdef VID_DISABLE_BLEND
        bool b_ok  = false;   // force pure-A: no crossfade ever
#else
        // Whole frames with pulldown only, unless blending is switched on
        // (vblend.h).  Interlaced output scans each flip out as alternating
        // fields, so a blend reads as back-and-forth motion on a CRT; and a
        // TV that detects 3:2 pulldown sees blended frames as a broken cadence.
        bool b_ok  = (jbuf_peek_next() != NULL) && s_vid_b_present &&
                     !timing_display_interlaced() && vblend_enabled();
#endif

        if (!b_ok || dur_a >= vblank_period_us) {
            // Pure-A: consume one vblank period, pop if frame exhausted
            const u32 seq0 = jbuf_peek_seq();
            const s64 left = jbuf_peek_dur() - vblank_period_us;
            jbuf_consume_dur(vblank_period_us);
            jbuf_advance();
            // The part of this vblank the old frame did not use belongs to the
            // new one.  Dropping it held every 24 fps frame for 3 vblanks on a
            // 59.94 Hz output (20 fps), and the A/V catch-up then dropped two
            // frames every half second: the rubber-banding.
            if (left < 0 && jbuf_peek_seq() != seq0) jbuf_consume_dur(-left);
            // A pop here is a NEW picture and must count as one.  At 59.94 a
            // film frame always ends in the crossfade branch below, so this
            // never mattered; at a 1:1 cadence (24p output, 2026-09-27) EVERY
            // frame ends here, frame_count stayed 0, and the video -- drawn
            // only once frame_count > 0 -- was never drawn: audio, black screen.
            if (jbuf_peek_seq() != seq0) do_pop = true;
            else                         silent_flip = true;
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

    // Clearing s_vid_frame_ready hands the texture shown until now to the
    // upload thread, which starts overwriting it at once, so the GPU must be
    // done reading it first.  A flip queued inside a seek (the reopen's
    // spinner) left the flip wait one frame behind for the rest of the
    // session: the CPU ran a frame ahead, the upload landed while the
    // previous draw was still sampling, and frames came out as two pictures
    // torn together -- the zig-zag on motion after a seek at 24p (2026-09-27).
    if (do_pop || (silent_flip && s_vid_frame_ready) ||
        (ps->show_seek_frame && ps->paused && s_vid_frame_ready))
        rsxSync();

    if (do_pop) {
        u32 disp_seq = jbuf_peek_seq();
        ps->frame_count++;
        __asm__ volatile("sync" ::: "memory");
        s_vid_frame_ready = false;
        s_vid_disp_idx ^= 1;
        stats_on_picture_shown();   // observe only; ignores re-staged frames
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
                s64  biased = avsync_biased_period(nominal_vblank_us);
                char buf3[176];
                snprintf(buf3, sizeof(buf3),
                    "avsync: smooth=%lldus locked=%d audio=%lluus video=%lluus bias=%lldus nom=%lldus",
                    (long long)smooth,
                    locked ? 1 : 0,
                    (unsigned long long)audio_get_clock_us(),
                    (unsigned long long)jbuf_peek_pts_us(),
                    (long long)(biased - nominal_vblank_us),
                    (long long)nominal_vblank_us);
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
        stats_on_picture_shown();   // observe only; ignores re-staged frames
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

    // Subtitles.  Drawn BEFORE the HUD and outside its visibility gate: the
    // seek bar comes and goes, subtitles must not.  They also use the clock
    // WITHOUT the seek preview offset that the HUD applies below -- while a
    // seek is armed the bar should show where you are going, but the words on
    // screen still belong to the frame actually being displayed.
    if (subs_active() && ps->frame_count > 0 && subs_is_pgs()) {
        const u64 now_ms = (ps->play_base_us + audio_get_clock_us()) / 1000ULL;
        const PgsBitmap *bmp = subs_pgs_at(now_ms);
        if (bmp && bmp->rgba && bmp->frame_w > 0 && bmp->frame_h > 0) {
            // Position scales from the PG stream's OWN authored coordinate
            // space (bmp->frame_w/h -- typically 1920x1080 for a BD disc)
            // into the actual display output. SIZE DOES NOT: drawBitmapRect-
            // Alpha is a crop, not a resize (see ui.h), so the bitmap is
            // drawn at its own decoded pixel dimensions regardless of
            // display resolution. For a PS3 set to output at the disc's
            // native resolution (the common 1080p BD -> 1080p output case)
            // that scale factor is 1:1 and this is exactly right; at a
            // lower output resolution (e.g. 720p) an authored-1080p bitmap
            // will render oversized relative to the frame. Flagged rather
            // than silently wrong -- a scaling blit is the fix if hardware
            // testing shows this matters in practice.
            int dx = (int)((s64)bmp->x * (s64)display_width  / bmp->frame_w);
            int dy = (int)((s64)bmp->y * (s64)display_height / bmp->frame_h);
            drawBitmapRectAlpha(bmp->rgba, (u32)bmp->width,
                                0, 0, (u32)bmp->width, (u32)bmp->height,
                                (u32)dx, (u32)dy);
        }
    } else if (subs_active() && ps->frame_count > 0) {
        const u64 now_ms = (ps->play_base_us + audio_get_clock_us()) / 1000ULL;
        const char *line = subs_text_at(now_ms);
        if (line && *line) {
            // Bottom-centred, one line above the other, inside the title-safe
            // area so an overscanning CRT or plasma does not clip the text.
            // TYPEFACE AND WEIGHT.
            //
            // The fonts people associate with subtitles -- Arial, Helvetica,
            // Netflix Sans, Tiresias -- are all proprietary and cannot ship in
            // a GPLv3 package. Open Sans, already bundled here for the UI, is
            // the open face closest to them: a humanist sans with a large
            // x-height and open apertures, which is what actually drives
            // legibility at a distance.
            //
            // Weight matters more than which humanist sans it is, and every
            // broadcaster and streaming service sets subtitles semibold or
            // bolder. The bold face is already loaded, so this costs nothing
            // and is the single biggest readability win available.
            const int W  = (int)display_width;
            const int px = (display_height >= 720) ? 32 : 22;
            const int lh = px + 8;
            // Fill, outline and outline weight all come from the chosen
            // look -- they are not independent. A pale fill needs a heavier
            // outline than a saturated one to hold its edge, and a
            // translucent fill needs a thin one or the outline ends up more
            // solid than the letters it surrounds. See subcolor.h.
            const u32 fill    = subcolor_fill();
            const u32 outline = subcolor_outline();
            const int ow      = subcolor_outline_px(display_height);
            int nlines = 1;
            for (const char *q = line; *q; q++) if (*q == '\n') nlines++;
            int y = (int)display_height - (int)(display_height / 12) - nlines * lh;

            const char *p2 = line;
            char row[SUB_ROW_MAX];
            while (*p2) {
                const char *nl = strchr(p2, '\n');
                int len = nl ? (int)(nl - p2) : (int)strlen(p2);
                if (len > SUB_ROW_MAX - 1) len = SUB_ROW_MAX - 1;
                memcpy(row, p2, len); row[len] = '\0';

                const int face = subfont_face();
                const int tw   = ttf_text_width_face(row, (float)px, face);
                const int x  = (W - tw) / 2;
                // Outline in every direction.  Film subtitles sit over
                // whatever happens to be on screen, and white on a bright
                // scene is unreadable without one; offset draws cost far less
                // than a shadow texture and need no extra GPU state. The
                // glyph cache means the repeats are cheap -- each glyph is
                // rasterized once and blitted nine times.
                for (int dy = -ow; dy <= ow; dy++)
                    for (int dx = -ow; dx <= ow; dx++)
                        if (dx || dy)
                            drawTTF_face((u32)(x + dx), (u32)(y + dy), row,
                                         (float)px, outline, face);
                drawTTF_face((u32)x, (u32)y, row, (float)px, fill, face);

                y += lh;
                if (!nl) break;
                p2 = nl + 1;
            }
        }
    }

    // HUD overlay.
    // The HUD is composed into a texture and drawn as one alpha-blended GPU
    // quad (hud_draw.cpp): no CPU writes into the framebuffer, so no rsxSync
    // fence is needed here — the quad is FIFO-ordered after the video draw
    // like any other command, and its vertices go inline (no array fetch to
    // wedge on).  This keeps the loop under one vblank with the bar visible;
    // the old CPU-drawn HUD (fenced with rsxSync each frame) pushed it to a
    // 2-vblank cadence, halving playback fps whenever the seek bar showed.
    if (hud_is_visible() && ps->frame_count > 0) {
        static int s_hg = 0;
        if (s_hg < 12) {
            char b[80];
            snprintf(b, sizeof(b), "hud_gate: vis paused=%d fr=%d",
                     (int)ps->paused, ps->frame_count);
            plog(b); s_hg++;
        }
        // While a seek is armed, show where the accumulated skip will land so
        // the user can keep tapping toward the right spot before it fires.
        u64 hud_elapsed = ps->play_base_us + audio_get_clock_us();
        if (ps->seek.pending_secs != 0) {   // tap armed or scrubbing: preview target
            s64 prev = (s64)hud_elapsed + (s64)ps->seek.pending_secs * 1000000LL;
            hud_elapsed = prev < 0 ? 0 : (u64)prev;
        }
        hud_draw(hud_elapsed, ps->paused, ps->seek.state == SEEK_SCRUB);
        { static int s_hg2 = 0; if (s_hg2 < 12) { plog("hud_gate: draw returned"); s_hg2++; } }
    }

    // Player stats overlay.  Same GPU-quad discipline as the HUD above — it
    // must be queued after the video draw, and it costs nothing at all when
    // the Settings toggle is off (returns before any compose or layout).
    if (ps->frame_count > 0)
        player_stats_render_overlay();
}
