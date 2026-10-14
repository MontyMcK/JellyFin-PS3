// Stage 5 preview: the offline-download screens, rendered on the host.
//
// Reuses preview.c's primitives (palette, wave background, TTF, PS button
// sprites) and -- unlike the mock screens there -- takes every label, status
// line, progress value and hint from the REAL model, source/offline/dl_ui.cpp,
// so what these frames say is what the console will say.  Layout constants
// mirror source/ui/xmb/ui_downloads.cpp and the info page's button row.
//
//   g++ -O2 -w -fpermissive -I. -I../../source/offline preview_offline.cpp \
//       ../../source/offline/dl_ui.cpp ../../source/offline/dl_model.cpp \
//       -lm -o preview_offline && ./preview_offline
//   (run from this directory: the fonts are loaded by relative path)

#define main preview_legacy_main
#include "preview.c"
#undef main

#include "dl_ui.h"

#define DL_WARN_CLR 0x00E8B64CUL

// ---- mirrors ui_downloads.cpp --------------------------------------------
static int list_top(void)    { return 112; }
static int list_bottom(void) { return (int)display_height - XMB_BOTTOM_PAD; }
static int row_h(void)       { return 72; }
static int row_pitch(void)   { return row_h() + 10; }
static int rows_visible(void) {
    int n = (list_bottom() - list_top()) / row_pitch();
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

static void clip_text(int x, int y, const char *text, float px, u32 color,
                      int max_w, int bold) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", text);
    int len = (int)strlen(buf);
    while (len > 3 && ttf_text_width(buf, px) > max_w) {
        buf[--len] = '\0';
        if (len > 3) { buf[len - 1] = '.'; buf[len - 2] = '.'; buf[len - 3] = '.'; }
    }
    drawTTF(x, y, buf, px, color, bold);
}

static void draw_title(const char *title, const char *banner, uint32_t banner_clr) {
    drawTTF(XMB_ITEM_PAD, 28, title, 28, XMB_TEXT, 1);
    if (banner && banner[0]) drawTTF(XMB_ITEM_PAD, 70, banner, 15, banner_clr, 0);
}

static void draw_row_frame(int x, int y, int w, int h, bool sel) {
    drawRect(x, y, w, h, sel ? XMB_PANEL_HI : XMB_PANEL);
    if (sel) drawRect(x - 4, y, 3, h, XMB_ACCENT);
}

static void draw_bar(int x, int y, int w, int pm) {
    drawRect(x, y, w, 4, XMB_HAIRLINE);
    int fw = (int)((long long)w * pm / 1000);
    if (fw > 0) drawRect(x, y, fw, 4, XMB_ACCENT);
}

static DlStatus st(const char *title, DlState s, uint64_t done, uint64_t total,
                   uint32_t retry = 0, DlError e = DL_ERR_NONE) {
    DlStatus x;
    memset(&x, 0, sizeof(x));
    dl_record_init(&x.rec);
    snprintf(x.rec.id, sizeof(x.rec.id), "id%s", title);
    snprintf(x.rec.title, sizeof(x.rec.title), "%s", title);
    snprintf(x.rec.url, sizeof(x.rec.url), "http://h/x");
    x.rec.state = s; x.rec.bytes_done = done; x.rec.bytes_total = total;
    x.retry_in_ms = retry; x.rec.error = e;
    x.active = s == DL_DOWNLOADING;
    return x;
}

static const uint64_t MB = 1024ull * 1024, GB = MB * 1024;

static void downloads(const char *out, DlUiContext cx, int sel) {
    DlStatus rows[] = {
        st("Interstellar", DL_DOWNLOADING, 1288 * MB, 3050 * MB),
        st("The Expanse  S1 E3", DL_QUEUED, 0, 0),
        st("Arrival", DL_QUEUED, 0, 0, 7200, DL_ERR_UNREACHABLE),
        st("Blade Runner 2049", DL_PAUSED, 2 * GB, 4 * GB),
        st("Dune: Part Two", DL_FAILED, 0, 0, 0, DL_ERR_NO_SPACE),
        st("Heat", DL_COMPLETED, (uint64_t)(3.4 * GB), (uint64_t)(3.4 * GB)),
    };
    const int n = (int)(sizeof(rows) / sizeof(rows[0]));
    draw_background();
    draw_title("Downloads", dl_ui_queue_banner(&cx), DL_WARN_CLR);
    const int x = XMB_ITEM_PAD, w = (int)display_width - 2 * XMB_ITEM_PAD;
    for (int i = 0; i < rows_visible() && i < n; i++) {
        DlUiRow r;
        dl_ui_row(&rows[i], &cx, &r);
        const bool s = i == sel;
        const int y = list_top() + i * row_pitch();
        draw_row_frame(x, y, w, row_h(), s);
        clip_text(x + 20, y + 10, r.title, 19, s ? XMB_WHITE : XMB_TEXT, w - 280, s);
        const u32 sc = r.warning ? DL_WARN_CLR : r.emphasis ? XMB_ACCENT : XMB_TEXT_DIM;
        clip_text(x + 20, y + 38, r.status, 14, sc, w - 280, 0);
        if (r.size[0]) {
            int sw = ttf_text_width(r.size, 14);
            drawTTF(x + w - 20 - sw, y + 38, r.size, 14, XMB_TEXT_DIM, 0);
        }
        if (r.permille >= 0) draw_bar(x + 20, y + row_h() - 12, w - 40, r.permille);
    }
    Hint h[3]; int nh = 0;
    h[nh].glyph = 'C'; h[nh].label = "Back"; nh++;
    const char *q = dl_ui_action_label(dl_ui_row_secondary(&rows[sel]));
    const char *p = dl_ui_action_label(dl_ui_row_primary(&rows[sel]));
    if (q[0]) { h[nh].glyph = 'S'; h[nh].label = q; nh++; }
    if (p[0]) { h[nh].glyph = 'X'; h[nh].label = p; nh++; }
    draw_hints_bar(h, nh);
    save_ppm(out);
}

