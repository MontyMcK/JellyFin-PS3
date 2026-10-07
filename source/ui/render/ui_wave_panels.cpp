// Immediate-mode panels for the spine: elliptical glows, rounded rectangles
// and alpha ramps.
//
// The detail screen (design-import-v3, "06 · Item detail") and the depth
// stage are built from rounded panels, pills, glows and scrims.  Drawing any of
// those on the CPU means blending against the framebuffer -- the one thing
// UI-BRIEF.md rules out -- so they are RSX immediate-mode geometry on the
// wave's own passthrough programs, like the divider and the tab-strip glow in
// ui_wave.cpp.
//
// Immediate mode on purpose: nothing here allocates, owns or reads a vertex
// buffer, so none of it goes near the JellyWave upload path or the vertex-
// array index range that draws stale data on this console (memory:
// rsx-low-vertex-index-stale).  Each call is one Begin/End run straight into
// the FIFO.  They live in their own file so ui_wave.cpp -- the renderer under
// active hardware debugging -- only gained one accessor, wave_imm_bind().
//
// All of these MUST be issued in a GPU phase, before the frame's rsxSync().

#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include "rsxutil.h"
#include "ui_wave.h"

static inline void panel_vert(float px, float py, u32 rgb, u8 a) {
    const float W = (float)display_width, H = (float)display_height;
    const u8    c4[4] = { (u8)((rgb >> 16) & 0xFF), (u8)((rgb >> 8) & 0xFF),
                          (u8)(rgb & 0xFF), a };
    const float p4[4] = { (2.0f * px / W) - 1.0f, 1.0f - (2.0f * py / H),
                          0.0f, 1.0f };
    // colour latched first, position last -- the position write commits it.
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, c4);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    p4);
}

static inline u32 panel_lerp_rgb(u32 a, u32 b, float t) {
    u32 out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        out |= (u32)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return out;
}

// A 12-segment radial fan (centre plus a closed 12-point ring): 14 vertices.
// The perimeter alpha is zero, so the RSX performs the falloff.  Alpha
// interpolates linearly from the centre to the ring, which is exactly a CSS
// radial-gradient with two stops -- what the spine's glow band is authored
// as -- and separate x/y radii make it the design's ellipse.  At the peak
// alphas used (under 0.3) the 12 facets are not visible.
void wave_draw_glow_gpu(int cx_px, int cy_px, int rx_px, int ry_px,
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
    if (rx_px <= 0 || ry_px <= 0 || !peak_alpha || !wave_imm_bind()) return;

    const u32 rgb = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_FAN);
    panel_vert((float)cx_px, (float)cy_px, rgb, peak_alpha);
    for (int i = 0; i < 13; i++)
        panel_vert((float)cx_px + ring[i][0] * (float)rx_px,
                   (float)cy_px + ring[i][1] * (float)ry_px, rgb, 0);
    rsxDrawVertexEnd(context);
}

void wave_draw_rrect_gpu(int x, int y, int w, int h, int r,
                         u32 rgb_left, u32 rgb_right, u8 alpha) {
    if (w <= 0 || h <= 0 || !alpha || !wave_imm_bind()) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;

    // Quarter circle in 15 degree steps (0..90), cos and sin, no libm.
    static const float QC[7] = { 1.0f, 0.965926f, 0.866025f, 0.707107f,
                                 0.5f, 0.258819f, 0.0f };
    const float fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h;
    const float fr = (float)r;
    // Corner centres, clockwise from top-right (screen y points down).
    const float ccx[4] = { fx + fw - fr, fx + fw - fr, fx + fr,      fx + fr      };
    const float ccy[4] = { fy + fr,      fy + fh - fr, fy + fh - fr, fy + fr      };

    #define PV(px, py) panel_vert((px), (py),                                  \
        panel_lerp_rgb(rgb_left, rgb_right, ((px) - fx) / fw), alpha)

    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_FAN);
    PV(fx + fw * 0.5f, fy + fh * 0.5f);
    float first_x = 0.0f, first_y = 0.0f;
    for (int c = 0; c < 4; c++) {
        for (int k = 0; k < 7; k++) {
            // Each corner sweeps 90 degrees, a = start + 15k, and the point
            // is centre + r (cos a, sin a).  With c = cos 15k, s = sin 15k:
            const float c15 = QC[k], s15 = QC[6 - k];
            float dx, dy;
            switch (c) {
            case 0:  dx =  s15; dy = -c15; break;          // a = -90 + 15k
            case 1:  dx =  c15; dy =  s15; break;          // a =   0 + 15k
            case 2:  dx = -s15; dy =  c15; break;          // a =  90 + 15k
            default: dx = -c15; dy = -s15; break;          // a = 180 + 15k
            }
            const float px = ccx[c] + dx * fr, py = ccy[c] + dy * fr;
            if (c == 0 && k == 0) { first_x = px; first_y = py; }
            PV(px, py);
        }
    }
    PV(first_x, first_y);                                  // close the fan
    rsxDrawVertexEnd(context);
    #undef PV
}

