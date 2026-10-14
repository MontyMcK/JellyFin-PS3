// Cold-boot animation -- renderer.  See boot_anim.h for the shape of it and
// docs/boot-animation.md for the whole story; the timeline is boot_seq.h.
//
// RSX DISCIPLINE.  This file adds no new GPU technique.  Every draw is one of
// the things this client already does on hardware:
//
//   * blended colour quads through the wave's passthrough programs, streamed
//     inline -- the veil, exactly like wave_dim_screen() and the hairline;
//   * the existing radial glow fan, wave_draw_glow_gpu() -- the halo;
//   * a textured quad through the video passthrough programs with the
//     player's own LINEAR, CLAMP_TO_EDGE bind -- the mark, and the SHINE's
//     glint, which is the same quad over a small texture the CPU rewrites
//     (write-only) each frame the glint shows, added with ONE / ONE;
//   * triangle fans on the wave's colour programs -- the SHINE's twinkle,
//     built exactly like the glow fan.
//
// The textured quad is only used when the user has already opted into RSX
// textures in the XMB (jellyfin_gpucards.txt or jellyfin_gputext.txt), since
// a bad texture bind on this hardware takes the console down rather than
// failing.  Without that, the mark is drawn by the CPU instead: write-only
// over black (cheap), or blended over the XMB after a fence (not cheap, but
// only for the ~0.6 s the large mark travels over a revealed screen).
//
// Nothing here touches JellyWave: wave_draw() is called by the XMB loop as it
// always is, and this module only ever draws AFTER it, on top.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

#include <rsx/rsx.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>

#include "rsxutil.h"
#include "ui.h"
#include "ui_visuals.h"
#include "ui_wave.h"
#include "ui_card_gpu.h"
#include "ui_text_gpu.h"
#include "video_shaders.h"     // video_vp_data / video_fp_data (TEX passthrough)
#include "wave_shaders.h"      // wave_vp_data / wave_fp_data (colour passthrough)
#include "stb_image.h"
#include "jfmark_hd_png.h"     // the lockup mark at 256px -- this TU only
#include "boot_anim.h"
#include "timing.h"
#include "plog.h"
#include "jf_paths.h"

extern void crash_log(const char *msg);

// The mark raster's geometry, in the design's viewBox units: 88 across, of
// which the bell is 72, centred (jfmark_png.h's JFMARK_SPAN_U / _BELL_U --
// restated because that header DEFINES its arrays and belongs to one TU).
#define MARK_SPAN_U 88
#define MARK_BELL_U 72

// Layout of the centred mark, as fractions of the screen height so every
// video mode frames it the same.  Slightly above centre: optical centre.
#define CENTRE_BELL_H   0.17f
#define CENTRE_Y_H      0.47f
#define HALO_RADIUS     1.55f   // x bell
#define STATUS_TEXT     "Connecting to server"
#define STATUS_GAP_H    0.045f  // below the mark's box, x screen height

// The lockup draws the docking mark itself (CPU, its own 80px raster) once
// the mark is within this factor of its final size AND the veil is gone.
#define CPU_ZONE        1.6f

#define BOOTANIM_FILE   "jellyfin_bootanim.txt"

// -------------------------------------------------------------------------
// State
// -------------------------------------------------------------------------

static bool      s_on      = false;   // between begin and DONE
static bool      s_flipped = false;   // a boot frame has been flipped
static BootSeq   s_seq;
static BootFrame s_frame;
static u64       s_last_us = 0;

// Where the mark is this frame, and who draws it.
static float s_mcx, s_mcy, s_mbell;
static bool  s_mark_overlay = false;  // the overlay draws it (else lockup/none)

static BootLockupPose s_pose;
static bool           s_pose_live = false;

// GPU resources.
static bool s_gpu = false;            // textured mark available
static u32 *s_tex_fp = NULL; static u32 s_tex_fp_off = 0;   // video_fp copy
static u32 *s_col_fp = NULL; static u32 s_col_fp_off = 0;   // wave_fp copy

// Pre-filtered sizes of the mark, sqrt(2) apart, so the bilinear sampler
// never minifies by more than 1.41:1 -- a 256px texture drawn at 60px with
// plain bilinear would sparkle as it moved.  A manual mip chain, because a
// linear-layout RSX texture takes no hardware mipmaps.
#define LEVELS 5
static const int k_level_px[LEVELS] = { 256, 181, 128, 91, 64 };
struct MarkLevel { u32 *mem; u32 off; int px; u32 pitch; };
static MarkLevel s_lv[LEVELS];

