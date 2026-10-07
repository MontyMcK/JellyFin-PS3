// XMB main screen — per-frame loop: input dispatch, CPU draw phase,
// RSX/TTF draw phase, contextual hints.

#include <stdio.h>
#include <string.h>

#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "ui_internal.h"
#include "ui_wave.h"
#include "ui_sfx.h"
#include "ui_card_gpu.h"
#include "ui_text_gpu.h"
#include "ui_strobe_test.h"
#include "ui_spine.h"      // the README 2.9 spine, gated by jellyfin_spine.txt
#include "boot_anim.h"
#include "thumbnail_cache.h"
#include "slog.h"
#include "plog.h"
#include "timing.h"        // timing_get_us() — background library retry
#include "jellyfin_api.h"   // g_auth_expired — the XMB leaves when it is set

extern void crash_log(const char *msg);

static void xmb_reset_state(void) {
    memset(g_items, 0, sizeof(g_items));
    memset(g_item_count, 0, sizeof(g_item_count));
    memset(g_items_loaded, 0, sizeof(g_items_loaded));
    memset(g_tab_start, 0, sizeof(g_tab_start));
    memset(g_tab_total, 0, sizeof(g_tab_total));
    memset(g_tab_name_filter, 0, sizeof(g_tab_name_filter));
    g_jumpbar_active = false;
    g_jumpbar_sel    = 1;
    g_settings_sel     = 0;
    g_settings_confirm = false;
    memset(g_search_buf, 0, sizeof(g_search_buf));
    g_search_results_count = 0;
    g_active_tab = XMB_TAB_HOME;
    g_sel = 0; g_scroll_top = 0;
    g_tv_sub_start = 0; g_tv_sub_total = 0;
    g_col_sub_start = 0; g_col_sub_total = 0;
    g_music_subtab = MUSIC_ST_ALBUMS;
    g_music_header = false;
    g_music_depth  = 0;
    g_music_sub_count = 0; g_music_sub_sel = 0;
    g_music_sub_scroll = 0; g_music_sub_total = 0;
    g_osk_row = 0; g_osk_col = 0; g_osk_sym = false;
}

// Resolve the card-grid view for the current tab: grid geometry, which item
// array, count, selection, scroll, grid origin, and whether more rows exist
// below.  Returns false for tabs that don't use the grid
// (search/settings).
bool xmb_grid_view(int tab, GridGeom *gg, const XMBItem **items,
                   int *count, int *sel, int *scroll, int *y0,
                   bool *more_below, int *abs_start, int *abs_total) {
    if (tab == XMB_TAB_SEARCH || tab == XMB_TAB_SETTINGS)
        return false;
    xmb_grid_geom(tab, gg);
    if (g_tv_depth > 0) {
        *items = g_tv_sub_items;  *count = g_tv_sub_count;
        *sel   = g_tv_sub_sel;    *scroll = g_tv_sub_scroll;
        *y0    = XMB_GRID_Y0 + 26;
        *more_below = g_tv_sub_scroll + gg->vis < g_tv_sub_count ||
                      g_tv_sub_start + g_tv_sub_count < g_tv_sub_total;
        *abs_start = g_tv_sub_start;  *abs_total = g_tv_sub_total;
        return true;
    }
    if (g_col_depth > 0) {
        *items = g_col_sub_items; *count = g_col_sub_count;
        *sel   = g_col_sub_sel;   *scroll = g_col_sub_scroll;
        *y0    = XMB_GRID_Y0 + 26;
        *more_below = g_col_sub_scroll + gg->vis < g_col_sub_count ||
                      g_col_sub_start + g_col_sub_count < g_col_sub_total;
        *abs_start = g_col_sub_start; *abs_total = g_col_sub_total;
        return true;
    }
    if (g_music_depth > 0) {
        *items = g_music_sub_items; *count = g_music_sub_count;
        *sel   = g_music_sub_sel;   *scroll = g_music_sub_scroll;
        *y0    = XMB_GRID_Y0 + 26;
        *more_below = g_music_sub_scroll + gg->vis < g_music_sub_count;
        *abs_start = 0; *abs_total = g_music_sub_count;  // single page
        return true;
    }
    *items = g_items[tab];  *count = g_item_count[tab];
    *sel   = g_sel;         *scroll = g_scroll_top;
    *y0    = XMB_GRID_Y0
           + (xmb_kind(tab) == TABKIND_MUSIC ? XMB_MUSIC_SUBTAB_H : 0);
    *more_below = g_scroll_top + gg->vis < g_item_count[tab] ||
                  g_tab_start[tab] + g_item_count[tab] < g_tab_total[tab];
    *abs_start = g_tab_start[tab]; *abs_total = g_tab_total[tab];
    return true;
}

