#include <math.h>
#include <string.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include "rsxutil.h"
#include "wave_shaders.h"
#include "ui_wave.h"
#include "ui_visuals.h"

#define WAVE_STEP_PX    20
#define WAVE_NS         8      // vertical slices per ribbon (fade resolution)
#define WAVE_MAX_COLS   98     // x columns: supports up to ~1940px wide (720p uses 65)

// Ribbons read as translucent veils over the background gradient: bright at
// the crest, fading to nothing below.  We do NOT lean on GPU per-vertex alpha
// + blending for that fade — the RSX vertex-array-fetch path is unreliable on
// real hardware (the same class of bug that retired HUD_DIM_GPU_ARRAY), so the
// alpha was being dropped and the ribbons rendered as solid bright bands (the
// washed-out blue glow seen on a real PS3).
//
// Instead the fade is *baked into opaque vertex colours* on the CPU.  Each
// ribbon is tessellated into WAVE_NS horizontal slices from its crest down to
// the screen bottom; at every grid node the colour is the ribbon tint
// composited over the gradient (and any earlier ribbons) at that exact height,
// using the same maths as tools/ui_preview/preview.c.  Drawn fully opaque, the
// GPU's plain colour interpolation reproduces the veil pixel-for-pixel with
// blending switched off.
//
// The vertices are streamed straight into the RSX command FIFO with
// rsxDrawVertex4f/4ub (immediate mode) rather than fetched from a bound vertex
// array — exactly how the player's HUD dim quad draws.  That keeps the work on
// the GPU (a CPU framebuffer fill at this size costs ~180ms/frame) while
// avoiding the vertex-array-fetch unit that grey-screens real hardware.
// WAVE_ALPHA is the crest opacity used for the pre-blend.
static const u32   WAVE_COLOR[3]  = { 0x004A52A8, 0x006C5BD4, 0x003A4290 };
static const u8    WAVE_ALPHA[3]  = { 56, 42, 72 };
static const float WAVE_AMP[3]    = { 30.0f, 22.0f, 15.0f };
static const float WAVE_FREQ[3]   = { 1.5f,  1.8f,  2.1f  };
// Crest baselines as a fraction of screen height — kept low so the ribbons
// sit in the bottom quarter of the screen instead of climbing into the middle.
static const float WAVE_BASEY[3]  = { 0.78f, 0.85f, 0.91f };
static const float WAVE_DPHASE[3] = { 0.008f, 0.013f, 0.018f };

// Sample the background gradient (XMB_BG_TOP -> XMB_BG_BOT, top to bottom) at
// screen-space y in [0,H], returning the three 8-bit channels.  Mirrors the
// gradient quad and tools/ui_preview/preview.c exactly.
static inline void grad_sample(float y, float H, u8 *r, u8 *g, u8 *b) {
    float t = (H > 1.0f) ? (y / (H - 1.0f)) : 0.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    int tr=(XMB_BG_TOP>>16)&0xFF, tg=(XMB_BG_TOP>>8)&0xFF, tb=XMB_BG_TOP&0xFF;
    int br=(XMB_BG_BOT>>16)&0xFF, bg=(XMB_BG_BOT>>8)&0xFF, bb=XMB_BG_BOT&0xFF;
    *r = (u8)(tr + (int)((br - tr) * t));
    *g = (u8)(tg + (int)((bg - tg) * t));
    *b = (u8)(tb + (int)((bb - tb) * t));
}

// src over dst with 8-bit alpha: result = (src*a + dst*(255-a)) / 255.
static inline u8 over8(u8 src, u8 dst, u8 a) {
    return (u8)(((int)src * a + (int)dst * (255 - a)) / 255);
}

static float  s_wave_phase[3]   = { 0.0f, 0.0f, 0.0f };
static u32   *s_wave_fp_buf     = NULL;
static u32    s_wave_fp_offset  = 0;

// Crest height (screen-space y) of ribbon li at horizontal position fx.
static inline float wave_crest(int li, float fx, float W, float H) {
    float wy = H * WAVE_BASEY[li]
             + sinf(fx / W * (WAVE_FREQ[li] * 3.14159265f) + s_wave_phase[li]) * WAVE_AMP[li];
    if (wy < 0.0f) wy = 0.0f;
    if (wy > H)    wy = H;
    return wy;
}

