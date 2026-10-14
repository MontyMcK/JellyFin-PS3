// Host tests for JellyWave 2.0's scope (wave_scope.h) and shape deformation
// (wave_deform.h).  libm is fine here; the kernels may not use it.

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "wave_scope.h"
#include "wave_deform.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

#define RATE 48000
#define BLK  800                /* one 60 fps frame of audio */
#define DT   (1.0f / 60.0f)
#define N    72                 /* WF_SAMPLES */

static float buf[BLK * 2];
static long  t_abs = 0;

// kind: 0 mono sine, 1 right only, 2 left only, 3 anti-phase
static void gen(int kind, float amp, float hz)
{
    int i;
    for (i = 0; i < BLK; i++) {
        const float v = amp * sinf(2.0f * 3.14159265f * hz * (float)(t_abs + i) / RATE);
        float l = v, r = v;
        if (kind == 1) l = 0.0f;
        if (kind == 2) r = 0.0f;
        if (kind == 3) r = -v;
        buf[2 * i] = l; buf[2 * i + 1] = r;
    }
    t_abs += BLK;
}

static void run(wsc_state *s, wsc_out *o, int kind, float amp, float hz, int frames, float present)
{
    int f;
    for (f = 0; f < frames; f++) {
        gen(kind, amp, hz);
        wsc_push(s, buf, BLK);
        wsc_frame(s, DT, present, o);
    }
}

static float wave_peak(const wsc_out *o)
{
    float p = 0.0f; int i;
    for (i = 0; i < WSC_POINTS; i++) if (fabsf(o->wave[i]) > p) p = fabsf(o->wave[i]);
    return p;
}

static void test_scope(void)
{
    static wsc_state s;
    wsc_out o;
    float quiet, loud;
    int i;

    wsc_init(&s);
    run(&s, &o, 1, 0.5f, 220.0f, 60, 1.0f);
    printf("  scope: right only -> balance %+.2f\n", o.balance);
    CHECK(o.balance > 0.8f, "right-only audio does not lean right (%.2f)", o.balance);
    run(&s, &o, 2, 0.5f, 220.0f, 90, 1.0f);
    printf("  scope: left only  -> balance %+.2f\n", o.balance);
    CHECK(o.balance < -0.8f, "left-only audio does not lean left (%.2f)", o.balance);
    run(&s, &o, 0, 0.5f, 220.0f, 90, 1.0f);
    printf("  scope: mono       -> balance %+.2f width %.2f\n", o.balance, o.width);
    CHECK(fabsf(o.balance) < 0.1f && o.width < 0.05f, "mono is not centred and narrow");
    run(&s, &o, 3, 0.5f, 220.0f, 90, 1.0f);
    printf("  scope: anti-phase -> width %.2f\n", o.width);
    CHECK(o.width > 0.8f, "anti-phase audio does not read as wide (%.2f)", o.width);

    // the waveform: bounded, tapered to 0 at both ends, level-independent
    run(&s, &o, 0, 0.05f, 180.0f, 600, 1.0f);   /* the normaliser falls slowly by design */
    for (quiet = 0.0f, i = 0; i < 30; i++) {       /* peak over half a second: */
        run(&s, &o, 0, 0.05f, 180.0f, 1, 1.0f);   /* one frame's depends on   */
        if (wave_peak(&o) > quiet) quiet = wave_peak(&o);   /* the taper phase */
    }
    run(&s, &o, 0, 0.9f, 180.0f, 150, 1.0f);
    for (loud = 0.0f, i = 0; i < 30; i++) {
        run(&s, &o, 0, 0.9f, 180.0f, 1, 1.0f);
        if (wave_peak(&o) > loud) loud = wave_peak(&o);
    }
    printf("  scope: waveform peak quiet %.2f loud %.2f, ends %.3f %.3f\n",
           quiet, loud, o.wave[0], o.wave[WSC_POINTS - 1]);
    CHECK(loud > 0.2f && loud <= 1.0f, "waveform peak %.2f out of range", loud);
    CHECK(fabsf(quiet - loud) < 0.15f, "waveform size follows loudness (%.2f vs %.2f)", quiet, loud);
    CHECK(o.wave[0] == 0.0f && o.wave[WSC_POINTS - 1] == 0.0f, "waveform ends are not pinned");

    // pitch-synchronous: a held 173 Hz note (not a divisor of the frame, so
    // its phase is different every frame) draws the same shape every frame
    {
        float prev[WSC_POINTS], dot = 0.0f, na = 0.0f, nb = 0.0f;
        int f2;
        run(&s, &o, 0, 0.5f, 173.0f, 60, 1.0f);
        memcpy(prev, o.wave, sizeof prev);
        for (f2 = 0; f2 < 30; f2++) {
            run(&s, &o, 0, 0.5f, 173.0f, 1, 1.0f);
            for (i = 0; i < WSC_POINTS; i++) { dot += o.wave[i] * prev[i]; na += o.wave[i] * o.wave[i]; nb += prev[i] * prev[i]; }
            memcpy(prev, o.wave, sizeof prev);
        }
        printf("  scope: held 173 Hz note -> period %d samples (%.0f Hz), frame-to-frame similarity %.3f\n",
               s.period, s.period ? 12000.0f / s.period : 0.0f, dot / sqrtf(na * nb + 1e-12f));
        CHECK(s.period > 0 && fabsf(12000.0f / s.period - 173.0f) < 12.0f, "period not found (%d)", s.period);
        CHECK(dot / sqrtf(na * nb + 1e-12f) > 0.98f, "a held note's shape does not hold still");
    }

    // silence (present 0): everything eases back to neutral
    run(&s, &o, 0, 0.0f, 180.0f, 120, 0.0f);
    printf("  scope: silence    -> balance %+.3f width %.3f wave %.3f\n", o.balance, o.width, wave_peak(&o));
    CHECK(fabsf(o.balance) < 0.01f && o.width < 0.01f && wave_peak(&o) < 0.01f,
          "silence does not return the scope to neutral");

    // hostile input stays finite
    for (i = 0; i < BLK * 2; i++) buf[i] = (i % 3) ? NAN : INFINITY;
    wsc_push(&s, buf, BLK);
    wsc_frame(&s, DT, 1.0f, &o);
    CHECK(o.balance == o.balance && o.width == o.width, "NaN/inf in -> NaN out");
    for (i = 0; i < WSC_POINTS; i++) CHECK(o.wave[i] == o.wave[i], "NaN waveform point %d", i);
}

