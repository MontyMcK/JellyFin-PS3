// See display_mode.h.  No PS3 headers: this file is compiled by the host
// suite exactly as the console compiles it.
#include "display_mode.h"

#include <stdio.h>
#include <string.h>

int dm_display_rate(uint16_t bits, uint32_t *num, uint32_t *den)
{
    // Precedence copied from timing_init(); keep the two identical.
    if      (bits & DM_RATE_59_94) { *num = 60000; *den = 1001; return 1; }
    else if (bits & DM_RATE_50)    { *num = 50;    *den = 1;    return 1; }
    else if (bits & DM_RATE_60)    { *num = 60;    *den = 1;    return 1; }
    else if (bits & DM_RATE_30)    { *num = 30;    *den = 1;    return 1; }
    *num = 60000; *den = 1001;
    return 0;
}

void dm_rates_str(uint16_t bits, char *out, unsigned outsz)
{
    static const struct { uint16_t bit; const char *name; } k[] = {
        { DM_RATE_59_94,   "59.94" },
        { DM_RATE_50,      "50" },
        { DM_RATE_60,      "60" },
        { DM_RATE_30,      "30" },
        { DM_RATE_24FAM_A, "24fam(0x10)" },
        { DM_RATE_24FAM_B, "24fam(0x20)" },
        { DM_RATE_UNK_40,  "unk(0x40)" },
    };
    unsigned n = 0;
    uint16_t left = bits;
    if (outsz == 0) return;
    out[0] = '\0';
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (!(bits & k[i].bit)) continue;
        left &= (uint16_t)~k[i].bit;
        int w = snprintf(out + n, outsz - n, "%s%s", n ? "|" : "", k[i].name);
        if (w < 0 || (unsigned)w >= outsz - n) return;
        n += (unsigned)w;
    }
    if (left)
        snprintf(out + n, outsz - n, "%sother(0x%04x)", n ? "|" : "", left);
    else if (n == 0)
        snprintf(out, outsz, "none");
}

dm_film dm_classify_film(uint32_t fps_num, uint32_t fps_den)
{
    if (fps_den == 0) return DM_FILM_NONE;
    // Exact fractions only.  23976/1000 is deliberately NOT film here: the
    // server rate is snapped to 24000/1001 before it ever reaches timing, so an
    // unsnapped value means something upstream changed and should be seen.
    if ((uint64_t)fps_num * 1001 == (uint64_t)fps_den * 24000) return DM_FILM_23976;
    if ((uint64_t)fps_num == (uint64_t)fps_den * 24)           return DM_FILM_24;
    return DM_FILM_NONE;
}

int dm_fps_confident(dm_fps_source src)
{
    return src == DM_FPS_VDEC_FRC || src == DM_FPS_SERVER;
}

const char *dm_fps_source_name(dm_fps_source src)
{
    switch (src) {
    case DM_FPS_VDEC_FRC: return "vdec_frc";
    case DM_FPS_SERVER:   return "server_rate";
    case DM_FPS_DEFAULT:  return "default_guess";
    case DM_FPS_TIMEOUT:  return "timeout_guess";
    }
    return "?";
}

dm_support dm_display_24p_support(const dm_mode *modes, int n, uint8_t res)
{
    if (!modes || n < 0) return DM_SUPPORT_UNKNOWN;
    int listed = 0;
    for (int i = 0; i < n; i++) {
        if (modes[i].res != res) continue;
        listed = 1;
        if (modes[i].rates & DM_RATE_24FAM) return DM_SUPPORT_YES;
    }
    // Listed without the bits is a firm "no".  Not listed at all is odd (we are
    // presumably running in that resolution) -- call it unknown, not no.
    return listed ? DM_SUPPORT_NO : DM_SUPPORT_UNKNOWN;
}

const char *dm_support_name(dm_support s)
{
    switch (s) {
    case DM_SUPPORT_UNKNOWN: return "unknown";
    case DM_SUPPORT_NO:      return "no";
    case DM_SUPPORT_YES:     return "advertised";
    }
    return "?";
}

