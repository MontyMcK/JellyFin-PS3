// GPU card images -- see ui_card_gpu.h for why.

#include <string.h>

#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "rsxutil.h"
#include "video_shaders.h"     // video_vp_data / video_fp_data
#include "wave_shaders.h"      // wave_vp_data / wave_fp_data (colour passthrough)
#include "ui_visuals.h"        // XMB_FOCUS_RING
#include "ui_card_gpu.h"
#include "plog.h"
#include "jf_paths.h"

extern void crash_log(const char *msg);
#include <stdio.h>

// The player's passthrough programs do exactly what a card needs and are
// already compiled and proven on this hardware:
//   VP: MOV result.position, vertex.attrib[0]; MOV result.texcoord[0], vertex.attrib[8];
//   FP: TEX col, fragment.texcoord[0], texture[0], 2D; MOV result.color, col;
// Reusing them avoids adding a second pair that would have to be kept in step.
static u32 *s_fp_buf = NULL;
static u32  s_fp_off = 0;
static bool s_ready  = false;

// OPT-IN, and off by default.
//
// This is a new RSX path in a part of the app that has never bound a texture
// (the XMB draws zero of them today), and a bad texture bind does not fail
// politely on this hardware -- it can wedge the GPU, which takes the whole
// console off the network and needs a power cycle.  That has already cost one
// this session.
//
// So the switch lives in a file that can be flipped over FTP without
// reflashing anything: put "1" in /dev_hdd0/tmp/jellyfin_gpucards.txt to use
// the GPU path, delete it to go back to the CPU blit.  Same shape as
// jellyfin_dtsma.txt and the other experimental gates.
//
// ON by default since 2026-09-27 (proven on hardware; the CPU path costs
// 6.4 ms of every Home frame).  The file saying 0 turns it off.
#define GPUCARDS_FILE "jellyfin_gpucards.txt"
static bool gpucards_enabled(void)
{
    FILE *f = fopen(jf_data_path(GPUCARDS_FILE), "r");
    if (!f) return true;
    int v = 1;
    bool on = !(fscanf(f, "%d", &v) == 1 && v == 0);
    fclose(f);
    return on;
}

void ui_card_gpu_init(void)
{
    if (s_ready) return;
    if (!gpucards_enabled()) {
        plog("card_gpu: disabled (jellyfin_gpucards.txt absent) -- CPU blit path");
        crash_log("card_gpu: OFF (CPU blit)");
        return;
    }
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)video_fp_data;
    void *ucode; u32 size;
    rsxFragmentProgramGetUCode(fpo, &ucode, &size);
    s_fp_buf = (u32 *)rsxMemalign(256, size);
    if (!s_fp_buf) { plog("card_gpu: fragment program alloc FAILED, staying on the CPU path"); return; }
    memcpy(s_fp_buf, ucode, size);
    rsxAddressToOffset(s_fp_buf, &s_fp_off);
    s_ready = true;
    plog("card_gpu: ready -- card images go through the RSX");
    // Synchronous breadcrumb as well as the log line: if a texture bind wedges
    // the GPU the console goes down with it, and plog's buffered tail is the
    // first thing lost.  crash_log survives a power cycle.
    crash_log("card_gpu: ON (RSX textured quads)");
}

bool ui_card_gpu_ready(void) { return s_ready; }

