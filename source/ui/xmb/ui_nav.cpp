// XMB navigation — tab switching and the browse-mode input handlers
// (settings, TV sub-screens, collections sub-screens, jump bar, item lists).

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "ui_internal.h"
#include "jellyfin_api.h"
#include "player.h"
#include "music_screen.h"
#include "plog.h"
#include "timing.h"

// -------------------------------------------------------
// Tab switching
// -------------------------------------------------------

void xmb_switch_tab(int new_tab) {
    if (new_tab < 0 || new_tab >= XMB_TAB_COUNT) return;
    if (!g_tabs[new_tab].enabled) return;
    int old = g_active_tab;
    if (old != XMB_TAB_SEARCH && old != XMB_TAB_SETTINGS
        && (g_tab_start[old] > 0 || g_tab_name_filter[old][0])) {
        g_items_loaded[old]       = false;
        g_item_count[old]         = 0;
        g_tab_start[old]          = 0;
        g_tab_total[old]          = 0;
        g_tab_name_filter[old][0] = '\0';
    }
    g_jumpbar_active = false;
    g_active_tab = new_tab;
    g_sel = 0;
    g_scroll_top = 0;
    g_tv_depth = 0; g_tv_sub_sel = 0; g_tv_sub_scroll = 0; g_tv_sub_start = 0; g_tv_sub_total = 0;
    g_col_depth = 0; g_col_sub_sel = 0; g_col_sub_scroll = 0; g_col_sub_start = 0; g_col_sub_total = 0;
    // Music keeps its sub-tab across visits but drops depth/header focus.
    g_music_depth = 0; g_music_header = false;
    g_music_sub_sel = 0; g_music_sub_scroll = 0;
    g_music_sub_count = 0; g_music_sub_total = 0;
    if (new_tab == XMB_TAB_SEARCH) {
        g_osk_row = 0; g_osk_col = 0; g_osk_sym = false;
    }
    if (new_tab == XMB_TAB_SETTINGS) {
        g_settings_sel = 0; g_settings_confirm = false;
    }
    // Home: reset focus and refetch the dynamic rows (Continue Watching /
    // Next Up change after every playback).
    if (new_tab == XMB_TAB_HOME)
        xmb_home_on_enter();
}

int xmb_next_enabled(int start, int dir) {
    int t = start + dir;
    while (t >= 0 && t < XMB_TAB_COUNT) {
        if (g_tabs[t].enabled) return t;
        t += dir;
    }
    return start;
}

// Launch the player for one list item, mapping XMBItem -> JFItem.
// resume_secs > 0 starts playback at that position (Continue Watching).
void xmb_play_item(const XMBItem *it, u32 resume_secs) {
    JFItem jf; memset(&jf, 0, sizeof(jf));
    strncpy(jf.id,   it->id,   sizeof(jf.id)-1);
    strncpy(jf.name, it->name, sizeof(jf.name)-1);
    strncpy(jf.type, it->type, sizeof(jf.type)-1);
    show_player(&jf, resume_secs);
}

// Play items[idx] and keep advancing through the list while the user
// accepts the player's end-of-item NEXT prompt (SELECT during the last
// 90 s).  Used for movies in a collection; episodes go through
// xmb_play_episode_with_next so the follower comes from the server.
static void xmb_play_list_with_next(const XMBItem *items, int count, int idx,
                                    const char *label, const char *hint) {
    for (;;) {
        if (idx + 1 < count) player_arm_next(label, hint);
        xmb_play_item(&items[idx], 0);
        if (idx + 1 >= count || !player_take_next_request()) break;
        idx++;
    }
}

// Play an episode and keep advancing while the user accepts the NEXT prompt
// (or the end-of-episode countdown fires).  The follower is resolved from
// the server before each playback, so this works no matter where the episode
// was launched from and keeps going across season boundaries.
void xmb_play_episode_with_next(const XMBItem *first, u32 resume_secs) {
    XMBItem cur = *first;
    u32 resume = resume_secs;
    for (;;) {
        XMBItem next;
        bool have = xmb_fetch_next_episode(cur.id, &next);
        if (have)
            player_arm_next("NEXT EPISODE", "Press SELECT for next episode");
        xmb_play_item(&cur, resume);
        if (!have || !player_take_next_request()) break;
        cur    = next;
        resume = 0;
    }
}

