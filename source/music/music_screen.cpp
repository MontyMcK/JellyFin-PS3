// Now Playing — the music player screen (see the mockup this implements):
// big glowing album art on the left, the audio-reactive bar visualizer
// above the track title, artist in accent violet, album/year and source
// info lines, an UP NEXT queue on the right, transport icons, and a seek
// bar.  SELECT opens a queue overlay for jumping straight to a track.
//
// Runs its own blocking frame loop like the video player, but keeps the
// XMB's animated wave background so the screen still feels like the rest
// of the app.  All drawing here is CPU-phase (after rsxSync).

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <sysutil/sysutil.h>

#include "ui_internal.h"
#include "ui_wave.h"
#include "music_screen.h"
#include "music_player.h"
#include "music_fft.h"
#include "plog.h"
#include "timing.h"

// -------------------------------------------------------
// Small local helpers
// -------------------------------------------------------

static void fmt_mmss(char *out, int cap, u32 secs) {
    snprintf(out, cap, "%u:%02u", secs / 60, secs % 60);
}

// Text clipped to max_w with ".." (local copy of the grid's helper).
static void draw_clipped(u32 x, u32 y, const char *text, float px,
                         u32 color, int max_w, bool bold = false) {
    if (ttf_text_width(text, px, bold) <= max_w) {
        drawTTF(x, y, text, px, color, bold);
        return;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", text);
    int len = (int)strlen(buf);
    while (len > 1) {
        buf[--len] = '\0';
        char trial[132];
        snprintf(trial, sizeof(trial), "%s..", buf);
        if (ttf_text_width(trial, px, bold) <= max_w) {
            drawTTF(x, y, trial, px, color, bold);
            return;
        }
    }
}

// Opaque filled circle — CPU row fills (write-only, fast).
static void fill_circle(int cx, int cy, int r, u32 color) {
    for (int dy = -r; dy <= r; dy++) {
        int hw = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        if (hw <= 0) continue;
        drawRect((u32)(cx - hw), (u32)(cy + dy), (u32)(hw * 2), 1, color);
    }
}

// 1px circle outline, alpha-blended — the play button's soft glow rings.
// Blended pixels read the framebuffer back, so keep these thin.
static void ring_blend(int cx, int cy, int r, u32 color, u8 alpha) {
    int px = cx + r, py = cy;
    for (int a = 1; a <= 64; a++) {
        float ang = (float)a * (2.0f * 3.14159265f / 64.0f);
        int nx = cx + (int)(cosf(ang) * (float)r + 0.5f);
        int ny = cy + (int)(sinf(ang) * (float)r + 0.5f);
        if (nx != px || ny != py)
            drawRectBlend((u32)nx, (u32)ny, 1, 1, color, alpha);
        px = nx; py = ny;
    }
    (void)py;
}

// Four-segment breadcrumb (the shared helper caps at three).
static void draw_breadcrumb4(int x, int y, const char *a, const char *b,
                             const char *c, const char *leaf) {
    const float px = 15.0f;
    const char *parts[4] = { a, b, c, leaf };
    for (int i = 0; i < 4; i++) {
        if (!parts[i]) continue;
        bool is_leaf = (i == 3);
        drawTTF((u32)x, (u32)y, parts[i], px,
                is_leaf ? 0x00C5CBE3UL : XMB_TEXT_FAINT);
        x += ttf_text_width(parts[i], px);
        if (!is_leaf) {
            drawIcon((u32)(x + 4), (u32)(y - 1), 0xE5CC, 16.0f, XMB_TEXT_FAINT);
            x += 24;
        }
    }
}

// -------------------------------------------------------
// The visualizer — the circled part of the mockup, with more and longer
// bars as requested: MUSIC_VIZ_BANDS columns, up to VIZ_H px tall, violet
// with a lighter cap so the peaks read at TV distance.
// -------------------------------------------------------

static void draw_visualizer(int x, int baseline, int width, int max_h) {
    float bands[MUSIC_VIZ_BANDS];
    music_viz_bands(bands);

    int gap = 3;
    int bw  = (width - gap * (MUSIC_VIZ_BANDS - 1)) / MUSIC_VIZ_BANDS;
    if (bw < 4) bw = 4;

    for (int i = 0; i < MUSIC_VIZ_BANDS; i++) {
        int h = 3 + (int)(bands[i] * (float)(max_h - 3));
        int bx = x + i * (bw + gap);
        int by = baseline - h;
        drawRect((u32)bx, (u32)by, (u32)bw, (u32)h, XMB_ACCENT);
        // Lighter 2px cap on any bar with real energy.
        if (h > 6)
            drawRect((u32)bx, (u32)by, (u32)bw, 2, 0x00C4B5F7UL);
    }
}

// -------------------------------------------------------
// Playback context — breadcrumb + meta lines for the queue's origin
// (album, artist/genre album, playlist, or the flat Songs list).
// -------------------------------------------------------

typedef struct {
    char parent[64];    // breadcrumb: "Albums" / "Songs" / artist / genre...
    char title[128];    // album or playlist name ("" for the Songs list)
    char year[8];       // shown after the title when known
    char genre[32];     // meta-line genre
} MusicCtx;

// -------------------------------------------------------
// Queue overlay (SELECT) — the whole queue in PLAY order (so a shuffle
// re-orders it live), full height, X jumps to the highlighted position.
// -------------------------------------------------------

static bool s_q_open   = false;
static int  s_q_sel    = 0;      // play-order position
static int  s_q_scroll = 0;

// D-pad focus: two zones.  TRANSPORT is the control row (0 rewind, 1 prev,
// 2 play/pause, 3 next, 4 ffwd, 5 shuffle) — left/right move, X activates.
// UP from there enters QUEUE — the Up Next list on the right — where
// up/down scroll the full remaining queue, X plays the highlighted track,
// LEFT/circle drop back to the transport.
enum { FZ_TRANSPORT = 0, FZ_QUEUE = 1 };
static int s_fzone   = FZ_TRANSPORT;
static int s_t_focus = 2;
static int s_u_sel    = 0;   // QUEUE zone: selected play-order position
static int s_u_scroll = 0;   // QUEUE zone: first visible position
static bool s_swallow_left = false;   // eat the held LEFT that exited QUEUE

// Rows that fit the Up Next list (54 px per entry, stopping above the
// seek bar's time labels).
static int uq_vis_rows(void) {
    int ey0 = (int)(display_height * 0.18f) + 34;
    int bot = (int)(display_height * 0.895f) - 16;
    int n   = (bot - ey0) / 54;
    return n < 1 ? 1 : n;
}

// -------------------------------------------------------
// Seek tap batching — the video player's model: each REW/FF tap moves the
// bar immediately and stretches a ~0.9 s gate; the stream only reopens
// once, after the taps stop.  While the restart is in flight the bar keeps
// showing the target instead of snapping back to the stale position.
// -------------------------------------------------------

static int s_seek_pend       = 0;    // accumulated ±10 s, not yet committed
static u64 s_seek_gate_us    = 0;    // commit when now passes this
static int s_seek_hold       = -1;   // committed target shown until caught
static u64 s_seek_hold_until = 0;
static int s_last_pos        = -1;   // track-change detector

static void seek_tap(int dir) {
    // Pin at the track edges so a long hold can't wind up a huge pending
    // jump past 0:00 or the end.
    int target = (int)music_elapsed_secs() + s_seek_pend + dir * 10;
    u32 dur    = music_duration_secs();
    if (target < -5) return;
    if (dur > 0 && target > (int)dur + 5) return;
    s_seek_pend   += dir * 10;
    s_seek_gate_us = timing_get_us() + 900000ULL;
}

// Hold-to-scrub on L2/R2: fires on the initial press, then ~4x/sec while
// held (the shared BTN_REPEAT machine only covers the d-pad).  The commit
// gate keeps stretching while ticks arrive, so the whole hold still costs
// a single stream reopen after release.
static bool seek_hold_tick(bool held, int idx) {
    static u64 s_next_us[2] = { 0, 0 };
    u64 now = timing_get_us();
    if (!held) { s_next_us[idx] = 0; return false; }
    if (s_next_us[idx] == 0) {                    // fresh press
        s_next_us[idx] = now + 350000ULL;
        return true;
    }
    if (now >= s_next_us[idx]) {                  // held: steady ticks
        s_next_us[idx] = now + 240000ULL;
        return true;
    }
    return false;
}

// Runs every frame: commit quiet taps, drop stale display state.
static void seek_update(void) {
    int pos = music_current_pos();
    if (pos != s_last_pos) {
        s_last_pos  = pos;
        s_seek_pend = 0;
        s_seek_hold = -1;
    }
    u64 now = timing_get_us();
    if (s_seek_pend != 0 && now >= s_seek_gate_us) {
        int tgt = (int)music_elapsed_secs() + s_seek_pend;
        u32 dur = music_duration_secs();
        if (tgt < 0) tgt = 0;
        if (dur > 0 && tgt > (int)dur - 1) tgt = (int)dur - 1;
        music_seek(s_seek_pend);
        s_seek_pend       = 0;
        s_seek_hold       = tgt;
        s_seek_hold_until = now + 5000000ULL;
    }
    if (s_seek_hold >= 0) {
        int d = (int)music_elapsed_secs() - s_seek_hold;
        if (d < 0) d = -d;
        if (d <= 2 || now >= s_seek_hold_until) s_seek_hold = -1;
    }
}

// Position the seek bar / time label should display right now.
static int seek_display_secs(void) {
    int el  = (int)music_elapsed_secs();
    u32 dur = music_duration_secs();
    int disp = el;
    if (s_seek_pend != 0)     disp = el + s_seek_pend;
    else if (s_seek_hold >= 0) disp = s_seek_hold;
    if (disp < 0) disp = 0;
    if (dur > 0 && disp > (int)dur) disp = (int)dur;
    return disp;
}

#define Q_ROW_H 42

// Rows that fit the full-height panel at the current resolution.
static int q_vis_rows(void) {
    int top = XMB_TOPBAR_H + 26;
    int bot = (int)display_height - XMB_BOTTOM_PAD - 6;
    int n   = (bot - top - 56 - 14) / Q_ROW_H;
    return n < 3 ? 3 : n;
}

static void draw_queue_overlay(const MusicTrack *tracks, int count,
                               const char *ctx_title) {
    wave_dim_screen(130);   // GPU dim, fenced — CPU panel lands on top

    const int rows_fit = q_vis_rows();
    const int rows     = count < rows_fit ? count : rows_fit;
    const int mw       = 640;
    const int mh       = 56 + rows * Q_ROW_H + 14;
    int mx = ((int)display_width - mw) / 2;
    int my = XMB_TOPBAR_H + 26;

    drawRect((u32)mx, (u32)my, (u32)mw, (u32)mh, XMB_PANEL);
    drawRect((u32)mx, (u32)my, (u32)mw, 1, XMB_HAIRLINE);
    drawRect((u32)mx, (u32)(my + mh - 1), (u32)mw, 1, XMB_HAIRLINE);
    drawRect((u32)mx, (u32)my, 1, (u32)mh, XMB_HAIRLINE);
    drawRect((u32)(mx + mw - 1), (u32)my, 1, (u32)mh, XMB_HAIRLINE);

    drawTTF((u32)(mx + 26), (u32)(my + 18), "QUEUE", 14, XMB_TEXT_FAINT, true);
    if (music_is_shuffle())
        drawIcon((u32)(mx + 92), (u32)(my + 16), 0xE043, 18.0f, XMB_ACCENT);
    if (ctx_title[0])
        draw_clipped((u32)(mx + 124), (u32)(my + 18), ctx_title, 14,
                     XMB_TEXT_DIM, mw - 240);
    {
        char pos_str[24];
        snprintf(pos_str, sizeof(pos_str), "%d / %d", s_q_sel + 1, count);
        int pw = ttf_text_width(pos_str, 13);
        drawTTF((u32)(mx + mw - 26 - pw), (u32)(my + 19), pos_str, 13,
                XMB_TEXT_DIM);
    }

    int cur_pos = music_current_pos();
    int list_y  = my + 48;
    for (int i = 0; i < rows; i++) {
        int p = s_q_scroll + i;
        if (p >= count) break;
        int orig = music_track_at(p);
        if (orig < 0) break;
        const MusicTrack *t = &tracks[orig];
        int  ry  = list_y + i * Q_ROW_H;
        bool sel = (p == s_q_sel);
        if (sel)
            drawRect((u32)(mx + 12), (u32)ry, (u32)(mw - 24), Q_ROW_H,
                     XMB_PANEL_HI);
        // Playing marker / play-order number (track # when unshuffled).
        if (p == cur_pos)
            drawIcon((u32)(mx + 24), (u32)(ry + 10), 0xE405, 20.0f,
                     XMB_ACCENT);
        else {
            int shown = (!music_is_shuffle() && t->track_num > 0)
                            ? t->track_num : p + 1;
            char num[8];
            snprintf(num, sizeof(num), "%d", shown);
            drawTTF((u32)(mx + 26), (u32)(ry + 12), num, 15, XMB_TEXT_FAINT);
        }
        // Album-art thumb (letter tile while it loads).
        if (!xmb_cpu_blit_thumb(t->art_id, mx + 56, ry + 3, 36, 36))
            xmb_draw_letter_tile(t->id, t->name, mx + 56, ry + 3, 36);
        draw_clipped((u32)(mx + 104), (u32)(ry + 11), t->name, 16,
                     sel ? XMB_WHITE : XMB_TEXT, mw - 214, sel);
        if (t->duration_secs > 0) {
            char d[16];
            fmt_mmss(d, sizeof(d), t->duration_secs);
            int dw = ttf_text_width(d, 14);
            drawTTF((u32)(mx + mw - 30 - dw), (u32)(ry + 12), d, 14,
                    XMB_TEXT_DIM);
        }
    }

    // Scrollbar along the right edge when the queue outruns the panel.
    if (count > rows) {
        int bar_x   = mx + mw - 12;
        int track_h = rows * Q_ROW_H - 8;
        drawRect((u32)bar_x, (u32)list_y, 3, (u32)track_h, XMB_TRACK);
        int th = track_h * rows / count;
        if (th < 18) th = 18;
        int rng = count - rows;
        int off = rng > 0 ? (track_h - th) * s_q_scroll / rng : 0;
        drawRect((u32)bar_x, (u32)(list_y + off), 3, (u32)th, XMB_ACCENT);
    }

    static const Hint h[] = {{'X', "Play"}, {'T', "Shuffle"}, {'C', "Close"}};
    draw_hints_bar(h, 3);
}

// -------------------------------------------------------
// Main frame draw
// -------------------------------------------------------

static void draw_now_playing(const MusicCtx *ctx, const MusicTrack *tracks,
                             int count) {
    int W = (int)display_width;
    int H = (int)display_height;

    xmb_draw_topbar();
    draw_breadcrumb4(40, XMB_TOPBAR_H + 1, "Music", ctx->parent,
                     ctx->title[0] ? ctx->title : NULL, "Now Playing");

    int cur = music_current_index();
    if (cur >= count) cur = count - 1;
    const MusicTrack *t = &tracks[cur];

    // ---- art (follows the current track's album) + accent frame/glow ----
    int A  = (int)(H * 0.42f);
    int ax = 40;
    int ay = (int)(H * 0.27f);
    if (!xmb_cpu_blit_thumb(t->art_id, ax, ay, A, A))
        xmb_draw_letter_tile(t->art_id,
                             ctx->title[0] ? ctx->title : t->name,
                             ax, ay, A);
    {
        const int T = 2, G = 2, O = G + T;
        drawRect((u32)(ax - O), (u32)(ay - O), (u32)(A + 2*O), T, XMB_ACCENT);
        drawRect((u32)(ax - O), (u32)(ay + A + G), (u32)(A + 2*O), T, XMB_ACCENT);
        drawRect((u32)(ax - O), (u32)(ay - G), T, (u32)(A + 2*G), XMB_ACCENT);
        drawRect((u32)(ax + A + G), (u32)(ay - G), T, (u32)(A + 2*G), XMB_ACCENT);
        drawRectBlend((u32)(ax - O - 1), (u32)(ay - O - 1), (u32)(A + 2*O + 2), 1, XMB_ACCENT, 80);
        drawRectBlend((u32)(ax - O - 1), (u32)(ay + A + O), (u32)(A + 2*O + 2), 1, XMB_ACCENT, 80);
        drawRectBlend((u32)(ax - O - 1), (u32)(ay - O), 1, (u32)(A + 2*O), XMB_ACCENT, 80);
        drawRectBlend((u32)(ax + A + O), (u32)(ay - O), 1, (u32)(A + 2*O), XMB_ACCENT, 80);
    }

    // ---- text column: visualizer, title, artist, album, meta ----
    int tx        = ax + A + 56;
    int up_x      = W - (int)(W * 0.27f);
    int col_w     = up_x - 30 - tx;
    int title_top = ay + (int)(H * 0.15f);

    draw_visualizer(tx, title_top - 18, (int)(W * 0.265f), (int)(H * 0.11f));

    draw_clipped((u32)tx, (u32)title_top, t->name, 32, XMB_WHITE, col_w, true);
    if (t->artist[0])
        draw_clipped((u32)tx, (u32)(title_top + 48), t->artist, 20,
                     XMB_ACCENT, col_w);
    {
        char line[160] = "";
        if (ctx->title[0]) snprintf(line, sizeof(line), "%s", ctx->title);
        if (ctx->year[0]) {
            int l = (int)strlen(line);
            snprintf(line + l, sizeof(line) - l, "%s%s",
                     line[0] ? " \xB7 " : "", ctx->year);
        }
        if (line[0])
            draw_clipped((u32)tx, (u32)(title_top + 80), line, 15,
                         XMB_TEXT_DIM, col_w);
    }
    {
        // "Track 7 of 13 · Electronic · 320 kbps FLAC" (play-order position)
        char meta[160];
        int  n = snprintf(meta, sizeof(meta), "Track %d of %d",
                          music_current_pos() + 1, count);
        if (ctx->genre[0])
            n += snprintf(meta + n, sizeof(meta) - n, " \xB7 %s", ctx->genre);
        const char *src = music_source_info();
        if (src[0])
            n += snprintf(meta + n, sizeof(meta) - n, " \xB7 %s", src);
        draw_clipped((u32)tx, (u32)(title_top + 128), meta, 14,
                     XMB_TEXT_FAINT, col_w);
    }

    // ---- UP NEXT — the full remaining queue: album-art thumbs (letter
    //      tile while loading), d-pad navigable, scrollbar when it
    //      outruns the window ----
    {
        int uy    = (int)(H * 0.18f);
        int ey0   = uy + 34;
        int n_vis = uq_vis_rows();
        int pos   = music_current_pos();
        int first = pos + 1;              // first upcoming position
        int n_up  = count - first;

        drawTTF((u32)up_x, (u32)uy, "UP NEXT", 13,
                s_fzone == FZ_QUEUE ? XMB_TEXT : XMB_TEXT_FAINT, true);

        if (n_up > 0) {
            // Window origin: follow the d-pad in QUEUE zone, playback
            // otherwise.
            int scroll = (s_fzone == FZ_QUEUE) ? s_u_scroll : first;
            if (scroll > count - n_vis) scroll = count - n_vis;
            if (scroll < first)         scroll = first;

            int text_w = W - 46 - (up_x + 56);
            int ey     = ey0;
            for (int p = scroll; p < count && p < scroll + n_vis; p++) {
                int orig = music_track_at(p);
                if (orig < 0) break;
                const MusicTrack *u = &tracks[orig];
                bool selq = (s_fzone == FZ_QUEUE && p == s_u_sel);
                if (selq)
                    drawRect((u32)(up_x - 8), (u32)(ey - 4),
                             (u32)(W - 34 - (up_x - 8)), 48, XMB_PANEL_HI);
                if (!xmb_cpu_blit_thumb(u->art_id, up_x, ey, 40, 40))
                    xmb_draw_letter_tile(u->id, u->name, up_x, ey, 40);
                draw_clipped((u32)(up_x + 56), (u32)(ey + 1), u->name, 15,
                             selq ? XMB_WHITE : XMB_TEXT, text_w, selq);
                if (u->artist[0])
                    draw_clipped((u32)(up_x + 56), (u32)(ey + 22), u->artist,
                                 12, XMB_TEXT_FAINT, text_w);
                ey += 54;
            }

            if (n_up > n_vis) {
                int bar_x   = W - 26;
                int track_h = n_vis * 54 - 14;
                drawRect((u32)bar_x, (u32)ey0, 3, (u32)track_h, XMB_TRACK);
                int th = track_h * n_vis / n_up;
                if (th < 18) th = 18;
                int rng = n_up - n_vis;
                int off = rng > 0 ? (track_h - th) * (scroll - first) / rng
                                  : 0;
                drawRect((u32)bar_x, (u32)(ey0 + off), 3, (u32)th,
                         XMB_ACCENT);
            }
        }
    }

    // ---- transport row (d-pad moves focus, X activates — same model as
    //      the video player's HUD).  Focused control pops white with an
    //      accent tick under it; the play disc gets a bright ring. ----
    {
        int cx = W / 2;
        int cy = (int)(H * 0.815f);
        bool paused = music_is_paused();

        static const int T_OFF[5] = { -130, -72, 0, 72, 130 };
        static const int T_CP[5]  = { 0xE020, 0xE045, 0, 0xE044, 0xE01F };
        static const float T_PX[5] = { 22.0f, 26.0f, 0.0f, 26.0f, 22.0f };

        for (int i = 0; i < 5; i++) {
            bool fc = (i == s_t_focus);
            int  ix = cx + T_OFF[i];
            if (i == 2) {
                // Play/pause disc with glow rings.
                if (fc) ring_blend(cx, cy, 34, 0x00E9EBF5UL, 150);
                ring_blend(cx, cy, 31, XMB_ACCENT, 60);
                ring_blend(cx, cy, 28, XMB_ACCENT, 110);
                fill_circle(cx, cy, 26, XMB_ACCENT_DEEP);
                fill_circle(cx, cy, 24, XMB_ACCENT);
                drawIcon((u32)(cx - 14), (u32)(cy - 14),
                         paused ? 0xE037 : 0xE034, 28.0f, XMB_WHITE);
            } else {
                int half = (int)(T_PX[i] * 0.5f);
                u32 col  = fc ? XMB_WHITE
                              : (i == 1 || i == 3) ? 0x00AEB5D2UL
                                                   : XMB_TEXT_FAINT;
                drawIcon((u32)(ix - half), (u32)(cy - half), T_CP[i],
                         T_PX[i], col);
            }
            if (fc)
                drawRect((u32)(ix - 8), (u32)(cy + 38), 16, 3, XMB_KEY_SEL);
        }

        // Shuffle — the 6th focusable control (triangle also toggles it).
        {
            bool fc = (s_t_focus == 5);
            bool on = music_is_shuffle();
            u32 col = on ? XMB_ACCENT
                         : fc ? 0x00AEB5D2UL : 0x00363D63UL;
            drawIcon((u32)(cx + 182 - 10), (u32)(cy - 10), 0xE043, 20.0f,
                     col);
            if (fc)
                drawRect((u32)(cx + 182 - 8), (u32)(cy + 38), 16, 3,
                         XMB_KEY_SEL);
        }
    }

    // ---- seek bar (shows the pending/committed seek target while a
    //      batched seek is in flight, so taps feel instant) ----
    {
        bool seeking  = (s_seek_pend != 0 || s_seek_hold >= 0);
        u32  shown    = (u32)seek_display_secs();
        u32  duration = music_duration_secs();
        if (duration > 0 && shown > duration) shown = duration;

        int bx0 = (int)(W * 0.30f);
        int bx1 = (int)(W * 0.70f);
        int by  = (int)(H * 0.895f);
        int bw  = bx1 - bx0;

        drawRect((u32)bx0, (u32)by, (u32)bw, 4, XMB_TRACK);
        int fill = (duration > 0) ? (int)((u64)bw * shown / duration) : 0;
        if (fill > bw) fill = bw;
        if (fill > 0)
            drawRect((u32)bx0, (u32)by, (u32)fill, 4, XMB_ACCENT);
        fill_circle(bx0 + fill, by + 2, 6, XMB_WHITE);

        char ts[16];
        fmt_mmss(ts, sizeof(ts), shown);
        int tw = ttf_text_width(ts, 14);
        drawTTF((u32)(bx0 - 16 - tw), (u32)(by - 6), ts, 14,
                seeking ? XMB_TEXT : XMB_TEXT_DIM);
        if (duration > 0) {
            fmt_mmss(ts, sizeof(ts), duration);
            drawTTF((u32)(bx1 + 16), (u32)(by - 6), ts, 14, XMB_TEXT_DIM);
        }
    }

    if (!s_q_open) {
        if (s_fzone == FZ_QUEUE) {
            static const Hint h[3] = {{'X', "Play"},
                                      {'T', "Shuffle"},
                                      {'C', "Back"}};
            draw_hints_bar(h, 3);
        } else {
            static const Hint h[4] = {{'X', "Select"},
                                      {'T', "Shuffle"},
                                      {'B', "Queue"},
                                      {'C', "Back"}};
            draw_hints_bar(h, 4);
        }
    }
}

// -------------------------------------------------------
// Input — returns true when the screen should close.
// -------------------------------------------------------

static bool music_screen_input(const MusicTrack *tracks, int count) {
    (void)tracks;

    seek_update();   // commit batched seek taps once they go quiet

    if (s_q_open) {
        int vis = q_vis_rows();
        if (BTN_REPEAT(up)   && s_q_sel > 0)         s_q_sel--;
        if (BTN_REPEAT(down) && s_q_sel < count - 1) s_q_sel++;
        if (s_q_sel < s_q_scroll)        s_q_scroll = s_q_sel;
        if (s_q_sel >= s_q_scroll + vis) s_q_scroll = s_q_sel - vis + 1;
        if (BTN_PRESSED(cross)) {
            music_jump(s_q_sel);         // s_q_sel is a play-order position
            s_q_open = false;
        }
        if (BTN_PRESSED(triangle)) {     // reshuffles the list live
            music_set_shuffle(!music_is_shuffle());
            // The order just changed under the cursor — follow the
            // playing track so the highlight stays meaningful.
            s_q_sel    = music_current_pos();
            s_q_scroll = s_q_sel - vis / 2;
            if (s_q_scroll > count - vis) s_q_scroll = count - vis;
            if (s_q_scroll < 0)           s_q_scroll = 0;
        }
        if (BTN_PRESSED(circle) || BTN_PRESSED(select))
            s_q_open = false;
        return false;
    }

    // ---- QUEUE zone: d-pad drives the Up Next list ----
    if (s_fzone == FZ_QUEUE) {
        int pos   = music_current_pos();
        int first = pos + 1;
        if (first >= count) { s_fzone = FZ_TRANSPORT; return false; }
        // Track advance / shuffle can move the queue under the cursor.
        if (s_u_sel < first)  s_u_sel = first;
        if (s_u_sel >= count) s_u_sel = count - 1;

        if (BTN_PRESSED(start)) return true;
        if (BTN_PRESSED(circle) || BTN_PRESSED(left)) {
            s_fzone   = FZ_TRANSPORT;
            s_t_focus = 5;            // land back on shuffle (entry point)
            // The exit press is usually still held next frame; without this
            // the transport's left-repeat sees it as a fresh press and
            // knocks focus straight from shuffle onto fast-forward.
            s_swallow_left = true;
            return false;
        }
        if (BTN_REPEAT(up)   && s_u_sel > first)     s_u_sel--;
        if (BTN_REPEAT(down) && s_u_sel < count - 1) s_u_sel++;
        int vis = uq_vis_rows();
        if (s_u_sel < s_u_scroll)        s_u_scroll = s_u_sel;
        if (s_u_sel >= s_u_scroll + vis) s_u_scroll = s_u_sel - vis + 1;
        if (s_u_scroll < first)          s_u_scroll = first;

        if (BTN_PRESSED(cross)) {
            music_jump(s_u_sel);
            s_fzone   = FZ_TRANSPORT;  // list re-centers on the new track
            s_t_focus = 2;
        }
        if (BTN_PRESSED(triangle)) {
            music_set_shuffle(!music_is_shuffle());
            s_u_sel = s_u_scroll = music_current_pos() + 1;
        }
        if (seek_hold_tick(btn_cur.l2 != 0, 0)) seek_tap(-1);
        if (seek_hold_tick(btn_cur.r2 != 0, 1)) seek_tap(+1);
        return false;
    }

    // ---- TRANSPORT zone ----
    if (BTN_PRESSED(circle) || BTN_PRESSED(start)) return true;

    // D-pad = control focus, X = activate (video-player HUD model);
    // RIGHT past shuffle moves into the Up Next queue.
    if (s_swallow_left) {
        if (!btn_cur.left) s_swallow_left = false;   // wait for release
    } else if (BTN_REPEAT(left) && s_t_focus > 0) s_t_focus--;
    if (BTN_REPEAT(right)) {
        if (s_t_focus < 5) {
            s_t_focus++;
        } else if (music_current_pos() + 1 < count) {
            s_fzone    = FZ_QUEUE;
            s_u_sel    = music_current_pos() + 1;
            s_u_scroll = s_u_sel;
            return false;
        }
    }
    if (BTN_PRESSED(cross)) {
        switch (s_t_focus) {
        case 0: seek_tap(-1);         break;
        case 1: music_prev();         break;
        case 2: music_toggle_pause(); break;
        case 3: music_next();         break;
        case 4: seek_tap(+1);         break;
        case 5: music_set_shuffle(!music_is_shuffle()); break;
        }
    }

    if (BTN_PRESSED(triangle))
        music_set_shuffle(!music_is_shuffle());

    if (BTN_PRESSED(select)) {
        int vis    = q_vis_rows();
        s_q_open   = true;
        s_q_sel    = music_current_pos();
        s_q_scroll = s_q_sel - vis / 2;
        if (s_q_scroll > count - vis) s_q_scroll = count - vis;
        if (s_q_scroll < 0)           s_q_scroll = 0;
    }

    // Shoulder shortcuts, same as the video player: L1/R1 track skip,
    // L2/R2 seek — tap for ±10 s, hold to keep scrubbing.
    if (BTN_PRESSED(l1)) music_prev();
    if (BTN_PRESSED(r1)) music_next();
    if (seek_hold_tick(btn_cur.l2 != 0, 0)) seek_tap(-1);
    if (seek_hold_tick(btn_cur.r2 != 0, 1)) seek_tap(+1);
    return false;
}

// -------------------------------------------------------
// Entry points
// -------------------------------------------------------

static MusicTrack s_tracks[MUSIC_QUEUE_MAX];

static void music_screen_run(const MusicCtx *ctx, int count, int start_idx) {
    {
        char buf[112];
        snprintf(buf, sizeof(buf), "music_screen: %.24s/%.32s tracks=%d at=%d",
                 ctx->parent, ctx->title, count, start_idx);
        plog(buf);
    }
    if (count <= 0) return;
    if (!music_start(s_tracks, count, start_idx)) return;

    // Entered mid-frame from the XMB input handler: that frame has RSX work
    // queued (clear + wave) but its flip hasn't been issued yet.  Finish it
    // first or the loop's waitflip() below spins forever on a flip that
    // never comes — the same entry dance as the info overlay and the video
    // player (ui_info.cpp / player.cpp).
    rsxSync();
    flip();

    s_q_open    = false;
    s_fzone     = FZ_TRANSPORT;
    s_t_focus   = 2;
    s_seek_pend = 0;
    s_seek_hold = -1;
    s_last_pos  = -1;
    init_btns();

    while (running) {
        waitflip();
        sysUtilCheckCallback();
        clearScreen(XMB_BG);
        wave_draw();

        poll_buttons();
        if (music_screen_input(s_tracks, count)) break;
        if (!music_is_active()) break;   // queue finished

        rsxSync();

        draw_now_playing(ctx, s_tracks, count);
        if (s_q_open)
            draw_queue_overlay(s_tracks, count, ctx->title);

        flip();
        sysUtilCheckCallback();
    }

    music_stop();
    plog("music_screen: exit");
}

void music_screen_open_album(const XMBItem *album, const char *parent) {
    MusicCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.parent, sizeof(ctx.parent), "%s",
             parent && parent[0] ? parent : "Albums");
    snprintf(ctx.title,  sizeof(ctx.title),  "%s", album->name);
    snprintf(ctx.year,   sizeof(ctx.year),   "%s", album->year_str);
    snprintf(ctx.genre,  sizeof(ctx.genre),  "%s", album->genre);
    int count = music_fetch_album_tracks(album->id, s_tracks, MUSIC_QUEUE_MAX);
    music_screen_run(&ctx, count, 0);
}