// Bind a card's VRAM mirror on texture unit 0.
//
// Same descriptor the player's bind_texture() uses, with two deliberate
// differences: no anisotropy (cards are drawn 1:1, nothing to gain) and
// GCM_TEXTURE_NEAREST filtering.  The CPU path this replaces was a straight
// memcpy with no filtering at all, so linear sampling would visibly change
// how every thumbnail looks -- and the brief is explicit that the existing
// design must not shift.
//
// `linear` is for SCALED draws only (ui_card_gpu_draw_scaled).  The video
// player samples this same A8R8G8B8 linear-layout format with LINEAR filtering
// every frame, so the state is proven on this hardware; a 1:1 card keeps
// NEAREST and is bit-identical to before.
static void bind_card(u32 tex_off, u32 w, u32 h, u32 pitch, bool linear)
{
    gcmTexture tex;
    memset(&tex, 0, sizeof(tex));
    tex.format    = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
    tex.mipmap    = 1;
    tex.dimension = GCM_TEXTURE_DIMS_2D;
    tex.cubemap   = GCM_FALSE;
    tex.remap     =
        ((u32)GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT)
      | ((u32)GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT)
      | ((u32)GCM_TEXTURE_REMAP_COLOR_R    << GCM_TEXTURE_REMAP_COLOR_R_SHIFT)
      | ((u32)GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT)
      | ((u32)GCM_TEXTURE_REMAP_COLOR_G    << GCM_TEXTURE_REMAP_COLOR_G_SHIFT)
      | ((u32)GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT)
      | ((u32)GCM_TEXTURE_REMAP_COLOR_B    << GCM_TEXTURE_REMAP_COLOR_B_SHIFT);
    tex.width    = (u16)w;
    tex.height   = (u16)h;
    tex.depth    = 1;
    tex.location = GCM_LOCATION_RSX;
    tex.pitch    = pitch;
    tex.offset   = tex_off;

    // Every card is a different texture, so the cache must be invalidated per
    // bind.  This is the same discipline rsxInvalidateVertexCache() enforces
    // on the vertex side in player_rsx.cpp: a stale binding is precisely the
    // failure this codebase has been bitten by before.
    rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
    rsxLoadTexture(context, 0, &tex);
    rsxTextureControl(context, 0, GCM_TRUE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(context, 0, 0,
        linear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
        linear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
        GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(context, 0,
        GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
        GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void card_draw(u32 tex_off, u32 tex_w, u32 tex_h, u32 tex_pitch,
                      int x, int y, int w, int h, bool linear,
                      float v_top = 0.0f, float v_bot = 1.0f, u8 alpha = 255)
{
    if (!s_ready || w <= 0 || h <= 0 || tex_w == 0 || tex_h == 0) return;
    if (tex_pitch < tex_w * 4 || (tex_pitch & 63u)) return;   // see the header

    rsxVertexProgram   *vpo = (rsxVertexProgram *)  video_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)video_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_fp_off, GCM_LOCATION_RSX);

    bind_card(tex_off, tex_w, tex_h, tex_pitch, linear);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    // Cards are opaque.  Blending them would cost fill rate for nothing and
    // would also make a card's own alpha channel matter, which the CPU memcpy
    // path ignored entirely.
    //
    // Except when the caller asks for an OPACITY (ui_card_gpu_draw_ex): then
    // the blend weight is a CONSTANT, set with rsxSetBlendColor, so the
    // texture's own alpha still does not matter -- a thumbnail decoded with
    // a zero alpha channel draws exactly as translucent as one with 0xFF.
    if (alpha < 255) {
        rsxSetBlendColor(context, (u32)alpha << 24, 0);
        rsxSetBlendFunc(context,
            GCM_CONSTANT_ALPHA, GCM_ONE_MINUS_CONSTANT_ALPHA,
            GCM_CONSTANT_ALPHA, GCM_ONE_MINUS_CONSTANT_ALPHA);
        rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
        rsxSetBlendEnable(context, GCM_TRUE);
    } else {
        rsxSetBlendEnable(context, GCM_FALSE);
    }

    // Framebuffer pixels -> normalised device coords (-1..1, +y up).
    const float W = (float)display_width, H = (float)display_height;
    const float x0 = (2.0f * (float)x / W) - 1.0f;
    const float x1 = (2.0f * (float)(x + w) / W) - 1.0f;
    const float y0 = 1.0f - (2.0f * (float)y / H);
    const float y1 = 1.0f - (2.0f * (float)(y + h) / H);

    // TEX0 before POS: the POS write latches the vertex.  Same ordering the
    // player's inline quads and the HUD dim quad use; getting it backwards
    // silently drops the attribute.
    // v_top / v_bot pick the texture rows the quad spans; the defaults are
    // the whole image, the right way up.  A reflection passes them reversed.
    const float uv_tl[4] = { 0.f, v_top, 0.f, 1.f }, p_tl[4] = { x0, y0, 0.f, 1.f };
    const float uv_tr[4] = { 1.f, v_top, 0.f, 1.f }, p_tr[4] = { x1, y0, 0.f, 1.f };
    const float uv_bl[4] = { 0.f, v_bot, 0.f, 1.f }, p_bl[4] = { x0, y1, 0.f, 1.f };
    const float uv_br[4] = { 1.f, v_bot, 0.f, 1.f }, p_br[4] = { x1, y1, 0.f, 1.f };

    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    rsxDrawVertex4f(context, GCM_VERTEX_ATTRIB_TEX0, uv_tl);
    rsxDrawVertex4f(context, GCM_VERTEX_ATTRIB_POS,  p_tl);
    rsxDrawVertex4f(context, GCM_VERTEX_ATTRIB_TEX0, uv_tr);
    rsxDrawVertex4f(context, GCM_VERTEX_ATTRIB_POS,  p_tr);
    rsxDrawVertex4f(context, GCM_VERTEX_ATTRIB_TEX0, uv_bl);
    rsxDrawVertex4f(context, GCM_VERTEX_ATTRIB_POS,  p_bl);
    rsxDrawVertex4f(context, GCM_VERTEX_ATTRIB_TEX0, uv_br);
    rsxDrawVertex4f(context, GCM_VERTEX_ATTRIB_POS,  p_br);
    rsxDrawVertexEnd(context);

    // Hand the usual blend function back straight away: the colour quads and
    // the wave set their own, but a stale CONSTANT_ALPHA would be a state
    // leak of exactly the kind ui_card_gpu_end() exists to prevent.
    if (alpha < 255)
        rsxSetBlendFunc(context,
            GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
            GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
}

void ui_card_gpu_draw(u32 tex_off, u32 tex_w, u32 tex_h, u32 tex_pitch,
                      int x, int y, int w, int h)
{
    card_draw(tex_off, tex_w, tex_h, tex_pitch, x, y, w, h, false);
}

void ui_card_gpu_draw_scaled(u32 tex_off, u32 tex_w, u32 tex_h, u32 tex_pitch,
                             int x, int y, int w, int h)
{
    const bool one_to_one = (u32)w == tex_w && (u32)h == tex_h;
    card_draw(tex_off, tex_w, tex_h, tex_pitch, x, y, w, h, !one_to_one);
}

void ui_card_gpu_draw_ex(u32 tex_off, u32 tex_w, u32 tex_h, u32 tex_pitch,
                         int x, int y, int w, int h,
                         float v_top, float v_bot, u8 alpha)
{
    if (alpha == 0) return;
    const bool one_to_one = (u32)w == tex_w && (u32)h == tex_h &&
                            v_top == 0.0f && v_bot == 1.0f;
    card_draw(tex_off, tex_w, tex_h, tex_pitch, x, y, w, h, !one_to_one,
              v_top, v_bot, alpha);
}

void ui_card_gpu_clip(int top, int bot)
{
    if (!s_ready) return;
    if (top < 0) top = 0;
    if (bot <= 0 || bot > (int)display_height) bot = (int)display_height;
    if (bot <= top) { top = 0; bot = (int)display_height; }
    rsxSetScissor(context, 0, (u16)top, (u16)display_width, (u16)(bot - top));
}

void ui_card_gpu_end(void)
{
    if (!s_ready) return;
    // Hand back the alpha-blend state ui_init() established, so anything the
    // GPU draws later this frame behaves as it always did.
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
    // Leave the scissor covering the whole surface.  A stale scissor would
    // silently clip the wave and the video quad on later frames, which is the
    // kind of bug that looks like a rendering fault rather than a state leak.
    rsxSetScissor(context, 0, 0, (u16)display_width, (u16)display_height);
}

// --- blended colour quads ------------------------------------------------
// Uses the wave's passthrough programs (POS + COLOR0 -> fragment colour),
// which are already compiled and proven; a private copy of the fragment
// program is uploaded so this does not depend on ui_wave.cpp's internals.

static u32 *s_rect_fp_buf = NULL;
static u32  s_rect_fp_off = 0;

static void rect_fp_init(void)
{
    if (s_rect_fp_buf) return;
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)wave_fp_data;
    void *ucode; u32 size;
    rsxFragmentProgramGetUCode(fpo, &ucode, &size);
    s_rect_fp_buf = (u32 *)rsxMemalign(256, size);
    if (!s_rect_fp_buf) return;
    memcpy(s_rect_fp_buf, ucode, size);
    rsxAddressToOffset(s_rect_fp_buf, &s_rect_fp_off);
}

void ui_rect_gpu_draw(int x, int y, int w, int h, u32 colour, u8 alpha)
{
    if (!s_ready || w <= 0 || h <= 0 || alpha == 0) return;
    rect_fp_init();
    if (!s_rect_fp_buf) return;

    rsxVertexProgram   *vpo = (rsxVertexProgram *)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)wave_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_rect_fp_off, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, alpha == 255 ? GCM_FALSE : GCM_TRUE);

    const float W = (float)display_width, H = (float)display_height;
    const float x0 = (2.0f * (float)x / W) - 1.0f;
    const float x1 = (2.0f * (float)(x + w) / W) - 1.0f;
    const float y0 = 1.0f - (2.0f * (float)y / H);
    const float y1 = 1.0f - (2.0f * (float)(y + h) / H);

    const u8 col[4] = { (u8)((colour >> 16) & 0xFF),
                        (u8)((colour >>  8) & 0xFF),
                        (u8)( colour        & 0xFF),
                        alpha };
    const float p_tl[4] = { x0, y0, 0.f, 1.f };
    const float p_tr[4] = { x1, y0, 0.f, 1.f };
    const float p_bl[4] = { x0, y1, 0.f, 1.f };
    const float p_br[4] = { x1, y1, 0.f, 1.f };

    // Colour first, position last -- the position write latches the vertex.
    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    p_tl);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    p_tr);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    p_bl);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    p_br);
    rsxDrawVertexEnd(context);
}