// -------------------------------------------------------
// Per-screen input handlers.  Each returns true when the XMB
// loop should exit (only the settings Log Out path does).
// -------------------------------------------------------

// Settings tab — account actions (Log Out).
static bool xmb_input_settings(void) {
    if (g_settings_confirm) {
        // Modal confirm: swallow all other input until resolved.
        if (BTN_PRESSED(cross)) {
            jellyfin_logout();
            return true;   // exit the XMB; main loop returns to the login screen
        }
        if (BTN_PRESSED(circle)) g_settings_confirm = false;
        return false;
    }
    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }
    if (BTN_REPEAT(up)   && g_settings_sel > 0)                      g_settings_sel--;
    if (BTN_REPEAT(down) && g_settings_sel < XMB_SETTINGS_COUNT - 1) g_settings_sel++;
    if (BTN_PRESSED(cross)) {
        if (g_settings_sel == 0) g_settings_confirm = true;             // Log Out
        if (g_settings_sel == 1) plog_set_enabled(!plog_enabled());     // Debug Logging
    }
    return false;
}

// TV sub-screen (Series -> Seasons -> Episodes) — card grid.
static void xmb_input_tv_sub(void) {
    GridGeom gg;
    xmb_grid_geom(XMB_TAB_TV, &gg);
    const int C   = gg.cols;
    const int VIS = gg.vis;
    if (BTN_PRESSED(circle)) {
        g_tv_depth--;
        g_tv_sub_sel = 0; g_tv_sub_scroll = 0; g_tv_sub_start = 0; g_tv_sub_total = 0;
        return;
    }
    if (BTN_REPEAT(up)) {
        if (g_tv_sub_sel >= C) {
            g_tv_sub_sel -= C;
            if (g_tv_sub_sel < g_tv_sub_scroll)
                g_tv_sub_scroll = (g_tv_sub_sel / C) * C;
        } else if (g_tv_sub_start > 0) {
            // Earlier pages were dropped by the forward slide: fetch back.
            int n = xmb_slide_tv_sub_backward();
            if (n > 0) {
                g_tv_sub_sel += n - C;   // one row up, same column
                if (g_tv_sub_sel < 0) g_tv_sub_sel = 0;
                g_tv_sub_scroll = (g_tv_sub_sel / C) * C;
            }
        }
    }
    if (BTN_REPEAT(down)) {
        if (g_tv_sub_sel + C < g_tv_sub_count) {
            g_tv_sub_sel += C;
            if (g_tv_sub_sel >= g_tv_sub_scroll + VIS)
                g_tv_sub_scroll += C;
        } else if (g_tv_sub_sel / C < (g_tv_sub_count - 1) / C) {
            g_tv_sub_sel = g_tv_sub_count - 1;
            if (g_tv_sub_sel >= g_tv_sub_scroll + VIS)
                g_tv_sub_scroll += C;
        } else if (g_tv_sub_start + g_tv_sub_count < g_tv_sub_total) {
            int first = xmb_slide_tv_sub_forward();
            if (first >= 0) {
                g_tv_sub_sel    = first;
                g_tv_sub_scroll = (first / C) * C;
            }
        }
    }
    if (BTN_REPEAT(right)) {
        if ((g_tv_sub_sel % C) < C - 1 && g_tv_sub_sel + 1 < g_tv_sub_count)
            g_tv_sub_sel++;
    }
    if (BTN_REPEAT(left)) {
        if ((g_tv_sub_sel % C) > 0) g_tv_sub_sel--;
    }
    if (BTN_PRESSED(cross) && g_tv_sub_count > 0 && g_tv_sub_sel < g_tv_sub_count) {
        const XMBItem *it = &g_tv_sub_items[g_tv_sub_sel];
        if (g_tv_depth == 1) {
            strncpy(g_tv_season_id,   it->id,   sizeof(g_tv_season_id)-1);
            strncpy(g_tv_season_name, it->name, sizeof(g_tv_season_name)-1);
            g_tv_sub_start = 0; g_tv_sub_total = 0;
            g_tv_sub_count = xmb_fetch_episodes(g_tv_series_id, g_tv_season_id,
                                                 g_tv_sub_items, XMB_ITEMS_MAX,
                                                 0, &g_tv_sub_total);
            g_tv_depth = 2; g_tv_sub_sel = 0; g_tv_sub_scroll = 0;
        } else {
            xmb_play_episode_with_next(&g_tv_sub_items[g_tv_sub_sel], 0);
            g_tv_depth = 0;
            g_tv_sub_sel = 0;
            g_tv_sub_scroll = 0;
        }
    }
}