// Background colour at (column ci, screen y): the gradient with ribbons
// [0..upto) already composited in, matching tools/ui_preview/preview.c's
// cumulative per-pixel blend.  crest[j][ci] is ribbon j's crest at column ci.
static void wave_bg(int upto, int ci, float y, float H,
                    const float crest[3][WAVE_MAX_COLS], u8 *r, u8 *g, u8 *b) {
    grad_sample(y, H, r, g, b);
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
    s_wave_phase[0] = 0.0f;
    s_wave_phase[1] = 0.0f;
    s_wave_phase[2] = 0.0f;
}

void wave_init(void) {
    rsxFragmentProgram *fpo = (rsxFragmentProgram*)wave_fp_data;
    void *fp_ucode; u32 fp_size;
    rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fp_size);
    s_wave_fp_buf = (u32*)rsxMemalign(256, fp_size);
    memcpy(s_wave_fp_buf, fp_ucode, fp_size);
    rsxAddressToOffset(s_wave_fp_buf, &s_wave_fp_offset);
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
    if (!s_wave_fp_buf) return;

    rsxVertexProgram  *vpo = (rsxVertexProgram*)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) wave_fp_data;

    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendEnable(context, GCM_FALSE);    // colours are pre-composited, opaque

    float W = (float)display_width;
    float H = (float)display_height;

    for (int li = 0; li < 3; li++) {
        s_wave_phase[li] += WAVE_DPHASE[li];
        if (s_wave_phase[li] > 62.83f) s_wave_phase[li] -= 62.83f;
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
    static float crest[3][WAVE_MAX_COLS];
    for (int li = 0; li < 3; li++)
        for (int ci = 0; ci < ncols; ci++)
            crest[li][ci] = wave_crest(li, colx[ci], W, H);

    // Background-gradient quad (XMB_BG_TOP -> XMB_BG_BOT), streamed inline.
    {
        u8 tr=(XMB_BG_TOP>>16)&0xFF, tg=(XMB_BG_TOP>>8)&0xFF, tb=XMB_BG_TOP&0xFF;
        u8 br=(XMB_BG_BOT>>16)&0xFF, bg=(XMB_BG_BOT>>8)&0xFF, bb=XMB_BG_BOT&0xFF;
        rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
        wave_vtx(-1.0f,  1.0f, tr, tg, tb);   // top-left
        wave_vtx(-1.0f, -1.0f, br, bg, bb);   // bottom-left
        wave_vtx( 1.0f,  1.0f, tr, tg, tb);   // top-right
        wave_vtx( 1.0f, -1.0f, br, bg, bb);   // bottom-right
        rsxDrawVertexEnd(context);
    }

    // One triangle strip per (ribbon, slice), back-to-front.  Slice k spans the
    // fraction [k/NS, (k+1)/NS] of the distance from this column's crest down to
    // the screen bottom; each vertex colour is the ribbon tint composited over
    // the background at that exact height, so the strips are drawn fully opaque
    // and never depend on GPU alpha blending (the part that broke on hardware).
    for (int li = 0; li < 3; li++) {
        u8 cr=(WAVE_COLOR[li]>>16)&0xFF, cg=(WAVE_COLOR[li]>>8)&0xFF, cb=WAVE_COLOR[li]&0xFF;
        for (int k = 0; k < WAVE_NS; k++) {
            float ft = (float)k       / WAVE_NS;     // top edge fraction
            float fb = (float)(k + 1) / WAVE_NS;     // bottom edge fraction
            u8 at = (u8)(WAVE_ALPHA[li] * (1.0f - ft));
            u8 ab = (u8)(WAVE_ALPHA[li] * (1.0f - fb));
            rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
            for (int ci = 0; ci < ncols; ci++) {
                float cy = crest[li][ci];
                float yt = cy + (H - cy) * ft;
                float yb = cy + (H - cy) * fb;
                u8 btr,btg,btb, bbr,bbg,bbb;
                wave_bg(li, ci, yt, H, crest, &btr,&btg,&btb);
                wave_bg(li, ci, yb, H, crest, &bbr,&bbg,&bbb);
                float cx = (2.0f * colx[ci] / W) - 1.0f;
                wave_vtx(cx, 1.0f - (2.0f*yt/H), over8(cr,btr,at), over8(cg,btg,at), over8(cb,btb,at));
                wave_vtx(cx, 1.0f - (2.0f*yb/H), over8(cr,bbr,ab), over8(cg,bbg,ab), over8(cb,bbb,ab));
            }
            rsxDrawVertexEnd(context);
        }
    }

    // Restore the UI's standard alpha-blend state for everything drawn after
    // the background this frame (ui_init configures the same).
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
}
