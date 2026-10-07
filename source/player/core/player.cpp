// show_player — Jellyfin PS3 media player orchestrator: session setup,
// thread spawn, the main display loop, and teardown.  The heavy lifting
// lives in player_session / player_menu / player_seek / player_display.

#include "segments.h"
#include "autoskip.h"
#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <net/net.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/thread.h>
#include <unistd.h>

#include "plog.h"
#include "subtitles.h"
#include "hd1080.h"
#include "vquality.h"
#include "surround.h"
#include "stream.h"
#include "audio.h"
#include "ui_sfx.h"
#include "adec.h"
#include "adec_dts.h"
#include "adec_truehd.h"
#include "video.h"
#include "timing.h"
#include "player.h"
#include "player_hud.h"
#include "player_internal.h"
#include "player_stats.h"
#include "ui.h"
#include "ui_visuals.h"
#include "jellyfin_api.h"
#include "rsxutil.h"
#include "ui_wave.h"          // wave_draw_rrect_gpu: the loading spinner
#include "thumbnail_cache.h"
#include "display_24p.h"   // physical 1080p24 output
#include "display_diag.h"
#include "meminfo.h"   // read-ahead ring sizing
#include "slog.h"
#include "trickplay.h"
#include "ui_buffering.h"
#include "music_player.h"   // music_join_stale
#include "experience.h"      // vpick_audio_words: the audio chip's words
#include "lclog.h"     // 24p lifecycle trace

extern void crash_log(const char *msg);

// A startup status screen ("Initializing decoder…", etc.) followed by a long
// CPU-only stretch.  flip() is ASYNCHRONOUS — it queues the frame and returns
// while the RSX is still reading the glyph-blit source memory.  The work that
// runs right after each of these screens (thumb_cache_shutdown, vdec_open's
// 96 MB memalign + heap reorg, jbuf_alloc, the prefill) frees/moves the heap
// out from under those in-flight blits.  On real hardware the RSX has already
// drained by then; RPCS3 has not, so it reads the freed pages (poisoned to
// 0xfeed0000), fails to decode the NV3089 blit, and its FIFO watchdog kills the
// RSX thread ("Dead FIFO commands queue state") — which is exactly why movie
// playback took the whole emulator down.  Draining the FIFO before the blocking
// work removes the use-after-free.  Hardware is byte-for-byte unaffected: the
// rsxSync() is compiled out and this is a plain flip() there.
static inline void player_startup_flip(void) {
    flip();
#if BUILD_FOR_RPCS3
    rsxSync();   // block until the RSX has consumed the just-queued blits
#endif
}

// A cosmetic "loading" screen shown between the XMB and live video.
//
// The bitmap-font text path (drawHeader/drawText) renders each glyph with the
// RSX NV3089 transfer-scale engine.  On RPCS3 that engine is freeze-prone — the
// same reason thumbnail_cache blits its cards with the CPU instead — and firing
// it here, during the fragile XMB->player transition, wedges the RSX FIFO
// ("Dead FIFO ... nv3089 decode_transfer_registers: Timer expired") and kills
// playback before it starts.  So on the emulator we DROP the glyph blits and
// just present a drained frame; the status text is only cosmetic and hardware
// (where NV3089 is fine) still draws it in full.  The flip keeps the display
// alive through the long vdec_open/stream stalls that follow.
static inline void player_status_screen(const char *name, const char *msg) {
    // With the spine gate on, the startup is one continuous presentation
    // (render/ui_buffering.cpp) instead of a sequence of text screens; the
    // callers name the step with player_startup_step() and this is not used.
    if (buffering_active()) { (void)name; (void)msg; buffering_frame(); return; }
#if !BUILD_FOR_RPCS3
    drawHeader();
    drawTextf(40, 100, "%.70s", name);
    drawText(40, 130, msg);
#else
    (void)name; (void)msg;
#endif
    player_startup_flip();
}

// One startup step: the buffering presentation's label when it is running,
// the old status line otherwise.
static void player_startup_step(const char *name, const char *label,
                                const char *old_msg) {
    if (buffering_active()) {
        buffering_step(label, false, NULL);
        buffering_frame();
    } else {
        player_status_screen(name, old_msg);
    }
}

// Leaving startup before playback (an error, or the user backed out): fade
// the presentation out quickly so the error screen does not cut into it.
static void player_startup_abort(void) {
    if (buffering_active()) buffering_finish(false);
}

// -------------------------------------------------------
// 24p output callbacks.  display_24p.cpp never draws or reads the pad itself:
// it asks for exactly one prompt, drawn and flipped while the ORIGINAL mode is
// still up, and then only polls for an answer.  See display_24p.h.
// -------------------------------------------------------
static const char *s_d24_title = "";

static bool d24_draw_prompt(const char *line1, const char *line2) {
#if !BUILD_FOR_RPCS3
    // Cleared each time: the prompt is redrawn every pass of the 24p confirm
    // wait with a changing countdown, which would otherwise smear.
    clearScreen(0x00000000);
    rsxSync();
    drawHeader();
    drawTextf(40, 100, "%.70s", s_d24_title);
    drawText(40, 130, line1);
    if (line2 && line2[0]) drawText(40, 160, line2);
#else
    (void)line1; (void)line2;
#endif
    gcmResetFlipStatus();
    flip();
    // Bounded: 500 ms is 12 frames even at 24Hz.  Never block forever on a
    // vblank that may not come (the 2026-09-18 hang).
    return waitflip_timeout(500000);
}

static int d24_poll_answer(void) {
    poll_buttons();
    if (BTN_PRESSED(cross))  return 1;
    if (BTN_PRESSED(circle)) return -1;
    return 0;
}

// Lifecycle trace (lclog.h): the whole playback session in one line, so each
// 24p phase can be read against the state of the player, the play session and
// the socket.  Only valid while no thread but the caller reads ps->sock --
// i.e. before player_spawn_decode -- because it peeks the socket.
static const PlayerState *s_lc_ps = NULL;

static void lc_session_snapshot(const char *phase, bool probe_sock) {
    const PlayerState *p = s_lc_ps;
    if (!p) { lc_logf("session[%s] no session", phase); return; }
    char sk[200] = "sock=(not probed: decode thread owns it)";
    if (probe_sock && p->sock >= 0) stream_probe(p->sock, sk, sizeof(sk));
    lc_logf("session[%s] running=%u playing=%d vdec_err=%d dec_tid=%llu "
            "jbuf=%d ring=%d/%d psid=%.12s",
            phase, running, (int)p->playing, (int)s_vdec_error,
            (unsigned long long)p->dec_tid, jbuf_count(),
            decode_ring_fill(), decode_ring_cap(), p->session_id);
    lc_logf("session[%s] %s", phase, sk);
}

static void d24_lifecycle(const char *phase) {
    // The decode thread owns the socket during the switch now: never peek it.
    lc_session_snapshot(phase, false);
}

// -------------------------------------------------------
// End-of-item auto-advance ("Next Episode")
// -------------------------------------------------------
// The UI arms this before show_player when the played item has a follower.
// In the last 90 s of playback the player shows the popup badge (just the
// label) with the instruction line drawn separately below it; SELECT ends
// playback with the next-request flag set, which the UI reads via
// player_take_next_request() to start the next item.  For episodes the
// last 30 s also run a countdown that fires the request automatically.
static bool s_next_armed     = false;
static bool s_next_requested = false;
static char s_next_label[32] = "NEXT EPISODE";
static char s_next_hint[64]  = "Press SELECT for next episode";

void player_arm_next(const char *label, const char *hint) {
    s_next_armed     = true;
    s_next_requested = false;
    if (label && label[0])
        snprintf(s_next_label, sizeof(s_next_label), "%s", label);
    if (hint && hint[0])
        snprintf(s_next_hint, sizeof(s_next_hint), "%s", hint);
}