static void in_rest(wdf_in *in, const wsc_out *sc)
{
    memset(in, 0, sizeof *in);
    in->scope = sc;
}

static void test_deform(void)
{
    static wsc_out sc;
    wdf_state st;
    wdf_look  d;
    wdf_in    in;
    float disp[N], ref[N];
    int i, l, f;

    // rest is rest: nothing live, the curve is untouched bit for bit
    memset(&st, 0, sizeof st); st.rim = 1.0f;
    memset(&sc, 0, sizeof sc);
    in_rest(&in, &sc);
    for (f = 0; f < 10; f++) wdf_map(&st, &in, DT, &d);
    for (i = 0; i < N; i++) disp[i] = ref[i] = 0.1f * sinf((float)i);
    for (l = 0; l < 3; l++) wdf_apply(&d, l, disp, N);
    CHECK(!d.live && d.rim == 1.0f, "rest is live (%d) or rim %.3f", d.live, d.rim);
    CHECK(memcmp(disp, ref, sizeof disp) == 0, "rest changes the curve");

    // everything at full: every sample inside the framing allowance
    {
        float lo = 1.0f, hi = -1.0f;
        for (i = 0; i < WSC_POINTS; i++) sc.wave[i] = (i & 1) ? 1.0f : -1.0f;
        sc.balance = 1.0f; sc.width = 1.0f;
        in_rest(&in, &sc);
        in.present = 1.0f; in.tempo_hz = 3.0f; in.tempo_conf = 1.0f;
        for (i = 0; i < 3; i++) { in.lvl[i] = 1.0f; in.punch[i] = 1.0f; }
        for (f = 0; f < 600; f++) {
            in.onset = (f % 20) == 0; in.onset_strength = 1.0f;
            wdf_map(&st, &in, DT, &d);
            for (l = 0; l < 3; l++) {
                memset(disp, 0, sizeof disp);
                wdf_apply(&d, l, disp, N);
                for (i = 0; i < N; i++) {
                    const float a = disp[i] / WDF_LAYER[l];
                    if (a > hi) hi = a;
                    if (a < lo) lo = a;
                }
            }
        }
        printf("  deform: full drive -> per-sample range %+.4f .. %+.4f (allowance %+.3f)\n",
               lo, hi, WDF_POS_MAX);
        CHECK(hi <= WDF_POS_MAX + 1e-6f && lo >= -WDF_NEG_MAX - 1e-6f, "deformation escapes its allowance");
        CHECK(d.rim > 1.3f && d.rim <= 1.0f + WDF_RIM_GAIN + 1e-4f, "treble rim gain %.2f", d.rim);
    }

    // the beat pulse swells and settles: not in one frame, gone within a second
    {
        float p[90], peak = 0.0f; int at = 0;
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        in_rest(&in, &sc);
        in.present = 1.0f;
        for (f = 0; f < 90; f++) {
            in.onset = (f == 5); in.onset_strength = 1.0f;
            wdf_map(&st, &in, DT, &d);
            p[f] = d.pulse;
            if (p[f] > peak) { peak = p[f]; at = f; }
        }
        printf("  deform: beat pulse first frame %.4f, peak %.4f at +%d frames, after 1 s %.4f\n",
               p[5], peak, at - 5, p[65]);
        CHECK(peak > 0.5f * WDF_PULSE_A, "beat pulse too weak (%.4f)", peak);
        CHECK(p[5] < 0.6f * peak, "beat pulse snaps instead of swelling");
        CHECK(p[65] < 0.1f * peak, "beat pulse lingers");
    }

    // stereo: right-heavy audio deforms the right half more than the left
    {
        float left = 0.0f, right = 0.0f;
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        sc.balance = 1.0f;
        in_rest(&in, &sc);
        in.present = 1.0f; in.lvl[1] = 0.8f;
        for (f = 0; f < 120; f++) wdf_map(&st, &in, DT, &d);
        memset(disp, 0, sizeof disp);
        wdf_apply(&d, 0, disp, N);
        for (i = 0; i < N; i++) { if (i < N / 2) left += disp[i]; else right += disp[i]; }
        printf("  deform: balance right -> mean left %+.4f right %+.4f\n", left / (N / 2), right / (N / 2));
        CHECK(right > left + 0.1f, "stereo does not lean the band");
    }

    // mids drive the ripple, and it travels
    {
        float a_lo, a_hi; int k0, k1, best; float bv;
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        in_rest(&in, &sc);
        in.present = 1.0f; in.lvl[1] = 0.0f;
        for (f = 0; f < 120; f++) wdf_map(&st, &in, DT, &d);
        a_lo = d.mid_a;
        in.lvl[1] = 1.0f; in.punch[1] = 1.0f;
        for (f = 0; f < 120; f++) wdf_map(&st, &in, DT, &d);
        a_hi = d.mid_a;
        memset(disp, 0, sizeof disp); wdf_apply(&d, 1, disp, N);
        for (best = 0, bv = -9.0f, i = 0; i < N; i++) if (disp[i] > bv) { bv = disp[i]; best = i; }
        k0 = best;
        for (f = 0; f < 30; f++) wdf_map(&st, &in, DT, &d);
        memset(disp, 0, sizeof disp); wdf_apply(&d, 1, disp, N);
        for (best = 0, bv = -9.0f, i = 0; i < N; i++) if (disp[i] > bv) { bv = disp[i]; best = i; }
        k1 = best;
        printf("  deform: ripple amp quiet mids %.4f loud mids %.4f; crest moved %d -> %d in 0.5 s\n",
               a_lo, a_hi, k0, k1);
        CHECK(a_hi > 2.0f * a_lo, "mids do not drive the ripple");
        CHECK(k0 != k1, "the ripple does not travel");
    }
}

