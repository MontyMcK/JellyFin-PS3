// Build: gcc -O2 -I source/ui/render -o jw_analyze tests/tools/jw_analyze.c -lm
// Input: ffmpeg -i track.flac -ac 2 -ar 48000 -f f32be track.raw
// Usage: jw_analyze track.raw name   (SECT=1 also prints the section/tint every 30 s)
// Run a real track through the PS3's audio-reactive pipeline, frame by frame
// at 60 fps, exactly as ui_wave_audio.cpp does, and report what the wave did.
// Input: raw stereo float32 big-endian at 48 kHz.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "wave_audio.h"
#include "wave_motion.h"
#include "wave_render_map.h"
#include "wave_deform.h"

#define FR 800            /* 48000 / 60 */

static float be(const unsigned char *p) {
    uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    float f; memcpy(&f, &u, 4); return f;
}

typedef struct { double s, s2; double lo, hi; long n; double cap; double motion; } stat;
static void st_add(stat *a, double v) { a->s += v; a->s2 += v * v; a->n++; }
static double st_mean(const stat *a) { return a->n ? a->s / a->n : 0; }
static double st_sd(const stat *a) { double m = st_mean(a); return a->n ? sqrt(fabs(a->s2 / a->n - m * m)) : 0; }

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *raw = malloc(len); fread(raw, 1, len, f); fclose(f);
    long frames = len / 8;
    float *pcm = malloc(sizeof(float) * 2 * FR);

    static wa_state wa; static wm_state wm; static wrm_db_state db; static wdf_state wdf;
    static wsc_state sc;
    wa_init(&wa, 48000.0f); wm_init(&wm); wsc_init(&sc);
    memset(&db, 0, sizeof db); memset(&wdf, 0, sizeof wdf); wdf.rim = 1.0f;
    const float gain = 1.8f, resp = 1.0f, dt = 1.0f / 60.0f;

    stat hit = {0}, lat = {0}, pdv[3] = {{0}}, amp[3] = {{0}}, lvl[3] = {{0}}, pun[3] = {{0}}, drama = {0}, defr = {0}, swing = {0};
    double motion[3] = {0}, capf[3] = {0}, prev[3] = {1, 1, 1};
    double tssum = 0, tsmax = 0, esum = 0, lvsum = 0, dnsum = 0, dbsum = 0;
    double c01 = 0, c00 = 0, c11 = 0;
    long onsets = 0, nfr = 0; double bhz = 0, bconf = 0;
    float win[30]; int wi = 0;
    double sub_med = 0, sub_med2 = 0, sub_fast2 = 0, sub_fast = 0;

    for (long pos = 0; pos + FR <= frames; pos += FR) {
        for (int i = 0; i < 2 * FR; i++) pcm[i] = be(raw + (pos * 2 + i) * 4);
        wa_push(&wa, pcm, FR, 2);
        wsc_push(&sc, pcm, FR);
        wa_features ft;
        wa_frame(&wa, dt, &ft);
        wm_update(&wm, &ft, dt);
        wrm_out o; float lum3[3];
        wrm_map_gain(&wm.p, gain, &o);
        float src[3];
        src[0] = ft.band[WA_SUB] > ft.band[WA_BASS] ? ft.band[WA_SUB] : ft.band[WA_BASS];
        src[1] = 0.20f * ft.band[WA_LOWMID] + 0.60f * ft.band[WA_MID] + 0.20f * ft.band[WA_HIGH];
        src[2] = ft.band_fast[WA_HIGH] > ft.band_fast[WA_AIR] ? ft.band_fast[WA_HIGH] : ft.band_fast[WA_AIR];
        const float present = ft.silence >= 0.999f ? 0.0f : 1.0f - ft.silence;
        db.in_fast[0] = ft.band_fast[WA_SUB] > ft.band_fast[WA_BASS] ? ft.band_fast[WA_SUB] : ft.band_fast[WA_BASS];
        db.in_fast[1] = 0.20f * ft.band_fast[WA_LOWMID] + 0.60f * ft.band_fast[WA_MID] + 0.20f * ft.band_fast[WA_HIGH];
        db.in_fast[2] = src[2];
        db.tempo_hz = ft.beat_hz; db.tempo_conf = ft.beat_conf; db.energy = ft.level; db.bass_rel = ft.bass_rel; db.onset = ft.onset; db.onset_strength = ft.onset_strength;
#ifdef WRM_HAS_FEATURES
        wrm_features(&db, &ft);
#endif
        wrm_distinct(&db, src, ft.band_fast[WA_SUB], present, resp, dt, &o, lum3);
        wsc_out so; wsc_frame(&sc, dt, present, &so);
        wdf_in in; memset(&in, 0, sizeof in);
        for (int i = 0; i < 3; i++) { in.lvl[i] = db.lvl[i]; in.punch[i] = db.punch[i]; }
        in.onset = ft.onset; in.onset_strength = ft.onset_strength;
        in.tempo_hz = ft.beat_hz; in.tempo_conf = ft.beat_conf; in.present = present; in.scope = &so; in.energy = db.energy_eff; esum += db.energy_eff;
        in.centroid = ft.centroid; in.bass_rel = ft.bass_rel; in.mid_rel = ft.mid_rel; in.air = ft.band_fast[WA_AIR];
        wdf_look d; wdf_map(&wdf, &in, dt, &d);
        if (d.bloom > 0.99f) printf("    DROP at %d:%02d\n", (int)(pos / 48000 / 60), (int)(pos / 48000 % 60));
        { static long ts = 0; if (++ts % 1800 == 0 && getenv("SECT")) printf("    t=%3lds section %+.2f tint %+.2f  beat %.0f BPM conf %.2f (ac %.0f @ %.2f, iv %.0f @ %.2f)\n", pos / 48000, d.section, d.tint, ft.beat_hz * 60, ft.beat_conf, wa.ac_hz * 60, wa.ac_conf, wa.beat_period > 0 ? 60 / wa.beat_period : 0, wa.beat_conf); }

        if (pos < 48000 * 5) continue;            /* skip the first 5 s: calibration */
        nfr++;
        for (int l = 0; l < 3; l++) {
            st_add(&amp[l], o.amp[l]); st_add(&lvl[l], db.lvl[l]); st_add(&pun[l], db.punch[l]);
            motion[l] += fabs(o.amp[l] - prev[l]); prev[l] = o.amp[l];
            if (o.amp[l] >= WRM_DB_AMP_MAX[l] - 0.03f) capf[l]++;
        }
        st_add(&drama, d.gain);
        {   /* separation: correlation of the bass and vocal layers' movement */
            static float pa0 = 1, pa1 = 1;
            const double d0 = o.amp[0] - pa0, d1 = o.amp[1] - pa1;
            c01 += d0 * d1; c00 += d0 * d0; c11 += d1 * d1;
            pa0 = o.amp[0]; pa1 = o.amp[1];
        } tssum += o.dt_scale; if (o.dt_scale > tsmax) tsmax = o.dt_scale; st_add(&pdv[0], db.pdev[0]); st_add(&pdv[1], db.pdev[1]); st_add(&pdv[2], db.pdev[2]);
        if (nfr % 6 == 0) {
            float disp[72]; memset(disp, 0, sizeof disp);
            wdf_apply(&d, 0, disp, 72);
            double e = 0; for (int k = 0; k < 72; k++) e += disp[k] * disp[k];
            st_add(&defr, sqrt(e / 72));
        }
        win[wi++ % 30] = o.amp[0];
        if (wi >= 30 && wi % 15 == 0) {
            float lo = 9, hi = -9; for (int k = 0; k < 30; k++) { if (win[k] < lo) lo = win[k]; if (win[k] > hi) hi = win[k]; }
            st_add(&swing, hi - lo);
        }
        /* per-hit: from each onset, the bass layer's rise over the next 250 ms
           and how long it took to reach 80% of it */
        {
            static float base = 0, peak = 0, hist[16], pre[9]; static int t = -1, n = 0, pi = 0;
            pre[pi++ % 9] = o.amp[0];
            if (ft.onset > 0 && t < 0) {
                base = o.amp[0];
                for (int q = 0; q < 9; q++) if (pre[q] < base) base = pre[q];   /* the trough before */
                peak = o.amp[0]; t = 0; n = 0;
            }
            if (t >= 0) {
                hist[t] = o.amp[0];
                if (o.amp[0] > peak) peak = o.amp[0];
                if (++t >= 15) {
                    const float rise = peak - base;
                    int k = 0; while (k < 15 && hist[k] < base + 0.8f * rise) k++;
                    if (rise > 0.02f) { st_add(&hit, rise); st_add(&lat, k * 1000.0 / 60.0); }
                    else st_add(&hit, 0.0);
                    t = -1;
                }
            }
        }
        onsets += ft.onset > 0; lvsum += ft.level; dnsum += ft.density; dbsum += 6.0206f*wa_log2(wa.rms_ref);
        bhz += ft.beat_hz; bconf += ft.beat_conf;
        sub_med += src[0]; sub_med2 += src[0] * src[0];
        sub_fast += db.in_fast[0]; sub_fast2 += db.in_fast[0] * db.in_fast[0];
    }
    double secs = nfr / 60.0;
    printf("%-10s %5.0fs | bass-in med %.2f+-%.2f fast +-%.2f | onsets/min %4.0f beat %.2f Hz conf %.2f\n",
           argv[2], secs, sub_med / nfr, sqrt(fabs(sub_med2 / nfr - pow(sub_med / nfr, 2))),
           sqrt(fabs(sub_fast2 / nfr - pow(sub_fast / nfr, 2))), onsets / secs * 60, bhz / nfr, bconf / nfr);
    for (int l = 0; l < 3; l++)
        printf("    layer %d: amp %.2f+-%.2f  at-cap %4.1f%%  motion %.2f/s  lvl %.2f+-%.2f  punch %.2f+-%.2f\n",
               l, st_mean(&amp[l]), st_sd(&amp[l]), 100 * capf[l] / nfr, motion[l] / secs,
               st_mean(&lvl[l]), st_sd(&lvl[l]), st_mean(&pun[l]), st_sd(&pun[l]));
    printf("    bass swing per 0.5 s %.2f+-%.2f | drama %.2f+-%.2f | deform rms %.4f+-%.4f\n",
           st_mean(&swing), st_sd(&swing), st_mean(&drama), st_sd(&drama), st_mean(&defr), st_sd(&defr));
    printf("    punch dev %.3f %.3f %.3f\n", st_mean(&pdv[0]), st_mean(&pdv[1]), st_mean(&pdv[2]));
    printf("    HIT: bass rise per beat %.3f+-%.3f, 80%% reached in %.0f ms\n", st_mean(&hit), st_sd(&hit), st_mean(&lat));
    printf("    level %.2f (rms_ref %.1f)  density %.2f  energy_eff %.2f\n", lvsum/nfr, dbsum/nfr, dnsum/nfr, esum/((double)frames/800));
    printf("    SEPARATION: bass/vocal layer movement correlation %.2f (lower = more distinct)\n",
           c01 / sqrt(c00 * c11 + 1e-12));
    printf("    TEMPO: wave speed x%.2f average, x%.2f max  (beat %.0f BPM)\n", tssum/nfr, tsmax, bhz/nfr*60);
    return 0;
}
