// The audio-reactive wave's console-side glue.  See ui_wave_audio.h.
//
// Everything hard about this subsystem is in the three header-only kernels,
// which are pure C, have no globals and are tested on the host.  What is left
// here is the three things they deliberately do not do, because doing them
// would make them untestable: a cross-thread tap, a clock, and a gate file.
//
// WHAT THIS FILE DOES NOT TOUCH.  No RSX state, no vertex arrays, no video
// memory, no framebuffer.  The renderer-side change is two call sites in
// ui_wave.cpp swapping two literals for two variables, which is exactly the
// seam wave_field.h's INTEGRATION note set up, plus wave_audio_look()'s two
// multipliers, which scale a layer's height and colour before any vertex is
// computed.

#include <stdio.h>
#include <string.h>
#include <sys/mutex.h>

#include "ui_wave_audio.h"
#include "wave_audio.h"
#include "wave_motion.h"
#include "wave_render_map.h"
#include "jf_paths.h"
#include "plog.h"
#include "timing.h"

extern void crash_log(const char *msg);

// Music is resampled to 48 kHz before it reaches the PCM ring (see
// music_player.cpp's decode path), so the tap is always at this rate whatever
// the source file was.
#define WAVE_AUDIO_RATE   48000.0f

// Gate, per UI-BRIEF rule 1.  This one DEFAULTS ON, which is a departure from
// the gates around the card, text and vertex-array paths, and the reason is
// that the risk is a different kind:
//
//   Those gates guard RSX state binds.  A wrong bind wedges the GPU, takes the
//   console off the network and needs a power cycle, so the safe default is
//   off and you opt in over FTP.
//
//   This path touches no GPU state at all.  Its worst failure is an ugly wave,
//   and its fallback -- wrm_map(NULL) -- is the exact set of constants the
//   renderer passes today.  Defaulting it off would mean the headline feature
//   needs an FTP round trip to see, to protect against a risk it does not
//   carry.
//
// So: absent or anything but "0" means on; a file containing 0 turns it off
// and the wave reverts to today's behaviour with no reflash.
#define WAVEAUDIO_FILE    "jellyfin_wavereact.txt"

// The file is also the INTENSITY, since the first look on a TV was "barely
// noticed":
//
//   0        off (today's constants, as before)
//   1        normal -- the mapping as measured, response gain x1.0
//   2        strong -- x1.8   (the default: absent or unreadable)
//   3 or up  max    -- x2.6
//
// The gain steepens the response only; every ceiling in wave_render_map.h is
// unchanged, so no level can push the band into the card grid.  Read once,
// at the first frame, like the other gates: a relaunch applies a change.
#define WAVEAUDIO_DEFAULT 2

static int gate_level(void)
{
    FILE *f = fopen(jf_data_path(WAVEAUDIO_FILE), "r");
    if (!f) return WAVEAUDIO_DEFAULT;               // absent = default
    int v = WAVEAUDIO_DEFAULT;
    if (fscanf(f, "%d", &v) != 1) v = WAVEAUDIO_DEFAULT;
    fclose(f);
    return v < 0 ? 0 : v;
}

static float level_gain(int level)
{
    if (level <= 1) return 1.0f;
    if (level == 2) return 1.8f;
    return 2.6f;
}

// Settings > Wave Intensity writes the same file, so FTP and the menu agree.
static void save_level(int level)
{
    FILE *f = fopen(jf_data_path(WAVEAUDIO_FILE), "w");
    if (!f) return;
    fprintf(f, "%d\n", level);
    fclose(f);
}

// --- state ---------------------------------------------------------------
// s_wa is written by BOTH threads (wa_push from playback, wa_frame from the
// UI) and is the only thing the mutex protects.  s_wm is UI-thread only --
// wm_update never sees the tap -- so it stays outside the lock, which keeps
// the audio thread's worst-case block down to wa_frame's arithmetic.
static wa_state     s_wa;
static wm_state     s_wm;
static sys_mutex_t  s_mtx;
static bool         s_mtx_ok  = false;
static bool         s_on      = false;      // gate + init both succeeded
static bool         s_init_ok = false;      // the analyser is set up (whatever the level)
static int          s_level   = WAVEAUDIO_DEFAULT;   // 0 off .. 3 max
static bool         s_started = false;      // one-shot init done
static u64          s_last_us = 0;

