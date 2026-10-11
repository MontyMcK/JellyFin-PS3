// The redesigned player HUD -- design-import-v3 "07 · Player HUD", drawn when
// the spine gate is on.  #included by hud_draw.cpp (it uses that file's
// staging-buffer statics); not a translation unit of its own.
//
// Same mechanism as the old HUD: composed into the main-memory staging buffer
// only when its content changes (about once a second), uploaded, and drawn as
// one blended quad over the video.  Nothing here reads video memory.
//
// Geometry is the canvas's, measured at 1280x720 (MEASURED-detail-hud.md) and
// scaled with UIS_*, which the old HUD's raw pixels never were.
//
// KEPT FROM THE OLD HUD, because the canvas shows no controls for them and
// dropping them would remove features: the focusable slots.  Left/Right still
// walk REW / PLAY / FF / AUDIO / VOLUME / CC, and X still activates the focused
// one.  They are drawn in the canvas's own vocabulary -- AUDIO, VOLUME and CC
// are the chips on the right, REW / FF show their step above the bar -- and
// the X hint names whatever X will do.
//
// NOT SHOWN, because the client has no such data yet: the buffered-to mark
// and the "2014 · Chapter 14" subline.

static const int V3_INCR[3] = { 10, 30, 300 };   // mirrors hud_core.cpp

static int v3x(int x) { return overscan_x() + UIS_W(x); }
static int v3y(int y) { return overscan_y() + UIS_H(y); }

// Straight-alpha fill into the staging buffer.
static void v3_fill(int x, int y, int w, int h, u32 rgb, u8 a) {
    if (w <= 0 || h <= 0) return;
    int x2 = x + w, y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > (int)s_ovl_w) x2 = (int)s_ovl_w;
    if (y2 > (int)s_ovl_h) y2 = (int)s_ovl_h;
    const u32 px = ((u32)a << 24) | (rgb & 0x00FFFFFFu);
    for (int yy = y; yy < y2; yy++) {
        u32 *row = s_ovl_stage + (u32)yy * s_ovl_w;
        for (int xx = x; xx < x2; xx++) row[xx] = px;
    }
    ovl_span(y, y2);
}

// Coverage of pixel (px, py) by a rounded rect, 0..1, with a one-pixel soft
// edge.  Only the corner squares need the distance test.
static float v3_rrect_cov(float px, float py, float x, float y, float w, float h,
                          float r) {
    const float cx = px < x + r ? x + r : (px > x + w - r ? x + w - r : px);
    const float cy = py < y + r ? y + r : (py > y + h - r ? y + h - r : py);
    const float dx = px - cx, dy = py - cy;
    float d;
    if (dx == 0.0f && dy == 0.0f) {
        // Inside the straight part: distance to the nearest edge.
        float e = px - x;
        if (x + w - px < e) e = x + w - px;
        if (py - y < e) e = py - y;
        if (y + h - py < e) e = y + h - py;
        return e >= 0.5f ? 1.0f : (e + 0.5f > 0.0f ? e + 0.5f : 0.0f);
    }
    d = sqrtf(dx * dx + dy * dy);
    const float c = r + 0.5f - d;
    return c <= 0.0f ? 0.0f : (c >= 1.0f ? 1.0f : c);
}

// Rounded rect in the staging buffer: a fill at fill_a, then a t-pixel ring
// at line_a over its edge (either alpha may be 0 to skip that part).
static void v3_rrect(int x, int y, int w, int h, int r, int t,
                     u32 fill_rgb, u8 fill_a, u32 line_rgb, u8 line_a) {
    if (w <= 0 || h <= 0) return;
    const float fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h;
    const float fr = (float)r, ft = (float)t;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= (int)s_ovl_h) continue;
        u32 *row = s_ovl_stage + (u32)yy * s_ovl_w;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= (int)s_ovl_w) continue;
            const float pcx = (float)xx + 0.5f, pcy = (float)yy + 0.5f;
            const float outer = v3_rrect_cov(pcx, pcy, fx, fy, fw, fh, fr);
            if (outer <= 0.0f) continue;
            const float inner = t > 0
                ? v3_rrect_cov(pcx, pcy, fx + ft, fy + ft, fw - 2 * ft, fh - 2 * ft,
                               fr > ft ? fr - ft : 0.0f)
                : outer;
            const float ring = outer - inner;
            u32 rgb; float a;
            if (line_a && ring > 0.001f && (!fill_a || ring * line_a >= inner * fill_a)) {
                rgb = line_rgb; a = ring * (float)line_a + inner * (float)fill_a * (1.0f - ring);
            } else {
                rgb = fill_rgb; a = inner * (float)fill_a;
            }
            if (a < 0.5f) continue;
            row[xx] = ((u32)(a + 0.5f) << 24) | (rgb & 0x00FFFFFFu);
        }
    }
    ovl_span(y, y + h);
}

