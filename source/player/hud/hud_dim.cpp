// HUD dim quad — darkens a rectangle of the framebuffer behind HUD elements.
//
// Three implementations selectable at compile time (define at most one):
//   HUD_DIM_CPU       -- CPU pixel blend (slow, bulletproof)
//   HUD_DIM_GPU_ARRAY -- original array-fetch GPU quad (freezes; testing only)
//   (neither)         -- inline GPU quad (default; fast, no array fetch wedge)
//
// History: the array-fetch quad hard-froze the console when paused (stale
// TEX0 attribute array left bound by the video path wedged the GPU); the CPU
// blend never froze but cost ~183ms/frame in CPU-writable RSX VRAM.  The
// inline quad submits vertices directly into the command FIFO — no fetch, no
// wedge, full speed (this is how Movian draws its overlays).

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "player_hud.h"
#include "player_hud_internal.h"
#include "hud_dim_shaders.h"
#include "rsxutil.h"

extern void crash_log(const char *msg);

// RSX resources for the GPU-drawn dim quad
static u8  *s_hud_vbuf     = NULL;
static u32  s_hud_vbuf_off = 0;
static u32 *s_hud_fp_buf   = NULL;
static u32  s_hud_fp_off   = 0;

void hud_gpu_init(void) {
    s_hud_vbuf = (u8*)rsxMemalign(128, 4 * 20);
    rsxAddressToOffset(s_hud_vbuf, &s_hud_vbuf_off);

    rsxFragmentProgram *fpo = (rsxFragmentProgram*)hud_dim_fp_data;
    void *fp_ucode; u32 fp_size;
    rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fp_size);
    s_hud_fp_buf = (u32*)rsxMemalign(256, fp_size);
    memcpy(s_hud_fp_buf, fp_ucode, fp_size);
    rsxAddressToOffset(s_hud_fp_buf, &s_hud_fp_off);
}

void hud_gpu_shutdown(void) {
    if (s_hud_vbuf)   { rsxFree(s_hud_vbuf);   s_hud_vbuf   = NULL; }
    if (s_hud_fp_buf) { rsxFree(s_hud_fp_buf);  s_hud_fp_buf = NULL; }
}

