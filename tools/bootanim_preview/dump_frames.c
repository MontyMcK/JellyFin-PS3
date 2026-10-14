// Dump the cold-boot animation's frames as JSON, for mkpreview.py.
//
// Runs the REAL timeline (source/ui/render/boot_seq.h) through a simulated
// startup and prints, per frame, what boot_anim.cpp would draw at 1920x1080:
// the veil rows, the halo, where the mark is and who draws it, the status
// line and each letter of the wordmark.  The pixel layout mirrors layout()
// in boot_anim.cpp and xmb_lockup_geom() at 1080p with no overscan -- keep
// the constants below in step with those if either moves.
//
//   cc -O2 -I../../source/ui/render -o dump_frames dump_frames.c -lm
//   ./dump_frames [xmb_ready_ms] [xmb_frame_ms] [skip_ms] > frames.json
//
// xmb_ready_ms  when the XMB starts drawing (default 1400: measured cold init)
// xmb_frame_ms  its frame time once it does (default 33.3: mode 3 today)
// skip_ms       a button press at this time, or -1 (default)

#include <stdio.h>
#include <stdlib.h>
#include "boot_seq.h"

#define W 1920.0f
#define H 1080.0f

// boot_anim.cpp
#define CENTRE_BELL_H 0.17f
#define CENTRE_Y_H    0.47f
#define CPU_ZONE      1.6f
// xmb_lockup_geom() at 1080p (UIS factor 1.5, integer UIS_H), overscan 0
#define LK_MARK_X   60.0f     // UIS_W(40)
#define LK_MARK_Y   33.0f     // cy 48 - 31/2
#define LK_MARK_PX  31.0f     // UIS_H(21)

static const char *phase_name(BootPhase p)
{
    static const char *n[] = { "DARK", "EMBLEM", "AWAIT", "SHINE", "EMERGE",
                               "DOCK", "WORDMARK", "DISMISS", "DONE" };
    return n[p];
}

int main(int argc, char **argv)
{
    float xmb_at   = argc > 1 ? (float)atof(argv[1]) : 1400.0f;
    float xmb_dt   = argc > 2 ? (float)atof(argv[2]) : 33.3f;
    float skip_at  = argc > 3 ? (float)atof(argv[3]) : -1.0f;
    BootSeq s;
    BootFrame f;
    float t = 0.0f;
    int first = 1, extra = 0;

    boot_seq_begin(&s);
    printf("{\"w\":%.0f,\"h\":%.0f,\"lockup\":{\"x\":%.0f,\"y\":%.0f,\"px\":%.0f,"
           "\"word_x\":100,\"word_cy\":48,\"word_px\":21,\"slide\":%.2f},"
           "\"frames\":[\n",
           W, H, LK_MARK_X, LK_MARK_Y, LK_MARK_PX, BOOT_WORD_SLIDE_EM * 21.0);

    while (t < 60000.0f) {
        const int xmb = t >= xmb_at;
        const float dt = xmb ? xmb_dt : 1000.0f / 60.0f;
        t += dt;
        if (xmb) boot_seq_signal(&s, BOOT_SIG_XMB);
        if (skip_at >= 0.0f && t >= skip_at && t - dt < skip_at && xmb &&
            boot_seq_skip_effective(&s))
            boot_seq_signal(&s, BOOT_SIG_SKIP);
        boot_seq_step(&s, dt);
        boot_seq_frame(&s, &f);

        // layout(), as boot_anim.cpp does it
        float bell0 = CENTRE_BELL_H * H, cx0 = 0.5f * W, cy0 = CENTRE_Y_H * H;
        float cx = cx0, cy = cy0, bell = bell0 * f.mark_scale;
        const char *owner = "none";
        if (!f.done) {
            if (!f.xmb_visible) {
                owner = f.mark_opacity > 0.0f ? "overlay" : "none";
            } else {
                float bell1 = LK_MARK_PX;
                float cx1 = LK_MARK_X + 0.5f * bell1, cy1 = LK_MARK_Y + 0.5f * bell1;
                bell = boot_dock_size_px(bell0, bell1, f.dock_size);
                boot_dock_point(cx0, cy0, cx1, cy1, f.dock_pos, &cx, &cy);
                if (f.dock_pos >= 1.0f && f.dock_size >= 1.0f) owner = "static";
                else if (f.veil >= 1.0f && bell <= CPU_ZONE * bell1) owner = "lockup";
                else owner = "overlay";
            }
        } else {
            // DONE: the console hands back a NULL pose and the static lockup
            // draws at its own geometry -- so that is where the mark is.
            owner = s.left ? "none" : "static";
            if (!s.left) {
                bell = LK_MARK_PX;
                cx = LK_MARK_X + 0.5f * LK_MARK_PX;
                cy = LK_MARK_Y + 0.5f * LK_MARK_PX;
            }
        }

        float ys[BOOT_VEIL_ROWS_MAX];
        unsigned char as[BOOT_VEIL_ROWS_MAX];
        int n = f.xmb_visible || f.done ? boot_veil_rows(f.veil, H, ys, as)
                                        : boot_veil_rows(0.0f, H, ys, as);
        if (f.done) n = 0;

        printf("%s{\"t\":%.1f,\"phase\":\"%s\",\"xmb\":%d,\"veil\":[",
               first ? "" : ",\n", t, phase_name(f.phase), f.xmb_visible);
        for (int i = 0; i < n; i++)
            printf("%s[%.1f,%u]", i ? "," : "", ys[i], as[i]);
        printf("],\"halo\":%.4f,\"status\":%.4f,\"op\":%.4f,"
               "\"glint\":%.4f,\"gpos\":%.4f,\"spark\":%.4f,\"srot\":%.4f,"
               "\"mark\":{\"cx\":%.2f,\"cy\":%.2f,\"bell\":%.2f,\"owner\":\"%s\"},"
               "\"word\":[",
               f.halo * BOOT_HALO_PEAK, f.status, f.mark_opacity,
               f.glint, f.glint_pos, f.spark, f.spark_rot,
               cx, cy, bell, owner);
        for (int i = 0; i < BOOT_WORD_LETTERS; i++)
            printf("%s[%.4f,%.4f]", i ? "," : "",
                   f.done && !s.left ? 1.0f : f.word_reveal[i],
                   f.done && !s.left ? 1.0f : f.word_alpha[i]);
        printf("]}");
        first = 0;
        // A second of the static result after DONE.
        if (f.done && ++extra > (int)(1000.0f / dt)) break;
    }
    printf("\n]}\n");
    return 0;
}