bool player_take_next_request(void) {
    bool r = s_next_requested;
    s_next_requested = false;
    return r;
}

// Popup badge top-right; the instruction line is drawn separately below
// the badge, outside the box.  auto_secs >= 0 appends the auto-advance
// countdown to the instruction line.
static void player_draw_next_popup(int auto_secs) {
    const float label_px = 26.0f;
    const float hint_px  = 16.0f;
    const int   pad_x = 26, pad_y = 14, margin = 48;

    int tw = ttf_text_width(s_next_label, label_px);
    int bw = tw + 2 * pad_x;
    int bh = (int)label_px + 2 * pad_y;
    int bx = (int)display_width - bw - margin;
    int by = margin;

    drawRect((u32)(bx - 1), (u32)(by - 1), (u32)(bw + 2), (u32)(bh + 2),
             XMB_HAIRLINE);
    drawRect((u32)bx, (u32)by, (u32)bw, (u32)bh, XMB_PANEL);
    drawRect((u32)bx, (u32)by, 3, (u32)bh, XMB_ACCENT);
    drawTTF((u32)(bx + pad_x), (u32)(by + pad_y), s_next_label, label_px,
            XMB_TEXT, true);

    char hint[96];
    if (auto_secs >= 0)
        snprintf(hint, sizeof(hint), "%s \xC2\xB7 starting in %ds",
                 s_next_hint, auto_secs);
    else
        snprintf(hint, sizeof(hint), "%s", s_next_hint);
    int hw = ttf_text_width(hint, hint_px);
    drawTTF((u32)(bx + bw - hw), (u32)(by + bh + 14), hint, hint_px,
            XMB_TEXT_DIM);
}

// "Skip Intro" badge, bottom right, above where the control bar sits: the X
// glyph and the label in the same panel style as the NEXT badge.
static void player_draw_skip_badge(const char *label) {
    const float label_px = 26.0f;
    const int   pad_x = 22, pad_y = 14, margin = 48, gap = 14;
    const int   icon_h = (int)label_px;
    const int   iw = ps_btn_width('X', icon_h);
    const int   tw = ttf_text_width(label, label_px);
    const int   bw = pad_x + iw + gap + tw + pad_x;
    const int   bh = (int)label_px + 2 * pad_y;
    const int   bx = (int)display_width - bw - margin;
    const int   by = (int)display_height - bh - margin * 3;

    drawRect((u32)(bx - 1), (u32)(by - 1), (u32)(bw + 2), (u32)(bh + 2), XMB_HAIRLINE);
    drawRect((u32)bx, (u32)by, (u32)bw, (u32)bh, XMB_PANEL);
    drawRect((u32)bx, (u32)by, 3, (u32)bh, XMB_ACCENT);
    draw_ps_button_vcentered((u32)(bx + pad_x), by + bh / 2, 'X', icon_h, 255);
    drawTTF((u32)(bx + pad_x + iw + gap), (u32)(by + pad_y), label, label_px, XMB_TEXT, true);
}

// The loading spinner (2026-09-27): ten dots round a ring, a bright head
// chasing round with a fading tail, over whatever video is on screen.  GPU
// phase (before rsxSync).  t in seconds.
void player_spinner_gpu(float t)
{
    const int cx = (int)display_width / 2, cy = (int)display_height / 2;
    const float R = (float)UIS_H(30);
    const int d = UIS_H(9) > 4 ? UIS_H(9) : 4;
    wave_draw_rrect_gpu(cx - (int)R - d * 2, cy - (int)R - d * 2, 2 * (int)R + d * 4,
                        2 * (int)R + d * 4, (int)R + d * 2, 0x00000000, 0x00000000, 90);
    for (int i = 0; i < 10; i++) {
        const float a = (float)i / 10.0f * 6.2831853f;
        float f = (float)i / 10.0f - t * 1.1f;
        f -= (float)(int)f; if (f < 0.0f) f += 1.0f;           // 0 = the head
        const float k = 1.0f - f;
        const int x = cx + (int)(R * __builtin_sinf(a)) - d / 2;
        const int y = cy - (int)(R * __builtin_cosf(a)) - d / 2;
        wave_draw_rrect_gpu(x, y, d, d, d / 2, 0x00FFFFFF, 0x00FFFFFF,
                            (u8)(40.0f + 215.0f * k * k));
    }
}

// Waiting on the server during a SEEK's reopen (2026-09-27: a fast-forward
// sat 60 s on a debrid server that then answered 500, with the screen frozen
// and no way out -- force-quit).  The last picture stays up with the spinner
// and a count; Circle gives up (playback ends, back to the menu).
static bool s_seekwait_flip = false;

// Settle the flip bookkeeping after drawing outside the main loop: every
// queued flip has landed once the GPU reaches a label behind it, so the flip
// status can be cleared with nothing pending.  The caller's next flip is
// then the one its next wait sees.
static void player_flip_resync(void)
{
    rsxSync();
    gcmResetFlipStatus();
    s_seekwait_flip = false;
}

bool player_seek_wait(unsigned elapsed_ms)
{
    poll_buttons();
    if (BTN_PRESSED(circle)) return false;
    if (s_seekwait_flip) waitflip_timeout(250000);
    const u32 fw = jbuf_fw(), fh = jbuf_fh();
    if (fw && fh) vid_gpu_draw(false, 0.0f, fw, fh);
    else          clearScreen(0x00000000);
    player_spinner_gpu((float)elapsed_ms * 0.001f);
    rsxSync();
    char msg[64];
    snprintf(msg, sizeof msg, "Waiting for the server \xC2\xB7 %us", elapsed_ms / 1000u);
    const float px = UIS_TF(16.0f);
    const int w = ttf_text_width(msg, px);
    const int y = (int)display_height / 2 + UIS_H(58);
    drawTTF((u32)(((int)display_width - w) / 2), (u32)y, msg, px, 0x00FFFFFF);
    {
        const char *c = "O  Cancel";
        const int cw = ttf_text_width(c, UIS_TF(13.0f));
        drawTTF((u32)(((int)display_width - cw) / 2), (u32)(y + UIS_H(26)), c,
                UIS_TF(13.0f), 0x00B8BCD0);
    }
    gcmResetFlipStatus();
    flip();
    s_seekwait_flip = true;
    return true;
}

// Drawn while stream_open() waits for the server's response headers.
//
// Switching quality makes Jellyfin start a fresh transcode, and a cold start
// can take tens of seconds.  Previously nothing was drawn and no button was
// read for that whole time, so the app looked hung and got force-quit --
// reported, reasonably, as a crash.  Now it says what it is waiting for,
// counts, and takes Circle for an answer.
static const char *s_wait_title = "";
static bool player_stream_wait(unsigned elapsed_ms)
{
    poll_buttons();                          // refresh btn_cur/btn_prev
    if (BTN_PRESSED(circle)) return false;   // user gave up: abort the open

    if (buffering_active()) {
        char lab[48];
        snprintf(lab, sizeof lab, "Waiting for the server \xC2\xB7 %us",
                 elapsed_ms / 1000u);
        buffering_step(lab, false, "Cancel");
        buffering_frame();
        return true;
    }
    char msg[96];
    snprintf(msg, sizeof(msg),
             "Waiting for the server... %us   (Circle to cancel)",
             elapsed_ms / 1000u);
    player_status_screen(s_wait_title, msg);
    return true;
}