// Cached output, so a second wave_draw() in the same frame returns the same
// numbers rather than a fresh set derived from a zero dt.
static wrm_out      s_out;
static float        s_gain = 1.0f;              // from the gate level
static wrm_db_state s_db;                       // per-band envelopes
static float        s_lum3[3] = { 1.0f, 1.0f, 1.0f };
static float        s_present = 0.0f;           // 0 at rest .. 1 with audio
static float        s_kick = 0.0f;              // sub-bass hit waiting for the snow
static wsc_state    s_sc;                       // stereo + waveform (wave_scope.h)
static wdf_state    s_wdf;                      // shape deformation (wave_deform.h)
static wdf_look     s_def;
static wa_features  s_feat;                     // last analyser frame, for viz_frame

// Lazy, on the first wave_audio_frame().  NOT at init time: UI-BRIEF rule 2 --
// ui_init() runs before the logger is loaded, so an init-time plog line is
// discarded.  By the first frame plog_load_setting() has run, so both the
// plog and the crash_log land.
static void wave_audio_start(void)
{
    s_started = true;
    wrm_map(NULL, &s_out);                  // idle values, valid from here on

    int level = gate_level();
    if (level > 3) level = 3;
    s_level = level;
    s_gain  = level_gain(level > 0 ? level : WAVEAUDIO_DEFAULT);
    // The analyser is set up even at level 0 now, so Settings > Wave
    // Intensity can turn it on without a relaunch.  At 0 nothing is fed and
    // the published look stays the idle set, exactly as before.
    if (!wa_init(&s_wa, WAVE_AUDIO_RATE)) {
        plog("wave: audio-reactive OFF (wa_init failed)");
        crash_log("wave: audio-reactive OFF (wa_init)");
        return;
    }
    wsc_init(&s_sc);
    memset(&s_wdf, 0, sizeof s_wdf);
    s_wdf.rim = 1.0f;
    wdf_rest(&s_def);
    if (!wm_init(&s_wm)) {
        plog("wave: audio-reactive OFF (wm_init failed)");
        crash_log("wave: audio-reactive OFF (wm_init)");
        return;
    }
    if (!s_mtx_ok) {
        sys_mutex_attr_t mattr;
        sysMutexAttrInitialize(mattr);
        if (sysMutexCreate(&s_mtx, &mattr) != 0) {
            plog("wave: audio-reactive OFF (mutex)");
            crash_log("wave: audio-reactive OFF (mutex)");
            return;
        }
        s_mtx_ok = true;
    }
    s_last_us = timing_get_us();
    s_init_ok = true;
    if (level == 0) {
        plog("wave: audio-reactive OFF (jellyfin_wavereact.txt = 0)");
        crash_log("wave: audio-reactive OFF (gate)");
        return;
    }
    s_on = true;
    {
        char b[96];
        snprintf(b, sizeof b,
                 "wave: audio-reactive ON (6-band filterbank, 48 kHz tap)"
                 " level %d, response x%d.%d", level > 3 ? 3 : level,
                 (int)s_gain, (int)(s_gain * 10.0f + 0.5f) % 10);
        plog(b);
    }
    crash_log("wave: audio-reactive ON");
}

bool wave_audio_active(void) { return s_on; }

int wave_audio_level(void)
{
    if (!s_started) {                       // before the first frame: the file's word
        const int l = gate_level();
        return l > 3 ? 3 : l;
    }
    return s_level;
}

const char *wave_audio_level_label(void)
{
    static const char *const L[4] = { "Off", "Normal", "Strong", "Max" };
    const int l = wave_audio_level();
    return L[l < 0 ? 0 : (l > 3 ? 3 : l)];
}

