#include <math.h>
#include <string.h>
#include <rsx/rsx.h>
#include "rsxutil.h"
#include "wave_shaders.h"
#include "ui_wave.h"
#include "ui_visuals.h"

#define WAVE_STEP_PX    20
#define WAVE_NS         8      // vertical slices per ribbon (fade resolution)
#define WAVE_MAX_COLS   98     // x columns: supports up to ~1940px wide (720p uses 65)
#define WAVE_STRIP_VERTS (WAVE_MAX_COLS * 2)
#define WAVE_GRAD_VERTS 4
// Fullscreen background-gradient quad + 3 ribbons, each tessellated into
// WAVE_NS horizontal strips so opaque colour interpolation tracks the gradient.
#define WAVE_STRIPS     (3 * WAVE_NS)
#define WAVE_VBUF_BYTES ((WAVE_GRAD_VERTS + WAVE_STRIPS * WAVE_STRIP_VERTS) * 20)

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
// blending switched off, so the result is identical on RPCS3 and hardware.
// WAVE_ALPHA is the crest opacity used for that pre-blend.
static const u32   WAVE_COLOR[3]  = { 0x004A52A8, 0x006C5BD4, 0x003A4290 };
static const u8    WAVE_ALPHA[3]  = { 56, 42, 72 };
static const float WAVE_AMP[3]    = { 34.0f, 24.0f, 16.0f };
static const float WAVE_FREQ[3]   = { 1.5f,  1.8f,  2.1f  };
static const float WAVE_BASEY[3]  = { 0.66f, 0.76f, 0.85f };
static const float WAVE_DPHASE[3] = { 0.008f, 0.013f, 0.018f };

typedef struct {
    float x, y, z, w;
    u8 r, g, b, a;
} WaveVert; // 20 bytes

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
static u8    *s_wave_vbuf       = NULL;
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
    s_wave_vbuf = (u8*)rsxMemalign(256, WAVE_VBUF_BYTES);

    rsxFragmentProgram *fpo = (rsxFragmentProgram*)wave_fp_data;
    void *fp_ucode; u32 fp_size;
    rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fp_size);
    s_wave_fp_buf = (u32*)rsxMemalign(256, fp_size);
    memcpy(s_wave_fp_buf, fp_ucode, fp_size);
    rsxAddressToOffset(s_wave_fp_buf, &s_wave_fp_offset);
}

