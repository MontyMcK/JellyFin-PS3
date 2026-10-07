#include "../../build_config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include "rsxutil.h"
#include "wave_shaders.h"
#include "wave_field.h"
#include "wave_gel.h"          /* JellyWave: pulls wave_cam.h + wave_light.h */
#include "wave_snow.h"
#include "wave_nav.h"          /* the wave answers the pad */
#include "wave_look.h"         /* haze, fringe, glow, 59.94 Hz blend */
#include "wave_drop.h"         /* JellyDrop: the ribbons closed into the bell */
#include "menusnow.h"         /* Settings > Menu Particles */
#include "bg_gradient.h"
#include "timing.h"
#include "month_bg.h"
#include "ui_wave_audio.h"
#include "ui_wave.h"
#include "ui_visuals.h"
#include "ui_strobe_test.h"
#include "plog.h"
#include "jf_paths.h"
#include <unistd.h>
#include <sys/thread.h>

extern void crash_log(const char *msg);

// --- CPU-vs-GPU background compositing -----------------------------------
// On real PS3, the framebuffer lives in RSX-local VRAM and PPU writes to it
// are uncached and very slow (a full-screen CPU fill costs ~150-180ms), so the
// XMB background is rendered with the RSX (wave_draw's immediate-mode path) and
// only small UI elements (text, key cells) are composited by the CPU.
//
// RPCS3 emulates that VRAM as ordinary host RAM, and — crucially — presents the
// GPU-rendered surface from its render-target cache on flip, silently dropping
// any later CPU writes to the same buffer.  The result is a frame where the GPU
// wave shows but every CPU-drawn element is invisible.
//
// So on the emulator the *entire* frame, background included, is composited on
// the CPU and no GPU op owns the display surface.  On hardware, the GPU wave.
//
// This is a compile-time switch (BUILD_FOR_RPCS3 in build_config.h), not
// runtime detection: timing probes misclassify real hardware.  See
// docs/wave-renderer-notes.md, "Emulator vs hardware".
bool ui_cpu_bg(void) { return BUILD_FOR_RPCS3 != 0; }

#define WAVE_STEP_PX    20
#define WAVE_NS         8      // vertical slices per ribbon (fade resolution)
#define WAVE_MAX_COLS   98     // x columns: supports up to ~1940px wide (720p uses 65)

// Ribbons read as translucent veils over the background gradient: bright at
// the crest, fading to nothing below.  The fade is *baked into opaque vertex
// colours* on the CPU rather than left to GPU alpha blending.  Each ribbon is
// tessellated into WAVE_NS horizontal slices from its crest down to the screen
// bottom; at every grid node the colour is the ribbon tint composited over the
// gradient (and any earlier ribbons) at that exact height, using the same
// maths as tools/ui_preview/preview.c.  Drawn fully opaque with blending off,
// the result is exact at the grid nodes (every WAVE_STEP_PX columns, WAVE_NS
// slices) and linearly interpolated between them: the crest is a polyline and
// each slice's fade a linear ramp, where preview.c evaluates every pixel.  The
// difference sits below the dither and is invisible at viewing distance, but
// it is an approximation, not a pixel match.
// WAVE_ALPHA is the crest opacity used for that pre-blend.
//
// SUBMISSION: two paths, see wave_draw() and gpuwave_mode().
//
//   immediate  every vertex streamed through the FIFO (rsxDrawVertex4f/4ub);
//              measured 2,129 us/frame for the ~4,700-vertex wave at 1080p.
//   arrays     vertices written once into RSX-local memory, then one
//              rsxDrawVertexArray per triangle strip against a single binding.
//
// The array path is safe only with two disciplines, both followed below:
//
//   * rsxInvalidateVertexCache() immediately before every rsxDrawVertexArray.
//   * Reset the attrib bindings after the last draw (COLOR0 re-bound at
//     stride 0, TEX0 disabled), so nothing downstream inherits an array
//     binding it did not ask for.  hud_dim.cpp does the same.
//
// A bad binding wedges the GPU (console off the network, power cycle), so the
// path is gated on jellyfin_gpuwave.txt and deleting the file falls back to
// immediate mode with no reflash.  History: docs/wave-renderer-notes.md,
// "Vertex arrays".
static const u32   WAVE_COLOR[3]  = { 0x004A52A8, 0x006C5BD4, 0x003A4290 };
static const u8    WAVE_ALPHA[3]  = { 56, 42, 72 };
static const float WAVE_AMP[3]    = { 30.0f, 22.0f, 15.0f };
// Crest baselines as a fraction of screen height — kept low so the ribbons
// sit in the bottom quarter of the screen instead of climbing into the middle.
static const float WAVE_BASEY[3]  = { 0.78f, 0.85f, 0.91f };

// --- where the crest shape comes from ------------------------------------
//
// wave_field.h: a driven, damped spring chain per ribbon, resampled through a
// uniform cubic B-spline (docs/wave-spec.md section 3b, tests/test_wave_field.c).
// The field supplies a unitless displacement and wave_crest() turns it into a
// screen row.
static wf_field s_field;

// This frame's height multiplier per solver layer and colour multiplier,
// from the audio mapping in wave_render_map.h.  Both are exactly 1.0 with no
// music, so the resting look is unchanged.  Written once per wave_draw(),
// next to wf_step, and only read after that.
static float    s_amp[3] = { 1.0f, 1.0f, 1.0f };
static float    s_lum    = 1.0f;
static float    s_lum3[3] = { 1.0f, 1.0f, 1.0f };   // per layer, distinct bands

// JellyWave 2.0: body swell and travelling accents, same rules.  Zero-filled
// s_acc means no live pulse, i.e. no accent, so the static initialiser is
// already the rest state.
static float          s_thick = 1.0f;
static wrm_accent_set s_acc;
static wdf_look       s_def;          // JellyWave 2.0 shape; zero = live 0 = nothing

// Everything jw_generate() takes from the audio, as one value.  The build
// runs on the generation worker, so it must never read the render thread's
// s_amp / s_lum / s_thick / s_acc directly -- they change every frame, under
// its feet.  jw_worker_queue() copies them into the job beside the field
// snapshot, before the same barrier, so a build always sees one frame's
// curve with that same frame's look.
struct jw_look {
    float          amp[3];
    float          lum;
    float          lum3[3];
    float          thick;
    wrm_accent_set acc;
    wdf_look       def;
    float          album[3];     // the album's colour pull, per channel (1 = none)
    float          art[3];       // a details page's poster colour, same rules
    float          sky[3];       // day / night and the dusk glow (1 = the night look)
    wnv_look       nav;          // the pad's pushes and bob (wave_nav.h)
    // the look pass (wave_look.h)
    jw_vec3        key;          // the key light, drifting (jw_key_drift)
    bg_quad        bg;           // the sky, for the haze
    float          px_y;         // one pixel in clip y, for the fringe
    int            look_on;      // jellyfin_jwlook.txt
    jd_pose        drop;         // JellyDrop (wave_drop.h); morph 0 = JellyWave
};

// --- the pad (wave_nav.h) ----------------------------------------------------
// btn_nav_repeat() reports every navigation step here, first press and repeat
// tick alike; wave_draw() steps the state on its own clock.  Render thread
// only, like the rest of this file's statics.
static wnv_state s_nav;
static bool      s_nav_init = false;
static wnv_fx    s_nav_fx   = { 0.0f, 0.0f, 0.0f, 1.0f };
static u64       s_nav_us   = 0;

void wave_nav_event(int dx, int dy)
{
    if (!s_nav_init) { wnv_init(&s_nav); s_nav_init = true; }
    wnv_event(&s_nav, dx, dy);
}

static void wave_nav_frame(void)
{
    if (!s_nav_init) { wnv_init(&s_nav); s_nav_init = true; }
    const u64 now = timing_get_us();
    // A long gap means the wave was not on screen (video playback, a blocking
    // load): whatever the pad did meanwhile is not the wave's to replay.
    if (s_nav_us && now - s_nav_us > 500000ULL) wnv_init(&s_nav);
    const float dt = s_nav_us ? (float)(now - s_nav_us) * 1.0e-6f : 0.0f;
    s_nav_us = now;
    wnv_step(&s_nav, dt, &s_nav_fx);
}

// --- JellyDrop (wave_drop.h) -------------------------------------------------
// L1 + R1 together (ui_input.cpp) turn the three ribbons into the Jellyfin
// bell and back.  The choice is kept in jellyfin_jellydrop.txt ("1" = bell).
// Render thread only, like the pad's state above: jd_frame() steps the
// motion and the morph once per wave_draw() call, and the build takes a copy
// of the pose in its snapshot (jw_look_now), so the worker never reads these.
#define JELLYDROP_FILE  "jellyfin_jellydrop.txt"
#define JD_MORPH_S      1.3f          // the transition, seconds (design 2.4: too slow on a TV)
#define JD_HEIGHT       0.20f         // the bell's height, fraction of the screen (was 0.40: half)
#define JD_CENTRE_Y     0.02f         // NDC, +up: just above the middle
static bool      s_jd_loaded = false;
static bool      s_jd_want   = false;  // where the morph is heading
static bool      s_jd_init   = false;
static float     s_jd_morph  = 0.0f;   // 0 wave .. 1 bell, linear in time
static jd_motion s_jd_mo;
static jd_motes  s_jd_motes;
static jd_rays   s_jd_rays;       // the radiating field
static jd_pose   s_jd_pose;            // this frame's, zero = JellyWave
static u64       s_jd_us     = 0;
static u32       s_jd_toggles = 0;
// The analyser's bands, read ONCE per call and shared by the bell and the
// snow: wave_audio_bands() hands the kick over only once.
static float     s_band_lvl[3] = { 0.0f, 0.0f, 0.0f };
static float     s_band_kick   = 0.0f;

static void jd_load(void)
{
    if (s_jd_loaded) return;
    s_jd_loaded = true;
    FILE *f = fopen(jf_data_path(JELLYDROP_FILE), "r");
    int v = 0;
    if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); }
    s_jd_want = v == 1;
}

void wave_drop_toggle(void)
{
    jd_load();
    s_jd_want = !s_jd_want;
    s_jd_toggles++;
    FILE *f = fopen(jf_data_path(JELLYDROP_FILE), "w");
    if (f) { fputs(s_jd_want ? "1\n" : "0\n", f); fclose(f); }
    plog(s_jd_want ? "wave: JellyDrop ON (L1+R1)" : "wave: JellyDrop off -- JellyWave (L1+R1)");
}

bool wave_drop_on(void) { jd_load(); return s_jd_want; }

// ---- the visualiser mode (Select) -------------------------------------------
#define VIS_FILE "jellyfin_visualiser.txt"
static int  s_vis      = -1;          // -1 = not loaded yet
static bool s_vis_off  = false;       // mode Off: gradient only
static u64  s_reveal_t0 = 0;          // Off -> JellyWave: the wave extends in from the left
#define VIS_REVEAL_US 1300000.0f

static void vis_load(void)
{
    if (s_vis >= 0) return;
    int v = -1;
    FILE *f = fopen(jf_data_path(VIS_FILE), "r");
    if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    if (v < 0 || v > 2) {                 // first run: carry JellyDrop's old toggle
        jd_load();
        v = s_jd_want ? WAVE_VIS_JELLYDROP : WAVE_VIS_JELLYWAVE;
    }
    s_vis = v;
    jd_load();
    s_jd_want = (v == WAVE_VIS_JELLYDROP);
    s_vis_off = (v == WAVE_VIS_OFF);
}

int wave_vis_mode(void) { vis_load(); return s_vis; }

void wave_vis_set(int mode)
{
    vis_load();
    if (mode < 0 || mode > 2) mode = WAVE_VIS_JELLYWAVE;
    if (mode == s_vis) return;
    s_vis = (mode + 2) % 3;      // wave_vis_cycle steps it on to `mode`
    wave_vis_cycle();
}

void wave_vis_cycle(void)
{
    vis_load();
    const int prev = s_vis;
    s_vis = (s_vis + 1) % 3;
    if (prev == WAVE_VIS_OFF && s_vis == WAVE_VIS_JELLYWAVE) s_reveal_t0 = timing_get_us();
    s_jd_want = (s_vis == WAVE_VIS_JELLYDROP);
    s_vis_off = (s_vis == WAVE_VIS_OFF);
    s_jd_toggles++;
    FILE *f = fopen(jf_data_path(VIS_FILE), "w");
    if (f) { fprintf(f, "%d\n", s_vis); fclose(f); }
    f = fopen(jf_data_path(JELLYDROP_FILE), "w");
    if (f) { fputs(s_jd_want ? "1\n" : "0\n", f); fclose(f); }
    static const char *const k[3] = { "JellyWave", "JellyDrop", "Off" };
    char b[64]; snprintf(b, sizeof b, "wave: visualiser -> %s (Square)", k[s_vis]);
    plog(b);
}

void wave_reveal_start(void)
{
    vis_load();
    if (s_vis == WAVE_VIS_JELLYWAVE) s_reveal_t0 = timing_get_us();
}

const char *wave_vis_next_label(void)
{
    vis_load();
    static const char *const next[3] = { "JellyDrop", "Off", "JellyWave" };
    return next[s_vis];
}

static void jd_frame(float aspect)
{
    vis_load();                 // the mode decides where the morph starts
    wave_audio_bands(s_band_lvl, &s_band_kick);
    if (!s_jd_init) {
        jd_motion_init(&s_jd_mo);
        jd_motes_init(&s_jd_motes, 0x4A44524Fu);
        jd_rays_init(&s_jd_rays, 0x52415953u);
        jd_load();
        s_jd_morph = s_jd_want ? 1.0f : 0.0f;       // a relaunch lands where it was left
        memset(&s_jd_pose, 0, sizeof s_jd_pose);
        s_jd_init = true;
    }
    const u64 now = timing_get_us();
    float dt = s_jd_us ? (float)(now - s_jd_us) * 1.0e-6f : 0.0f;
    s_jd_us = now;
    if (dt > 0.1f) dt = 0.1f;                       // a blocking load is not a jump

    const float prev = s_jd_morph;
    const float want = s_jd_want ? 1.0f : 0.0f;
    if (s_jd_morph < want)      { s_jd_morph += dt / JD_MORPH_S; if (s_jd_morph > want) s_jd_morph = want; }
    else if (s_jd_morph > want) { s_jd_morph -= dt / JD_MORPH_S; if (s_jd_morph < want) s_jd_morph = want; }
    if (prev < 0.9f && s_jd_morph >= 0.9f) s_jd_mo.drop_v += 2.2f;   // the seams close: an arrival wobble
    if (prev > 0.9f && s_jd_morph <= 0.9f) s_jd_mo.sq_v   -= 2.5f;   // the bell stretches before it splits

    if (!(s_jd_morph > 0.0f)) {                     // JellyWave: nothing to pose
        s_jd_pose.morph = 0.0f;
        return;
    }
    // wa_features, as far as this analyser goes: three bands and a kick.  The
    // design's balance / width / pitch / drop / key_change are PROPOSED
    // fields it does not produce, so they rest at zero.
    // 2026-09-27: fed like JellyWave -- level plus the per-layer PUNCH (the
    // transients the wave's deform rides), the effective energy, the tempo,
    // all scaled by Settings > Wave Intensity.  It used to take only the
    // smoothed levels, unscaled, which left it drifting half-asleep.
    jd_in in;
    memset(&in, 0, sizeof in);
    float punch[3], energy = 0.0f, thz = 0.0f, tconf = 0.0f, gain = 0.0f;
    wave_audio_features(punch, &energy, &thz, &tconf, &gain);
    const float g = gain > 0.0f ? 0.75f + 0.45f * gain : 0.0f;    // Normal ~1.2 .. Max ~2
    #define JDC(v) ((v) < 0.0f ? 0.0f : ((v) > 1.0f ? 1.0f : (v)))
    in.sub    = JDC(g * (s_band_lvl[0] + 0.9f * punch[0]));
    in.bass   = JDC(g * (0.8f * s_band_lvl[0] + 1.1f * punch[0]));
    in.lowmid = JDC(g * (0.9f * s_band_lvl[1] + 0.5f * punch[1]));
    in.mid    = JDC(g * (s_band_lvl[1] + 0.8f * punch[1]));
    in.high   = JDC(g * (s_band_lvl[2] + 0.8f * punch[2]));
    in.air    = JDC(g * (0.8f * s_band_lvl[2] + 0.6f * punch[2]));
    in.rms    = JDC(g * (0.5f * energy + 0.5f * (s_band_lvl[0] + s_band_lvl[1] + s_band_lvl[2]) * (1.0f / 3.0f)));
    in.width  = JDC(g * 0.8f * punch[2]);
    in.beat_hz = thz; in.beat_conf = tconf;
    {
        // A hit: the sub kick, or a punch on the bass or mid layer.
        static float s_prev_p0 = 0.0f, s_prev_p1 = 0.0f;
        const float hit = s_band_kick > 0.02f ? s_band_kick
                        : (punch[0] > 0.30f && s_prev_p0 <= 0.30f) ? punch[0]
                        : (punch[1] > 0.40f && s_prev_p1 <= 0.40f) ? 0.6f * punch[1] : 0.0f;
        s_prev_p0 = punch[0]; s_prev_p1 = punch[1];
        if (hit > 0.0f && g > 0.0f) {
            in.onset = 1.0f;
            in.onset_strength = JDC(hit * g);
        }
    }
    #undef JDC
    {
        // ~50 ms low-pass on the continuous inputs (not the onset): punch
        // spikes from one frame to the next, and fed raw into the springs it
        // made the bell twitch.
        static float lp[8];
        static bool  lp_init = false;
        float *v[8] = { &in.sub, &in.bass, &in.lowmid, &in.mid, &in.high, &in.air, &in.rms, &in.width };
        // Fast attack, SLOW release: when the music stops every input fell
        // to zero in 50 ms and the stiff springs rang -- the shake on going
        // idle (2026-09-27).  Now it lets go over half a second.
        const float a_up = dt > 0.0f ? 1.0f - __builtin_expf(-dt / 0.05f) : 1.0f;
        const float a_dn = dt > 0.0f ? 1.0f - __builtin_expf(-dt / 0.55f) : 1.0f;
        for (int i = 0; i < 8; i++) {
            if (!lp_init) lp[i] = *v[i];
            lp[i] += (*v[i] - lp[i]) * (*v[i] > lp[i] ? a_up : a_dn);
            *v[i] = lp[i];
        }
        lp_init = true;
        // Silence eases in over ~1 s once the music has gone: the bell
        // settles and slows rather than stopping dead.
        static float s_sil = 0.0f;
        const float quiet = (in.rms < 0.015f && in.sub < 0.02f && in.mid < 0.02f) ? 1.0f : 0.0f;
        s_sil += (quiet - s_sil) * (dt > 0.0f ? 1.0f - __builtin_expf(-dt / 0.9f) : 1.0f);
        in.silence = s_sil;
    }
    if (gain <= 0.0f) in.silence = 0.0f;            // reactive off: it still breathes
    jd_motion_step(&s_jd_mo, &in, dt);
    jd_place(&s_jd_mo, s_jd_morph, aspect, 0.0f, JD_CENTRE_Y, JD_HEIGHT, &s_jd_pose);
    jd_motes_step(&s_jd_motes, dt, s_jd_mo.ts, s_band_lvl[1], in.onset_strength);
    jd_rays_step(&s_jd_rays, dt, in.rms, in.onset_strength);
}