static void offline(const char *out, bool empty) {
    draw_background();
    draw_title("Offline", empty ? "" : "Plays from the HDD -- no server needed", XMB_TEXT_DIM);
    const int x = XMB_ITEM_PAD, w = (int)display_width - 2 * XMB_ITEM_PAD;
    if (empty) {
        int y = list_top() + 40;
        drawTTF(XMB_ITEM_PAD, y, "Nothing downloaded yet.", 20, XMB_TEXT_DIM, 0);
        drawTTF(XMB_ITEM_PAD, y + 32, "Open a film or an episode and choose Download.", 15,
                XMB_TEXT_FAINT, 0);
        Hint h[] = {{'C', "Back"}};
        draw_hints_bar(h, 1);
        save_ppm(out);
        return;
    }
    struct { const char *title, *series; int s, e, year; unsigned rt; uint64_t b; bool ok; } it[] = {
        { "Heat", "", -1, -1, 1995, 10200, (uint64_t)(3.4 * GB), true },
        { "Pilot", "The Expanse", 1, 1, 0, 2820, 1288 * MB, true },
        { "Dulcinea", "The Expanse", 1, 2, 0, 2700, 1210 * MB, true },
        { "Arrival", "", -1, -1, 2016, 6960, (uint64_t)(2.1 * GB), true },
        { "Recovered item", "", -1, -1, 0, 0, 900 * MB, false },   // stale meta.txt
    };
    for (int i = 0; i < 5 && i < rows_visible(); i++) {
        DlMeta m;
        dl_meta_init(&m);
        snprintf(m.id, sizeof(m.id), "x%d", i);
        snprintf(m.title, sizeof(m.title), "%s", it[i].title);
        snprintf(m.series, sizeof(m.series), "%s", it[i].series);
        m.season = it[i].s; m.episode = it[i].e; m.year = it[i].year; m.runtime_secs = it[i].rt;
        char t[128], sub[160];
        dl_ui_offline_lines(&m, it[i].ok, it[i].b, t, sizeof(t), sub, sizeof(sub));
        const bool s = i == 1;
        const int y = list_top() + i * row_pitch();
        draw_row_frame(x, y, w, row_h(), s);
        clip_text(x + 20, y + 12, t, 19, s ? XMB_WHITE : XMB_TEXT, w - 40, s);
        clip_text(x + 20, y + 40, sub, 14, XMB_TEXT_DIM, w - 40, 0);
    }
    Hint h[] = {{'C', "Back"}, {'S', "Delete"}, {'X', "Play"}};
    draw_hints_bar(h, 3);
    save_ppm(out);
}