// --- leaving playback --------------------------------------------------------
//
// Measured 2026-09-24: backing out of a film froze the screen for 6-10 s.  The
// Stopped report and the transcode stop ran on this thread, AFTER the threads
// were joined but BEFORE the stream socket was closed -- so the server was
// still pushing video into a 512 KB receive buffer nobody read, on a 128 KB
// libnet pool, and both requests timed out at 5 s (`http=-1`).  Now the socket
// is closed first, the two requests run on a thread of their own, and the
// Returning screen is up while they and the teardown run.
static struct { char item[64]; char sess[128]; u64 ticks; } s_exit_rep;
static volatile bool    s_exit_rep_done = true;
static bool             s_exit_rep_live = false;    // a thread still to join
static sys_ppu_thread_t s_exit_rep_tid;

static void exit_reports_run(void) {
    jellyfin_report_stopped(s_exit_rep.item, s_exit_rep.sess, s_exit_rep.ticks);
    // Kill the server-side transcode for this session.  Without this the job
    // is left running when playback ends, and starting the SAME item and
    // version again collides with the orphan -- the new stream request comes
    // back HTTP 500.  Seeks already do this (player_seek.cpp) for the same
    // reason.
    if (s_exit_rep.sess[0]) jellyfin_stop_transcode(s_exit_rep.sess);
}

static void exit_reports_fn(void *arg) {
    (void)arg;
    exit_reports_run();
    __sync_synchronize();
    s_exit_rep_done = true;
    sysThreadExit(0);
}

// A previous exit's reports, if a sick network ran them past the Returning
// screen: the next stream must not be asked for before its predecessor's
// transcode has been stopped (see exit_reports_run).
static void exit_reports_join(void) {
    if (!s_exit_rep_live) return;
    u64 r;
    sysThreadJoin(s_exit_rep_tid, &r);
    s_exit_rep_live = false;
}

static void exit_reports_start(const char *item_id, const char *sess, u64 ticks) {
    exit_reports_join();
    snprintf(s_exit_rep.item, sizeof s_exit_rep.item, "%s", item_id ? item_id : "");
    snprintf(s_exit_rep.sess, sizeof s_exit_rep.sess, "%s", sess ? sess : "");
    s_exit_rep.ticks = ticks;
    s_exit_rep_done = false;
    __sync_synchronize();
    static char name[] = "jf_exitrep";
    if (sysThreadCreate(&s_exit_rep_tid, exit_reports_fn, NULL, 1100, 64 * 1024,
                        THREAD_JOINABLE, name) != 0) {
        exit_reports_run();              // no thread: the old way, inline
        s_exit_rep_done = true;
        return;
    }
    s_exit_rep_live = true;
}

// One frame of the Returning screen, when it is up.
static inline void returning_pump(bool on) {
    if (!on) return;
    sysUtilCheckCallback();
    buffering_frame();
}

// ---- episode chain carry-over (see player.h) -------------------------------
static bool s_chain_on = false;
static char s_chain_lang[48]  = "";      // "English", from the audio label's first part
static char s_chain_label[64] = "";      // the exact audio label last played
static char s_chain_src[128]  = "";      // the version's label last played

void player_chain_begin(void) {
    s_chain_on = true;
    s_chain_lang[0] = s_chain_label[0] = s_chain_src[0] = '\0';
}
void player_chain_end(void) { s_chain_on = false; }
const char *player_chain_source_label(void) { return s_chain_src; }

// The language part of a DisplayTitle ("English - EAC3 - 5.1 - Default").
static void audio_lang(const char *label, char *out, size_t cap) {
    size_t n = 0;
    const char *dash = strstr(label, " - ");
    n = dash ? (size_t)(dash - label) : strlen(label);
    if (n >= cap) n = cap - 1;
    memcpy(out, label, n); out[n] = '\0';
}

// Same language as the last episode: the exact label first (same language,
// codec and channels), else the first track in that language.
static int chain_find_audio(const JFTracks *t) {
    if (!s_chain_on || !s_chain_lang[0]) return -1;
    for (int i = 0; i < t->n_audio; i++)
        if (strcmp(t->audio[i].label, s_chain_label) == 0) return i;
    for (int i = 0; i < t->n_audio; i++) {
        char l[48]; audio_lang(t->audio[i].label, l, sizeof l);
        if (strcasecmp(l, s_chain_lang) == 0) return i;
    }
    return -1;
}