// energy of one layer's deformation
static float def_rms(const wdf_look *d, int l)
{
    float disp[N], e = 0.0f; int i;
    memset(disp, 0, sizeof disp);
    wdf_apply(d, l, disp, N);
    for (i = 0; i < N; i++) e += disp[i] * disp[i];
    return sqrtf(e / N);
}

static void test_organism(void)
{
    static wsc_out sc;
    wdf_state st;
    wdf_look  d;
    wdf_in    in;
    int f, i, l;

    // deformation continues after the audio peak: one hit, then nothing
    {
        float peak = 0.0f, later;
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        memset(&in, 0, sizeof in); in.scope = &sc; in.present = 1.0f;
        for (f = 0; f < 60; f++) {
            const int hit = (f >= 10 && f < 16);
            in.lvl[0] = hit ? 1.0f : 0.0f; in.punch[0] = hit ? 1.0f : 0.0f;
            in.onset = (f == 10); in.onset_strength = 1.0f;
            wdf_map(&st, &in, DT, &d);
            if (f <= 16 && def_rms(&d, 0) > peak) peak = def_rms(&d, 0);
        }
        later = 0.0f;
        for (f = 0; f < 6; f++) { wdf_map(&st, &in, DT, &d); if (def_rms(&d, 0) > later) later = def_rms(&d, 0); }
        wdf_map(&st, &in, DT, &d);
        printf("  organism: hit peak %.4f, 0.8 s after the audio stopped %.4f (swell %+.4f)\n",
               peak, later, d.swell_a);
        CHECK(later > 0.25f * peak, "the deformation dies with the audio (%.4f vs %.4f)", later, peak);
    }

    // nonlinear: full level deforms far more than twice half level
    {
        float half, full;
        for (l = 0; l < 2; l++) {
            const float v = l ? 1.0f : 0.5f;
            float acc = 0.0f;
            memset(&st, 0, sizeof st); st.rim = 1.0f;
            memset(&sc, 0, sizeof sc);
            memset(&in, 0, sizeof in); in.scope = &sc; in.present = 1.0f;
            for (i = 0; i < 3; i++) { in.lvl[i] = v; in.punch[i] = 0.0f; }
            for (f = 0; f < 600; f++) {
                wdf_map(&st, &in, DT, &d);
                if (f >= 300) acc += def_rms(&d, 1);
            }
            if (l) full = acc; else half = acc;
        }
        printf("  organism: deformation at half level %.4f, full level %.4f (x%.2f)\n",
               half / 300, full / 300, full / half);
        CHECK(full > 2.4f * half, "the response is not dramatic enough when loud (x%.2f)", full / half);
    }

    // a beat warps the layers one after another (volume), near first
    {
        int at[3] = { -1, -1, -1 };
        float best[3] = { 0, 0, 0 };
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        memset(&in, 0, sizeof in); in.scope = &sc; in.present = 1.0f;
        for (i = 0; i < 3; i++) in.lvl[i] = 0.6f;
        for (f = 0; f < 120; f++) wdf_map(&st, &in, DT, &d);   // settle the drama
        for (f = 0; f < 60; f++) {
            in.onset = (f == 0); in.onset_strength = 1.0f;
            wdf_map(&st, &in, DT, &d);
            for (l = 0; l < 3; l++) {
                const float w = wdf_warp_at(&d, 0, l, d.warp_x[0]) / WDF_LAYER[l];
                if (w > best[l]) { best[l] = w; at[l] = f; }
            }
        }
        printf("  organism: warp peaks at frame %d / %d / %d (near / mid / far)\n", at[0], at[1], at[2]);
        CHECK(at[0] < at[1] && at[1] < at[2], "the beat does not travel through the layers");
    }

    // evolution: steady audio, and the wave's character still changes
    {
        float k_lo = 9, k_hi = 0, g_lo = 9, g_hi = 0, a[N], b[N], dot = 0, na = 0, nb = 0;
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        memset(&in, 0, sizeof in); in.scope = &sc; in.present = 1.0f;
        for (i = 0; i < 3; i++) in.lvl[i] = 0.7f;
        for (f = 0; f < 60 * 600; f++) {
            wdf_map(&st, &in, DT, &d);
            if (f > 600) {
                if (d.k1m[1] < k_lo) k_lo = d.k1m[1];
                if (d.k1m[1] > k_hi) k_hi = d.k1m[1];
                if (d.gain < g_lo) g_lo = d.gain;
                if (d.gain > g_hi) g_hi = d.gain;
            }
            if (f == 60 * 60) { memset(a, 0, sizeof a); wdf_apply(&d, 1, a, N); }
            if (f == 60 * 60 + 60 * 27) { memset(b, 0, sizeof b); wdf_apply(&d, 1, b, N); }
        }
        for (i = 0; i < N; i++) { dot += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }
        printf("  organism: over 10 min of steady audio -- wavelength x%.2f..x%.2f, strength %.3f..%.3f, "
               "shape correlation after 27 s %.2f\n", k_lo, k_hi, g_lo, g_hi, dot / sqrtf(na * nb + 1e-12f));
        CHECK(k_hi - k_lo > 0.3f && k_lo > 0.7f && k_hi < 1.35f, "the wavelength does not evolve within bounds");
        CHECK(g_hi / g_lo > 1.3f, "the strength does not evolve");
        CHECK(dot / sqrtf(na * nb + 1e-12f) < 0.9f, "the wave repeats itself");
    }

    // the glow: none at rest, and after a beat a pulse runs along the band
    {
        float gain[80], g0 = 0, g1 = 0; int p0 = 0, p1 = 0;
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        memset(&in, 0, sizeof in); in.scope = &sc;
        wdf_map(&st, &in, DT, &d);
        CHECK(wdf_glow(&d, 0, gain, 80) == 0, "glow at rest");
        in.present = 1.0f;
        for (f = 0; f < 40; f++) {
            in.onset = (f == 0); in.onset_strength = 1.0f;
            wdf_map(&st, &in, DT, &d);
            if (f == 8 || f == 30) {
                float best = 0; int at = 0;
                wdf_glow(&d, 0, gain, 80);
                for (i = 0; i < 80; i++) if (gain[i] > best) { best = gain[i]; at = i; }
                if (f == 8) { g0 = best; p0 = at; } else { g1 = best; p1 = at; }
            }
        }
        printf("  organism: glow %.2f at station %d, then %.2f at station %d\n", g0, p0, g1, p1);
        CHECK(g0 > 1.1f && g0 <= 1.0f + WDF_GLOW_A + WDF_SHIMMER_A + 1e-4f, "glow strength %.2f", g0);
        CHECK(p0 != p1, "the glow does not travel");
    }
}

