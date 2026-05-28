// Video GPU blit state, RSX buffer init/free, and per-vblank draw call.

#include <string.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "plog.h"
#include "rsxutil.h"
#include "video_shaders.h"

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
// vid_gpu_draw — per-vblank RSX texture bind and draw
// -------------------------------------------------------

void vid_gpu_draw(bool render_blend, float blend_factor, u32 fw, u32 fh) {
    // Clear framebuffer to black (covers bars outside the quad)
    rsxSetClearColor(context, 0x00000000);
    rsxSetClearDepthStencil(context, 0xffff);
    rsxClearSurface(context,
        GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

    // Common render state
    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetColorMask(context,
        GCM_COLOR_MASK_R | GCM_COLOR_MASK_G |
        GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    {
        float vp_sc[4]  = { display_width  *  0.5f,
                           -(float)display_height * 0.5f, 0.5f, 0.0f };
        float vp_off[4] = { display_width  *  0.5f,
                            (float)display_height * 0.5f, 0.5f, 0.0f };
        rsxSetViewport(context, 0, 0,
            (u16)display_width, (u16)display_height,
            0.0f, 1.0f, vp_sc, vp_off);
    }

    // Load shaders
    {
        rsxVertexProgram   *vid_vpo = (rsxVertexProgram*)  video_vp_data;
        rsxFragmentProgram *vid_fpo = (rsxFragmentProgram*) video_fp_data;
        void *vp_ucode; u32 vp_size;
        rsxVertexProgramGetUCode(vid_vpo, &vp_ucode, &vp_size);
        rsxLoadVertexProgram(context, vid_vpo, vp_ucode);
        rsxSetVertexAttribOutputMask(context, vid_vpo->output_mask);
        rsxLoadFragmentProgramLocation(context, vid_fpo,
            s_vid_fp_off, GCM_LOCATION_RSX);
    }

    // Vertex buffer binding (shared by both passes)
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
        s_vid_vbuf_off, 24, 4,
        GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
        s_vid_vbuf_off + 16, 24, 2,
        GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

    if (render_blend) {
        // Pass 1: frame A — no blend
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
            tex.width     = (u16)fw;
            tex.height    = (u16)fh;
            tex.depth     = 1;
            tex.location  = GCM_LOCATION_RSX;
            tex.pitch     = fw * 4;
            tex.offset    = s_vid_tex_off[s_vid_disp_idx];
            rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
            rsxLoadTexture(context, 0, &tex);
            rsxTextureControl(context, 0, GCM_TRUE, 0, 12 << 8,
                GCM_TEXTURE_MAX_ANISO_1);
            rsxTextureFilter(context, 0, 0,
                GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
                GCM_TEXTURE_CONVOLUTION_QUINCUNX);
            rsxTextureWrapMode(context, 0,
                GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
        }
        rsxSetBlendEnable(context, GCM_FALSE);
        rsxInvalidateVertexCache(context);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);

        // Pass 2: frame B — constant-alpha blend over frame A
        // Result = A*blend_factor + B*(1-blend_factor)
        //   alpha_const = 1-blend_factor (weight of B in the dst equation)
        {
            u8  a   = (u8)((1.0f - blend_factor) * 255.0f);
            u32 col = ((u32)a << 24) | ((u32)a << 16) | ((u32)a << 8) | (u32)a;
            rsxSetBlendColor(context, col, col);
            rsxSetBlendFunc(context,
                GCM_CONSTANT_ALPHA, GCM_ONE_MINUS_CONSTANT_ALPHA,
                GCM_CONSTANT_ALPHA, GCM_ONE_MINUS_CONSTANT_ALPHA);
            rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
            rsxSetBlendEnable(context, GCM_TRUE);
        }
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
            tex.width     = (u16)fw;
            tex.height    = (u16)fh;
            tex.depth     = 1;
            tex.location  = GCM_LOCATION_RSX;
            tex.pitch     = fw * 4;
            tex.offset    = s_vid_tex_off_b[s_vid_disp_idx];
            rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
            rsxLoadTexture(context, 0, &tex);
            rsxTextureControl(context, 0, GCM_TRUE, 0, 12 << 8,
                GCM_TEXTURE_MAX_ANISO_1);
            rsxTextureFilter(context, 0, 0,
                GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
                GCM_TEXTURE_CONVOLUTION_QUINCUNX);
            rsxTextureWrapMode(context, 0,
                GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
        }
        rsxInvalidateVertexCache(context);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
        rsxSetBlendEnable(context, GCM_FALSE);
    } else {
        // Single pass: frame A only
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
            tex.width     = (u16)fw;
            tex.height    = (u16)fh;
            tex.depth     = 1;
            tex.location  = GCM_LOCATION_RSX;
            tex.pitch     = fw * 4;
            tex.offset    = s_vid_tex_off[s_vid_disp_idx];
            rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
            rsxLoadTexture(context, 0, &tex);
            rsxTextureControl(context, 0, GCM_TRUE, 0, 12 << 8,
                GCM_TEXTURE_MAX_ANISO_1);
            rsxTextureFilter(context, 0, 0,
                GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
                GCM_TEXTURE_CONVOLUTION_QUINCUNX);
            rsxTextureWrapMode(context, 0,
                GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
        }
        rsxSetBlendEnable(context, GCM_FALSE);
        rsxInvalidateVertexCache(context);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
    }
}