#if BUILD_FOR_RPCS3
// STATE breadcrumb for menu navigation.  Called once per frame from the XMB
// loop, but only emits when the *effective* (tab, depth, selection, count)
// actually changes — so the host log-assert harness sees exactly one MENU
// line per navigation step rather than one per frame.  Reuses xmb_grid_view()
// (above) to resolve sel/total for whichever grid/sub-screen is active.
static void slog_menu_tick(void) {
    int         tab   = g_active_tab;
    const char *ctx   = g_tabs[tab].label;
    int         sel   = -1, total = 0, depth = 0;

    if (tab == XMB_TAB_SETTINGS) {
        sel = g_settings_sel; total = XMB_SETTINGS_COUNT;
    } else if (tab == XMB_TAB_SEARCH) {
        sel = g_search_focus_results ? 1 : 0; total = g_search_results_count;
    } else {
        GridGeom gg; const XMBItem *items;
        int count, s, scroll, y0, a_start, a_total; bool more;
        if (xmb_grid_view(tab, &gg, &items, &count, &s, &scroll, &y0,
                          &more, &a_start, &a_total)) {
            sel   = s;
            total = (a_total > count) ? a_total : count;   // paged vs. loaded
            // Whichever drill-down is open — it is no longer implied by the
            // tab's kind, since any library can hold a Series or a BoxSet.
            depth = g_tv_depth  ? g_tv_depth
                  : g_col_depth ? g_col_depth
                  : g_music_depth;
        }
        // else: Home — no grid; just track the tab (sel stays 0).
        else { sel = 0; total = 0; }
    }

    bool header = (xmb_kind(tab) == TABKIND_MUSIC && g_music_header);

    static int  s_tab = -1, s_sel = -2, s_total = -1, s_depth = -1;
    static bool s_header = false;
    if (tab != s_tab || sel != s_sel || total != s_total ||
        depth != s_depth || header != s_header) {
        s_tab = tab; s_sel = sel; s_total = total; s_depth = depth;
        s_header = header;
        slog_state("MENU tab=%s depth=%d sel=%d total=%d%s",
                   ctx, depth, sel, total, header ? " header=1" : "");
    }
}
#else
static inline void slog_menu_tick(void) {}
#endif

// GPU draw phase — RSX commands, runs BEFORE rsxSync().
//
// Card images are submitted here as textured quads instead of being memcpy'd
// into video memory by the CPU after the fence.  Measured motivation: cards
// were 6,446 us of a 15,661 us Home frame (docs/spu-feasibility.md).  Order
// matters and is the whole reason this is a separate phase -- these are queued
// RSX commands, so they must be issued before the fence for the CPU's
// selection borders, progress strips and labels to land on top of them.
static bool s_div_on_gpu = false;   // set per frame by the GPU phase

// Grid scroll easing (spine_grid_motion).  While rows are moving, tell the
// grid walks which rows to draw and clip them to the grid's band, so a row
// easing out past the top never draws over the breadcrumb or the spine.  At
// rest -- and always with the spine gate off -- this does nothing at all.
static bool grid_motion_begin(const GridGeom *gg, int scroll, int y0, bool gpu) {
    int r0, rn, dy;
    spine_grid_motion(gg, scroll, &r0, &rn, &dy);
    if (r0 == 0 && rn == XMB_GRID_ROWS && dy == 0) return false;
    xmb_grid_motion(r0, rn, dy);
    const int top = y0, bot = y0 + XMB_GRID_ROWS * gg->stride;
    if (gpu) ui_card_gpu_clip(top, bot);
    else     { g_cpu_clip_top = top; g_cpu_clip_bot = bot; }
    return true;
}
static void grid_motion_end(bool on, bool gpu) {
    if (!on) return;
    xmb_grid_motion(0, XMB_GRID_ROWS, 0);
    if (gpu) ui_card_gpu_clip(0, 0);
    else     { g_cpu_clip_top = 0; g_cpu_clip_bot = 0; }
}

