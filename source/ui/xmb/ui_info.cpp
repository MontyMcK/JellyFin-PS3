// Triangle detail overlay — shown when the user presses triangle on a list item.

#include <stdio.h>
#include <string.h>

#include <rsx/rsx.h>
#include <sysutil/sysutil.h>
#include <io/pad.h>

#include "ui_internal.h"
#include "ui_wave.h"
#include "jellyfin_api.h"
#include "vquality.h"
#include "hd1080.h"
#include "rsxutil.h"
#include "timing.h"
#include "plog.h"
#include "slog.h"
#include "detail_media.h"
#include "thumbnail_cache.h"

// The title band behind the header — a deep stripe over the backdrop (the
// official page's grey bar, re-tinted to fit the theme).  Now the `detail_band`
// theme token: XMB wave keeps the exact 412C73 this was hardcoded to, so the
// shipping theme is unchanged, and Golden Age finally gets a brass band instead
// of a violet one on a gold screen.  See theme.h.

// "More Like This" recommendations to request/show.
#define INFO_SIMILAR_MAX 12


// Draw text at (x,y), truncated with ".." to max_w px.  y is a signed screen
// coordinate (the page scrolls), so a line above the top is simply culled by
// drawTTF's own clipping.
// face < 0 keeps the system face (regular or bold).  Pass a UI_FACE_* for the
// handoff section 3.0 faces: UI_FACE_DISPLAY for the media title,
// UI_FACE_SPEC for codec and quality values.
static void info_clip_text(int x, int y, const char *text, float px,
                           u32 color, int max_w, bool bold, int face = -1) {
    if (!text || !text[0]) return;
    if (face >= 0) {
        if (ttf_text_width_face(text, px, face) <= max_w) {
            drawTTF_face((u32)x, (u32)y, text, px, color, face);
            return;
        }
        // Clip against the SAME face the text is drawn in, or the ellipsis
        // lands in the wrong place.
        char fb[160];
        snprintf(fb, sizeof(fb), "%s", text);
        int fl = (int)strlen(fb);
        while (fl > 1) {
            fb[--fl] = '\0';
            char tr[164];
            snprintf(tr, sizeof(tr), "%s..", fb);
            if (ttf_text_width_face(tr, px, face) <= max_w) {
                drawTTF_face((u32)x, (u32)y, tr, px, color, face);
                return;
            }
        }
        return;
    }
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

// Modal version picker used by the info page.  The full source list lives only
// here, before playback starts; the player receives one selected MediaSourceId.
static int info_choose_version(const char *title,
                               const JFMediaSources *sources, int current) {
    if (!sources || sources->n_sources <= 0) return -1;
    int sel = (current >= 0 && current < sources->n_sources) ? current : 0;
    bool armed = false;
    rsxSync();
    flip();
    init_btns();

    while (running) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();

        if (!armed) {
            if (!btn_cur.cross && !btn_cur.circle) armed = true;
        } else {
            if (BTN_PRESSED(circle)) { init_btns(); return -1; }
            if (BTN_REPEAT(up) && sel > 0) sel--;
            if (BTN_REPEAT(down) && sel < sources->n_sources - 1) sel++;
            if (BTN_PRESSED(cross)) { init_btns(); return sel; }
        }

        clearScreen(XMB_BG);
        wave_draw();
        rsxSync();

        const int max_rows = 8;
        int shown = sources->n_sources < max_rows
                      ? sources->n_sources : max_rows;
        int first = sel - shown / 2;
        if (first < 0) first = 0;
        if (first > sources->n_sources - shown)
            first = sources->n_sources - shown;

        int pw = UIS_W(760);
        int row_h = UIS_H(58);            // two lines: the summary, the full name
        int ph = UIS_H(104) + shown * row_h;
        int px = ((int)display_width - pw) / 2;
        int py = ((int)display_height - ph) / 2;
        drawRect((u32)px, (u32)py, (u32)pw, (u32)ph, XMB_PANEL);
        drawRect((u32)px, (u32)py, (u32)pw, 1, XMB_HAIRLINE);
        drawRect((u32)px, (u32)(py + ph - 1), (u32)pw, 1, XMB_HAIRLINE);
        drawRect((u32)px, (u32)py, 1, (u32)ph, XMB_HAIRLINE);
        drawRect((u32)(px + pw - 1), (u32)py, 1, (u32)ph, XMB_HAIRLINE);

        int cx = px + UIS_W(30);
        // Media title -- one of the two things section 3.0 gives to the
        // display face.
        info_clip_text(cx, py + UIS_H(22), title, UIS_TF(24), XMB_WHITE,
                       pw - UIS_W(160), false, UI_FACE_DISPLAY);
        char count[24];
        snprintf(count, sizeof(count), "%d / %d", sel + 1,
                 sources->n_sources);
        int cw = ttf_text_width(count, UIS_TF(17));
        drawTTF((u32)(px + pw - UIS_W(30) - cw),
                (u32)(py + UIS_H(27)), count, UIS_TF(17), XMB_TEXT_DIM);

        int y0 = py + UIS_H(72);
        for (int row = 0; row < shown; row++) {
            int idx = first + row;
            int ry = y0 + row * row_h;
            if (idx == sel) {
                drawRect((u32)cx, (u32)ry,
                         (u32)(pw - UIS_W(60)), (u32)(row_h - UIS_H(4)),
                         XMB_PANEL_HI);
                drawRect((u32)(cx - UIS_W(4)), (u32)ry, UIS_W(3),
                         (u32)(row_h - UIS_H(4)), XMB_ACCENT);
            }
            // The summary ("1080p · REMUX · DTS-HD MA 7.1 · 38.8 GB"), and
            // the full, cleaned name small beneath it.
            info_clip_text(cx + UIS_W(16), ry + UIS_H(7),
                           sources->source[idx].summary[0] ? sources->source[idx].summary
                                                           : sources->source[idx].label,
                           UIS_TF(18), idx == sel ? XMB_TEXT : XMB_TEXT_DIM,
                           pw - UIS_W(100), idx == sel);
            info_clip_text(cx + UIS_W(16), ry + UIS_H(33),
                           sources->source[idx].label, UIS_TF(12.5f),
                           idx == sel ? XMB_TEXT_DIM : XMB_TEXT_FAINT,
                           pw - UIS_W(100), false);
        }

        { static const Hint h[] = {{'X', "Select"}, {'C', "Back"}};
          draw_hints_bar(h, 2); }
        flip();
    }
    init_btns();
    return -1;
}

// ===========================================================================
// Item detail, redesigned -- design-import-v3 "06 · Item detail", L3 of the
// spine's depth model.  Used whenever jellyfin_spine.txt is on; the screen
// above is the pre-revamp page and still runs with the gate off.
//
// Every position is the canvas's, measured by script at 1280x720 (see
// design-import-v3/MEASURED-detail-hud.md) and scaled like everything else.
//
// WHAT IS DRAWN WHERE, and why it matters here:
//   GPU phase  backdrop + scrims, the spine's glow, the divider, every panel,
//              chip and button (rounded fans), the uploaded poster.  All of
//              it blended on the RSX, none of it read back.
//   CPU phase  cast portraits (write-only circle blits), hairlines.
//   text       everything else, through the text-run cache -- the old page
//              drew every glyph on the CPU against the framebuffer.
//
// DEVIATIONS FROM THE CANVAS, all because the data is not there:
//   * the second chip shows the VIDEO line ("1080p H264 SDR"), not the HDMI
//     audio path -- the client does not know the output path until playback;
//   * no Trailer: nothing plays trailers yet, so the hint says what X does;
//   * "More Like This", the tagline and the facts list are not on this page.
//     The canvas has none of them and the old page keeps them.
// ===========================================================================

#include "ui_card_gpu.h"
#include "ui_art.h"
#include "ui_buffering.h"
#include "ui_spine.h"
#include "ui_depth.h"      // the L2 -> L3 arrival
#include "ui_text_gpu.h"
#include "http.h"
#include "api_facts.h"
#include "vremember.h"       // jellyfin_warm_playback()

static int IX(int x) { return XMB_OX + UIS_W(x); }
static int IY(int y) { return XMB_OY + UIS_H(y); }

// Colour mixed toward another; used for the canvas's opacity steps on text
// (a run's colour is cached, so these are fixed values, not animations).
static u32 info_mix(u32 a, u32 b, float t) {
    u32 out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
        out |= (u32)(ca + (int)((float)(cb - ca) * t)) << sh;
    }
    return out;
}