// The picture-shape glyph, bottom right: the screen as a frame, and what the
// film does inside it.  FIT shows the whole film with bars above and below;
// FILL covers the frame (the notches outside are the cropped sides); STRETCH
// pulls it to the frame's edges (arrows).  22 x 16 at (x, y).
static int v3_aspect_icon_w(void) { return UIS_W(22) > 18 ? UIS_W(22) : 18; }   // never a speck on SD

static void v3_aspect_icon(int x, int y, int mode, bool foc) {
    const int fw = v3_aspect_icon_w(), fh = fw * 16 / 22;
    const u32 col = foc ? 0x00FFFFFFu : 0x00E4E6F0u;
    v3_rrect(x, y, fw, fh, UIS_H(2), 1, 0x00000000, 0, col, 210);          // the screen
    if (mode == PIC_FILL) {
        v3_fill(x + 2, y + 2, fw - 4, fh - 4, col, 235);                    // covers it
        const int nh = UIS_H(6);
        v3_fill(x - UIS_W(3), y + (fh - nh) / 2, UIS_W(2), nh, col, 200);   // the cropped sides
        v3_fill(x + fw + UIS_W(1), y + (fh - nh) / 2, UIS_W(2), nh, col, 200);
    } else if (mode == PIC_STRETCH) {
        const int my = y + fh / 2, a = UIS_W(2);                            // arrow shaft + heads
        v3_fill(x + 4, my, fw - 8, 1, col, 235);
        for (int k = 0; k < 3; k++) {
            v3_fill(x + 3 + k * a / 2, my - k, 1, 2 * k + 1, col, 235);
            v3_fill(x + fw - 4 - k * a / 2, my - k, 1, 2 * k + 1, col, 235);
        }
    } else {
        const int ih = (fw - 4) * 9 / 16;                                   // the whole film, 16:9
        v3_fill(x + 2, y + (fh - ih) / 2, fw - 4, ih, col, 235);
    }
}

// The banner: what a channel change lands on, or the picture-size readout,
// top left.
static void v3_draw_banner(void) {
    const u32 WHITE = 0x00FFFFFF;
    if (g_hud.banner[0] && timing_get_us() < g_hud.banner_until_us) {
        const float p1 = UIS_TF(26.0f), p2 = UIS_TF(14.0f);
        char l1[96], l2[96];
        snprintf(l1, sizeof l1, "%s", g_hud.banner);
        snprintf(l2, sizeof l2, "%s", g_hud.banner_sub);
        const int maxw = v3x(900) - v3x(57);
        int n1 = (int)strlen(l1), n2 = (int)strlen(l2);
        while (n1 > 1 && ttf_text_width(l1, p1, true) > maxw) l1[--n1] = 0;
        while (n2 > 1 && ttf_text_width(l2, p2) > maxw) l2[--n2] = 0;
        int w1 = ttf_text_width(l1, p1, true), w2 = l2[0] ? ttf_text_width(l2, p2) : 0;
        const int bw3 = (w1 > w2 ? w1 : w2) + UIS_W(40);
        const int bh3 = UIS_H(l2[0] ? 84 : 56);
        const int bx3 = v3x(57), by3 = v3y(48);
        v3_rrect(bx3, by3, bw3, bh3, UIS_H(12), 1, 0x000A0C18, 205, WHITE, 40);
        drawTTF((u32)(bx3 + UIS_W(20)), (u32)(by3 + UIS_H(12)), l1, p1, WHITE, true);
        if (l2[0])
            drawTTF((u32)(bx3 + UIS_W(20)), (u32)(by3 + UIS_H(50)), l2, p2, 0x00C2C2C2);
        ovl_span(by3, by3 + bh3);
    }
}