#if defined(HUD_DIM_CPU)
// Slow fallback: darken strip in-place on color_buffer[curr_fb].
// Precondition: rsxSync() called by caller before hud_draw().
static void dim_rect_cpu(u32 rx, u32 ry, u32 rw, u32 rh, u8 alpha) {
    crash_log("dr1 enter");
    u32 x_end = rx + rw;
    u32 y_end = ry + rh;
    if (x_end > display_width)  x_end = display_width;
    if (y_end > display_height) y_end = display_height;
    u32 inv = 255u - (u32)alpha;
    for (u32 y = ry; y < y_end; y++) {
        u32 *row = color_buffer[curr_fb] + y * display_width;
        for (u32 x = rx; x < x_end; x++) {
            u32 p = row[x];
            u32 r = (((p >> 16) & 0xFFu) * inv) >> 8;
            u32 g = (((p >>  8) & 0xFFu) * inv) >> 8;
            u32 b = (((p      ) & 0xFFu) * inv) >> 8;
            row[x] = (p & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }
    crash_log("dr6 return");
}

#elif defined(HUD_DIM_GPU_ARRAY)
// Array-fetch GPU quad. Known root cause of the original freeze: stale TEX0
// array binding from the video path wedges the RSX vertex fetch unit.
// Kept for reference testing only; do not use in production.
static void draw_dim_rect(u32 rx, u32 ry, u32 rw, u32 rh, u8 alpha) {
    crash_log("dr1 enter");
    if (!s_hud_vbuf || !s_hud_fp_buf) { crash_log("dr1 null rsrc"); return; }

    rsxSync();
    crash_log("dr2 synced");

    float W = (float)display_width;
    float H = (float)display_height;
    float x0 = (float)rx         / W * 2.0f - 1.0f;
    float x1 = (float)(rx + rw)  / W * 2.0f - 1.0f;
    float y0 = 1.0f - (float)ry        / H * 2.0f;
    float y1 = 1.0f - (float)(ry + rh) / H * 2.0f;

    typedef struct { float x, y, z, w; u8 r, g, b, a; } Vert;
    Vert *v = (Vert*)s_hud_vbuf;
    v[0].x=x0; v[0].y=y0; v[0].z=0.f; v[0].w=1.f; v[0].r=0; v[0].g=0; v[0].b=0; v[0].a=alpha;
    v[1].x=x1; v[1].y=y0; v[1].z=0.f; v[1].w=1.f; v[1].r=0; v[1].g=0; v[1].b=0; v[1].a=alpha;
    v[2].x=x0; v[2].y=y1; v[2].z=0.f; v[2].w=1.f; v[2].r=0; v[2].g=0; v[2].b=0; v[2].a=alpha;
    v[3].x=x1; v[3].y=y1; v[3].z=0.f; v[3].w=1.f; v[3].r=0; v[3].g=0; v[3].b=0; v[3].a=alpha;
    crash_log("dr2 verts written");

    rsxVertexProgram  *vpo = (rsxVertexProgram*) hud_dim_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) hud_dim_fp_data;

    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_hud_fp_off, GCM_LOCATION_RSX);
    rsxTextureControl(context, 0, GCM_FALSE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    crash_log("dr3 progs loaded");

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetColorMask(context,
        GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
        GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
        s_hud_vbuf_off, 20, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
        s_hud_vbuf_off + 16, 20, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
        0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    crash_log("dr4 attribs bound");

    rsxInvalidateVertexCache(context);
    rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
    crash_log("dr5 draw issued");

    rsxSetBlendEnable(context, GCM_FALSE);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
        s_hud_vbuf_off + 16, 0, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    crash_log("dr6 return");
}

#else
// Default: inline GPU quad -- all 4 vertices submitted directly into the RSX
// command FIFO. No vertex-array fetch step: no stale array binding can wedge
// the GPU. Runs on the GPU so frame time stays at ~16ms with HUD visible.
static void draw_dim_rect_inline(u32 rx, u32 ry, u32 rw, u32 rh, u8 alpha) {
    crash_log("dr1 enter");
    if (!s_hud_fp_buf) { crash_log("dr1 null rsrc"); return; }

    float W = (float)display_width;
    float H = (float)display_height;
    float x0 = (float)rx        / W * 2.0f - 1.0f;
    float x1 = (float)(rx + rw) / W * 2.0f - 1.0f;
    float y0 = 1.0f - (float)ry        / H * 2.0f;
    float y1 = 1.0f - (float)(ry + rh) / H * 2.0f;

    rsxVertexProgram  *vpo = (rsxVertexProgram*)  hud_dim_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) hud_dim_fp_data;

    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_hud_fp_off, GCM_LOCATION_RSX);
    rsxTextureControl(context, 0, GCM_FALSE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetColorMask(context,
        GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
        GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    crash_log("dr2 state set");

    // POS = attrib 0, COLOR0 = attrib 3 (from hud_dim_vp: vertex.position/vertex.color).
    // Push color before position per vertex; the trailing position command latches
    // the vertex into the FIFO (matches Movian realityAttr4f/realityVertex4f ordering).
    const u8   col[4] = { 0, 0, 0, alpha };
    const float tl[4] = { x0, y0, 0.f, 1.f };
    const float tr[4] = { x1, y0, 0.f, 1.f };
    const float bl[4] = { x0, y1, 0.f, 1.f };
    const float br[4] = { x1, y1, 0.f, 1.f };

    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    crash_log("dr3 begin");

    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    tl);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    tr);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    bl);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    br);
    crash_log("dr4 verts pushed");

    rsxDrawVertexEnd(context);
    rsxSetBlendEnable(context, GCM_FALSE);
    crash_log("dr5 end blend off");
    crash_log("dr6 return");
}
#endif /* dim path selector */

// One entry point for all HUD dimming.  The GPU paths are fenced with
// rsxSync() before returning so CPU pixel writes (text, rects) may follow
// immediately; the CPU path needs no fence.
void hud_dim_rect(u32 rx, u32 ry, u32 rw, u32 rh, u8 alpha) {
#if defined(HUD_DIM_CPU)
    dim_rect_cpu(rx, ry, rw, rh, alpha);
#elif defined(HUD_DIM_GPU_ARRAY)
    draw_dim_rect(rx, ry, rw, rh, alpha);
    rsxSync();
#else
    draw_dim_rect_inline(rx, ry, rw, rh, alpha);
    rsxSync();
#endif
}