// Render thread (the Settings row).  Off -> Normal -> Strong -> Max -> Off.
void wave_audio_set_level(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    save_level(level);
    if (!s_started) return;                 // wave_audio_start() reads the file
    s_level = level;
    if (level > 0) s_gain = level_gain(level);
    if (level == 0) {
        if (!s_on) return;
        // Off: stop feeding (the tap checks s_on) and publish the idle set,
        // exactly what the gate-off path has always published.
        s_on = false;
        wrm_map(NULL, &s_out);
        wdf_rest(&s_def);
        s_present = 0.0f;
        s_kick = 0.0f;
        s_lum3[0] = s_lum3[1] = s_lum3[2] = 1.0f;
        plog("wave: audio-reactive OFF (Settings)");
        return;
    }
    if (!s_on && s_init_ok) {
        // On again: start the analysers from rest, as a launch would.
        sysMutexLock(s_mtx, 0);
        wa_init(&s_wa, WAVE_AUDIO_RATE);
        wsc_init(&s_sc);
        sysMutexUnlock(s_mtx);
        wm_init(&s_wm);
        memset(&s_db, 0, sizeof s_db);
        memset(&s_wdf, 0, sizeof s_wdf);
        s_wdf.rim = 1.0f;
        wdf_rest(&s_def);
        s_last_us = timing_get_us();
        s_on = true;
    }
    char b[64];
    snprintf(b, sizeof b, "wave: audio-reactive level %d (Settings)", level);
    plog(b);
}

void wave_audio_push(const float *lr, int n_pairs)
{
    // s_on is written once by the UI thread before any push can matter and
    // only ever goes false->true, so an unsynchronised read here is safe: the
    // worst case is dropping the first block or two while the UI thread is
    // still in wave_audio_start(), which no envelope can notice.
    if (!s_on || !s_mtx_ok || !lr || n_pairs <= 0) return;
    sysMutexLock(s_mtx, 0);
    wa_push(&s_wa, lr, n_pairs, 2);
    wsc_push(&s_sc, lr, n_pairs);
    sysMutexUnlock(s_mtx);
}