static void test_features(void)
{
    static wsc_out sc;
    wdf_state st;
    wdf_look  d;
    wdf_in    in;
    int f, blooms = 0;
    float s_lo = 9, s_hi = -9;

    // tint follows the brightness, slowly, and is 0 at rest
    memset(&st, 0, sizeof st); st.rim = 1.0f;
    memset(&sc, 0, sizeof sc);
    sc.key = -1;                          /* no tonal centre: brightness alone */
    memset(&in, 0, sizeof in); in.scope = &sc;
    wdf_map(&st, &in, DT, &d);
    CHECK(d.tint == 0.0f, "tint at rest");
    in.present = 1.0f; in.centroid = 0.9f;
    for (f = 0; f < 240; f++) wdf_map(&st, &in, DT, &d);
    {
        const float bright = d.tint;
        in.centroid = 0.1f;
        wdf_map(&st, &in, DT, &d);
        const float one = d.tint;
        for (f = 0; f < 240; f++) wdf_map(&st, &in, DT, &d);
        printf("  features: tint bright %+.2f, one frame after going dark %+.2f, settled dark %+.2f\n",
               bright, one, d.tint);
        CHECK(bright > 0.6f && d.tint < -0.6f, "tint does not follow the brightness");
        CHECK(one > bright - 0.1f, "tint jumps instead of easing");
    }

    // sections: 20 s calm, then a drop; then 30 s steady
    memset(&st, 0, sizeof st); st.rim = 1.0f;
    memset(&in, 0, sizeof in); in.scope = &sc; in.present = 1.0f;
    for (f = 0; f < 60 * 60; f++) {
        const float v = f < 60 * 20 ? 0.15f : 0.75f;
        in.lvl[0] = in.lvl[1] = in.lvl[2] = v;
        in.punch[0] = in.punch[1] = in.punch[2] = f < 60 * 20 ? 0.0f : 0.4f;
        wdf_map(&st, &in, DT, &d);
        if (d.bloom > 0.99f) blooms++;
        if (f > 60 * 5 && f < 60 * 20 && d.section < s_lo) s_lo = d.section;
        if (f > 60 * 20 && f < 60 * 25 && d.section > s_hi) s_hi = d.section;
    }
    printf("  features: calm -> drop: section %+.2f then %+.2f, blooms %d, section after 40 s steady %+.2f\n",
           s_lo, s_hi, blooms, d.section);
    CHECK(blooms == 1, "a drop gave %d blooms", blooms);
    CHECK(s_hi > 0.5f, "the drop does not read as a peak");
    CHECK(d.section > -0.2f && d.section < 0.3f, "a steady song does not settle to neutral");
    {   // and a steady song never blooms
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        blooms = 0;
        for (f = 0; f < 60 * 60; f++) { wdf_map(&st, &in, DT, &d); if (d.bloom > 0.99f) blooms++; }
        CHECK(blooms == 0, "a steady song bloomed %d times", blooms);
    }
}