static void xmb_draw_gpu_phase(int tab) {
    // The hairline goes first and is NOT gated on the card path: it is a
    // blended quad over the wave, and at 1,351 us of CPU VRAM reads it was the
    // single largest item in the frame.  It has to be issued here, before the
    // frame's rsxSync() -- see wave_draw_divider_gpu().
    if (wave_gpu_blend_ready()) {
        // The spine's base layer draws no divider; between levels it fades
        // (spine_divider_alpha() is 1 with the gate off, so this is 72 there).
        const u8 div_a = (u8)(72.0f * spine_divider_alpha() + 0.5f);
        if (div_a) wave_draw_divider_gpu(XMB_DIVIDER_Y, 0x8A, 0x93, 0xC8, div_a);
        if (g_spine_on) {
            spine_draw_gpu();  // glow band + bloom
        } else if (!strobe_test_disable_gpu_glow()) {
            // The tab strip's focus glow, exactly as the strobe line has it.
            const u32 accent = XMB_ACCENT;
            const int alpha = xmb_nav_depth() > 0 ? 24 : 72;
            wave_draw_glow_gpu(xmb_tab_focus_center(),
                               XMB_OY + UIS_H(61) + UIS_H(16),
                               UIS_H(34),
                               (u8)((accent >> 16) & 0xFF),
                               (u8)((accent >> 8) & 0xFF),
                               (u8)(accent & 0xFF), (u8)alpha);
        }
        s_div_on_gpu = true;
    } else {
        s_div_on_gpu = false;
    }

    if (!ui_card_gpu_ready()) return;

    // Under the spine every category is a depth-engine stage: Home's column
    // swings into its queue at every depth, a library's into its grid until
    // the grid takes the screen back (render/depth.h, ui_depth.cpp).
    if (g_spine_on && tab == XMB_TAB_HOME) {
        xmb_home_stage_gpu();
    } else if (depth_lib_owns(tab)) {
        depth_lib_gpu(tab);          // the L1 column, or its swing into the grid
    } else if (tab == XMB_TAB_HOME) {
        xmb_home_gpu_phase();
    } else if (tab != XMB_TAB_SEARCH && tab != XMB_TAB_SETTINGS) {
        GridGeom gg; const XMBItem *items;
        int count, sel, scroll, y0, a_start, a_total; bool more;
        if (xmb_grid_view(tab, &gg, &items, &count, &sel, &scroll, &y0,
                          &more, &a_start, &a_total))
        {
            const bool m = grid_motion_begin(&gg, scroll, y0, true);
            xmb_grid_gpu(&gg, items, count, sel, scroll, y0);
            grid_motion_end(m, true);
        }
    }
    ui_card_gpu_end();
}

// CPU draw phase — direct framebuffer writes, runs after rsxSync().
static void xmb_draw_cpu_phase(int tab) {
    // Only when the GPU phase did not already lay it down as a blended quad.
    if (!s_div_on_gpu && spine_divider_alpha() >= 0.5f &&
        !strobe_test_disable_cpu_divider())
        xmb_draw_divider();

    if (g_spine_on && tab == XMB_TAB_HOME) {
        xmb_home_stage_cpu();
        depth_lib_prefetch(xmb_next_enabled(tab, +1), 6);   // the libraries either side
        depth_lib_prefetch(xmb_next_enabled(tab, -1), 6);
    } else if (depth_lib_owns(tab)) {
        depth_lib_cpu(tab);
        // The neighbours' first posters, after this tab's own requests.
        depth_lib_prefetch(xmb_next_enabled(tab, +1), 6);
        depth_lib_prefetch(xmb_next_enabled(tab, -1), 6);
    } else if (tab == XMB_TAB_SEARCH) {
        xmb_cpu_draw_osk();
        xmb_cpu_draw_search_results();
    } else if (tab == XMB_TAB_SETTINGS) {
        xmb_cpu_draw_settings();
    } else if (tab == XMB_TAB_HOME) {
        xmb_home_cpu_phase();
    } else {
        GridGeom gg; const XMBItem *items;
        int count, sel, scroll, y0, a_start, a_total; bool more;
        if (xmb_grid_view(tab, &gg, &items, &count, &sel, &scroll, &y0,
                          &more, &a_start, &a_total))
        {
            const bool m = grid_motion_begin(&gg, scroll, y0, false);
            xmb_grid_cpu(&gg, items, count, sel, scroll, y0);
            grid_motion_end(m, false);
        }
    }
}

