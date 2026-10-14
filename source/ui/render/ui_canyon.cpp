// The Canyon music visualizer on the RSX.  See canyon.h for what it is and
// where every number came from, ui_canyon.h for how the music screen uses it.
//
// GPU DISCIPLINE -- copied from wave_draw(), which is proven on hardware:
//   * the passthrough programs from wave_shaders.h (clip-space POS + COLOR0);
//   * cy_vert is wave_draw's WaveVert: 24 bytes, aligned(8), colour one u32.
//     cy_emit() writes it straight into RSX local memory, so the same store
//     rules apply (whole aligned words, 8-aligned 64-bit stores -- see the
//     WaveVert comment in ui_wave.cpp for the console-wedging history);
//   * rsxInvalidateVertexCache() before the draws, COLOR0 re-bound at stride 0
//     and TEX0 left disabled afterwards, full scissor and the UI's standard
//     alpha blend restored -- the state wave_draw() leaves behind.
//
// Two vertex buffers, alternated per frame: the music screen waits for the
// previous flip before drawing, so the buffer being written is never the one
// the RSX is still reading.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "rsxutil.h"
#include "wave_shaders.h"
#include "canyon.h"
#include "sv_spectrum.h"
#include "music_sv.h"
#include "ui_canyon.h"
#include "timing.h"
#include "plog.h"
#include "jf_paths.h"
#include "stb_image.h"           // stbi_zlib_decode_buffer (implementation in thumbnail_cache.cpp)

extern void crash_log(const char *msg);

#define CANYON_QRC  "/dev_flash/vsh/resource/qgl/canyon.qrc"
#define VIZ_FILE    "jellyfin_visualizer.txt"
#define XFADE_S     1.6f         // preset cross-fade
#define STALE_US    350000ULL    // no fresh audio for this long = silence

// ------------------------------------------------------------------ mode

static int  s_mode = VIZ_WAVE;
static bool s_mode_loaded = false;

int viz_mode(void) {
    if (!s_mode_loaded) {
        s_mode_loaded = true;
        FILE *f = fopen(jf_data_path(VIZ_FILE), "r");
        if (f) {
            int v = 0;
            if (fscanf(f, "%d", &v) == 1 && v >= 0 && v < VIZ_COUNT) s_mode = v;
            fclose(f);
        }
    }
    return s_mode;
}

void viz_set_mode(int mode) {
    if (mode < 0 || mode >= VIZ_COUNT) mode = VIZ_WAVE;
    s_mode = mode;
    s_mode_loaded = true;
    FILE *f = fopen(jf_data_path(VIZ_FILE), "w");
    if (f) { fprintf(f, "%d\n", mode); fclose(f); }
    char b[64];
    snprintf(b, sizeof b, "viz: mode %s", viz_mode_name(mode));
    plog(b);
}

const char *viz_mode_name(int mode) {
    return mode == VIZ_CANYON ? "Canyon" : "Wave";
}

// --------------------------------------------------------------- presets

static cy_bank   s_bank;
static bool      s_bank_tried = false;
static int       s_order[CY_MAX_PRESETS];
static int       s_order_pos = 0;
static int       s_preset = -1;          // index into s_bank.p, -1 = CY_DEFAULT
static cy_preset s_from, s_to, s_cur;
static float     s_xfade = 1.0f;
static int       s_track = -1;