static void test_beat_calm(void)
{
    static wsc_out sc;
    wdf_state st;
    wdf_look  d;
    wdf_in    in;
    int f, n = 0, na = 0;
    float err = 0.0f, adv = 0.0f, prev = -1.0f;

    // beat lock: 120 BPM, trusted, an onset every 30 frames -- the ripple
    // advances WDF_BEAT_WL per beat and sits on the grid at each beat
    memset(&st, 0, sizeof st); st.rim = 1.0f;
    memset(&sc, 0, sizeof sc);
    memset(&in, 0, sizeof in); in.scope = &sc; in.present = 1.0f;
    in.tempo_hz = 2.0f; in.tempo_conf = 1.0f;
    for (f = 0; f < 60 * 20; f++) {
        in.onset = (f % 30) == 0; in.onset_strength = 0.8f;
        in.lvl[0] = 0.6f; in.punch[0] = (f % 30) < 4 ? 0.8f : 0.0f;
        wdf_map(&st, &in, DT, &d);
        if (f > 60 * 12 && (f % 30) == 0) {
            float q = d.mid_ph / WDF_BEAT_WL, e = q - floorf(q + 0.5f);
            err += fabsf(e); n++;
            if (prev >= 0.0f) { adv += fmodf(d.mid_ph - prev + 1.0f, 1.0f); na++; }
            prev = d.mid_ph;
        }
    }
    printf("  beat lock: ripple %.3f of a wavelength per beat (want %.2f), crest-to-beat error %.3f beats\n",
           adv / (na ? na : 1), WDF_BEAT_WL, err / (n ? n : 1));
    CHECK(fabsf(adv / (na ? na : 1) - WDF_BEAT_WL) < 0.05f, "the ripple does not travel in time with the beat");
    CHECK(err / (n ? n : 1) < 0.08f, "the ripple does not land on the beat");

    // vocal calm: mids alone, no beat -> calm; drums back -> gone quickly
    memset(&st, 0, sizeof st); st.rim = 1.0f;
    memset(&in, 0, sizeof in); in.scope = &sc; in.present = 1.0f;
    in.lvl[1] = 0.7f; in.lvl[0] = 0.1f; in.lvl[2] = 0.1f;
    in.bass_rel = 0.30f; in.mid_rel = 0.95f;          // the bass has dropped out
    for (f = 0; f < 60 * 5; f++) wdf_map(&st, &in, DT, &d);
    {
        const float calm = d.calm, g_calm = d.gain;
        float g_drums;
        in.lvl[0] = 0.7f; in.lvl[2] = 0.5f;
        in.bass_rel = 0.95f;                          // ... and is back
        for (f = 0; f < 60 * 3; f++) {
            in.onset = (f % 30) == 0; in.punch[0] = (f % 30) < 4 ? 0.8f : 0.0f;
            wdf_map(&st, &in, DT, &d);
        }
        g_drums = d.gain;
        printf("  vocal calm: voice alone %.2f, 3 s after the drums return %.2f (gain %.3f -> %.3f)\n",
               calm, d.calm, g_calm, g_drums);
        CHECK(calm > 0.6f, "a vocal-only passage does not calm the wave (%.2f)", calm);
        CHECK(d.calm < 0.15f, "the calm lingers after the drums return (%.2f)", d.calm);
    }
}

