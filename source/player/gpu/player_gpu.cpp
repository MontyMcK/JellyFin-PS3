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

// -------------------------------------------------------
// Video GPU blit state (allocated per playback session)
// -------------------------------------------------------

// Double-buffered textures: upload thread writes to tex_buf[disp_idx^1],
// display thread binds tex_buf[disp_idx] for RSX draw.
volatile u32 *s_vid_tex_buf[2]    = {NULL, NULL};
u32           s_vid_tex_off[2]    = {0, 0};
// Second pair for frame B (temporal blend — next jbuf slot)
volatile u32 *s_vid_tex_buf_b[2]  = {NULL, NULL};
u32  s_vid_tex_off_b[2]  = {0, 0};
u32 *s_vid_fp_buf        = NULL;  // RSX-local FP ucode copy
u32  s_vid_fp_off        = 0;
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
    s_vid_tex_buf[0] = (u32*)rsxMemalign(64, fw * fh * 4);
    rsxAddressToOffset(const_cast<u32*>(s_vid_tex_buf[0]), &s_vid_tex_off[0]);
    s_vid_tex_buf[1] = (u32*)rsxMemalign(64, fw * fh * 4);
    rsxAddressToOffset(const_cast<u32*>(s_vid_tex_buf[1]), &s_vid_tex_off[1]);
    s_vid_tex_buf_b[0] = (u32*)rsxMemalign(64, fw * fh * 4);
    rsxAddressToOffset(const_cast<u32*>(s_vid_tex_buf_b[0]), &s_vid_tex_off_b[0]);
    s_vid_tex_buf_b[1] = (u32*)rsxMemalign(64, fw * fh * 4);
    rsxAddressToOffset(const_cast<u32*>(s_vid_tex_buf_b[1]), &s_vid_tex_off_b[1]);
    s_vid_disp_idx    = 0;
    s_vid_frame_ready = false;
    s_vid_b_present   = false;

    rsxFragmentProgram *vid_fpo = (rsxFragmentProgram*)video_fp_data;
    void *fp_ucode; u32 fp_size;
    rsxFragmentProgramGetUCode(vid_fpo, &fp_ucode, &fp_size);
    s_vid_fp_buf = (u32*)rsxMemalign(256, fp_size);
    memcpy(s_vid_fp_buf, fp_ucode, fp_size);
    rsxAddressToOffset(s_vid_fp_buf, &s_vid_fp_off);

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
    if (s_vid_tex_buf[0])   { rsxFree(const_cast<u32*>(s_vid_tex_buf[0]));   s_vid_tex_buf[0]   = NULL; }
    if (s_vid_tex_buf[1])   { rsxFree(const_cast<u32*>(s_vid_tex_buf[1]));   s_vid_tex_buf[1]   = NULL; }
    if (s_vid_tex_buf_b[0]) { rsxFree(const_cast<u32*>(s_vid_tex_buf_b[0])); s_vid_tex_buf_b[0] = NULL; }
    if (s_vid_tex_buf_b[1]) { rsxFree(const_cast<u32*>(s_vid_tex_buf_b[1])); s_vid_tex_buf_b[1] = NULL; }
    if (s_vid_fp_buf)       { rsxFree(s_vid_fp_buf);       s_vid_fp_buf       = NULL; }
    if (s_vid_vbuf)         { rsxFree(s_vid_vbuf);         s_vid_vbuf         = NULL; }
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
