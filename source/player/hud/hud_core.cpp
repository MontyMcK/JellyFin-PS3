// HUD state and input — visibility/auto-hide, focus navigation across the
// control row, popup menu cursor, and the public setter API.

#include <stdio.h>

#include <ppu-types.h>

#include "player_hud.h"
#include "player_hud_internal.h"
#include "ui.h"
#include "timing.h"
#include "audio.h"
#include "ui_visuals.h"   // g_spine_on: the redesigned HUD's buttons
#include "statsovl.h"

HudState g_hud;

static const int s_incr_vals[3] = { 10, 30, 300 };

void hud_show(void) {
    g_hud.visible = true;
    g_hud.show_us = timing_get_us();
}

// -------------------------------------------------------
// Public API
// -------------------------------------------------------

void hud_init(u32 total_secs, const char *audio_label) {
    g_hud.total_secs = total_secs;
    snprintf(g_hud.audio_label, sizeof(g_hud.audio_label), "%s",
             (audio_label && audio_label[0]) ? audio_label : "AUDIO");
    g_hud.visible      = false;
    g_hud.show_us      = 0;
    g_hud.seek_delta   = 0;
    g_hud.focus        = -1;
    g_hud.incr_idx     = 0;
    g_hud.cc_active    = false;
    g_hud.vol_active   = false;
    g_hud.menu_visible = false;
    g_hud.menu_n       = 0;
    g_hud.menu_choice  = -1;
    g_hud.menu_epoch   = 0;
    g_hud.title[0]     = '\0';
    hud_gpu_init();
}

void hud_shutdown(void) {
    g_hud.visible = false;
    hud_gpu_shutdown();
}

bool hud_is_visible(void) { return g_hud.visible; }
int  hud_seek_delta(void) { return g_hud.seek_delta; }

void hud_set_audio_label(const char *label) {
    snprintf(g_hud.audio_label, sizeof(g_hud.audio_label), "%s",
             (label && label[0]) ? label : "AUDIO");
}

void hud_set_cc_active(bool active) { g_hud.cc_active = active; }

void hud_set_title(const char *title) {
    snprintf(g_hud.title, sizeof(g_hud.title), "%s", title ? title : "");
}

void hud_open_menu(const char *title, const char *const *items,
                   int n_items, int current) {
    if (n_items > MENU_MAX) n_items = MENU_MAX;
    snprintf(g_hud.menu_title, sizeof(g_hud.menu_title), "%s", title ? title : "");
    for (int i = 0; i < n_items; i++) g_hud.menu_items[i] = items[i];
    g_hud.menu_n   = n_items;
    g_hud.menu_cur = (current >= 0 && current < n_items) ? current : 0;
    g_hud.menu_sel = g_hud.menu_cur;
    g_hud.menu_visible = (n_items > 0);
    g_hud.menu_epoch++;
    hud_show();
}

int hud_menu_choice(void) { return g_hud.menu_choice; }

static bool s_skip_offered = false;
void hud_set_skip_offered(bool offered) { s_skip_offered = offered; }