void wave_audio_frame(float *dt_scale, float *perturb, float *drive)
{
    if (!s_started) wave_audio_start();

    if (s_on) {
        u64   now = timing_get_us();
        u64   el  = (now > s_last_us) ? (now - s_last_us) : 0;
        float dt  = (float)el * 1.0e-6f;

        // A second call inside the same frame lands here with an elapsed time
        // of a few microseconds.  Rather than feed that in -- which would be
        // harmless but would make the analyser's rate depend on how many times
        // the background happened to be composited -- anything under half a
        // frame is treated as the same frame and the cached values stand.
        if (dt >= 0.008f) {
            wa_features f;
            s_last_us = now;
            if (dt > WA_DT_MAX) dt = WA_DT_MAX;   // a stall, not a frame

            sysMutexLock(s_mtx, 0);
            wa_frame(&s_wa, dt, &f);              // reads and clears the tap
            s_feat = f;
            sysMutexUnlock(s_mtx);

            wm_update(&s_wm, &f, dt);             // UI-thread state only
            wrm_map_gain(&s_wm.p, s_gain, &s_out);
            // Distinct bands (wave_render_map.h): lows, mids and highs each
            // move their own layer, over a calm shared base.
            {
                float src[3];
                src[0] = f.band[WA_SUB] > f.band[WA_BASS] ? f.band[WA_SUB] : f.band[WA_BASS];
                // The vocal layer: mostly 700 Hz - 2 kHz with a little of the
                // presence band, and only a fifth of the low-mids -- where a
                // distorted 808's and a kick's harmonics live.
                src[1] = 0.20f * f.band[WA_LOWMID] + 0.60f * f.band[WA_MID] + 0.20f * f.band[WA_HIGH];
                src[2] = f.band_fast[WA_HIGH] > f.band_fast[WA_AIR]
                       ? f.band_fast[WA_HIGH] : f.band_fast[WA_AIR];
                const float present = f.silence >= 0.999f ? 0.0f : 1.0f - f.silence;
                s_present = present;
                const float resp = s_gain <= 1.0f ? 0.8f : (s_gain < 2.0f ? 1.0f : 1.25f);
                s_db.in_fast[0] = f.band_fast[WA_SUB] > f.band_fast[WA_BASS]
                                ? f.band_fast[WA_SUB] : f.band_fast[WA_BASS];
                s_db.in_fast[1] = 0.20f * f.band_fast[WA_LOWMID] + 0.60f * f.band_fast[WA_MID]
                                + 0.20f * f.band_fast[WA_HIGH];
                s_db.in_fast[2] = src[2];
                s_db.tempo_hz   = f.beat_hz;
                s_db.tempo_conf = f.beat_conf;
                s_db.energy     = f.level;
                s_db.bass_rel   = f.bass_rel;
                s_db.onset      = f.onset;
                s_db.onset_strength = f.onset_strength;
                wrm_distinct(&s_db, src, f.band_fast[WA_SUB], present, resp, dt,
                             &s_out, s_lum3);
                if (s_db.kick > s_kick) s_kick = s_db.kick;   // held until the snow takes it

                // JellyWave 2.0 shape: stereo + waveform, then each signal's
                // deformation (wave_deform.h).
                {
                    wsc_out sc;
                    wdf_in  in;
                    sysMutexLock(s_mtx, 0);
                    wsc_frame(&s_sc, dt, present, &sc);
                    sysMutexUnlock(s_mtx);
                    for (int i = 0; i < 3; i++) { in.lvl[i] = s_db.lvl[i]; in.punch[i] = s_db.punch[i]; }
                    in.onset          = f.onset;
                    in.onset_strength = f.onset_strength;
                    in.tempo_hz       = f.beat_hz;
                    in.tempo_conf     = f.beat_conf;
                    in.present        = present;
                    in.energy         = s_db.energy_eff;
                    in.centroid       = f.centroid;
                    in.bass_rel       = f.bass_rel;
                    in.air            = f.band_fast[WA_AIR];
                    in.mid_rel        = f.mid_rel;
                    in.scope          = &sc;
                    wdf_map(&s_wdf, &in, dt, &s_def);
                }
            }

            // A bounded trace of what the wave is actually being driven with.
            //
            // This subsystem has three ways to look identical from the sofa --
            // the tap never arriving, the analyser hearing nothing, and the
            // mapped drive being too gentle to see -- and they need completely
            // different fixes.  One line every five seconds tells them apart:
            // rms says whether audio reached the analyser at all, drive says
            // what the renderer was asked for.
            //
            // The cap was 24 lines, which ran out two minutes into a session
            // and took the render thread's only proof of life with it -- so
            // when the app died there was no way to tell whether the UI had
            // stopped drawing.  At one line per five seconds, 600 covers a
            // fifty-minute sitting for 30 KB of log.
            static int s_dbg_n  = 0;
            static u64 s_dbg_us = 0;
            if (s_dbg_n < 600 && (s_dbg_us == 0 || now - s_dbg_us >= 5000000ULL)) {
                s_dbg_us = now;
                s_dbg_n++;
                // amp and lum added so a wave that moves but does not look
                // different per band can be told from one whose bands never
                // separated in the analyser.
                char b[240];
                snprintf(b, sizeof b,
                         "wave: rms=%d.%02d b0=%d.%02d drive=%d.%02d ts=%d.%02d"
                         " amp=%d.%02d/%d.%02d/%d.%02d lum=%d.%02d"
                         " thk=%d.%02d acc=%d L=%d.%02d/%d.%02d/%d.%02d",
                         (int)f.rms, (int)(f.rms * 100) % 100,
                         (int)f.band[0], (int)(f.band[0] * 100) % 100,
                         (int)s_out.drive, (int)(s_out.drive * 100) % 100,
                         (int)s_out.dt_scale, (int)(s_out.dt_scale * 100) % 100,
                         (int)s_out.amp[0], (int)(s_out.amp[0] * 100) % 100,
                         (int)s_out.amp[1], (int)(s_out.amp[1] * 100) % 100,
                         (int)s_out.amp[2], (int)(s_out.amp[2] * 100) % 100,
                         (int)s_out.lum, (int)(s_out.lum * 100) % 100,
                         (int)s_out.thick, (int)(s_out.thick * 100) % 100,
                         (s_out.acc.a[0] > 0.0f) + (s_out.acc.a[1] > 0.0f)
                         + (s_out.acc.a[2] > 0.0f) + (s_out.acc.a[3] > 0.0f),
                         (int)s_lum3[0], (int)(s_lum3[0] * 100) % 100,
                         (int)s_lum3[1], (int)(s_lum3[1] * 100) % 100,
                         (int)s_lum3[2], (int)(s_lum3[2] * 100) % 100);
                plog(b);
            }
        }
    }

    // Always publish.  When the gate is off, or init failed, or this is the
    // very first call, s_out holds wrm_map(NULL) -- the idle set, which is the
    // constants ui_wave.cpp used before any of this existed.
    if (dt_scale) *dt_scale = s_out.dt_scale;
    if (perturb)  *perturb  = s_out.perturb;
    if (drive)    *drive    = s_out.drive;
}