// CPU copy of the 256px master, premultiplied ARGB.  Kept only on the CPU
// path; the GPU path frees it as soon as the levels are uploaded.
static u32 *s_master = NULL;
static int  s_master_px = 0;

// The SHINE's glint layer.  s_glint_a is the mark's coverage at GLINT_PX
// (16 KB, main RAM); the texture is rewritten from it every frame the glint
// shows -- the band times the coverage, so it only ever lights the mark --
// and drawn additively over the mark at the mark's box.  128px is plenty: the
// band is soft, and its only hard edge is the mark's own, which the mark
// itself draws sharp underneath.
//
// Rewriting a texture the RSX samples is safe HERE because every write
// happens after the frame's waitflip() (pump) or rsxSync() (XMB overlay), so
// the previous frame's draw of it has retired.
#define GLINT_PX     128
#define GLINT_PITCH  (GLINT_PX * 4)         // 512: a 64-byte multiple
static u8  *s_glint_a   = NULL;
static u32 *s_glint_tex = NULL;
static u32  s_glint_off = 0;

// Where the twinkle sits: the bell's apex, which the 256px raster puts at
// exactly the top of the bell -- (127.5, 23) of 256 -- a touch inside it.
#define SPARK_APEX_Y  (-0.47f)   // x bell, from the mark's centre
#define SPARK_SIZE     0.38f     // arm length at full twinkle, x bell

// -------------------------------------------------------------------------
// Setup
// -------------------------------------------------------------------------

static bool gate_on(void)
{
    FILE *f = fopen(jf_data_path(BOOTANIM_FILE), "r");
    if (!f) return true;                       // default ON
    int v = 1;
    if (fscanf(f, "%d", &v) != 1) v = 1;
    fclose(f);
    return v != 0;
}

static u32 *upload_fp(const unsigned char *data, u32 *off)
{
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)data;
    void *ucode; u32 size;
    rsxFragmentProgramGetUCode(fpo, &ucode, &size);
    u32 *buf = (u32 *)rsxMemalign(256, size);
    if (!buf) return NULL;
    memcpy(buf, ucode, size);
    rsxAddressToOffset(buf, off);
    return buf;
}

// Area-average a premultiplied square into another size.  Premultiplied is
// what makes plain averaging correct: straight-alpha averaging would drag
// the transparent border's black into the edge pixels as a dark fringe.
static void resample(const u32 *src, int sp, u32 *dst, int dp, u32 dst_pitch_px)
{
    for (int y = 0; y < dp; y++) {
        int sy0 = y * sp / dp, sy1 = (y + 1) * sp / dp;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dp; x++) {
            int sx0 = x * sp / dp, sx1 = (x + 1) * sp / dp;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            u32 a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) {
                    u32 p = src[yy * sp + xx];
                    a += p >> 24; r += (p >> 16) & 0xFF;
                    g += (p >> 8) & 0xFF; b += p & 0xFF; n++;
                }
            dst[y * dst_pitch_px + x] =
                ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
}

static bool load_mark(void)
{
    const int v = xmb_mark_variant();
    int w, h, comp;
    unsigned char *img = stbi_load_from_memory(
        v ? jfmark_hd_gold_png : jfmark_hd_cool_png,
        (int)(v ? jfmark_hd_gold_png_len : jfmark_hd_cool_png_len),
        &w, &h, &comp, 4);
    if (!img) return false;
    if (w != JFMARK_HD_MASTER || h != JFMARK_HD_MASTER) { stbi_image_free(img); return false; }
    u32 *m = (u32 *)malloc((size_t)w * h * 4);
    if (!m) { stbi_image_free(img); return false; }
    for (int i = 0; i < w * h; i++) {
        u32 a = img[i * 4 + 3];
        m[i] = (a << 24) |
               (((u32)img[i * 4 + 0] * a / 255) << 16) |
               (((u32)img[i * 4 + 1] * a / 255) << 8) |
                ((u32)img[i * 4 + 2] * a / 255);
    }
    stbi_image_free(img);
    s_master = m;
    s_master_px = w;
    return true;
}

static bool upload_levels(void)
{
    for (int i = 0; i < LEVELS; i++) {
        const int px = k_level_px[i];
        const u32 pitch = ((u32)px * 4 + 63u) & ~63u;     // 64-byte rows
        u32 *mem = (u32 *)rsxMemalign(128, pitch * (u32)px);
        if (!mem) return false;
        // Resample straight into video memory: sequential stores, no reads.
        resample(s_master, s_master_px, mem, px, pitch / 4);
        s_lv[i].mem = mem;
        s_lv[i].px = px;
        s_lv[i].pitch = pitch;
        rsxAddressToOffset(mem, &s_lv[i].off);
    }
    return true;
}