// TTF/RSX draw phase — text and icons on top of the CPU-drawn layer.
static void xmb_draw_text_phase(int tab) {
    xmb_draw_topbar();

    if (g_spine_on && tab == XMB_TAB_HOME) {
        xmb_home_stage_text();
    } else if (depth_lib_owns(tab)) {
        depth_lib_text(tab);
    } else if (tab == XMB_TAB_SEARCH) {
        xmb_rsx_draw_osk();
    } else if (tab == XMB_TAB_SETTINGS) {
        xmb_draw_settings();
    } else if (tab == XMB_TAB_HOME) {
        xmb_home_text_phase();
    } else {
        // Card-grid tabs: breadcrumb (sub-screens), empty text, grid labels.
        if (xmb_kind(tab) == TABKIND_MUSIC && g_music_depth == 0) {
            GridGeom mg;
            xmb_grid_geom(tab, &mg);
            xmb_draw_music_subtabs(mg.x0, XMB_CONTENT_Y + UIS_H(2),
                                   g_music_subtab, g_music_header);
        } else if (g_music_depth > 0) {
            // Reached from a music library OR a standalone Playlists one, so
            // name what is actually listed: a playlist drills into its tracks,
            // an artist/genre into its albums.
            bool tracks = (g_music_sub_count > 0 &&
                           strcmp(g_music_sub_items[0].type, "Audio") == 0);
            xmb_draw_breadcrumb(XMB_ITEM_PAD, XMB_CONTENT_Y + UIS_H(2),
                                g_music_parent_name,
                                tracks ? "Tracks" : "Albums", NULL);
        }
        if (g_tv_depth > 0) {
            if (g_tv_depth == 1)
                xmb_draw_breadcrumb(XMB_ITEM_PAD, XMB_CONTENT_Y + UIS_H(2),
                                    g_tv_series_name, "Seasons", NULL);
            else
                xmb_draw_breadcrumb(XMB_ITEM_PAD, XMB_CONTENT_Y + UIS_H(2),
                                    g_tv_series_name, g_tv_season_name,
                                    "Episodes");
        } else if (g_col_depth > 0) {
            xmb_draw_breadcrumb(XMB_ITEM_PAD, XMB_CONTENT_Y + UIS_H(2),
                                g_col_name, "Movies", NULL);
        }

        GridGeom gg; const XMBItem *items;
        int count, sel, scroll, y0, a_start, a_total; bool more;
        if (xmb_grid_view(tab, &gg, &items, &count, &sel, &scroll, &y0,
                          &more, &a_start, &a_total)) {
            bool sub = (g_tv_depth > 0) ||
                       (g_col_depth > 0) ||
                       (g_music_depth > 0);
            if (count == 0) {
                bool loaded = sub || g_items_loaded[tab];
                if (loaded)
                    xmb_draw_empty_state(tab,
                            tab == XMB_TAB_RESUME ? "Nothing in progress"
                                                  : "No items in this library");
            } else {
                const bool m = grid_motion_begin(&gg, scroll, y0, false);
                xmb_grid_text(&gg, items, count, sel, scroll, y0, more,
                              a_start, a_total);
                grid_motion_end(m, false);
            }
            // Letter jump bar on library tabs at depth 0 (not the resume
            // list — it isn't alphabetical).
            if (!sub && tab != XMB_TAB_RESUME)
                xmb_draw_jumpbar(tab);
        }
    }
}

