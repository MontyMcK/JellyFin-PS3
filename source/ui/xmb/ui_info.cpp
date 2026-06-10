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
            u32 X = XMB_ITEM_PAD;
            u32 Y = XMB_TOPBAR_H + 10;
            drawTTF(X, Y, it->name, 56, 0x00FFFFFF);
            Y += 80;
            char meta_line[256];
            snprintf(meta_line, sizeof(meta_line), "%s  %s  %s  %s",
                     it->year_str,
                     it->duration_str,
                     detail.official_rating[0] ? detail.official_rating : "",
                     detail.community_rating[0] ? detail.community_rating : "");
            drawTTF(X, Y, meta_line, 28, 0x00AAAAAA);
            Y += 56;
            if (detail.video_info[0]) {
                drawTTF(X,       Y, "Video:", 24, 0x00888888);
                drawTTF(X + 160, Y, detail.video_info, 24, 0x00FFFFFF);
                Y += 40;
            }
            if (detail.audio_info[0]) {
                drawTTF(X,       Y, "Audio:", 24, 0x00888888);
                drawTTF(X + 160, Y, detail.audio_info, 24, 0x00FFFFFF);
                Y += 48;
            }
            if (detail.tagline[0]) {
                drawTTF(X, Y, detail.tagline, 32, 0x00AACCFF);
                Y += 56;
            }
            if (detail.overview[0]) {
                const int max_cpl   = 40;
                const int max_lines = 6;
                const char *p = detail.overview;
                int lines_drawn = 0;
                while (*p && lines_drawn < max_lines) {
                    int line_len = (int)strlen(p);
                    if (line_len > max_cpl) {
                        int i;
                        for (i = max_cpl; i > 0; i--)
                            if (p[i] == ' ') break;
                        if (i == 0) i = max_cpl;
                        char buf[128];
                        snprintf(buf, sizeof(buf), "%.*s", i, p);
                        drawTTF(X, Y, buf, 24, 0x00FFFFFF);
                        p += i;
                        if (*p == ' ') p++;
                    } else {
                        drawTTF(X, Y, p, 24, 0x00FFFFFF);
                        p += line_len;
                    }
                    Y += 36;
                    lines_drawn++;
                }
                Y += 16;
            }
            if (detail.genres[0]) {
                drawTTF(X,       Y, "Genres:", 24, 0x00888888);
                drawTTF(X + 160, Y, detail.genres, 24, 0x00FFFFFF);
                Y += 40;
            }
            if (detail.studios[0]) {
                drawTTF(X,       Y, "Studios:", 24, 0x00888888);
                drawTTF(X + 160, Y, detail.studios, 24, 0x00FFFFFF);
            }
        }
        {
            static const Hint h[] = {{'C',"BACK"}};
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
