// XMB main screen — per-frame loop: input dispatch, CPU draw phase,
// RSX/TTF draw phase, contextual hints.

#include <stdio.h>
#include <string.h>

#include <rsx/rsx.h>
#include <sysutil/sysutil.h>

#include "ui_internal.h"
#include "ui_wave.h"

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
    g_active_tab = XMB_TAB_MOVIES;
    g_sel = 0; g_scroll_top = 0;
    g_tv_sub_start = 0; g_tv_sub_total = 0;
    g_col_sub_start = 0; g_col_sub_total = 0;
    g_osk_row = 0; g_osk_col = 0; g_osk_sym = false;
}

// CPU draw phase — direct framebuffer writes, runs after rsxSync().
static void xmb_draw_cpu_phase(int tab) {
    // Divider line
    u32 *div = color_buffer[curr_fb] + XMB_DIVIDER_Y * display_width;
    for (u32 x = 0; x < display_width; x++) div[x] = XMB_DIVIDER_CLR;

    if (tab == XMB_TAB_SEARCH) {
        xmb_cpu_draw_osk();
        xmb_cpu_draw_search_results();
    } else if (tab == XMB_TAB_SETTINGS) {
        xmb_cpu_draw_settings();
    } else if (tab != XMB_TAB_MUSIC) {
        if (tab == XMB_TAB_TV && g_tv_depth > 0)
            xmb_cpu_draw_sub();
        else if (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0)
            xmb_cpu_draw_col_sub();
        else
            xmb_cpu_draw_items(tab);
    }
}

// TTF/RSX draw phase — text and icons on top of the CPU-drawn layer.
static void xmb_draw_text_phase(int tab) {
    drawTTF(XMB_ITEM_PAD, 16, "JELLYFIN-PS3", 28, 0x007C3CEA, true);
    draw_topbar_lr();

    {
        const char *tab_name = g_tabs[g_active_tab].label;
        int hx = (int)display_width / 2 - ttf_text_width(tab_name, 28) / 2;
        if (hx < (int)XMB_ITEM_PAD) hx = (int)XMB_ITEM_PAD;
        drawTTF((u32)hx, (u32)(XMB_DIVIDER_Y + 14), tab_name, 28, 0x00FFFFFF);
    }

    if (tab == XMB_TAB_SEARCH) {
        xmb_rsx_draw_osk();
    } else if (tab == XMB_TAB_SETTINGS) {
        xmb_draw_settings();
    } else if (tab == XMB_TAB_MUSIC) {
        drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 40), "Coming soon", 22, 0x00FFFFFF);
        xmb_draw_jumpbar(tab);
    } else if (tab == XMB_TAB_TV && g_tv_depth > 0) {
        char crumb[256];
        if (g_tv_depth == 1)
            snprintf(crumb, sizeof(crumb), "%s > Seasons", g_tv_series_name);
        else
            snprintf(crumb, sizeof(crumb), "%s > %s > Episodes",
                     g_tv_series_name, g_tv_season_name);
        drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 2), crumb, 14, 0x00888888);
        if (g_tv_sub_count == 0)
            drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 46), "No items.", 16, 0x00FFFFFF);
        else
            xmb_draw_sub_list();
    } else if (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0) {
        char crumb[256];
        snprintf(crumb, sizeof(crumb), "%s > Movies", g_col_name);
        drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 2), crumb, 14, 0x00888888);
        if (g_col_sub_count == 0)
            drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 46), "No items.", 16, 0x00FFFFFF);
        else
            xmb_draw_col_sub_list();
    } else {
        if (g_items_loaded[tab] && g_item_count[tab] == 0)
            drawTTF(XMB_ITEM_PAD, (u32)(XMB_CONTENT_Y + 20), "No items.", 16, 0x00FFFFFF);
        else
            xmb_draw_item_list(tab);
        xmb_draw_jumpbar(tab);
    }
}

// Contextual hints bar for the current tab / mode.
static void xmb_draw_hints(int tab) {
    bool in_tv_sub  = (tab == XMB_TAB_TV          && g_tv_depth  > 0);
    bool in_col_sub = (tab == XMB_TAB_COLLECTIONS && g_col_depth > 0);

    if (tab == XMB_TAB_SETTINGS) {
        if (g_settings_confirm) {
            static const Hint h[] = {{'X',"CONFIRM"},{'C',"CANCEL"}};
            draw_hints_bar(h, 2);
        } else {
            static const Hint h[] = {{'D',"NAV"},{'X',"SELECT"}};
            draw_hints_bar(h, 2);
        }
    } else if (tab == XMB_TAB_SEARCH) {
        if (g_search_focus_results) {
            static const Hint h[] = {{'D',"NAV"},{'X',"PLAY"},{'C',"BACK"}};
            draw_hints_bar(h, 3);
        } else {
            static const Hint h[] = {{'D',"NAV"},{'X',"TYPE"},{'C',"CLEAR"}};
            draw_hints_bar(h, 3);
        }
    } else if (in_tv_sub || in_col_sub) {
        static const Hint h[] = {{'D',"NAV"},{'X',"SELECT"},{'C',"BACK"}};
        draw_hints_bar(h, 3);
    } else if (g_jumpbar_active) {
        static const Hint h[] = {{'D',"NAV"},{'X',"FILTER"},{'C',"CANCEL"}};
        draw_hints_bar(h, 3);
    } else {
        static const Hint h[] = {{'D',"NAV"},{'E',"JUMP"},{'X',"SELECT"},{'C',"BACK"},{'T',"INFO"}};
        draw_hints_bar(h, 5);
    }
}

void ui_run_xmb(void) {
    xmb_reset_state();
    wave_reset();

    xmb_detect_tabs();

    if (!g_tabs[XMB_TAB_MOVIES].enabled) {
        for (int t = 0; t < XMB_TAB_COUNT; t++) {
            if (g_tabs[t].enabled) { g_active_tab = t; break; }
        }
    }

    OSK_Y0 = XMB_CONTENT_Y + 58;

    init_btns();

    while (running) {
        waitflip();
        sysUtilCheckCallback();
        clearScreen(XMB_BG);
        wave_draw();

        int tab = g_active_tab;
        if (tab != XMB_TAB_SEARCH && tab != XMB_TAB_MUSIC && tab != XMB_TAB_SETTINGS)
            if (!g_items_loaded[tab]) xmb_fetch_tab_items(tab);

        poll_buttons();
        bool should_exit = false;
        if (tab == XMB_TAB_SEARCH)
            should_exit = xmb_handle_input_search();
        else
            should_exit = xmb_handle_input_browse();
        if (should_exit) break;

        rsxSync();

        xmb_draw_cpu_phase(tab);
        xmb_draw_text_phase(tab);
        xmb_draw_hints(tab);
        xmb_draw_tabs();

        flip();
        sysUtilCheckCallback();
    }
}