void music_screen_open_songs(const XMBItem *items, int count, int start_idx) {
    if (count > MUSIC_QUEUE_MAX) count = MUSIC_QUEUE_MAX;
    if (start_idx >= count) start_idx = count - 1;
    for (int i = 0; i < count; i++) {
        MusicTrack *t = &s_tracks[i];
        memset(t, 0, sizeof(*t));
        snprintf(t->id,     sizeof(t->id),     "%s", items[i].id);
        snprintf(t->name,   sizeof(t->name),   "%s", items[i].name);
        snprintf(t->artist, sizeof(t->artist), "%s", items[i].artist);
        snprintf(t->art_id, sizeof(t->art_id), "%s",
                 items[i].album_id[0] ? items[i].album_id : items[i].id);
        t->duration_secs = items[i].dur_secs;
    }
    MusicCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.parent, sizeof(ctx.parent), "Songs");
    snprintf(ctx.genre,  sizeof(ctx.genre), "%s", items[start_idx].genre);
    music_screen_run(&ctx, count, start_idx);
}

void music_screen_open_playlist(const XMBItem *playlist) {
    MusicCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.parent, sizeof(ctx.parent), "Playlists");
    snprintf(ctx.title,  sizeof(ctx.title),  "%s", playlist->name);
    int count = music_fetch_playlist_tracks(playlist->id, s_tracks,
                                            MUSIC_QUEUE_MAX);
    music_screen_run(&ctx, count, 0);
}
