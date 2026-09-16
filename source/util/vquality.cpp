// Video quality selection store — see vquality.h.

#include "vquality.h"
#include "jf_paths.h"     // jf_data_path()
#include <stdio.h>

#define VQUALITY_FILE "jellyfin_vquality.txt"

static vquality_t s_q = VQ_AUTO;

vquality_t vquality_get(void) { return s_q; }

void vquality_set(vquality_t q) {
    if (q < VQ_AUTO || q >= VQ_COUNT) q = VQ_AUTO;
    s_q = q;
    vquality_save();
}

void vquality_next(int delta) {
    int n = (int)s_q + delta;
    if (n < 0)          n = VQ_COUNT - 1;
    if (n >= VQ_COUNT)  n = 0;
    vquality_set((vquality_t)n);
}

const char *vquality_label(vquality_t q) {
    switch (q) {
    case VQ_1080P: return "1080p";
    case VQ_720P:  return "720p";
    case VQ_480P:  return "480p";
    case VQ_360P:  return "360p";
    default:       return "Auto";
    }
}

void vquality_params(vquality_t q, bool hd_toggle,
                     u32 max_w, u32 max_h,
                     u32 *out_w, u32 *out_h,
                     const char **out_profile, const char **out_level,
                     unsigned *out_vbitrate)
{
    // Resolve Auto to whatever the 1080p toggle says, so the two settings
    // never contradict each other and Auto stays exactly the old behaviour.
    if (q == VQ_AUTO) q = hd_toggle ? VQ_1080P : VQ_720P;

    u32         w, h;
    unsigned    br;
    const char *profile = "baseline";
    const char *level   = "31";

    switch (q) {
    case VQ_1080P:
        // 1080p exceeds baseline/level-3.1, so it needs High/4.2 — the same
        // pairing the 1080p (Alpha) path has always requested.
        w = 1920; h = 1080; br = 10000000u;
        profile = "high"; level = "42";
        break;
    case VQ_480P: w = 854; h = 480; br = 1500000u; break;
    case VQ_360P: w = 640; h = 360; br =  700000u; break;
    case VQ_720P:
    default:      w = 1280; h = 720; br = 4000000u; break;
    }

    // Never ask for more than the output can show.  1080p is deliberately
    // exempt: the point of that mode is the higher-detail transcode, and the
    // player allocates its jitter buffer for the full frame.
    if (q != VQ_1080P) {
        if (max_w && w > max_w) w = max_w;
        if (max_h && h > max_h) h = max_h;
    }

    if (out_w)        *out_w        = w;
    if (out_h)        *out_h        = h;
    if (out_profile)  *out_profile  = profile;
    if (out_level)    *out_level    = level;
    if (out_vbitrate) *out_vbitrate = br;
}

// Missing file => Auto, which is the behaviour that shipped.
void vquality_load(void) {
    FILE *f = fopen(jf_data_path(VQUALITY_FILE), "r");
    if (!f) return;
    int v = 0;
    if (fscanf(f, "%d", &v) == 1 && v >= VQ_AUTO && v < VQ_COUNT)
        s_q = (vquality_t)v;
    fclose(f);
}

void vquality_save(void) {
    FILE *f = fopen(jf_data_path(VQUALITY_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)s_q);
    fclose(f);
}