// -------------------------------------------------------
// Per-frame cost line
// -------------------------------------------------------
// The XMB has never reported what a frame actually costs, which is why the
// SPU feasibility work had to estimate it from layout constants multiplied by
// synthetic bandwidth figures (docs/spu-feasibility.md, Deliverable 5).  That
// estimate has a known weakness -- the framebuffer read rate was measured with
// a `volatile` loop that cannot batch -- so before any of the renderer is
// restructured on the strength of it, the real split needs to be on the record.
//
// Emitted once a second to player_log.txt when plog is on, so it costs nothing
// when it is off and never grows the log quickly.  timing_get_us() is a
// timebase read plus one divide; eight per frame is far below the noise of
// what it measures.
//
// Reading the line:
//   vsync  time parked in waitflip() -- NOT work, it is the 60 Hz budget the
//          frame did not use.  Large vsync means the frame has headroom.
//   gpu    submitting the clear + wave (RSX does the work asynchronously)
//   sync   rsxSync(), the full GPU fence -- the PPU stalled here, and nothing
//          overlaps it
//   cards  CPU phase: thumbnail memcpy into video memory
//   text   text phase: glyph compositing, the read-modify-write path
//   chrome hints + tab bar
//   flip   gcmSetFlip + buffer swap
//   other  everything unaccounted, mostly xmb_fetch_tab_items' network I/O
//   gl     glyphs drawn; bpx/opx glyph pixels that blended vs stored opaque
//   tgpu   submitting the frame's text runs as RSX quads (GPU text path only)
//   tr/tm  text runs queued / of those, the ones that missed the cache and had
//          to be rasterized and uploaded; tkb is what those misses uploaded.
//          A warm screen should settle at tm=0 tkb=0 -- caching per RUN rather
//          than per glyph is the entire point of that path.
struct XmbFrameCost {
    u64 vsync, gpu, sync, cards, text, chrome, flip, textgpu, total;
    u32 frames;
};
static XmbFrameCost s_fc;

static void xmb_cost_tick(u64 frame_us)
{
    s_fc.total += frame_us;
    if (++s_fc.frames < 60) return;

    u32 gl = 0, bpx = 0, opx = 0;
    ui_text_stats_get(&gl, &bpx, &opx);
    u32 tr = 0, tm = 0, tb = 0, tf = 0;
    ui_text_gpu_stats_get(&tr, &tm, &tb, &tf);

    const u32 n = s_fc.frames;
    u64 acc = s_fc.vsync + s_fc.gpu + s_fc.sync + s_fc.cards +
              s_fc.text + s_fc.chrome + s_fc.flip + s_fc.textgpu;
    u64 other = (s_fc.total > acc) ? (s_fc.total - acc) : 0;


    char b[320];
    snprintf(b, sizeof(b),
             "xmb: frame=%llu.%02llums vsync=%llu gpu=%llu sync=%llu cards=%llu "
             "text=%llu chrome=%llu tgpu=%llu flip=%llu other=%llu (us/frame) "
             "gl=%u bpx=%u opx=%u tr=%u tm=%u tkb=%u strobe=%d:%s",
             (unsigned long long)(s_fc.total / n / 1000),
             (unsigned long long)((s_fc.total / n % 1000) / 10),
             (unsigned long long)(s_fc.vsync  / n),
             (unsigned long long)(s_fc.gpu    / n),
             (unsigned long long)(s_fc.sync   / n),
             (unsigned long long)(s_fc.cards  / n),
             (unsigned long long)(s_fc.text   / n),
             (unsigned long long)(s_fc.chrome  / n),
             (unsigned long long)(s_fc.textgpu / n),
             (unsigned long long)(s_fc.flip    / n),
             (unsigned long long)(other        / n),
             gl / n, bpx / n, opx / n, tr / n, tm, tb / 1024u,
             strobe_test_profile(), strobe_test_profile_name());
    plog(b);

    memset(&s_fc, 0, sizeof(s_fc));
    ui_text_stats_reset();
    ui_text_gpu_stats_reset();
}

// Every bar but Search's ends with Square and the visualiser it switches TO
// (2026-09-27): the bottom-right corner always says what Square does.
static void draw_hints_vis(const Hint *h, int n) {
    Hint all[10];
    int k = 0;
    for (int i = 0; i < n && k < 9; i++) all[k++] = h[i];
    all[k].glyph = 'S';
    all[k].label = wave_vis_next_label();
    k++;
    draw_hints_bar(all, k);
}