// --- a details page's poster colour ------------------------------------------
// Same per-channel pull as the album's, but it does not breathe: it eases in
// over ~1.5 s when the page opens and holds, and eases out when it closes.
static float s_art_rgb[3] = { 1.0f, 1.0f, 1.0f };
static float s_art_k      = 0.0f;       // eased 0..1
static bool  s_art_on     = false;
static u64   s_art_us     = 0;
void wave_set_art_tint(unsigned int rgb, float strength)
{
    if (strength <= 0.0f || !rgb) { s_art_on = false; return; }
    const float r = (float)((rgb >> 16) & 0xFF), g = (float)((rgb >> 8) & 0xFF), b = (float)(rgb & 0xFF);
    const float m = (r + g + b) * (1.0f / 3.0f) + 1.0f;
    const float k[3] = { r / m, g / m, b / m };
    for (int i = 0; i < 3; i++) {
        float v = 1.0f + strength * (k[i] - 1.0f);
        if (v < 0.80f) v = 0.80f;
        if (v > 1.20f) v = 1.20f;
        s_art_rgb[i] = v;
    }
    if (s_art_rgb[1] > 1.05f) s_art_rgb[1] = 1.05f;     // never much toward green
    s_art_on = true;
}

// The album colour (music screen): a gentle per-channel pull toward the
// cover's accent, inside the purple/blue family.  1,1,1 = none.
static float s_album_rgb[3] = { 1.0f, 1.0f, 1.0f };
void wave_set_album_tint(unsigned int rgb, float strength)
{
    if (strength <= 0.0f || !rgb) { s_album_rgb[0] = s_album_rgb[1] = s_album_rgb[2] = 1.0f; return; }
    const float r = (float)((rgb >> 16) & 0xFF), g = (float)((rgb >> 8) & 0xFF), b = (float)(rgb & 0xFF);
    const float m = (r + g + b) * (1.0f / 3.0f) + 1.0f;
    float k[3] = { r / m, g / m, b / m };
    for (int i = 0; i < 3; i++) {
        float v = 1.0f + strength * (k[i] - 1.0f);
        if (v < 0.80f) v = 0.80f;                     // up to ~20%; the cycle below
        if (v > 1.20f) v = 1.20f;                     // breathes it 0 -> 20% -> 0
        s_album_rgb[i] = v;
    }
    if (s_album_rgb[1] > 1.05f) s_album_rgb[1] = 1.05f;     // never much toward green
}

static void jw_look_extras(jw_look *k);     // below: needs s_bg and the gates
static void jw_look_now(jw_look *k)
{
    k->amp[0] = s_amp[0]; k->amp[1] = s_amp[1]; k->amp[2] = s_amp[2];
    k->lum    = s_lum;
    k->lum3[0] = s_lum3[0]; k->lum3[1] = s_lum3[1]; k->lum3[2] = s_lum3[2];
    k->thick  = s_thick;
    k->acc    = s_acc;
    k->def    = s_def;
    // The album colour BREATHES: 0 -> full -> 0 over ~24 s (v11), so it is
    // seen coming and going instead of sitting as a fixed cast.
    {
        static float ph = 0.0f;
        static u64   last = 0;
        const u64 now = timing_get_us();
        if (last) ph += (float)(now - last) * 1.0e-6f / 24.0f;
        last = now;
        if (ph > 1.0f) ph -= (float)(int)ph;
        const float c = 0.5f - 0.5f * wdf_sin2pi(ph + 0.25f);    // 0 .. 1 .. 0
        for (int i = 0; i < 3; i++) k->album[i] = 1.0f + (s_album_rgb[i] - 1.0f) * c;
    }
    // The poster: in over ~1.5 s, out over ~0.8 s, eased.
    {
        const u64 now = timing_get_us();
        float dt = s_art_us ? (float)(now - s_art_us) * 1.0e-6f : 0.0f;
        s_art_us = now;
        if (dt > 0.1f) dt = 0.1f;
        if (s_art_on) { s_art_k += dt / 1.5f; if (s_art_k > 1.0f) s_art_k = 1.0f; }
        else          { s_art_k -= dt / 0.8f; if (s_art_k < 0.0f) s_art_k = 0.0f; }
        const float e = s_art_k * s_art_k * (3.0f - 2.0f * s_art_k);
        for (int i = 0; i < 3; i++) k->art[i] = 1.0f + (s_art_rgb[i] - 1.0f) * e;
    }
    // Day / night (month_bg.h): by night the ribbons are exactly as tuned; by
    // day a touch brighter and bluer, to sit in the brighter sky; the dusk
    // glow warms them for the length of the ramp.
    {
        const float d = month_bg_day(), g = month_bg_dusk();
        k->sky[0] = (1.0f + (1.04f - 1.0f) * d) * (1.0f + 0.10f * g);
        k->sky[1] = (1.0f + (1.10f - 1.0f) * d);
        k->sky[2] = (1.0f + (1.14f - 1.0f) * d) * (1.0f - 0.04f * g);
    }
    wnv_snapshot(&s_nav, &k->nav);
    k->drop = s_jd_pose;
    jw_look_extras(k);
}


// One layer's accented copy of the solver curve, rebuilt per layer just
// before the loft reads it.  One per thread that can run jw_generate().
static float          s_jw_disp[WF_SAMPLES];      // render thread
static float          s_jw_wdisp[WF_SAMPLES];     // generation worker

// Unitless displacement -> pixels.  Two normalisations, so that WAVE_AMP alone
// decides each ribbon's height:
//
//   WF_NOMINAL_PEAK  the chain's measured peak (it never reaches +/-1).
//   WF_DRIVE[li]     chain amplitude is linear in drive, and the back layers
//                    run at lower drive for calmer motion.  WAVE_AMP already
//                    tapers their height, so divide the drive back out or the
//                    taper is applied twice.
//
// Measurements behind both: docs/wave-renderer-notes.md, "Crest amplitude".
static inline float wave_field_px(int li, float fx, float W) {
    return (wf_disp(&s_field, li, fx / W) + wrm_accent_at(&s_acc, li, fx / W))
         * WAVE_AMP[li] * s_amp[li] / (WF_NOMINAL_PEAK * WF_DRIVE[li]);
}

// Solver step per wave_draw() call, in the spec's TIMESTEP units, not seconds.
// The wave advances per frame, not per wall-clock second (a real frame delta
// would be better and is a separate change).  1.25 gives WK_W1 * dt = 0.0081
// rad/frame of primary drift, the calibrated resting speed.
#define WAVE_FIELD_DT    1.25f

// The background, as four corners (see bg_gradient.h).  Rebuilt at the top of
// every draw rather than cached: it is two reads of g_theme and a struct copy,
// and caching would need invalidation on theme and day/night changes.  The
// initialiser only makes a draw before the first refresh come out black.
static bg_quad s_bg = { { 0, 0, 0, 0 } };

// 0 = the gradient as themed .. 1 = black.  Set per frame by the screen that
// wants it (the music screen's focus mode); everyone else leaves it at 0.
static float s_bg_dim = 0.0f;
void wave_set_bg_dim(float d) { s_bg_dim = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d); }

static inline void wave_bg_refresh(void) {
    // Without jellyfin_months.ini this is exactly bg_from_two(top, bot).
    s_bg = month_bg_current(XMB_BG_TOP, XMB_BG_BOT, XMB_ACCENT);
}

// Sample the background gradient at screen-space (u in [0,1], y in [0,H]),
// returning the three 8-bit channels.  Same maths as the gradient quad and
// tools/ui_preview/preview.c.
//
// Undithered on purpose: the result is an input to the ribbon blend, and
// dithering it would double the noise under the ribbons.  The dither is
// applied where the gradient becomes a pixel (wave_draw_cpu, or the RSX).
static inline void grad_sample(float u, float y, float H, u8 *r, u8 *g, u8 *b) {
    float v = (H > 1.0f) ? (y / (H - 1.0f)) : 0.0f;
    bg_sample(&s_bg, u, v, r, g, b);
}

// src over dst with 8-bit alpha: result = (src*a + dst*(255-a)) / 255.
static inline u8 over8(u8 src, u8 dst, u8 a) {
    return (u8)(((int)src * a + (int)dst * (255 - a)) / 255);
}

static u32   *s_wave_fp_buf     = NULL;
static u32    s_wave_fp_offset  = 0;

// --- vertex-array submission ---------------------------------------------
// hud_dim.cpp's layout: 4 floats of position, then colour at +16, read by the
// same passthrough vertex program (POS + COLOR0).
//
// RSX-LOCAL MEMORY STORE RULES.  Both of these hard-hang the console (black
// screen, off the network, power cycle) if broken, and both have been broken
// by nothing more than GCC rescheduling stores:
//
//   * aligned(8): sizeof is 24, not the natural 20.  At 20 bytes every odd
//     vertex sits on a 4-byte boundary, and a merged 64-bit `std` there is a
//     misaligned store to VRAM.  Do not remove the padding.
//   * colour is ONE u32, not four u8 fields: the PPU must not issue sub-word
//     stores to this memory.  Big-endian PPC stores it as r,g,b,a at +16..+19,
//     which is what the GCM_VERTEX_DATA_TYPE_U8 binding reads.
//
// Anything that writes vertex or texture data into VRAM must obey the same
// rule: whole aligned words, never bytes.  The incidents that established it:
// docs/wave-renderer-notes.md, "RSX-local store rules".
typedef struct { float x, y, z, w; u32 rgba; }
    __attribute__((aligned(8))) WaveVert;

#define WAVE_RGBA(r, g, b, a) (((u32)(r) << 24) | ((u32)(g) << 16) | \
                               ((u32)(b) <<  8) |  (u32)(a))

// Gradient quad (4) plus one strip per ribbon-slice, each two vertices per
// column.  This is the worst case; a 720p frame uses ~65 columns of it.
#define WAVE_LEGACY_VERTS  (4 + 3 * WAVE_NS * WAVE_MAX_COLS * 2)

// --- JellyWave's budget (mode 3) -----------------------------------------
//
// Each layer is emitted as ONE triangle strip covering all JW_SECTION strips
// of the lofted section, joined by degenerate pairs -- see the emit loop in
// wave_draw() for why that is safe here.  So per layer:
//
//   body  JW_SECTION strips * 2 * JW_STATIONS, plus 2 vertices per join
//   rim   6 strips (the ones carrying the rolled edge) on the same plan
//
// At JW_SECTION 12 and JW_STATIONS 80 that is 1,942 + 970 = 2,912 vertices and
// TWO draw calls per layer, so 8,740 vertices and 7 draws for the whole
// background including the gradient.  Against the legacy baked path's 4,708
// vertices and 25 draws that is 1.9x the geometry for 3.5x FEWER draw calls,
// which is the trade the degenerate joins buy.
//
// The buffer is sized for the larger of the two paths because both are
// compiled in and the gate picks between them at runtime.
#define JW_BODY_VERTS   (JW_SECTION * 2 * JW_STATIONS + (JW_SECTION - 1) * 2)
#define JW_RIM_STRIPS   6
#define JW_RIM_VERTS    (JW_RIM_STRIPS * 2 * JW_STATIONS + (JW_RIM_STRIPS - 1) * 2)
// JellyDrop's core rides in the glow range: one strip per shell, fanned as
// centre/rim pairs, joined by degenerate pairs.
#define JD_CORE_VERTS   (2 + JD_CORE_SHELLS * (2 * (JD_CORE_K + 1) + 2))
#define JW_TOTAL_VERTS  (4 + JWL_GLOW_VERTS + JD_CORE_VERTS \
                         + JW_LAYERS * (JW_BODY_VERTS + JW_RIM_VERTS + 2 * JWL_FRINGE_VERTS))

#define WAVE_MAX_VERTS  ((WAVE_LEGACY_VERTS > JW_TOTAL_VERTS) \
                         ? WAVE_LEGACY_VERTS : JW_TOTAL_VERTS)

// Two buffers, alternated on every call.  The RSX fetches the array
// asynchronously, so rebuilding the memory a queued draw has not consumed yet
// would tear the geometry.
//
// The rotation is owned here rather than taken from curr_fb deliberately:
// wave_draw() has six call sites across the XMB, the info panes, the music
// screen and the login OSK, and keying the buffer to the framebuffer index
// would alias if any frame ever drew the background twice.  Flipping per call
// gives every draw a buffer the previous one is finished with, whatever the
// call pattern, and costs a single XOR.
static WaveVert *s_wave_vbuf[2]     = { NULL, NULL };
static u32       s_wave_vbuf_off[2] = { 0, 0 };
static int       s_wave_vbuf_turn   = 0;
static bool      s_wave_varray      = false;
static bool      s_wave_blend       = false;   // mode 2
static bool      s_wave_jelly       = false;   // mode 3

// JellyWave's CPU-side scratch: ONE layer's finished vertices, reused for each
// of the three.  It is plain main memory, not the RSX buffer -- jw_vert is not
// WaveVert and deliberately never goes near video memory, so the one dangerous
// layout stays in the one file that documents why it is dangerous.
static jw_vert   s_jw[JW_VERTS];

/*
 * Finished WaveVert stream in ordinary PPU/main memory.
 *
 * IMPORTANT: this is the only place the JellyWave WaveVert stream is built.
 * The RSX-local vertex buffers are WRITE-ONLY from the PPU.  Never read a
 * WaveVert back from s_wave_vbuf[].
 */
static WaveVert s_jw_stage[WAVE_MAX_VERTS];

static u64 s_jw_upload_us = 0;
static u32 s_jw_repaired  = 0;
static u32 s_jw_dropped   = 0;

/*
 * Upload the completed stream using whole aligned 64-bit stores only.
 *
 * WaveVert is 24 bytes and aligned(8), so every vertex and the complete
 * JellyWave stream are 64-bit aligned.  may_alias prevents strict-aliasing
 * from turning this into an unsafe typed access on the PPU.
 */
typedef u64 jw_upload_word __attribute__((may_alias));

