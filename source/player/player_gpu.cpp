// Video GPU blit state, RSX buffer init/free, and per-vblank draw call.

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

    // Aspect-ratio fit — same formula as display loop
    u32 sw, sh;
    if ((u64)fw * display_height > (u64)fh * display_width) {
        sw = display_width;
        sh = (u32)((u64)fh * display_width / fw);
    } else {
        sh = display_height;
        sw = (u32)((u64)fw * display_height / fh);
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
    rsxAddressToOffset(s_vid_vbuf, &s_vid_vbuf_off);

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
    rsx_draw_frame(render_blend, blend_factor, fw, fh);
}
