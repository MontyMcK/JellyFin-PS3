// The audio-reactive wave's console-side glue.  See ui_wave_audio.h.
//
// Everything hard about this subsystem is in the three header-only kernels,
// which are pure C, have no globals and are tested on the host.  What is left
// here is the three things they deliberately do not do, because doing them
// would make them untestable: a cross-thread tap, a clock, and a gate file.
//
// WHAT THIS FILE DOES NOT TOUCH.  No RSX state, no vertex arrays, no video
// memory, no framebuffer.  The whole renderer-side change is two call sites in
// ui_wave.cpp swapping two literals for two variables, which is exactly the
// seam wave_field.h's INTEGRATION note set up.

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

static bool gate_enabled(void)
{
    FILE *f = fopen(jf_data_path(WAVEAUDIO_FILE), "r");
    if (!f) return true;                    // absent = on
    int v = 1;
    if (fscanf(f, "%d", &v) != 1) v = 1;    // unreadable = on
    fclose(f);
    return v != 0;
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
static bool         s_started = false;      // one-shot init done
static u64          s_last_us = 0;

// Cached output, so a second wave_draw() in the same frame returns the same
// numbers rather than a fresh set derived from a zero dt.
static wrm_out      s_out;

// Lazy, on the first wave_audio_frame().  NOT at init time: UI-BRIEF rule 2 --
// ui_init() runs before the logger is loaded, so an init-time plog line is
// discarded.  By the first frame plog_load_setting() has run, so both the
// plog and the crash_log land.
static void wave_audio_start(void)
{
    s_started = true;
    wrm_map(NULL, &s_out);                  // idle values, valid from here on

    if (!gate_enabled()) {
        plog("wave: audio-reactive OFF (jellyfin_wavereact.txt = 0)");
        crash_log("wave: audio-reactive OFF (gate)");
        return;
    }
    if (!wa_init(&s_wa, WAVE_AUDIO_RATE)) {
        plog("wave: audio-reactive OFF (wa_init failed)");
        crash_log("wave: audio-reactive OFF (wa_init)");
        return;
    }
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
    s_on = true;
    plog("wave: audio-reactive ON (6-band filterbank, 48 kHz tap)");
    crash_log("wave: audio-reactive ON");
}

bool wave_audio_active(void) { return s_on; }

void wave_audio_push(const float *lr, int n_pairs)
{
    // s_on is written once by the UI thread before any push can matter and
    // only ever goes false->true, so an unsynchronised read here is safe: the
    // worst case is dropping the first block or two while the UI thread is
    // still in wave_audio_start(), which no envelope can notice.
    if (!s_on || !s_mtx_ok || !lr || n_pairs <= 0) return;
    sysMutexLock(s_mtx, 0);
    wa_push(&s_wa, lr, n_pairs, 2);
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
            sysMutexUnlock(s_mtx);

            wm_update(&s_wm, &f, dt);             // UI-thread state only
            wrm_map(&s_wm.p, &s_out);

            // A bounded trace of what the wave is actually being driven with.
            //
            // This subsystem has three ways to look identical from the sofa --
            // the tap never arriving, the analyser hearing nothing, and the
            // mapped drive being too gentle to see -- and they need completely
            // different fixes.  One line every five seconds tells them apart:
            // rms says whether audio reached the analyser at all, drive says
            // what the renderer was asked for.  Capped at 24 lines so a long
            // listening session does not fill the log.
            static int s_dbg_n  = 0;
            static u64 s_dbg_us = 0;
            if (s_dbg_n < 24 && (s_dbg_us == 0 || now - s_dbg_us >= 5000000ULL)) {
                s_dbg_us = now;
                s_dbg_n++;
                char b[112];
                snprintf(b, sizeof b,
                         "wave: rms=%d.%02d b0=%d.%02d drive=%d.%02d ts=%d.%02d",
                         (int)f.rms, (int)(f.rms * 100) % 100,
                         (int)f.band[0], (int)(f.band[0] * 100) % 100,
                         (int)s_out.drive, (int)(s_out.drive * 100) % 100,
                         (int)s_out.dt_scale, (int)(s_out.dt_scale * 100) % 100);
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