static inline void jw_upload(WaveVert *dst, const WaveVert *src, u32 count)
{
    volatile jw_upload_word *d = (volatile jw_upload_word *)dst;
    const jw_upload_word *s = (const jw_upload_word *)src;
    u32 words = (count * (u32)sizeof(WaveVert)) / 8u;
    u32 i;

    for (i = 0; i < words; i++)
        d[i] = s[i];

    __asm__ __volatile__("sync" ::: "memory");
}

// Rolling cost, logged once a second.  The xmb: frame line's `gpu` bucket
// measures SUBMISSION, not RSX work, so the only honest things to report from
// here are what the PPU spent building the geometry and how much of it there
// was; the GPU's own cost shows up as `vsync` shrinking, which is read on the
// far side.
static u64 s_jw_gen_us   = 0;
static u32 s_jw_frames   = 0;
static u32 s_jw_rebuilds = 0;
static u32 s_jw_verts    = 0;
static u32 s_jw_draws    = 0;

// The draw ranges of whatever is CURRENTLY in the vertex buffer: [slot][0] is
// the body, [slot][1] the rim, slot 0 the furthest layer.  These are file
// scope rather than locals because a reuse frame draws last build's ranges --
// see JW_REBUILD below.
static u32 s_jw_off[JW_LAYERS][2];
static u32 s_jw_cnt[JW_LAYERS][2];
static int s_jw_have_geom = 0;     // 0 until the first build lands
static u32 s_jw_vo_frame  = 0;     // the buffer wave_draw() drew from this frame
static bool s_jw_drawn    = false; // ... and whether it drew JellyWave at all

// --- the snow field (wave_snow.h), while music plays -------------------------
//
// Screen-wide particles drifting down with depth of field -- bokeh near, sharp
// in the middle, faint far -- swaying with the lows, twinkling with the highs
// and thrown up and outward by every sub-bass hit.  Fades in with the music and
// out with it; with no audio nothing is simulated, shaded or drawn.
//
// Drawn from buffers of their OWN -- never the JellyWave stage or its ranges.
// Built in main memory, uploaded with jw_upload() right after the JellyWave
// upload (whose rsxSync is the fence for both), drawn once after the gel with
// an ADDITIVE blend.  A tiny particle is one flat quad; a larger one a fan,
// bright centre to transparent rim (4 segments, 6 for the bokeh).
#define MOTE_VERTS  ((WS_MAX + JD_MOTES + JD_RAYS) * 6 * 3)
static ws_state    s_snow;
static ws_sprite   s_snow_spr[WS_MAX];
static WaveVert    s_mote_stage[MOTE_VERTS] __attribute__((aligned(16)));
static WaveVert   *s_mote_vbuf[2] = { NULL, NULL };
static u32         s_mote_voff[2] = { 0, 0 };
static u32         s_mote_n  = 0;          // vertices uploaded by this call
static u32         s_mote_far_n = 0;       // ... of which [0, this) are behind the gel
static bool        s_jw_look_on = true;    // jellyfin_jwlook.txt (see below)
// Particles at least this deep sit BEHIND the ribbons (wave_snow.h's z:
// 0 nearest .. 1 furthest).  The far ones are the small, dim, sharp-to-faint
// half of the field, which is what reads as further away than the gel.
#define WS_BEHIND_Z 0.55f
static bool        s_mote_ok = false;
static float       s_mote_a  = 0.0f;       // the eased fade, 0..1
static float       s_mote_lin = 0.0f;      // the fade's linear ramp, 0..1
static float       s_mote_lvl = 1.0f;      // 1 with music, dimmer as menu ambience
static bool        s_mote_music = false;   // music gate, with hysteresis
static float       s_mote_quiet = 0.0f;    // s of silence while the gate is open
static bool        s_mote_amb_req = false; // wave_snow_ambient() this frame
static u64         s_mote_us = 0;

void wave_snow_ambient(void) { s_mote_amb_req = true; }

static ws_rect s_obst_px[WS_OBST_MAX];   // x0,y0,x1,y1 in px (y down) until built
static int     s_obst_n = 0;
void wave_snow_obstacles(const int *xywh, int n)
{
    if (!xywh || n < 0) n = 0;
    if (n > WS_OBST_MAX) n = WS_OBST_MAX;
    for (int i = 0; i < n; i++) {
        s_obst_px[i].x0 = (float)xywh[4 * i];
        s_obst_px[i].y0 = (float)xywh[4 * i + 1];
        s_obst_px[i].x1 = (float)(xywh[4 * i] + xywh[4 * i + 2]);
        s_obst_px[i].y1 = (float)(xywh[4 * i + 1] + xywh[4 * i + 3]);
    }
    s_obst_n = n;
}

static void motes_init(void)
{
    for (int i = 0; i < 2; i++) {
        s_mote_vbuf[i] = (WaveVert *)rsxMemalign(128, MOTE_VERTS * sizeof(WaveVert));
        if (!s_mote_vbuf[i]) { plog("wave: snow buffer alloc FAILED -- no particles"); return; }
        rsxAddressToOffset(s_mote_vbuf[i], &s_mote_voff[i]);
    }
    ws_init(&s_snow, WS_COUNT_DEF, 0x4A454C4Cu);
    s_mote_ok = true;
    plog("wave: snow ready (700, shown while music plays)");
}

static inline void mote_v(WaveVert *v, float x, float y, u32 c)
{
    v->x = x; v->y = y; v->z = 0.0f; v->w = 1.0f; v->rgba = c;
}

// Step, shade and build this call's particles into s_mote_stage; returns the
// vertex count (0 = draw nothing).
static u32 motes_build(float aspect)
{
    const u64 now = timing_get_us();
    float dt = s_mote_us ? (float)(now - s_mote_us) * 1.0e-6f : 0.0f;
    s_mote_us = now;
    if (dt > WS_DT_MAX) dt = WS_DT_MAX;

    // Who wants the snow: music (gated with hysteresis, so a quiet intro or a
    // gap between tracks does not flicker it), or a menu with Menu Particles
    // on.  The fade is a linear ramp through an ease curve -- in over ~2.5 s,
    // out over ~1.3 s -- and wave_snow.h staggers it per particle, so they
    // arrive one by one rather than as a sheet.
    const float pres = wave_audio_presence();
    if (pres > 0.05f) { s_mote_music = true; s_mote_quiet = 0.0f; }
    else if (s_mote_music && (s_mote_quiet += dt) > 1.2f) s_mote_music = false;
    const bool amb = s_mote_amb_req && menusnow_enabled();
    s_mote_amb_req = false;
    const float want = (s_mote_music || amb) ? 1.0f : 0.0f;
    const float lvl_want = s_mote_music ? 1.0f : 0.75f;
    if (want > s_mote_lin) { s_mote_lin += dt / 2.5f; if (s_mote_lin > want) s_mote_lin = want; }
    else                   { s_mote_lin -= dt / 1.3f; if (s_mote_lin < want) s_mote_lin = want; }
    s_mote_lvl += (lvl_want - s_mote_lvl) * (dt / (0.8f + dt));
    s_mote_a = s_mote_lin * s_mote_lin * (3.0f - 2.0f * s_mote_lin) * s_mote_lvl;
    // In JellyDrop the only particles are the bell's own orbit (2026-09-27):
    // the wide field fades out as the bell forms and back as it opens.
    const float jd_hide = jw_smooth(0.15f, 0.6f, s_jd_morph);
    s_mote_a *= 1.0f - jd_hide;
    float lvl[3], kick = s_band_kick;
    lvl[0] = s_band_lvl[0]; lvl[1] = s_band_lvl[1]; lvl[2] = s_band_lvl[2];
    // JellyDrop's orbit is on whenever the bell is (music or not): it is what
    // gives the bell its depth.
    const float jd_vis = jw_smooth(0.55f, 1.0f, s_jd_morph);
    const bool  snow   = s_mote_lin > 0.0f && jd_hide < 0.999f;
    if (!snow && !(jd_vis > 0.0f)) return 0;

    // The near layer's vertical velocity across the screen, in clip units/s
    // (+up), so the particles near the band ride its motion -- ripples too.
    static float s_wy_prev[WS_WV];
    static float s_wv[WS_WV];
    static bool  s_wy_have = false;
    {
        const float Wd = (float)display_width, Hd = (float)display_height;
        for (int k = 0; k < WS_WV; k++) {
            const float fx = Wd * (float)k / (float)(WS_WV - 1);
            const float y  = -2.0f * wave_field_px(0, fx, Wd) / Hd;
            float v = (s_wy_have && dt > 0.0f) ? (y - s_wy_prev[k]) / dt : 0.0f;
            if (v >  1.5f) v =  1.5f;
            if (v < -1.5f) v = -1.5f;
            s_wv[k] += (v - s_wv[k]) * 0.5f;       // one frame of smoothing
            s_wy_prev[k] = y;
        }
        s_wy_have = true;
    }

    ws_ctl c;
    ws_rect obst[WS_OBST_MAX];
    {
        const float Wd = (float)display_width, Hd = (float)display_height;
        for (int i = 0; i < s_obst_n; i++) {      // px (y down) -> clip (y up)
            obst[i].x0 = 2.0f * s_obst_px[i].x0 / Wd - 1.0f;
            obst[i].x1 = 2.0f * s_obst_px[i].x1 / Wd - 1.0f;
            obst[i].y0 = 1.0f - 2.0f * s_obst_px[i].y1 / Hd;
            obst[i].y1 = 1.0f - 2.0f * s_obst_px[i].y0 / Hd;
        }
    }
    // The youngest live warp becomes the particles' shockwave ring.
    c.ring_a = 0.0f; c.ring_x = c.ring_y = c.ring_r = 0.0f;
    {
        int best = -1;
        for (int j = 0; j < WDF_WARPS; j++)
            if (s_def.live && s_def.warp_a[j] > 0.0f && s_def.warp_age[j] < WDF_WARP_LIFE &&
                (best < 0 || s_def.warp_age[j] < s_def.warp_age[best]))
                best = j;
        if (best >= 0) {
            const float age = s_def.warp_age[best];
            c.ring_x = 2.0f * (s_def.warp_x[best] + s_def.warp_dir[best] * 0.10f * age) - 1.0f;
            c.ring_y = 1.0f - 2.0f * WAVE_BASEY[0];
            c.ring_r = 0.95f * age;
            c.ring_a = (s_def.warp_a[best] / WDF_WARP_A) * wdf_warp_env(age) * 0.9f;
        }
    }
    c.calm    = s_def.live ? s_def.calm : 0.0f;
    c.obst    = s_obst_n ? obst : 0;
    c.n_obst  = s_obst_n;
    s_obst_n  = 0;                            // one frame only
    c.wv      = s_wv;
    c.band_y  = 1.0f - 2.0f * WAVE_BASEY[0];
    c.sway    = lvl[0];
    c.twinkle = lvl[2];
    c.kick    = kick;
    c.bright  = (lvl[0] + lvl[1] + lvl[2]) * (1.0f / 3.0f);
    c.wind_x  = s_nav_fx.wind_x;              // the pad (wave_nav.h)
    c.wind_y  = s_nav_fx.wind_y;
    c.jostle  = s_nav_fx.jostle;
    if (snow) {
        ws_step(&s_snow, &c, dt);
        ws_shade(&s_snow, &c, s_mote_a, s_snow_spr, WS_MAX);
    } else {
        for (int i = 0; i < s_snow.n; i++) s_snow_spr[i].a = 0;
    }
    static jd_sprite s_jd_spr[JD_MOTES + JD_RAYS];
    for (int i = 0; i < JD_MOTES; i++) {
        s_jd_spr[i].a = 0;
        if (jd_vis > 0.0f)
            jd_motes_sprite(&s_jd_motes, i, &s_jd_pose, aspect, jd_vis, lvl[2], &s_jd_spr[i]);
    }
    for (int i = 0; i < JD_RAYS; i++) {
        s_jd_spr[JD_MOTES + i].a = 0;
        if (jd_vis > 0.0f)
            jd_rays_sprite(&s_jd_rays, i, &s_jd_pose, aspect, jd_vis, lvl[2], &s_jd_spr[JD_MOTES + i]);
    }

    static const float C4[5] = { 1.0f, 0.0f, -1.0f, 0.0f, 1.0f };
    static const float S4[5] = { 0.0f, 1.0f, 0.0f, -1.0f, 0.0f };
    static const float C6[7] = { 1.0f, 0.5f, -0.5f, -1.0f, -0.5f, 0.5f, 1.0f };
    static const float S6[7] = { 0.0f, 0.8660254f, 0.8660254f, 0.0f,
                                 -0.8660254f, -0.8660254f, 0.0f };
    const float px2y = 1.0f / 540.0f;              // 1080p pixels -> clip y
    const float inv_aspect = 1.0f / aspect;
    u32 n = 0;
    // Two passes: the FAR particles first (drawn behind the ribbons), then
    // the near ones (in front).  s_mote_far_n is where the split falls.  With
    // the look pass off every particle is "near", as before.
    s_mote_far_n = 0;
    for (int mp = 0; mp < 2; mp++) {
    if (mp == 1) s_mote_far_n = n;
    for (int i = 0; i < s_snow.n; i++) {
        const ws_sprite *m = &s_snow_spr[i];
        if (!m->a) continue;
        const bool far_p = s_jw_look_on && s_snow.z[i] >= WS_BEHIND_Z;
        if (far_p != (mp == 0)) continue;
        const u32 cc = WAVE_RGBA(m->r, m->g, m->b, m->a);
        const u32 ce = WAVE_RGBA(m->r, m->g, m->b, 0);
        if (m->r_px < 2.2f) {                          // a speck: one flat quad
            const float sy = m->r_px * px2y, sx = sy * inv_aspect;
            WaveVert *v = &s_mote_stage[n];
            mote_v(&v[0], m->x - sx, m->y - sy, cc);
            mote_v(&v[1], m->x + sx, m->y - sy, cc);
            mote_v(&v[2], m->x + sx, m->y + sy, cc);
            mote_v(&v[3], m->x - sx, m->y - sy, cc);
            mote_v(&v[4], m->x + sx, m->y + sy, cc);
            mote_v(&v[5], m->x - sx, m->y + sy, cc);
            n += 6;
            continue;
        }
        const bool big = m->r_px >= 7.0f;
        const int  seg = big ? 6 : 4;
        const float *CS = big ? C6 : C4, *SN = big ? S6 : S4;
        const float sy = m->r_px * 1.5f * px2y, sx = sy * inv_aspect;   // soft rim past the core
        for (int k = 0; k < seg; k++) {
            WaveVert *v = &s_mote_stage[n];
            mote_v(&v[0], m->x, m->y, cc);
            mote_v(&v[1], m->x + sx * CS[k],     m->y + sy * SN[k],     ce);
            mote_v(&v[2], m->x + sx * CS[k + 1], m->y + sy * SN[k + 1], ce);
            n += 3;
        }
    }
    // JellyDrop's orbit: the far half in the behind pass, the near half in
    // front, as soft fans (a speck when small), same blend as the snow.
    for (int i = 0; i < JD_MOTES + JD_RAYS; i++) {
        const jd_sprite *o = &s_jd_spr[i];
        if (!o->a) continue;
        if ((o->front != 0) != (mp == 1)) continue;
        if (n + 18 > MOTE_VERTS) break;
        const u32 cc = WAVE_RGBA(o->r, o->g, o->b, o->a);
        const u32 ce = WAVE_RGBA(o->r, o->g, o->b, 0);
        if (o->r_px < 2.2f) {
            const float sy = o->r_px * px2y, sx = sy * inv_aspect;
            WaveVert *v = &s_mote_stage[n];
            mote_v(&v[0], o->x - sx, o->y - sy, cc);
            mote_v(&v[1], o->x + sx, o->y - sy, cc);
            mote_v(&v[2], o->x + sx, o->y + sy, cc);
            mote_v(&v[3], o->x - sx, o->y - sy, cc);
            mote_v(&v[4], o->x + sx, o->y + sy, cc);
            mote_v(&v[5], o->x - sx, o->y + sy, cc);
            n += 6;
            continue;
        }
        const bool big = o->r_px >= 7.0f;
        const int  seg = big ? 6 : 4;
        const float *CS = big ? C6 : C4, *SN = big ? S6 : S4;
        const float sy = o->r_px * 1.5f * px2y, sx = sy * inv_aspect;
        for (int k = 0; k < seg; k++) {
            WaveVert *v = &s_mote_stage[n];
            mote_v(&v[0], o->x, o->y, cc);
            mote_v(&v[1], o->x + sx * CS[k],     o->y + sy * SN[k],     ce);
            mote_v(&v[2], o->x + sx * CS[k + 1], o->y + sy * SN[k + 1], ce);
            n += 3;
        }
    }
    }
    return n;
}