// Contextual hints bar for the current tab / mode.
static void xmb_draw_hints(int tab) {
    bool in_tv_sub  = (g_tv_depth > 0);
    bool in_col_sub = (g_col_depth > 0);

    // The spine's base layer: the row is walked with the d-pad and a tab is
    // entered with Down or X.  Inside a tab, the Home and library hints gain
    // O Back -- the way out to the base layer.
    if (spine_at_base()) {
        static const Hint h[] = {{'E',"Nav"},{'X',"Open"}};
        draw_hints_vis(h, 2);
        return;
    }
    const bool spine_back = g_spine_on;

    if (tab == XMB_TAB_SETTINGS) {
        if (g_settings_confirm) {
            static const Hint h[] = {{'X',"Confirm"},{'C',"Cancel"}};
            draw_hints_vis(h, 2);
        } else {
            static const Hint h[] = {{'X',"Select"},{'T',"Details"}};
            draw_hints_vis(h, 2);
        }
    } else if (tab == XMB_TAB_SEARCH) {
        if (g_search_focus_results) {
            static const Hint h[] = {{'X',"Open"},{'T',"Details"},{'S',"Delete"},{'C',"Back"}};
            draw_hints_bar(h, 4);
        } else {
            static const Hint h[] = {{'X',"Type"},{'S',"Delete"},{'C',"Clear"}};
            draw_hints_bar(h, 3);
        }
    } else if (xmb_kind(tab) == TABKIND_MUSIC && g_music_header) {
        static const Hint h[] = {{'D',"Switch"},{'X',"Select"}};
        draw_hints_vis(h, 2);
    } else if (g_music_depth > 0 && xmb_kind(tab) == TABKIND_MUSIC) {
        static const Hint h[] = {{'X',"Select"},{'C',"Back"}};
        draw_hints_vis(h, 2);
    } else if (in_tv_sub || in_col_sub) {
        static const Hint h[] = {{'X',"Select"},{'T',"Details"},{'C',"Back"}};
        draw_hints_vis(h, 3);
    } else if (g_jumpbar_active) {
        static const Hint h[] = {{'X',"Jump"},{'C',"Cancel"}};
        draw_hints_vis(h, 2);
    } else if (tab == XMB_TAB_HOME && spine_back) {
        // The canvas's L2 bar: the d-pad walks the queue and the categories,
        // X opens detail, O goes back up to the base layer.
        static const Hint h[] = {{'E',"Nav"},{'X',"Open"},{'T',"Details"},{'C',"Back"}};
        draw_hints_vis(h, 4);
    } else if (tab == XMB_TAB_HOME) {
        // The L1/R1 cluster leads, per handoff section 3.1.  Label is "Tab"
        // and not the document's "Page" because L1/R1 switch TABS on this
        // build (ui_nav.cpp / ui_home.cpp) -- paging is Phase 4, and a hint
        // must describe what the button does today.
        static const Hint h[] = {{'l',""},{'r',"Tab"},
                                 {'X',"Open"},{'T',"Details"},{'C',"Back"}};
        draw_hints_vis(h, spine_back ? 5 : 4);
    } else {
        static const Hint h[] = {{'l',""},{'r',"Tab"},
                                 {'E',"Nav"},{'X',"Select"},{'T',"Details"},
                                 {'C',"Back"}};
        static const Hint hm[] = {{'l',""},{'r',"Tab"},
                                  {'E',"Nav"},{'X',"Select"},{'C',"Back"}};
        if (xmb_kind(tab) == TABKIND_MUSIC) draw_hints_vis(hm, spine_back ? 5 : 4);
        else                                draw_hints_vis(h, spine_back ? 6 : 5);
    }
}

