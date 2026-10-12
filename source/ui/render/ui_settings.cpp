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
#include "rsxutil.h"        // rsxSync, flip: finish the frame before the keyboard
extern void crash_log(const char *msg);   // synchronous: survives a freeze

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
// The saved MAC, read once: the row values are drawn every frame.
static bool    s_mac_read = false, s_mac_have = false;
static uint8_t s_mac[6];
static bool wol_have(uint8_t mac[6]) {
    if (!s_mac_read) { s_mac_read = true; s_mac_have = wol_saved(s_mac); }
    if (s_mac_have) memcpy(mac, s_mac, 6);
    return s_mac_have;
}
static void v_wolmac(char *b, int c, bool *l) {
    uint8_t mac[6];
    if (wol_have(mac)) { wol_format_mac(mac, b, c); *l = true; }
    else               val_text(b, c, l, TR("Not set"), false);
}
static void v_wake(char *b, int c, bool *l) {
    uint8_t mac[6];
    if (!wol_have(mac))                                        val_text(b, c, l, TR("Set the MAC first"), false);
    else if (s_wake_sent_us && timing_get_us() - s_wake_sent_us < 5000000ULL) val_text(b, c, l, TR("Sent"), true);
    else                                                       val_text(b, c, l, TR("X to send"), false);
}
static void a_wolmac(void) {
    char text[40] = "";
    uint8_t mac[6];
    if (wol_have(mac)) wol_format_mac(mac, text, sizeof text);
    // Menu input runs after this frame has already queued the wave's drawing, and
    // the keyboard draws the wave again.  Finish the frame first, as every other
    // popup over the XMB does (xmb_dl_confirm, show_details): the keyboard opened
    // from here froze the console otherwise.
    crash_log("wol: keyboard");
    rsxSync();
    flip();
    const int r = get_input(text, sizeof text, TR("Server MAC address (like 1C:69:7A:12:34:56)"), false, true);
    crash_log("wol: keyboard done");
    init_btns();
    if (r != 1 || !text[0]) return;
    if (!wol_parse_mac(text, mac)) {
        xmb_dl_notice(TR("Wake-on-LAN"), TR("That is not a MAC address: six pairs of digits, like 1C:69:7A:12:34:56."));
        return;
    }
    if (wol_save(mac)) { memcpy(s_mac, mac, 6); s_mac_have = true; s_mac_read = true; }
}
static void a_wake(void) {
    uint8_t mac[6];
    if (!wol_have(mac)) {
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
//  The screen
// -------------------------------------------------------------------------
//  Two levels, like the PS3's own Settings column.  The first lists the
//  categories, each an icon tile of its own with its name and what is inside;
//  X opens one and shows just its settings, with what the highlighted one does
//  written underneath, and O goes back.  g_settings_cat is -1 on the first
//  level, else the open category (a setting_section).

// One icon per category, none shared (the glyphs are in the icon font subset).
static int cat_icon(setting_section s) {
    switch (s) {
    case SEC_PLAYBACK:  return ICON_PLAY;
    case SEC_AUDIO:     return ICON_VOLUME;
    case SEC_SUBTITLES: return ICON_CAPTIONS;
    case SEC_DOWNLOADS: return ICON_DOWNLOAD;
    case SEC_DISPLAY:   return ICON_DESKTOP;
    default:            return ICON_SETTINGS;
    }
}

// Sized so that all six categories fit at once below the spine's header, and a
// category shows five of its settings above the description.
#define CAT_ROW_H  UIS_H(56)
#define CAT_TILE   UIS_H(40)
#define ROW_H      UIS_H(46)
#define ITEM_GAP   UIS_H(6)
#define HEAD_H     UIS_H(60)
#define DESC_H     UIS_H(60)

static int list_x(void)      { return ((int)display_width - XMB_LIST_W) / 2; }
static int top_y(void)       { return XMB_CONTENT_Y + UIS_H(12); }
static int bottom_y(void)    { return (int)display_height - XMB_BOTTOM_PAD; }
static int rows_top(void)    { return top_y() + HEAD_H; }
static int rows_bottom(void) { return bottom_y() - DESC_H - UIS_H(8); }

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

// `src` cut on a character boundary, with "..." when it does not fit `max_w`.
static void fit_text(const char *src, float px, int max_w, bool bold, char *out, int cap) {
    snprintf(out, (size_t)cap, "%s", src);
    if (ttf_text_width(out, px, bold) <= max_w) return;
    int len = (int)strlen(out);
    while (len > 1) {
        do { len--; } while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
        char t[200];
        snprintf(t, sizeof t, "%.*s...", len, out);
        if (ttf_text_width(t, px, bold) <= max_w) { snprintf(out, (size_t)cap, "%s", t); return; }
    }
}

// What is inside a category, as one line: its settings' names.
static void cat_summary(setting_section s, char *out, int cap) {
    out[0] = '\0';
    const int n = settings_category_row_count(s);
    int used = 0;
    for (int k = 0; k < n; k++) {
        const char *l = tr(row_at(settings_category_row(s, k))->label);
        const int w = snprintf(out + used, (size_t)(cap - used), "%s%s", k ? "  \xC2\xB7  " : "", l);
        if (w < 0 || w >= cap - used) { out[used] = '\0'; break; }
        used += w;
    }
}

static int s_scroll_a = 0, s_scroll_b = 0;     // the category list, and the rows inside one
static int s_last_cat = -2;

// Keeps the selection on screen.  Called by both draw phases; the second finds
// nothing left to move.
static void scroll_update(void) {
    if (g_settings_cat != s_last_cat) { s_last_cat = g_settings_cat; s_scroll_b = 0; }
    if (g_settings_cat < 0) {
        s_scroll_a = settings_scroll_to(settings_category_count(), g_settings_catsel,
                                        CAT_ROW_H, ITEM_GAP, bottom_y() - top_y(), s_scroll_a);
    } else {
        const setting_section sec = (setting_section)g_settings_cat;
        s_scroll_b = settings_scroll_to(settings_category_row_count(sec),
                                        settings_row_in_category(g_settings_sel),
                                        ROW_H, ITEM_GAP, rows_bottom() - rows_top(), s_scroll_b);
    }
}

static int cat_y(int i)  { return top_y() + i * (CAT_ROW_H + ITEM_GAP) - s_scroll_a; }
static int row_y(int k)  { return rows_top() + k * (ROW_H + ITEM_GAP) - s_scroll_b; }
static bool cat_visible(int i) { const int y = cat_y(i); return y >= top_y() && y + CAT_ROW_H <= bottom_y(); }
static bool row_visible(int k) { const int y = row_y(k); return y >= rows_top() && y + ROW_H <= rows_bottom(); }

// CPU phase: panels, highlights, icon tiles, the confirm dialog's panel.
void xmb_cpu_draw_settings(void) {
    const int lx = list_x();

    if (g_settings_confirm) {
        int mx, my, mw, mh;
        settings_confirm_rect(&mx, &my, &mw, &mh);
        drawRect((u32)mx, (u32)my, (u32)mw, (u32)mh, XMB_PANEL);
        hairline_frame(mx, my, mw, mh);
        return;
    }
    scroll_update();

    if (g_settings_cat < 0) {
        const int n = settings_category_count();
        for (int i = 0; i < n; i++) {
            if (!cat_visible(i)) continue;
            const int y = cat_y(i);
            const bool sel = (i == g_settings_catsel);
            drawRect((u32)lx, (u32)y, (u32)XMB_LIST_W, (u32)CAT_ROW_H, sel ? XMB_PANEL_HI : XMB_PANEL);
            if (sel) drawRect((u32)(lx - UIS_W(4)), (u32)y, UIS_W(3), (u32)CAT_ROW_H, XMB_ACCENT);
            drawRect((u32)(lx + UIS_W(14)), (u32)(y + (CAT_ROW_H - CAT_TILE) / 2), (u32)CAT_TILE, (u32)CAT_TILE,
                     sel ? XMB_ACCENT_DEEP : XMB_THUMB_DIM);
        }
        return;
    }

    // Inside a category: the header tile, the rows, the description.
    drawRect((u32)(lx + UIS_W(14)), (u32)(top_y() + (HEAD_H - CAT_TILE) / 2 - UIS_H(4)), (u32)CAT_TILE, (u32)CAT_TILE,
             XMB_ACCENT_DEEP);
    drawRect((u32)lx, (u32)(top_y() + HEAD_H - UIS_H(8)), (u32)XMB_LIST_W, 1, XMB_HAIRLINE);
    const setting_section sec = (setting_section)g_settings_cat;
    const int n = settings_category_row_count(sec);
    const int cur = settings_row_in_category(g_settings_sel);
    for (int k = 0; k < n; k++) {
        if (!row_visible(k) || k != cur) continue;
        const int y = row_y(k);
        drawRect((u32)lx, (u32)y, (u32)XMB_LIST_W, (u32)ROW_H, XMB_PANEL_HI);
        drawRect((u32)(lx - UIS_W(4)), (u32)y, UIS_W(3), (u32)ROW_H, XMB_ACCENT);
    }
    const int dy = rows_bottom() + UIS_H(8);
    drawRect((u32)lx, (u32)dy, (u32)XMB_LIST_W, (u32)DESC_H, XMB_PANEL);
    hairline_frame(lx, dy, XMB_LIST_W, DESC_H);
    drawRect((u32)lx, (u32)dy, UIS_W(3), (u32)DESC_H, XMB_ACCENT);
}

// RSX phase: icons and text.
void xmb_draw_settings(void) {
    const int W  = (int)display_width;
    const int lx = list_x();

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
    scroll_update();

    const float icon_px = UIS_TF(24.0f);
    const int   tile_x  = lx + UIS_W(14);

    if (g_settings_cat < 0) {
        // Who is signed in, above the list.
        char who[300], fit[300];
        snprintf(who, sizeof who, "%s: %s  \xC2\xB7  %s", TR("Account"),
                 g_username[0] ? g_username : TR("(unknown)"), g_server[0] ? g_server : TR("(no server)"));
        fit_text(who, UIS_TF(13), XMB_LIST_W, false, fit, sizeof fit);
        drawTTF((u32)lx, (u32)(top_y() - UIS_H(20)), fit, UIS_TF(13), XMB_TEXT_FAINT);

        const int n = settings_category_count();
        int last_bottom = top_y();
        for (int i = 0; i < n; i++) {
            if (!cat_visible(i)) continue;
            const int y = cat_y(i);
            last_bottom = y + CAT_ROW_H;
            const setting_section sec = settings_category_at(i);
            const bool sel = (i == g_settings_catsel);
            const int ty = y + (CAT_ROW_H - CAT_TILE) / 2;
            drawIcon((u32)(tile_x + (CAT_TILE - (int)icon_px) / 2), (u32)(ty + (CAT_TILE - (int)icon_px) / 2),
                     cat_icon(sec), icon_px, sel ? XMB_WHITE : XMB_TEXT_DIM);
            const int tx = tile_x + CAT_TILE + UIS_W(18);
            const int text_w = XMB_LIST_W - (tx - lx) - UIS_W(56);
            char lab[96];
            fit_text(tr(settings_section_label(sec)), UIS_TF(20), text_w, sel, lab, sizeof lab);
            drawTTF((u32)tx, (u32)(y + UIS_H(7)), lab, UIS_TF(20), sel ? XMB_WHITE : XMB_TEXT, sel);
            char sum[300], sumf[300];
            cat_summary(sec, sum, sizeof sum);
            fit_text(sum, UIS_TF(13), text_w, false, sumf, sizeof sumf);
            drawTTF((u32)tx, (u32)(y + UIS_H(32)), sumf, UIS_TF(13), XMB_TEXT_FAINT);
            drawIcon((u32)(lx + XMB_LIST_W - UIS_W(34)), (u32)(y + (CAT_ROW_H - UIS_H(20)) / 2),
                     ICON_CHEVRON_RIGHT, UIS_TF(20.0f), sel ? XMB_ACCENT : XMB_TEXT_FAINT);
        }
        // Version footer, when the list leaves room for it.
        const int footer_y = bottom_y() - UIS_H(26);
        if (footer_y > last_bottom + 6) {
            char ver[96];
            snprintf(ver, sizeof ver, TR("Jellyfin for PS3 %s \xC2\xB7 built %s"), APP_VERSION, __DATE__);
            const int vw = ttf_text_width(ver, UIS_TF(13));
            drawTTF((u32)((W - vw) / 2), (u32)footer_y, ver, UIS_TF(13), XMB_TEXT_FAINT);
        }
        return;
    }

    // Inside a category.
    const setting_section sec = (setting_section)g_settings_cat;
    {
        const int ty = top_y() + (HEAD_H - CAT_TILE) / 2 - UIS_H(4);
        drawIcon((u32)(tile_x + (CAT_TILE - (int)icon_px) / 2), (u32)(ty + (CAT_TILE - (int)icon_px) / 2),
                 cat_icon(sec), icon_px, XMB_WHITE);
        const char *title = tr(settings_section_label(sec));
        drawTTF((u32)(tile_x + CAT_TILE + UIS_W(18)), (u32)(ty + (CAT_TILE - UIS_H(24)) / 2 - UIS_H(2)),
                title, UIS_TF(24), XMB_WHITE, true);
    }
    const int n = settings_category_row_count(sec);
    const int cur = settings_row_in_category(g_settings_sel);
    for (int k = 0; k < n; k++) {
        if (!row_visible(k)) continue;
        const int y = row_y(k);
        const setting_row *r = row_at(settings_category_row(sec, k));
        const UiRow *u = ui_of(r->id);
        const bool sel = (k == cur);
        const u32  clr = sel ? XMB_WHITE : XMB_TEXT_DIM;
        drawIcon((u32)(lx + UIS_W(20)), (u32)(y + (ROW_H - UIS_H(20)) / 2),
                 u ? u->icon : ICON_TV, UIS_TF(20.0f), clr);
        int vw = 0;
        char val[48];
        bool lit = false;
        val[0] = '\0';
        if (u && u->value) {
            u->value(val, sizeof val, &lit);
            vw = ttf_text_width(val, UIS_TF(18), sel);
        }
        char lab[96];
        fit_text(tr(r->label), UIS_TF(18), XMB_LIST_W - UIS_W(52) - vw - UIS_W(56), sel, lab, sizeof lab);
        drawTTF((u32)(lx + UIS_W(52)), (u32)(y + (ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                lab, UIS_TF(18), clr, sel);
        if (val[0])
            drawTTF((u32)(lx + XMB_LIST_W - UIS_W(24) - vw), (u32)(y + (ROW_H - UIS_H(18)) / 2 - UIS_H(2)),
                    val, UIS_TF(18), lit ? XMB_ACCENT : XMB_TEXT_FAINT, sel);
    }

    // What the highlighted setting does.
    const setting_row *cr = row_at(g_settings_sel);
    if (cr) {
        const int dy = rows_bottom() + UIS_H(8);
        const char *t = tr(cr->help);
        for (int ln = 0; ln < 2 && t && *t; ln++) {
            const char *nl = strchr(t, '\n');
            int len = nl ? (int)(nl - t) : (int)strlen(t);
            char raw[200], hl[200];
            if (len > (int)sizeof raw - 1) len = (int)sizeof raw - 1;
            memcpy(raw, t, (size_t)len); raw[len] = '\0';
            fit_text(raw, UIS_TF(15), XMB_LIST_W - UIS_W(44), false, hl, sizeof hl);
            drawTTF((u32)(lx + UIS_W(22)), (u32)(dy + UIS_H(8) + ln * UIS_H(24)), hl, UIS_TF(15),
                    ln == 0 ? XMB_TEXT : XMB_TEXT_DIM, false);
            t = nl ? nl + 1 : NULL;
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
