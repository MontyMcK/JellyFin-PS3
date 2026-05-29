// RSX viewport, shader, texture, and draw-call logic for video playback.

#include <string.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "rsxutil.h"
#include "video_shaders.h"
#include "player_rsx.h"

// State owned by player_gpu.cpp
extern u32           s_vid_fp_off;
extern u32           s_vid_vbuf_off;
extern u32           s_vid_tex_off[2];
extern u32           s_vid_tex_off_b[2];
extern volatile int  s_vid_disp_idx;

static void bind_texture(u32 tex_off, u32 fw, u32 fh) {
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
    tex.offset    = tex_off;
    rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
    rsxLoadTexture(context, 0, &tex);
    rsxTextureControl(context, 0, GCM_TRUE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(context, 0, 0,
        GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR,
        GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(context, 0,
        GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
        GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

void rsx_draw_frame(bool render_blend, float blend_factor, u32 fw, u32 fh) {
    // Clear framebuffer to black (covers bars outside the quad)
    rsxSetClearColor(context, 0x00000000);
    rsxSetClearDepthStencil(context, 0xffff);
    rsxClearSurface(context,
        GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);

    // Render state
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
        bind_texture(s_vid_tex_off[s_vid_disp_idx], fw, fh);
        rsxSetBlendEnable(context, GCM_FALSE);
        rsxInvalidateVertexCache(context);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);

        // Pass 2: frame B — constant-alpha blend over frame A
        // Result = A*blend_factor + B*(1-blend_factor)
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
        bind_texture(s_vid_tex_off_b[s_vid_disp_idx], fw, fh);
        rsxInvalidateVertexCache(context);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
        rsxSetBlendEnable(context, GCM_FALSE);
    } else {
        // Single pass: frame A only
        bind_texture(s_vid_tex_off[s_vid_disp_idx], fw, fh);
        rsxSetBlendEnable(context, GCM_FALSE);
        rsxInvalidateVertexCache(context);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
    }
}