// The rim pass colour, times the treble's rim gain (exactly the old colour at
// rest: the gain is only applied while the deformation is live).
// Per-channel gains (the tint) times a brightness gain (the glow / rim).
// Exactly the stored colour when every gain is 1.
static float s_tint_rgb[3] = { 1.0f, 1.0f, 1.0f };   // set per build, see jw_generate
static inline u8 jw_c8(float v) { return v >= 255.0f ? 255 : (u8)v; }
static inline u32 jw_body_rgba(const jw_vert *q, int a, float g, const float *t)
{
    if (g == 1.0f && t[0] == 1.0f && t[1] == 1.0f && t[2] == 1.0f)
        return WAVE_RGBA(q->r, q->g, q->b, a);
    return WAVE_RGBA(jw_c8((float)q->r * g * t[0]), jw_c8((float)q->g * g * t[1]),
                     jw_c8((float)q->b * g * t[2]), a);
}

static inline u32 jw_rim_rgba(const jw_vert *q, float g, const float *t)
{
    if (g == 1.0f && t[0] == 1.0f && t[1] == 1.0f && t[2] == 1.0f)
        return WAVE_RGBA(q->rr, q->rg, q->rb, 255);
    return WAVE_RGBA(jw_c8((float)q->rr * g * t[0]), jw_c8((float)q->rg * g * t[1]),
                     jw_c8((float)q->rb * g * t[2]), 255);
}

// Build the complete JellyWave stream into dst, starting at index n (the
// gradient quad owns [0,4)), from one snapshot of the field's sampled
// displacement.  Writes every layer's draw ranges into off/cnt and returns the
// new vertex count.  Pure: it reads only its arguments and wave_gel.h's
// constants, so it runs unchanged on the render thread (the first build) or
// on the generation worker (every build after it).  Nothing here touches the
// RSX-local buffers.
// What a build produces besides the layer ranges (the look pass, 2026-09-26):
// the near ribbon's glow range, and the signature that says whether this
// build can be blended vertex-for-vertex with the one before it.
struct jw_extra {
    u32 glow_off, glow_cnt;
    u32 sig;
};

// The stage's own extra (what s_jw_stage holds), and whether this rebuild
// call put a new build there.
static jw_extra s_jw_ex_stage = { 0, 0, 0 };
static bool     s_jw_fresh    = false;

static int jw_generate(WaveVert *dst, int n, const float (*sy)[WF_SAMPLES],
                       const jw_look *look, float *disp,
                       float aspect, jw_vert *scratch,
                       u32 off[JW_LAYERS][2], u32 cnt[JW_LAYERS][2],
                       jw_extra *ex,
                       u32 *repaired_out, u32 *dropped_out)
{
    // G: the travelling glow's gain at this station (1 = none).
    #define JW_EMIT(Q, A, USE_RIM, G) JW_EMIT2(Q, A, USE_RIM, G, G)
    // GR: the rim's own gain (the glow times the hi-hat glints)
    #define JW_EMIT2(Q, A, USE_RIM, G, GR) do {                 \
        const jw_vert *q_ = (Q);                                \
        dst[n].x = q_->x;                                       \
        dst[n].y = q_->y;                                       \
        dst[n].z = 0.0f;                                        \
        dst[n].w = 1.0f;                                        \
        dst[n].rgba = (USE_RIM)                                 \
            ? jw_rim_rgba(q_, rimg * (GR), tint)                \
            : jw_body_rgba(q_, (A), (G), tint);                 \
        n++;                                                    \
    } while (0)
    // A vertex from explicit values (the fringe and the glow).
    #define JW_PUT(X, Y, RGBA) do {                             \
        dst[n].x = (X); dst[n].y = (Y);                         \
        dst[n].z = 0.0f; dst[n].w = 1.0f;                       \
        dst[n].rgba = (RGBA);                                   \
        n++;                                                    \
    } while (0)

    const float rimg = look->def.live ? look->def.rim : 1.0f;
    // Tint: a dark mix leans violet, a bright one blue -- both inside the
    // palette, a few percent per channel.  Local: this runs on the worker.
    float tint[3] = { 1.0f, 1.0f, 1.0f };
    if (look->def.live && look->def.tint != 0.0f) {
        const float t = look->def.tint;
        const float dk = t < 0.0f ? -t : 0.0f, br = t > 0.0f ? t : 0.0f;
        tint[0] = 1.0f + 0.20f * dk - 0.14f * br;       // v9: stronger (was 0.12 / 0.08)
        tint[1] = 1.0f - 0.10f * dk + 0.08f * br;
        tint[2] = 1.0f + 0.05f * dk + 0.16f * br;
    }
    (void)s_tint_rgb;
    // the album's colour, a hint on top
    tint[0] *= look->album[0]; tint[1] *= look->album[1]; tint[2] *= look->album[2];
    // ... and a details page's poster, and the time of day (1,1,1 at night)
    tint[0] *= look->art[0] * look->sky[0];
    tint[1] *= look->art[1] * look->sky[1];
    tint[2] *= look->art[2] * look->sky[2];

    // The look pass's per-build state.
    const bool lk     = look->look_on != 0;
    const float fdy   = look->px_y * JWL_FRINGE_PX;     // the fringe, in clip y
    u32  sig          = 0x4A57u;
    bool near_ok      = false;                          // the near layer is in scratch
    int  n_top[JW_STATIONS], n_bot[JW_STATIONS];        // its silhouette, for the glow
    ex->glow_off = ex->glow_cnt = 0;

    for (int slot = 0; slot < JW_LAYERS; slot++) {
        const int       li = JW_LAYERS - 1 - slot;
        // The audio look for this layer, on a copy: disp_gain scales the
        // solver's displacement, bright the body and rim colour, scale the
        // width and thickness -- all before the loft computes a vertex, so
        // jw_build_layer, the sanitizer, the strip order and the emit below
        // see an ordinary layer.  The accent goes into a copy of the curve,
        // never the solver.  At rest every multiplier is exactly 1.0 and the
        // accent adds exactly 0.0f, so the build is bit-identical to spine14.
        jw_layer        Lk = JW_LAYER[li];
        Lk.disp_gain *= look->amp[li];
        Lk.bright    *= look->lum * look->lum3[li];
        Lk.scale     *= look->thick;
        // stereo width: a wide mix spreads the ribbons further apart.
        // VERTICALLY only: pushing a ribbon deeper moves it toward the
        // horizon, i.e. UP the screen (the framing test caught that); a
        // larger negative y offset only moves it down, away from the content.
        if (look->def.live && look->def.width > 0.0f)
            Lk.y_off *= 1.0f + 0.50f * look->def.width;
        // the pad's bob: only ever down (wave_nav.h), so the same argument holds
        Lk.y_off += look->nav.bob[li];
        // JellyDrop: the layer's one alpha moves toward the bell's
        Lk.alpha = jd_layer_alpha(&Lk, li, look->drop.morph);
        const jw_layer *L  = &Lk;
        int order[JW_SECTION];
        int top[JW_STATIONS], bot[JW_STATIONS];
        int pass, s, i;

        cnt[slot][0] = cnt[slot][1] = 0;
        off[slot][0] = off[slot][1] = 0;

        wrm_accent(&look->acc, li, sy[li], disp, WF_SAMPLES);
        // JellyWave 2.x shape; the neighbouring layer's curve bends this one
        wdf_apply2(&look->def, li, disp, sy[li == 0 ? 1 : li - 1], WF_SAMPLES);
        // the pad's pushes: dips only, after the audio so they ride on top
        wnv_apply(&look->nav, li, disp, WF_SAMPLES);
        float glow[JW_STATIONS];
        const bool glow_on = wdf_glow(&look->def, li, glow, JW_STATIONS) != 0;
        float rimg_st[JW_STATIONS];
        const bool rim_on = wdf_rim_glow(&look->def, li, rimg_st, JW_STATIONS) != 0;
        // The key light, drifting (wave_light.h's jw_key_drift) -- or fixed.
        // ... or JellyDrop's loft of the same vertices round the bell
        // (wave_drop.h), which is this exact call while morph is 0.
        if (!jd_build_layer_k(L, li, disp, WF_SAMPLES, aspect, look->key, &look->drop,
                              scratch, JW_VERTS)) {
            sig = jwl_sig_mix(sig, 0xDEADu + (u32)slot);
            continue;
        }

        {
            int repaired = 0;
            if (!jw_sanitize_layer(scratch, &repaired)) {
                (*dropped_out)++;
                sig = jwl_sig_mix(sig, 0xDEADu + (u32)slot);
                continue;
            }
            *repaired_out += (u32)repaired;
        }

        // Aerial perspective: the further layers take on the sky behind them.
        // After the sanitizer (every vertex is on screen-ish and finite), and
        // before anything reads a colour.
        if (lk && JWL_HAZE[li] > 0.0f) {
            for (int k = 0; k < JW_VERTS; k++) {
                jw_vert *q = &scratch[k];
                float br, bgc, bb;
                bg_sample_f(&look->bg, (q->x + 1.0f) * 0.5f, (1.0f - q->y) * 0.5f, &br, &bgc, &bb);
                jwl_haze(q, br, bgc, bb, JWL_HAZE[li]);
            }
        }

        jw_strip_order(scratch, order);
        for (s = 0; s < JW_SECTION; s++) sig = jwl_sig_mix(sig, (u32)order[s]);

        // The outline, once per layer: the fringe below uses it, and the
        // near layer's also feeds the glow.
        for (i = 0; i < JW_STATIONS; i++) {
            if (!jwl_silhouette(scratch, i, &top[i], &bot[i])) {
                top[i] = i ? top[i - 1] : 3;          // cannot happen after the sanitizer
                bot[i] = i ? bot[i - 1] : 9;
            }
        }

        for (pass = 0; pass < 2; pass++) {
            int started = 0;
            const jw_vert *tail = NULL;

            off[slot][pass] = (u32)n;

            for (s = 0; s < JW_SECTION; s++) {
                const int j  = order[s];
                const int j2 = (j + 1) % JW_SECTION;

                if (pass == 1 && !jw_strip_has_rim(j))
                    continue;

                if (n + 2 * JW_STATIONS + 2 > WAVE_MAX_VERTS)
                    break;

                if (started) {
                    /*
                     * Degenerate join, entirely from CPU/main memory.
                     * NEVER read back from the RSX-local destination.
                     */
                    JW_EMIT(tail, L->alpha, pass, 1.0f);
                    JW_EMIT(&scratch[0 * JW_SECTION + j], L->alpha, pass, 1.0f);
                }

                started = 1;

                for (i = 0; i < JW_STATIONS; i++) {
                    const float g_  = glow_on ? glow[i] : 1.0f;
                    const float gr_ = rim_on ? g_ * rimg_st[i] : g_;
                    JW_EMIT2(&scratch[i * JW_SECTION + j],  L->alpha, pass, g_, gr_);
                    JW_EMIT2(&scratch[i * JW_SECTION + j2], L->alpha, pass, g_, gr_);
                }

                tail = &scratch[(JW_STATIONS - 1) * JW_SECTION + j2];
            }

            // THE FRINGE -- edge anti-aliasing.  Along the top and the bottom
            // outline, a strip JWL_FRINGE_PX wide going OUTWARD, from the edge
            // vertex's own colour at the layer's alpha to the same colour at
            // alpha 0: the stair-steps of the hard polygon edge get a ramp to
            // fall into.  Same pass, same blend, so it rides in this range with
            // no extra draw: the body's fringe on the standard blend, the rim's
            // (the lit edge, light to ADD) on the additive one.
            // Not on JellyDrop: a bell has no top and bottom outline.
            if (lk && started && !(look->drop.morph > 0.0f)) {
                for (int side = 0; side < 2; side++) {
                    if (n + 2 * JW_STATIONS + 2 > WAVE_MAX_VERTS) break;
                    const int  *idx = side ? bot : top;
                    const float dy  = side ? -fdy : fdy;
                    const jw_vert *first = &scratch[0 * JW_SECTION + idx[0]];
                    // Degenerate join: the LAST VERTEX EMITTED, repeated (after
                    // the first fringe that is an outer, alpha-0 vertex, not a
                    // scratch point -- repeating the inner one instead makes a
                    // real triangle across the screen), then this strip's first.
                    // dst is main memory, the stage or the worker's buffer.
                    { const WaveVert last = dst[n - 1]; JW_PUT(last.x, last.y, last.rgba); }
                    JW_EMIT(first, L->alpha, pass, 1.0f);
                    for (i = 0; i < JW_STATIONS; i++) {
                        const jw_vert *q = &scratch[i * JW_SECTION + idx[i]];
                        const float g_  = glow_on ? glow[i] : 1.0f;
                        const float gr_ = rim_on ? g_ * rimg_st[i] : g_;
                        const u32 c = pass ? jw_rim_rgba(q, rimg * gr_, tint)
                                           : jw_body_rgba(q, L->alpha, g_, tint);
                        JW_PUT(q->x, q->y, c);
                        JW_PUT(q->x, q->y + dy, c & 0xFFFFFF00u);
                    }
                }
            }

            cnt[slot][pass] = (u32)n - off[slot][pass];
            sig = jwl_sig_mix(sig, cnt[slot][pass]);
        }

        if (li == 0) {
            near_ok = true;
            for (i = 0; i < JW_STATIONS; i++) { n_top[i] = top[i]; n_bot[i] = bot[i]; }
        }
    }

    // THE GLOW -- the near ribbon's light on the sky around it.  Emitted after
    // every layer (the near one is the last built, so scratch still holds it)
    // but DRAWN first, before any layer, additive: it lights the background
    // and shows through the gel.  Two strips per station column, from the
    // band's centre line (at JWL_GLOW_A) to nothing JWL_GLOW_UP above and
    // JWL_GLOW_DOWN below, in the station's own average body colour -- so it
    // follows the palette, the tint and the music's brightness for free.
    // JellyDrop fades it out over the first half of the morph: a ring has no
    // band for it to light.
    const float glow_k = 1.0f - jw_smooth(0.0f, 0.5f, look->drop.morph);
    const u32   glow_a = (u32)((float)JWL_GLOW_A * glow_k + 0.5f);
    if (lk && near_ok && glow_a && n + JWL_GLOW_VERTS <= WAVE_MAX_VERTS) {
        float cx[JW_STATIONS], cy[JW_STATIONS];
        u32   cc[JW_STATIONS];
        for (int i = 0; i < JW_STATIONS; i++) {
            const jw_vert *t = &scratch[i * JW_SECTION + n_top[i]];
            const jw_vert *b = &scratch[i * JW_SECTION + n_bot[i]];
            cx[i] = 0.5f * (t->x + b->x);
            cy[i] = 0.5f * (t->y + b->y);
            float r = 0.0f, g = 0.0f, bl = 0.0f;
            for (int j = 0; j < JW_SECTION; j++) {
                const jw_vert *q = &scratch[i * JW_SECTION + j];
                r += q->r; g += q->g; bl += q->b;
            }
            const float k = 1.0f / (float)JW_SECTION;
            cc[i] = WAVE_RGBA(jw_c8(r * k * tint[0]), jw_c8(g * k * tint[1]),
                              jw_c8(bl * k * tint[2]), 0);
        }
        ex->glow_off = (u32)n;
        for (int i = 0; i < JW_STATIONS; i++) {                 // above
            JW_PUT(cx[i], cy[i], cc[i] | glow_a);
            JW_PUT(cx[i], cy[i] + JWL_GLOW_UP, cc[i]);
        }
        JW_PUT(cx[JW_STATIONS - 1], cy[JW_STATIONS - 1] + JWL_GLOW_UP, cc[JW_STATIONS - 1]);  // join
        JW_PUT(cx[0], cy[0], cc[0] | glow_a);
        for (int i = 0; i < JW_STATIONS; i++) {                 // below
            JW_PUT(cx[i], cy[i], cc[i] | glow_a);
            JW_PUT(cx[i], cy[i] - JWL_GLOW_DOWN, cc[i]);
        }
        ex->glow_cnt = (u32)n - ex->glow_off;
    }

    // THE CORE -- JellyDrop's lit hole (wave_drop.h): three nested shells of
    // the hole's own contour, additive, bright centre to a dimmer (shell 0) or
    // vanishing (the outer two) rim.  Emitted into the glow's range -- the
    // same additive draw, before every layer -- as centre/rim pairs, which a
    // strip turns into a fan (every other triangle is degenerate).
    if (jd_core_on(&look->drop) > 0.002f && n + JD_CORE_VERTS <= WAVE_MAX_VERTS) {
        const float CR = 0.62f, CG = 0.86f, CB = 1.00f;     // lilac-cyan light
        const float tr = tint[0], tg = tint[1], tb = tint[2];
        if (!ex->glow_cnt) ex->glow_off = (u32)n;
        bool any = ex->glow_cnt != 0;
        for (int k = 0; k < JD_CORE_SHELLS; k++) {
            float ccx, ccy, rx[JD_CORE_K], ry[JD_CORE_K], gc, gr, ac, ar;
            if (!jd_core_shell(&look->drop, k, aspect, &ccx, &ccy, rx, ry, &gc, &gr, &ac, &ar))
                continue;
            const u32 cc = WAVE_RGBA(jw_u8(CR * gc * tr), jw_u8(CG * gc * tg), jw_u8(CB * gc * tb),
                                     jw_u8(ac));
            const u32 cr = WAVE_RGBA(jw_u8(CR * gr * tr), jw_u8(CG * gr * tg), jw_u8(CB * gr * tb),
                                     jw_u8(ar));
            if (any) {                                      // degenerate join
                const WaveVert last = dst[n - 1];
                JW_PUT(last.x, last.y, last.rgba);
                JW_PUT(ccx, ccy, cc);
            }
            for (int i = 0; i <= JD_CORE_K; i++) {
                JW_PUT(ccx, ccy, cc);
                JW_PUT(rx[i % JD_CORE_K], ry[i % JD_CORE_K], cr);
            }
            any = true;
        }
        ex->glow_cnt = (u32)n - ex->glow_off;
    }
    sig = jwl_sig_mix(sig, ex->glow_cnt);
    ex->sig = sig;
    #undef JW_EMIT
    #undef JW_EMIT2
    #undef JW_PUT
    return n;
}

