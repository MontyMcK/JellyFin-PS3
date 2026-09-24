#include "ui_tab_anim.h"
#include "timing.h"
#include "ui_visuals.h"
#include <math.h>

// 450 ms: an unhurried turn that reads clearly on a TV.  A second press mid-
// turn carries on from where the wheel is, so a run of presses still flows.
#define TAB_ANIM_US    450000ULL
// The content rides with the wheel but travels only this fraction of the
// tab's distance -- under 40 px at 720p.  At the full distance it started a
// whole tab step away, which read as the page teleporting on the first frame
// and pushed the left column off the screen.
#define TAB_CONTENT_FRAC 0.28f

static u64   s_t0    = 0;      // 0 = never started, i.e. at rest
static float s_from  = 0.0f;
static float s_delta = 0.0f;
static int   s_dir   = 0;      // -1 content enters from the left, +1 right

// Ease-out cubic over [0,1]: fast away, gentle landing.
static float tab_anim_ease(void) {
    if (!s_t0) return 1.0f;
    const u64 el = timing_get_us() - s_t0;
    if (el >= TAB_ANIM_US) return 1.0f;
    const float t = 1.0f - (float)el / (float)TAB_ANIM_US;
    return 1.0f - t * t * t;
}

void tab_anim_start(float from_pos, float delta) {
    s_from  = from_pos;
    s_delta = delta;
    s_dir   = delta > 0.0f ? 1 : (delta < 0.0f ? -1 : 0);
    s_t0    = timing_get_us();
}

float tab_anim_ring_pos(float target) {
    const float e = tab_anim_ease();
    if (e >= 1.0f) return target;
    return s_from + s_delta * e;
}

int tab_wheel_x(float d) {
    // Radius chosen so a neighbour sits one normal tab step from the centre.
    const float rad = (float)UIS_H(136) / sinf(TAB_WHEEL_RAD);
    return (int)(rad * sinf(d * TAB_WHEEL_RAD));
}

int tab_anim_content_dx(void) {
    const float e = tab_anim_ease();
    if (e >= 1.0f || !s_dir) return 0;
    // Follows the NEW tab on the wheel -- same curve, same instant -- scaled
    // down so it glides a short way rather than flying in.
    return (int)((float)tab_wheel_x(s_delta * (1.0f - e)) * TAB_CONTENT_FRAC);
}

// --- d-pad focus glide ------------------------------------------------------

// 170 ms: quick enough that the d-pad never feels behind the thumb, long
// enough to be seen travelling.  Holding a direction restarts the glide from
// wherever the ring is, so a run of presses chains into one smooth slide.
#define FOCUS_GLIDE_US 170000ULL

static int   s_fg_ctx  = -1;
static u64   s_fg_t0   = 0;
static u64   s_fg_last = 0;           // when the ring was last drawn
static float s_fg_from[4], s_fg_to[4], s_fg_now[4];

void focus_glide(int ctx, int *x, int *y, int *w, int *h) {
    const u64 now = timing_get_us();
    const float tgt[4] = { (float)*x, (float)*y, (float)*w, (float)*h };
    const bool snap = ctx != s_fg_ctx
                   || !s_fg_last || now - s_fg_last > 120000ULL
                   || tab_anim_content_dx() != 0;
    s_fg_ctx  = ctx;
    s_fg_last = now;

    if (snap) {
        for (int i = 0; i < 4; i++) s_fg_from[i] = s_fg_to[i] = s_fg_now[i] = tgt[i];
        s_fg_t0 = 0;
        return;
    }
    if (tgt[0] != s_fg_to[0] || tgt[1] != s_fg_to[1] ||
        tgt[2] != s_fg_to[2] || tgt[3] != s_fg_to[3]) {
        // New target: set off from what is on screen right now.
        for (int i = 0; i < 4; i++) { s_fg_from[i] = s_fg_now[i]; s_fg_to[i] = tgt[i]; }
        s_fg_t0 = now;
    }
    float e = 1.0f;
    if (s_fg_t0) {
        const u64 el = now - s_fg_t0;
        if (el < FOCUS_GLIDE_US) {
            const float t = 1.0f - (float)el / (float)FOCUS_GLIDE_US;
            e = 1.0f - t * t * t;
        }
    }
    for (int i = 0; i < 4; i++)
        s_fg_now[i] = s_fg_from[i] + (s_fg_to[i] - s_fg_from[i]) * e;
    *x = (int)(s_fg_now[0] + 0.5f);
    *y = (int)(s_fg_now[1] + 0.5f);
    *w = (int)(s_fg_now[2] + 0.5f);
    *h = (int)(s_fg_now[3] + 0.5f);
}