// Mark an item played on the server.  POST /Users/{user}/PlayedItems/{id},
// the same call the web client makes.
static bool info_mark_played(const char *item_id) {
    char url[512];
    static char resp[2048];
    snprintf(url, sizeof url, "%s/Users/%s/PlayedItems/%s",
             g_server, g_userid, item_id);
    int st = http_request(HTTP_POST, url, "", g_token, resp, sizeof resp);
    char msg[160];
    snprintf(msg, sizeof msg, "info: mark played %s -> %d", item_id, st);
    plog(msg);
    const bool ok = st >= 200 && st < 300;
    if (ok) g_play_gen++;         // Continue Watching / Next Up are now stale
    return ok;
}

// The audio line without its leading language ("English EAC3 5.1" -> "EAC3
// 5.1"), for the codec chip.  Unchanged when there is only one word.
static void info_audio_chip(const char *audio, char *out, size_t cap) {
    const char *sp = strchr(audio, ' ');
    snprintf(out, cap, "%s", (sp && sp[1]) ? sp + 1 : audio);
}

// Word-wrap `text` at max_w, drawing up to max_lines at `pitch`.  Returns the
// number of lines drawn.
static int info_wrap(int x, int y, const char *text, float px, u32 colour,
                     int max_w, int pitch, int max_lines) {
    const char *p = text;
    int lines = 0;
    while (*p && lines < max_lines) {
        int fit = 0, last_sp = -1, wpx = 0;
        char buf[200], one[2] = { 0, 0 };
        while (p[fit] && fit < (int)sizeof(buf) - 4) {
            if (p[fit] == ' ') last_sp = fit;
            one[0] = p[fit];
            wpx += ttf_text_width(one, px);
            if (wpx > max_w) break;
            fit++;
        }
        const bool more = p[fit] != 0;
        int take = (!more || fit >= (int)sizeof(buf) - 4) ? fit
                 : (last_sp > 0 ? last_sp : fit);
        if (take <= 0) take = 1;
        if (lines == max_lines - 1 && more && take < (int)strlen(p))
            snprintf(buf, sizeof buf, "%.*s...", take, p);   // last line: cut
        else
            snprintf(buf, sizeof buf, "%.*s", take, p);
        drawTTF((u32)x, (u32)(y + lines * pitch), buf, px, colour);
        p += take;
        while (*p == ' ') p++;
        lines++;
    }
    return lines;
}

// One pill chip (canvas: height 29, radius full, border 0.8, 8 px padding).
// GPU part: the shape.  Returns its width so the caller can place the next.
static int info_chip_width(const char *label, float px, int face) {
    return ttf_text_width_face(label, px, face) + UIS_W(20);
}

enum { F3_RESUME, F3_PLAY, F3_START, F3_WATCHED, F3_VERSION, F3_QUALITY };

// The details page's blocking loads, run behind loading_run().  Everything here
// is thread-safe: the item fetches use responseBuffer while the render thread
// only draws the loader, and detail_media carries its own decoder and buffers.
struct InfoLoad {
    const XMBItem  *it;
    XMBItemDetail  *detail;
    JFMediaSources *versions;
    int             pw, ph;
    Bitmap          poster, back;
    bool            back_ok;
};
static void info_load_work(void *arg) {
    InfoLoad *ld = (InfoLoad *)arg;
    jellyfin_fetch_item_detail(ld->it->id, ld->detail);
    if (strcmp(ld->it->type, "Movie") == 0 || strcmp(ld->it->type, "Episode") == 0 ||
        strcmp(ld->it->type, "Video") == 0)
        jellyfin_fetch_media_sources(ld->it->id, ld->versions);
    for (int v = 0; v < ld->versions->n_sources; v++)            // emoji -> gone
        ttf_clean_text(ld->versions->source[v].label, (int)sizeof ld->versions->source[v].label);
    // Episodes: the show's poster (the title card Home shows) and the
    // episode's own still -- its Primary image, 16:9 -- as the backdrop.  The
    // show's backdrop stands in when an episode has no still.
    const bool ep = strcmp(ld->it->type, "Episode") == 0 && ld->detail->series_id[0];
    const char *poster_id = ep ? ld->detail->series_id : ld->it->id;
    if (!detail_media_load(poster_id, "Primary", ld->pw, ld->ph, 0.5f, &ld->poster) && ep)
        detail_media_load(ld->it->id, "Primary", ld->pw, ld->ph, 0.5f, &ld->poster);
    ld->back_ok = ep && detail_media_load(ld->it->id, "Primary", 960, 540, 0.3f, &ld->back);
    // A Gelato title found by Search is the title item, whose "Backdrop" is
    // its poster again; its first version (what Home lists) carries the real
    // one.  Ask the version first when the two ids differ.
    if (!ld->back_ok && !ep && ld->versions->n_sources > 0 &&
        ld->versions->source[0].id[0] && strcmp(ld->versions->source[0].id, ld->it->id) != 0)
        ld->back_ok = detail_media_load(ld->versions->source[0].id, "Backdrop", 960, 540, 0.3f, &ld->back);
    if (!ld->back_ok)
        ld->back_ok = detail_media_load(ep ? ld->detail->series_id : ld->it->id,
                                        "Backdrop", 960, 540, 0.3f, &ld->back);
}

