// Settings tab rendering: the account card and the sectioned list of
// settings.  What the rows are, their order and the layout/scroll rules are
// the host-tested model in ui/settings_model.c; this file keeps what only the
// console knows about each row -- its icon, how its value reads, and what
// X and Left/Right do -- in a table keyed by setting_id.

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ui_visuals.h"
#include "settings_model.h"
#include "jellyfin_api.h"
#include "update_check.h"
#include "log_upload.h"
#include "plog.h"
#include "hd1080.h"
#include "surround.h"
#include "audio_bitstream.h"   // audio_passthrough_wanted(): Dialogue Boost n/a
#include "centermix.h"
#include "statsovl.h"
#include "menusnow.h"
#include "autoskip.h"
#include "directstream.h"
#include "display_24p.h"    // 24Hz Output
#include "month_bg.h"       // Day / Night Palette
#include "ui_wave_audio.h"  // Wave Intensity
#include "subfont.h"
#include "subcolor.h"
#include "dl_manager.h"     // Downloads / Offline Library rows
#include "dl_service.h"     // Download Location
#include "i18n_store.h"     // Language
#include "wol.h"            // Server MAC Address, Wake Server
#include "wol_net.h"
#include "timing.h"

// xmb/ui_peek.cpp (declared in ui_internal.h, not included here).
void peek_open_text(const char *title, const char *body, int x, int y, int w, int h);

// -------------------------------------------------------------------------
//  The console half of each row
// -------------------------------------------------------------------------
//  value:    the right-aligned text and whether it is drawn lit (accent)
//  activate: X.  NULL = nothing.
//  step:     Left / Right.  NULL on action rows (Log Out, Screen Size,
//            Software Update, Send Log, Downloads), which ignore it; a toggle
//            flips on either direction.

typedef struct {
    setting_id id;
    int        icon;
    void     (*value)(char *buf, int cap, bool *lit);
    void     (*activate)(void);
    void     (*step)(int dir);
} UiRow;

static void val_text(char *buf, int cap, bool *lit, const char *t, bool on) {
    snprintf(buf, (size_t)cap, "%s", t);
    *lit = on;
}
static void val_onoff(char *buf, int cap, bool *lit, bool on) {
    val_text(buf, cap, lit, on ? TR("On") : TR("Off"), on);
}