// --- the generation worker ------------------------------------------------
//
// A JellyWave build costs ~6.3 ms of PPU; on the render thread that made
// every rebuild frame miss vsync on busy screens (docs/wave-renderer-notes.md,
// "JellyWave cost").  The PPU has two hardware threads and the render thread
// uses one, so the build runs on the other: on a rebuild call the render thread snapshots
// the field's sampled displacement (3 x WF_SAMPLES floats) and hands it over;
// the worker builds the complete stream into its OWN buffer; the next rebuild
// call copies the finished stream into the stage (~0.1 ms) and queues the
// next.  The geometry is therefore one rebuild interval old when drawn, which
// on a wave this slow is invisible.
//
// What does NOT change, deliberately: the stage is still written only on a
// rebuild call, on the render thread; a reuse call still writes nothing; the
// fence, the buffer rotation and jw_upload() are untouched.  The worker never
// sees the stage or the RSX buffers.  If a build is not finished when the next
// rebuild call comes round, that call keeps the previous geometry (the stage
// is simply not rewritten) -- it never waits.
enum { JW_JOB_IDLE = 0, JW_JOB_QUEUED = 1, JW_JOB_DONE = 2 };
static WaveVert         s_jw_back[WAVE_MAX_VERTS] __attribute__((aligned(16)));
static jw_vert          s_jw_wscratch[JW_VERTS];
static float            s_jw_job_sy[WF_LAYERS][WF_SAMPLES];
static float            s_jw_job_aspect = 1.0f;
static jw_look          s_jw_job_look;
static u32              s_jw_back_off[JW_LAYERS][2];
static u32              s_jw_back_cnt[JW_LAYERS][2];
static jw_extra         s_jw_back_ex;
static int              s_jw_back_n = 0;
static u32              s_jw_back_repaired = 0, s_jw_back_dropped = 0;
static u64              s_jw_back_us = 0;
static volatile int     s_jw_job = JW_JOB_IDLE;
static volatile bool    s_jw_worker_run = false;
static bool             s_jw_worker = false;
static sys_ppu_thread_t s_jw_tid;
static u32              s_jw_late = 0;     // rebuild calls that found no build ready

static void jw_worker_fn(void *arg)
{
    (void)arg;
    while (s_jw_worker_run) {
        if (s_jw_job != JW_JOB_QUEUED) { usleep(1000); continue; }
        __sync_synchronize();                  // the snapshot, before reading it
        const u64 t0 = timing_get_us();
        u32 rep = 0, drop = 0;
        const int m = jw_generate(s_jw_back, 4, s_jw_job_sy,
                                  &s_jw_job_look, s_jw_wdisp, s_jw_job_aspect,
                                  s_jw_wscratch, s_jw_back_off, s_jw_back_cnt,
                                  &s_jw_back_ex, &rep, &drop);
        s_jw_back_n        = m;
        s_jw_back_repaired = rep;
        s_jw_back_dropped  = drop;
        s_jw_back_us       = timing_get_us() - t0;
        __sync_synchronize();                  // the build lands before the flag
        s_jw_job = JW_JOB_DONE;
    }
    sysThreadExit(0);
}

static void jw_worker_start(void)
{
    if (s_jw_worker) return;
    s_jw_job = JW_JOB_IDLE;
    s_jw_worker_run = true;
    static char name[] = "jf_jwgen";
    // Below the render thread's priority: it runs on the PPU's other hardware
    // thread, and must never be what delays a frame.
    s32 rc = sysThreadCreate(&s_jw_tid, jw_worker_fn, NULL, 1200, 64 * 1024,
                             THREAD_JOINABLE, name);
    if (rc != 0) {
        s_jw_worker_run = false;
        char b[96];
        snprintf(b, sizeof b, "wave: JellyWave worker FAILED 0x%08x -- builds inline",
                 (u32)rc);
        plog(b);
        return;
    }
    s_jw_worker = true;
}

// Rebuild call, worker mode: copy the finished build into the stage from index
// n (4, just past the gradient) and return the new vertex count -- or, when no
// build is ready yet, leave the stage as it is and return the current count.
static int jw_worker_publish(int n)
{
    if (s_jw_job != JW_JOB_DONE) {
        s_jw_late++;
        return (int)s_jw_verts;
    }
    __sync_synchronize();                      // see the worker's writes
    const int m = s_jw_back_n > n ? s_jw_back_n : n;
    memcpy(&s_jw_stage[n], &s_jw_back[n], (size_t)(m - n) * sizeof(WaveVert));
    memcpy(s_jw_off, s_jw_back_off, sizeof s_jw_off);
    memcpy(s_jw_cnt, s_jw_back_cnt, sizeof s_jw_cnt);
    s_jw_ex_stage = s_jw_back_ex;
    s_jw_fresh    = true;                      // a new build is in the stage
    s_jw_gen_us   += s_jw_back_us;
    s_jw_repaired += s_jw_back_repaired;
    s_jw_dropped  += s_jw_back_dropped;
    s_jw_rebuilds++;
    __sync_synchronize();
    s_jw_job = JW_JOB_IDLE;
    return m;
}

// Hand the worker the field as it is now.  One build in flight at most.
static void jw_worker_queue(float aspect)
{
    if (s_jw_job != JW_JOB_IDLE) return;
    memcpy(s_jw_job_sy, s_field.sy, sizeof s_jw_job_sy);
    jw_look_now(&s_jw_job_look);               // this frame's look, same snapshot
    s_jw_job_aspect = aspect;
    __sync_synchronize();                      // the snapshot lands before the flag
    s_jw_job = JW_JOB_QUEUED;
}

// --- 59.94 Hz motion ------------------------------------------------------
//
// The geometry is rebuilt every s_jw_rebuild_every calls (2), so on its own it
// moves at 29.97 Hz and each build is shown twice.  Between builds, the RSX now draws the PREVIOUS build blended
// toward the NEWEST by a vertex program (wave_interp_vp_data: POS/COLOR0 from
// one buffer, TEX0/COLOR1 from the other, c[4].x the fraction), so every flip
// moves by the same amount.  The fraction comes from the call count
// (jwl_interp_t), not a clock, so it runs at the rate the console actually
// presents -- 59.94 Hz (60000/1001), not 60 -- with nothing to drift.
//
// Two builds blend only when their signatures match (same strip order and
// counts everywhere, jwl_sig_mix); otherwise that frame simply shows the
// newest build, as before.
//
// GATED, default OFF (jellyfin_jwinterp.txt = 1 to enable): it is the first
// vertex program and the first four-attribute binding this renderer has run,
// and a bad binding wedges the GPU.  A crash marker makes a wedge self-limiting:
// jf_jwinterp_pending.txt is written before the first blended draw of a run
// and removed after JW_INTERP_PROVEN of them; finding it at startup means the
// last run died there, and the gate turns itself off.
#define JWINTERP_FILE     "jellyfin_jwinterp.txt"
#define JWINTERP_PENDING  "jf_jwinterp_pending.txt"
#define JW_INTERP_PROVEN  600                       // ~10 s of blended frames
static bool  s_jw_interp_on  = false;
static bool  s_jw_uploaded   = false;   // a build is in the current buffer
static bool  s_jw_prev_ok    = false;   // ... and the other holds the one before
static u32   s_jw_prev_sig   = 0, s_jw_cur_sig = 0;
static u32   s_jw_glow_off   = 0, s_jw_glow_cnt = 0;
static int   s_jw_k          = 0;       // calls since the current build landed
static u32   s_jw_vo_prev_frame = 0;    // this frame's blend, for wave_draw_front
static bool  s_jw_lerp_frame = false;
static float s_jw_t_frame    = 1.0f;
static u32   s_jw_lerp_n     = 0;       // blended draws, for the log
static int   s_jw_marker     = 0;       // 0 not written, 1 written, 2 proven

static void jw_interp_marker(void)
{
    if (s_jw_marker == 0) {
        FILE *f = fopen(jf_data_path(JWINTERP_PENDING), "w");
        if (f) { fputs("blending\n", f); fclose(f); }
        crash_log("wave: JellyWave 59.94 Hz blend -- first blended draw");
        s_jw_marker = 1;
    } else if (s_jw_marker == 1 && s_jw_lerp_n >= JW_INTERP_PROVEN) {
        remove(jf_data_path(JWINTERP_PENDING));
        plog("wave: JellyWave 59.94 Hz blend proven this run (marker cleared)");
        s_jw_marker = 2;
    }
}

// Bind the blend for the draws that follow: false (nothing changed) when this
// frame should simply draw the newest build.
static bool jw_interp_begin(u32 vo_prev, u32 vo, float t)
{
    if (!s_jw_interp_on || !s_jw_prev_ok || s_jw_prev_sig != s_jw_cur_sig) return false;
    if (!(t < 1.0f)) return false;
    jw_interp_marker();
    rsxVertexProgram *ivp = (rsxVertexProgram *)wave_interp_vp_data;
    void *ucode; u32 usize;
    rsxVertexProgramGetUCode(ivp, &ucode, &usize);
    rsxLoadVertexProgram(context, ivp, ucode);
    rsxSetVertexAttribOutputMask(context, ivp->output_mask);
    const f32 c4[4] = { t, t, t, t };
    rsxLoadVertexProgramParameterBlock(context, 4, 1, c4);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
        vo_prev, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
        vo_prev + 16, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
        vo, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR1, 0,
        vo + 16, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    rsxInvalidateVertexCache(context);
    s_jw_lerp_n++;
    return true;
}

