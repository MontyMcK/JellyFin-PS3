// Triangle detail overlay — shown when the user presses triangle on a list item.

#include <stdio.h>
#include <string.h>

#include <rsx/rsx.h>
#include <sysutil/sysutil.h>
#include <io/pad.h>

#include "ui_internal.h"
#include "ui_wave.h"
#include "jellyfin_api.h"
#include "rsxutil.h"
#include "timing.h"
#include "plog.h"
#include "slog.h"
#include "detail_media.h"
#include "thumbnail_cache.h"

// The title band behind the header — a deep purple stripe over the backdrop
// (the official page's grey bar, re-tinted to fit the XMB indigo palette).
#define XMB_DETAIL_BAND 0x00412C73UL

// "More Like This" recommendations to request/show.
#define INFO_SIMILAR_MAX 12


// Draw text at (x,y), truncated with ".." to max_w px.  y is a signed screen
// coordinate (the page scrolls), so a line above the top is simply culled by
// drawTTF's own clipping.
static void info_clip_text(int x, int y, const char *text, float px,
                           u32 color, int max_w, bool bold) {
    if (!text || !text[0]) return;
    if (ttf_text_width(text, px, bold) <= max_w) {
        drawTTF((u32)x, (u32)y, text, px, color, bold);
        return;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", text);
    int len = (int)strlen(buf);
    while (len > 1) {
        buf[--len] = '\0';
        char trial[164];
        snprintf(trial, sizeof(trial), "%s..", buf);
        if (ttf_text_width(trial, px, bold) <= max_w) {
            drawTTF((u32)x, (u32)y, trial, px, color, bold);
            return;
        }
    }
}

// A frame that bails out early — switching titles, or coming back from the
// player — must STILL queue a flip before looping.  waitflip() at the top of
// the next iteration spins until the PREVIOUS flip lands
// (`while (gcmGetFlipStatus() != 0) usleep(50);`), so a `continue` that skips
// flip() leaves the status stuck at "pending" and hangs the UI thread forever
// at 20k syscalls/sec.  This is the same rsxSync()+flip() the screen does on
// entry; call it immediately before any early `continue`.
static void info_skip_frame(void) { rsxSync(); flip(); }

// 1:1 CPU blit of a main-memory bitmap to the current framebuffer, clipped to
// the display.  Same idea as the grid's card blit (ui_lists.cpp) — call after
// rsxSync.  The pixels are precomputed, so this is a per-row memcpy into
// write-combined VRAM (fast on hardware); no per-pixel blend/read.
static void info_blit(const Bitmap *bm, int dx, int dy) {
    if (!bm || !bm->pixels) return;
    int w = (int)bm->width, h = (int)bm->height;
    for (int row = 0; row < h; row++) {
        int sy = dy + row;
        if (sy < 0 || sy >= (int)display_height) continue;
        int sx0 = dx, cw = w, srcx = 0;
        if (sx0 < 0) { srcx = -sx0; cw += sx0; sx0 = 0; }
        if (sx0 + cw > (int)display_width) cw = (int)display_width - sx0;
        if (cw <= 0) continue;
        memcpy(color_buffer[curr_fb] + (u32)sy * display_width + sx0,
               bm->pixels + (u32)row * bm->width + srcx, (size_t)cw * 4);
    }
}

void xmb_show_item_info(const XMBItem *root) {
    {
        char dbg[260];
        snprintf(dbg, sizeof(dbg),
            "info: ENTER "
            "btn_cur(tri=%d cir=%d crs=%d) btn_prev(tri=%d cir=%d crs=%d) "
            "name='%.40s'",
            btn_cur.triangle, btn_cur.circle, btn_cur.cross,
            btn_prev.triangle, btn_prev.circle, btn_prev.cross,
            root->name);
        plog(dbg);
    }
    rsxSync();
    flip();
    init_btns();

    // ---- The title currently on screen.  Opening a "More Like This" pick
    // REPLACES it in place: Circle always leaves for the library grid, so there
    // is no back-trail worth keeping and only one title's detail + ~550KB
    // poster is ever resident (this screen used to re-enter itself recursively,
    // holding every visited title's page state and poster at once).
    XMBItem cur_item = *root;
    int nav_hops = 0;   // drill-ins so far — STATE log / test assertions only

    // ---- Layout, computed once (poster + text column geometry) ----
    const int X   = XMB_ITEM_PAD;
    const int top = XMB_OY + XMB_TOPBAR_H + 12;
    const int content_bot = (int)display_height - XMB_BOTTOM_PAD;
    int poster_h = content_bot - top;
    if (poster_h > 456) poster_h = 456;
    const int poster_w = poster_h * 2 / 3;
    const int poster_x = X, poster_y = top;
    const int tx    = poster_x + poster_w + 28;   // text column left edge
    const int max_w = (int)display_width - tx - X;
    const int back_w = (int)display_width;
    const int band_y0 = top - 14;                 // purple title-band extent
    const int band_y1 = top + 96;
    const int view_bottom = content_bot;
    const int SIM_CH = 222;                       // More Like This card height

    // ---- Per-title state, (re)loaded whenever the nav stack moves ----
    XMBItemDetail detail;
    memset(&detail, 0, sizeof(detail));
    Bitmap hero_poster;
    memset(&hero_poster, 0, sizeof(hero_poster));
    XMBItem similar[INFO_SIMILAR_MAX];
    int  n_similar = 0;
    bool reload    = true;

    // The page is taller than the screen; d-pad up/down jumps to top/bottom.
    // max_scroll is recomputed from the laid-out content height each frame.
    int scroll_y = 0, max_scroll = 0;
    // Focus moves between the Play button and the More Like This row, which is
    // what Cross acts on.  (Gating this on "is the row visible" made Cross mean
    // Open even at the top of the page, so Play could never be pressed.)
    enum { FOCUS_PLAY = 0, FOCUS_SIM = 1 };
    int focus = FOCUS_PLAY;
    int sim_sel = 0, sim_row_scroll = 0;

    int info_frames = 0;
    int info_exit_reason = 0;
    bool exit_armed = false;
    while (running) {
        if (reload) {
            reload = false;
            const XMBItem *cur = &cur_item;
            detail_media_free(&hero_poster);
            memset(&detail, 0, sizeof(detail));
            jellyfin_fetch_item_detail(cur->id, &detail);
            // Hi-res poster: the grid thumbnail cache only holds card-sized art,
            // so a poster blown up from it looks pixelated.  On failure the draw
            // loop falls back to the cached card thumb.
            detail_media_load(cur->id, "Primary", poster_w, poster_h, 0.5f,
                              &hero_poster);
            n_similar = xmb_fetch_similar(cur->id, similar, INFO_SIMILAR_MAX);
            // Cast headshots + similar posters are card-sized: let the grid
            // cache reuse its slots for them (ticked each frame below).
            thumb_cache_retarget();
            scroll_y = 0; max_scroll = 0;
            sim_sel  = 0; sim_row_scroll = 0;
            focus    = FOCUS_PLAY;
            exit_armed = false;
            init_btns();
            slog_state("INFO_OPEN depth=%d item_id=%s name=%.40s",
                       nav_hops, cur->id, cur->name);
        }
        const XMBItem *it = &cur_item;

        waitflip();
        sysUtilCheckCallback();
        poll_buttons();

        if (!exit_armed) {
            if (!btn_cur.circle && !btn_cur.triangle && !btn_cur.cross)
                exit_armed = true;
        } else {
            // Circle always leaves the screen for the library grid — including
            // from a title reached through More Like This.
            if (BTN_PRESSED(circle)) { info_exit_reason = 1; goto info_done; }
            if (BTN_PRESSED(triangle)) { info_exit_reason = 2; goto info_done; }

            // Up/down move focus AND jump the page: down drops from the Play
            // button to the recommendations at the bottom, up returns to Play
            // at the top — one press each way.
            if (BTN_PRESSED(down)) {
                if (focus == FOCUS_PLAY && n_similar > 0) focus = FOCUS_SIM;
                scroll_y = max_scroll;
            }
            if (BTN_PRESSED(up)) {
                if (focus == FOCUS_SIM) focus = FOCUS_PLAY;
                scroll_y = 0;
            }
            if (focus == FOCUS_SIM) {
                if (BTN_REPEAT(left)  && sim_sel > 0)             sim_sel--;
                if (BTN_REPEAT(right) && sim_sel < n_similar - 1) sim_sel++;
            }
            if (BTN_PRESSED(cross)) {
                if (focus == FOCUS_SIM) {
                    // Swap the page over to the highlighted recommendation.
                    // Copied by value first — the reload overwrites similar[].
                    cur_item = similar[sim_sel];
                    nav_hops++;
                    reload = true;
                    info_skip_frame();
                    continue;
                } else {
                    // Play — same launch flow as the grid (resume prompt first).
                    int resume = xmb_resume_choice(it);
                    if (resume >= 0) {
                        if (strcmp(it->type, "Episode") == 0)
                            xmb_play_episode_with_next(it, (u32)resume);
                        else
                            xmb_play_item(it, (u32)resume);
                    }
                    exit_armed = false;
                    init_btns();
                    info_skip_frame();
                    continue;
                }
            }
        }
        if (scroll_y < 0)          scroll_y = 0;
        if (scroll_y > max_scroll) scroll_y = max_scroll;

        thumb_cache_tick();
        clearScreen(XMB_BG);
        wave_draw();
        rsxSync();
        {
            // Everything below is drawn in SCREEN space = layout position
            // minus scroll_y, so the whole page scrolls as one.
            const int py = poster_y - scroll_y;

            // ---- Purple title band behind the header (over the wave).
            drawRect(0, (u32)(band_y0 - scroll_y), (u32)back_w,
                     (u32)(band_y1 - band_y0), XMB_DETAIL_BAND);

            // ---- Left column: portrait poster.  Hi-res if it loaded, else the
            // cached card thumb upscaled (dim placeholder while it fetches).
            if (hero_poster.pixels)
                info_blit(&hero_poster, poster_x, py);
            else
                xmb_cpu_blit_thumb_scaled(it->id, poster_x, py,
                                          poster_w, poster_h);
            // Hairline frame just outside the artwork.
            drawRect((u32)(poster_x - 1), (u32)(py - 1),
                     (u32)(poster_w + 2), 1, XMB_HAIRLINE);
            drawRect((u32)(poster_x - 1), (u32)(py + poster_h),
                     (u32)(poster_w + 2), 1, XMB_HAIRLINE);
            drawRect((u32)(poster_x - 1), (u32)(py - 1),
                     1, (u32)(poster_h + 2), XMB_HAIRLINE);
            drawRect((u32)(poster_x + poster_w), (u32)(py - 1),
                     1, (u32)(poster_h + 2), XMB_HAIRLINE);

            // ---- Right column: title + metadata + text, to the poster's right.
            int Y = top - scroll_y;

            // Title, truncated to the text-column width.
            {
                char tbuf[132];
                snprintf(tbuf, sizeof(tbuf), "%s", it->name);
                int len = (int)strlen(tbuf);
                while (len > 1 && ttf_text_width(tbuf, 40) > max_w)
                    tbuf[--len] = '\0';
                drawTTF((u32)tx, (u32)Y, tbuf, 40, XMB_WHITE, true);
            }
            Y += 62;

            // Meta row: year · duration, official-rating chip, ★ rating.
            {
                int mx = tx;
                char meta[64] = "";
                if (it->year_str[0])
                    snprintf(meta, sizeof(meta), "%s", it->year_str);
                if (it->duration_str[0]) {
                    if (meta[0]) strncat(meta, " \xB7 ", sizeof(meta)-strlen(meta)-1);
                    strncat(meta, it->duration_str, sizeof(meta)-strlen(meta)-1);
                }
                if (meta[0]) {
                    drawTTF((u32)mx, (u32)Y, meta, 18, XMB_TEXT_DIM);
                    mx += ttf_text_width(meta, 18) + 18;
                }
                if (detail.official_rating[0]) {
                    // Outlined chip.
                    int cw = ttf_text_width(detail.official_rating, 13) + 16;
                    int ch = 22;
                    drawRect((u32)mx, (u32)Y, (u32)cw, 1, XMB_HAIRLINE);
                    drawRect((u32)mx, (u32)(Y + ch - 1), (u32)cw, 1, XMB_HAIRLINE);
                    drawRect((u32)mx, (u32)Y, 1, (u32)ch, XMB_HAIRLINE);
                    drawRect((u32)(mx + cw - 1), (u32)Y, 1, (u32)ch, XMB_HAIRLINE);
                    drawTTF((u32)(mx + 8), (u32)(Y + 3), detail.official_rating,
                            13, XMB_TEXT_DIM);
                    mx += cw + 18;
                }
                if (detail.community_rating[0]) {
                    drawIcon((u32)mx, (u32)(Y + 1), ICON_STAR, 18.0f, 0x00E8B64CUL);
                    drawTTF((u32)(mx + 24), (u32)Y, detail.community_rating,
                            18, XMB_TEXT_DIM);
                }
            }
            Y += 44;

            // Play button — Cross launches playback (with the resume prompt if
            // the title is partly watched), matching the grid's launch flow.
            // Focused by default; a white frame shows when Cross will play.
            {
                const int bw = 128, bh = 40;
                const bool pf = (focus == FOCUS_PLAY);
                drawRect((u32)tx, (u32)Y, (u32)bw, (u32)bh,
                         pf ? XMB_ACCENT : XMB_PANEL_HI);
                if (pf) {
                    drawRect((u32)(tx - 2), (u32)(Y - 2), (u32)(bw + 4), 2, XMB_KEY_SEL);
                    drawRect((u32)(tx - 2), (u32)(Y + bh), (u32)(bw + 4), 2, XMB_KEY_SEL);
                    drawRect((u32)(tx - 2), (u32)(Y - 2), 2, (u32)(bh + 4), XMB_KEY_SEL);
                    drawRect((u32)(tx + bw), (u32)(Y - 2), 2, (u32)(bh + 4), XMB_KEY_SEL);
                }
                u32 fg = pf ? 0x00131630UL : XMB_TEXT_DIM;
                drawIcon((u32)(tx + 16), (u32)(Y + (bh - 22) / 2), ICON_PLAY,
                         22.0f, fg);
                drawTTF((u32)(tx + 46), (u32)(Y + (bh - 20) / 2 + 1), "Play",
                        20, fg, true);
                Y += bh + 18;
            }

            if (detail.tagline[0]) {
                drawTTF((u32)tx, (u32)Y, detail.tagline, 20, 0x00AFA3E8UL);
                Y += 38;
            }

            // Overview, word-wrapped by pixel width.
            if (detail.overview[0]) {
                const int wrap_w    = max_w > 760 ? 760 : max_w;
                const int max_lines = 6;
                const char *p = detail.overview;
                int lines_drawn = 0;
                while (*p && lines_drawn < max_lines) {
                    // Longest prefix that fits, broken at a space.
                    // (Width accumulated per char; ignoring kerning is fine
                    // for wrapping.)
                    int  fit = 0, last_sp = -1;
                    int  wpx = 0;
                    char buf[160];
                    char one[2] = { 0, 0 };
                    while (p[fit] && fit < (int)sizeof(buf) - 1) {
                        if (p[fit] == ' ') last_sp = fit;
                        one[0] = p[fit];
                        wpx += ttf_text_width(one, 19);
                        if (wpx > wrap_w) break;
                        fit++;
                    }
                    int take = (!p[fit] || fit >= (int)sizeof(buf) - 1) ? fit
                             : (last_sp > 0 ? last_sp : fit);
                    if (take <= 0) take = 1;
                    snprintf(buf, sizeof(buf), "%.*s", take, p);
                    drawTTF((u32)tx, (u32)Y, buf, 19, 0x00C9CEE4UL);
                    p += take;
                    while (*p == ' ') p++;
                    Y += 30;
                    lines_drawn++;
                }
                Y += 14;
            }

            // Fact rows: faint label column, bright values.
            struct { const char *label; const char *value; } facts[] = {
                { "Video",   detail.video_info  },
                { "Audio",   detail.audio_info  },
                { "Genres",  detail.genres      },
                { "Studios", detail.studios     },
            };
            for (int i = 0; i < 4; i++) {
                if (!facts[i].value[0]) continue;
                drawTTF((u32)tx,         (u32)(Y + 2), facts[i].label, 15,
                        XMB_TEXT_FAINT);
                drawTTF((u32)(tx + 120), (u32)Y, facts[i].value, 17, XMB_TEXT);
                Y += 32;
            }

            // ---- Sections below the hero: begin under whichever column is
            // taller — the fact list or the poster.
            int hero_bottom = poster_y + poster_h - scroll_y;
            int sec = (Y > hero_bottom ? Y : hero_bottom) + 40;

            // Cast & Crew — headshot cards with name + role.
            if (detail.n_people > 0) {
                drawTTF((u32)X, (u32)sec, "Cast & Crew", 22, XMB_TEXT, true);
                sec += 42;
                const int cw = 118, ch = 176, gap = 22;
                for (int i = 0; i < detail.n_people; i++) {
                    int cxp = X + i * (cw + gap);
                    if (cxp + cw > (int)display_width - X) break;   // no h-scroll yet
                    xmb_cpu_blit_thumb_scaled(detail.people[i].id, cxp, sec, cw, ch);
                    info_clip_text(cxp, sec + ch + 8, detail.people[i].name,
                                   14, XMB_TEXT, cw, true);
                    if (detail.people[i].role[0])
                        info_clip_text(cxp, sec + ch + 28, detail.people[i].role,
                                       12, XMB_TEXT_DIM, cw, false);
                }
                sec += ch + 8 + 44;
            }

            // More Like This — recommended poster cards.  Left/right selects a
            // card (Cross opens it); the row scrolls horizontally to follow.
            if (n_similar > 0) {
                drawTTF((u32)X, (u32)sec, "More Like This", 22, XMB_TEXT, true);
                sec += 42;
                const int cw = 148, ch = SIM_CH, gap = 22;
                const int pitch = cw + gap;
                const int window_w = (int)display_width - 2 * X;
                // Keep the selected card within the visible window.
                int sel_left = sim_sel * pitch;
                if (sel_left < sim_row_scroll) sim_row_scroll = sel_left;
                if (sel_left + cw > sim_row_scroll + window_w)
                    sim_row_scroll = sel_left + cw - window_w;
                int max_row = n_similar * pitch - gap - window_w;
                if (max_row < 0) max_row = 0;
                if (sim_row_scroll < 0)       sim_row_scroll = 0;
                if (sim_row_scroll > max_row) sim_row_scroll = max_row;

                for (int i = 0; i < n_similar; i++) {
                    int cxp = X + i * pitch - sim_row_scroll;
                    if (cxp + cw <= 0 || cxp >= (int)display_width) continue;
                    xmb_cpu_blit_thumb_scaled(similar[i].id, cxp, sec, cw, ch);
                    bool selected = (i == sim_sel && focus == FOCUS_SIM);
                    if (selected) {
                        drawRect((u32)(cxp - 2), (u32)(sec - 2), (u32)(cw + 4), 2, XMB_KEY_SEL);
                        drawRect((u32)(cxp - 2), (u32)(sec + ch),  (u32)(cw + 4), 2, XMB_KEY_SEL);
                        drawRect((u32)(cxp - 2), (u32)(sec - 2), 2, (u32)(ch + 4), XMB_KEY_SEL);
                        drawRect((u32)(cxp + cw), (u32)(sec - 2), 2, (u32)(ch + 4), XMB_KEY_SEL);
                    }
                    info_clip_text(cxp, sec + ch + 8, similar[i].name, 14,
                                   selected ? XMB_WHITE : XMB_TEXT_DIM, cw,
                                   selected);
                }
                sec += ch + 8 + 30;
            }

            // Content height (absolute) → how far the page can scroll.
            int content_bottom = sec + scroll_y;
            max_scroll = content_bottom - view_bottom;
            if (max_scroll < 0) max_scroll = 0;

            // Scrollbar on the right edge when the page overflows.
            if (max_scroll > 0) {
                int bx = (int)display_width - 10;
                int ty0 = top, th = view_bottom - top;
                drawRect((u32)bx, (u32)ty0, 3, (u32)th, XMB_TRACK);
                int total = content_bottom > 0 ? content_bottom : 1;
                int thb = th * view_bottom / total;
                if (thb < 26) thb = 26;
                if (thb > th) thb = th;
                int off = (th - thb) * scroll_y / max_scroll;
                drawRect((u32)bx, (u32)(ty0 + off), 3, (u32)thb, XMB_ACCENT);
            }
        }
        {
            Hint h[3]; int nh = 0;
            h[nh].glyph = 'C'; h[nh].label = "Back";  nh++;
            if (max_scroll > 0) { h[nh].glyph = 'D'; h[nh].label = "Scroll"; nh++; }
            h[nh].glyph = 'X';
            h[nh].label = (focus == FOCUS_SIM) ? "Open" : "Play"; nh++;
            draw_hints_bar(h, nh);
        }
        flip();
        info_frames++;
        if ((info_frames % 30) == 0) {
            char dbg[80];
            snprintf(dbg, sizeof(dbg), "info: frame=%d", info_frames);
            plog(dbg);
            slog_state("INFO_FRAME n=%d", info_frames);   // liveness heartbeat
        }
    }
    info_exit_reason = 3;
    info_done:
    {
        char dbg[200];
        snprintf(dbg, sizeof(dbg),
            "info: EXIT reason=%d frames=%d "
            "btn_cur(tri=%d cir=%d crs=%d) btn_prev(tri=%d cir=%d crs=%d)",
            info_exit_reason, info_frames,
            btn_cur.triangle, btn_cur.circle, btn_cur.cross,
            btn_prev.triangle, btn_prev.circle, btn_prev.cross);
        plog(dbg);
    }
    slog_state("INFO_CLOSE reason=%d frames=%d", info_exit_reason, info_frames);
    detail_media_free(&hero_poster);
    g_info_cooldown_until = timing_get_us() + 500000;
    init_btns();
}

// Resume-or-restart prompt for a partly-watched item.  Blocks with its own
// draw/input loop over the wave background, exactly like xmb_show_item_info.
// Returns where playback should start:
//   >= 0  play at this many seconds (0 = from the beginning)
//   <  0  the user cancelled — don't play.
// Items with no meaningful resume point (< 10 s watched) skip the prompt and
// return 0 (start from the beginning) so a fresh item plays immediately.
int xmb_resume_choice(const XMBItem *it) {
    if (!it || it->resume_secs < 10) return 0;

    // Format the saved position as H:MM:SS / M:SS.
    char tstr[16];
    u32 s = it->resume_secs;
    if (s >= 3600) snprintf(tstr, sizeof(tstr), "%u:%02u:%02u",
                            s / 3600, (s % 3600) / 60, s % 60);
    else           snprintf(tstr, sizeof(tstr), "%u:%02u", s / 60, s % 60);
    char opt_resume[48];
    snprintf(opt_resume, sizeof(opt_resume), "Resume from %s", tstr);
    char sub[48];
    snprintf(sub, sizeof(sub), "Stopped at %s", tstr);
    const char *opts[2] = { opt_resume, "Start from beginning" };

    rsxSync();
    flip();
    init_btns();

    int  sel   = 0;       // 0 = resume (default), 1 = start over
    bool armed = false;   // wait for the launching cross/circle to release
    while (running) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();

        if (!armed) {
            if (!btn_cur.cross && !btn_cur.circle) armed = true;
        } else {
            if (BTN_PRESSED(circle)) { init_btns(); return -1; }
            if (BTN_PRESSED(up))     sel = 0;
            if (BTN_PRESSED(down))   sel = 1;
            if (BTN_PRESSED(cross)) {
                int r = (sel == 0) ? (int)it->resume_secs : 0;
                init_btns();
                return r;
            }
        }

        clearScreen(XMB_BG);
        wave_draw();
        rsxSync();

        int pw = UIS_W(560), ph = UIS_H(236);
        int px = ((int)display_width  - pw) / 2;
        int py = ((int)display_height - ph) / 2;
        drawRect((u32)px, (u32)py, (u32)pw, (u32)ph, XMB_PANEL);
        drawRect((u32)px, (u32)py, (u32)pw, 1, XMB_HAIRLINE);
        drawRect((u32)px, (u32)(py + ph - 1), (u32)pw, 1, XMB_HAIRLINE);
        drawRect((u32)px, (u32)py, 1, (u32)ph, XMB_HAIRLINE);
        drawRect((u32)(px + pw - 1), (u32)py, 1, (u32)ph, XMB_HAIRLINE);

        int cx    = px + 32;
        int max_w = pw - 64;
        int y     = py + 30;

        // Item title, truncated to the panel width.
        {
            char tbuf[132];
            snprintf(tbuf, sizeof(tbuf), "%s", it->name);
            int len = (int)strlen(tbuf);
            while (len > 1 && ttf_text_width(tbuf, 25, true) > max_w)
                tbuf[--len] = '\0';
            drawTTF((u32)cx, (u32)y, tbuf, 25, XMB_WHITE, true);
        }
        y += 40;
        drawTTF((u32)cx, (u32)y, sub, 15, XMB_TEXT_DIM);
        y += 34;

        // Two option rows; the selected one gets the panel-hi fill + accent bar.
        int ow = max_w, oh = UIS_H(46);
        for (int i = 0; i < 2; i++) {
            int oy = y + i * (oh + 10);
            if (i == sel) {
                drawRect((u32)cx, (u32)oy, (u32)ow, (u32)oh, XMB_PANEL_HI);
                drawRect((u32)(cx - 4), (u32)oy, 3, (u32)oh, XMB_ACCENT);
            }
            drawTTF_vcentered((u32)(cx + 16), oy + oh / 2, opts[i], 19,
                              i == sel ? XMB_TEXT : XMB_TEXT_DIM);
        }

        { static const Hint h[] = {{'X', "Select"}, {'C', "Back"}};
          draw_hints_bar(h, 2); }
        flip();
    }
    init_btns();
    return -1;
}