static void xmb_show_item_info_v3(const XMBItem *root) {
    rsxSync();
    flip();
    init_btns();
    // Hand back whatever level this was opened from: X on the base layer
    // opens detail too, and closing it should land there, not inside the tab.
    const int back_level = spine_target_level();

    // THE ARRIVAL.  Detail is the next depth, not a screen swap: its poster
    // flies out of the card it was opened from (the stage or the focus ring
    // noted that rect on the last frame) while the spine rises to L3 and the
    // backdrop fades in.  The move starts only once the first fetch has
    // landed -- see `arrived` below -- or the fetch's blocking time would
    // arrive as one huge frame and the spine would land in a single step.
    int  org_x = 0, org_y = 0, org_w = 0, org_h = 0;
    const bool  have_org = depth_last_focus_rect(&org_x, &org_y, &org_w, &org_h);
    const float d_from   = spine_depth();
    bool arrived = false;

    XMBItem cur_item = *root;
    XMBItemDetail detail;
    static JFMediaSources versions;
    int  version_sel = 0;
    bool reload = true, exit_armed = false, watched = false;
    Bitmap poster_cpu;               // CPU fallback when the upload fails
    memset(&poster_cpu, 0, sizeof poster_cpu);
    bool poster_gpu = false, back_gpu = false;

    // Row 0: the action buttons; row 1: the selectors.  Built per title.
    int row0[3], n0 = 0, row1[2], n1 = 0;
    int frow = 0, fcol = 0;

    // Layout, from the canvas.
    const int PX = IX(57), PY = IY(187), PW = UIS_W(216), PH = UIS_H(324);
    const int TX = IX(313);
    const int TW = UIS_W(700);

    while (running) {
        if (reload) {
            reload = false;
            const XMBItem *it = &cur_item;
            memset(&detail, 0, sizeof detail);
            if (facts_wanted(it->type)) facts_request(it->id);   // lands while this loads
            memset(&versions, 0, sizeof versions);
            detail_media_free(&poster_cpu);
            // The fetches run behind the LOADING screen (render/ui_buffering).
            // A flip is always pending here: the entry flip, or the last frame's.
            InfoLoad ld;
            memset(&ld, 0, sizeof ld);
            ld.it = it; ld.detail = &detail; ld.versions = &versions;
            ld.pw = PW; ld.ph = PH;
            loading_run(info_load_work, &ld, "Loading", true);
            {
                int remembered = vquality_for_item(it->id);
                if (remembered >= 0) vquality_set((vquality_t)remembered);
            }
            version_sel = 0;
            // Warm the server while the page is read: its first-time stream
            // lookup (Gelato, 6-8 s) then happens before Play, not after.
            if (strcmp(it->type, "Movie") == 0 || strcmp(it->type, "Episode") == 0 ||
                strcmp(it->type, "Video") == 0)
                jellyfin_warm_playback(it->id);
            watched = false;

            // Poster: into VRAM once.  Kept in main memory only if that fails.
            Bitmap poster = ld.poster;
            // The artwork's accent, sampled once from main memory before the
            // poster goes to VRAM (render/art_colour.h).  The buffering
            // screen's ring and glow use it when this title is played.
            {
                const art_palette pal = ui_art_palette(it->id, &poster);
                // ... and the wave takes a hint of it while the page is open
                // (steady, not breathing; cleared when the page closes).
                wave_set_art_tint(pal.valid ? pal.accent : 0u, 0.70f);
            }
            poster_gpu = ui_gpu_tex_upload(GPU_TEX_POSTER, &poster);
            if (poster_gpu) ui_gpu_tex_set_tag(GPU_TEX_POSTER, it->id);
            if (poster_gpu) detail_media_free(&poster);
            else            poster_cpu = poster;

            // Backdrop: 960x540 is plenty under a 0.88-0.96 scrim, and the RSX
            // scales it to the screen with linear filtering.  Freed as soon as
            // it is in VRAM -- the 2 MB never sits in main memory.
            Bitmap back = ld.back;
            back_gpu = false;
            if (ld.back_ok)
                back_gpu = ui_gpu_tex_upload(GPU_TEX_BACKDROP, &back);
            if (back_gpu) ui_gpu_tex_set_tag(GPU_TEX_BACKDROP, it->id);
            detail_media_free(&back);
            if (!back_gpu) ui_gpu_tex_clear(GPU_TEX_BACKDROP);

            thumb_cache_retarget();     // cast headshots come from the card cache

            const bool has_resume = it->resume_secs >= 10;
            n0 = 0;
            if (has_resume) { row0[n0++] = F3_RESUME; row0[n0++] = F3_START; }
            else            { row0[n0++] = F3_PLAY; }
            row0[n0++] = F3_WATCHED;
            n1 = 0;
            if (versions.n_sources > 1) row1[n1++] = F3_VERSION;
            row1[n1++] = F3_QUALITY;
            frow = 0; fcol = 0;
            exit_armed = false;
            init_btns();
            slog_state("INFO3_OPEN item_id=%s name=%.40s", it->id, it->name);
            if (!arrived) {
                arrived = true;
                spine_clock_reset();          // step from here, not from the fetch
                spine_set_level(SPINE_L3);
            }
        }
        const XMBItem *it = &cur_item;

        waitflip();
        sysUtilCheckCallback();
        poll_buttons();
        spine_frame_begin();

        const int focus = frow == 0 ? row0[fcol] : (frow == 1 ? row1[fcol] : -1);
        if (!exit_armed) {
            if (!btn_cur.circle && !btn_cur.triangle && !btn_cur.cross) exit_armed = true;
        } else {
            if (BTN_PRESSED(circle)) break;
            const int n = frow == 0 ? n0 : n1;
            const bool can_season = strcmp(it->type, "Episode") == 0 &&
                                    detail.series_id[0] && detail.season_id[0];
            // BTN_REPEAT is stateful (the first call per press consumes it),
            // so read each direction ONCE.  Reading Left twice -- here for
            // Back to Season, then for the move -- lost every Left press
            // once the focus left the first button (2026-09-27).
            const bool nav_l = BTN_REPEAT(left);
            const bool nav_r = BTN_REPEAT(right);
            if (frow == 2) {
                if (nav_r) { frow = 0; fcol = 0; }
            } else if (nav_l && fcol == 0 && frow == 0 && can_season) {
                frow = 2;
            } else {
            if (nav_l && fcol > 0)     fcol--;
            if (nav_r && fcol < n - 1) fcol++;
            }
            if (frow == 2 && BTN_PRESSED(cross)) {
                // Leave for the episode's season: TV tab -> the series ->
                // that season's episodes, this episode selected.
                XMBItem ser;
                memset(&ser, 0, sizeof ser);
                snprintf(ser.id, sizeof ser.id, "%s", detail.series_id);
                snprintf(ser.name, sizeof ser.name, "%s", detail.series_name[0] ? detail.series_name : "");
                snprintf(ser.type, sizeof ser.type, "Series");
                if (xmb_open_series(&ser)) {
                    for (int i = 0; i < g_tv_sub_count; i++) {
                        if (strcmp(g_tv_sub_items[i].id, detail.season_id) != 0) continue;
                        snprintf(g_tv_season_id, sizeof g_tv_season_id, "%s", g_tv_sub_items[i].id);
                        snprintf(g_tv_season_name, sizeof g_tv_season_name, "%s", g_tv_sub_items[i].name);
                        g_tv_sub_start = 0; g_tv_sub_total = 0;
                        g_tv_sub_count = xmb_fetch_episodes(g_tv_series_id, g_tv_season_id,
                                                            g_tv_sub_items, XMB_ITEMS_MAX,
                                                            0, &g_tv_sub_total);
                        g_tv_depth = 2; g_tv_sub_sel = 0; g_tv_sub_scroll = 0;
                        for (int e = 0; e < g_tv_sub_count; e++)
                            if (strcmp(g_tv_sub_items[e].id, it->id) == 0) { g_tv_sub_sel = e; break; }
                        break;
                    }
                }
                init_btns();
                break;
            }
            if (BTN_PRESSED(down) && frow == 0 && n1 > 0) { frow = 1; if (fcol >= n1) fcol = n1 - 1; }
            if (BTN_PRESSED(up)   && frow == 1)           { frow = 0; if (fcol >= n0) fcol = n0 - 1; }
            if (frow != 2 && BTN_PRESSED(cross)) {
                if (focus == F3_RESUME || focus == F3_PLAY || focus == F3_START) {
                    const u32 at = focus == F3_RESUME ? it->resume_secs : 0;
                    const char *source_id = versions.n_sources > 0
                        ? versions.source[version_sel].id : NULL;
                    if (strcmp(it->type, "Episode") == 0)
                        xmb_play_episode_with_next(it, at, source_id);
                    else
                        xmb_play_item(it, at, source_id);
                    exit_armed = false;
                    init_btns();
                    info_skip_frame();
                    continue;
                } else if (focus == F3_WATCHED) {
                    if (!watched) watched = info_mark_played(it->id);
                } else if (focus == F3_VERSION) {
                    int chosen = info_choose_version(it->name, &versions, version_sel);
                    if (chosen >= 0) version_sel = chosen;
                    exit_armed = false;
                    init_btns();
                    info_skip_frame();
                    continue;
                } else if (focus == F3_QUALITY) {
                    vquality_next(+1);
                    vquality_remember_item(it->id, vquality_get());
                }
            }
        }

        thumb_cache_tick();
        clearScreen(XMB_BG);
        wave_draw();

        // ---- GPU phase ---------------------------------------------------
        const int W = (int)display_width, H = (int)display_height;
        // How far the arrival has got: 0 where it was opened, 1 at L3.
        const float arr = d_from < 1.999f
            ? spine_ease(spine_clampf((spine_depth() - d_from) / (2.0f - d_from), 0.0f, 1.0f))
            : 1.0f;
        int PXa = PX, PYa = PY, PWa = PW, PHa = PH;
        if (have_org && arr < 1.0f) {
            PXa = org_x + (int)((float)(PX - org_x) * arr + 0.5f);
            PYa = org_y + (int)((float)(PY - org_y) * arr + 0.5f);
            PWa = org_w + (int)((float)(PW - org_w) * arr + 0.5f);
            PHa = org_h + (int)((float)(PH - org_h) * arr + 0.5f);
        }
        if (back_gpu) ui_gpu_tex_draw_a(GPU_TEX_BACKDROP, 0, 0, W, H, depth_a8(arr));
        {
            // 90deg rgba(3,4,8) .96 -> .88 @46% -> .34, and a 200 px bottom
            // scrim to .95.  Without a backdrop they still sit over the wave,
            // which keeps the text contrast the same either way.
            static const float hp[3] = { 0.0f, 0.46f, 1.0f };
            static const u8    ha[3] = { 245, 224, 87 };
            wave_draw_ramp_gpu(0, 0, W, H, 0x00030408, false, 3, hp, ha);
            static const float vp[2] = { 0.0f, 1.0f };
            static const u8    va[2] = { 0, 242 };
            wave_draw_ramp_gpu(0, IY(519), W, H - IY(519), 0x00030408, true, 2, vp, va);
        }
        spine_draw_gpu();
        {
            const spine_level L = spine_eval(spine_depth());
            const u8 da = (u8)(72.0f * L.divider_a);
            if (da) wave_draw_divider_gpu(IY((int)L.divider_y), 0x8A, 0x93, 0xC8, da);
        }
        if (poster_gpu) ui_gpu_tex_draw(GPU_TEX_POSTER, PXa, PYa, PWa, PHa);

        // Chips (y=262, h=29, pill).
        // The facts worker's words when they have arrived ("DTS-HD MA 5.1",
        // "1080P H.264"), the detail fetch's DisplayTitle until then.  The
        // audio chip keeps its blue outline for lossless audio only: blue is
        // "state the file has", and lossless is the state worth stating.
        char achip[96] = "", vchip[64] = "", pchip[48] = "";
        ItemFacts fx;
        const bool hfx = facts_get(it->id, &fx, NULL);
        bool a_ll = true;
        if (hfx && fx.audio[0]) { snprintf(achip, sizeof achip, "%s", fx.audio); a_ll = fx.lossless; }
        // 2026-09-27: the chosen VERSION's own default audio track, so the
        // chip changes as the version does.  "English - DTS-HD MA - 7.1 -
        // Default" -> "DTS-HD MA 7.1"; blue edge for the lossless ones.
        if (version_sel >= 0 && version_sel < versions.n_sources) {
            const JFTracks *tk = &versions.source[version_sel].tracks;
            if (tk->n_audio > 0) {
                int d = tk->default_audio;
                if (d < 0 || d >= tk->n_audio) d = 0;
                const char *src = tk->audio[d].label;
                char tmp[96]; snprintf(tmp, sizeof tmp, "%s", src);
                char outc[96] = ""; int seg = 0, nseg = 1;
                for (const char *q = tmp; (q = strstr(q, " - ")) != NULL; q += 3) nseg++;
                char *tokp = tmp;
                while (tokp) {
                    char *nx = strstr(tokp, " - ");
                    if (nx) *nx = 0;
                    const bool skip = (seg == 0 && nseg > 1) || !strcmp(tokp, "Default") ||
                                      !strcmp(tokp, "Forced") || !strcmp(tokp, "External");
                    if (!skip && tokp[0]) {
                        const size_t L = strlen(outc);
                        snprintf(outc + L, sizeof outc - L, "%s%s", L ? " " : "", tokp);
                    }
                    seg++;
                    tokp = nx ? nx + 3 : NULL;
                }
                if (outc[0]) {
                    snprintf(achip, sizeof achip, "%s", outc);
                    a_ll = strstr(outc, "TrueHD") || strstr(outc, "DTS-HD MA") || strstr(outc, "MLP") ||
                           strstr(outc, "DTS:X") || strstr(outc, "FLAC") || strstr(outc, "PCM");
                }
            }
        }
        else if (detail.audio_info[0]) info_audio_chip(detail.audio_info, achip, sizeof achip);
        if (hfx && fx.video[0]) snprintf(vchip, sizeof vchip, "%s", fx.video);
        else if (detail.video_info[0]) snprintf(vchip, sizeof vchip, "%s", detail.video_info);
        {
            u32 qw = 0, qh = 0; unsigned qbr = 0;
            vquality_params(vquality_get(), hd1080_enabled(), display_width,
                            display_height, &qw, &qh, NULL, NULL, &qbr);
            if (qbr == 0) snprintf(pchip, sizeof pchip, "direct play");
            else          snprintf(pchip, sizeof pchip, "transcode %u Mbps", qbr / 1000000u);
        }
        const int CY = IY(262), CH = UIS_H(29), CG = UIS_W(8);
        const float apx = UIS_TF(10.0f), cpx = UIS_TF(11.0f);
        int cx = TX;
        // 2026-09-27: the audio chip in the regular face and the video chip in
        // the spec face (swapped), and no "direct play / transcode" chip.
        (void)pchip;
        const int aw = achip[0] ? info_chip_width(achip, cpx, UI_FACE_REGULAR) : 0;
        const float vpx = UIS_TF(12.5f);                 // the video chip's text: bigger (user, 09-27)
        const int vw = vchip[0] ? info_chip_width(vchip, vpx, UI_FACE_SPEC) : 0;
        if (aw) { wave_draw_rrect_outline_gpu(cx, CY, aw, CH, CH / 2, 1,
                                              a_ll ? XMB_ACCENT_ALT : XMB_HAIRLINE, 255,
                                              XMB_PANEL, XMB_PANEL, 255); cx += aw + CG; }
        if (vw) { wave_draw_rrect_outline_gpu(cx, CY, vw, CH, CH / 2, 1, XMB_HAIRLINE, 255,
                                              XMB_PANEL, XMB_PANEL, 255); cx += vw + CG; }

        // Action row (y=410, h=44, r=4, gaps 12).  Resume/Play is the one
        // primary: the accent->accent_alt ramp with its glow; the others are
        // panels.  The focused control gets the focus ring, whichever it is.
        // 2026-09-27: the row moved down into the wave's band (was y=410, above
        // the cast), and every button is see-through so the wave moves behind
        // it.  The director line and the cast moved up into its old place.
        const int AY = IY(562), AH = UIS_H(44), AR = UIS_H(4), AG = UIS_W(12);
        int ax[3], aww[3];
        {
            int x = TX;
            for (int i = 0; i < n0; i++) {
                const int id = row0[i];
                aww[i] = UIS_W(id == F3_RESUME ? 200 : id == F3_PLAY ? 150
                             : id == F3_START ? 150 : 168);
                ax[i] = x;
                x += aww[i] + AG;
                const bool primary = (id == F3_RESUME || id == F3_PLAY);
                const bool foc = (frow == 0 && fcol == i);
                if (primary) {
                    wave_draw_glow_gpu(ax[i] + aww[i] / 2, AY + AH / 2,
                                       aww[i] / 2 + UIS_W(24), AH / 2 + UIS_H(24),
                                       (u8)((XMB_ACCENT >> 16) & 0xFF),
                                       (u8)((XMB_ACCENT >> 8) & 0xFF),
                                       (u8)(XMB_ACCENT & 0xFF), 60);
                    wave_draw_rrect_outline_gpu(ax[i], AY, aww[i], AH, AR, 1,
                                                XMB_ACCENT_ALT, 230,
                                                XMB_ACCENT, XMB_ACCENT_ALT, 105);
                } else {
                    wave_draw_rrect_outline_gpu(ax[i], AY, aww[i], AH, AR, 1,
                                                XMB_HAIRLINE, 255,
                                                XMB_PANEL, XMB_PANEL, 110);
                }
                // The card focus ring, gliding between controls the way it
                // glides between posters (spine_focus_ring_gpu).
                if (foc) spine_focus_ring_gpu(ax[i], AY, aww[i], AH);
            }
        }

        // "Back to Season N" under the poster (episodes).  y 523, poster width.
        const bool season_btn = strcmp(it->type, "Episode") == 0 &&
                                detail.series_id[0] && detail.season_id[0];
        const int BSY = IY(523), BSH = UIS_H(34);
        if (season_btn) {
            wave_draw_rrect_outline_gpu(PX, BSY, PW, BSH, AR, 1, XMB_HAIRLINE, 255,
                                        XMB_PANEL, XMB_PANEL, 110);
            if (frow == 2) spine_focus_ring_gpu(PX, BSY, PW, BSH);
        }

        // Selectors (y=634, h=34, r=4).
        const int SY = IY(634), SH = UIS_H(34);
        int sx_[2], sw_[2];
        {
            int x = TX;
            for (int i = 0; i < n1; i++) {
                sw_[i] = UIS_W(row1[i] == F3_VERSION ? 172 : 216);
                sx_[i] = x;
                x += sw_[i] + UIS_W(12);
                wave_draw_rrect_outline_gpu(sx_[i], SY, sw_[i], SH, AR, 1,
                                            XMB_HAIRLINE, 255, XMB_PANEL, XMB_PANEL, 255);
                if (frow == 1 && fcol == i) spine_focus_ring_gpu(sx_[i], SY, sw_[i], SH);
            }
        }

        rsxSync();
        ui_text_gpu_begin();

        // ---- CPU phase ---------------------------------------------------
        if (!poster_gpu) {
            if (poster_cpu.pixels) info_blit(&poster_cpu, PX, PY);
            else xmb_cpu_blit_thumb_scaled(it->id, PX, PY, PW, PH);
        }
        // 1 px hairline ring just outside the poster (the canvas's box-shadow),
        // following it while it arrives.
        if (PXa >= 1 && PYa >= 1) {
            drawRect((u32)(PXa - 1), (u32)(PYa - 1), (u32)(PWa + 2), 1, XMB_HAIRLINE);
            drawRect((u32)(PXa - 1), (u32)(PYa + PHa), (u32)(PWa + 2), 1, XMB_HAIRLINE);
            drawRect((u32)(PXa - 1), (u32)(PYa - 1), 1, (u32)(PHa + 2), XMB_HAIRLINE);
            drawRect((u32)(PXa + PWa), (u32)(PYa - 1), 1, (u32)(PHa + 2), XMB_HAIRLINE);
        }

        // Cast: 58x87 (2:3) portraits on a 104 px pitch from x=313, y=493.  The
        // canvas has no crew here except the director, who gets the line
        // above, so crew entries are skipped.
        const char *director = NULL;
        int cast_idx[5], n_cast = 0;
        for (int i = 0; i < detail.n_people; i++) {
            if (strcmp(detail.people[i].role, "Director") == 0) {
                if (!director) director = detail.people[i].name;
                continue;
            }
            if (n_cast < 5) cast_idx[n_cast++] = i;
        }
        // Portraits in the XMB's own shape: 2:3 cards (a headshot's native
        // aspect, so no face is cropped) with a hairline frame, not circles.
        const int KY = IY(426), KW = UIS_W(58), KD = UIS_H(87), KP = UIS_W(104);   // was 493
        for (int k = 0; k < n_cast; k++) {
            const int kx = TX + k * KP;
            if (!xmb_cpu_blit_thumb(detail.people[cast_idx[k]].id, kx, KY, KW, KD))
                drawRect((u32)kx, (u32)KY, (u32)KW, (u32)KD, XMB_THUMB_DIM);
            drawRect((u32)(kx - 1), (u32)(KY - 1), (u32)(KW + 2), 1, XMB_HAIRLINE);
            drawRect((u32)(kx - 1), (u32)(KY + KD), (u32)(KW + 2), 1, XMB_HAIRLINE);
            drawRect((u32)(kx - 1), (u32)(KY - 1), 1, (u32)(KD + 2), XMB_HAIRLINE);
            drawRect((u32)(kx + KW), (u32)(KY - 1), 1, (u32)(KD + 2), XMB_HAIRLINE);
        }

        // ---- text --------------------------------------------------------
        xmb_draw_topbar();
        spine_draw();

        // Title: 25px/700 display face, -0.01em.
        {
            const float tpx = UIS_TF(25.0f), trk = tpx * -0.01f;
            char tbuf[132];
            snprintf(tbuf, sizeof tbuf, "%s", it->name);
            int len = (int)strlen(tbuf);
            while (len > 1 && ttf_text_width_tracked(tbuf, tpx, UI_FACE_DISPLAY, trk) > TW)
                tbuf[--len] = 0;
            drawTTF_tracked((u32)TX, (u32)PY, tbuf, tpx, XMB_TEXT, UI_FACE_DISPLAY, trk);
        }

        // Meta row: 13.5 text_dim, separators at 50%, rating in accent_alt.
        {
            const float mpx = UIS_TF(13.5f);
            const u32 sep_c = info_mix(XMB_TEXT_DIM, XMB_BG, 0.5f);
            const char *parts[4] = { it->year_str, it->duration_str,
                                     detail.official_rating, detail.genres };
            int mx = TX;
            bool first = true;
            for (int i = 0; i < 4; i++) {
                if (!parts[i] || !parts[i][0]) continue;
                if (!first) {
                    drawTTF((u32)(mx + UIS_W(6)), (u32)IY(228), "\xC2\xB7", mpx, sep_c);
                    mx += UIS_W(18);
                }
                drawTTF((u32)mx, (u32)IY(228), parts[i], mpx, XMB_TEXT_DIM);
                mx += ttf_text_width(parts[i], mpx);
                first = false;
            }
            if (detail.community_rating[0]) {
                if (!first) {
                    drawTTF((u32)(mx + UIS_W(6)), (u32)IY(228), "\xC2\xB7", mpx, sep_c);
                    mx += UIS_W(18);
                }
                drawTTF((u32)mx, (u32)IY(228), detail.community_rating, mpx, XMB_ACCENT_ALT);
            }
        }

        // Chip labels, vertically centred in the 29 px pills.
        {
            int x = TX;
            const int ty_s = CY + (CH - (int)apx) / 2 - UIS_H(1);
            const int ty_r = CY + (CH - (int)cpx) / 2 - UIS_H(1);
            if (aw) { drawTTF((u32)(x + UIS_W(10)), (u32)ty_r, achip, cpx,
                              a_ll ? XMB_ACCENT_ALT : XMB_TEXT_DIM);
                      x += aw + CG; }
            if (vw) { const float vpx = UIS_TF(12.5f);
                      drawTTF_face((u32)(x + UIS_W(10)), (u32)(CY + (CH - (int)vpx) / 2 - UIS_H(1)),
                                   vchip, vpx, XMB_TEXT_DIM, UI_FACE_SPEC);
                      x += vw + CG; }
            (void)x;                    // (the transcode chip is gone)
        }

        // Overview: 14 px text_dim, 620 wide, four lines at 1.55.
        if (detail.overview[0])
            info_wrap(TX, IY(304), detail.overview, UIS_TF(14.0f), XMB_TEXT_DIM,
                      UIS_W(620), UIS_H(22), 4);

        // Button labels.
        for (int i = 0; i < n0; i++) {
            const int id = row0[i];
            char lab[40];
            if (id == F3_RESUME) {
                const u32 s = it->resume_secs;
                snprintf(lab, sizeof lab, "Resume %u:%02u:%02u",
                         s / 3600u, (s / 60u) % 60u, s % 60u);
            } else if (id == F3_PLAY)    snprintf(lab, sizeof lab, "Play");
            else if (id == F3_START)     snprintf(lab, sizeof lab, "Play from start");
            else                         snprintf(lab, sizeof lab, watched ? "Watched" : "Mark as watched");
            const bool primary = (id == F3_RESUME || id == F3_PLAY);
            const float bpx = UIS_TF(15.0f);
            const int lw = ttf_text_width(lab, bpx, primary);
            int lx = ax[i] + (aww[i] - lw) / 2;
            if (primary) {
                // The 22 px cross glyph sits at the left, the label after it.
                const int gx = ax[i] + UIS_W(20);
                draw_ps_button_vcentered((u32)gx, AY + AH / 2, 'X', UIS_H(22), 0x00FFFFFF);
                lx = gx + UIS_W(32);
            }
            drawTTF((u32)lx, (u32)(AY + (AH - (int)bpx) / 2 - UIS_H(1)), lab, bpx,
                    primary ? XMB_WHITE : XMB_TEXT, primary);
        }

        if (season_btn) {
            char sl[48];
            if (detail.season_num == 0)      snprintf(sl, sizeof sl, "\xE2\x80\xB9  Back to Specials");
            else if (detail.season_num > 0)  snprintf(sl, sizeof sl, "\xE2\x80\xB9  Back to Season %d", detail.season_num);
            else                             snprintf(sl, sizeof sl, "\xE2\x80\xB9  Back to Season");
            const float spx = UIS_TF(13.0f);
            const int sw = ttf_text_width(sl, spx, frow == 2);
            drawTTF((u32)(PX + (PW - sw) / 2), (u32)(BSY + (BSH - (int)spx) / 2 - UIS_H(1)), sl, spx,
                    frow == 2 ? XMB_WHITE : XMB_TEXT, frow == 2);
        }

        // Director line.
        if (director) {
            char line[140];
            snprintf(line, sizeof line, "Director \xC2\xB7 %s", director);
            drawTTF((u32)TX, (u32)IY(402), line, UIS_TF(13.0f), XMB_TEXT_FAINT);   // was 467
        }

        // Cast names: actor above character, Satoshi one weight.
        for (int k = 0; k < n_cast; k++) {
            const JFPerson *pp = &detail.people[cast_idx[k]];
            const int x = TX + k * KP;
            info_clip_text(x, KY + KD + UIS_H(6), pp->name, UIS_TF(12.0f), XMB_TEXT,
                           KP - UIS_W(8), false, UI_FACE_TAB_REG);
            if (pp->role[0])
                info_clip_text(x, KY + KD + UIS_H(22), pp->role, UIS_TF(11.5f),
                               XMB_TEXT_DIM, KP - UIS_W(8), false, UI_FACE_TAB_REG);
        }

        // Selector labels.
        for (int i = 0; i < n1; i++) {
            const int y = SY + (SH - (int)UIS_TF(12.0f)) / 2 - UIS_H(1);
            const char *name = row1[i] == F3_VERSION ? "Version" : "Quality";
            drawTTF((u32)(sx_[i] + UIS_W(15)), (u32)y, name, UIS_TF(12.0f), XMB_TEXT_DIM);
            char val[64];
            if (row1[i] == F3_VERSION) {
                snprintf(val, sizeof val, "%s", versions.source[version_sel].summary[0]
                                                  ? versions.source[version_sel].summary
                                                  : versions.source[version_sel].label);
            } else {
                u32 qw = 0, qh = 0; unsigned qbr = 0;
                const vquality_t vq = vquality_get();
                vquality_params(vq, hd1080_enabled(), display_width, display_height,
                                &qw, &qh, NULL, NULL, &qbr);
                if (qbr == 0) snprintf(val, sizeof val, "Original \xC2\xB7 direct play");
                else          snprintf(val, sizeof val, "%s \xC2\xB7 %u Mbps",
                                       vquality_label(vq), qbr / 1000000u);
            }
            const int vx = sx_[i] + UIS_W(row1[i] == F3_VERSION ? 70 : 66);
            info_clip_text(vx, y, val, UIS_TF(12.5f), XMB_TEXT,
                           sx_[i] + sw_[i] - vx - UIS_W(24), true);
            drawTTF((u32)(sx_[i] + sw_[i] - UIS_W(18)), (u32)(y + UIS_H(1)),
                    "\xE2\x96\xBE", UIS_TF(10.0f), XMB_TEXT_FAINT);
        }

        // Hints: what X does on the focused control, and Back.
        {
            Hint h[2];
            h[0].glyph = 'X';
            h[0].label = focus == F3_RESUME ? "Resume" : focus == F3_PLAY ? "Play"
                       : focus == F3_START ? "Play from start"
                       : focus == F3_WATCHED ? "Mark watched"
                       : focus == F3_VERSION ? "Choose version" : "Change quality";
            h[1].glyph = 'C'; h[1].label = "Back";
            draw_hints_bar(h, 2);
        }

        ui_text_gpu_flush();
        flip();
    }

    detail_media_free(&poster_cpu);
    ui_gpu_tex_clear(GPU_TEX_POSTER);
    ui_gpu_tex_clear(GPU_TEX_BACKDROP);
    wave_set_art_tint(0u, 0.0f);          // back to the plain palette
    spine_set_level(back_level);
    slog_state("INFO3_CLOSE");
    g_info_cooldown_until = timing_get_us() + 500000;
    init_btns();
}