// Undo it: the two extra arrays off (TEX0 exactly as wave_draw() has always
// left it; COLOR1 the same way), the passthrough program back, and POS /
// COLOR0 on the newest build -- the state everything after expects.
static void jw_interp_end(u32 vo)
{
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
        0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR1, 0,
        0, 0, 0, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    rsxVertexProgram *vpo = (rsxVertexProgram *)wave_vp_data;
    void *ucode; u32 usize;
    rsxVertexProgramGetUCode(vpo, &ucode, &usize);
    rsxLoadVertexProgram(context, vpo, ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
        vo, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
        vo + 16, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    rsxInvalidateVertexCache(context);
}

// The look pass's share of a jw_look: the drifting key (on its own clock --
// seconds, wrapped at the two periods' product so the float never loses
// precision), the sky for the haze, one pixel in clip y for the fringe.
static void jw_look_extras(jw_look *k)
{
    static float t = 0.0f;
    static u64   last = 0;
    const u64 now = timing_get_us();
    if (last) {
        float dt = (float)(now - last) * 1.0e-6f;
        if (dt > 0.5f) dt = 0.5f;
        t += dt;
        if (t > JW_DRIFT_T_YAW * JW_DRIFT_T_PIT) t -= JW_DRIFT_T_YAW * JW_DRIFT_T_PIT;
    }
    last = now;
    k->key     = s_jw_look_on ? jw_key_drift(t) : jw_key_dir();
    k->bg      = s_bg;
    k->px_y    = display_height ? 2.0f / (float)display_height : 2.0f / 1080.0f;
    k->look_on = s_jw_look_on ? 1 : 0;
}

// --- speed ----------------------------------------------------------------
//
// A JellyWave-only multiplier on wf_step's dt, as a percentage.  The band
// moves about +/-320 px against the legacy ribbons' +/-30, so the same drift
// rate reads as far more agitated.  WAVE_FIELD_DT stays as calibrated for the
// legacy modes.  A smaller dt also injects less noise (wave_kernel.h scales
// its perturbation by sqrt(h)) and shrinks the step between rebuilds, which
// is what reads as choppiness.
//
// Tuned on a TV, so it is a file, not a constant: jellyfin_jwspeed.txt.
#define JWSPEED_FILE  "jellyfin_jwspeed.txt"
#define JW_SPEED_DEF  14   // as tuned on the TV (was 20)
static float s_jw_speed = JW_SPEED_DEF / 100.0f;

static int jwspeed_setting(void) {
    FILE *f = fopen(jf_data_path(JWSPEED_FILE), "r");
    if (!f) return JW_SPEED_DEF;
    int v = JW_SPEED_DEF;
    if (fscanf(f, "%d", &v) != 1) v = JW_SPEED_DEF;
    fclose(f);
    if (v < 1)   v = 1;          // 0 would freeze the wave entirely
    if (v > 400) v = 400;
    return v;
}

// --- rebuild cadence ------------------------------------------------------
//
// Rebuild the geometry every Nth call and let the RSX re-draw the buffer it
// already has in between (and the 59.94 Hz blend interpolate across it).
//
// Safe with the double buffer: the flip happens only on a rebuild, so the
// buffer being written was last read N frames ago, and a reuse frame writes
// nothing and needs no `sync` barrier.  Only acceptable because the wave is
// slow -- a higher N needs a lower JW_SPEED.  A theme change takes up to N
// calls to reach the gradient, which shares the buffer.
//
// The build runs on the worker, so N no longer buys render-thread time; 2
// (30 Hz sampling) is chosen for smoothness.
#define JWREBUILD_FILE  "jellyfin_jwrebuild.txt"
#define JW_REBUILD_DEF  2
static int s_jw_rebuild_every = JW_REBUILD_DEF;
static int s_jw_rebuild_phase = 0;

static int jwrebuild_setting(void) {
    FILE *f = fopen(jf_data_path(JWREBUILD_FILE), "r");
    if (!f) return JW_REBUILD_DEF;
    int v = JW_REBUILD_DEF;
    if (fscanf(f, "%d", &v) != 1) v = JW_REBUILD_DEF;
    fclose(f);
    if (v < 1) v = 1;            // 1 = rebuild every call, the old behaviour
    if (v > 8) v = 8;            // beyond this the motion visibly steps
    return v;
}

// How many times wave_draw_front() lays the near body over the music cover:
// 1 = as translucent as everywhere else, 2 the default, 3 nearly opaque.  Read once at startup.
#define JWFRONT_FILE  "jellyfin_jwfront.txt"
#define JW_FRONT_DEF  2
static int s_jw_front_body = JW_FRONT_DEF;

static int jwfront_setting(void) {
    FILE *f = fopen(jf_data_path(JWFRONT_FILE), "r");
    if (!f) return JW_FRONT_DEF;
    int v = JW_FRONT_DEF;
    if (fscanf(f, "%d", &v) != 1) v = JW_FRONT_DEF;
    fclose(f);
    if (v < 1) v = 1;
    if (v > 3) v = 3;
    return v;
}

// The background dither gate, separate from the wave's so the two can be
// bisected independently over FTP.  Default ON: it sets one register, turned
// off again a few draws later, so its failure mode is a slightly different
// gradient, not a wedged GPU.  Write 0 to the file to disable it.
#define BGDITHER_FILE "jellyfin_bgdither.txt"
static int s_bg_dither = 1;

static int bgdither_setting(void) {
    FILE *f = fopen(jf_data_path(BGDITHER_FILE), "r");
    if (!f) return 1;                       // absent = on
    int v = 1;
    if (fscanf(f, "%d", &v) != 1) v = 1;
    fclose(f);
    return v ? 1 : 0;
}

// The wave's gate: one file holding a mode, so any mode is one character
// away over FTP with no reflash.
//
//   0           immediate mode, baked opaque colours
//   absent      3 (JellyWave) since 2026-09-27: it was opt-in, so a fresh
//               install showed the old flat bands
//   1           vertex arrays, baked opaque colours
//   2           vertex arrays, GPU alpha blending
//   3           JellyWave: the translucent-gel design, lofted in 3-D on the
//               PPU and projected through wave_cam.h's fixed camera.  Same
//               WaveVert, bindings, barrier and teardown as 1-2, plus an
//               additive blend for the rim pass.
//
// Both shaders are passthrough, so per-vertex alpha reaches the blender;
// modes 0-1 bake the blend into opaque colours instead.
#define GPUWAVE_FILE "jellyfin_gpuwave.txt"
static int gpuwave_mode(void) {
    FILE *f = fopen(jf_data_path(GPUWAVE_FILE), "r");
    if (!f) return 3;
    int v = 3;
    if (fscanf(f, "%d", &v) != 1) v = 3;
    fclose(f);
    return (v >= 1 && v <= 3) ? v : 0;
}

// Crest height (screen-space y) of ribbon li at horizontal position fx.
//
// The clamp stays even though the field cannot overshoot the way a sine could:
// WAVE_BASEY[2] is 0.91 and a full-amplitude excursion is under 24 px, so this
// never fires at any resolution the client runs at -- but W or H arriving as
// something absurd is a different failure, and a crest off the bottom of the
// screen is a worse one.
static inline float wave_crest(int li, float fx, float W, float H) {
    float wy = H * WAVE_BASEY[li] + wave_field_px(li, fx, W);
    if (wy < 0.0f) wy = 0.0f;
    if (wy > H)    wy = H;
    return wy;
}

// Background colour at (column ci, screen y): the gradient with ribbons
// [0..upto) already composited in, matching tools/ui_preview/preview.c's
// cumulative per-pixel blend.  crest[j][ci] is ribbon j's crest at column ci.
static void wave_bg(int upto, int ci, float u, float y, float H,
                    const float crest[3][WAVE_MAX_COLS], u8 *r, u8 *g, u8 *b) {
    grad_sample(u, y, H, r, g, b);
    for (int j = 0; j < upto; j++) {
        float cj = crest[j][ci];
        if (y < cj || cj >= H) continue;
        u8 aj = (u8)(WAVE_ALPHA[j] * (1.0f - (y - cj) / (H - cj)));
        u8 jr = (WAVE_COLOR[j] >> 16) & 0xFF;
        u8 jg = (WAVE_COLOR[j] >>  8) & 0xFF;
        u8 jb =  WAVE_COLOR[j]        & 0xFF;
        *r = over8(jr, *r, aj);
        *g = over8(jg, *g, aj);
        *b = over8(jb, *b, aj);
    }
}

void wave_reset(void) {
    // Re-seeding rather than zeroing: a chain zeroed in place is a FLAT LINE
    // that takes about t=20 to climb back to amplitude, so the old behaviour
    // (phases back to 0, shape unchanged) has no equivalent here.  wf_init
    // runs the warm-up, so this returns a field that is already moving.
    wf_init(&s_field, 0);
}

void wave_init(void) {
    // FIRST, before any of the early returns below.  ui_cpu_bg() and mode 0
    // both leave this function early, and an unseeded field reads flat -- so
    // seeding it further down would give the emulator and the immediate-mode
    // path three motionless straight ribbons while the vertex-array path
    // animated normally.  It costs one warm-up, once, at startup.
    wf_init(&s_field, 0);

    // Also before the early returns: the gradient quad is drawn on the
    // immediate-mode path too, so reading this only after the mode-0 return
    // would leave the dither off in exactly the configuration the client
    // ships with the gate file absent.
    s_bg_dither = bgdither_setting();
    month_bg_load();
    wave_bg_refresh();

    rsxFragmentProgram *fpo = (rsxFragmentProgram*)wave_fp_data;
    void *fp_ucode; u32 fp_size;
    rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fp_size);
    s_wave_fp_buf = (u32*)rsxMemalign(256, fp_size);
    memcpy(s_wave_fp_buf, fp_ucode, fp_size);
    rsxAddressToOffset(s_wave_fp_buf, &s_wave_fp_offset);

    if (ui_cpu_bg()) return;          // emulator draws the background on the CPU
    const int mode = gpuwave_mode();
    if (mode == 0) {
        plog("wave: vertex arrays disabled (jellyfin_gpuwave.txt = 0) -- immediate mode");
        crash_log("wave: OFF (immediate mode)");
        return;
    }
    s_wave_blend = (mode == 2 || mode == 3);
    s_wave_jelly = (mode == 3);
    // Either both buffers allocate or the feature stays off; a half-allocated
    // pair would give one framebuffer a working path and the other a null one.
    for (int i = 0; i < 2; i++) {
        s_wave_vbuf[i] = (WaveVert*)rsxMemalign(128, WAVE_MAX_VERTS * sizeof(WaveVert));
        if (!s_wave_vbuf[i]) {
            plog("wave: vertex buffer alloc FAILED -- staying on immediate mode");
            return;
        }
        rsxAddressToOffset(s_wave_vbuf[i], &s_wave_vbuf_off[i]);
    }
    s_wave_varray = true;
    {
        char msg[144];
        const char *what = s_wave_jelly ? "JellyWave 3D"
                         : s_wave_blend ? "GPU blending" : "baked opaque";
        int verts = s_wave_jelly ? (int)JW_TOTAL_VERTS
                  : s_wave_blend ? (4 + 3 * WAVE_MAX_COLS * 2)
                                 : (int)WAVE_LEGACY_VERTS;
        snprintf(msg, sizeof(msg), "wave: vertex arrays ON, mode %d (%s), %d verts",
                 mode, what, verts);
        plog(msg);
    }
    if (s_wave_jelly) {
        char msg[192];
        int  sp = jwspeed_setting();
        s_jw_speed         = (float)sp / 100.0f;
        s_jw_rebuild_every = jwrebuild_setting();
        s_jw_front_body    = jwfront_setting();
        s_jw_rebuild_phase = 0;
        {
            // The look pass: absent / anything but 0 = on (CPU colour and
            // geometry only, on the existing bindings).
            FILE *f = fopen(jf_data_path("jellyfin_jwlook.txt"), "r");
            int v = 1;
            if (f) { if (fscanf(f, "%d", &v) != 1) v = 1; fclose(f); }
            s_jw_look_on = v != 0;
        }
        {
            // The 59.94 Hz blend: only with the gate = 1, and never after a
            // run that died mid-blend (the marker survived it).
            FILE *f = fopen(jf_data_path(JWINTERP_FILE), "r");
            int v = 0;
            if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); }
            s_jw_interp_on = v == 1;
            FILE *m = fopen(jf_data_path(JWINTERP_PENDING), "r");
            if (m) {
                fclose(m);
                remove(jf_data_path(JWINTERP_PENDING));
                if (s_jw_interp_on) {
                    FILE *g = fopen(jf_data_path(JWINTERP_FILE), "w");
                    if (g) { fputs("0\n", g); fclose(g); }
                    crash_log("wave: 59.94 Hz blend DISABLED -- the last run died mid-blend");
                }
                s_jw_interp_on = false;
            }
            crash_log(s_jw_interp_on ? "wave: JellyWave 59.94 Hz blend ON"
                                     : "wave: JellyWave 59.94 Hz blend off");
        }
        s_jw_have_geom     = 0;
        jw_worker_start();
        motes_init();
        snprintf(msg, sizeof(msg),
                 "wave: JellyWave %d stations x %d section, %d verts/layer, "
                 "%d layers, %d draws",
                 JW_STATIONS, JW_SECTION, JW_BODY_VERTS + JW_RIM_VERTS,
                 JW_LAYERS, 1 + JW_LAYERS * 2);
        plog(msg);
        snprintf(msg, sizeof(msg),
                 "wave: JellyWave speed=%d%% rebuild=every %d call%s front=%dx "
                 "(%s / %s / %s)",
                 sp, s_jw_rebuild_every, s_jw_rebuild_every == 1 ? "" : "s",
                 s_jw_front_body, JWSPEED_FILE, JWREBUILD_FILE, JWFRONT_FILE);
        plog(msg);
    }
    // Synchronous breadcrumb, and the ONLY one that survives: wave_init() runs
    // inside ui_init(), which main.cpp calls BEFORE plog_load_setting(), so
    // the plog line above is discarded on a cold boot.  ui_card_gpu_init() and
    // ui_text_gpu_init() were both moved out of ui_init() for exactly this;
    // until wave_init() follows them, crash_log is the instrument.
    crash_log(s_wave_jelly ? "wave: ON (JellyWave 3D)"
            : s_wave_blend ? "wave: ON (vertex arrays + GPU blend)"
                           : "wave: ON (vertex arrays, baked)");
}

// CPU rasterisation of the XMB background — gradient plus the three translucent
// ribbons — straight into the current framebuffer.  Mirrors the GPU wave_draw()
// math (grad_sample / wave_crest / over8) and tools/ui_preview/preview.c's
// cumulative per-pixel blend, including the animated phase.  This path is
// per-pixel; the GPU path is the grid approximation described at the top of
// the file, so the two agree at the grid nodes and closely in between.  Used
// only when ui_cpu_bg() is true (emulator).
static void wave_draw_cpu(void) {
    u32 *fb = color_buffer[curr_fb];
    if (!fb) return;
    const int W = (int)display_width;
    const int H = (int)display_height;

    wave_bg_refresh();

    // Four-corner gradient with an ordered dither, per pixel (the reference
    // dithers by one output code; without it every 8-bit step is a straight
    // band across the screen).  For a fixed row the field is affine in u, so
    // two edge samples and a running sum give three adds a pixel, plus one
    // table lookup and compare per channel for the dither.
    for (int y = 0; y < H; y++) {
        const float v = (H > 1) ? ((float)y / (float)(H - 1)) : 0.0f;
        float lr, lg, lb, rr, rg, rb, dr, dg, db;
        u32 *row = fb + (u32)y * W;

        bg_sample_f(&s_bg, 0.0f, v, &lr, &lg, &lb);
        bg_sample_f(&s_bg, 1.0f, v, &rr, &rg, &rb);
        dr = (W > 1) ? (rr - lr) / (float)(W - 1) : 0.0f;
        dg = (W > 1) ? (rg - lg) / (float)(W - 1) : 0.0f;
        db = (W > 1) ? (rb - lb) / (float)(W - 1) : 0.0f;

        for (int x = 0; x < W; x++) {
            row[x] = ((u32)bg_dither_channel(lr, x, y) << 16)
                   | ((u32)bg_dither_channel(lg, x, y) <<  8)
                   |  (u32)bg_dither_channel(lb, x, y);
            lr += dr; lg += dg; lb += db;
        }
    }

    // Advance the animation exactly like the GPU path.
    {
        float ts, pert, drv;
        wave_audio_frame(&ts, &pert, &drv);
        wave_audio_look(s_amp, &s_lum);
        wave_audio_lum3(s_lum3);
        wave_audio_shape(&s_thick, &s_acc);
        wave_audio_deform(&s_def);
        wf_step(&s_field, WAVE_FIELD_DT * ts, pert, drv);
    }

    // Ribbons back-to-front, each composited over whatever is already in the
    // framebuffer (gradient + earlier ribbons) — same cumulative blend as
    // wave_bg().  Alpha fades linearly from the crest (WAVE_ALPHA) to 0 at the
    // screen bottom; step it per row to avoid a per-pixel divide.
    for (int li = 0; li < 3; li++) {
        u32 cr = (WAVE_COLOR[li] >> 16) & 0xFF;
        u32 cg = (WAVE_COLOR[li] >>  8) & 0xFF;
        u32 cb =  WAVE_COLOR[li]        & 0xFF;
        for (int x = 0; x < W; x++) {
            float cy = wave_crest(li, (float)x, (float)W, (float)H);
            int y0 = (int)cy;
            if (y0 < 0)  y0 = 0;
            if (y0 >= H) continue;
            // Fractional coverage of the crest pixel — without it the curve's
            // top edge lands on whole pixels and staircases; scaling the first
            // pixel's alpha by how much of it the ribbon actually covers
            // feathers the edge to match the crest's true sub-pixel position.
            float topcov = 1.0f - (cy - (float)y0);
            if (topcov < 0.0f) topcov = 0.0f;
            if (topcov > 1.0f) topcov = 1.0f;
            float denom = (float)(H - y0);
            if (denom < 1.0f) continue;
            float a_f  = (float)WAVE_ALPHA[li];
            float step = -(float)WAVE_ALPHA[li] / denom;
            u32  *col  = fb + (u32)y0 * W + x;
            for (int y = y0; y < H; y++, a_f += step, col += W) {
                u32 a = (u32)(y == y0 ? a_f * topcov : a_f);
                if (!a) continue;
                u32 bgp = *col;
                u32 ia  = 255 - a;
                u32 nr = (a * cr + ia * ((bgp >> 16) & 0xFF)) / 255;
                u32 ng = (a * cg + ia * ((bgp >>  8) & 0xFF)) / 255;
                u32 nb = (a * cb + ia * ( bgp        & 0xFF)) / 255;
                *col = (nr << 16) | (ng << 8) | nb;
            }
        }
    }
}

// One ribbon grid node: its NDC position and its pre-composited opaque colour.
// `fy` is the fraction of the way from this column's crest down to the screen
// bottom; `alpha` is the ribbon's opacity at that height.
//
// Both submission paths go through this, so the immediate-mode fallback and
// the vertex-array path cannot drift into drawing different pictures — which
// matters, because the only way to tell them apart is to look at a TV.
static inline void wave_node(int li, int ci, float fy, u8 alpha,
                             float W, float H,
                             const float *colx,
                             const float crest[3][WAVE_MAX_COLS],
                             float *out_x, float *out_y,
                             u8 *out_r, u8 *out_g, u8 *out_b) {
    const u8 cr = (WAVE_COLOR[li] >> 16) & 0xFF;
    const u8 cg = (WAVE_COLOR[li] >>  8) & 0xFF;
    const u8 cb =  WAVE_COLOR[li]        & 0xFF;
    float cy = crest[li][ci];
    float y  = cy + (H - cy) * fy;
    u8 br_, bg_, bb_;
    // The column's horizontal position, which the background now depends on:
    // colx[] is in pixels and the sampler wants [0,1].  W is never zero here --
    // wave_draw() returns early on a degenerate display size before building
    // colx at all.
    wave_bg(li, ci, colx[ci] / W, y, H, crest, &br_, &bg_, &bb_);
    *out_x = (2.0f * colx[ci] / W) - 1.0f;
    *out_y = 1.0f - (2.0f * y / H);
    *out_r = over8(cr, br_, alpha);
    *out_g = over8(cg, bg_, alpha);
    *out_b = over8(cb, bb_, alpha);
}

// Push one immediate-mode vertex (colour latched first, position last — the
// position write commits the vertex, matching the HUD dim quad ordering).
static inline void wave_vtx(float x, float y, u8 r, u8 g, u8 b) {
    const u8    col[4] = { r, g, b, 255 };
    const float pos[4] = { x, y, 0.0f, 1.0f };
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    pos);
}