static void test_hints(void)
{
    static wsc_state s;
    wsc_out o;
    int f, changes = 0, k0 = -2, k1 = -2;

    // key: 5 s of A (220 Hz) then 6 s of C (261.63 Hz)
    wsc_init(&s);
    for (f = 0; f < 60 * 5; f++) { gen(0, 0.4f, 220.0f); wsc_push(&s, buf, BLK); wsc_frame(&s, DT, 1.0f, &o); changes += o.key_changed; }
    k0 = o.key;
    for (f = 0; f < 60 * 6; f++) { gen(0, 0.4f, 261.63f); wsc_push(&s, buf, BLK); wsc_frame(&s, DT, 1.0f, &o); changes += o.key_changed; }
    k1 = o.key;
    printf("  hints: key A -> %d, then C -> %d, %d change event(s)\n", k0, k1, changes);
    CHECK(k0 == 9 && k1 == 0 && changes == 1, "key tracking (want 9 then 0, one change)");

    // bass glide: a 50 -> 100 Hz slide over 1 s reads as ~ +1 octave/s
    {
        float g = 0.0f, ph = 0.0f; int i;
        wsc_init(&s);
        for (f = 0; f < 90; f++) {
            const float hz = f < 30 ? 50.0f : (f < 90 ? 50.0f * powf(2.0f, (float)(f - 30) / 60.0f) : 100.0f);
            for (i = 0; i < BLK; i++) {
                ph += hz / RATE; if (ph > 1.0f) ph -= 1.0f;
                buf[2 * i] = buf[2 * i + 1] = 0.6f * sinf(2.0f * 3.14159265f * ph);
            }
            wsc_push(&s, buf, BLK); wsc_frame(&s, DT, 1.0f, &o);
            if (f > 60 && f < 88) g += o.bass_glide / 27.0f;
        }
        printf("  hints: 808 slide 50 -> 100 Hz in 1 s reads %.2f octaves/s\n", g);
        CHECK(g > 0.6f && g < 1.4f, "bass glide %.2f", g);
    }

    // hats: air-band hits light glints on the far ribbon's rim only
    {
        wdf_state st; wdf_look d; wdf_in in; float gain[80], best = 1.0f; int i;
        static wsc_out sc;
        memset(&st, 0, sizeof st); st.rim = 1.0f;
        memset(&sc, 0, sizeof sc);
        memset(&in, 0, sizeof in); in.scope = &sc; in.present = 1.0f;
        for (f = 0; f < 120; f++) {
            in.air = (f % 8) < 2 ? 0.8f : 0.3f;          // 16th-note hats
            wdf_map(&st, &in, DT, &d);
        }
        CHECK(wdf_rim_glow(&d, 0, gain, 80) == 0, "hat glints on the near ribbon");
        CHECK(wdf_rim_glow(&d, 1, gain, 80) == 1, "no hat glints on the middle ribbon");
        wdf_rim_glow(&d, 2, gain, 80);
        for (i = 0; i < 80; i++) if (gain[i] > best) best = gain[i];
        printf("  hints: hi-hat rim glint peak x%.2f (limit x%.2f)\n", best, 1.0f + WDF_HAT_RIM);
        CHECK(best > 1.05f && best <= 1.0f + WDF_HAT_RIM + 1e-4f, "hat glint %.2f", best);
    }
}