static void release(void)
{
    // Called only once nothing queued can still sample these: after a fence,
    // or from an XMB frame after DONE (whose rsxSync has already retired
    // every earlier frame's commands).
    for (int i = 0; i < LEVELS; i++)
        if (s_lv[i].mem) { rsxFree(s_lv[i].mem); s_lv[i].mem = NULL; }
    if (s_tex_fp) { rsxFree(s_tex_fp); s_tex_fp = NULL; }
    if (s_col_fp) { rsxFree(s_col_fp); s_col_fp = NULL; }
    if (s_master) { free(s_master); s_master = NULL; }
    if (s_glint_tex) { rsxFree(s_glint_tex); s_glint_tex = NULL; }
    if (s_glint_a) { free(s_glint_a); s_glint_a = NULL; }
    s_gpu = false;
}

// The glint's coverage map, from the master before it is freed.  Failure
// only costs the glint (the twinkle and everything else still play).
static void make_glint(void)
{
    u32 *tmp = (u32 *)malloc(GLINT_PX * GLINT_PX * 4);
    s_glint_a = (u8 *)malloc(GLINT_PX * GLINT_PX);
    s_glint_tex = (u32 *)rsxMemalign(128, GLINT_PITCH * GLINT_PX);
    if (!tmp || !s_glint_a || !s_glint_tex) {
        free(tmp);
        if (s_glint_a) { free(s_glint_a); s_glint_a = NULL; }
        if (s_glint_tex) { rsxFree(s_glint_tex); s_glint_tex = NULL; }
        plog("boot: no memory for the glint -- the shine is the twinkle only");
        return;
    }
    resample(s_master, s_master_px, tmp, GLINT_PX, GLINT_PX);
    for (int i = 0; i < GLINT_PX * GLINT_PX; i++) s_glint_a[i] = (u8)(tmp[i] >> 24);
    free(tmp);
    rsxAddressToOffset(s_glint_tex, &s_glint_off);
}

// Rewrite the glint texture for this frame: premultiplied near-white, alpha
// 0 (it is ADDED, so alpha plays no part), zero wherever the band is not.
static void fill_glint(float pos, float strength)
{
    const float gain = BOOT_GLINT_GAIN * strength / 255.0f;
    const float inv = 1.0f / (float)GLINT_PX;
    for (int y = 0; y < GLINT_PX; y++) {
        u32 *row = s_glint_tex + y * (GLINT_PITCH / 4);
        const u8 *a = s_glint_a + y * GLINT_PX;
        const float v = ((float)y + 0.5f) * inv;
        for (int x = 0; x < GLINT_PX; x++) {
            u32 out = 0;
            if (a[x]) {
                float k = boot_glint_at(((float)x + 0.5f) * inv, v, pos) *
                          gain * (float)a[x];
                if (k > 0.0f) {
                    if (k > 1.0f) k = 1.0f;
                    out = ((u32)(255.0f * k) << 16) | ((u32)(245.0f * k) << 8) |
                           (u32)(250.0f * k);
                }
            }
            row[x] = out;               // sequential stores, no reads
        }
    }
}

// -------------------------------------------------------------------------
// GPU draws
// -------------------------------------------------------------------------

static void restore_blend(void)
{
    // The state ui_init() established, which everything after us expects.
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
}

// Full-width black rows of (y, alpha), interpolated by the rasteriser: the
// veil, or with two rows a full-screen fade.  Two vertices per row.
static void gpu_rows(const float *ys, const unsigned char *as, int n)
{
    if (!s_col_fp || n < 2) return;
    rsxVertexProgram   *vpo = (rsxVertexProgram *)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)wave_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_col_fp_off, GCM_LOCATION_RSX);
    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    restore_blend();

    const float H = (float)display_height;
    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    for (int i = 0; i < n; i++) {
        const u8    c4[4] = { 0, 0, 0, as[i] };
        const float y     = 1.0f - 2.0f * ys[i] / H;
        const float pl[4] = { -1.0f, y, 0.0f, 1.0f };
        const float pr[4] = {  1.0f, y, 0.0f, 1.0f };
        // Colour first, position last -- the position write latches.
        rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, c4);
        rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    pl);
        rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, c4);
        rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    pr);
    }
    rsxDrawVertexEnd(context);
}

