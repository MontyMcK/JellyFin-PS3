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

void xmb_show_item_info(const XMBItem *it) {
    {
        char dbg[260];
        snprintf(dbg, sizeof(dbg),
            "info: ENTER "
            "btn_cur(tri=%d cir=%d crs=%d) btn_prev(tri=%d cir=%d crs=%d) "
            "name='%.40s'",
            btn_cur.triangle, btn_cur.circle, btn_cur.cross,
            btn_prev.triangle, btn_prev.circle, btn_prev.cross,
            it->name);
        plog(dbg);
    }
    rsxSync();
    flip();
    init_btns();
    {
        char dbg[200];
        snprintf(dbg, sizeof(dbg),
            "info: after init_btns btn_cur(tri=%d cir=%d crs=%d) "
            "btn_prev(tri=%d cir=%d crs=%d)",
            btn_cur.triangle, btn_cur.circle, btn_cur.cross,
            btn_prev.triangle, btn_prev.circle, btn_prev.cross);
        plog(dbg);
    }
    XMBItemDetail detail;
    memset(&detail, 0, sizeof(detail));
    jellyfin_fetch_item_detail(it->id, &detail);
    int info_frames = 0;
    int info_exit_reason = 0;
    bool exit_armed = false;
    while (running) {
        waitflip();
        sysUtilCheckCallback();
        poll_buttons();
        if (!exit_armed) {
            if (!btn_cur.circle && !btn_cur.triangle)
                exit_armed = true;
        } else {
            if (BTN_PRESSED(circle))   { info_exit_reason = 1; goto info_done; }
            if (BTN_PRESSED(triangle)) { info_exit_reason = 2; goto info_done; }
        }
        clearScreen(XMB_BG);
        wave_draw();
        rsxSync();
        {
            int X = XMB_ITEM_PAD;
            int Y = XMB_TOPBAR_H + 12;
            int max_w = (int)display_width - 2 * X;

            // Title, truncated to the content width.
            {
                char tbuf[132];
                snprintf(tbuf, sizeof(tbuf), "%s", it->name);
                int len = (int)strlen(tbuf);
                while (len > 1 && ttf_text_width(tbuf, 40) > max_w)
                    tbuf[--len] = '\0';
                drawTTF((u32)X, (u32)Y, tbuf, 40, XMB_WHITE, true);
            }
            Y += 62;

            // Meta row: year · duration, official-rating chip, ★ rating.
            {
                int mx = X;
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

            if (detail.tagline[0]) {
                drawTTF((u32)X, (u32)Y, detail.tagline, 20, 0x00AFA3E8UL);
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
                    drawTTF((u32)X, (u32)Y, buf, 19, 0x00C9CEE4UL);
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
                drawTTF((u32)X,         (u32)(Y + 2), facts[i].label, 15,
                        XMB_TEXT_FAINT);
                drawTTF((u32)(X + 120), (u32)Y, facts[i].value, 17, XMB_TEXT);
                Y += 32;
            }
        }
        {
            static const Hint h[] = {{'C',"Back"}};
            draw_hints_bar(h, 1);
        }
        flip();
        info_frames++;
        if ((info_frames % 30) == 0) {
            char dbg[80];
            snprintf(dbg, sizeof(dbg), "info: frame=%d", info_frames);
            plog(dbg);
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
    g_info_cooldown_until = timing_get_us() + 500000;
    init_btns();
}