// ---- values ----
static void v_debug(char *b, int c, bool *l)    { val_onoff(b, c, l, plog_enabled()); }
static void v_hd1080(char *b, int c, bool *l)   { val_onoff(b, c, l, hd1080_enabled()); }
static void v_particles(char *b, int c, bool *l){ val_onoff(b, c, l, menusnow_enabled()); }
static void v_daynight(char *b, int c, bool *l) { val_onoff(b, c, l, daynight_enabled()); }
static void v_autoskip(char *b, int c, bool *l) { val_onoff(b, c, l, autoskip_enabled()); }
#if ENABLE_PLAYER_STATS
static void v_stats(char *b, int c, bool *l)    { val_onoff(b, c, l, statsovl_enabled()); }
#endif
static void v_dlloc(char *b, int c, bool *l) {
    if (!dl_location_usb()) { val_text(b, c, l, TR("Internal HDD"), false); return; }
    char drive[48], note[8];
    dl_location_drive(drive, sizeof(drive));
    dl_location_note(note, sizeof(note));
    if (drive[0])     { snprintf(b, (size_t)c, "USB: %s", drive); *l = true; }
    else if (note[0]) val_text(b, c, l, TR("USB (not ready)"), false);
    else              val_text(b, c, l, TR("USB drive"), true);
}
static void v_direct(char *b, int c, bool *l)   { val_text(b, c, l, directstream_enabled() ? TR("Auto") : TR("Off"), directstream_enabled()); }
static void v_24hz(char *b, int c, bool *l)     { val_text(b, c, l, d24_enabled() ? TR("Auto") : TR("Off"), d24_enabled()); }
static void v_audio(char *b, int c, bool *l)    { val_text(b, c, l, tr(surround_mode_label()), surround_enabled()); }
static void v_dialogue(char *b, int c, bool *l) {
    // Nothing is decoded here with Dolby Digital output, so there is nothing
    // to boost: the value reads n/a and is drawn faint.
    if (audio_passthrough_wanted()) val_text(b, c, l, TR("n/a"), false);
    else                            val_text(b, c, l, tr(centermix_label()), centermix_active());
}
static void v_subfont(char *b, int c, bool *l)  { val_text(b, c, l, subfont_label(), subfont_get() != 0); }
static void v_subcolor(char *b, int c, bool *l) { val_text(b, c, l, tr(subcolor_label()), subcolor_get() != 0); }
// Always lit: the accent IS what the row changes, so its colour previews it.
static void v_theme(char *b, int c, bool *l)    { val_text(b, c, l, theme_current_name(), true); }
static void v_wave(char *b, int c, bool *l)     { val_text(b, c, l, tr(wave_audio_level_label()), wave_audio_level() != 0); }
static void v_language(char *b, int c, bool *l) { val_text(b, c, l, i18n_pref_label(i18n_pref()), true); }
static void v_screen(char *b, int c, bool *l) {
    const int pm = (int)(overscan_frac() * 1000.0f + 0.5f);   // permille
    if (pm == 0) val_text(b, c, l, TR("Off"), false);
    else { snprintf(b, (size_t)c, "%d.%d%%", pm / 10, pm % 10); *l = true; }
}
static void v_update(char *b, int c, bool *l) {
    const int st = update_check_state();
    if (st == UPD_AVAILABLE) {
        char tag[32] = "";
        update_check_result(tag, sizeof tag);
        const char *v = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
        snprintf(b, (size_t)c, TR("%s available"), v);
    } else {
        if (st == UPD_CURRENT) snprintf(b, (size_t)c, TR("Up to date (%s)"), APP_VERSION);
        else snprintf(b, (size_t)c, "%s",
                 st == UPD_CHECKING ? TR("Checking...")
               : st == UPD_FAILED   ? TR("Couldn't check")
               :                      APP_VERSION);
    }
    *l = st == UPD_AVAILABLE;
}
static void v_sendlog(char *b, int c, bool *l) {
    const int st = log_upload_state();
    val_text(b, c, l,
             st == LOGUP_SENDING ? TR("Sending...")
           : st == LOGUP_SENT    ? TR("Sent")
           : st == LOGUP_FAILED  ? TR("Failed")
           : st == LOGUP_NOLOG   ? TR("No log yet")
           :                       TR("X to send"),
             st == LOGUP_SENT);
}
// The tallies are a lock and a walk of the slot table (no copies, no disk).
static void v_downloads(char *b, int c, bool *l) {
    int active = 0, completed = 0, failed = 0;
    dl_counts(&active, &completed, &failed);
    if (!dl_manager_ready())  { val_text(b, c, l, TR("Unavailable"), false); return; }
    if (active)      { snprintf(b, (size_t)c, TR("%d active"), active); *l = true; }
    else if (failed) { snprintf(b, (size_t)c, TR("%d failed"), failed); *l = false; }
    else             val_text(b, c, l, TR("None"), false);
}
static void v_offline(char *b, int c, bool *l) {
    int active = 0, completed = 0, failed = 0;
    dl_counts(&active, &completed, &failed);
    if (!dl_manager_ready())  { val_text(b, c, l, TR("Unavailable"), false); return; }
    if (completed) { snprintf(b, (size_t)c, "%d", completed); *l = true; }
    else           val_text(b, c, l, TR("Empty"), false);
}

// ---- activation and stepping ----
static void a_logout(void)     { g_settings_confirm = true; }
static void a_screen(void)     { g_overscan_calib_prev = overscan_frac(); g_overscan_calib = true; }
static void a_update(void) {
    if (update_check_state() == UPD_AVAILABLE) xmb_update_popup_reopen();
    else                                       update_check_again();
}
static void a_sendlog(void)    { log_upload_start(); }