static void gpu_fade(float opacity)
{
    if (opacity >= 1.0f) return;
    const float ys[2] = { 0.0f, (float)display_height };
    const u8 a = (u8)((1.0f - (opacity < 0.0f ? 0.0f : opacity)) * 255.0f + 0.5f);
    const unsigned char as[2] = { a, a };
    if (a) gpu_rows(ys, as, 2);
}

// The mark as a textured quad: premultiplied texels, ONE / ONE_MINUS_SRC_ALPHA,
// LINEAR filtering.  The bind is the player's bind_texture() with a pitch.
// A square texture drawn as a quad of `box` px centred on (cx, cy).
// additive: ONE / ONE (the glint); otherwise premultiplied OVER.
static void gpu_tex_quad(u32 off, int px, u32 pitch, float cx, float cy,
                         float box, bool additive)
{
    rsxVertexProgram   *vpo = (rsxVertexProgram *)  video_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)video_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_tex_fp_off, GCM_LOCATION_RSX);

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
    tex.width    = (u16)px;
    tex.height   = (u16)px;
    tex.depth    = 1;
    tex.location = GCM_LOCATION_RSX;
    tex.pitch    = pitch;
    tex.offset   = off;
    rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
    rsxLoadTexture(context, 0, &tex);
    rsxTextureControl(context, 0, GCM_TRUE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(context, 0, 0,
        GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
        GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(context, 0,
        GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
        GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    if (additive)
        rsxSetBlendFunc(context, GCM_ONE, GCM_ONE, GCM_ONE, GCM_ONE);
    else
        rsxSetBlendFunc(context,
            GCM_ONE, GCM_ONE_MINUS_SRC_ALPHA,
            GCM_ONE, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    const float W = (float)display_width, H = (float)display_height;
    const float x0 = 2.0f * (cx - box * 0.5f) / W - 1.0f;
    const float x1 = 2.0f * (cx + box * 0.5f) / W - 1.0f;
    const float y0 = 1.0f - 2.0f * (cy - box * 0.5f) / H;
    const float y1 = 1.0f - 2.0f * (cy + box * 0.5f) / H;

    // TEX0 before POS: the POS write latches the vertex.
    const float uv_tl[4] = { 0.f, 0.f, 0.f, 1.f }, p_tl[4] = { x0, y0, 0.f, 1.f };
    const float uv_tr[4] = { 1.f, 0.f, 0.f, 1.f }, p_tr[4] = { x1, y0, 0.f, 1.f };
    const float uv_bl[4] = { 0.f, 1.f, 0.f, 1.f }, p_bl[4] = { x0, y1, 0.f, 1.f };
    const float uv_br[4] = { 1.f, 1.f, 0.f, 1.f }, p_br[4] = { x1, y1, 0.f, 1.f };
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

    restore_blend();
}

static void gpu_mark(float cx, float cy, float bell)
{
    if (!s_gpu || bell <= 0.0f) return;
    const float box = bell * (float)MARK_SPAN_U / (float)MARK_BELL_U;
    // Smallest level at least as large as the box (or the largest).
    int L = 0;
    for (int i = LEVELS - 1; i >= 0; i--)
        if ((float)s_lv[i].px >= box) { L = i; break; }
    gpu_tex_quad(s_lv[L].off, s_lv[L].px, s_lv[L].pitch, cx, cy, box, false);
}

static void gpu_glint(float cx, float cy, float bell, float pos, float strength)
{
    if (!s_gpu || !s_glint_tex || strength <= 0.0f || bell <= 0.0f) return;
    fill_glint(pos, strength);
    gpu_tex_quad(s_glint_off, GLINT_PX, GLINT_PITCH, cx, cy,
                 bell * (float)MARK_SPAN_U / (float)MARK_BELL_U, true);
}

// One four-point star as a triangle fan: a bright centre, four long arms
// and four short waists between them, alpha 0 at every rim point -- so the
// rasteriser draws the falloff.  Added (SRC_ALPHA, ONE), so it brightens
// whatever it crosses, the mark included, the way a real glint does.
static void gpu_star(float x, float y, float arm, float waist, float rot, u8 a)
{
    if (!s_col_fp || arm <= 0.0f || !a) return;
    rsxVertexProgram   *vpo = (rsxVertexProgram *)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)wave_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_col_fp_off, GCM_LOCATION_RSX);
    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendFunc(context, GCM_SRC_ALPHA, GCM_ONE, GCM_SRC_ALPHA, GCM_ONE);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    const float W = (float)display_width, H = (float)display_height;
    #define SV(px, py, pa) do {                                        \
        const u8 c4[4] = { 255, 248, 255, (pa) };                      \
        const float p4[4] = { 2.0f * (px) / W - 1.0f,                  \
                               1.0f - 2.0f * (py) / H, 0.0f, 1.0f };   \
        rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, c4);       \
        rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    p4);       \
    } while (0)
    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_FAN);
    SV(x, y, a);
    for (int k = 0; k <= 8; k++) {
        const float ang = rot + (float)k * 0.78539816f;     // 45 degrees
        const float r   = (k & 1) ? waist : arm;
        SV(x + r * cosf(ang), y + r * sinf(ang), 0);
    }
    rsxDrawVertexEnd(context);
    #undef SV
    restore_blend();
}