// Collections sub-screen (Collection -> Movies) — card grid.
static void xmb_input_col_sub(void) {
    GridGeom gg;
    xmb_grid_geom(XMB_TAB_COLLECTIONS, &gg);
    const int C   = gg.cols;
    const int VIS = gg.vis;
    if (BTN_PRESSED(circle)) {
        g_col_depth = 0;
        g_col_sub_sel = 0; g_col_sub_scroll = 0; g_col_sub_start = 0; g_col_sub_total = 0;
        return;
    }
    if (BTN_REPEAT(up)) {
        if (g_col_sub_sel >= C) {
            g_col_sub_sel -= C;
            if (g_col_sub_sel < g_col_sub_scroll)
                g_col_sub_scroll = (g_col_sub_sel / C) * C;
        } else if (g_col_sub_start > 0) {
            // Earlier pages were dropped by the forward slide: fetch back.
            int n = xmb_slide_col_sub_backward();
            if (n > 0) {
                g_col_sub_sel += n - C;   // one row up, same column
                if (g_col_sub_sel < 0) g_col_sub_sel = 0;
                g_col_sub_scroll = (g_col_sub_sel / C) * C;
            }
        }
    }
    if (BTN_REPEAT(down)) {
        if (g_col_sub_sel + C < g_col_sub_count) {
            g_col_sub_sel += C;
            if (g_col_sub_sel >= g_col_sub_scroll + VIS)
                g_col_sub_scroll += C;
        } else if (g_col_sub_sel / C < (g_col_sub_count - 1) / C) {
            g_col_sub_sel = g_col_sub_count - 1;
            if (g_col_sub_sel >= g_col_sub_scroll + VIS)
                g_col_sub_scroll += C;
        } else if (g_col_sub_start + g_col_sub_count < g_col_sub_total) {
            int first = xmb_slide_col_sub_forward();
            if (first >= 0) {
                g_col_sub_sel    = first;
                g_col_sub_scroll = (first / C) * C;
            }
        }
    }
    if (BTN_REPEAT(right)) {
        if ((g_col_sub_sel % C) < C - 1 && g_col_sub_sel + 1 < g_col_sub_count)
            g_col_sub_sel++;
    }
    if (BTN_REPEAT(left)) {
        if ((g_col_sub_sel % C) > 0) g_col_sub_sel--;
    }
    if (BTN_PRESSED(cross) && g_col_sub_count > 0 && g_col_sub_sel < g_col_sub_count) {
        xmb_play_list_with_next(g_col_sub_items, g_col_sub_count,
                                g_col_sub_sel, "NEXT MOVIE",
                                "Press SELECT for next movie");
        g_col_depth = 0;
        g_col_sub_sel = 0;
        g_col_sub_scroll = 0;
    }
}

// -------------------------------------------------------
// Music tab — sub-tab header + Artist/Genre→Albums sub-screen
// -------------------------------------------------------