static void hud_body_v3(u64 elapsed_us, bool paused, bool scrubbing) {
    const int W = (int)s_ovl_w;
    const u32 WHITE = 0x00FFFFFF;

    // ---- bottom scrim: y=459, 260 tall, transparent -> black 0.90 @ 62% ----
    {
        const int y0 = v3y(459), h = (int)s_ovl_h - y0;
        for (int i = 0; i < h; i++) {
            float t = (float)i / ((float)h * 0.62f);
            if (t > 1.0f) t = 1.0f;
            const u8 a = (u8)(230.0f * t + 0.5f);
            if (a) v3_fill(0, y0 + i, W, 1, 0, a);
        }
    }

    // ---- elapsed / remaining ----
    const u32 el = (u32)(elapsed_us / 1000000ULL);
    char el_s[16], rem_s[20] = "";
    fmt_time(el_s, sizeof el_s, el);
    if (g_hud.live) {
        el_s[0] = '\0';                       // no position in a live stream
    } else if (g_hud.total_secs > 0) {
        const u32 rem = g_hud.total_secs > el ? g_hud.total_secs - el : 0;
        char t[16]; fmt_time(t, sizeof t, rem);
        snprintf(rem_s, sizeof rem_s, "-%s", t);
    }

    // ---- the big Play / Pause button, bottom left (2026-09-27) ----
    // What X does by default: PLAY while paused, PAUSE while playing.  A
    // 64 px disc at the start of the title row, the title moved right of it.
    const int pb_d = UIS_H(50), pb_x = v3x(57), pb_y = v3y(529);
    {
        const bool foc = g_hud.focus == FOCUS_PP;
        if (foc)
            v3_rrect(pb_x - UIS_W(5), pb_y - UIS_H(5), pb_d + UIS_H(10), pb_d + UIS_H(10),
                     (pb_d + UIS_H(10)) / 2, 0, XMB_ACCENT, 60, 0, 0);
        v3_rrect(pb_x, pb_y, pb_d, pb_d, pb_d / 2, 1, XMB_ACCENT, 190,
                 foc ? XMB_ACCENT_ALT : WHITE, foc ? 200 : 70);
        const int cx = pb_x + pb_d / 2, cy = pb_y + pb_d / 2;
        if (paused) {
            // a play triangle, nudged right of centre so it LOOKS centred
            const int th = pb_d * 26 / 64, tw = th * 7 / 8;
            const int tx = cx - tw / 2 + pb_d / 32, ty = cy - th / 2;
            for (int r = 0; r < th; r++) {
                const float f = (float)r / (float)(th - 1);
                const int w = (int)((1.0f - (f < 0.5f ? 1.0f - 2.0f * f : 2.0f * f - 1.0f)) * (float)tw + 0.5f);
                if (w > 0) v3_fill(tx, ty + r, w, 1, WHITE, 255);
            }
        } else {
            // two pause bars
            const int bh = pb_d * 24 / 64, bw = pb_d * 7 / 64, gap = pb_d * 7 / 64;
            v3_rrect(cx - gap / 2 - bw, cy - bh / 2, bw, bh, bw / 3, 0, WHITE, 255, 0, 0);
            v3_rrect(cx + gap / 2,      cy - bh / 2, bw, bh, bw / 3, 0, WHITE, 255, 0, 0);
        }
    }

    // ---- title: 30px/700 white, -0.01em, beside the button ----
    const int chips_right = v3x(1223);
    if (g_hud.title[0]) {
        const float tpx = UIS_TF(30.0f);
        const int tx0 = pb_x + pb_d + UIS_W(20);
        char tb[128];
        snprintf(tb, sizeof tb, "%s", g_hud.title);
        const int maxw = v3x(960) - tx0;
        int len = (int)strlen(tb);
        while (len > 1 && ttf_text_width(tb, tpx, true) > maxw) tb[--len] = 0;
        drawTTF((u32)tx0, (u32)(pb_y + (pb_d - (int)tpx) / 2 - UIS_H(2)), tb, tpx, WHITE, true);
    }

    // ---- chips, right-aligned to x=1223: audio, volume, subtitles ----
    // Each is also a focus slot.  Styled as XMB glass rather than flat
    // pills: a dark translucent body, a hairline border, a one-pixel light
    // catch along the top edge, and two lines of type -- the control's name
    // small and spaced in the eyebrow face, its value under it.  The volume
    // chip carries a level meter along its floor; subtitles light an accent
    // dot when on.  Focus is the accent_alt border with a soft accent halo.
    // All of it is composed into the staging buffer on change, like the rest
    // of the HUD -- no per-frame cost.
    {
        // On a low-resolution display the type has a floor (uis_tf), so the chip
        // grows to hold it, and the small type is drawn heavy: thin strokes
        // vanish on a CRT.
        const bool  sd  = (int)display_height < 720;
        const float kpx = (sd && UIS_TF(8.5f) < 9.5f) ? 9.5f : UIS_TF(8.5f), vpx = UIS_TF(12.0f);
        const int  ky = UIS_H(7);
        const int  vy = UIS_H(18) > ky + (int)kpx + UIS_H(3) ? UIS_H(18) : ky + (int)kpx + UIS_H(3);
        const int  ch = UIS_H(38) > vy + (int)vpx + UIS_H(7) ? UIS_H(38) : vy + (int)vpx + UIS_H(7);
        const int  cy = v3y(574), gap = UIS_W(10), pad = UIS_W(14);
        const int  r  = UIS_H(9);
        char a_val[72], v_val[16];
        const char *al = g_hud.audio_label;
        const bool named = al[0] && strcmp(al, "AUDIO") != 0;
        snprintf(a_val, sizeof a_val, "%s", named ? al : TR("Default"));
        if (audio_passthrough_active()) snprintf(v_val, sizeof v_val, TR("Receiver"));
        else                            snprintf(v_val, sizeof v_val, "%d%%", audio_get_volume());
        const int aspect = picture_get().aspect;
        const char *keys[4] = { TR("AUDIO"), TR("VOLUME"), TR("SUBTITLES"), TR("PICTURE") };
        const char *vals[4] = { a_val, v_val, g_hud.cc_active ? TR("On") : TR("Off"),
                                aspect == PIC_FILL ? TR("Fill") : aspect == PIC_STRETCH ? TR("Stretch") : TR("Fit") };
        const int   slot[4] = { FOCUS_AUDIO, FOCUS_VOLUME, FOCUS_CC, FOCUS_ASPECT };
        // The audio value is the long one: clip it so the row never runs
        // into the title.
        {
            const int maxw = UIS_W(300);
            int len = (int)strlen(a_val);
            while (len > 1 && ttf_text_width(a_val, vpx, sd) > maxw) a_val[--len] = 0;
        }
        int w[4], x = chips_right;
        for (int i = 3; i >= 0; i--) {
            const int kw = ttf_text_width_face(keys[i], kpx, UI_FACE_EYEBROW);
            const int vw = ttf_text_width(vals[i], vpx, sd);
            // CC: a dot left of both lines.  PICTURE: an icon left of the value only.
            const int dot = (slot[i] == FOCUS_CC) ? UIS_W(12)
                          : (slot[i] == FOCUS_ASPECT) ? v3_aspect_icon_w() + UIS_W(10) : 0;
            w[i] = (slot[i] == FOCUS_ASPECT ? (kw > vw + dot ? kw : vw + dot)
                                            : (kw > vw ? kw : vw) + dot) + 2 * pad;
            if (slot[i] == FOCUS_VOLUME && w[i] < UIS_W(76)) w[i] = UIS_W(76);
            x -= w[i];
            const bool foc = g_hud.focus == slot[i];
            // halo (focused only), body, border
            if (foc)
                v3_rrect(x - UIS_W(4), cy - UIS_H(4), w[i] + UIS_W(8), ch + UIS_H(8),
                         r + UIS_H(4), 0, XMB_ACCENT, 46, 0, 0);
            v3_rrect(x, cy, w[i], ch, r, 1, 0x000A0C18, 184,
                     foc ? XMB_ACCENT_ALT : WHITE, foc ? 255 : 52);
            // the light catch along the top straight edge
            v3_fill(x + r, cy + 1, w[i] - 2 * r, 1, WHITE, foc ? 70 : 34);
            // type
            const int tx = x + pad + dot;
            const int kx = (slot[i] == FOCUS_ASPECT) ? x + pad : tx;
            if (slot[i] == FOCUS_ASPECT) v3_aspect_icon(x + pad, cy + vy, aspect, foc);
            drawTTF_face((u32)kx, (u32)(cy + ky), keys[i], kpx,
                         foc ? 0x00C9C3F2 : 0x008A90B4, UI_FACE_EYEBROW);
            drawTTF((u32)tx, (u32)(cy + vy), vals[i], vpx,
                    foc ? WHITE : 0x00E4E6F0, sd);
            if (slot[i] == FOCUS_CC) {
                const int d = UIS_H(6);
                v3_rrect(x + pad, cy + ch / 2 - d / 2, d, d, d / 2, 0,
                         g_hud.cc_active ? XMB_ACCENT_ALT : WHITE,
                         g_hud.cc_active ? 255 : 60, 0, 0);
            }
            // (The volume chip's level meter -- a purple line on its floor --
            // is gone: it read as a mystery underline.  The % says it.)
            // The volume slider opens above its chip while adjusting.
            if (slot[i] == FOCUS_VOLUME && g_hud.vol_active && audio_passthrough_active()) {
                // Nothing is mixed here: the receiver decodes and owns the level.
                const char *msg = TR("Volume is set on your soundbar or receiver");
                const float mpx = UIS_TF(12.0f);
                const int mw = ttf_text_width(msg, mpx) + UIS_W(28), mh = UIS_H(34);
                const int mx = x + w[i] - mw, my = cy - UIS_H(14) - mh;
                v3_rrect(mx, my, mw, mh, UIS_H(9), 1, 0x000A0C18, 200, WHITE, 40);
                drawTTF((u32)(mx + UIS_W(14)), (u32)(my + (mh - (int)mpx) / 2), msg, mpx, WHITE);
            } else if (slot[i] == FOCUS_VOLUME && g_hud.vol_active) {
                const float cpx = UIS_TF(11.0f);
                const int vol = audio_get_volume();
                const int sx  = x + w[i] / 2, bot = cy - UIS_H(14);
                const int th  = UIS_H(90), tw = UIS_W(6);
                v3_rrect(sx - UIS_W(22), bot - th - UIS_H(34), UIS_W(44), th + UIS_H(44),
                         UIS_H(9), 1, 0x000A0C18, 200, WHITE, 40);
                v3_fill(sx - tw / 2, bot - th, tw, th, WHITE, 60);
                const int fh = th * vol / 100;
                v3_fill(sx - tw / 2, bot - fh, tw, fh, XMB_ACCENT, 255);
                char vb[8]; snprintf(vb, sizeof vb, "%d", vol);
                const int vw2 = ttf_text_width(vb, cpx);
                drawTTF((u32)(sx - vw2 / 2), (u32)(bot - th - UIS_H(26)), vb, cpx, WHITE);
            }
            x -= gap;
        }
    }

    // ---- progress: (57,628) 1166 x 6, r=3; played in accent ----
    const int bx = v3x(57), by = v3y(628), bw = v3x(1223) - v3x(57), bh = UIS_H(6);
    v3_rrect(bx, by, bw, bh, bh / 2, 0, WHITE, 41, 0, 0);
    int fill = 0;
    if (g_hud.live) {
        if (g_hud.live_permille > 0) fill = (int)((long long)bw * g_hud.live_permille / 1000);
    } else if (g_hud.total_secs > 0) {
        float p = (float)el / (float)g_hud.total_secs;
        if (p > 1.0f) p = 1.0f;
        fill = (int)(p * (float)bw);
    }
    if (fill > 0) v3_rrect(bx, by, fill, bh, bh / 2, 0, XMB_ACCENT, 255, 0, 0);
    if (!g_hud.live && (scrubbing || g_hud.focus == FOCUS_REW || g_hud.focus == FOCUS_FF))
        v3_rrect(bx + fill - UIS_H(7), by + bh / 2 - UIS_H(7), UIS_H(14), UIS_H(14),
                 UIS_H(7), 0, WHITE, 255, 0, 0);

    // REW / FF focus: the step they will jump, above the bar at its ends.
    if (!g_hud.live && (g_hud.focus == FOCUS_REW || g_hud.focus == FOCUS_FF)) {
        char st[24];
        const int n = V3_INCR[g_hud.incr_idx < 0 ? 0 : g_hud.incr_idx % 3];
        const float spx = UIS_TF(12.5f);
        if (g_hud.focus == FOCUS_REW) {
            snprintf(st, sizeof st, TR("\xE2\x80\xB9 back %ds"), n);
            drawTTF((u32)bx, (u32)(by - UIS_H(24)), st, spx, XMB_ACCENT_ALT);
        } else {
            snprintf(st, sizeof st, TR("ahead %ds \xE2\x80\xBA"), n);
            const int sw = ttf_text_width(st, spx);
            drawTTF((u32)(bx + bw - sw), (u32)(by - UIS_H(24)), st, spx, XMB_ACCENT_ALT);
        }
    }

    // Trickplay preview while hold-scrubbing, centred on the play head.
    if (scrubbing) {
        const TrickplayTile *tp = trickplay_current();
        if (tp && tp->sheet_rgb && tp->tile_w > 0 && tp->tile_h > 0) {
            const int cw = tp->tile_w + CARD_PAD * 2, chh = tp->tile_h + CARD_PAD * 2;
            int cx = bx + fill - cw / 2;
            if (cx < bx) cx = bx;
            if (cx + cw > bx + bw) cx = bx + bw - cw;
            const int cyy = by - UIS_H(34) - chh;
            ovl_dim((u32)cx, (u32)cyy, (u32)cw, (u32)chh, 235);
            drawBitmapRect(tp->sheet_rgb, (u32)tp->sheet_w,
                           (u32)tp->tile_x, (u32)tp->tile_y,
                           (u32)tp->tile_w, (u32)tp->tile_h,
                           (u32)(cx + CARD_PAD), (u32)(cyy + CARD_PAD));
        }
    }

    // ---- times: 12.5px at y=644, white@76% ----
    {
        const float tpx = UIS_TF(12.5f);
        if (g_hud.live) {
            // The LIVE badge, then the programme; its times at the right.
            const int bh2 = UIS_H(18), bw2 = ttf_text_width_face(TR("LIVE"), UIS_TF(9.5f), UI_FACE_EYEBROW) + UIS_W(16);
            v3_rrect(bx, v3y(644), bw2, bh2, bh2 / 2, 0, 0x00D8344A, 235, 0, 0);
            drawTTF_face((u32)(bx + UIS_W(8)), (u32)(v3y(644) + UIS_H(4)), TR("LIVE"), UIS_TF(9.5f),
                         WHITE, UI_FACE_EYEBROW);
            int tw_max = bw - bw2 - UIS_W(12);
            if (g_hud.live_times[0]) {
                const int rw = ttf_text_width(g_hud.live_times, tpx);
                drawTTF((u32)(bx + bw - rw), (u32)v3y(644), g_hud.live_times, tpx, 0x00C2C2C2);
                tw_max -= rw + UIS_W(20);
            }
            char pt[96];
            snprintf(pt, sizeof pt, "%s", g_hud.live_title);
            int len = (int)strlen(pt);
            while (len > 1 && ttf_text_width(pt, tpx) > tw_max) pt[--len] = 0;
            drawTTF((u32)(bx + bw2 + UIS_W(12)), (u32)v3y(644), pt, tpx, 0x00C2C2C2);
        } else {
            const bool hv = (int)display_height < 720;      // heavy on SD: see the chips
            drawTTF((u32)bx, (u32)v3y(644), el_s, tpx, 0x00E0E0E0, hv);
            if (rem_s[0]) {
                const int rw = ttf_text_width(rem_s, tpx, hv);
                drawTTF((u32)(bx + bw - rw), (u32)v3y(644), rem_s, tpx, 0x00E0E0E0, hv);
            }
        }
    }

    // (The canvas's audio-path pill at the top right is gone: it only ever
    // read AUDIO.  The track is named in the audio chip instead.)

    // ---- hints: what X does, then Tracks / Stop / Hide ----
    {
        Hint h[6]; int n = 0;
        h[n].glyph = 'X';
        h[n].label = g_hud.focus == FOCUS_REW    ? TRN("Back")
                   : g_hud.focus == FOCUS_FF     ? TRN("Ahead")
                   : g_hud.focus == FOCUS_AUDIO  ? TRN("Audio")
                   : g_hud.focus == FOCUS_VOLUME ? TRN("Volume")
                   : g_hud.focus == FOCUS_CC     ? TRN("Subtitles")
                   : g_hud.focus == FOCUS_ASPECT ? TRN("Picture shape")
                   : paused                      ? TRN("Play") : TRN("Pause");
        n++;
        if (g_hud.live) {
            h[n].glyph = 'D'; h[n].label = TRN("Channel"); n++;
        } else {
            h[n].glyph = 'L'; h[n].label = "";       n++;   // L2 R2: one cluster
            h[n].glyph = 'R'; h[n].label = TRN("Rewind / Fast Forward"); n++;
        }
        h[n].glyph = 'T'; h[n].label = TRN("Tracks"); n++;
        h[n].glyph = 'C'; h[n].label = TRN("Stop");   n++;
        h[n].glyph = 'S'; h[n].label = TRN("Hide");   n++;
        draw_hints_bar(h, n);
        ovl_span(v3y(660), (int)s_ovl_h);
    }

    v3_draw_banner();

    // The track menu, lifted above the chips instead of sitting on the strip.
    if (g_hud.menu_visible && g_hud.menu_n > 0)
        draw_menu(W, v3y(560) + HUD_STRIP_H + 6);
}