// The SHINE's twinkle on the apex: a soft bloom, a long star, and a shorter
// one turned 45 degrees inside it.  Grows a little as it brightens.
static void gpu_sparkle(float x, float y, float bell, float spark, float rot)
{
    if (spark <= 0.0f || bell <= 0.0f) return;
    const float size = bell * SPARK_SIZE * (0.55f + 0.45f * spark);
    wave_draw_glow_gpu((int)x, (int)y, (int)(size * 0.45f), 255, 248, 255,
                       (u8)(spark * 0.40f * 255.0f + 0.5f));
    gpu_star(x, y, size, size * 0.11f, rot, (u8)(spark * 255.0f + 0.5f));
    gpu_star(x, y, size * 0.5f, size * 0.08f, rot + 0.78539816f,
             (u8)(spark * 0.7f * 255.0f + 0.5f));
}

// -------------------------------------------------------------------------
// CPU draws (after the frame's fence)
// -------------------------------------------------------------------------

// The mark from the premultiplied master, area-sampled to `box`.  over_black
// writes without reading -- exact when the pixels underneath are black, and
// ~100x cheaper than a blend because RSX memory reads are that slow.
static void cpu_mark(float cx, float cy, float bell, float opacity, bool over_black,
                     float gpos, float gstr)
{
    if (!s_master || bell <= 0.0f || opacity <= 0.0f) return;
    const int box = (int)(bell * (float)MARK_SPAN_U / (float)MARK_BELL_U + 0.5f);
    if (box <= 0) return;
    const int x0 = (int)(cx - box * 0.5f + 0.5f), y0 = (int)(cy - box * 0.5f + 0.5f);
    const u32 k = opacity >= 1.0f ? 256u : (u32)(opacity * 256.0f);
    const int sp = s_master_px;
    const u32 tw = cpu_draw_w();

    for (int oy = 0; oy < box; oy++) {
        const int sy = y0 + oy;
        if (sy < 0 || cpu_row_clipped(sy)) continue;
        u32 *row = cpu_draw_row((u32)sy);
        int my0 = oy * sp / box, my1 = (oy + 1) * sp / box;
        if (my1 <= my0) my1 = my0 + 1;
        for (int ox = 0; ox < box; ox++) {
            const int sx = x0 + ox;
            if (sx < 0 || (u32)sx >= tw) continue;
            int mx0 = ox * sp / box, mx1 = (ox + 1) * sp / box;
            if (mx1 <= mx0) mx1 = mx0 + 1;
            u32 a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int my = my0; my < my1; my++) {
                const u32 *src = s_master + my * sp;
                for (int mx = mx0; mx < mx1; mx++) {
                    u32 p = src[mx];
                    a += p >> 24; r += (p >> 16) & 0xFF;
                    g += (p >> 8) & 0xFF; b += p & 0xFF; n++;
                }
            }
            a = a / n * k >> 8; r = r / n * k >> 8;
            g = g / n * k >> 8; b = b / n * k >> 8;
            if (gstr > 0.0f && a) {
                // The SHINE's glint, same profile as the GPU layer: added,
                // scaled by this pixel's coverage.
                const float gk = boot_glint_at(((float)ox + 0.5f) / (float)box,
                                               ((float)oy + 0.5f) / (float)box, gpos)
                               * gstr * BOOT_GLINT_GAIN * (float)a;
                r += (u32)(gk); g += (u32)(gk * 0.96f); b += (u32)(gk * 0.98f);
            }
            if (over_black) {
                row[sx] = ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) |
                           (b > 255 ? 255 : b);
                continue;
            }
            if (a == 0) continue;
            const u32 d = row[sx], ia = a >= 255 ? 0 : 255 - a;
            u32 ro = r + ((d >> 16) & 0xFF) * ia / 255;
            u32 go = g + ((d >>  8) & 0xFF) * ia / 255;
            u32 bo = b + ( d        & 0xFF) * ia / 255;
            row[sx] = ((ro > 255 ? 255 : ro) << 16) | ((go > 255 ? 255 : go) << 8) |
                       (bo > 255 ? 255 : bo);
        }
    }
}