void ui_card_gpu_selection(int cx, int cy, int w, int h)
{
    if (!s_ready) return;
    // Geometry copied verbatim from xmb_draw_card's CPU version so the two
    // are pixel-interchangeable: T=2 px frame, G=2 px gap, then a 1 px
    // 70-alpha halo one pixel outside that.
    const int T = 2, G = 2, O = G + T;
    ui_rect_gpu_draw(cx - O,     cy - O,     w + 2*O, T,       XMB_FOCUS_RING, 255);
    ui_rect_gpu_draw(cx - O,     cy + h + G, w + 2*O, T,       XMB_FOCUS_RING, 255);
    ui_rect_gpu_draw(cx - O,     cy - G,     T,       h + 2*G, XMB_FOCUS_RING, 255);
    ui_rect_gpu_draw(cx + w + G, cy - G,     T,       h + 2*G, XMB_FOCUS_RING, 255);
    ui_rect_gpu_draw(cx - O - 1, cy - O - 1, w + 2*O + 2, 1,       XMB_FOCUS_RING, 70);
    ui_rect_gpu_draw(cx - O - 1, cy + h + O, w + 2*O + 2, 1,       XMB_FOCUS_RING, 70);
    ui_rect_gpu_draw(cx - O - 1, cy - O,     1,           h + 2*O, XMB_FOCUS_RING, 70);
    ui_rect_gpu_draw(cx + w + O, cy - O,     1,           h + 2*O, XMB_FOCUS_RING, 70);
}