void show_player(const JFItem *item, u32 resume_secs,
                 const char *media_source_id) {
    crash_log("p1 enter");
    exit_reports_join();
    music_join_stale();
    plog("show_player: enter");
    plog("show_player: BUILD=seek-diag-1");
    init_btns();

#if BUILD_FOR_RPCS3
    // Drain the XMB's final frame off the RSX before the first long PPU stall.
    // The last grid frame still has its movie-poster thumbnail blits (128x48,
    // NV3089) queued; the network calls just below (get_play_session_id /
    // fetch_tracks) then block the PPU for ~seconds while thumb_cache_shutdown
    // is about to free those poster bitmaps.  On real hardware the RSX drains
    // in the background and waits happily; RPCS3 sees the FIFO not advance and
    // trips its dead-FIFO watchdog ("Timer expired" in nv3089), killing the RSX
    // and taking playback down before it starts.  Emptying the FIFO here means
    // every startup stall (network, then vdec_open) runs on an idle queue with
    // nothing for the watchdog to fire on.  Compiled out on hardware.
    rsxSync();
#endif
    {
        static bool s_logged_cbufs = false;
        if (!s_logged_cbufs) {
            s_logged_cbufs = true;
            char buf[128];
            snprintf(buf, sizeof(buf), "color_buffer[0]=%p", (void*)color_buffer[0]);
            plog(buf);
            snprintf(buf, sizeof(buf), "color_buffer[1]=%p", (void*)color_buffer[1]);
            plog(buf);
        }
    }

    // Consume the auto-advance arming one-shot so a later, unrelated
    // playback can never inherit a stale NEXT popup.
    bool have_next = s_next_armed;
    s_next_armed = false;

    // Auto-advance countdown applies only to episodes; movies keep the
    // manual SELECT prompt.
    bool auto_next      = have_next && strcmp(item->type, "Episode") == 0;
    bool in_auto_window = false;   // last frame was inside the countdown
    bool user_stopped   = false;   // START pressed (never auto-advance)

    // Session state shared with the player_* helpers.  ~2KB of JFTracks —
    // keep off the stack.
    static PlayerState ps;
    memset(&ps, 0, sizeof(ps));
    ps.item      = item;
    ps.playing   = true;
    ps.dec_run   = true;
    ps.cur_audio = -1;
    ps.cur_sub   = -1;               // subtitles start off
    ps.sub_is_text = false;
    subs_clear();                    // a previous title's cues are not this one's
    trickplay_reset();                // ditto for a previous title's scrub sheet
    ps.menu_kind = PLAYER_MENU_NONE;

    // Baseline H.264 level 3.1 caps at 1280×720 @ 30fps.  1080p (Alpha) asks
    // the server for a full 1920×1080 High-profile transcode instead — flat,
    // not capped to the display mode, so the frame is decoded at full res and
    // downscaled at present.  Gated: OFF => the exact 720p ship path.
    // The quality row on the info screen overrides both; at its default
    // (Auto) vquality_params() resolves to exactly the two cases above, so
    // an install that never touches it requests what it always did.
    vquality_params(vquality_get(), hd1080_enabled(),
                    display_width, display_height,
                    &ps.req_w, &ps.req_h, NULL, NULL, NULL);

    // The buffering presentation starts BEFORE PlaybackInfo now (2026-09-26).
    // On this server PlaybackInfo is where Gelato syncs a title's streams --
    // 6-8 s the first time a title is opened -- and it used to run with
    // nothing on screen: the "freeze between pressing play and Connecting".
    // The call runs on a worker while the render thread animates; the
    // presentation only draws (its artwork is already in video memory) and
    // makes no request, so responseBuffer is the worker's alone meanwhile.
    if (g_spine_on) buffering_begin(item->id, item->name);
    player_startup_step(item->name, "Connecting", "Connecting to server...");
    {
        static struct {
            const JFItem *item; const char *msid; PlayerState *ps;
            volatile bool done; bool ok;
        } pi;
        pi.item = item; pi.msid = media_source_id; pi.ps = &ps;
        pi.done = false; pi.ok = false;
        __sync_synchronize();
        sys_ppu_thread_t tid;
        static char tname[] = "jf_pbinfo";
        const bool threaded = buffering_active() &&
            sysThreadCreate(&tid, [](void *a) {
                auto *p = (decltype(pi) *)a;
                p->ok = jellyfin_get_playback_info(p->item->id, p->msid, p->ps->session_id,
                                                   sizeof(p->ps->session_id), &p->ps->total_secs,
                                                   NULL, &p->ps->source, true);
                __sync_synchronize();
                p->done = true;
                sysThreadExit(0);
            }, &pi, 1100, 64 * 1024, THREAD_JOINABLE, tname) == 0;
        if (threaded) {
            const u64 t0 = timing_get_us();
            while (!pi.done) {
                sysUtilCheckCallback();
                buffering_frame_paced(16000);
                usleep(2000);
            }
            u64 rv; sysThreadJoin(tid, &rv);
            char b[128];
            snprintf(b, sizeof b, "show_player: PlaybackInfo took %llu ms (behind the buffering screen)",
                     (unsigned long long)((timing_get_us() - t0) / 1000));
            plog(b);
        } else {
            pi.ok = jellyfin_get_playback_info(item->id, media_source_id, ps.session_id,
                                               sizeof(ps.session_id), &ps.total_secs,
                                               NULL, &ps.source, true);
        }
        if (!pi.ok) {
            plog("show_player: PlaybackInfo failed, streaming without PlaySessionId");
            ps.session_id[0] = '\0';
        }
    }
    lc_logf("play-session CREATED psid=%s item=%s resume=%us",
            ps.session_id[0] ? ps.session_id : "(none)", item->id, resume_secs);

    // Only the version chosen on the info screen enters the player.  Its own
    // tracks come from the same source-aware PlaybackInfo response.
    if (ps.source.id[0]) {
        ps.tracks      = ps.source.tracks;
        ps.have_tracks = true;
        if (ps.source.runtime_secs > 0)
            ps.total_secs = ps.source.runtime_secs;
    } else {
        snprintf(ps.source.id, sizeof(ps.source.id), "%s",
                 (media_source_id && media_source_id[0])
                    ? media_source_id : item->id);
        snprintf(ps.source.label, sizeof(ps.source.label), "Default");
        ps.have_tracks = jellyfin_fetch_tracks(item->id, &ps.tracks);
        ps.source.tracks = ps.tracks;
        ps.source.runtime_secs = ps.total_secs;
    }
    if (ps.have_tracks && ps.tracks.n_audio > 0) {
        ps.cur_audio = ps.tracks.default_audio;
        // A track the user picked earlier this session (see player_menu.cpp)
        // wins over the server's own default -- that is the whole point of
        // remembering it, otherwise every episode reopens on commentary or
        // a lossy default and has to be switched by hand again.
        int pref_audio = track_pref_find_audio(&ps.tracks);
        if (pref_audio >= 0) ps.cur_audio = pref_audio;
        // Next episode: stay in the language the last one was watched in.
        else {
            const int ca = chain_find_audio(&ps.tracks);
            if (ca >= 0) {
                ps.cur_audio = ca;
                char b[112];
                snprintf(b, sizeof b, "show_player: audio kept from the last episode: %.64s",
                         ps.tracks.audio[ca].label);
                plog(b);
            }
        }
    }

    // A remembered TEXT subtitle preference is applied the same way a manual
    // pick is: fetch and parse the cues now so the URL builder below sees
    // sub_is_text=true and never asks the server to burn it in. A remembered
    // preference is only ever noted from a track the user chose themselves
    // (track_pref_note_sub), so this can never surprise them with a track
    // they never picked, and it never matches a bitmap track (track_pref_
    // find_sub requires jf_sub_is_text), so it can never trigger an
    // unexpected transcode.
    // Intro / credits markers, fetched off this thread (segments.h).
    segments_start(item->id, ps.source.id);

    if (ps.have_tracks && ps.tracks.n_subs > 0) {
        int pref_sub = track_pref_find_sub(&ps.tracks);
        if (pref_sub >= 0) {
            const JFStream *st = &ps.tracks.subs[pref_sub];
            if (subs_load(item->id, ps.source.id, st->index) > 0) {
                ps.cur_sub     = pref_sub;
                ps.sub_is_text = true;
                hud_set_cc_active(true);
            }
        }
    }

    // Continue Watching: open the transcode at the saved position.  The new
    // stream's PTS starts at 0, so play_base_us anchors the absolute clock —
    // the same mechanism every seek uses.
    if (ps.total_secs > 0 && resume_secs >= ps.total_secs)
        resume_secs = 0;             // stale position at/past the end: restart
    ps.play_base_us = (u64)resume_secs * 1000000ULL;
    if (resume_secs > 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "resume: start at %us", resume_secs);
        plog(buf);
    }

    char url[768];
    build_stream_url(url, sizeof(url), &ps, (u64)resume_secs * 10000000ULL);
    plog_url("url", url);

    // The buffering presentation starts here and runs until the first frame
    // (spine gate on; the gate off keeps the status lines below).  It reuses
    // the detail page's artwork, which is still in video memory.
    if (g_spine_on && !buffering_active()) buffering_begin(item->id, item->name);
    player_startup_step(item->name, "Preparing", "Initializing decoder...");

    // Release the UI thumbnail cache (joins its fetch thread, frees ~15 MB
    // of card bitmaps) — the decoder + jitter buffer below need every MB,
    // and jbuf_alloc fails outright with the cache resident.  Re-created on
    // every exit path; the UI simply refetches its thumbs.
    thumb_cache_shutdown();

    // Claim the cached HUD overlay buffers before the big allocations so
    // they sit low in the heap once and forever (see player_hud.h).
    hud_overlay_alloc();
    // Same reasoning for the stats panel — and it only allocates at all when
    // the Settings toggle is on, so a normal build costs nothing.
    player_stats_reset();
    player_stats_overlay_alloc();

    crash_log("p2 vdec_open begin");
    plog("show_player: vdec_open");
    if (!vdec_open()) {
        plog("show_player: vdec_open FAILED");
        player_startup_abort();
        vdec_close();
        thumb_cache_init();
        show_error("VDEC init failed.", "See /dev_hdd0/tmp/player_log.txt");
        ui_restore_rsx_state();
        return;
    }
    plog("show_player: vdec_open OK");
    crash_log("p3 vdec_open OK");

    crash_log("p4 audio_open begin");
    plog("show_player: audio_open");
    // A new title gets a fresh shot at DTS: any session veto from the
    // previous one (a coreless track, below) does not carry over.
    surround_hd_session_reset();
    // The menu sounds keep a stereo port of their own; a bitstream or an
    // 8-channel program gets the output to itself.  audio_close() resumes them.
    ui_sfx_suspend();
    audio_open(surround_enabled() ? 8 : 2);
    adec_init();
    adec_start();
    plog("show_player: audio_open done");
    crash_log("p5 audio_open OK");

    player_startup_step(item->name, "Connecting", "Connecting to stream...");

    crash_log("p6 stream_open begin");
    plog("show_player: stream_open");
    s_wait_title = item->name;
    stream_set_wait_cb(player_stream_wait);
    ps.sock = stream_open(url);
    stream_set_wait_cb(NULL);
    if (ps.sock < 0) {
        plog("show_player: stream_open FAILED");
        player_startup_abort();
        adec_stop();
        audio_close();
        vdec_close();
        thumb_cache_init();
        show_error(stream_last_error(), url);
        ui_restore_rsx_state();
        return;
    }
    plog("show_player: stream_open OK");
    lc_logf("stream CONNECTED sock=%d", ps.sock);
    crash_log("p7 stream_open OK");

    player_startup_step(item->name, "Buffering", "Streaming... START=stop");

    video_reset();
    display_diag_reset();   // this session's frame rate, not the last one's
    // Clear any avsync state (incl. the video-PTS base correction) left over
    // from a previous playback session so the first frame of THIS stream
    // re-latches cleanly.  Seeks/track-changes reset it via avsync_reset() in
    // the seek path; this covers session start.
    avsync_reset();

    if (!jbuf_alloc(ps.req_w, ps.req_h)) {
        plog("show_player: jbuf_alloc FAILED");
        player_startup_abort();
        netClose(ps.sock);
        adec_stop();
        audio_close();
        vdec_close();
        thumb_cache_init();
        ui_restore_rsx_state();
        return;
    }
    {
        char buf[72];
        snprintf(buf, sizeof(buf), "jbuf: %d slots %ux%u ~%u KB each",
                 jbuf_cap(), ps.req_w, ps.req_h,
                 vid_frame_bytes(ps.req_w, ps.req_h) / 1024);
        plog(buf);
    }
    for (int i = 0; i < jbuf_cap(); i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "jbuf_addr: slot=%d ptr=%p", i, (void*)jbuf_slot_ptr(i));
        plog(buf);
    }
    crash_log("p8 jbuf alloc OK");

    // ---- Compressed read-ahead ring ----
    // Claimed after the jitter buffer, from whatever is left: this is the
    // buffer that decides whether a server delivering its transcode unevenly
    // stutters or not, and compressed seconds are ~75x cheaper per second
    // than decoded ones.  Half of free memory, leaving the rest for the UI,
    // the HUD and the decode path; the ring clamps itself to its own bounds
    // and halves down if the heap cannot manage the ask.
    {
        u32 total = 0, avail = 0;
        u32 want = 6u * 1024u * 1024u;          // if meminfo is unavailable
        // Three quarters, not half: the jitter buffer handed ~18 MB back
        // and this is where it does the most good.  Bounded by
        // RING_BYTES_MAX and halved down on allocation failure.
        if (meminfo_get(&total, &avail)) want = (avail / 4u) * 3u;
        decode_ring_alloc(want);
    }

    // Video GPU blit init — allocate RSX buffers once per session
    vid_gpu_init(jbuf_fw(), jbuf_fh());

    // 5 ms socket receive timeout keeps the network thread responsive
    { struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 5000;   // the lv2 layout (16 bytes)
      netSetSockOpt(ps.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }

    // The button always reads "AUDIO" — track names are too long for the HUD
    // row; the selected track is plogged when cycled.
    // The audio chip names the track ("English \xC2\xB7 DTS-HD MA \xC2\xB7 5.1"),
    // in the version selector's words; it used to read AUDIO forever.
    {
        char w[64] = "";
        if (ps.have_tracks && ps.cur_audio >= 0 && ps.cur_audio < ps.tracks.n_audio)
            vpick_audio_words(ps.tracks.audio[ps.cur_audio].label, w, sizeof w);
        hud_init(ps.total_secs, (g_spine_on && w[0]) ? w : NULL);   // the old HUD keeps AUDIO
    }
    hud_set_title(item->name);
    plog("hud: prewarm start");
    ttf_prewarm_hud();
    plog("hud: prewarm done");

    // ---- Pre-fill: decode JBUF_PREFILL frames before display starts ----
    plog("jbuf: pre-fill start");
    player_prefill(&ps, true, 0x7FFFFFFF);

    if (!ps.playing) {
        lc_logf("show_player: RETURN to UI -- prefill ended playback (before 24p)");
        player_startup_abort();
        vid_gpu_free();
        decode_ring_free();
        jbuf_free();
        netClose(ps.sock);
        adec_stop();
        audio_close();
        vdec_close();
        thumb_cache_init();
        return;
    }

    plog("jbuf: pre-fill done — starting threads");
    timing_register_vblank();

    s_lc_ps = &ps;
    lc_session_snapshot("before_decode_spawn", true);

    // ---- Spawn decode thread ----
    bool dec_ok;
    {
        dec_ok = player_spawn_decode(&ps);
        lc_logf("playback: decode thread %s playing=%d",
                dec_ok ? "STARTED" : "NOT started", (int)ps.playing);
    }

    // ---- Physical 24Hz output for 24fps film ----
    // The frame rate is known (the prefill decoded frames) and nothing is
    // presenting yet.  This runs AFTER the decode thread starts, on purpose:
    // the first hardware run did it before, nothing read the socket for the
    // ~20 s of switch + confirmation, and playback ended straight after the
    // revert.  Now the stream keeps flowing into the read-ahead ring while
    // the display changes.  The decode thread never touches the GPU or the
    // display mode, so it is safe alongside the switch.  Off unless
    // jellyfin_24p.txt says 1; every gate and the measurement that verifies
    // the switch are in display_24p.cpp.
    if (dec_ok && ps.playing) {
        s_d24_title = item->name;
        lc_session_snapshot("before_24p", false);
        const d24_ui ui = { d24_draw_prompt, d24_poll_answer, d24_lifecycle };
        const bool d24_ok = d24_session_begin(&ui);
        lc_logf("24p: session_begin returned %s", d24_ok ? "SWITCHED" : "not switched");
        lc_session_snapshot("after_24p", false);
        init_btns();
    }

    // ---- Pre-roll: fill the read-ahead ring before the picture starts ----
    //
    // The jitter buffer is full by now, which is only ~0.5 s at 1080p.  The
    // decode thread keeps reading past that into the compressed ring, and
    // starting playback before that ring has something in it throws away the
    // whole point of having one: the first hard scene arrives with no reserve
    // and stutters, exactly as it did before the ring existed.
    //
    // So wait for it here.  Waiting a few seconds up front to play a film
    // through without stalling is a trade worth making, and the wait is
    // bounded: it gives up at the deadline, on Circle, or the moment the
    // stream ends.  Nothing is lost by the timeout — playback simply starts
    // with whatever was buffered.
    if (ps.playing && decode_ring_cap() > 0) {
        // 90%, was 75%.  The ring is the only thing standing between a slow
        // patch and a stall, so fill it as far as it will go before starting.
        // The wait stays bounded by the deadline below and by Circle-to-skip.
        const int target   = (decode_ring_cap() * 9) / 10;  // 90% full
        const u64 deadline = timing_get_us() + 90000000ULL; // 90 s ceiling
        u64 last_draw_us   = 0;
        int last_pct       = -1;
        plog("preroll: filling read-ahead ring");
        init_btns();
        if (buffering_active()) buffering_step("Buffering", true, "Start now");
        while (running && ps.playing && decode_ring_fill() < target &&
               timing_get_us() < deadline) {
            sysUtilCheckCallback();
            poll_buttons();
            if (BTN_PRESSED(circle)) { plog("preroll: skipped by user"); break; }
            // The audio queue can fill before the ring does (HD audio is
            // several Mbps), and the decode thread stops reading when it
            // does.  Waiting past that point achieves nothing.
            if (!adec_pes_queue_hungry()) {
                plog("preroll: audio queue full, starting");
                break;
            }

            // Redraw at ~4 Hz: the decode thread owns the socket, so this
            // loop is doing nothing but showing progress.
            u64 now = timing_get_us();
            int pct = decode_ring_fill() * 100 / (decode_ring_cap() ? decode_ring_cap() : 1);
            if (buffering_active()) {
                // The presentation animates for the whole wait: ~30 fps,
                // paced by its own flips, with the fill as its progress.
                buffering_progress((float)pct / 90.0f);
                buffering_frame_paced(33000ULL);
            } else if (pct != last_pct && now - last_draw_us > 250000ULL) {
                last_draw_us = now;
                last_pct     = pct;
                char msg[64];
                snprintf(msg, sizeof(msg), "Buffering... %d%%   (O: start now)",
                         pct * 100 / 90 > 100 ? 100 : pct * 100 / 90);
                player_status_screen(item->name, msg);
            }
            usleep(20000);
        }
        {
            char b[80];
            snprintf(b, sizeof(b), "preroll: done ring=%d/%d packets",
                     decode_ring_fill(), decode_ring_cap());
            plog(b);
        }
        lc_session_snapshot("after_preroll", false);
        init_btns();
    }
    // Ready (or skipped with O, or the deadline): close the ring and fade.
    // The decode thread keeps filling the ring while the outro plays.
    if (buffering_active()) buffering_finish(ps.playing);

    // ---- Spawn audio thread ----
    AudioCtx         aud_ctx = { &ps.playing, &ps.paused };
    sys_ppu_thread_t aud_tid = 0;
    if (ps.playing) {
        int trc = sysThreadCreate(&aud_tid, audio_thread_fn,
                                  (void *)&aud_ctx,
                                  700, 32 * 1024,
                                  THREAD_JOINABLE, "jf_audio");
        if (trc != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "show_player: aud thread_create FAILED rc=%d", trc);
            plog(buf);
            ps.playing = false;
        }
    }

    // ---- Spawn upload thread — memcpy jbuf→RSX-local off the display thread ----
    UploadCtx        upl_ctx = { &ps.playing, jbuf_fw(), jbuf_fh() };
    sys_ppu_thread_t upl_tid = 0;
    if (ps.playing) {
        int trc = sysThreadCreate(&upl_tid, upload_thread_fn,
                                  (void *)&upl_ctx,
                                  850, 32 * 1024,
                                  THREAD_JOINABLE, "jf_upload");
        if (trc != 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "show_player: upl thread_create FAILED rc=%d", trc);
            plog(buf);
            ps.playing = false;
        }
    }

    // ---- Spawn progress reporter — keeps server resume position current ----
    // It sends the /Sessions/Playing start report itself.  Sent from here it
    // blocked the display loop while the audio thread, already spawned, was
    // playing: a 6 s timeout put the first picture 4.9 s behind the sound
    // (hardware, 2026-09-27).
    // Detached, and fed only through g_prog (player_internal.h): it may still
    // be inside a slow report after this function returns.
    g_prog.playing   = false;
    g_prog.gen       = g_prog.gen + 1;
    snprintf(g_prog.item, sizeof g_prog.item, "%s", item->id);
    snprintf(g_prog.sess, sizeof g_prog.sess, "%s", ps.session_id);
    g_prog.base_us   = ps.play_base_us;
    g_prog.paused    = ps.paused;
    g_prog.pos_valid = true;
    __sync_synchronize();
    g_prog.playing   = ps.playing;
    sys_ppu_thread_t prog_tid = 0;
    if (ps.playing) {
        int trc = sysThreadCreate(&prog_tid, progress_thread_fn,
                                  (void *)(uintptr_t)g_prog.gen,
                                  1100, 16 * 1024,
                                  0, "jf_progress");
        if (trc != 0) {
            plog("show_player: progress thread_create failed (non-fatal)");
            prog_tid = 0;
        }
    }
    if (!prog_tid)
        jellyfin_report_playing(item->id, ps.session_id, ps.play_base_us * 10ULL);

    crash_log("p9 threads started");
    lc_logf("playback: START aud=%d upl=%d prog=%d playing=%d",
            aud_tid != 0, upl_tid != 0, prog_tid != 0, (int)ps.playing);
    slog_state("PLAYBACK_STARTED item_id=%s name=%.40s total=%us "
               "resume=%us w=%u h=%u",
               item->id, item->name, ps.total_secs,
               (unsigned)(ps.play_base_us / 1000000ULL), ps.req_w, ps.req_h);

    crash_log("p10 loop");
    // Paused-idle gate state (see below).  flip_queued: whether the previous
    // iteration queued a flip — waitflip() blocks forever if none is pending.
    // Start from a drained pipeline: the buffering screen and the 24p prompt
    // flipped outside this loop, and a wait that tracks the wrong flip keeps
    // the loop a frame ahead for the whole session (see player_flip_resync).
    player_flip_resync();
    bool flip_queued  = false;
    bool was_paused   = false;
    int  pause_settle = 0;   // frames still to draw after a pause-state change
    bool lc_first_frame = false;

    // ---- Main (display) loop ----
    while (running && ps.playing && !s_vdec_error) {
        // Bounded: a display mode change (the 24p check's revert) can leave
        // no flip pending, and an unbounded wait then froze playback on a
        // black screen for good (hardware, 2026-09-27).  A normal flip lands
        // within one vblank, so 250 ms only ever fires in that case.
        if (flip_queued && !waitflip_timeout(250000)) {
            static bool s_logged = false;
            if (!s_logged) { plog("player: flip wait timed out; continuing"); s_logged = true; }
        }
        flip_queued = false;
        sysUtilCheckCallback();

        static u64 s_loop_iter_us   = 0;
        static u64 s_loop_frame_dur = 0;
        { u64 now = timing_get_us();
          if (s_loop_iter_us) s_loop_frame_dur = now - s_loop_iter_us;
          s_loop_iter_us = now; }

        poll_buttons();
        bool l2_pressed = BTN_PRESSED(l2);
        bool r2_pressed = BTN_PRESSED(r2);
        if (BTN_PRESSED(start)) {
            plog("playing=0 reason=user_stop");
            user_stopped = true;
            ps.playing = false;
        }

        // End-of-item auto-advance: within the last 90 s of a followed item
        // (or once its credits start), show the NEXT popup; SELECT ends this
        // session with the next-request flag set so the UI starts the
        // follower.  Episodes also count down 25 s -- from the start of the
        // credits when the server marks them (within the last 10 minutes),
        // else through the last 25 s -- and fire the request at zero.
        bool next_popup = false;
        int  auto_secs  = -1;
        in_auto_window  = false;
        if (have_next && ps.total_secs > 90) {
            const s64 pos   = (s64)((ps.play_base_us + audio_get_clock_us()) / 1000000ULL);
            const s64 total = (s64)ps.total_secs;
            const double outro = segments_outro_start();
            s64 cd_start = total - 25;
            if (outro > 0.0 && (s64)outro < cd_start && (s64)outro + 600 >= total)
                cd_start = (s64)outro;
            next_popup = (pos + 90 >= total) || pos >= cd_start;
            if (next_popup && auto_next && pos >= cd_start) {
                s64 rem = cd_start + 25 - pos;
                if (total - pos < rem) rem = total - pos;
                auto_secs = rem > 0 ? (int)rem : 0;
                in_auto_window = true;
            }
        }
        if (next_popup && BTN_PRESSED(select)) {
            plog("playing=0 reason=next_item");
            s_next_requested = true;
            ps.playing = false;
        }
        if (auto_secs == 0 && ps.playing) {
            plog("playing=0 reason=next_auto");
            s_next_requested = true;
            ps.playing = false;
        }
        if (!ps.playing) break;

        // The HUD owns the D-pad (focus navigation) and X (activates the
        // focused control: play/pause, REW/FF, AUDIO, or CC), returning the
        // action to perform.
        const double seg_pos = (double)(ps.play_base_us + audio_get_clock_us()) / 1e6;
        const MediaSegment *skip_seg = ps.paused ? NULL : segments_at(seg_pos);
        hud_set_skip_offered(skip_seg != NULL);
        HudAction act = hud_handle_input(l2_pressed, r2_pressed, ps.paused);
        // Settings > Auto Skip: intros and recaps go by themselves.  Credits
        // stay: they run the next episode's countdown.
        if (act == HUD_ACTION_NONE && skip_seg && skip_seg->type != SEG_OUTRO &&
            autoskip_enabled()) {
            plog("segments: auto skip");
            act = HUD_ACTION_SKIP_SEGMENT;
        }
        if (act == HUD_ACTION_SKIP_SEGMENT && skip_seg) {
            segments_skipped();
            // Credits with a next item queued: go straight to it, as SELECT
            // on the NEXT badge does.  Otherwise jump to the segment's end.
            if (skip_seg->type == SEG_OUTRO && have_next) {
                plog("playing=0 reason=skip_credits_next");
                s_next_requested = true;
                ps.playing = false;
                break;
            }
            char line[80];
            snprintf(line, sizeof(line), "segments: skip %.1f -> %.1f s",
                     seg_pos, skip_seg->end_secs);
            plog(line);
            player_seek_queue_tap(&ps, (int)(skip_seg->end_secs - seg_pos + 0.999));
            act = HUD_ACTION_NONE;
        }
        if (act == HUD_ACTION_STOP) {          // O on the redesigned HUD
            plog("playing=0 reason=user_stop_circle");
            user_stopped = true;
            ps.playing = false;
            break;
        }

        // D-pad / focus-mode taps come through the HUD; queue them like any
        // tap.  R2/L2 are NOT handled here — the seek input machine owns them,
        // so their flickery digital edge can't fire spurious taps.
        if (act == HUD_ACTION_SEEK) {
            player_seek_queue_tap(&ps, hud_seek_delta());
            act = HUD_ACTION_NONE;
        }

        // AUDIO / CC popup menus; a track change comes back as a 0-delta
        // HUD_ACTION_SEEK — the reopen applies the new track.
        act = player_handle_menu_action(&ps, act);

        // HD mode, undecodable copied track: either a DTS-HD MA / DTS:X track
        // with no backward-compatible core (legal, and nothing on this
        // platform decodes the extension substreams), or a TrueHD track the
        // decoder cannot make sense of at all.  Either way no audio is coming
        // out.  Veto the copy path for the rest of the session and reopen at
        // the current position — the same 0-delta reopen a track change uses
        // — which re-negotiates the stream as an AC-3 5.1 transcode.  Silence
        // is not an acceptable resting state.
        if (act == HUD_ACTION_NONE && surround_hd_preferred() &&
            (adec_dts_no_core() || adec_truehd_no_audio())) {
            const bool dts = adec_dts_no_core();
            plog(dts ? "dts: no core substream in this track, falling back to AC-3"
                     : "truehd: no decodable audio in this track, falling back to AC-3");
            slog_state("HD_UNDECODABLE codec=%s fallback=ac3",
                       dts ? "dts" : "truehd");
            surround_hd_session_disable();
            act = HUD_ACTION_SEEK;      // 0-delta reopen applies the fallback
        }

        // R2/L2 tap/hold machine + commit gate.
        act = player_seek_input_update(&ps, act);

        if (act == HUD_ACTION_TOGGLE_PAUSE) {
            ps.paused = !ps.paused;
            { sys_ppu_thread_t cur_tid = 0;
              sysThreadGetId(&cur_tid);
              char buf[128];
              snprintf(buf, sizeof(buf),
                  "hud: %s clk=%lluus tid=%llu fr_dur=%lluus",
                  ps.paused ? "paused" : "resumed",
                  (unsigned long long)audio_get_clock_us(),
                  (unsigned long long)cur_tid,
                  (unsigned long long)s_loop_frame_dur);
              plog(buf); }
            slog_state("PLAYBACK_%s pos=%llus",
                       ps.paused ? "PAUSED" : "RESUMED",
                       (unsigned long long)((ps.play_base_us +
                           audio_get_clock_us()) / 1000000ULL));
        } else if (act == HUD_ACTION_SEEK) {
            g_prog.pos_valid = false;
            const bool seek_ok = player_execute_seek(&ps);
            snprintf(g_prog.sess, sizeof g_prog.sess, "%s", ps.session_id);
            g_prog.base_us   = ps.play_base_us;
            __sync_synchronize();
            g_prog.pos_valid = seek_ok;
            if (!seek_ok) break;
            // The reopen's spinner frames flipped mid-iteration, so the one
            // this loop's wait tracks is not the last one queued.  Drain the
            // GPU (its label sits behind every queued flip's wait) and start
            // the count clean, or the loop runs a frame ahead for good.
            player_flip_resync();
        }

        // Paused-idle gate.  While paused the seek bar stays up and every
        // redrawn frame is pixel-identical, yet redrawing costs the GPU
        // clear + video quad, three rsxSync stalls, and CPU alpha-blended
        // text into RSX VRAM — at 60 Hz that churn is what makes the whole
        // player lag whenever the bar is visible.  Draw only when something
        // can have changed (input, seek/scrub activity, a landed paused
        // seek, or the pause state itself); otherwise sleep one vblank and
        // just keep polling input.  Playback (!paused) is never gated.
        if (ps.paused != was_paused) { was_paused = ps.paused; pause_settle = 2; }
        bool any_input =
            btn_cur.up || btn_cur.down || btn_cur.left || btn_cur.right ||
            btn_cur.cross || btn_cur.circle || btn_cur.square || btn_cur.triangle ||
            btn_cur.start || btn_cur.select ||
            btn_cur.l1 || btn_cur.r1 || btn_cur.l2 || btn_cur.r2;
        if (ps.paused && !any_input && act == HUD_ACTION_NONE &&
            !ps.show_seek_frame && ps.seek.pending_secs == 0 &&
            ps.seek.state == SEEK_IDLE && pause_settle == 0) {
            usleep(16000);
            continue;
        }
        if (pause_settle > 0) pause_settle--;

#if BUILD_FOR_RPCS3
        // AV-sync heartbeat (~1 Hz) — the app already measures drift, so the
        // choppy-audio / desync bug is visible in the log as diff_us climbing
        // or jbuf underrunning, without needing to eyeball a screenshot.
        {
            static u64 s_avsync_next_us = 0;
            u64 hb_now = timing_get_us();
            if (!ps.paused && hb_now >= s_avsync_next_us) {
                s_avsync_next_us = hb_now + 1000000ULL;
                slog_state("AVSYNC diff_us=%lld locked=%d jbuf=%d pos=%llus "
                           "frame=%d",
                           (long long)avsync_get_smoothed_diff(),
                           (int)avsync_is_locked(), jbuf_count(),
                           (unsigned long long)((ps.play_base_us +
                               audio_get_clock_us()) / 1000000ULL),
                           ps.frame_count);
            }
        }
#endif

        g_prog.paused = ps.paused;
        player_display_frame(&ps);
        {
            // No new picture for 0.4 s while playing -- a stall, a seek, the
            // network: the spinner says so instead of a frozen frame.
            static int s_sp_fr = -1; static u64 s_sp_us = 0;
            const u64 now_sp = timing_get_us();
            // A pause restarts the timer too (the idle gate skips this block
            // while paused), or every resume flashed the spinner.
            static bool s_sp_paused = false;
            if (ps.frame_count != s_sp_fr || ps.frame_count == 0 || ps.paused || s_sp_paused) {
                s_sp_fr = ps.frame_count; s_sp_us = now_sp;
            }
            s_sp_paused = ps.paused;
            if (!ps.paused && ps.frame_count > 0 && ps.seek.state != SEEK_SCRUB &&
                now_sp - s_sp_us > 400000ULL)
                player_spinner_gpu((float)(now_sp - s_sp_us) * 1.0e-6f);
        }
        if (!lc_first_frame && ps.frame_count > 0) {
            lc_first_frame = true;
            lc_logf("playback: first frame displayed fr=%d", ps.frame_count);
        }

        if (next_popup && ps.frame_count > 0) {
            // Fence the in-flight video draw before CPU framebuffer writes,
            // same as the HUD overlay does.
            rsxSync();
            player_draw_next_popup(auto_secs);
        }
        if (ps.frame_count > 0 && !hud_is_visible() && !ps.paused) {
            const MediaSegment *seg = segments_at(
                (double)(ps.play_base_us + audio_get_clock_us()) / 1e6);
            if (seg) {
                if (!next_popup) rsxSync();
                player_draw_skip_badge(media_segment_skip_label(seg->type));
            }
        }

        flip();
        flip_queued = true;
    }

    crash_log("p11 loop exit");
    {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "show_player: loop exit running=%u playing=%d vdec_err=%d fr=%d",
            running, (int)ps.playing, (int)s_vdec_error, ps.frame_count);
        plog(buf);
        // Which of the loop's three conditions ended it.  "playing cleared"
        // is set by another thread or a helper; its own "playing=0 reason="
        // line just before this one says which.
        lc_logf("playback: STOP cause=%s user_stop=%d next=%d frames=%d",
                !running        ? "sysutil_exit(running=0)"
              : s_vdec_error    ? "vdec_error"
              : user_stopped    ? "user_stop"
              : s_next_requested ? "next_item"
              : "playing_cleared(see playing=0 reason=)",
                (int)user_stopped, (int)s_next_requested, ps.frame_count);
    }

    // Transcoded streams often run slightly short of RunTimeTicks, so EOF
    // can land before the countdown reaches zero.  If playback ended on its
    // own inside the countdown window, still advance to the next episode.
    if (in_auto_window && !s_next_requested && !user_stopped &&
        !ps.playing && running && !s_vdec_error) {
        plog("next_auto: eof inside countdown window");
        s_next_requested = true;
    }

    timing_shutdown();
    segments_stop();
    hud_shutdown();

    // Final position for the server's resume bookmark — read before the
    // audio clock is torn down.
    u64 final_pos_ticks = (ps.play_base_us + audio_get_clock_us()) * 10ULL;

    // Signal all threads to stop, join in order: decode → audio → upload
    ps.playing = false;
    g_prog.playing = false;          // the reporter is detached: not joined
    usleep(16000);

    if (ps.dec_tid) {
        u64 tret;
        sysThreadJoin(ps.dec_tid, &tret);
        plog("show_player: decode thread joined");
    }
    if (aud_tid) {
        u64 tret;
        sysThreadJoin(aud_tid, &tret);
        plog("show_player: audio thread joined");
    }
    if (upl_tid) {
        u64 tret;
        sysThreadJoin(upl_tid, &tret);
        plog("show_player: upload thread joined");
    }
    crash_log("p12 threads joined");

    // The stream first: nothing reads it any more, and while it is open the
    // server keeps filling its receive buffer -- the two requests below then
    // starve on the network pool (see exit_reports_start).
    netClose(ps.sock);

    // Back to the mode the session started in (24p output), before any UI --
    // the Returning screen below included -- draws again.
    d24_session_end();

    // What this episode was watched with, for the next one (player.h).
    if (ps.have_tracks && ps.cur_audio >= 0 && ps.cur_audio < ps.tracks.n_audio) {
        snprintf(s_chain_label, sizeof s_chain_label, "%s", ps.tracks.audio[ps.cur_audio].label);
        audio_lang(s_chain_label, s_chain_lang, sizeof s_chain_lang);
    }
    snprintf(s_chain_src, sizeof s_chain_src, "%s", ps.source.label);

    // The Returning screen (spine gate): the buffering screen's look -- the
    // item's backdrop under a veil, the Jellyfin mark and ring -- saying
    // RETURNING while the reports and the teardown run.  It takes over from
    // the player's last frame, whose flip is still pending.
    // Not between episodes: an auto-advance goes straight on to the next
    // one's buffering screen (and that playback joins these reports first).
    const bool ret_screen = g_spine_on && running && !s_next_requested;
    if (ret_screen) {
        if (flip_queued) { waitflip_timeout(250000); flip_queued = false; }
        ui_restore_rsx_state();
        buffering_begin(item->id, item->name);
        buffering_step("Returning", false, NULL);
        buffering_frame();
    }
    const u64 ret_t0 = timing_get_us();
    lc_logf("play-session DESTROY psid=%s pos=%llus (report_stopped + stop_transcode)",
            ps.session_id[0] ? ps.session_id : "(none)",
            (unsigned long long)(final_pos_ticks / 10000000ULL));

    // Tell the server where we stopped (also finalizes Continue Watching) and
    // stop its transcode -- on a thread, so a slow server never freezes this.
    exit_reports_start(item->id, ps.session_id, final_pos_ticks);

    // Free video GPU blit resources before releasing the jitter buffer
    vid_gpu_free();
    returning_pump(ret_screen);

    crash_log("p17 jbuf_free begin");
    decode_ring_free();
    jbuf_free();
    crash_log("p18 jbuf_free OK");
    returning_pump(ret_screen);
    adec_stop();
    crash_log("p15 audio_close begin");
    audio_close();
    crash_log("p16 audio_close OK");
    returning_pump(ret_screen);
    crash_log("p13 vdec_close begin");
    vdec_close();
    crash_log("p14 vdec_close OK");
    returning_pump(ret_screen);

    thumb_cache_init();
    ui_restore_rsx_state();
    // Cues belong to the title that was playing; the table itself is kept
    // for the next one (see subtitles.cpp) but its contents must not outlive
    // this playback.
    subs_clear();

    // Wait for the reports -- Continue Watching is only right once Stopped
    // has landed -- but never for long: a sick network gets 2.5 s, then the
    // UI comes back and the thread finishes on its own (the next playback
    // joins it first).
    {
        const u64 cap = s_next_requested ? 0ULL
                      : ret_screen       ? 2500000ULL : 6000000ULL;
        while (!s_exit_rep_done && timing_get_us() - ret_t0 < cap) {
            if (ret_screen) returning_pump(true);
            else            usleep(10000);
        }
        if (s_exit_rep_done) exit_reports_join();
        char b[96];
        snprintf(b, sizeof b, "show_player: exit took %llu ms (reports %s)",
                 (unsigned long long)((timing_get_us() - ret_t0) / 1000),
                 s_exit_rep_done ? "done" : "still running");
        plog(b);
    }
    if (ret_screen) {
        buffering_finish(false);     // quick fade; leaves its last flip pending
        flip_queued = true;
    }
    crash_log("p19 done");
    plog("show_player: done");
    lc_logf("show_player: RETURN to UI (caller's screen, e.g. Home) frames=%d", ps.frame_count);
    s_lc_ps = NULL;
    slog_state("PLAYBACK_STOPPED reason=%s frames=%d vdec_err=%d",
               user_stopped     ? "user_stop"
             : s_next_requested ? "next_item"
             : s_vdec_error     ? "vdec_error" : "eof",
               ps.frame_count, (int)s_vdec_error);
}
