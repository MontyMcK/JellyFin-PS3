// Video quality selection store — see vquality.h.

#include "vquality.h"
#include "jf_paths.h"     // jf_data_path()
#include <stdio.h>
#include <string.h>

#define VQUALITY_FILE "jellyfin_vquality.txt"

static vquality_t s_q = VQ_AUTO;

// Offered steps, in picture-quality order -- see vquality.h.  The enum's own
// order is historical (the 1080p ceilings were appended as they were added),
// so cycling it directly made left/right jump about.
const vquality_t VQUALITY_ORDER[] = {
    VQ_AUTO,
    VQ_360P,
    VQ_480P,
    VQ_720P,
    VQ_1080P,       // 10 Mbps -- "High"
    VQ_1080P_20,    // 20 Mbps -- "Very High"
    VQ_1080P_25,    // 25 Mbps -- "Max"
    // 30 Mbps is BACK, on trial.  It was retired on a measurement taken when
    // two other things were wrong: the libnet pool change had throughput at
    // 12 Mbps median instead of 25, and preroll was ending at 33% of the ring
    // because the audio queue hit its slot limit.  Both are fixed, and a
    // bitrate setting is a CEILING not a constant -- a 30 Mbps film sits well
    // under 30 most of the time and spikes on demanding scenes, which is
    // exactly what a cushion is for. It has never been tried on a healthy
    // build with a full buffer.  Labelled as a test so nobody mistakes it for
    // a recommendation until the numbers say so.
    VQ_1080P_30,    // 30 Mbps -- "30 Mbps (test)"
};
const int VQUALITY_ORDER_N = (int)(sizeof(VQUALITY_ORDER) / sizeof(VQUALITY_ORDER[0]));

vquality_t vquality_sanitize(int v) {
    if (v < VQ_AUTO || v >= VQ_COUNT) return VQ_AUTO;
    // Original sat far past the receive ceiling -- 53 Mbps sustained against a
    // console that pulls ~25 is a permanent deficit, not a spike to bridge --
    // so it stays retired and lands on the best step that works.  30 is on
    // trial again and is offered, so it is NOT mapped away.
    if (v == VQ_ORIGINAL) return VQ_1080P_25;
    return (vquality_t)v;
}

// Index of q in the offered list, or -1.  Used only for cycling.
static int order_index(vquality_t q) {
    for (int i = 0; i < VQUALITY_ORDER_N; i++)
        if (VQUALITY_ORDER[i] == q) return i;
    return -1;
}

vquality_t vquality_get(void) { return s_q; }

void vquality_set(vquality_t q) {
    s_q = vquality_sanitize((int)q);
    vquality_save();
}

void vquality_next(int delta) {
    int i = order_index(s_q);
    if (i < 0) i = 0;                       // retired value: start from Auto
    i += delta;
    if (i < 0)                  i = VQUALITY_ORDER_N - 1;
    if (i >= VQUALITY_ORDER_N)  i = 0;
    vquality_set(VQUALITY_ORDER[i]);
}