// "Connecting to server", under the mark, at its own opacity -- through the
// posed-wordmark path, which is the one CPU text call that takes an alpha.
static void cpu_status(float cx, float mark_bottom, float alpha)
{
    if (alpha <= 0.0f) return;
    static float ones[32], al[32];
    const int n = (int)sizeof(STATUS_TEXT) - 1;
    for (int i = 0; i < n && i < 32; i++) { ones[i] = 1.0f; al[i] = alpha; }
    const float px = UIS_TF(15.0f);
    const u32 col = XMB_TEXT_FAINT;
    const int w = ttf_text_width_face(STATUS_TEXT, px, UI_FACE_REGULAR);
    const int x = (int)cx - w / 2;
    const int y = (int)(mark_bottom + STATUS_GAP_H * (float)display_height);
    if (x < 0 || y < 0) return;
    drawTTF_ramp_posed((u32)x, (u32)y, STATUS_TEXT, px, &col, 1,
                       UI_FACE_REGULAR, 0.0f, ones, al, n < 32 ? n : 32);
}

// -------------------------------------------------------------------------
// The frame
// -------------------------------------------------------------------------

static void step(void)
{
    const u64 now = timing_get_us();
    const float dt = (float)(now - s_last_us) / 1000.0f;
    s_last_us = now;
    boot_seq_step(&s_seq, dt);
    boot_seq_frame(&s_seq, &s_frame);
}

// Place the mark for this frame and decide who draws it; build the pose.
static void layout(void)
{
    const BootFrame &f = s_frame;
    const float W = (float)display_width, H = (float)display_height;
    const float bell0 = CENTRE_BELL_H * H;
    const float cx0 = 0.5f * W, cy0 = CENTRE_Y_H * H;

    memset(&s_pose, 0, sizeof(s_pose));
    s_pose_live = false;
    s_mark_overlay = false;

    if (f.done) return;

    if (!f.xmb_visible) {
        // Centred, possibly settling or fading.  The lockup is empty.
        s_mcx = cx0; s_mcy = cy0; s_mbell = bell0 * f.mark_scale;
        s_mark_overlay = f.mark_opacity > 0.0f;
        s_pose_live = true;               // show_mark = show_word = false
        return;
    }

    XmbLockupGeom lk;
    xmb_lockup_geom(&lk);
    const float bell1 = (float)lk.mark_px;
    const float cx1 = (float)lk.mark_x + 0.5f * bell1;
    const float cy1 = (float)lk.mark_y + 0.5f * bell1;
    s_mbell = boot_dock_size_px(bell0, bell1, f.dock_size);
    boot_dock_point(cx0, cy0, cx1, cy1, f.dock_pos, &s_mcx, &s_mcy);

    s_pose_live = true;
    if (f.dock_pos >= 1.0f && f.dock_size >= 1.0f) {
        s_pose.show_mark = true;          // landed: the static mark itself
    } else if (f.veil >= 1.0f && s_mbell <= CPU_ZONE * bell1) {
        // Small, and nothing drawn over it any more: the lockup's own code
        // draws it, so the landing is the static path by construction.
        s_pose.show_mark  = true;
        s_pose.mark_posed = true;
        s_pose.mark_bell  = (int)(s_mbell + 0.5f);
        s_pose.mark_x     = (int)(s_mcx - 0.5f * (float)s_pose.mark_bell + 0.5f);
        s_pose.mark_y     = (int)(s_mcy - 0.5f * (float)s_pose.mark_bell + 0.5f);
    } else {
        s_mark_overlay = true;
    }

    bool any = false, all_home = true;
    for (int i = 0; i < BOOT_WORD_LETTERS; i++) {
        s_pose.word_reveal[i] = f.word_reveal[i];
        s_pose.word_alpha[i]  = f.word_alpha[i];
        if (f.word_alpha[i] > 0.0f) any = true;
        if (f.word_reveal[i] != 1.0f || f.word_alpha[i] != 1.0f) all_home = false;
    }
    s_pose.show_word  = any;
    s_pose.word_posed = !all_home;
    s_pose.word_slide_px = BOOT_WORD_SLIDE_EM * lk.word_px;
}