// Before the first wave_audio_frame() s_out is still zero-filled, and a zero
// height would flatten the wave -- so that case returns the rest values
// rather than the cache.
void wave_audio_look(float amp[3], float *lum)
{
    wrm_out idle;
    const wrm_out *o = &s_out;
    if (!s_started) { wrm_map(NULL, &idle); o = &idle; }
    if (amp) { amp[0] = o->amp[0]; amp[1] = o->amp[1]; amp[2] = o->amp[2]; }
    if (lum) *lum = o->lum;
}

const wm_params *wave_audio_params(void) { return s_on ? &s_wm.p : NULL; }
float wave_audio_presence(void) { return s_on ? s_present : 0.0f; }

void wave_audio_bands(float lvl[3], float *kick)
{
    if (lvl) {
        lvl[0] = s_on ? s_db.lvl[0] : 0.0f;
        lvl[1] = s_on ? s_db.lvl[1] : 0.0f;
        lvl[2] = s_on ? s_db.lvl[2] : 0.0f;
    }
    if (kick) { *kick = s_kick; s_kick = 0.0f; }
}

void wave_audio_features(float punch[3], float *energy, float *tempo_hz,
                         float *tempo_conf, float *gain)
{
    if (punch) for (int i = 0; i < 3; i++) punch[i] = s_on ? s_db.punch[i] : 0.0f;
    if (energy)     *energy     = s_on ? s_db.energy_eff : 0.0f;
    if (tempo_hz)   *tempo_hz   = s_on ? s_db.tempo_hz : 0.0f;
    if (tempo_conf) *tempo_conf = s_on ? s_db.tempo_conf : 0.0f;
    if (gain)       *gain       = s_on ? s_gain : 0.0f;
}

void wave_audio_deform(wdf_look *out)
{
    if (!out) return;
    if (!s_started || !s_on) { wdf_rest(out); return; }
    *out = s_def;
}

void wave_audio_lum3(float lum3[3])
{
    if (!lum3) return;
    if (!s_started) { lum3[0] = lum3[1] = lum3[2] = 1.0f; return; }
    lum3[0] = s_lum3[0]; lum3[1] = s_lum3[1]; lum3[2] = s_lum3[2];
}

void wave_audio_shape(float *thick, wrm_accent_set *acc)
{
    wrm_out idle;
    const wrm_out *o = &s_out;
    if (!s_started) { wrm_map(NULL, &idle); o = &idle; }
    if (thick) *thick = o->thick;
    if (acc)   *acc   = o->acc;
}

// viz_frame (viz_frame.h): every preset reads the same frame.  Peeks at the
// kick without consuming it -- the snow still owns that.  Spectra, dt and
// paused are filled by the caller.
void wave_audio_viz(viz_frame *v)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
    if (!s_started || !s_on) return;
    for (int i = 0; i < 6; i++) v->band[i] = s_feat.band[i];
    for (int i = 0; i < 3; i++) v->lvl3[i] = s_db.lvl[i];
    v->kick = s_kick;  v->flux = s_feat.flux;  v->centroid = s_feat.centroid;
    v->beat_hz = s_feat.beat_hz;  v->beat_conf = s_feat.beat_conf;
    v->stereo_bal = s_sc.bal;  v->stereo_width = s_sc.width;
    v->presence = s_present;
}