// Wake-on-LAN (net/wol.h): the server PC's MAC, entered once, and the send.
static u64 s_wake_sent_us = 0;
static void v_wolmac(char *b, int c, bool *l) {
    uint8_t mac[6];
    if (wol_saved(mac)) { wol_format_mac(mac, b, c); *l = true; }
    else                val_text(b, c, l, TR("Not set"), false);
}
static void v_wake(char *b, int c, bool *l) {
    uint8_t mac[6];
    if (!wol_saved(mac))                                       val_text(b, c, l, TR("Set the MAC first"), false);
    else if (s_wake_sent_us && timing_get_us() - s_wake_sent_us < 5000000ULL) val_text(b, c, l, TR("Sent"), true);
    else                                                       val_text(b, c, l, TR("X to send"), false);
}
static void a_wolmac(void) {
    char text[40] = "";
    uint8_t mac[6];
    if (wol_saved(mac)) wol_format_mac(mac, text, sizeof text);
    if (get_input(text, sizeof text, TR("Server MAC address (like 1C:69:7A:12:34:56)"), false) != 1) return;
    init_btns();
    if (!text[0]) return;
    if (!wol_parse_mac(text, mac)) {
        xmb_dl_notice(TR("Wake-on-LAN"), TR("That is not a MAC address: six pairs of digits, like 1C:69:7A:12:34:56."));
        return;
    }
    wol_save(mac);
}
static void a_wake(void) {
    uint8_t mac[6];
    if (!wol_saved(mac)) {
        xmb_dl_notice(TR("Wake-on-LAN"), TR("Enter the server's MAC address first (Server MAC Address)."));
        return;
    }
    if (wol_send(mac)) s_wake_sent_us = timing_get_us();
}
static void a_downloads(void)  { xmb_show_downloads(); }
static void a_offline(void)    { xmb_show_offline(); }

static void t_debug(int)       { plog_set_enabled(!plog_enabled()); }
static void t_hd1080(int)      { hd1080_set_enabled(!hd1080_enabled()); }
static void t_particles(int)   { menusnow_set_enabled(!menusnow_enabled()); }
static void t_daynight(int)    { daynight_set_enabled(!daynight_enabled()); }
static void t_autoskip(int)    { autoskip_set_enabled(!autoskip_enabled()); }
#if ENABLE_PLAYER_STATS
static void t_stats(int)       { statsovl_set_enabled(!statsovl_enabled()); }
#endif
static void t_direct(int)      { directstream_set_enabled(!directstream_enabled()); }
static void t_dlloc(int)       { dl_location_set_usb(!dl_location_usb()); }
static void t_24hz(int)        { d24_set_enabled(!d24_enabled()); }
static void s_audio(int d)     { surround_step(d); }
static void s_dialogue(int d)  { if (!audio_passthrough_wanted()) centermix_step(d); }
static void s_subfont(int d)   { subfont_step(d); }
static void s_subcolor(int d)  { subcolor_step(d); }
static void s_theme(int d)     { theme_step(d); }
static void s_wave(int d)      { wave_audio_step(d); }
static void s_language(int d)  { i18n_step_pref(d); }

// A toggle takes X and either direction as the same flip; a value row takes X
// as a step forward.  Neither names an activate function: X falls through to
// step(+1).  An action row has no step, so Left and Right ignore it.
#define TOGGLE(id, icon, val, flip) { id, icon, val, NULL, flip }
#define VALUE(id, icon, val, st)    { id, icon, val, NULL, st }
#define ACTION(id, icon, val, act)  { id, icon, val, act, NULL }