// Everything the animation draws, over whatever is already in the frame.
// scene_black: a pump frame, where the "scene" is a cleared black screen.
static void draw(bool scene_black)
{
    const BootFrame &f = s_frame;
    const float halo_a = f.halo * BOOT_HALO_PEAK;
    const bool  pre    = !f.xmb_visible;

    // ---- GPU, in FIFO order ----
    if (!scene_black) {
        // The wave's entrance rides the EMERGE: the moment the veil starts to
        // lift, the ribbons begin sweeping in from the left edge (the same
        // Off -> JellyWave animation Square plays), so the XMB rises out of
        // the black with the wave extending across it.
        static bool s_wave_in = false;
        if (!s_wave_in && f.veil > 0.0f) { s_wave_in = true; wave_reveal_start(); }
        float ys[BOOT_VEIL_ROWS_MAX];
        unsigned char as[BOOT_VEIL_ROWS_MAX];
        const int n = boot_veil_rows(f.veil, (float)display_height, ys, as);
        if (n) gpu_rows(ys, as, n);
    }
    if (s_gpu && halo_a > 0.0f && s_mbell > 0.0f) {
        const u32 c = XMB_LK_MARK_A;
        wave_draw_glow_gpu((int)s_mcx, (int)s_mcy, (int)(s_mbell * HALO_RADIUS),
                           (u8)((c >> 16) & 0xFF), (u8)((c >> 8) & 0xFF),
                           (u8)(c & 0xFF), (u8)(halo_a * 255.0f + 0.5f));
    }
    if (s_gpu && s_mark_overlay) {
        gpu_mark(s_mcx, s_mcy, s_mbell);
        gpu_glint(s_mcx, s_mcy, s_mbell, f.glint_pos, f.glint);
    }
    // Fading in or out over black: dim mark and halo together with one
    // full-screen quad.  Only ever before the XMB shows, so the only thing
    // under it is black and dimming it is exact.
    if (s_gpu && pre && (s_mark_overlay || halo_a > 0.0f))
        gpu_fade(f.mark_opacity);

    // ---- CPU, after the fence ----
    const bool cpu_mark_needed = !s_gpu && s_mark_overlay;
    if (cpu_mark_needed || f.status > 0.0f) {
        rsxSync();
        if (cpu_mark_needed)
            cpu_mark(s_mcx, s_mcy, s_mbell, pre ? f.mark_opacity : 1.0f, pre,
                     f.glint_pos, f.glint);
        const float box = s_mbell * (float)MARK_SPAN_U / (float)MARK_BELL_U;
        cpu_status(s_mcx, s_mcy + 0.5f * box, f.status);
    }

    // The twinkle goes last, so it lies over the mark on both paths (on the
    // CPU path the mark was just written after the fence; these GPU commands
    // run after it).
    if (f.spark > 0.0f)
        gpu_sparkle(s_mcx, s_mcy + SPARK_APEX_Y * s_mbell, s_mbell,
                    f.spark, f.spark_rot);
}

static void finish_if_done(void)
{
    if (s_on && s_frame.done) {
        s_on = false;
        char b[96];
        snprintf(b, sizeof b, "boot: done after %u frames, %.0f ms%s",
                 s_seq.frames, (double)s_seq.t, s_seq.left ? " (dismissed)" : "");
        plog(b);
        crash_log("boot: done");
    }
}

// -------------------------------------------------------------------------
// Public
// -------------------------------------------------------------------------

bool boot_anim_begin(void)
{
    if (s_on) return true;
    if (ui_cpu_bg()) {
        plog("boot: animation off on the emulator build (CPU background)");
        return false;
    }
    if (!gate_on()) {
        plog("boot: animation off (jellyfin_bootanim.txt = 0) -- old splash");
        crash_log("boot: OFF (gate)");
        return false;
    }

    // Black, now: nothing else is ready to be seen and nothing half-built
    // may be.  The decode below then happens inside DARK.
    crash_log("boot: begin");
    clearScreen(0x000000);
    flip();
    s_flipped = true;
    s_last_us = timing_get_us();

    if (!load_mark()) {
        plog("boot: mark decode FAILED -- old splash");
        crash_log("boot: mark decode FAILED");
        release();
        return false;
    }
    s_col_fp = upload_fp(wave_fp_data, &s_col_fp_off);

    // The textured mark only where the user has already proven RSX textures
    // on this console; see the file header.
    if (ui_card_gpu_ready() || ui_text_gpu_ready()) {
        s_tex_fp = upload_fp(video_fp_data, &s_tex_fp_off);
        if (s_tex_fp && s_col_fp && upload_levels()) {
            s_gpu = true;
            make_glint();                 // from the master, before it goes
            free(s_master);               // the CPU copy is not needed
            s_master = NULL;
        } else {
            for (int i = 0; i < LEVELS; i++)
                if (s_lv[i].mem) { rsxFree(s_lv[i].mem); s_lv[i].mem = NULL; }
            plog("boot: VRAM for the mark unavailable -- CPU mark");
        }
    }

    boot_seq_begin(&s_seq);
    boot_seq_frame(&s_seq, &s_frame);
    layout();
    s_on = true;

    char b[128];
    snprintf(b, sizeof b, "boot: animation ON, %s mark (%s), %ux%u",
             s_gpu ? "RSX" : "CPU", xmb_mark_variant() ? "gold" : "cool",
             display_width, display_height);
    plog(b);
    crash_log(s_gpu ? "boot: ON (RSX mark)" : "boot: ON (CPU mark)");
    return true;
}