// Read + inflate + parse the console's canyon.qrc.  Everything transient is
// freed before returning; only the ~7 KB table is kept.
static void bank_load(void) {
    s_bank_tried = true;
    s_bank.n = 0;
    FILE *f = fopen(CANYON_QRC, "rb");
    if (!f) { plog("canyon: " CANYON_QRC " unavailable -- built-in look"); return; }
    fseek(f, 0, SEEK_END);
    long raw_n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (raw_n < 16 || raw_n > (1 << 20)) { fclose(f); plog("canyon: canyon.qrc size unexpected"); return; }
    uint8_t *raw = (uint8_t *)malloc((size_t)raw_n);
    if (!raw) { fclose(f); plog("canyon: no memory for canyon.qrc"); return; }
    size_t got = fread(raw, 1, (size_t)raw_n, f);
    fclose(f);
    uint32_t out_n = cy__be32(raw + 4);
    if (got != (size_t)raw_n || memcmp(raw, "QRCC", 4) != 0 || out_n < 64 || out_n > (4u << 20)) {
        free(raw);
        plog("canyon: canyon.qrc is not a QRCC archive");
        return;
    }
    uint8_t *img = (uint8_t *)malloc(out_n);
    if (!img) { free(raw); plog("canyon: no memory to inflate canyon.qrc"); return; }
    int dec = stbi_zlib_decode_buffer((char *)img, (int)out_n, (const char *)raw + 8, (int)raw_n - 8);
    free(raw);
    int np = dec > 0 ? cy_bank_load(&s_bank, img, (uint32_t)dec) : -1;
    free(img);
    char b[128];
    snprintf(b, sizeof b, "canyon: %d presets, %d keys from canyon.qrc (inflated %d)",
             np, s_bank.keys, dec);
    plog(b);
    if (np <= 0) s_bank.n = 0;
    // Play order: shuffled once per session.
    for (int i = 0; i < s_bank.n; i++) s_order[i] = i;
    u32 seed = (u32)timing_get_us() | 1u;
    for (int i = s_bank.n - 1; i > 0; i--) {
        seed = seed * 1664525u + 1013904223u;
        int j = (int)((seed >> 8) % (u32)(i + 1));
        int t = s_order[i]; s_order[i] = s_order[j]; s_order[j] = t;
    }
}

static void preset_target(int idx, bool instant) {
    cy_preset p = (idx >= 0 && idx < s_bank.n) ? s_bank.p[idx] : CY_DEFAULT;
    cy_preset_sanitise(&p);
    s_preset = (idx >= 0 && idx < s_bank.n) ? idx : -1;
    if (instant) { s_from = s_to = s_cur = p; s_xfade = 1.0f; return; }
    s_from = s_cur;
    s_to = p;
    s_xfade = 0.0f;
}

static void preset_advance(bool instant) {
    if (s_bank.n <= 0) { preset_target(-1, instant); return; }
    preset_target(s_order[s_order_pos % s_bank.n], instant);
    s_order_pos++;
}

void canyon_track(int track_index) {
    if (track_index == s_track) return;
    const bool first = (s_track < 0);
    s_track = track_index;
    if (!s_bank_tried) bank_load();
    preset_advance(first);
}

void canyon_next_preset(void) {
    if (!s_bank_tried) bank_load();
    preset_advance(false);
    char b[64];
    snprintf(b, sizeof b, "canyon: preset %s", canyon_preset_name());
    plog(b);
}

const char *canyon_preset_name(void) {
    return s_preset >= 0 ? s_bank.name[s_preset] : "Jellyfin";
}

// ------------------------------------------------------------------ draw

static sv_spec_t s_sv;
static cy_state  s_st;
static bool      s_ready = false, s_failed = false;
static cy_vert  *s_vbuf[2] = { NULL, NULL };
static cy_vert  *s_stage = NULL;   // cached main memory: cy_emit writes here
static u32       s_voff[2];
static u32      *s_fp_buf = NULL;
static u32       s_fp_off = 0;
static u32       s_slot = 0;
static u64       s_last_us = 0, s_fresh_us = 0;
static float     s_L[SV_BINS], s_R[SV_BINS];
static bool      s_have_spec = false;
static u64       s_cost_us = 0;
static u32       s_cost_n = 0;
static u64       s_emit_us = 0, s_copy_us = 0;

// Cover tint: target from the music screen, current eased toward it.
static float     s_tint_to[3] = { 0, 0, 0 }, s_tint[3] = { 0, 0, 0 };
static float     s_tint_amt_to = 0.0f, s_tint_amt = 0.0f;

void canyon_set_tint(u32 rgb, float amount) {
    if (!rgb) { s_tint_amt_to = 0.0f; return; }
    s_tint_to[0] = (float)((rgb >> 16) & 0xFF) / 255.0f;
    s_tint_to[1] = (float)((rgb >>  8) & 0xFF) / 255.0f;
    s_tint_to[2] = (float)( rgb        & 0xFF) / 255.0f;
    s_tint_amt_to = amount;
}