static void report_cost(void)
{
    static wsc_state s;
    static wsc_out   o;
    wdf_state st; wdf_look d; wdf_in in;
    float disp[N];
    clock_t t0; int f, l;
    const int F = 20000;
    wsc_init(&s);
    memset(&st, 0, sizeof st); st.rim = 1.0f;
    memset(&in, 0, sizeof in);
    in.present = 1.0f; in.lvl[1] = 0.5f; in.scope = &o;
    t0 = clock();
    for (f = 0; f < F; f++) {
        gen(0, 0.5f, 220.0f);
        wsc_push(&s, buf, BLK);
        wsc_frame(&s, DT, 1.0f, &o);
        wdf_map(&st, &in, DT, &d);
        in.onset = (f % 25) == 0; in.onset_strength = 0.8f;
        for (l = 0; l < 3; l++) {
            static float gain[80];
            memset(disp, 0, sizeof disp); wdf_apply(&d, l, disp, N);
            wdf_glow(&d, l, gain, 80);
        }
    }
    printf("  cost: scope + map + apply + glow (3 layers, beats every 25 frames) = %.1f us/frame on this host "
           "(signal generation included)\n",
           1e6 * (double)(clock() - t0) / CLOCKS_PER_SEC / F);
}

int main(void)
{
    printf("-- scope --\n");   test_scope();
    printf("-- deform --\n");  test_deform();
    printf("-- organism --\n"); test_organism();
    printf("-- features --\n"); test_features();
    printf("-- beat lock + calm --\n"); test_beat_calm();
    printf("-- hints --\n"); test_hints();
    printf("-- cost --\n");    report_cost();
    if (failures) { printf("\nFAILED: %d check(s)\n", failures); return 1; }
    printf("\nOK\n");
    return 0;
}