// ---------------------------------------------------------------------------
// Uploaded textures: art too big for the thumbnail cache.
//
// The redesigned detail page draws a full-bleed backdrop and a 216x324 poster.
// Blitting a 1080p backdrop on the CPU is ~8 MB of VRAM writes per frame
// (~11 ms at the measured 767 MB/s), so each is copied into VRAM ONCE, when
// the page loads a title, and the RSX scales it every frame from there.
//
// One buffer per slot, grown (never shrunk) to the largest image it has held,
// so opening title after title does not churn RSX allocations.  The copy is
// row by row into a 64-byte-aligned pitch (the RSX requires it of a linear
// texture) and ends with a sync, the same write-gather rule the vertex
// uploads follow.
#define GPU_TEX_SLOTS 2

typedef struct {
    u32 *mem;
    u32  off;
    u32  cap;          // bytes
    u32  w, h, pitch;  // w == 0: empty
} GpuTex;

static GpuTex s_tex[GPU_TEX_SLOTS];

bool ui_gpu_tex_upload(int slot, const Bitmap *bm)
{
    if (!s_ready || slot < 0 || slot >= GPU_TEX_SLOTS) return false;
    GpuTex *t = &s_tex[slot];
    t->w = 0;
    if (!bm || !bm->pixels || !bm->width || !bm->height) return false;

    const u32 pitch = (bm->width * 4u + 63u) & ~63u;
    const u32 need  = pitch * bm->height;
    if (need > t->cap) {
        if (t->mem) rsxFree(t->mem);
        t->mem = (u32 *)rsxMemalign(128, need);
        t->cap = t->mem ? need : 0;
        if (!t->mem) {
            plog("gpu_tex: VRAM allocation failed -- detail art falls back to the CPU");
            return false;
        }
        rsxAddressToOffset(t->mem, &t->off);
    }
    const u8 *src = (const u8 *)bm->pixels;
    u8       *dst = (u8 *)t->mem;
    for (u32 row = 0; row < bm->height; row++)
        memcpy(dst + (size_t)row * pitch, src + (size_t)row * bm->width * 4u,
               bm->width * 4u);
    __asm__ __volatile__ ("sync" ::: "memory");
    t->w = bm->width; t->h = bm->height; t->pitch = pitch;
    return true;
}