// ICON_BUG is reused for diagnostics rows and ICON_MUSIC for audio: the icon
// font is a 21-glyph subset (ui/fonts/tabler_icons.h) and a new glyph would
// mean regenerating it.
static const UiRow k_ui[] = {
    TOGGLE(SET_DEBUG_LOG,   ICON_BUG,         v_debug,     t_debug),
    ACTION(SET_SCREEN_SIZE, ICON_TV,          v_screen,    a_screen),
    TOGGLE(SET_HD1080,      ICON_MOVIE,       v_hd1080,    t_hd1080),
    VALUE (SET_AUDIO_OUT,   ICON_MUSIC,       v_audio,     s_audio),
    VALUE (SET_DIALOGUE,    ICON_MUSIC,       v_dialogue,  s_dialogue),
    VALUE (SET_SUB_FONT,    ICON_TV,          v_subfont,   s_subfont),
    VALUE (SET_SUB_COLOUR,  ICON_TV,          v_subcolor,  s_subcolor),
    VALUE (SET_LANGUAGE,    ICON_TV,          v_language,  s_language),
    VALUE (SET_THEME,       ICON_PHOTO,       v_theme,     s_theme),
    TOGGLE(SET_PARTICLES,   ICON_PHOTO,       v_particles, t_particles),
    TOGGLE(SET_DAYNIGHT,    ICON_PHOTO,       v_daynight,  t_daynight),
    VALUE (SET_WAVE_INT,    ICON_MUSIC,       v_wave,      s_wave),
    TOGGLE(SET_AUTOSKIP,    ICON_MOVIE,       v_autoskip,  t_autoskip),
    TOGGLE(SET_24HZ,        ICON_TV,          v_24hz,      t_24hz),
    TOGGLE(SET_DIRECT_STREAM, ICON_MOVIE,     v_direct,    t_direct),
    ACTION(SET_UPDATE,      ICON_TV,          v_update,    a_update),
    ACTION(SET_SEND_LOG,    ICON_BUG,         v_sendlog,   a_sendlog),
    ACTION(SET_WOL_MAC,     ICON_SETTINGS,    v_wolmac,    a_wolmac),
    ACTION(SET_WAKE,        ICON_TV,          v_wake,      a_wake),
#if ENABLE_PLAYER_STATS
    TOGGLE(SET_STATS,       ICON_BUG,         v_stats,     t_stats),
#endif
    // Downloads: the stacked-cards glyph (a queue); Offline: play.
    TOGGLE(SET_DL_LOCATION, ICON_COLLECTIONS, v_dlloc,     t_dlloc),
    ACTION(SET_DOWNLOADS,   ICON_COLLECTIONS, v_downloads, a_downloads),
    ACTION(SET_OFFLINE_LIB, ICON_PLAY,        v_offline,   a_offline),
    ACTION(SET_LOGOUT,      ICON_LOGOUT,      NULL,        a_logout),
};

static const UiRow *ui_of(setting_id id) {
    for (unsigned i = 0; i < sizeof(k_ui) / sizeof(k_ui[0]); i++)
        if (k_ui[i].id == id) return &k_ui[i];
    return NULL;
}

// Rows by their position in the displayed order (what g_settings_sel is).
static const setting_row *row_at(int i) { return settings_row(i); }

void settings_activate(int row) {
    const setting_row *r = row_at(row);
    const UiRow *u = r ? ui_of(r->id) : NULL;
    if (!u) return;
    if (u->activate)  u->activate();
    else if (u->step) u->step(+1);
}

void settings_step(int row, int dir) {
    const setting_row *r = row_at(row);
    const UiRow *u = r ? ui_of(r->id) : NULL;
    if (u && u->step) u->step(dir);
}

// -------------------------------------------------------------------------
//  Geometry
// -------------------------------------------------------------------------

static void help_rect(int *x, int *y, int *w, int *h) {
    *w = XMB_LIST_W;
    *h = UIS_H(104);
    *x = ((int)display_width - *w) / 2;
    *y = (int)display_height - XMB_BOTTOM_PAD - *h - UIS_H(6);
}

#define SET_PANEL_H UIS_H(96)
#define SET_ROW_H   UIS_H(56)
#define SET_HEADER_H UIS_H(34)

static int settings_panel_y(void) { return XMB_CONTENT_Y + 16; }

// The band the list scrolls in: below the account card, above the safe area.
static int band_top(void)    { return settings_panel_y() + SET_PANEL_H + UIS_H(14); }
static int band_bottom(void) { return (int)display_height - XMB_BOTTOM_PAD; }

// The row rhythm never changes; the list scrolls instead of compressing.
static int settings_row_pitch(void) { return SET_ROW_H + UIS_H(10); }

static struct {
    settings_item items[SETTINGS_MAX_ITEMS];
    int n, total, scroll;
} s_lay;

// Rebuild the layout for this frame's geometry and move the scroll just far
// enough to keep the selection, and its section header, on screen.  Called by
// both draw phases; the second finds nothing left to move.
static void layout_update(void) {
    s_lay.n = settings_layout(s_lay.items, SETTINGS_MAX_ITEMS, SET_HEADER_H,
                              SET_ROW_H, settings_row_pitch(), &s_lay.total);
    s_lay.scroll = settings_scroll_for(s_lay.items, s_lay.n, g_settings_sel,
                                       band_bottom() - band_top(), s_lay.scroll,
                                       s_lay.total);
}