bool boot_anim_active(void) { return s_on; }

bool boot_anim_pump(void)
{
    if (!s_on) return false;
    if (s_flipped) waitflip();
    // Only once main() has set running = 1.  Before that, an exit request
    // consumed here would be overwritten by that assignment and lost; left
    // queued, the first check after it sees it, as it always did.
    if (running) sysUtilCheckCallback();
    step();
    layout();
    clearScreen(0x000000);
    draw(true);
    flip();
    s_flipped = true;
    finish_if_done();
    return true;
}

static void (*volatile s_work_fn)(void) = NULL;
static volatile bool s_work_done = false;

static void work_thread(void *arg)
{
    (void)arg;
    s_work_fn();
    __sync_synchronize();          // the work's writes land before the flag
    s_work_done = true;
    sysThreadExit(0);
}

void boot_anim_run(void (*work)(void), const char *what)
{
    if (!s_on) { work(); return; }

    s_work_fn = work;
    s_work_done = false;
    sys_ppu_thread_t tid;
    static char name[] = "jf_bootprep";    // the API takes char *, not const
    // 128 KB: the work is HTTP + JSON parsing, which the 64 KB search and
    // update-check workers already do; the headroom is for safety only.
    s32 rc = sysThreadCreate(&tid, work_thread, NULL, 1500, 128 * 1024,
                             THREAD_JOINABLE, name);
    if (rc != 0) {
        char b[96];
        snprintf(b, sizeof b, "boot: worker for %s FAILED 0x%08x -- inline",
                 what, (u32)rc);
        plog(b);
        work();
        return;
    }
    const u64 t0 = timing_get_us();
    while (!s_work_done) {
        if (!boot_anim_pump()) usleep(16000);
    }
    __sync_synchronize();
    u64 ret;
    sysThreadJoin(tid, &ret);
    char b[96];
    snprintf(b, sizeof b, "boot: %s took %llu ms behind the animation", what,
             (unsigned long long)((timing_get_us() - t0) / 1000));
    plog(b);
}

void boot_anim_leave(void)
{
    if (!s_on) return;
    boot_seq_signal(&s_seq, BOOT_SIG_LEAVE);
    while (s_on) boot_anim_pump();
    rsxSync();
    release();
}

void boot_anim_finish(void)
{
    if (s_on) {
        plog("boot: finished early (the XMB returned first)");
        s_on = false;
    }
    s_pose_live = false;
    if (s_master || s_tex_fp || s_col_fp || s_lv[0].mem || s_glint_tex) {
        rsxSync();
        release();
    }
}

bool boot_anim_xmb_frame(void)
{
    if (!s_on) {
        // The frame after DONE: this frame's fence has not run yet but every
        // earlier frame's has, and nothing this frame will sample the mark.
        s_pose_live = false;
        if (s_master || s_tex_fp || s_col_fp || s_lv[0].mem || s_glint_tex)
            release();
        return false;
    }
    boot_seq_signal(&s_seq, BOOT_SIG_XMB);

    // Any new press skips to the end -- and is swallowed, so a press made to
    // skip the animation does not ALSO open whatever card is focused.
    const u8 *cur  = (const u8 *)&btn_cur;
    const u8 *prev = (const u8 *)&btn_prev;
    bool pressed = false;
    for (size_t i = 0; i < sizeof(ButtonState); i++)
        if (cur[i] && !prev[i]) { pressed = true; break; }
    if (pressed && boot_seq_skip_effective(&s_seq)) {
        boot_seq_signal(&s_seq, BOOT_SIG_SKIP);
        plog("boot: skipped by a button press");
    }

    step();
    layout();
    finish_if_done();
    return true;          // the boot owned this frame's input either way
}

void boot_anim_xmb_overlay(void)
{
    if (!s_on) return;
    draw(false);
}

const BootLockupPose *boot_anim_lockup_pose(void)
{
    return (s_on && s_pose_live) ? &s_pose : NULL;
}