static char s_tex_tag[GPU_TEX_SLOTS][64];

void ui_gpu_tex_clear(int slot)
{
    if (slot >= 0 && slot < GPU_TEX_SLOTS) { s_tex[slot].w = 0; s_tex_tag[slot][0] = 0; }
}

void ui_gpu_tex_set_tag(int slot, const char *item_id)
{
    if (slot < 0 || slot >= GPU_TEX_SLOTS) return;
    snprintf(s_tex_tag[slot], sizeof s_tex_tag[slot], "%s", item_id ? item_id : "");
}

const char *ui_gpu_tex_tag(int slot)
{
    if (slot < 0 || slot >= GPU_TEX_SLOTS || !s_tex[slot].w) return "";
    return s_tex_tag[slot];
}

bool ui_gpu_tex_draw_crop(int slot, int x, int y, int w, int h,
                          float v_top, float v_bot)
{
    if (!s_ready || slot < 0 || slot >= GPU_TEX_SLOTS) return false;
    const GpuTex *t = &s_tex[slot];
    if (!t->w) return false;
    ui_card_gpu_draw_ex(t->off, t->w, t->h, t->pitch, x, y, w, h, v_top, v_bot, 255);
    return true;
}

bool ui_gpu_tex_draw(int slot, int x, int y, int w, int h)
{
    if (!s_ready || slot < 0 || slot >= GPU_TEX_SLOTS) return false;
    const GpuTex *t = &s_tex[slot];
    if (!t->w) return false;
    ui_card_gpu_draw_scaled(t->off, t->w, t->h, t->pitch, x, y, w, h);
    return true;
}

bool ui_gpu_tex_draw_a(int slot, int x, int y, int w, int h, u8 alpha)
{
    if (alpha >= 255) return ui_gpu_tex_draw(slot, x, y, w, h);
    if (!s_ready || slot < 0 || slot >= GPU_TEX_SLOTS) return false;
    const GpuTex *t = &s_tex[slot];
    if (!t->w) return false;
    ui_card_gpu_draw_ex(t->off, t->w, t->h, t->pitch, x, y, w, h, 0.0f, 1.0f, alpha);
    return true;
}