void wave_draw(void) {
    if (!s_wave_vbuf || !s_wave_fp_buf) return;

    // TEMP (2026-06-14): the GPU vertex-array path below wedges the real RSX on
    // hardware — boot hangs the instant it enters wave_draw at 1920x1080 (grey
    // screen of death; confirmed by crash_log stopping at "13.5d wave_draw").
    // clearScreen(XMB_BG) already laid down the background this frame, so bail
    // here to keep the XMB usable. The wave needs re-implementing off the
    // unreliable vertex-array-fetch path (inline FIFO like the HUD dim quad,
    // or a CPU fill) before this can be re-enabled.
    return;

    rsxVertexProgram  *vpo = (rsxVertexProgram*)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) wave_fp_data;

    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);

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

    // Gradient quad at the head of the buffer (triangle strip).
    {
        WaveVert *gv = (WaveVert*)s_wave_vbuf;
        u8 tr=(XMB_BG_TOP>>16)&0xFF, tg=(XMB_BG_TOP>>8)&0xFF, tb=XMB_BG_TOP&0xFF;
        u8 br=(XMB_BG_BOT>>16)&0xFF, bg=(XMB_BG_BOT>>8)&0xFF, bb=XMB_BG_BOT&0xFF;
        gv[0].x=-1.0f; gv[0].y= 1.0f; gv[0].r=tr; gv[0].g=tg; gv[0].b=tb;
        gv[1].x=-1.0f; gv[1].y=-1.0f; gv[1].r=br; gv[1].g=bg; gv[1].b=bb;
        gv[2].x= 1.0f; gv[2].y= 1.0f; gv[2].r=tr; gv[2].g=tg; gv[2].b=tb;
        gv[3].x= 1.0f; gv[3].y=-1.0f; gv[3].r=br; gv[3].g=bg; gv[3].b=bb;
        for (int i = 0; i < WAVE_GRAD_VERTS; i++) { gv[i].z=0.0f; gv[i].w=1.0f; gv[i].a=255; }
    }

    // Build one triangle strip per (ribbon, slice).  Slice k spans the fraction
    // [k/NS, (k+1)/NS] of the distance from this column's crest down to the
    // screen bottom; each vertex colour is the ribbon tint composited over the
    // background at that exact height, so the strips are drawn fully opaque and
    // never depend on GPU alpha blending (the part that broke on hardware).
    WaveVert *strips = (WaveVert*)(s_wave_vbuf + WAVE_GRAD_VERTS * sizeof(WaveVert));
    for (int li = 0; li < 3; li++) {
        u8 cr=(WAVE_COLOR[li]>>16)&0xFF, cg=(WAVE_COLOR[li]>>8)&0xFF, cb=WAVE_COLOR[li]&0xFF;
        for (int k = 0; k < WAVE_NS; k++) {
            int strip_idx = li * WAVE_NS + k;
            WaveVert *v = strips + strip_idx * WAVE_STRIP_VERTS;
            float ft = (float)k       / WAVE_NS;     // top edge fraction
            float fb = (float)(k + 1) / WAVE_NS;     // bottom edge fraction
            for (int ci = 0; ci < ncols; ci++) {
                float cy = crest[li][ci];
                float yt = cy + (H - cy) * ft;
                float yb = cy + (H - cy) * fb;
                u8 at = (u8)(WAVE_ALPHA[li] * (1.0f - ft));
                u8 ab = (u8)(WAVE_ALPHA[li] * (1.0f - fb));
                u8 btr,btg,btb, bbr,bbg,bbb;
                wave_bg(li, ci, yt, H, crest, &btr,&btg,&btb);
                wave_bg(li, ci, yb, H, crest, &bbr,&bbg,&bbb);
                float cx = (2.0f * colx[ci] / W) - 1.0f;
                WaveVert *vt = &v[ci*2], *vb = &v[ci*2 + 1];
                vt->x=cx; vt->y=1.0f-(2.0f*yt/H); vt->z=0.0f; vt->w=1.0f;
                vt->r=over8(cr,btr,at); vt->g=over8(cg,btg,at); vt->b=over8(cb,btb,at); vt->a=255;
                vb->x=cx; vb->y=1.0f-(2.0f*yb/H); vb->z=0.0f; vb->w=1.0f;
                vb->r=over8(cr,bbr,ab); vb->g=over8(cg,bbg,ab); vb->b=over8(cb,bbb,ab); vb->a=255;
            }
        }
    }

    // Issue draw calls back-to-front: gradient first, then ribbons 0..2 (each
    // ribbon's slices in order).  All geometry is already in RSX memory and
    // pre-composited, so blending stays off.
    u32 vbuf_base;
    rsxAddressToOffset(s_wave_vbuf, &vbuf_base);

    rsxSetBlendEnable(context, GCM_FALSE);

    int npass = 1 + WAVE_STRIPS;             // gradient + every ribbon slice
    for (int pass = 0; pass < npass; pass++) {
        u32 vbuf_off, nverts;
        if (pass == 0) {
            vbuf_off = vbuf_base;
            nverts   = WAVE_GRAD_VERTS;
        } else {
            int strip_idx = pass - 1;
            vbuf_off = vbuf_base + (u32)((WAVE_GRAD_VERTS + strip_idx * WAVE_STRIP_VERTS) * sizeof(WaveVert));
            nverts   = (u32)(ncols * 2);
        }

        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
            vbuf_off,
            (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
            vbuf_off + 16u,
            (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);

        rsxInvalidateVertexCache(context);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, nverts);
    }

    // Restore the UI's standard alpha-blend state for everything drawn after
    // the background this frame (ui_init configures the same).
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
}