// One colour toward the tint's hue at the colour's OWN luminance, so a dark
// preset stays dark and a bright line stays bright.
static void tint3(float *r, float *g, float *b, float amt) {
    const float L  = 0.30f * *r + 0.59f * *g + 0.11f * *b;
    float Lt = 0.30f * s_tint[0] + 0.59f * s_tint[1] + 0.11f * s_tint[2];
    if (Lt < 0.05f) Lt = 0.05f;
    const float k = L / Lt;
    *r += (s_tint[0] * k - *r) * amt;
    *g += (s_tint[1] * k - *g) * amt;
    *b += (s_tint[2] * k - *b) * amt;
}

static bool ensure_ready(void) {
    if (s_ready) return true;
    if (s_failed) return false;
    for (int i = 0; i < 2; i++) {
        s_vbuf[i] = (cy_vert *)rsxMemalign(128, CY_MAX_VERTS * sizeof(cy_vert));
        if (!s_vbuf[i]) {
            plog("canyon: vertex buffer alloc FAILED -- staying on the wave");
            crash_log("canyon: vbuf alloc failed");
            s_failed = true;
            return false;
        }
        rsxAddressToOffset(s_vbuf[i], &s_voff[i]);
    }
    // The staging copy.  Measured on hardware (09-27): emitting straight
    // into RSX memory cost 19 ms/frame -- field-by-field stores to uncached
    // memory plus the degenerate copies READ back from it -- against ~0.7 ms
    // for the same maths on the host.  Emit into cached RAM, then one
    // sequential pass of aligned 64-bit stores (the WaveVert store rules).
    s_stage = (cy_vert *)memalign(128, CY_MAX_VERTS * sizeof(cy_vert));
    if (!s_stage) { plog("canyon: staging alloc FAILED"); s_failed = true; return false; }
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)wave_fp_data;
    void *fp_ucode; u32 fp_size;
    rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fp_size);
    s_fp_buf = (u32 *)rsxMemalign(256, fp_size);
    if (!s_fp_buf) { plog("canyon: fp alloc FAILED"); s_failed = true; return false; }
    memcpy(s_fp_buf, fp_ucode, fp_size);
    rsxAddressToOffset(s_fp_buf, &s_fp_off);

    sv_spec_init(&s_sv);
    cy_init(&s_st);
    if (!s_bank_tried) bank_load();
    if (s_track < 0) preset_advance(true);
    s_ready = true;
    char b[96];
    snprintf(b, sizeof b, "canyon: ready, %d verts x2 (%u KB RSX), preset %s",
             (int)CY_MAX_VERTS, (unsigned)(2 * CY_MAX_VERTS * sizeof(cy_vert) / 1024),
             canyon_preset_name());
    plog(b);
    crash_log("canyon: ready");
    return true;
}