void dm_cadence_str(uint32_t disp_num, uint32_t disp_den,
                    uint32_t fps_num, uint32_t fps_den,
                    char *out, unsigned outsz)
{
    // Vblanks per content frame = (disp_num/disp_den) / (fps_num/fps_den)
    //                           = (disp_num*fps_den) / (disp_den*fps_num)
    const uint64_t a = (uint64_t)disp_num * fps_den;
    const uint64_t b = (uint64_t)disp_den * fps_num;
    if (b == 0 || a == 0) { snprintf(out, outsz, "unknown"); return; }
    if (a % b == 0) {
        const uint64_t k = a / b;
        if (k == 1) snprintf(out, outsz, "1:1");
        else        snprintf(out, outsz, "%llu:%llu", (unsigned long long)k,
                             (unsigned long long)k);
        return;
    }
    if ((2 * a) % b == 0) {
        // Half-integer: alternate n+1 and n vblanks.  2.5 is film 3:2 on 59.94.
        const uint64_t k2 = 2 * a / b;          // odd
        snprintf(out, outsz, "%llu:%llu pulldown",
                 (unsigned long long)(k2 / 2 + 1), (unsigned long long)(k2 / 2));
        return;
    }
    const uint64_t milli = (a * 1000 + b / 2) / b;
    snprintf(out, outsz, "uneven x%llu.%03llu",
             (unsigned long long)(milli / 1000), (unsigned long long)(milli % 1000));
}

dm_decision dm_decide(dm_film film, int fps_confident,
                      uint32_t width, uint32_t height,
                      uint8_t display_res, uint16_t display_rates,
                      int progressive, dm_support support, int enabled)
{
    dm_decision d;
    memset(&d, 0, sizeof(d));
    d.support = support;
    d.path    = "none";
    d.attempt = 0;
    d.result  = "not_attempted";

    if (film == DM_FILM_NONE) {
        d.candidate_why = "content is not 24fps-family";
    } else if (!fps_confident) {
        d.candidate_why = "frame rate was guessed, not detected";
    } else if (display_res != DM_RES_1080) {
        // 720p-only and SD setups must behave exactly as today.
        d.candidate_why = "output is not 1920x1080";
    } else if (!progressive) {
        d.candidate_why = "output is interlaced";
    } else if (width < 1280 || height < 720) {
        d.candidate_why = "content below 720p";
    } else if (display_rates & DM_RATE_24FAM) {
        d.candidate_why = "output already reports a 24Hz-family rate";
    } else {
        d.candidate     = 1;
        d.candidate_why = "24fps film on a non-24Hz 1080p output";
    }

    if (!d.candidate) {
        d.result_why = "not a candidate";
    } else if (support != DM_SUPPORT_YES) {
        d.result_why = "display does not advertise 24Hz for 1080";
    } else if (!enabled) {
        d.result_why = "24p output is off (jellyfin_24p.txt)";
    } else {
        d.path       = "cellVideoOutConfigure2";
        d.attempt    = 1;
        d.result     = "pending";
        d.result_why = "switching; verified by vblank timing";
    }
    return d;
}

static const struct { dm_clock c; uint32_t num, den; const char *name; } kClocks[] = {
    { DM_CLK_23976, 24000, 1001, "23.976" },
    { DM_CLK_24,    24,    1,    "24.000" },
    { DM_CLK_25,    25,    1,    "25.000" },
    { DM_CLK_2997,  30000, 1001, "29.970" },
    { DM_CLK_30,    30,    1,    "30.000" },
    { DM_CLK_50,    50,    1,    "50.000" },
    { DM_CLK_5994,  60000, 1001, "59.940" },
    { DM_CLK_60,    60,    1,    "60.000" },
};

dm_clock dm_classify_period(uint64_t period_ns, uint32_t *num, uint32_t *den)
{
    if (period_ns == 0) return DM_CLK_UNKNOWN;
    for (unsigned i = 0; i < sizeof(kClocks) / sizeof(kClocks[0]); i++) {
        // nominal period in ns = 1e9 * den / num; compare in ppm without floats:
        // |period*num - 1e9*den| <= 250e-6 * 1e9*den
        const int64_t ideal = (int64_t)1000000000 * kClocks[i].den;
        const int64_t got   = (int64_t)period_ns * kClocks[i].num;
        const int64_t diff  = got > ideal ? got - ideal : ideal - got;
        if (diff * 4000 <= ideal) {                 // <= 250 ppm
            if (num) *num = kClocks[i].num;
            if (den) *den = kClocks[i].den;
            return kClocks[i].c;
        }
    }
    return DM_CLK_UNKNOWN;
}

const char *dm_clock_name(dm_clock c)
{
    for (unsigned i = 0; i < sizeof(kClocks) / sizeof(kClocks[0]); i++)
        if (kClocks[i].c == c) return kClocks[i].name;
    return "unknown";
}