void xmb_show_item_info(const XMBItem *root) {
    // The redesigned page (above) whenever the spine is on; this pre-revamp
    // page is what jellyfin_spine.txt=0 still gets.
    if (g_spine_on) { xmb_show_item_info_v3(root); return; }
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
    const int top = XMB_OY + XMB_TOPBAR_H + UIS_H(12);
    const int content_bot = (int)display_height - XMB_BOTTOM_PAD;
    int poster_h = content_bot - top;
    // v1.0: 216x324 at the authoring size (the 2:3 below gives the width).
    if (poster_h > UIS_H(324)) poster_h = UIS_H(324);
    const int poster_w = poster_h * 2 / 3;
    const int poster_x = X, poster_y = top;
    const int tx    = poster_x + poster_w + UIS_W(28);   // text column left edge
    const int max_w = (int)display_width - tx - X;
    const int back_w = (int)display_width;
    const int band_y0 = top - UIS_H(14);                 // purple title-band extent
    const int band_y1 = top + UIS_H(96);
    const int view_bottom = content_bot;
    const int SIM_CH = UIS_H(222);                // More Like This card height

    // ---- Per-title state, (re)loaded whenever the nav stack moves ----
    XMBItemDetail detail;
    memset(&detail, 0, sizeof(detail));
    Bitmap hero_poster;
    memset(&hero_poster, 0, sizeof(hero_poster));
    XMBItem similar[INFO_SIMILAR_MAX];
    int  n_similar = 0;
    // Static keeps the up-to-32-source table out of the PPU stack.  It is
    // discarded/reused whenever this page navigates to another title.
    static JFMediaSources versions;
    int version_sel = 0;
    bool reload    = true;

    // The page is taller than the screen; d-pad up/down jumps to top/bottom.
    // max_scroll is recomputed from the laid-out content height each frame.
    int scroll_y = 0, max_scroll = 0;
    // Focus moves between the Play button and the More Like This row, which is
    // what Cross acts on.  (Gating this on "is the row visible" made Cross mean
    // Open even at the top of the page, so Play could never be pressed.)
    enum { FOCUS_PLAY = 0, FOCUS_VERSION = 1, FOCUS_QUALITY = 2, FOCUS_SIM = 3 };
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
            // Re-apply the quality last chosen for THIS title, if any.
            // Done on load rather than at play time so the info row shows
            // what will actually be requested.
            {
                int remembered = vquality_for_item(cur->id);
                if (remembered >= 0) vquality_set((vquality_t)remembered);
            }
            memset(&versions, 0, sizeof(versions));
            if (strcmp(cur->type, "Movie") == 0 ||
                strcmp(cur->type, "Episode") == 0 ||
                strcmp(cur->type, "Video") == 0)
                jellyfin_fetch_media_sources(cur->id, &versions);
            for (int v = 0; v < versions.n_sources; v++)
                ttf_clean_text(versions.source[v].label, (int)sizeof versions.source[v].label);
            version_sel = 0;
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
            // Row order down the page: Play, Version (only when there is a
            // real choice), Quality (always), then the recommendations.
            if (BTN_PRESSED(down)) {
                if (focus == FOCUS_PLAY && versions.n_sources > 1) {
                    focus = FOCUS_VERSION;
                    scroll_y = 0;
                } else if (focus == FOCUS_PLAY || focus == FOCUS_VERSION) {
                    focus = FOCUS_QUALITY;
                    scroll_y = 0;
                } else if (focus == FOCUS_QUALITY && n_similar > 0) {
                    focus = FOCUS_SIM;
                    scroll_y = max_scroll;
                }
            }
            if (BTN_PRESSED(up)) {
                if (focus == FOCUS_SIM)
                    focus = FOCUS_QUALITY;
                else if (focus == FOCUS_QUALITY && versions.n_sources > 1)
                    focus = FOCUS_VERSION;
                else if (focus == FOCUS_QUALITY || focus == FOCUS_VERSION)
                    focus = FOCUS_PLAY;
                scroll_y = 0;
            }
            if (focus == FOCUS_SIM) {
                if (BTN_REPEAT(left)  && sim_sel > 0)             sim_sel--;
                if (BTN_REPEAT(right) && sim_sel < n_similar - 1) sim_sel++;
            } else if (focus == FOCUS_VERSION && versions.n_sources > 1) {
                if (BTN_REPEAT(left) && version_sel > 0) version_sel--;
                if (BTN_REPEAT(right) && version_sel < versions.n_sources - 1)
                    version_sel++;
            } else if (focus == FOCUS_QUALITY) {
                // Persisted immediately, so the choice carries to the next
                // title the way the web player's quality dropdown does.
                // Each BTN_REPEAT read consumes the press: read once.
                const bool ql = BTN_REPEAT(left);
                const bool qr = BTN_REPEAT(right);
                if (ql || qr) {
                    vquality_next(ql ? -1 : +1);
                    // Remember it for this title as well as globally, so
                    // coming back to a heavy remux does not mean re-picking.
                    vquality_remember_item(cur_item.id, vquality_get());
                }
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
                } else if (focus == FOCUS_VERSION) {
                    int chosen = info_choose_version(it->name, &versions,
                                                     version_sel);
                    if (chosen >= 0) version_sel = chosen;
                    exit_armed = false;
                    init_btns();
                    info_skip_frame();
                    continue;
                } else if (focus == FOCUS_QUALITY) {
                    // X steps the value, same as Right.  Deliberately NO
                    // `continue` here: this frame still has to reach its
                    // flip() below, or waitflip() at the top of the next
                    // iteration spins on a flip that was never queued and
                    // the UI hangs (see info_skip_frame's note above).
                    vquality_next(+1);
                } else {
                    // Play — same launch flow as the grid (resume prompt first).
                    int resume = xmb_resume_choice(it);
                    if (resume >= 0) {
                        const char *source_id = versions.n_sources > 0
                            ? versions.source[version_sel].id : NULL;
                        if (strcmp(it->type, "Episode") == 0)
                            xmb_play_episode_with_next(it, (u32)resume,
                                                       source_id);
                        else
                            xmb_play_item(it, (u32)resume, source_id);
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
                     (u32)(poster_w + UIS_W(2)), 1, XMB_HAIRLINE);
            drawRect((u32)(poster_x - 1), (u32)(py + poster_h),
                     (u32)(poster_w + UIS_W(2)), 1, XMB_HAIRLINE);
            drawRect((u32)(poster_x - 1), (u32)(py - 1),
                     1, (u32)(poster_h + UIS_H(2)), XMB_HAIRLINE);
            drawRect((u32)(poster_x + poster_w), (u32)(py - 1),
                     1, (u32)(poster_h + UIS_H(2)), XMB_HAIRLINE);

            // ---- Right column: title + metadata + text, to the poster's right.
            int Y = top - scroll_y;

            // Title.  v1.0 puts this on --font-display at 25px/700 (it was 40px
            // on the bold system face), so the poster stops competing with it.
            //
            // The display face is a subset -- see UI_FACE_DISPLAY in
            // ui_visuals.h -- which is exactly why the truncation loop has to
            // measure through ttf_text_width_face on the SAME face it draws
            // with: a hyphen costs Rodin's advance here, not zero.
            {
                char tbuf[132];
                snprintf(tbuf, sizeof(tbuf), "%s", it->name);
                const float tpx  = UIS_TF(25);
                // v1.0 tracks titles -0.01em (tighter).  Applied HERE and not
                // to card titles: a tracked draw takes the per-glyph CPU path,
                // because ui_text_gpu.cpp keys a cached run on
                // (text, px, face, colour) with no room for tracking.  One hero
                // title per frame is free; ten card titles would not be, and
                // -0.25px per letter gap does not buy back the run cache.
                const float ttrk = tpx * -0.01f;
                int len = (int)strlen(tbuf);
                while (len > 1 &&
                       ttf_text_width_tracked(tbuf, tpx, UI_FACE_DISPLAY, ttrk) > max_w)
                    tbuf[--len] = 0;
                drawTTF_tracked((u32)tx, (u32)Y, tbuf, tpx, XMB_WHITE,
                                UI_FACE_DISPLAY, ttrk);
            }
            Y += UIS_H(40);

            // Meta row: year · duration, official-rating chip, ★ rating.
            {
                int mx = tx;
                char meta[64] = "";
                if (it->year_str[0])
                    snprintf(meta, sizeof(meta), "%s", it->year_str);
                if (it->duration_str[0]) {
                    if (meta[0]) strncat(meta, " \xC2\xB7 ", sizeof(meta)-strlen(meta)-1);
                    strncat(meta, it->duration_str, sizeof(meta)-strlen(meta)-1);
                }
                if (meta[0]) {
                    drawTTF((u32)mx, (u32)Y, meta, UIS_TF(18), XMB_TEXT_DIM);
                    mx += ttf_text_width(meta, UIS_TF(18)) + 18;
                }
                if (detail.official_rating[0]) {
                    // Outlined chip.
                    int cw = ttf_text_width(detail.official_rating, UIS_TF(13)) + UIS_W(16);
                    int ch = UIS_H(22);
                    drawRect((u32)mx, (u32)Y, (u32)cw, 1, XMB_HAIRLINE);
                    drawRect((u32)mx, (u32)(Y + ch - 1), (u32)cw, 1, XMB_HAIRLINE);
                    drawRect((u32)mx, (u32)Y, 1, (u32)ch, XMB_HAIRLINE);
                    drawRect((u32)(mx + cw - 1), (u32)Y, 1, (u32)ch, XMB_HAIRLINE);
                    drawTTF((u32)(mx + UIS_W(8)), (u32)(Y + UIS_H(3)), detail.official_rating,
                            UIS_TF(13), XMB_TEXT_DIM);
                    mx += cw + 18;
                }
                if (detail.community_rating[0]) {
                    // Rating gold is semantic, not palette: a star reads as gold
                    // in every theme, the way a warning reads as amber.
                    drawIcon((u32)mx, (u32)(Y + 1), ICON_STAR, UIS_TF(18.0f), 0x00E8B64CUL);
                    drawTTF((u32)(mx + UIS_W(24)), (u32)Y, detail.community_rating,
                            UIS_TF(18), XMB_TEXT_DIM);
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
                    drawRect((u32)(tx - UIS_W(2)), (u32)(Y - UIS_H(2)), (u32)(bw + UIS_W(4)), UIS_H(2), XMB_FOCUS_RING);
                    drawRect((u32)(tx - UIS_W(2)), (u32)(Y + bh), (u32)(bw + UIS_W(4)), UIS_H(2), XMB_FOCUS_RING);
                    drawRect((u32)(tx - UIS_W(2)), (u32)(Y - UIS_H(2)), UIS_W(2), (u32)(bh + UIS_H(4)), XMB_FOCUS_RING);
                    drawRect((u32)(tx + bw), (u32)(Y - UIS_H(2)), UIS_W(2), (u32)(bh + UIS_H(4)), XMB_FOCUS_RING);
                }
                u32 fg = pf ? XMB_KEY_LABEL_SEL : XMB_TEXT_DIM;
                drawIcon((u32)(tx + UIS_W(16)), (u32)(Y + (bh - UIS_H(22)) / 2), ICON_PLAY,
                         UIS_TF(22.0f), fg);
                drawTTF((u32)(tx + UIS_W(46)), (u32)(Y + (bh - UIS_H(20)) / 2 + 1), "Play",
                        UIS_TF(20), fg, true);
                Y += bh + 18;
            }

            // Version selector is shown only when it has a real choice.  X
            // opens the full scrollable list; Left/Right also step through it.
            if (versions.n_sources > 1) {
                const int bh = UIS_H(44);
                const int bw = max_w > 680 ? 680 : max_w;
                const bool vf = (focus == FOCUS_VERSION);
                drawRect((u32)tx, (u32)Y, (u32)bw, (u32)bh,
                         vf ? XMB_PANEL_HI : XMB_PANEL);
                if (vf) {
                    drawRect((u32)(tx - UIS_W(4)), (u32)Y, UIS_W(3), (u32)bh, XMB_ACCENT);
                    drawRect((u32)(tx - 1), (u32)(Y - 1), (u32)(bw + UIS_W(2)), 1,
                             XMB_HAIRLINE);
                    drawRect((u32)(tx - 1), (u32)(Y + bh), (u32)(bw + UIS_W(2)), 1,
                             XMB_HAIRLINE);
                }
                drawTTF_vcentered((u32)(tx + UIS_W(16)), Y + bh / 2, "Version", UIS_TF(16),
                                  vf ? XMB_ACCENT : XMB_TEXT_FAINT, true);
                info_clip_text(tx + UIS_W(118), Y + UIS_H(11),
                               versions.source[version_sel].label, UIS_TF(18),
                               vf ? XMB_WHITE : XMB_TEXT,
                               bw - UIS_W(166), vf);
                char pos[20];
                snprintf(pos, sizeof(pos), "%d/%d", version_sel + 1,
                         versions.n_sources);
                int pw = ttf_text_width(pos, UIS_TF(15));
                drawTTF_vcentered((u32)(tx + bw - pw - UIS_W(14)), Y + bh / 2,
                                  pos, UIS_TF(15), XMB_TEXT_DIM);
                Y += bh + 18;
            }

            // Quality selector — always shown, because unlike Version it is
            // always a real choice.  Left/Right (or X) step it; the value is
            // persisted, so it applies to this title and the next one.
            {
                const int bh = UIS_H(44);
                const int bw = max_w > 680 ? 680 : max_w;
                const bool qf = (focus == FOCUS_QUALITY);
                drawRect((u32)tx, (u32)Y, (u32)bw, (u32)bh,
                         qf ? XMB_PANEL_HI : XMB_PANEL);
                if (qf) {
                    drawRect((u32)(tx - UIS_W(4)), (u32)Y, UIS_W(3), (u32)bh, XMB_ACCENT);
                    drawRect((u32)(tx - 1), (u32)(Y - 1), (u32)(bw + UIS_W(2)), 1,
                             XMB_HAIRLINE);
                    drawRect((u32)(tx - 1), (u32)(Y + bh), (u32)(bw + UIS_W(2)), 1,
                             XMB_HAIRLINE);
                }
                drawTTF_vcentered((u32)(tx + UIS_W(16)), Y + bh / 2, "Quality", UIS_TF(16),
                                  qf ? XMB_ACCENT : XMB_TEXT_FAINT, true);

                // Spell out what the setting actually asks the server for —
                // the resolution alone does not say how much bandwidth this
                // costs, which is the reason to change it.
                const vquality_t vq = vquality_get();
                u32 qw = 0, qh = 0; unsigned qbr = 0;
                vquality_params(vq, hd1080_enabled(), display_width,
                                display_height, &qw, &qh, NULL, NULL, &qbr);
                char qtxt[64];
                if (qbr == 0)
                    // Direct play.  Retired from the ladder (it could not hold
                    // a remux on this console), so this is only reachable from
                    // a settings file written by an older build -- say what it
                    // is rather than printing a bitrate of zero.
                    snprintf(qtxt, sizeof(qtxt),
                             "Original  (direct play, no re-encode)");
                else if (vq == VQ_AUTO)
                    snprintf(qtxt, sizeof(qtxt), "Auto  (%ux%u, %u Mbps)",
                             (unsigned)qw, (unsigned)qh, qbr / 1000000u);
                else
                    snprintf(qtxt, sizeof(qtxt), "%s  (%u Mbps)",
                             vquality_label(vq), qbr / 1000000u);
                // A quality value ("Auto  (1920x1080, 25 Mbps)") -- section 3.0
                // gives codec and quality values to the spec face, "set one
                // step smaller than its host line".
                info_clip_text(tx + UIS_W(118), Y + UIS_H(11), qtxt, UIS_TF(17),
                               qf ? XMB_WHITE : XMB_TEXT, bw - UIS_W(166), false,
                               UI_FACE_SPEC);
                Y += bh + 18;
            }

            if (detail.tagline[0]) {
                drawTTF((u32)tx, (u32)Y, detail.tagline, UIS_TF(20), XMB_TEXT_DIM);
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
                        wpx += ttf_text_width(one, UIS_TF(19));
                        if (wpx > wrap_w) break;
                        fit++;
                    }
                    int take = (!p[fit] || fit >= (int)sizeof(buf) - 1) ? fit
                             : (last_sp > 0 ? last_sp : fit);
                    if (take <= 0) take = 1;
                    snprintf(buf, sizeof(buf), "%.*s", take, p);
                    drawTTF((u32)tx, (u32)Y, buf, UIS_TF(19), XMB_TEXT);
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
                drawTTF((u32)tx,         (u32)(Y + UIS_H(2)), facts[i].label, UIS_TF(15),
                        XMB_TEXT_FAINT);
                drawTTF((u32)(tx + UIS_W(120)), (u32)Y, facts[i].value, UIS_TF(17), XMB_TEXT);
                Y += 32;
            }

            // ---- Sections below the hero: begin under whichever column is
            // taller — the fact list or the poster.
            int hero_bottom = poster_y + poster_h - scroll_y;
            int sec = (Y > hero_bottom ? Y : hero_bottom) + UIS_H(40);

            // Cast & Crew.  v1.0 restyles this row: the headshots were 118x176
            // rectangles and are now 64px circles in a 92px column, with the
            // name and character stacked under them on --font-tab.
            //
            // The images are NOT new -- parse_people() has filled
            // detail.people[] since before this fork, and this row has always
            // drawn them.  Only the shape, sizes and face changed.
            if (detail.n_people > 0) {
                xmb_draw_eyebrow(X, sec, "Cast & Crew", XMB_TEXT_FAINT);
                sec += UIS_H(42);
                const int dia = UIS_H(64);          // the circle
                const int cw  = UIS_W(92);          // the column it sits in
                const int gap = UIS_W(12);
                const int name_y = sec + dia + UIS_H(7);
                for (int i = 0; i < detail.n_people; i++) {
                    int cxp = X + i * (cw + gap);
                    if (cxp + cw > (int)display_width - X) break;   // no h-scroll yet
                    // Centre the circle in its column; the labels stay flush
                    // left under it, which is what the design shows.
                    xmb_cpu_blit_thumb_circle(detail.people[i].id,
                                              cxp, sec, dia, XMB_HAIRLINE);
                    info_clip_text(cxp, name_y, detail.people[i].name,
                                   UIS_TF(12), XMB_TEXT, cw, false, UI_FACE_TAB_REG);
                    if (detail.people[i].role[0])
                        info_clip_text(cxp, name_y + UIS_H(16), detail.people[i].role,
                                       UIS_TF(11.5f), XMB_TEXT_DIM, cw, false,
                                       UI_FACE_TAB_REG);
                }
                sec += dia + UIS_H(7) + UIS_H(40);
            }

            // More Like This — recommended poster cards.  Left/right selects a
            // card (Cross opens it); the row scrolls horizontally to follow.
            if (n_similar > 0) {
                xmb_draw_eyebrow(X, sec, "More Like This", XMB_TEXT_FAINT);
                sec += UIS_H(42);
                const int cw = UIS_W(148), ch = SIM_CH, gap = UIS_W(22);
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
                        drawRect((u32)(cxp - UIS_W(2)), (u32)(sec - UIS_H(2)), (u32)(cw + UIS_W(4)), UIS_H(2), XMB_FOCUS_RING);
                        drawRect((u32)(cxp - UIS_W(2)), (u32)(sec + ch),  (u32)(cw + UIS_W(4)), UIS_H(2), XMB_FOCUS_RING);
                        drawRect((u32)(cxp - UIS_W(2)), (u32)(sec - UIS_H(2)), UIS_W(2), (u32)(ch + UIS_H(4)), XMB_FOCUS_RING);
                        drawRect((u32)(cxp + cw), (u32)(sec - UIS_H(2)), UIS_W(2), (u32)(ch + UIS_H(4)), XMB_FOCUS_RING);
                    }
                    info_clip_text(cxp, sec + ch + UIS_H(8), similar[i].name, UIS_TF(14),
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
                int bx = (int)display_width - UIS_W(10);
                int ty0 = top, th = view_bottom - top;
                drawRect((u32)bx, (u32)ty0, UIS_W(3), (u32)th, XMB_TRACK);
                int total = content_bottom > 0 ? content_bottom : 1;
                int thb = th * view_bottom / total;
                if (thb < 26) thb = 26;
                if (thb > th) thb = th;
                int off = (th - thb) * scroll_y / max_scroll;
                drawRect((u32)bx, (u32)(ty0 + off), UIS_W(3), (u32)thb, XMB_ACCENT);
            }
        }
        {
            Hint h[3]; int nh = 0;
            h[nh].glyph = 'C'; h[nh].label = "Back";  nh++;
            if (max_scroll > 0) { h[nh].glyph = 'D'; h[nh].label = "Scroll"; nh++; }
            h[nh].glyph = 'X';
            h[nh].label = (focus == FOCUS_SIM)     ? "Open" :
                          (focus == FOCUS_VERSION) ? "Choose version" :
                          (focus == FOCUS_QUALITY) ? "Change quality" : "Play";
            nh++;
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

        int cx    = px + UIS_W(32);
        int max_w = pw - UIS_W(64);
        int y     = py + UIS_H(30);

        // Item title, truncated to the panel width.
        {
            char tbuf[132];
            snprintf(tbuf, sizeof(tbuf), "%s", it->name);
            int len = (int)strlen(tbuf);
            while (len > 1 && ttf_text_width(tbuf, UIS_TF(25), true) > max_w)
                tbuf[--len] = '\0';
            drawTTF((u32)cx, (u32)y, tbuf, UIS_TF(25), XMB_WHITE, true);
        }
        y += 40;
        drawTTF((u32)cx, (u32)y, sub, UIS_TF(15), XMB_TEXT_DIM);
        y += 34;

        // Two option rows; the selected one gets the panel-hi fill + accent bar.
        int ow = max_w, oh = UIS_H(46);
        for (int i = 0; i < 2; i++) {
            int oy = y + i * (oh + UIS_H(10));
            if (i == sel) {
                drawRect((u32)cx, (u32)oy, (u32)ow, (u32)oh, XMB_PANEL_HI);
                drawRect((u32)(cx - UIS_W(4)), (u32)oy, UIS_W(3), (u32)oh, XMB_ACCENT);
            }
            drawTTF_vcentered((u32)(cx + UIS_W(16)), oy + oh / 2, opts[i], UIS_TF(19),
                              i == sel ? XMB_TEXT : XMB_TEXT_DIM);
        }

        { static const Hint h[] = {{'X', "Select"}, {'C', "Back"}};
          draw_hints_bar(h, 2); }
        flip();
    }
    init_btns();
    return -1;
}