void wave_draw_rrect_outline_gpu(int x, int y, int w, int h, int r, int t,
                                 u32 line_rgb, u8 line_a,
                                 u32 fill_left, u32 fill_right, u8 fill_a) {
    // An outline is the outer shape in the line colour with the inner one on
    // top.  Both are filled fans, so there is no hairline rasterisation to get
    // wrong -- the border is exactly t pixels on every side.
    wave_draw_rrect_gpu(x, y, w, h, r, line_rgb, line_rgb, line_a);
    if (w > 2 * t && h > 2 * t)
        wave_draw_rrect_gpu(x + t, y + t, w - 2 * t, h - 2 * t,
                            r > t ? r - t : 0, fill_left, fill_right, fill_a);
}

void wave_draw_ramp_gpu(int x, int y, int w, int h, u32 rgb, bool vertical,
                        int n, const float *pos, const u8 *alpha) {
    if (w <= 0 || h <= 0 || n < 2 || !wave_imm_bind()) return;
    const float fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h;
    // A strip across the ramp's axis: one pair of vertices per stop, so the
    // RSX interpolates alpha between stops exactly as a CSS gradient does.
    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    for (int i = 0; i < n; i++) {
        if (vertical) {
            const float py = fy + fh * pos[i];
            panel_vert(fx,      py, rgb, alpha[i]);
            panel_vert(fx + fw, py, rgb, alpha[i]);
        } else {
            const float px = fx + fw * pos[i];
            panel_vert(px, fy,      rgb, alpha[i]);
            panel_vert(px, fy + fh, rgb, alpha[i]);
        }
    }
    rsxDrawVertexEnd(context);
}

// ---------------------------------------------------------------------------
// Experience panels: the buffering screen's ring and mark, the music screen's
// halo.  Same rules as everything above: immediate mode, GPU phase only.

#include <math.h>
#include "jf_logo_geom.h"

void wave_draw_ring_arc_gpu(int cx, int cy, float r_px, float t_px,
                            float a0, float arc, int segs,
                            u32 rgb_tail, u8 a_tail, u32 rgb_head, u8 a_head) {
    if (r_px <= 0.0f || t_px <= 0.0f || arc <= 0.0f || segs < 2 || !wave_imm_bind()) return;
    if (arc > 1.0f) arc = 1.0f;
    if (segs > 128) segs = 128;
    const float ri = r_px - t_px * 0.5f, ro = r_px + t_px * 0.5f;
    const float two_pi = 6.2831853f;
    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    for (int i = 0; i <= segs; i++) {
        const float u = (float)i / (float)segs;             // tail 0 -> head 1
        const float ang = (a0 + arc * u) * two_pi;
        const float sx = sinf(ang), cy_ = -cosf(ang);       // 0 turns = up
        const u32 rgb = panel_lerp_rgb(rgb_tail, rgb_head, u);
        const u8  a   = (u8)((float)a_tail + ((float)a_head - (float)a_tail) * u + 0.5f);
        panel_vert((float)cx + sx * ro, (float)cy + cy_ * ro, rgb, a);
        panel_vert((float)cx + sx * ri, (float)cy + cy_ * ri, rgb, a);
    }
    rsxDrawVertexEnd(context);
}

void wave_draw_jf_logo_gpu(int cx, int cy, float half_w_px,
                           u32 rgb_a, u32 rgb_b, u8 alpha) {
    if (half_w_px <= 0.0f || !alpha || !wave_imm_bind()) return;
    const float fx = (float)cx, fy = (float)cy, s = half_w_px;
    // The logo's gradient runs from its upper left to its lower right; in
    // mark units that is t = (x + 0.25 y) mapped from [-1.25, 1.25] to [0, 1].
    #define LV(px, py) panel_vert(fx + (px) * s, fy + (py) * s, \
        panel_lerp_rgb(rgb_a, rgb_b, ((px) + 0.25f * (py) + 1.25f) / 2.5f), alpha)
    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    for (int i = 0; i <= JF_LOGO_N; i++) {
        const int k = i % JF_LOGO_N;
        LV(JF_LOGO_OUTER[k][0], JF_LOGO_OUTER[k][1]);
        LV(JF_LOGO_HOLE[k][0],  JF_LOGO_HOLE[k][1]);
    }
    rsxDrawVertexEnd(context);
    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_FAN);
    LV(JF_LOGO_INNER_C[0], JF_LOGO_INNER_C[1]);
    for (int i = 0; i <= JF_LOGO_M; i++) {
        const int k = i % JF_LOGO_M;
        LV(JF_LOGO_INNER[k][0], JF_LOGO_INNER[k][1]);
    }
    rsxDrawVertexEnd(context);
    #undef LV
}
