// Video GPU blit state, RSX buffer init/free, and per-vblank draw call.

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "plog.h"
#include "rsxutil.h"
#include "video_shaders.h"
#include "player_rsx.h"
#include "video.h"      // vid_frame_bytes()

// -------------------------------------------------------
// Video GPU blit state (allocated per playback session)
// -------------------------------------------------------

// Planar YUV plane textures, double-buffered: upload thread fills [disp_idx^1],
// display thread binds [disp_idx].  Three SEPARATE textures per frame
// (Movian-style — sub-regions of one buffer weren't honoured by RPCS3, so all
// planes read luma).  Indexed [dbuf 0/1][plane 0=Y 1=Cb 2=Cr]; A = current
// frame, B = next frame for the temporal blend.
volatile u8 *s_yuvA_buf[2][3] = {};  u32 s_yuvA_off[2][3] = {};
volatile u8 *s_yuvB_buf[2][3] = {};  u32 s_yuvB_off[2][3] = {};
u32 *s_vid_fp_buf        = NULL;  // RGB passthrough FP (HUD overlay pass)
u32  s_vid_fp_off        = 0;
// YUV->RGB fragment programs for the video quad: BT.709 (HD) and BT.601 (SD),
// selected per-frame by resolution at draw time.
u32 *s_vid_fp_buf_yuv    = NULL;
u32  s_vid_fp_off_yuv    = 0;
u32 *s_vid_fp_buf_yuv601 = NULL;
u32  s_vid_fp_off_yuv601 = 0;
u8  *s_vid_vbuf          = NULL;  // RSX-local 4-vertex quad buffer
u32  s_vid_vbuf_off      = 0;
volatile int  s_vid_disp_idx    = 0;
volatile bool s_vid_frame_ready = false;
volatile bool s_vid_b_present   = false;

// Frame size the letterbox quad was last built for.  vid_gpu_init builds it
// from the *allocated* jbuf size, but the server's transcode preserves the
// source aspect (a 2.40:1 movie comes back 1280×534, not 1280×720) and the
// real size is only known once the first frame decodes (jbuf_set_dims).
// vid_gpu_draw compares against this and rebuilds, otherwise the frame is
// stretched onto the stale full-screen quad.
static u32 s_vid_quad_fw = 0, s_vid_quad_fh = 0;

// -------------------------------------------------------
// vid_gpu_build_quad — aspect-correct letterbox fit of a fw×fh frame
// -------------------------------------------------------
// The fit works in *visual* units: framebuffer pixels aren't square on SD
// modes (720×576 / 720×480), so scale by the display pixel aspect ratio —
// otherwise 576i letterboxes to the wrong bar height and circles go oval.

static void vid_gpu_build_quad(u32 fw, u32 fh) {
    if (!s_vid_vbuf || fw == 0 || fh == 0) return;

    // Content visual aspect vs screen visual aspect, cross-multiplied to
    // stay in integers: content wider → pillar-fit to width, else to height.
    u32 sw, sh;
    if ((u64)fw * display_par_den * display_height >
        (u64)fh * display_par_num * display_width) {
        sw = display_width;
        sh = (u32)((u64)fh * display_par_num * display_width /
                   ((u64)fw * display_par_den));
        if (sh > display_height) sh = display_height;
    } else {
        sh = display_height;
        sw = (u32)((u64)fw * display_par_den * display_height /
                   ((u64)fh * display_par_num));
        if (sw > display_width) sw = display_width;
    }
    u32 ox0v = (display_width  - sw) / 2;
    u32 oy0v = (display_height - sh) / 2;
    float cx0 = (float)ox0v / display_width  * 2.0f - 1.0f;
    float cx1 = (float)(ox0v + sw) / display_width  * 2.0f - 1.0f;
    float cy0 = 1.0f - (float)oy0v / display_height * 2.0f;
    float cy1 = 1.0f - (float)(oy0v + sh) / display_height * 2.0f;

    float *v = (float*)s_vid_vbuf;
    v[0]=cx0; v[1]=cy0; v[2]=0.f; v[3]=1.f; v[4]=0.f; v[5]=0.f;      // TL
    v[6]=cx1; v[7]=cy0; v[8]=0.f; v[9]=1.f; v[10]=1.f; v[11]=0.f;    // TR
    v[12]=cx0; v[13]=cy1; v[14]=0.f; v[15]=1.f; v[16]=0.f; v[17]=1.f; // BL
    v[18]=cx1; v[19]=cy1; v[20]=0.f; v[21]=1.f; v[22]=1.f; v[23]=1.f; // BR

    s_vid_quad_fw = fw;
    s_vid_quad_fh = fh;

    char buf[96];
    snprintf(buf, sizeof(buf), "vid_gpu: quad %ux%u -> %ux%u+%u+%u",
             fw, fh, sw, sh, ox0v, oy0v);
    plog(buf);
}

// -------------------------------------------------------
// vid_gpu_init — allocate RSX buffers, build vertex quad, load FP ucode
// -------------------------------------------------------