static int item_y(const settings_item *it) { return band_top() + it->y - s_lay.scroll; }

// Only items that fit the band whole are drawn: text and rects have no clip
// here, so a half-visible row would spill over the account card or the hints.
static bool item_on_screen(const settings_item *it) {
    const int y = item_y(it);
    return y >= band_top() && y + it->h <= band_bottom();
}

static int row_y(int row) {
    const int k = settings_item_of_row(s_lay.items, s_lay.n, row);
    return k < 0 ? band_top() : item_y(&s_lay.items[k]);
}

// Triangle (spine gate): the highlighted row turns over like a poster's
// quick-peek (xmb/ui_peek.cpp), its back the row's description.
void settings_open_help_peek(void) {
    const setting_row *r = row_at(g_settings_sel);
    if (!r) return;
    layout_update();
    const int list_x = ((int)display_width - XMB_LIST_W) / 2;
    peek_open_text(tr(r->label), tr(r->help), list_x, row_y(g_settings_sel), XMB_LIST_W, SET_ROW_H);
}

// Centered confirm dialog rect.
static void settings_confirm_rect(int *x, int *y, int *w, int *h) {
    *w = UIS_W(520); *h = UIS_H(118);
    *x = ((int)display_width - *w) / 2;
    *y = XMB_CONTENT_Y + UIS_H(100);
}

// 1px hairline outline around a rect.
static void hairline_frame(int x, int y, int w, int h) {
    drawRect((u32)x, (u32)y, (u32)w, 1, XMB_HAIRLINE);
    drawRect((u32)x, (u32)(y + h - 1), (u32)w, 1, XMB_HAIRLINE);
    drawRect((u32)x, (u32)y, 1, (u32)h, XMB_HAIRLINE);
    drawRect((u32)(x + w - 1), (u32)y, 1, (u32)h, XMB_HAIRLINE);
}

// Filled circle, scanline by scanline (small radii only).
static void fill_circle(int cx, int cy, int r, u32 color) {
    for (int dy = -r; dy <= r; dy++) {
        int hw = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        drawRect((u32)(cx - hw), (u32)(cy + dy), (u32)(2 * hw + 1), 1, color);
    }
}