void wave_draw(void) {
    s_jw_drawn = false;
    if (ui_cpu_bg()) { wave_draw_cpu(); return; }
    if (!s_wave_fp_buf) return;
    const bool jellywave = s_wave_jelly && !strobe_test_disable_jellywave();

    rsxVertexProgram  *vpo = (rsxVertexProgram*)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) wave_fp_data;

    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    // Off for the gradient in every mode; mode 2 turns it back on for the
    // ribbons once the gradient has landed.
    rsxSetBlendEnable(context, GCM_FALSE);

    // The RSX's own dither unit (the CPU path does the same in software).  The
    // gradient is the one large, smooth, low-contrast thing drawn, so the only
    // thing that bands.  It is global RSX state, so it is turned off again
    // before this function returns; text and card blits must not inherit it.
    if (s_bg_dither) rsxSetDitherEnable(context, GCM_TRUE);

    float W = (float)display_width;
    float H = (float)display_height;

    // The background's four corners, read fresh from the theme every frame.
    wave_bg_refresh();

    // One solver step per wave_draw() call (a screen that draws the
    // background twice steps twice).  perturb / drive / dt_scale are stage B's
    // live values mapped by wave_render_map.h; with no music they are the idle
    // set (WM_IDLE_PERTURB, WRM_DRIVE_IDLE, dt_scale 1.0).
    //
    // wave_audio_frame() is time-based, so a second call in the same frame is
    // a no-op returning the same numbers.  s_jw_speed applies in mode 3 only.
    //
    // The solver steps on EVERY call even when the geometry is rebuilt less
    // often: only the sampling of the curve drops to the rebuild rate, and
    // bigger steps would change the physics.
    {
        float ts, pert, drv;
        wave_audio_frame(&ts, &pert, &drv);
        wave_audio_look(s_amp, &s_lum);
        wave_audio_lum3(s_lum3);
        wave_audio_shape(&s_thick, &s_acc);
        wave_audio_deform(&s_def);
        wave_nav_frame();
        if (jellywave) jd_frame(W / H);          // JellyDrop's motion and morph
        // JellyWave also takes less of the broadband perturbation (0.55x):
        // the fine ripple is what makes a slow wave look agitated rather
        // than floating.  Legacy modes are untouched.  A racing scroll
        // raises it again (wave_nav.h's shake, 1.0 at rest).
        wf_step(&s_field,
                WAVE_FIELD_DT * ts * (jellywave ? s_jw_speed : 1.0f),
                jellywave ? pert * 0.55f * s_nav_fx.perturb : pert, drv);
    }

    // Column positions across the screen (x in px, clamped to WAVE_MAX_COLS).
    float colx[WAVE_MAX_COLS];
    int ncols = 0;
    for (int px = 0; px <= (int)W && ncols < WAVE_MAX_COLS; px += WAVE_STEP_PX)
        colx[ncols++] = (float)px;
    if (ncols >= 2 && colx[ncols - 1] < W) {       // ensure the right edge is covered
        if (ncols < WAVE_MAX_COLS) colx[ncols++] = W; else colx[ncols - 1] = W;
    }

    // Precompute every ribbon's crest per column up front; wave_bg needs the
    // earlier ribbons' crests to composite them under the current one.
    //
    // JellyWave does not use crests at all -- it lofts a 3-D section along the
    // spine instead of stroking a 2-D curve -- so this is skipped there.  It
    // is 294 wave_field_px() calls a frame at 1080p, each carrying a divide,
    // and computing them for a path that will not read them is the kind of
    // cost that is invisible in a profile because it is spread over three
    // loops.
    static float crest[3][WAVE_MAX_COLS];
    if (!jellywave) {
        for (int li = 0; li < 3; li++)
            for (int ci = 0; ci < ncols; ci++)
                crest[li][ci] = wave_crest(li, colx[ci], W, H);
    }

    // The background quad's four corners.
    //
    // This used to be a top colour and a bottom colour, each written to two
    // vertices.  It is four independent corners now, which costs the RSX
    // nothing whatsoever -- the quad already emitted four vertices and the
    // gouraud interpolator already had to interpolate between them; it was
    // simply being handed the same colour twice.  With a two-stop theme
    // bg_from_two() puts the old values back in that arrangement and the
    // output is unchanged.
    // wave_set_bg_dim(): the music screen's focus mode takes the gradient
    // down to near black so only the wave / JellyDrop / particles light the
    // screen.  Only the quad: s_bg itself (which the ribbons' haze samples)
    // is left alone, so the ribbons keep their own brightness.
    const u32 bgk = (u32)((1.0f - s_bg_dim) * 256.0f + 0.5f);
    #define BGD(v) (u8)((((u32)(v) & 0xFF) * bgk) >> 8)
    const u8 gtlr=BGD(s_bg.c[BG_TL]>>16), gtlg=BGD(s_bg.c[BG_TL]>>8), gtlb=BGD(s_bg.c[BG_TL]);
    const u8 gtrr=BGD(s_bg.c[BG_TR]>>16), gtrg=BGD(s_bg.c[BG_TR]>>8), gtrb=BGD(s_bg.c[BG_TR]);
    const u8 gblr=BGD(s_bg.c[BG_BL]>>16), gblg=BGD(s_bg.c[BG_BL]>>8), gblb=BGD(s_bg.c[BG_BL]);
    const u8 gbrr=BGD(s_bg.c[BG_BR]>>16), gbrg=BGD(s_bg.c[BG_BR]>>8), gbrb=BGD(s_bg.c[BG_BR]);
    #undef BGD

    // Slice k spans the fraction [k/NS, (k+1)/NS] of the distance from this
    // column's crest down to the screen bottom.  One triangle strip per
    // (ribbon, slice), back-to-front, fully opaque.
    if (s_wave_varray) {
        // Rebuild, or re-draw what is already in the buffer?  Only JellyWave
        // has a cadence; every other mode rebuilds on every call exactly as it
        // always did.  The first call after init must build whatever the
        // cadence says, or the first frames would draw an empty buffer.
        bool jw_rebuild = true;
        if (jellywave && s_jw_rebuild_every > 1 && s_jw_have_geom) {
            jw_rebuild = (s_jw_rebuild_phase == 0);
            if (++s_jw_rebuild_phase >= s_jw_rebuild_every)
                s_jw_rebuild_phase = 0;
        }

        // The RSX consumes vertex arrays asynchronously.  The two-buffer
        // rotation is normally enough, but JellyWave can leave a buffer queued
        // for several frames while the PPU rebuilds the other one.  If the RSX
        // falls more than one buffer behind, rotating back after only three
        // wave_draw calls can overwrite a buffer that is still being fetched.
        //
        // DIAGNOSTIC SAFETY FENCE: before reusing a JellyWave buffer, wait until
        // all previously queued RSX work has consumed the old buffer.  This is
        // intentionally conservative.  If this removes the real-PS3 strobe,
        // the next optimization is a per-buffer fence rather than removing the
        // safety entirely.
        if (jw_rebuild) {
            if (jellywave && s_jw_have_geom)
                rsxSync();
        }

        WaveVert *v  = jellywave
                     ? s_jw_stage
                     : s_wave_vbuf[s_wave_vbuf_turn];
        u32       vo = s_wave_vbuf_off[s_wave_vbuf_turn];
        int       n  = 0;

        // Gradient quad first, so it occupies vertices [0,4).
        #define WV_PUT(px, py, pr, pg, pb, pa) do {             \
            v[n].x = (px); v[n].y = (py); v[n].z = 0.0f;        \
            v[n].w = 1.0f;                                      \
            v[n].rgba = WAVE_RGBA((pr), (pg), (pb), (pa));      \
            n++;                                                \
        } while (0)

        // The gradient shares the buffer with the ribbons, so on a reuse frame
        // it is held along with them.  That is what costs a theme change up to
        // one rebuild interval to appear -- 50 ms at the default cadence.
        if (jw_rebuild) {
            WV_PUT(-1.0f,  1.0f, gtlr, gtlg, gtlb, 255);   // top-left
            WV_PUT(-1.0f, -1.0f, gblr, gblg, gblb, 255);   // bottom-left
            WV_PUT( 1.0f,  1.0f, gtrr, gtrg, gtrb, 255);   // top-right
            WV_PUT( 1.0f, -1.0f, gbrr, gbrg, gbrb, 255);   // bottom-right
        }

        if (jellywave && jw_rebuild) {
            /*
             * Build the COMPLETE JellyWave stream in main memory (the stage).
             * Nothing below reads from the RSX-local destination buffer.
             *
             * After the first build the generation worker does the work (see
             * jw_worker_fn): this call only copies its finished build into the
             * stage and hands it the field for the next one.  The very first
             * build is made here, so the first frame is never empty.
             */
            if (s_jw_worker && s_jw_have_geom) {
                n = jw_worker_publish(n);
                jw_worker_queue(W / H);
            } else {
                u64 jw_t0 = timing_get_us();
                jw_look look;
                jw_look_now(&look);
                n = jw_generate(s_jw_stage, n, s_field.sy, &look, s_jw_disp,
                                W / H, s_jw,
                                s_jw_off, s_jw_cnt, &s_jw_ex_stage,
                                &s_jw_repaired, &s_jw_dropped);
                s_jw_fresh = true;
                s_jw_gen_us += timing_get_us() - jw_t0;
                s_jw_rebuilds++;
            }
            s_jw_verts = (u32)n;
            s_jw_have_geom = 1;
        } else if (jellywave) {
            // Reuse call: s_jw_stage still holds the last build and is
            // uploaded as-is below.  Nothing may be written into it here.
            // Falling through to the legacy branches (as this once did)
            // wrote 6*ncols legacy crest->bottom vertices over the start of
            // the JellyWave stream on 2 of every 3 frames -- the far body,
            // at 1080p indices 0..581 -- which the RSX then drew as huge
            // full-height triangles: the "JellyWave strobe" (~13 ms of fill,
            // 30 Hz frames).  Found by diffing the one clean hardware build
            // (whose reuse writes landed past the drawn range) against the
            // strobing ones.
        } else if (s_wave_blend) {
            // One quad per ribbon: constant tint, alpha ramping from the
            // crest opacity down to zero at the screen bottom.  The GPU
            // interpolates that ramp, which is why the WAVE_NS slicing is not
            // needed here -- the true fade is linear in y for a given column,
            // so a single quad is EXACT along every column edge and
            // approximates between columns exactly as the sliced version did.
            //
            // Per vertex this costs one NDC conversion and nothing else: no
            // grad_sample, no wave_bg, no over8. That arithmetic -- not the
            // FIFO -- was the 1,660 us the vertex-array change left behind.
            for (int li = 0; li < 3; li++) {
                const u8 cr = (WAVE_COLOR[li] >> 16) & 0xFF;
                const u8 cg = (WAVE_COLOR[li] >>  8) & 0xFF;
                const u8 cb =  WAVE_COLOR[li]        & 0xFF;
                for (int ci = 0; ci < ncols; ci++) {
                    float cx = (2.0f * colx[ci] / W) - 1.0f;
                    float cy = crest[li][ci];
                    WV_PUT(cx, 1.0f - (2.0f * cy / H), cr, cg, cb, WAVE_ALPHA[li]);
                    WV_PUT(cx, -1.0f,                  cr, cg, cb, 0);
                }
            }
        } else {
            for (int li = 0; li < 3; li++) {
                for (int k = 0; k < WAVE_NS; k++) {
                    float ft = (float)k       / WAVE_NS;
                    float fb = (float)(k + 1) / WAVE_NS;
                    u8 at = (u8)(WAVE_ALPHA[li] * (1.0f - ft));
                    u8 ab = (u8)(WAVE_ALPHA[li] * (1.0f - fb));
                    for (int ci = 0; ci < ncols; ci++) {
                        float xt, yt, xb, yb; u8 tr_,tg_,tb_, br_,bg_,bb_;
                        wave_node(li, ci, ft, at, W, H, colx, crest, &xt, &yt, &tr_,&tg_,&tb_);
                        wave_node(li, ci, fb, ab, W, H, colx, crest, &xb, &yb, &br_,&bg_,&bb_);
                        WV_PUT(xt, yt, tr_, tg_, tb_, 255);
                        WV_PUT(xb, yb, br_, bg_, bb_, 255);
                    }
                }
            }
        }
        #undef WV_PUT

        /*
         * JellyWave is always uploaded from main-memory staging into the
         * alternate RSX buffer.  The full RSX fence happens BEFORE reusing
         * the destination buffer; the upload itself is write-only.
         *
         * This deliberately happens on reuse calls too.  With two buffers,
         * the destination is the buffer the RSX used on the previous call,
         * so the fence makes the ownership transition explicit.
         */
        if (jellywave) {
            u64 jw_up0 = timing_get_us();

            /*
             * DIAGNOSTIC: the destination RSX buffer must no longer be
             * in use before the PPU overwrites it.  The sync inside
             * jw_upload() only orders PPU stores; it does NOT wait for
             * the RSX to finish consuming the previous contents.
             */
            rsxSync();

            // 2026-09-26: only a NEW build is uploaded, into the buffer that
            // held the older of the two -- so the other still holds the
            // previous build, which the in-between frames blend from (see
            // "59.94 Hz motion" below).  A reuse call, or a rebuild call whose
            // build was late, uploads nothing and draws what is there.  The
            // rsxSync above is a full fence, so the buffer written is idle
            // whichever one it is.
            if (s_jw_fresh || !s_jw_uploaded) {
                s_wave_vbuf_turn ^= 1;
                jw_upload(s_wave_vbuf[s_wave_vbuf_turn], s_jw_stage, s_jw_verts);
                s_jw_prev_ok  = s_jw_uploaded;
                s_jw_prev_sig = s_jw_cur_sig;
                s_jw_cur_sig  = s_jw_ex_stage.sig;
                s_jw_glow_off = s_jw_ex_stage.glow_off;
                s_jw_glow_cnt = s_jw_ex_stage.glow_cnt;
                s_jw_k        = 0;
                s_jw_uploaded = true;
            } else if (s_jw_k < 64) {
                s_jw_k++;
            }
            s_jw_fresh = false;

            v  = s_wave_vbuf[s_wave_vbuf_turn];
            vo = s_wave_vbuf_off[s_wave_vbuf_turn];

            // The motes ride the same fence: the rsxSync above means nothing
            // queued is still reading either of their buffers.
            s_mote_n = s_mote_ok ? motes_build(W / H) : 0;
            if (s_mote_n)
                jw_upload(s_mote_vbuf[s_wave_vbuf_turn], s_mote_stage, s_mote_n);
            s_jw_upload_us += timing_get_us() - jw_up0;
        }

        // Drain the PPU's write-gather buffer before the RSX reads these
        // vertices.  Same barrier, same reason, as ui_text_gpu.cpp's
        // submit_queue() -- the PPU has just written this buffer and the GPU is
        // about to fetch it, and those writes are still sitting in the gather
        // buffer until something forces them out.
        //
        // Not optional: without it, whether the RSX sees a fully written
        // array depends on how GCC happened to schedule the stores, and a
        // partial array wedges the GPU.  Do not remove it because "it works
        // without it" (docs/wave-renderer-notes.md, "RSX-local store rules").
        //
        // Skipped only on a JellyWave reuse call, which writes nothing; every
        // call that writes a vertex takes it.
        if (jw_rebuild && !jellywave)
            __asm__ __volatile__ ("sync" ::: "memory");

        // The gradient is the first four vertices in the same reusable
        // array as the JellyWave geometry. Keep it on the array-fetch path:
        // the previous frame leaves POS array-bound, so submitting this quad
        // with rsxDrawVertex* would mix immediate vertices with a live POS
        // binding. That can make the first primitive fetch stale data and
        // manifest as a black/colour-strobing frame on real hardware.
        //
        // Drawing the gradient from the same array also makes rebuild and
        // reuse frames identical: on a reuse frame vertices [0,4) are already
        // present in the buffer, so no CPU writes or extra synchronization are
        // needed.
        //
        // Bind POS/COLOR first, then draw the gradient while blending is still
        // disabled. TEX0 is explicitly disabled rather than inherited.
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
            vo, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
            vo + 16, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
            0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

        rsxInvalidateVertexCache(context);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);

        if (s_wave_blend) {
            // src*a + dst*(1-a), ribbons back to front -- algebraically the
            // same cumulative composite wave_bg() does on the CPU, and the
            // same blend the UI uses everywhere else.
            rsxSetBlendFunc(context,
                GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
            rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
            rsxSetBlendEnable(context, GCM_TRUE);
        }

        vis_load();
        if (jellywave && s_vis_off) {
            // Visualiser Off (Select): the gradient above, nothing over it.
            // s_jw_drawn stays false, so wave_draw_front stands down too.
        } else if (jellywave) {
            // Off -> JellyWave: the ribbons are uncovered from the left edge
            // across the screen (the gradient above is already whole).
            bool revealing = false;
            if (s_reveal_t0) {
                float r = (float)(timing_get_us() - s_reveal_t0) / VIS_REVEAL_US;
                if (r >= 1.0f) s_reveal_t0 = 0;
                else {
                    r = 1.0f - (1.0f - r) * (1.0f - r) * (1.0f - r);      // ease out
                    int rw = (int)(r * W);
                    if (rw < 1) rw = 1;
                    rsxSetScissor(context, 0, 0, (u16)rw, (u16)H);
                    revealing = true;
                }
            }
            (void)revealing;
            // Explicitly establish JellyWave's hardware-validated standard
            // alpha blend state. Do not inherit whatever state the previous
            // UI/background operation left behind.
            rsxSetBlendFunc(context,
                GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
            rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
            rsxSetBlendEnable(context, GCM_TRUE);

            // Body then rim, layer by layer, furthest first.  The rim is
            // ADDITIVE, as the design draws it (rimMat, AdditiveBlending):
            // rimColor is light to add and is near-black wherever the rim
            // weight or the key is low, so on the standard blend it paints
            // dark tubes along every rolled edge.  Each rim follows its own
            // body because there is no depth buffer: a nearer layer must be
            // able to cover a further layer's edge.  The standard blend is
            // restored after the loop.

            // Draw the finished JellyWave stream uploaded from CPU staging.
            //
            // IMPORTANT: s_jw_off/s_jw_cnt describe the CPU-built stream;
            // the RSX buffer is write-only from the PPU. Never read it back.
            rsxInvalidateVertexCache(context);
            s_jw_draws = 1;   // gradient plus the body/rim strips sent below

            // The FAR particles first, behind every ribbon (the look pass):
            // the gel passes in front of them.  Their own buffer for this one
            // draw, the wave's straight back after, as the near motes do.
            if (s_mote_n && s_mote_far_n) {
                const u32 mo = s_mote_voff[s_wave_vbuf_turn];
                rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
                    mo, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
                rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
                    mo + 16, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
                rsxInvalidateVertexCache(context);
                rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE, GCM_SRC_ALPHA, GCM_ONE);
                rsxDrawVertexArray(context, GCM_TYPE_TRIANGLES, 0, s_mote_far_n);
                s_jw_draws++;
                rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
                    vo, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
                rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
                    vo + 16, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
                rsxInvalidateVertexCache(context);
            }

            // 59.94 Hz motion: between builds, blend from the previous one.
            const u32 vo_prev = s_wave_vbuf_off[s_wave_vbuf_turn ^ 1];
            const float jt = jwl_interp_t(s_jw_k, s_jw_rebuild_every);
            const bool lerp = jw_interp_begin(vo_prev, vo, jt);

            // The glow: behind every layer, additive (the look pass).
            if (s_jw_glow_cnt) {
                rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE, GCM_SRC_ALPHA, GCM_ONE);
                rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, s_jw_glow_off, s_jw_glow_cnt);
                s_jw_draws++;
            }

            for (int slot = 0; slot < JW_LAYERS; slot++) {
                for (int pass = 0; pass < 2; pass++) {
                    if ((pass == 0 && strobe_test_disable_jellywave_body()) ||
                        (pass == 1 && strobe_test_disable_jellywave_rim()))
                        continue;
                    const u32 count = s_jw_cnt[slot][pass];
                    if (!count)
                        continue;

                    if (pass == 0)
                        rsxSetBlendFunc(context,
                            GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                            GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
                    else
                        rsxSetBlendFunc(context,          // src*a + dst
                            GCM_SRC_ALPHA, GCM_ONE,
                            GCM_SRC_ALPHA, GCM_ONE);

                    rsxDrawVertexArray(context,
                        GCM_TYPE_TRIANGLE_STRIP,
                        s_jw_off[slot][pass],
                        count);
                    s_jw_draws++;
                }
            }

            if (lerp) jw_interp_end(vo);

            // The motes, in front of the gel, additive.  Their own buffer is
            // bound for this one draw and POS goes straight back to the wave's
            // buffer, so the teardown below finds exactly the binding it
            // always has.  Only the NEAR ones now: [s_mote_far_n, s_mote_n).
            if (s_mote_n > s_mote_far_n) {
                const u32 mo = s_mote_voff[s_wave_vbuf_turn];
                rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
                    mo, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
                rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
                    mo + 16, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
                rsxInvalidateVertexCache(context);
                rsxSetBlendFunc(context,
                    GCM_SRC_ALPHA, GCM_ONE,
                    GCM_SRC_ALPHA, GCM_ONE);
                rsxDrawVertexArray(context, GCM_TYPE_TRIANGLES, s_mote_far_n, s_mote_n - s_mote_far_n);
                s_jw_draws++;
                rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
                    vo, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
            }
            s_jw_vo_frame = vo;
            s_jw_vo_prev_frame = vo_prev;
            s_jw_lerp_frame = lerp;
            s_jw_t_frame  = jt;
            s_jw_drawn    = true;
        }

        if (s_reveal_t0) rsxSetScissor(context, 0, 0, (u16)W, (u16)H);   // the reveal's clip off

        // Release the colour array.  Everything the UI draws after the
        // background this frame -- cards, text, chrome, the dim quad --
        // submits inline, and a stale per-vertex COLOR0 array is what the
        // "unreliable fetch" folklore was actually describing.
        //
        // This is hud_dim.cpp's teardown verbatim: COLOR0 back to stride 0,
        // POS left bound.  That exact sequence is proven on this console, and
        // the failure mode for getting it wrong is a wedged GPU and a power
        // cycle, so it is copied rather than improved on.
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
            vo + 16, 0, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    } else {
        // Background-gradient quad (XMB_BG_TOP -> XMB_BG_BOT), streamed inline.
        rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
        wave_vtx(-1.0f,  1.0f, gtlr, gtlg, gtlb);   // top-left
        wave_vtx(-1.0f, -1.0f, gblr, gblg, gblb);   // bottom-left
        wave_vtx( 1.0f,  1.0f, gtrr, gtrg, gtrb);   // top-right
        wave_vtx( 1.0f, -1.0f, gbrr, gbrg, gbrb);   // bottom-right
        rsxDrawVertexEnd(context);

        for (int li = 0; li < 3; li++) {
            for (int k = 0; k < WAVE_NS; k++) {
                float ft = (float)k       / WAVE_NS;
                float fb = (float)(k + 1) / WAVE_NS;
                u8 at = (u8)(WAVE_ALPHA[li] * (1.0f - ft));
                u8 ab = (u8)(WAVE_ALPHA[li] * (1.0f - fb));
                rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
                for (int ci = 0; ci < ncols; ci++) {
                    float xt, yt, xb, yb; u8 tr_,tg_,tb_, br_,bg_,bb_;
                    wave_node(li, ci, ft, at, W, H, colx, crest, &xt, &yt, &tr_,&tg_,&tb_);
                    wave_node(li, ci, fb, ab, W, H, colx, crest, &xb, &yb, &br_,&bg_,&bb_);
                    wave_vtx(xt, yt, tr_, tg_, tb_);
                    wave_vtx(xb, yb, br_, bg_, bb_);
                }
                rsxDrawVertexEnd(context);
            }
        }
    }

    // Restore the UI's standard alpha-blend state for everything drawn after
    // the background this frame (ui_init configures the same).
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    // And the dither off again, or every card blit, glyph and chrome quad
    // drawn after the background inherits it.
    if (s_bg_dither) rsxSetDitherEnable(context, GCM_FALSE);

    // JellyWave's own cost line, once a second.  `gen` is PPU time per build
    // and `verts` / `draws` are exact; the RSX's cost is NOT here (there is no
    // GPU timer) -- read it from `vsync` in the xmb: frame line, which shrinks
    // by whatever the GPU spends.  Counted per call: a screen that draws the
    // background twice pays twice.
    if (jellywave && ++s_jw_frames >= 60) {
        char msg[224];
        u32  nb = s_jw_rebuilds ? s_jw_rebuilds : 1;
        snprintf(msg, sizeof(msg),
                 "jellywave: gen=%lluus/build amort=%lluus/call "
                 "up=%lluus/call verts=%u draws=%u rebuild=1/%d "
                 "speed=%d%% repaired=%u dropped=%u (%d calls, %d builds) "
                 "worker=%d late=%u lerp=%u look=%d drop=%.2f/%d",
                 (unsigned long long)(s_jw_gen_us / nb),
                 (unsigned long long)(s_jw_gen_us / s_jw_frames),
                 (unsigned long long)(s_jw_upload_us / s_jw_frames),
                 s_jw_verts, s_jw_draws, s_jw_rebuild_every,
                 (int)(s_jw_speed * 100.0f + 0.5f),
                 (unsigned)s_jw_repaired, (unsigned)s_jw_dropped,
                 (int)s_jw_frames, (int)s_jw_rebuilds,
                 s_jw_worker ? 1 : 0, (unsigned)s_jw_late,
                 (unsigned)s_jw_lerp_n, s_jw_look_on ? 1 : 0,
                 (double)s_jd_morph, (int)s_jd_toggles);
        plog(msg);
        s_jw_late       = 0;
        s_jw_gen_us     = 0;
        s_jw_upload_us  = 0;
        s_jw_frames     = 0;
        s_jw_rebuilds   = 0;
    }
}