void vid_gpu_init(u32 fw, u32 fh) {
    // Planar: allocate Y (fw×fh) + Cb/Cr (½×½) as three independent RSX
    // textures, double-buffered, for frame A and B.  Height padded to a
    // macroblock to match the decoder's coded size.
    {
        u32 fh_pad = (fh + 15u) & ~15u;
        u32 ysz = fw * fh_pad;
        u32 csz = (fw / 2u) * (fh_pad / 2u);
        for (int db = 0; db < 2; db++) {
            u32 sz[3] = { ysz, csz, csz };
            for (int p = 0; p < 3; p++) {
                s_yuvA_buf[db][p] = (u8*)rsxMemalign(64, sz[p]);
                rsxAddressToOffset(const_cast<u8*>(s_yuvA_buf[db][p]), &s_yuvA_off[db][p]);
                s_yuvB_buf[db][p] = (u8*)rsxMemalign(64, sz[p]);
                rsxAddressToOffset(const_cast<u8*>(s_yuvB_buf[db][p]), &s_yuvB_off[db][p]);
            }
        }
    }
    s_vid_disp_idx    = 0;
    s_vid_frame_ready = false;
    s_vid_b_present   = false;

    rsxFragmentProgram *vid_fpo = (rsxFragmentProgram*)video_fp_data;
    void *fp_ucode; u32 fp_size;
    rsxFragmentProgramGetUCode(vid_fpo, &fp_ucode, &fp_size);
    s_vid_fp_buf = (u32*)rsxMemalign(256, fp_size);
    memcpy(s_vid_fp_buf, fp_ucode, fp_size);
    rsxAddressToOffset(s_vid_fp_buf, &s_vid_fp_off);

    // Upload both YUV->RGB programs for the video quad (the HUD overlay still
    // uses the RGB passthrough): BT.709 for HD frames, BT.601 for SD.
    {
        rsxFragmentProgram *yuv_fpo = (rsxFragmentProgram*)video_fp_yuv_data;
        void *yfp_ucode; u32 yfp_size;
        rsxFragmentProgramGetUCode(yuv_fpo, &yfp_ucode, &yfp_size);
        s_vid_fp_buf_yuv = (u32*)rsxMemalign(256, yfp_size);
        memcpy(s_vid_fp_buf_yuv, yfp_ucode, yfp_size);
        rsxAddressToOffset(s_vid_fp_buf_yuv, &s_vid_fp_off_yuv);

        rsxFragmentProgram *y601_fpo = (rsxFragmentProgram*)video_fp_yuv601_data;
        rsxFragmentProgramGetUCode(y601_fpo, &yfp_ucode, &yfp_size);
        s_vid_fp_buf_yuv601 = (u32*)rsxMemalign(256, yfp_size);
        memcpy(s_vid_fp_buf_yuv601, yfp_ucode, yfp_size);
        rsxAddressToOffset(s_vid_fp_buf_yuv601, &s_vid_fp_off_yuv601);
    }

    // 4 vertices: float4 pos + float2 uv = 24 bytes each
    s_vid_vbuf = (u8*)rsxMemalign(128, 4 * 24);
    rsxAddressToOffset(s_vid_vbuf, &s_vid_vbuf_off);

    // fw/fh here are the *allocated* size; vid_gpu_draw rebuilds the quad
    // once the decoder reports the stream's real dimensions.
    vid_gpu_build_quad(fw, fh);

    plog("vid_gpu: init done");
}

// -------------------------------------------------------
// vid_gpu_free — release all six RSX-local allocations
// -------------------------------------------------------

void vid_gpu_free(void) {
    for (int db = 0; db < 2; db++) {
        for (int p = 0; p < 3; p++) {
            if (s_yuvA_buf[db][p]) { rsxFree(const_cast<u8*>(s_yuvA_buf[db][p])); s_yuvA_buf[db][p] = NULL; }
            if (s_yuvB_buf[db][p]) { rsxFree(const_cast<u8*>(s_yuvB_buf[db][p])); s_yuvB_buf[db][p] = NULL; }
        }
    }
    if (s_vid_fp_buf)        { rsxFree(s_vid_fp_buf);        s_vid_fp_buf        = NULL; }
    if (s_vid_fp_buf_yuv)    { rsxFree(s_vid_fp_buf_yuv);    s_vid_fp_buf_yuv    = NULL; }
    if (s_vid_fp_buf_yuv601) { rsxFree(s_vid_fp_buf_yuv601); s_vid_fp_buf_yuv601 = NULL; }
    if (s_vid_vbuf)          { rsxFree(s_vid_vbuf);          s_vid_vbuf          = NULL; }
}

// -------------------------------------------------------
// vid_gpu_draw — thin wrapper; RSX logic lives in player_rsx.cpp
// -------------------------------------------------------

void vid_gpu_draw(bool render_blend, float blend_factor, u32 fw, u32 fh) {
    // The decoder corrects the jbuf dimensions on the first decoded frame
    // (and that happens during pre-fill, before the first draw reads the
    // quad) — refit the letterbox instead of stretching into the old one.
    if (fw != s_vid_quad_fw || fh != s_vid_quad_fh)
        vid_gpu_build_quad(fw, fh);
    rsx_draw_frame(render_blend, blend_factor, fw, fh);
}