// A section header's label, upper case with a little air between letters.
// Whole UTF-8 characters: a byte at a time would split a kana or an accent.
static int utf8_len(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

static void header_label(const char *src, char *out, int cap) {
    int n = 0;
    for (int i = 0; src[i]; ) {
        const int k = utf8_len((unsigned char)src[i]);
        if (n + k > cap - 1) break;
        for (int j = 0; j < k && src[i + j]; j++, i++)
            out[n++] = (k == 1 && src[i] >= 'a' && src[i] <= 'z') ? (char)(src[i] - 32) : src[i];
    }
    out[n] = '\0';
}

#define HEADER_PX      UIS_TF(13)
#define HEADER_TRACK   UIS_W(2)       // letter spacing

static int header_label_width(const char *label) {
    int w = 0;
    for (int i = 0; label[i]; ) {
        const int k = utf8_len((unsigned char)label[i]);
        char one[5] = { 0 };
        for (int j = 0; j < k && label[i]; j++) one[j] = label[i++];
        w += ttf_text_width(one, HEADER_PX) + HEADER_TRACK;
    }
    return w;
}

// CPU phase: account card, section rules, row highlight, confirm dialog panel.
void xmb_cpu_draw_settings(void) {
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;

    if (g_settings_confirm) {
        int mx, my, mw, mh;
        settings_confirm_rect(&mx, &my, &mw, &mh);
        drawRect((u32)mx, (u32)my, (u32)mw, (u32)mh, XMB_PANEL);
        hairline_frame(mx, my, mw, mh);
        return;
    }

    layout_update();

    // Account card with avatar disc.
    int py = settings_panel_y();
    drawRect((u32)list_x, (u32)py, (u32)XMB_LIST_W, SET_PANEL_H, XMB_PANEL);
    hairline_frame(list_x, py, XMB_LIST_W, SET_PANEL_H);
    fill_circle(list_x + UIS_W(46), py + SET_PANEL_H / 2, UIS_H(22), XMB_ACCENT_DEEP);

    for (int k = 0; k < s_lay.n; k++) {
        const settings_item *it = &s_lay.items[k];
        if (!item_on_screen(it)) continue;
        const int iy = item_y(it);
        if (it->is_header) {
            // A 1px rule from the end of the label to the list's right edge.
            char lab[48];
            header_label(tr(settings_section_label((setting_section)it->index)), lab, sizeof lab);
            const int x0 = list_x + UIS_W(20) + header_label_width(lab) + UIS_W(12);
            const int x1 = list_x + XMB_LIST_W;
            if (x1 > x0)
                drawRect((u32)x0, (u32)(iy + it->h - UIS_H(14)), (u32)(x1 - x0), 1, XMB_HAIRLINE);
        } else if (it->index == g_settings_sel) {
            drawRect((u32)list_x, (u32)iy, (u32)XMB_LIST_W, SET_ROW_H, XMB_PANEL_HI);
            drawRect((u32)(list_x - UIS_W(4)), (u32)iy, UIS_W(3), SET_ROW_H, XMB_ACCENT);
        }
    }

    if (g_settings_help) {
        int hx, hy, hw, hh;
        help_rect(&hx, &hy, &hw, &hh);
        drawRect((u32)hx, (u32)hy, (u32)hw, (u32)hh, XMB_PANEL_HI);
        hairline_frame(hx, hy, hw, hh);
        drawRect((u32)hx, (u32)hy, UIS_W(3), (u32)hh, XMB_ACCENT);
    }
}

// RSX phase: account text, section labels, rows, confirm prompt.
void xmb_draw_settings(void) {
    int W      = (int)display_width;
    int list_x = (W - XMB_LIST_W) / 2;

    if (g_settings_confirm) {
        int mx, my, mw, mh;
        settings_confirm_rect(&mx, &my, &mw, &mh);
        const char *q = TR("Log out of this account?");
        int qw = ttf_text_width(q, UIS_TF(21), true);
        drawTTF((u32)(mx + (mw - qw) / 2), (u32)(my + UIS_H(28)), q, UIS_TF(21), XMB_TEXT, true);
        const char *s = TR("You'll need to sign in again to browse your library.");
        int sw = ttf_text_width(s, UIS_TF(14));
        drawTTF((u32)(mx + (mw - sw) / 2), (u32)(my + UIS_H(66)), s, UIS_TF(14), XMB_TEXT_DIM);
        return;
    }

    layout_update();

    int py = settings_panel_y();
    int tx = list_x + UIS_W(84);

    // Avatar initial.
    {
        char ini[2] = { ' ', '\0' };
        if (g_username[0]) {
            ini[0] = g_username[0];
            if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] -= 32;
        }
        int iw = ttf_text_width(ini, UIS_TF(22), true);
        drawTTF((u32)(list_x + UIS_W(46) - iw / 2), (u32)(py + SET_PANEL_H / 2 - UIS_H(12)),
                ini, UIS_TF(22), XMB_WHITE, true);
    }

    // Identity.
    drawTTF((u32)tx, (u32)(py + UIS_H(14)), TR("Account"), UIS_TF(13), XMB_TEXT_FAINT);
    char line[320];
    snprintf(line, sizeof(line), "%s", g_username[0] ? g_username : TR("(unknown)"));
    drawTTF((u32)tx, (u32)(py + UIS_H(34)), line, UIS_TF(21), XMB_TEXT, true);
    snprintf(line, sizeof(line), "%s", g_server[0] ? g_server : TR("(no server)"));
    drawTTF((u32)tx, (u32)(py + UIS_H(64)), line, UIS_TF(14), XMB_TEXT_DIM);

    // Headers and rows.  Only what is fully inside the band is drawn; the
    // input handler owns the whole selection range.
    int last_bottom = band_top();
    for (int k = 0; k < s_lay.n; k++) {
        const settings_item *it = &s_lay.items[k];
        if (!item_on_screen(it)) continue;
        const int iy = item_y(it);
        last_bottom = iy + it->h;

        if (it->is_header) {
            char lab[48];
            header_label(tr(settings_section_label((setting_section)it->index)), lab, sizeof lab);
            int x = list_x + UIS_W(20);
            const int ty = iy + it->h - UIS_H(14) - (int)HEADER_PX / 2;
            for (int i = 0; lab[i]; ) {
                const int k = utf8_len((unsigned char)lab[i]);
                char one[5] = { 0 };
                for (int j = 0; j < k && lab[i]; j++) one[j] = lab[i++];
                drawTTF((u32)x, (u32)ty, one, HEADER_PX, XMB_TEXT_FAINT);
                x += ttf_text_width(one, HEADER_PX) + HEADER_TRACK;
            }
            continue;
        }

        const setting_row *r = row_at(it->index);
        const UiRow *u = ui_of(r->id);
        const bool sel = (it->index == g_settings_sel);
        const u32  clr = sel ? XMB_WHITE : XMB_TEXT_DIM;
        drawIcon((u32)(list_x + UIS_W(20)), (u32)(iy + (SET_ROW_H - UIS_H(20)) / 2),
                 u ? u->icon : ICON_TV, UIS_TF(20.0f), clr);
        drawTTF((u32)(list_x + UIS_W(52)), (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                tr(r->label), UIS_TF(18), clr, sel);
        if (u && u->value) {
            char val[48];
            bool lit = false;
            u->value(val, sizeof val, &lit);
            const int vw = ttf_text_width(val, UIS_TF(18), sel);
            drawTTF((u32)(list_x + XMB_LIST_W - UIS_W(24) - vw),
                    (u32)(iy + (SET_ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18), lit ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
        }
    }

    // Triangle's description panel, over the list.
    const setting_row *cur = row_at(g_settings_sel);
    if (g_settings_help && cur) {
        int hx, hy, hw, hh;
        help_rect(&hx, &hy, &hw, &hh);
        drawTTF((u32)(hx + UIS_W(24)), (u32)(hy + UIS_H(12)),
                tr(cur->label), UIS_TF(17), XMB_ACCENT, true);
        char hl[160];
        const char *t = tr(cur->help);
        for (int ln = 0; ln < 2 && t && *t; ln++) {
            const char *nl = strchr(t, '\n');
            int n = nl ? (int)(nl - t) : (int)strlen(t);
            if (n > (int)sizeof(hl) - 1) n = (int)sizeof(hl) - 1;
            memcpy(hl, t, (size_t)n); hl[n] = '\0';
            drawTTF((u32)(hx + UIS_W(24)), (u32)(hy + UIS_H(42) + ln * UIS_H(24)),
                    hl, UIS_TF(15), XMB_TEXT, false);
            t = nl ? nl + 1 : NULL;
        }
        return;      // the panel sits where the footer would
    }

    // Version footer, when the list is scrolled to its end with room to spare.
    {
        int footer_y = (int)display_height - XMB_BOTTOM_PAD - UIS_H(26);
        if (footer_y > last_bottom + 6) {
            char ver[96];
            snprintf(ver, sizeof ver, TR("Jellyfin for PS3 %s \xC2\xB7 built %s"), APP_VERSION, __DATE__);
            int vw = ttf_text_width(ver, UIS_TF(13));
            drawTTF((u32)((W - vw) / 2), (u32)footer_y, ver, UIS_TF(13), XMB_TEXT_FAINT);
        }
    }
}

// -------------------------------------------------------
// Overscan calibration screen (Settings > Screen Size)
// -------------------------------------------------------
// A full-screen takeover: a light "safe-area" field inset by the current
// overscan, with a bright border and corner L-brackets the user aligns to the
// visible edges of their CRT (d-pad left/right adjusts).  Because the field is
// drawn at exactly the inset that the whole UI will use, matching the corners
// to the screen edges guarantees nothing else clips.

// DELIBERATELY NOT THEMED.  The overscan calibration screen has to stay
// legible under ANY theme -- including one a user writes badly -- because it is
// what you use to fix a screen you cannot read.  Fixed light field, dark ink.
#define OVL_FIELD   0x00C8CCDAUL   // light safe-area field
#define OVL_SURND   0x00101018UL   // near-black surround (over the wave)
#define OVL_INK     0x00202634UL   // dark title text on the field
#define OVL_INK_DIM 0x003A4256UL   // dark secondary text

void xmb_overscan_calib_cpu(void) {
    int W = (int)display_width, H = (int)display_height;
    int ox = overscan_x(), oy = overscan_y();
    if (W - 2 * ox < 40 || H - 2 * oy < 40) { ox = 0; oy = 0; }

    // Flat surround covers the animated wave so the frame edges read cleanly.
    drawRect(0, 0, (u32)W, (u32)H, OVL_SURND);
    // The light field = the area that survives the inset everywhere else.
    drawRect((u32)ox, (u32)oy, (u32)(W - 2 * ox), (u32)(H - 2 * oy), OVL_FIELD);

    // Thin accent border at the inset edge.
    const int t = UIS_H(4);
    drawRect((u32)ox, (u32)oy,            (u32)(W - 2 * ox), (u32)t, XMB_ACCENT);
    drawRect((u32)ox, (u32)(H - oy - t),  (u32)(W - 2 * ox), (u32)t, XMB_ACCENT);
    drawRect((u32)ox, (u32)oy, (u32)t, (u32)(H - 2 * oy), XMB_ACCENT);
    drawRect((u32)(W - ox - t), (u32)oy, (u32)t, (u32)(H - 2 * oy), XMB_ACCENT);

    // Bolder corner L-brackets — the primary alignment guide.
    const int arm = (W < 1000 ? 40 : 64), ct = 7;
    // top-left
    drawRect((u32)ox, (u32)oy, (u32)arm, (u32)ct, XMB_ACCENT);
    drawRect((u32)ox, (u32)oy, (u32)ct, (u32)arm, XMB_ACCENT);
    // top-right
    drawRect((u32)(W - ox - arm), (u32)oy, (u32)arm, (u32)ct, XMB_ACCENT);
    drawRect((u32)(W - ox - ct),  (u32)oy, (u32)ct,  (u32)arm, XMB_ACCENT);
    // bottom-left
    drawRect((u32)ox, (u32)(H - oy - ct),  (u32)arm, (u32)ct, XMB_ACCENT);
    drawRect((u32)ox, (u32)(H - oy - arm), (u32)ct,  (u32)arm, XMB_ACCENT);
    // bottom-right
    drawRect((u32)(W - ox - arm), (u32)(H - oy - ct),  (u32)arm, (u32)ct, XMB_ACCENT);
    drawRect((u32)(W - ox - ct),  (u32)(H - oy - arm), (u32)ct,  (u32)arm, XMB_ACCENT);
}

void xmb_overscan_calib_text(void) {
    int W = (int)display_width, H = (int)display_height;
    int cy = H / 2;

    const char *title = TR("Screen Size");
    int tw = ttf_text_width(title, UIS_TF(26), true);
    drawTTF((u32)((W - tw) / 2), (u32)(cy - UIS_H(78)), title, UIS_TF(26), OVL_INK, true);

    const char *l1 = TR("Match the corners to the edges of your screen");
    int l1w = ttf_text_width(l1, UIS_TF(16));
    drawTTF((u32)((W - l1w) / 2), (u32)(cy - UIS_H(34)), l1, UIS_TF(16), OVL_INK_DIM);

    int pm = (int)(overscan_frac() * 1000.0f + 0.5f);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d.%d%%", pm / 10, pm % 10);
    int pw = ttf_text_width(pct, UIS_TF(30), true);
    drawTTF((u32)((W - pw) / 2), (u32)(cy - UIS_H(2)), pct, UIS_TF(30), XMB_ACCENT_DEEP, true);

    const char *hint = TR("D-pad Left / Right to adjust");
    int hw = ttf_text_width(hint, UIS_TF(15));
    drawTTF((u32)((W - hw) / 2), (u32)(cy + UIS_H(44)), hint, UIS_TF(15), OVL_INK_DIM);

    const char *keys = TR("Cross  Save        Circle  Cancel");
    int kw = ttf_text_width(keys, UIS_TF(15));
    drawTTF((u32)((W - kw) / 2), (u32)(cy + UIS_H(70)), keys, UIS_TF(15), OVL_INK_DIM);
}