bool wave_gpu_blend_ready(void) { return s_wave_varray && s_wave_blend; }

// The near body over the music cover; see ui_wave.h.  Everything here is
// wave_draw()'s own sequence for ONE layer: its programs, its bindings (the
// buffer it drew from this frame -- the same RSX-local stream, already fenced
// and uploaded, only read again), body on the standard blend then the rim
// additive, and hud_dim's teardown.  Nothing touches the stage, the upload or
// the reuse guard.
void wave_draw_front(int x, int y, int w, int h) {
    if (!s_jw_drawn || !s_wave_fp_buf || ui_cpu_bg()) return;
    // JellyDrop always floats behind the covers and the text: no ribbon over
    // the cover while it is on or morphing.
    if (s_jd_morph > 0.0f || s_jd_want) return;
    const int slot = JW_LAYERS - 1;               // the near ribbon is drawn last
    if (!s_jw_cnt[slot][0]) return;
    const int W = (int)display_width, H = (int)display_height;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;

    rsxVertexProgram  *vpo = (rsxVertexProgram*)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) wave_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);
    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);

    const u32 vo = s_jw_vo_frame;
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
        vo, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
        vo + 16, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
        0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    // The same in-between as wave_draw() drew this frame, or the ribbon over
    // the cover would sit half a step off the one beside it.
    const bool lerp = s_jw_lerp_frame && jw_interp_begin(s_jw_vo_prev_frame, vo, s_jw_t_frame);
    rsxSetScissor(context, (u16)x, (u16)y, (u16)w, (u16)h);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    // The same one-register dither wave_draw() puts over the gradient and the
    // gel (s_bg_dither), for the same reason: a smooth translucent fill over a
    // cover is exactly where 8-bit banding shows.  Off again below.
    if (s_bg_dither) rsxSetDitherEnable(context, GCM_TRUE);
    rsxInvalidateVertexCache(context);
    for (int pass = 0; pass < 2; pass++) {
        const u32 count = s_jw_cnt[slot][pass];
        if (!count) continue;
        if (pass == 0)
            rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                                     GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
        else
            rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE,
                                     GCM_SRC_ALPHA, GCM_ONE);
        // Over the cover the body goes down s_jw_front_body times: the same
        // colour composited again, so its opacity is 1 - (1 - a)^n with no
        // change of hue -- at the near layer's alpha 150 that is 59% for one
        // pass, 83% for two (the default), 93% for three.  "Less transparent
        // in front of the cover", hardware 2026-09-26.
        const int reps = pass == 0 ? s_jw_front_body : 1;
        for (int r = 0; r < reps; r++)
            rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, s_jw_off[slot][pass], count);
    }
    // The ribbon LIGHTS the cover where it passes: its body once more,
    // added at a constant 9% -- a hint, not a glow (the same constant-alpha
    // blend the card opacity uses, proven on this console).
    if (s_jw_cnt[slot][0]) {
        rsxSetBlendColor(context, (u32)46 << 24, 0);      // 18% (v9: 9% went unnoticed)
        rsxSetBlendFunc(context, GCM_CONSTANT_ALPHA, GCM_ONE,
                                 GCM_CONSTANT_ALPHA, GCM_ONE);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, s_jw_off[slot][0], s_jw_cnt[slot][0]);
    }
    if (lerp) jw_interp_end(vo);
    // hud_dim's teardown, as wave_draw() ends: COLOR0 back to stride 0, POS
    // left bound; the full scissor and the UI's standard blend.
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
        vo + 16, 0, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    rsxSetScissor(context, 0, 0, (u16)W, (u16)H);
    rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                             GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    if (s_bg_dither) rsxSetDitherEnable(context, GCM_FALSE);
}

// The faded hairline under the tab bar, as a blended GPU quad.  Its falloff,
// a = peak * min(x, W-x) * 2 / W, is linear from each edge to the centre, so
// three columns of vertices (two quads) reproduce it exactly -- with no VRAM
// reads, which is what made the CPU version cost ~1.35 ms a frame.
void wave_draw_divider_gpu(int y_px, u8 r, u8 g, u8 b, u8 peak_alpha) {
    if (!s_wave_fp_buf) return;

    rsxVertexProgram   *vpo = (rsxVertexProgram*)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) wave_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    const float H  = (float)display_height;
    const float yt = 1.0f - (2.0f * (float)y_px       / H);
    const float yb = 1.0f - (2.0f * (float)(y_px + 1) / H);

    // colour latched first, position last -- the position write commits it.
    #define DV(px, py, pa) do {                                        \
        const u8    c4[4] = { r, g, b, (pa) };                         \
        const float p4[4] = { (px), (py), 0.0f, 1.0f };                \
        rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, c4);       \
        rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    p4);       \
    } while (0)

    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    DV(-1.0f, yt, 0);            DV(-1.0f, yb, 0);
    DV( 0.0f, yt, peak_alpha);   DV( 0.0f, yb, peak_alpha);
    DV( 1.0f, yt, 0);            DV( 1.0f, yb, 0);
    rsxDrawVertexEnd(context);
    #undef DV
}

// A 12-segment radial fan (centre plus a closed 12-point ring): 14 vertices
// total.  The perimeter alpha is zero, so the RSX performs the falloff without
// reading or blending pixels on the PPU.
void wave_draw_glow_gpu(int cx_px, int cy_px, int radius_px,
                        u8 r, u8 g, u8 b, u8 peak_alpha) {
    static const float ring[13][2] = {
        { 1.000000f,  0.000000f }, { 0.866025f,  0.500000f },
        { 0.500000f,  0.866025f }, { 0.000000f,  1.000000f },
        {-0.500000f,  0.866025f }, {-0.866025f,  0.500000f },
        {-1.000000f,  0.000000f }, {-0.866025f, -0.500000f },
        {-0.500000f, -0.866025f }, { 0.000000f, -1.000000f },
        { 0.500000f, -0.866025f }, { 0.866025f, -0.500000f },
        { 1.000000f,  0.000000f },
    };

    if (!s_wave_fp_buf || radius_px <= 0 || !peak_alpha) return;

    rsxVertexProgram   *vpo = (rsxVertexProgram*)wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*)wave_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    const float W = (float)display_width;
    const float H = (float)display_height;
    #define GV(px, py, pa) do {                                        \
        const u8 c4[4] = { r, g, b, (pa) };                            \
        const float p4[4] = { (2.0f * (px) / W) - 1.0f,                \
                               1.0f - (2.0f * (py) / H), 0.0f, 1.0f }; \
        rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, c4);       \
        rsxDrawVertex4f(context, GCM_VERTEX_ATTRIB_POS, p4);           \
    } while (0)

    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_FAN);
    GV((float)cx_px, (float)cy_px, peak_alpha);
    for (int i = 0; i < 13; i++)
        GV((float)cx_px + ring[i][0] * radius_px,
           (float)cy_px + ring[i][1] * radius_px, 0);
    rsxDrawVertexEnd(context);
    #undef GV
}

// Full-screen dim quad — darkens the finished frame under a modal (the
// update popup).  Same inline immediate-mode path as the player's HUD dim
// quad (hud_dim.cpp): a blended black quad streamed straight into the FIFO,
// no vertex-array fetch, reusing the wave's resident passthrough programs.
// Fenced with rsxSync() before returning so the caller's CPU pixel writes
// (panel, text) may follow immediately.
void wave_dim_screen(u8 alpha) {
    if (!s_wave_fp_buf) return;

    rsxVertexProgram   *vpo = (rsxVertexProgram*)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) wave_fp_data;

    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    // wave_vtx() forces opaque vertices, so push these by hand (colour first,
    // position last — the position write commits the vertex).
    const u8    col[4] = { 0, 0, 0, alpha };
    const float tl[4]  = { -1.0f,  1.0f, 0.0f, 1.0f };
    const float tr[4]  = {  1.0f,  1.0f, 0.0f, 1.0f };
    const float bl[4]  = { -1.0f, -1.0f, 0.0f, 1.0f };
    const float br[4]  = {  1.0f, -1.0f, 0.0f, 1.0f };
    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    tl);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    tr);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    bl);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    br);
    rsxDrawVertexEnd(context);

    rsxSync();
}

// Bind the wave's resident passthrough programs and the UI's standard alpha
// blend, for IMMEDIATE-MODE geometry issued by other files -- the spine's
// panels, glows and ramps (ui_wave_panels.cpp).  Exactly the preamble
// wave_draw_divider_gpu() and wave_draw_glow_gpu() above already use.  It
// touches no vertex buffer, no vertex-array binding and no fence: the
// JellyWave upload path and its buffers are not involved.  False until
// wave_init() has put the fragment program in RSX memory.
bool wave_imm_bind(void) {
    if (!s_wave_fp_buf) return false;

    rsxVertexProgram   *vpo = (rsxVertexProgram*)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) wave_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    return true;
}
