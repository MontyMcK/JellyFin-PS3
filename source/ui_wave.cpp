#include <math.h>
#include <string.h>
#include <rsx/rsx.h>
#include "rsxutil.h"
#include "wave_shaders.h"
#include "ui_wave.h"

#define WAVE_STEP_PX    20
#define WAVE_VERTS_MAX  200
#define WAVE_VBUF_BYTES 12288  // 3 layers x 200 verts x 20 bytes

static const u32   WAVE_COLOR[3]  = { 0x002d2745, 0x001e1a37, 0x00261f44 };
static const float WAVE_AMP[3]    = { 30.0f, 20.0f, 14.0f };
static const float WAVE_FREQ[3]   = { 1.5f,  1.8f,  2.1f  };
static const float WAVE_BASEY[3]  = { 0.70f, 0.78f, 0.86f };
static const float WAVE_DPHASE[3] = { 0.008f, 0.013f, 0.018f };

typedef struct {
    float x, y, z, w;
    u8 r, g, b, a;
} WaveVert; // 20 bytes

static float  s_wave_phase[3]   = { 0.0f, 0.0f, 0.0f };
static u8    *s_wave_vbuf       = NULL;
static u32   *s_wave_fp_buf     = NULL;
static u32    s_wave_fp_offset  = 0;

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

    // Loop 1: build all three layers into separate sub-regions of the vertex buffer.
    int layer_n[3];
    for (int li = 0; li < 3; li++) {
        float base_y = H * WAVE_BASEY[li];
        float amp    = WAVE_AMP[li];
        float freq   = WAVE_FREQ[li];
        float phase  = s_wave_phase[li];
        u32   col    = WAVE_COLOR[li];
        u8    r = (col >> 16) & 0xFF;
        u8    g = (col >>  8) & 0xFF;
        u8    b =  col        & 0xFF;

        WaveVert *verts = (WaveVert*)(s_wave_vbuf + li * WAVE_VERTS_MAX * sizeof(WaveVert));
        int n = 0;

        for (int px = 0; px <= (int)W; px += WAVE_STEP_PX) {
            float fx = (float)px;
            float wy = base_y + sinf(fx / W * (freq * 3.14159265f) + phase) * amp;
            if (wy < 0.0f) wy = 0.0f;
            if (wy > H)    wy = H;

            float cx     = (2.0f * fx / W) - 1.0f;
            float cy_top = 1.0f - (2.0f * wy / H);
            float cy_bot = -1.0f;

            verts[n].x=cx; verts[n].y=cy_top; verts[n].z=0.0f; verts[n].w=1.0f;
            verts[n].r=r;  verts[n].g=g;      verts[n].b=b;     verts[n].a=255;
            n++;
            verts[n].x=cx; verts[n].y=cy_bot; verts[n].z=0.0f; verts[n].w=1.0f;
            verts[n].r=r;  verts[n].g=g;      verts[n].b=b;     verts[n].a=255;
            n++;
        }
        layer_n[li] = n;
    }

    // Loop 2: issue draw calls back-to-front; all geometry is already in RSX memory.
    u32 vbuf_base;
    rsxAddressToOffset(s_wave_vbuf, &vbuf_base);

    rsxSetBlendEnable(context, GCM_FALSE);

    for (int li = 0; li < 3; li++) {
        u32 vbuf_off = vbuf_base + (u32)(li * WAVE_VERTS_MAX * sizeof(WaveVert));

        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
            vbuf_off,
            (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
            vbuf_off + 16u,
            (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);

        rsxInvalidateVertexCache(context);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, (u32)layer_n[li]);
    }

    rsxSetBlendEnable(context, GCM_TRUE);
}