// The info page's button row (mirrors ui_info.cpp): Play, then DOWNLOAD
// with its live label and progress, then the Quality row below.
static void info_row(const char *out, const DlStatus *stp, bool focus_dl, const char *toast) {
    draw_background();
    const int poster_w = 304, poster_h = 456;
    fake_poster(XMB_ITEM_PAD, 76, poster_w, poster_h, 0x003A4290, 0x00151A38, "I");
    const int tx = XMB_ITEM_PAD + poster_w + 28;
    int Y = 76;
    drawTTF(tx, Y, "Interstellar", 40, XMB_WHITE, 1);
    Y += 62;
    drawTTF(tx, Y, "2014 \xB7 2h 49m", 18, XMB_TEXT_DIM, 0);
    Y += 44;
    DlUiContext cx = { false, false, true };
    const int bw = 128, bh = 40;
    const bool pf = !focus_dl;
    drawRect(tx, Y, bw, bh, pf ? XMB_ACCENT : XMB_PANEL_HI);
    drawTTF(tx + 46, Y + (bh - 20) / 2 + 1, "Play", 20, pf ? 0x00131630UL : XMB_TEXT_DIM, 1);
    char lbl[48];
    dl_ui_item_label(stp, true, &cx, lbl, sizeof(lbl));
    const int dx = tx + bw + 16, dw = 250;
    drawRect(dx, Y, dw, bh, focus_dl ? XMB_PANEL_HI : XMB_PANEL);
    if (focus_dl) {
        drawRect(dx - 2, Y - 2, dw + 4, 2, XMB_KEY_SEL);
        drawRect(dx - 2, Y + bh, dw + 4, 2, XMB_KEY_SEL);
        drawRect(dx - 2, Y - 2, 2, bh + 4, XMB_KEY_SEL);
        drawRect(dx + dw, Y - 2, 2, bh + 4, XMB_KEY_SEL);
    }
    const int pm = stp && stp->rec.state != DL_COMPLETED ? dl_progress_permille(&stp->rec) : -1;
    if (pm >= 0) {
        drawRect(dx, Y + bh - 4, dw, 4, XMB_HAIRLINE);
        drawRect(dx, Y + bh - 4, dw * pm / 1000, 4, XMB_ACCENT);
    }
    clip_text(dx + 16, Y + (bh - 18) / 2, lbl, 18, focus_dl ? XMB_WHITE : XMB_TEXT_DIM,
              dw - 32, focus_dl);
    Y += bh + 18;
    if (toast && toast[0]) { drawTTF(tx, Y - 14, toast, 14, DL_WARN_CLR, 0); Y += 12; }
    drawRect(tx, Y, 680, 44, XMB_PANEL);
    drawTTF(tx + 16, Y + 12, "Quality", 16, XMB_TEXT_FAINT, 1);
    drawTTF(tx + 118, Y + 11, "Auto  (1280x720, 4 Mbps)", 18, XMB_TEXT, 0);
    const char *a = focus_dl ? dl_ui_action_label(dl_ui_item_action(stp, true, &cx)) : "Play";
    Hint h[] = {{'C', "Back"}, {'X', a}};
    draw_hints_bar(h, a[0] ? 2 : 1);
    save_ppm(out);
}

static void confirm(const char *out) {
    draw_background();
    const int pw = 600, ph = 236, px = (W - pw) / 2, py = (H - ph) / 2;
    drawRect(px, py, pw, ph, XMB_PANEL);
    drawRect(px, py, pw, 1, XMB_HAIRLINE); drawRect(px, py + ph - 1, pw, 1, XMB_HAIRLINE);
    drawRect(px, py, 1, ph, XMB_HAIRLINE); drawRect(px + pw - 1, py, 1, ph, XMB_HAIRLINE);
    const int cx = px + 32, mw = pw - 64;
    int y = py + 28;
    clip_text(cx, y, "Heat", 24, XMB_WHITE, mw, 1);
    y += 38;
    clip_text(cx, y, "Delete the downloaded file from the HDD?", 15, XMB_TEXT_DIM, mw, 0);
    y += 36;
    const char *o[2] = { "Keep it", "Delete" };
    for (int i = 0; i < 2; i++) {
        int oy = y + i * 52;
        if (i == 0) { drawRect(cx, oy, mw, 44, XMB_PANEL_HI); drawRect(cx - 4, oy, 3, 44, XMB_ACCENT); }
        drawTTF(cx + 16, oy + 12, o[i], 19, i == 0 ? XMB_TEXT : XMB_TEXT_DIM, 0);
    }
    Hint h[] = {{'X', "Select"}, {'C', "Back"}};
    draw_hints_bar(h, 2);
    save_ppm(out);
}

int main(void) {
    gamma_init();
    stbtt_InitFont(&s_font, load_file("../../source/gfx/fonts/OpenSans-Regular.ttf"), 0);
    stbtt_InitFont(&s_font_bold, load_file("../../source/gfx/fonts/OpenSans-Bold.ttf"), 0);
    stbtt_InitFont(&s_icons, load_file("../../source/gfx/fonts/MaterialIcons-Regular.ttf"), 0);
    ps_sprites_load();
    DlUiContext ok = { false, false, true }, streaming = { true, false, true };
    downloads("8_downloads.ppm", ok, 0);
    downloads("9_downloads_streaming.ppm", streaming, 1);
    downloads("10_downloads_failed_sel.ppm", ok, 4);
    offline("11_offline.ppm", false);
    offline("12_offline_empty.ppm", true);
    DlStatus d = st("Interstellar", DL_DOWNLOADING, 1288 * MB, 3050 * MB);
    DlStatus c = st("Interstellar", DL_COMPLETED, 3050 * MB, 3050 * MB);
    info_row("13_info_download.ppm", NULL, true, "");
    info_row("14_info_downloading.ppm", &d, true, "Added to Downloads (Settings > Downloads)");
    info_row("15_info_play_offline.ppm", &c, true, "");
    confirm("16_confirm_delete.ppm");
    printf("done\n");
    return 0;
}