// Switch the music content sub-tab: clear the letter filter and force a
// refetch (the frame loop reloads whenever g_items_loaded drops).
static void music_set_subtab(int st) {
    if (st < 0)               st = 0;
    if (st >= MUSIC_ST_COUNT) st = MUSIC_ST_COUNT - 1;
    if (st == g_music_subtab) return;
    g_music_subtab = st;
    g_tab_name_filter[XMB_TAB_MUSIC][0] = '\0';
    g_items_loaded[XMB_TAB_MUSIC] = false;
    g_item_count[XMB_TAB_MUSIC]   = 0;
    g_tab_start[XMB_TAB_MUSIC]    = 0;
    g_tab_total[XMB_TAB_MUSIC]    = 0;
    g_sel        = 0;
    g_scroll_top = 0;
}

// Header row focused: LEFT/RIGHT switch sub-tab, DOWN/X drop into the grid.
static void xmb_input_music_header(void) {
    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return; }
    if (BTN_REPEAT(left))  music_set_subtab(g_music_subtab - 1);
    if (BTN_REPEAT(right)) music_set_subtab(g_music_subtab + 1);
    if (BTN_PRESSED(down) || BTN_PRESSED(cross) || BTN_PRESSED(circle))
        g_music_header = false;
}

// Albums-of-one-artist/genre sub-screen (single fetched page, no sliding —
// a 50-album discography is already an outlier).
static void xmb_input_music_sub(void) {
    GridGeom gg;
    xmb_grid_geom(XMB_TAB_MUSIC, &gg);
    const int C   = gg.cols;
    const int VIS = gg.vis;
    if (BTN_PRESSED(circle)) {
        g_music_depth = 0;
        g_music_sub_sel = 0; g_music_sub_scroll = 0; g_music_sub_total = 0;
        return;
    }
    if (BTN_REPEAT(up) && g_music_sub_sel >= C) {
        g_music_sub_sel -= C;
        if (g_music_sub_sel < g_music_sub_scroll)
            g_music_sub_scroll = (g_music_sub_sel / C) * C;
    }
    if (BTN_REPEAT(down)) {
        if (g_music_sub_sel + C < g_music_sub_count) {
            g_music_sub_sel += C;
            if (g_music_sub_sel >= g_music_sub_scroll + VIS)
                g_music_sub_scroll += C;
        } else if (g_music_sub_sel / C < (g_music_sub_count - 1) / C) {
            g_music_sub_sel = g_music_sub_count - 1;
            if (g_music_sub_sel >= g_music_sub_scroll + VIS)
                g_music_sub_scroll += C;
        }
    }
    if (BTN_REPEAT(right)) {
        if ((g_music_sub_sel % C) < C - 1 &&
            g_music_sub_sel + 1 < g_music_sub_count)
            g_music_sub_sel++;
    }
    if (BTN_REPEAT(left)) {
        if ((g_music_sub_sel % C) > 0) g_music_sub_sel--;
    }
    if (BTN_PRESSED(cross) && g_music_sub_count > 0 &&
        g_music_sub_sel < g_music_sub_count) {
        music_screen_open_album(&g_music_sub_items[g_music_sub_sel],
                                g_music_parent_name);
        init_btns();
    }
}

// Jump bar — alphabetical letter filter.
static void xmb_input_jumpbar(int tab) {
    if (BTN_REPEAT(up))
        g_jumpbar_sel = (g_jumpbar_sel - 1 + JBAR_ENTRIES) % JBAR_ENTRIES;
    if (BTN_REPEAT(down))
        g_jumpbar_sel = (g_jumpbar_sel + 1) % JBAR_ENTRIES;
    if (BTN_PRESSED(right) || BTN_PRESSED(circle))
        g_jumpbar_active = false;
    if (BTN_PRESSED(cross)) {
        if (g_jumpbar_sel == 0) {
            g_tab_name_filter[tab][0] = '#';
            g_tab_name_filter[tab][1] = '\0';
        } else {
            g_tab_name_filter[tab][0] = (char)('A' + g_jumpbar_sel - 1);
            g_tab_name_filter[tab][1] = '\0';
        }
        g_items_loaded[tab] = false;
        g_item_count[tab]   = 0;
        g_tab_start[tab]    = 0;
        g_tab_total[tab]    = 0;
        g_sel               = 0;
        g_scroll_top        = 0;
        g_jumpbar_active    = false;
    }
}

// -------------------------------------------------------
// Browse input dispatcher
// -------------------------------------------------------