void ui_run_xmb(void) {
    crash_log("13.1 reset_state");
    xmb_reset_state();
    wave_reset();

    crash_log("13.2 detect_tabs");
    // Already done behind the boot animation on a cold boot (xmb_prepare).
    if (!xmb_take_prepared()) xmb_detect_tabs();
    crash_log("13.3 detect_tabs done");

    if (!g_tabs[g_active_tab].enabled) {
        for (int t = 0; t < XMB_TAB_COUNT; t++) {
            if (g_tabs[t].enabled) { g_active_tab = t; break; }
        }
    }

    OSK_Y0 = XMB_CONTENT_Y + UIS_H(80);

    crash_log("13.4 init_btns");
    init_btns();
    crash_log("13.5 loop enter");

    // Breadcrumbs inside the loop fire only on the first pass so the log
    // doesn't grow unbounded once the UI is actually running.
    bool first_iter = true;
    strobe_test_enter_xmb();
    while (running) {
        // The server revoked this device's token (http.cpp saw a 401 on a
        // request that carried it).  Every fetch from here on returns nothing,
        // so the XMB would just sit there looking like an empty library —
        // leave, and let main() send the user back to the login screen.
        if (g_auth_expired) {
            crash_log("13.x auth expired, leaving XMB");
            plog("xmb: session revoked by server, returning to login");
            xmb_search_shutdown();
            strobe_test_leave_xmb();
            return;
        }
        u64 t_f0 = timing_get_us();
        if (first_iter) crash_log("13.5a waitflip");
        waitflip();
        u64 t_vsync = timing_get_us();
        s_fc.vsync += t_vsync - t_f0;
        if (first_iter) crash_log("13.5b syscb");
        sysUtilCheckCallback();

        // Self-heal an empty library bar.
        //
        // The library list is fetched once, at startup.  If the network was not
        // up yet -- routine for the first launch after a console power-cycle --
        // detect_tabs exhausted its tries and the XMB sat there with no Movies
        // and no TV for the rest of the session, which reads as a broken app
        // rather than a slow network.  Keep trying quietly instead.
        //
        // ONE attempt per wake-up, never the full retry set: this runs on the
        // render thread, and a failed request costs its timeout.  The interval
        // doubles from 5s to a 60s ceiling so a genuinely absent server costs
        // almost nothing, while the usual case -- network arriving a few
        // seconds late -- recovers on the first or second try.
        if (!g_auth_expired) {
            static u64 s_lib_next_us = 0;
            static u64 s_lib_gap_us  = 5000000ULL;
            bool have_lib = false;
            for (int t = XMB_TAB_LIB0; t < XMB_TAB_COUNT; t++)
                if (g_tabs[t].enabled) { have_lib = true; break; }
            if (have_lib) {
                s_lib_next_us = 0;
                s_lib_gap_us  = 5000000ULL;
            } else {
                u64 now = timing_get_us();
                if (s_lib_next_us == 0) {
                    s_lib_next_us = now + s_lib_gap_us;
                } else if (now >= s_lib_next_us) {
                    plog("xmb: library bar empty - retrying detect_tabs");
                    if (xmb_detect_tabs_once()) {
                        plog("xmb: libraries recovered");
                        if (!g_tabs[g_active_tab].enabled) {
                            for (int t = 0; t < XMB_TAB_COUNT; t++)
                                if (g_tabs[t].enabled) { g_active_tab = t; break; }
                        }
                    }
                    if (s_lib_gap_us < 60000000ULL) s_lib_gap_us *= 2;
                    s_lib_next_us = timing_get_us() + s_lib_gap_us;
                }
            }
        }
        thumb_cache_tick();   // age out thumbs nothing on screen still wants
        u64 t_gpu0 = timing_get_us();
        if (first_iter) crash_log("13.5c clearScreen");
        clearScreen(XMB_BG);
        if (first_iter) crash_log("13.5d wave_draw");
        wave_snow_ambient();   // a menu: Menu Particles may show here
        wave_draw();
        s_fc.gpu += timing_get_us() - t_gpu0;
        if (first_iter) crash_log("13.6 wave_draw done");

        int tab = g_active_tab;
        if (tab != XMB_TAB_SEARCH && tab != XMB_TAB_SETTINGS
            && tab != XMB_TAB_HOME)
            if (!g_items_loaded[tab]) {
                if (first_iter) crash_log("13.7 fetch_tab_items");
                xmb_fetch_tab_items(tab);
                if (first_iter) crash_log("13.7b fetch done");
            }

        poll_buttons();
        input_media_keys_as_shoulders();   // a remote with no L1 / R1
        spine_frame_begin();   // one depth sample for the whole frame
        // The ambient screensaver: a press that wakes it does nothing else.
        const bool amb_swallow =
            ambient_update(!xmb_update_popup_active() && !g_overscan_calib);
        const bool draw_ui = ambient_draw_ui();
        bool should_exit = false;
        if (boot_anim_xmb_frame())
            ;   // cold boot still assembling: a press skips it, nothing else
        else if (amb_swallow)
            ;                           // it only brought the UI back
        else if (xmb_update_popup_active())
            xmb_update_popup_input();   // modal: the screen below keeps focus state
        else if (peek_active())
            peek_input();               // the quick-peek owns the pad while up
        else if (BTN_PRESSED(square) && tab != XMB_TAB_SEARCH) {
            // Square: the visualiser, JellyWave -> JellyDrop -> Off (ui_wave.h).
            // Not on Search, where Square deletes a character.
            wave_vis_cycle();
            ui_sfx_play(SFX_OPTION);
        }
        else if (spine_at_base())
            should_exit = spine_input_base();
        else if (spine_try_back(tab))
            ;                           // Up / O at the tab's root: back to base
        else if (tab == XMB_TAB_SEARCH)
            should_exit = xmb_handle_input_search();
        else if (tab == XMB_TAB_HOME)
            should_exit = xmb_handle_input_home();
        else
            should_exit = xmb_handle_input_browse();
        if (should_exit) break;

        slog_menu_tick();   // STATE: MENU ... (emulator-only, emits on change)

        // Card images go into the FIFO with the wave, ahead of the fence.
        if (!g_overscan_calib) {
            if (draw_ui) xmb_draw_gpu_phase(tab);
            else         ambient_gpu();
        }

        u64 t_sync0 = timing_get_us();
        rsxSync();
        u64 t_sync1 = timing_get_us();
        s_fc.sync += t_sync1 - t_sync0;

        // Open the text-run collecting window.  From here until the flush
        // below, drawTTF hands whole strings to the RSX instead of blending
        // glyph pixels against video memory.  It has to be bracketed: a queued
        // run that nobody submits is invisible text, so only the loop that
        // promises to flush is allowed to queue.  See ui_text_gpu.h.
        ui_text_gpu_begin();

        if (g_overscan_calib) {
            // Full-screen overscan calibration takeover — no chrome/hints/tabs.
            xmb_overscan_calib_cpu();
            xmb_overscan_calib_text();
        } else if (!draw_ui) {
            ambient_text();
        } else {
            if (first_iter) crash_log("13.8 cpu_phase");
            xmb_draw_cpu_phase(tab);
            u64 t_cards = timing_get_us();
            s_fc.cards += t_cards - t_sync1;
            if (first_iter) crash_log("13.8b text_phase");
            xmb_draw_text_phase(tab);
            u64 t_text = timing_get_us();
            s_fc.text += t_text - t_cards;
            if (first_iter) crash_log("13.8c hints");
            bool popup = xmb_update_popup_active();
            // The popup and the quick-peek draw their own hints.
            if (!popup && !peek_visible()) xmb_draw_hints(tab);
            if (first_iter) crash_log("13.8d tabs");
            if (g_spine_on) spine_draw();
            else            xmb_draw_tabs();
            if (popup) {
                // The popup GPU-dims the whole screen and then lays an opaque
                // panel on it, so everything queued so far has to be on the
                // framebuffer BEFORE it draws -- otherwise the screen behind
                // the modal would be dimmed without its text and the text
                // would then land on top of the modal, undimmed.  This is the
                // one place in the frame where a CPU draw covers earlier text;
                // the hints bar and the tab bar draw no background at all.
                ui_text_gpu_flush_fenced();
                xmb_update_popup_draw();
            }
            s_fc.chrome += timing_get_us() - t_text;
        }

        // Submit the frame's text runs.  No fence: flip() queues the flip
        // behind these quads and waitflip() at the top of the next iteration
        // is the wait, so the RSX gets the rest of the vblank to draw them.
        u64 t_tg0 = timing_get_us();
        ui_text_gpu_flush();
        peek_draw_over();              // the quick-peek, over the grid's text
        ambient_cover_over_ui();       // the dissolve, over everything
        s_fc.textgpu += timing_get_us() - t_tg0;

        // Cold boot: the veil and the travelling mark, over everything above.
        boot_anim_xmb_overlay();

        u64 t_flip0 = timing_get_us();
        if (first_iter) crash_log("13.9 first flip");
        flip();
        s_fc.flip += timing_get_us() - t_flip0;
        if (first_iter) { crash_log("13.10 first frame done"); first_iter = false; }
        sysUtilCheckCallback();
        xmb_cost_tick(timing_get_us() - t_f0);
        strobe_test_tick();
    }

    // A search can still be out on its worker.  It writes into file-static
    // storage in ui_search.cpp and calls http_request(), so letting it outlive
    // the screen that started it would leave a thread using the network while
    // the caller tears it down.  Join it here -- worst case this waits out one
    // request, which is bounded by the HTTP timeouts.
    xmb_search_shutdown();
    strobe_test_leave_xmb();
}