const char *vquality_label(vquality_t q) {
    switch (q) {
    // The three 1080p steps differ only in ceiling, so naming them by rank
    // reads as the ladder it is.  The row prints the Mbps alongside.
    case VQ_1080P:    return "High";
    case VQ_1080P_20: return "Very High";
    case VQ_1080P_25: return "Max";
    // On trial -- see VQUALITY_ORDER.  Named for what it is rather than given
    // a rank, so it does not read as a recommendation.
    case VQ_1080P_30: return "30 Mbps (test)";
    // Retired -- unreachable through the UI, kept so a stale saved digit
    // that slipped past sanitising still prints as itself rather than "Auto".
    case VQ_ORIGINAL: return "Original";
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
    case VQ_ORIGINAL:
        // Direct play.  The frame size still says 1920x1080 because the
        // jitter buffer has to be sized for something, and because MaxWidth/
        // MaxHeight double as the SAFETY GATE: a 4K source exceeds them, so
        // the server transcodes it down rather than copying something this
        // console cannot decode.  A 1080p-or-smaller source fits and is
        // copied untouched.
        w = 1920; h = 1080; br = 0;         // 0 => no ceiling, allow copy
        profile = "high"; level = "42";
        break;
    case VQ_1080P_20:
        w = 1920; h = 1080; br = 20000000u;
        profile = "high"; level = "42";
        break;
    case VQ_1080P_30:
        w = 1920; h = 1080; br = 30000000u;
        profile = "high"; level = "42";
        break;
    case VQ_1080P_25:
        w = 1920; h = 1080; br = 25000000u;
        profile = "high"; level = "42";
        break;
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
    if (q != VQ_1080P && q != VQ_1080P_20 && q != VQ_1080P_25 &&
        q != VQ_1080P_30 &&
        q != VQ_ORIGINAL) {
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
// -------------------------------------------------------------------------
//  Per-title memory
// -------------------------------------------------------------------------
//  A heavy Blu-ray remux and a small episode do not want the same setting,
//  and re-picking it every time is exactly the sort of chore that makes
//  people leave it on the wrong one.  So the choice is remembered against
//  the item it was made for, and re-applied when that item is opened.
//
//  Deliberately simple: one line per title, newest first, capped at
//  VQ_ITEMS_MAX and rewritten whole.  At 64 entries that is ~2 KB, which is
//  not worth a smarter structure, and dropping the oldest entry costs a
//  user nothing but one re-pick on a title they have not touched in a long
//  time.
#define VQ_ITEMS_FILE "jellyfin_vq_items.txt"
#define VQ_ITEMS_MAX  64
#define VQ_ID_LEN     40

int vquality_for_item(const char *item_id) {
    if (!item_id || !item_id[0]) return -1;
    FILE *f = fopen(jf_data_path(VQ_ITEMS_FILE), "r");
    if (!f) return -1;
    char id[VQ_ID_LEN]; int q; int found = -1;
    while (fscanf(f, "%39s %d", id, &q) == 2) {
        if (strcmp(id, item_id) == 0 && q >= VQ_AUTO && q < VQ_COUNT) {
            // A title remembered on a retired step must not resurrect it.
            found = (int)vquality_sanitize(q);
            break;
        }
    }
    fclose(f);
    return found;
}

void vquality_remember_item(const char *item_id, vquality_t q) {
    if (!item_id || !item_id[0]) return;
    if (q < VQ_AUTO || q >= VQ_COUNT) return;
    if (strlen(item_id) >= VQ_ID_LEN) return;

    static char ids[VQ_ITEMS_MAX][VQ_ID_LEN];
    static int  qs [VQ_ITEMS_MAX];
    int n = 0;

    // This item goes first, so the cap drops the least recently chosen.
    snprintf(ids[n], VQ_ID_LEN, "%s", item_id);
    qs[n] = (int)q;
    n++;

    FILE *f = fopen(jf_data_path(VQ_ITEMS_FILE), "r");
    if (f) {
        char id[VQ_ID_LEN]; int old;
        while (n < VQ_ITEMS_MAX && fscanf(f, "%39s %d", id, &old) == 2) {
            if (strcmp(id, item_id) == 0) continue;   // superseded above
            if (old < VQ_AUTO || old >= VQ_COUNT) continue;
            snprintf(ids[n], VQ_ID_LEN, "%s", id);
            qs[n] = old;
            n++;
        }
        fclose(f);
    }

    f = fopen(jf_data_path(VQ_ITEMS_FILE), "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s %d\n", ids[i], qs[i]);
    fclose(f);
}
void vquality_load(void) {
    FILE *f = fopen(jf_data_path(VQUALITY_FILE), "r");
    if (!f) return;
    int v = 0;
    if (fscanf(f, "%d", &v) == 1)
        s_q = vquality_sanitize(v);
    fclose(f);
}

void vquality_save(void) {
    FILE *f = fopen(jf_data_path(VQUALITY_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)s_q);
    fclose(f);
}