bool xmb_handle_input_browse(void) {
    static bool s_movie_just_exited = false;
    int tab = g_active_tab;

    if (tab == XMB_TAB_SETTINGS) return xmb_input_settings();

    if (BTN_PRESSED(l1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, -1)); return false; }
    if (BTN_PRESSED(r1)) { xmb_switch_tab(xmb_next_enabled(g_active_tab, +1)); return false; }

    if (tab == XMB_TAB_TV && g_tv_depth > 0)           { xmb_input_tv_sub();  return false; }
    if (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0) { xmb_input_col_sub(); return false; }
    if (tab == XMB_TAB_MUSIC && g_music_depth > 0)     { xmb_input_music_sub();    return false; }
    if (tab == XMB_TAB_MUSIC && g_music_header)        { xmb_input_music_header(); return false; }

    bool jbar_mode = g_jumpbar_active &&
        (tab == XMB_TAB_MOVIES || tab == XMB_TAB_TV ||
         tab == XMB_TAB_MUSIC  || tab == XMB_TAB_COLLECTIONS);
    if (jbar_mode) { xmb_input_jumpbar(tab); return false; }

    // Normal browse — 3-column card grid.
    if (BTN_PRESSED(circle)) return false;

    int count = g_item_count[tab];
    GridGeom gg;
    xmb_grid_geom(tab, &gg);
    const int C   = gg.cols;
    const int VIS = gg.vis;

    if (BTN_REPEAT(up)) {
        if (g_sel >= C) {
            g_sel -= C;
            if (g_sel < g_scroll_top) g_scroll_top = (g_sel / C) * C;
        } else if (g_tab_start[tab] > 0) {
            // Top of the loaded window with earlier pages dropped by the
            // forward slide: fetch the previous page back in.
            int n = xmb_slide_tab_backward(tab);
            if (n > 0) {
                g_sel += n - C;   // one row up, same column
                if (g_sel < 0) g_sel = 0;
                g_scroll_top = (g_sel / C) * C;
            }
        } else if (tab == XMB_TAB_MUSIC) {
            // Top row: move d-pad focus up onto the sub-tab header.
            g_music_header = true;
            return false;
        }
    }
    if (BTN_REPEAT(down)) {
        if (g_sel + C < count) {
            g_sel += C;
            if (g_sel >= g_scroll_top + VIS) g_scroll_top += C;
        } else if (g_sel / C < (count - 1) / C) {
            g_sel = count - 1;          // partial last row
            if (g_sel >= g_scroll_top + VIS) g_scroll_top += C;
        } else if (g_tab_start[tab] + count < g_tab_total[tab]) {
            // Infinite scroll: slide the window forward one page.
            int first = xmb_slide_tab_forward(tab);
            if (first >= 0) {
                g_sel        = first;
                g_scroll_top = (first / C) * C;
            }
        }
    }
    if (BTN_REPEAT(right)) {
        if ((g_sel % C) < C - 1 && g_sel + 1 < count) g_sel++;
    }
    if (BTN_REPEAT(left) && (g_sel % C) > 0) {
        g_sel--;
    } else if (BTN_PRESSED(left) && (g_sel % C) == 0 &&
               (tab == XMB_TAB_MOVIES || tab == XMB_TAB_TV ||
                tab == XMB_TAB_MUSIC  || tab == XMB_TAB_COLLECTIONS)) {
        // Left at the first column opens the letter jump bar.
        const char *filt = g_tab_name_filter[tab];
        if (filt[0] == '#') {
            g_jumpbar_sel = 0;
        } else if (filt[0] >= 'A' && filt[0] <= 'Z') {
            g_jumpbar_sel = 1 + (filt[0] - 'A');
        } else if (count > 0) {
            int idx = (g_sel < count) ? g_sel : 0;
            char c = (char)toupper((unsigned char)g_items[tab][idx].name[0]);
            g_jumpbar_sel = (c >= 'A' && c <= 'Z') ? 1 + (c - 'A') : 0;
        } else {
            g_jumpbar_sel = 1;
        }
        g_jumpbar_active = true;
        return false;
    }

    if (s_movie_just_exited) { s_movie_just_exited = false; return false; }

    if (BTN_PRESSED(cross) && count > 0 && g_sel < count) {
        const XMBItem *it = &g_items[tab][g_sel];
        if (tab == XMB_TAB_TV && strcmp(it->type, "Series") == 0) {
            strncpy(g_tv_series_id,   it->id,   sizeof(g_tv_series_id)-1);
            strncpy(g_tv_series_name, it->name, sizeof(g_tv_series_name)-1);
            g_tv_sub_start = 0; g_tv_sub_total = 0;
            g_tv_sub_count = xmb_fetch_seasons(g_tv_series_id, g_tv_sub_items, XMB_ITEMS_MAX,
                                                0, &g_tv_sub_total);
            g_tv_depth = 1; g_tv_sub_sel = 0; g_tv_sub_scroll = 0;
        } else if (tab == XMB_TAB_COLLECTIONS) {
            strncpy(g_col_id,   it->id,   sizeof(g_col_id)-1);
            strncpy(g_col_name, it->name, sizeof(g_col_name)-1);
            g_col_sub_start = 0; g_col_sub_total = 0;
            g_col_sub_count = xmb_fetch_collection_items(g_col_id, g_col_sub_items, XMB_ITEMS_MAX,
                                                          0, &g_col_sub_total);
            g_col_depth = 1; g_col_sub_sel = 0; g_col_sub_scroll = 0;
        } else if (tab == XMB_TAB_MUSIC) {
            if (strcmp(it->type, "MusicAlbum") == 0) {
                // Album → Now Playing (blocks until the user backs out).
                music_screen_open_album(it, "Albums");
                s_movie_just_exited = true;
                init_btns();
                return false;
            } else if (strcmp(it->type, "Audio") == 0) {
                // Songs list → play from here, rest of the page queued.
                music_screen_open_songs(g_items[tab], count, g_sel);
                s_movie_just_exited = true;
                init_btns();
                return false;
            } else if (strcmp(it->type, "Playlist") == 0) {
                music_screen_open_playlist(it);
                s_movie_just_exited = true;
                init_btns();
                return false;
            } else if (strcmp(it->type, "MusicArtist") == 0 ||
                       strcmp(it->type, "MusicGenre")  == 0) {
                // Drill into the artist's / genre's albums.
                strncpy(g_music_parent_id,   it->id,
                        sizeof(g_music_parent_id)-1);
                strncpy(g_music_parent_name, it->name,
                        sizeof(g_music_parent_name)-1);
                g_music_sub_total = 0;
                g_music_sub_count = xmb_fetch_music_children(
                    strcmp(it->type, "MusicArtist") == 0 ? "AlbumArtistIds"
                                                         : "GenreIds",
                    it->id, g_music_sub_items, XMB_ITEMS_MAX,
                    &g_music_sub_total);
                g_music_depth = 1;
                g_music_sub_sel = 0; g_music_sub_scroll = 0;
            }
        } else {
            // Continue Watching launches at the saved position.
            u32 resume = (tab == XMB_TAB_RESUME) ? it->resume_secs : 0;
            if (strcmp(it->type, "Episode") == 0)
                xmb_play_episode_with_next(it, resume);
            else
                xmb_play_item(it, resume);
            s_movie_just_exited = true;
            init_btns();
            return false;
        }
    }

    // Triangle — detail overlay
    if (btn_cur.triangle || btn_prev.triangle) {
        char dbg[200];
        snprintf(dbg, sizeof(dbg),
            "outer: triangle state cur=%d prev=%d (BTN_PRESSED would be %d)",
            btn_cur.triangle, btn_prev.triangle,
            (btn_cur.triangle && !btn_prev.triangle) ? 1 : 0);
        plog(dbg);
    }
    u64 now_us = timing_get_us();
    if (BTN_PRESSED(triangle) && count > 0 && g_sel < count
        && now_us >= g_info_cooldown_until) {
        xmb_show_item_info(&g_items[tab][g_sel]);
    }
    return false;
}