HudAction hud_handle_input(bool l2_pressed, bool r2_pressed, bool paused) {
    g_hud.seek_delta = 0;

    // The skip badge only shows while the bar is hidden, and then X is its
    // button -- checked before anything else so the press does not also
    // reveal the bar.
    if (s_skip_offered && !g_hud.visible && BTN_PRESSED(cross))
        return HUD_ACTION_SKIP_SEGMENT;
    // Media keys (Blu-ray remote, keyboard) act at once, bar shown or not, as
    // on a disc player -- no reveal press first.  PLAY only resumes and PAUSE
    // only pauses, so pressing either twice is harmless.  There are no
    // chapters for NEXT/PREV to skip between, so they jump by the largest HUD
    // increment; SCAN repeats while held.
    if (BTN_PRESSED(playpause) || (BTN_PRESSED(play) && paused) ||
        (BTN_PRESSED(pause) && !paused)) {
        hud_show();
        return HUD_ACTION_TOGGLE_PAUSE;
    }
    if (BTN_PRESSED(stop))     return HUD_ACTION_STOP;
    if (BTN_PRESSED(audio))    return HUD_ACTION_AUDIO_TRACK;
    if (BTN_PRESSED(subtitle)) return HUD_ACTION_SUBTITLE;
    {
        const bool scan = btn_nav_repeat(btn_cur.ffwd || btn_cur.rew, NAV_media);
        const bool skip = BTN_PRESSED(next) || BTN_PRESSED(prev);
        if (scan || skip) {
            const int step = skip ? s_incr_vals[2] : s_incr_vals[g_hud.incr_idx];
            g_hud.seek_delta = (btn_cur.rew || btn_cur.prev) ? -step : step;
            hud_show();
            return HUD_ACTION_SEEK;
        }
    }
    if (BTN_PRESSED(info)) {
#if ENABLE_PLAYER_STATS
        statsovl_set_enabled(!statsovl_enabled());
#endif
        hud_show();
        return HUD_ACTION_NONE;
    }

    // Any button activity wakes the HUD.
    bool was_hidden = !g_hud.visible;
    if (btn_cur.cross || btn_cur.circle || btn_cur.square || btn_cur.triangle ||
        btn_cur.l1 || btn_cur.r1 || btn_cur.l2 || btn_cur.r2 ||
        l2_pressed || r2_pressed ||
        btn_cur.up || btn_cur.down || btn_cur.left || btn_cur.right) {
        hud_show();
        // First button while the bar was hidden just reveals it, with the
        // play/pause control focused by default.
        if (was_hidden) g_hud.focus = FOCUS_PP;
    }

    // Auto-hide after timeout — only when playing; stay visible indefinitely while paused.
    if (g_hud.visible && !paused && (timing_get_us() - g_hud.show_us) >= HUD_SHOW_US) {
        g_hud.visible      = false;
        g_hud.focus        = -1;
        g_hud.menu_visible = false;
        g_hud.vol_active   = false;
        return HUD_ACTION_NONE;
    }
    if (!g_hud.visible) return HUD_ACTION_NONE;

    // Volume slider modal: while open, up/down change the level and X/O close
    // it (d-pad left/right are swallowed so focus stays on the speaker).
    if (g_hud.vol_active) {
        if (BTN_REPEAT(up))        audio_set_volume(audio_get_volume() + VOL_STEP);
        else if (BTN_REPEAT(down)) audio_set_volume(audio_get_volume() - VOL_STEP);
        else if (BTN_PRESSED(cross) || BTN_PRESSED(circle)) g_hud.vol_active = false;
        return HUD_ACTION_NONE;
    }

    // While a popup menu is open it owns the input: up/down move the cursor,
    // X picks the entry, circle closes without picking.
    if (g_hud.menu_visible) {
        if (BTN_PRESSED(up)) {
            if (g_hud.menu_sel > 0) g_hud.menu_sel--;
        } else if (BTN_PRESSED(down)) {
            if (g_hud.menu_sel < g_hud.menu_n - 1) g_hud.menu_sel++;
        } else if (BTN_PRESSED(cross)) {
            g_hud.menu_visible = false;
            g_hud.menu_choice  = g_hud.menu_sel;
            return HUD_ACTION_MENU_SELECT;
        } else if (BTN_PRESSED(circle)) {
            g_hud.menu_visible = false;
        }
        return HUD_ACTION_NONE;
    }

    // O stops playback, bar up or not, as START does: O backs out of
    // everything on the XMB and people reach for it (tester, 2026-09-27).
    // A popup menu or the volume slider, above, takes O as "close" first.
    if (BTN_PRESSED(circle)) return HUD_ACTION_STOP;

    // Square with the bar already up puts it away without waiting for the
    // timeout; the press that brings it up only does that.
    if (BTN_PRESSED(square) && !was_hidden) {
        g_hud.visible = false;
        g_hud.focus   = -1;
        return HUD_ACTION_NONE;
    }

    // D-pad left/right move the focus cursor across the control row, in screen
    // order: REW · PLAY/PAUSE · FF · AUDIO · VOLUME · CC.  This is how you reach
    // the AUDIO, VOLUME and CC controls.  R2/L2 (handled in the main loop) scrub.
    // The reveal press above is swallowed so the bar only navigates once it's
    // already showing.
    if (!was_hidden) {
        if (BTN_PRESSED(left)) {
            if (g_hud.focus < 0)      g_hud.focus = FOCUS_PP;
            else if (g_hud.focus > 0) g_hud.focus--;
            // The redesigned HUD has no REW / FF buttons: those stops only
            // said "back 10s" / "ahead 10s" and confused (user, 09-27).
            // L2 / R2 seek.  Step over them.
            if (g_spine_on && g_hud.focus == FOCUS_FF)  g_hud.focus = FOCUS_PP;
            if (g_spine_on && g_hud.focus == FOCUS_REW) g_hud.focus = FOCUS_PP;
            return HUD_ACTION_NONE;
        }
        if (BTN_PRESSED(right)) {
            if (g_hud.focus < 0)                   g_hud.focus = FOCUS_PP;
            else if (g_hud.focus < FOCUS_COUNT - 1) g_hud.focus++;
            if (g_spine_on && g_hud.focus == FOCUS_FF)  g_hud.focus = FOCUS_AUDIO;
            return HUD_ACTION_NONE;
        }
    }

    // The redesigned HUD (design-import-v3 "07 · Player HUD") names Triangle
    // Tracks in its hint cluster.  Only with the spine gate on, and not on the
    // press that revealed the bar.  (O and Square are handled above for both
    // HUDs; the stats overlay is on DISPLAY and in Settings.)
    if (g_spine_on && !was_hidden) {
        if (BTN_PRESSED(triangle)) return HUD_ACTION_AUDIO_TRACK;
    }

    // X (cross) activates the focused control.
    if (BTN_PRESSED(cross)) {
        switch (g_hud.focus) {
        case FOCUS_REW:    g_hud.seek_delta = -s_incr_vals[g_hud.incr_idx]; return HUD_ACTION_SEEK;
        case FOCUS_FF:     g_hud.seek_delta = +s_incr_vals[g_hud.incr_idx]; return HUD_ACTION_SEEK;
        case FOCUS_AUDIO:  return HUD_ACTION_AUDIO_TRACK;
        case FOCUS_VOLUME: g_hud.vol_active = true; return HUD_ACTION_NONE;
        case FOCUS_CC:     return HUD_ACTION_SUBTITLE;
        case FOCUS_PP:
        default:          return HUD_ACTION_TOGGLE_PAUSE;
        }
    }

    return HUD_ACTION_NONE;
}