bool canyon_draw(float bright, bool paused, float alpha) {
    if (!ensure_ready()) return false;
    const u64 t0 = timing_get_us();
    float dt = s_last_us ? (float)(t0 - s_last_us) * 1e-6f : 1.0f / 60.0f;
    s_last_us = t0;
    if (dt > 0.1f) dt = 0.1f;   // re-entry after the screen was closed

    // Audio -> spectrum -> row target.  Between decoder pushes the last
    // spectrum is held; only a real gap (pause, stall, end) reads as silence.
    static float lr[SV_N * 2];
    if (!paused && music_sv_latest(lr)) {
        sv_spec_run(&s_sv, lr, s_L, s_R);
        s_have_spec = true;
        s_fresh_us = t0;
    }
    if (!paused && s_have_spec && t0 - s_fresh_us < STALE_US) cy_feed(&s_st, s_L, s_R);
    else cy_feed_silence(&s_st);

    if (s_xfade < 1.0f) {
        s_xfade += dt / XFADE_S;
        if (s_xfade > 1.0f) s_xfade = 1.0f;
        float e = s_xfade * s_xfade * (3.0f - 2.0f * s_xfade);
        cy_preset_lerp(&s_cur, &s_from, &s_to, e);
    }
    if (!paused) cy_step(&s_st, &s_cur, dt);

    // Ease the tint (the same time as a preset cross-fade), then apply it to
    // a copy: the preset itself -- each song's own look -- is never changed.
    {
        float k = dt / XFADE_S;
        if (k > 1.0f) k = 1.0f;
        if (s_tint_amt <= 0.001f && s_tint_amt_to > 0.0f) {   // first colour: no drift in from black
            s_tint[0] = s_tint_to[0]; s_tint[1] = s_tint_to[1]; s_tint[2] = s_tint_to[2];
        }
        for (int i = 0; i < 3; i++) s_tint[i] += (s_tint_to[i] - s_tint[i]) * k;
        s_tint_amt += (s_tint_amt_to - s_tint_amt) * k;
    }
    cy_preset look = s_cur;
    if (s_tint_amt > 0.001f) {
        tint3(&look.col_r,  &look.col_g,  &look.col_b,  s_tint_amt);
        tint3(&look.fog_r,  &look.fog_g,  &look.fog_b,  s_tint_amt * 0.8f);
        tint3(&look.line_r, &look.line_g, &look.line_b, s_tint_amt * 0.6f);
    }
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    const u32 slot = (s_slot++) & 1;
    cy_counts c;
    const float aspect = display_height ? (float)display_width / (float)display_height : 16.0f / 9.0f;
    const u64 te = timing_get_us();
    const int nv = cy_emit(&s_st, &look, aspect, bright, s_stage, CY_MAX_VERTS, &c);
    if (nv <= 0) return true;
    const bool fading = alpha < 0.999f;
    if (fading) {   // in cached RAM, and only for the half second of a fade
        const u32 a8 = (u32)(alpha * 255.0f + 0.5f);
        for (int k = c.sky_off; k < c.land_off + c.land_n; k++)
            s_stage[k].rgba = (s_stage[k].rgba & 0xFFFFFF00u) | a8;
        for (int k = c.line_off; k < c.line_off + c.line_n; k++) {
            const u32 a = s_stage[k].rgba & 0xFFu;
            s_stage[k].rgba = (s_stage[k].rgba & 0xFFFFFF00u) | ((a * a8) / 255u);
        }
    }
    const u64 tc = timing_get_us();
    {   // 24-byte verts, 128-aligned buffers: always whole 8-byte words.
        const u64 *src = (const u64 *)s_stage;
        volatile u64 *dst = (volatile u64 *)s_vbuf[slot];
        const int words = nv * (int)(sizeof(cy_vert) / 8);
        for (int k = 0; k < words; k++) dst[k] = src[k];
    }
    s_emit_us += tc - te;
    s_copy_us += timing_get_us() - tc;

    rsxVertexProgram   *vpo = (rsxVertexProgram *)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)wave_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_fp_off, GCM_LOCATION_RSX);
    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetScissor(context, 0, 0, (u16)display_width, (u16)display_height);

    const u32 vo = s_voff[slot];
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
        vo, (u8)sizeof(cy_vert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
        vo + 16, (u8)sizeof(cy_vert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
        0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxInvalidateVertexCache(context);

    // Sky + land opaque, far to near; the line added on top.
    // Opaque normally; while fading, blended over what is already there.
    if (fading) {
        rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                                 GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
        rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    }
    rsxSetBlendEnable(context, fading ? GCM_TRUE : GCM_FALSE);
    rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, (u32)c.sky_off, (u32)c.sky_n);
    rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, (u32)c.land_off, (u32)c.land_n);
    rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE, GCM_SRC_ALPHA, GCM_ONE);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, (u32)c.line_off, (u32)c.line_n);

    // wave_draw()'s exit state.
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
        vo + 16, 0, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                             GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    s_cost_us += timing_get_us() - t0;
    if (++s_cost_n >= 300) {
        char b[160];
        snprintf(b, sizeof b, "canyon: %lluus/frame (PPU total; emit %llu, copy %llu) preset=%s rows=%d "
                 "q=%.2f/%.2f/%.2f/%.2f",
                 (unsigned long long)(s_cost_us / s_cost_n),
                 (unsigned long long)(s_emit_us / s_cost_n), (unsigned long long)(s_copy_us / s_cost_n),
                 canyon_preset_name(), s_st.rows_pushed,
                 s_st.quarter[0], s_st.quarter[1], s_st.quarter[2], s_st.quarter[3]);
        plog(b);
        s_cost_us = 0;
        s_cost_n = 0;
        s_emit_us = s_copy_us = 0;
    }
    return true;
}
