#include "../../build_config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include "rsxutil.h"
#include "wave_shaders.h"
#include "wave_field.h"
#include "ui_wave_audio.h"
#include "ui_wave.h"
#include "ui_visuals.h"
#include "plog.h"
#include "jf_paths.h"

extern void crash_log(const char *msg);

// --- CPU-vs-GPU background compositing -----------------------------------
// On real PS3, the framebuffer lives in RSX-local VRAM and PPU writes to it
// are uncached and very slow (a full-screen CPU fill costs ~150-180ms), so the
// XMB background is rendered with the RSX (wave_draw's immediate-mode path) and
// only small UI elements (text, key cells) are composited by the CPU.
//
// RPCS3 emulates that VRAM as ordinary host RAM, and — crucially — presents the
// GPU-rendered surface from its render-target cache on flip, silently dropping
// any later CPU writes to the same buffer.  The result is a frame where the GPU
// wave shows but every CPU-drawn element is invisible.
//
// The fix is: on the emulator, composite the *entire* frame — background
// included — on the CPU so no GPU op owns the display surface and the flip
// presents exactly what we drew.  On hardware, keep the GPU wave.
//
// This is a COMPILE-TIME switch, not runtime detection.  A startup probe that
// timed one full-screen CPU write misclassified real hardware (PPU write-
// gathering beat the threshold), which put a retail PS3 on the CPU path —
// every frame then read back uncached VRAM and the whole UI crawled.
// The flag lives in build_config.h (BUILD_FOR_RPCS3) — the single switch
// for emulator vs hardware builds.
bool ui_cpu_bg(void) { return BUILD_FOR_RPCS3 != 0; }

#define WAVE_STEP_PX    20
#define WAVE_NS         8      // vertical slices per ribbon (fade resolution)
#define WAVE_MAX_COLS   98     // x columns: supports up to ~1940px wide (720p uses 65)

// Ribbons read as translucent veils over the background gradient: bright at
// the crest, fading to nothing below.  The fade is *baked into opaque vertex
// colours* on the CPU rather than left to GPU alpha blending.  Each ribbon is
// tessellated into WAVE_NS horizontal slices from its crest down to the screen
// bottom; at every grid node the colour is the ribbon tint composited over the
// gradient (and any earlier ribbons) at that exact height, using the same
// maths as tools/ui_preview/preview.c.  Drawn fully opaque, the GPU's plain
// colour interpolation reproduces the veil pixel-for-pixel with blending off.
// WAVE_ALPHA is the crest opacity used for that pre-blend.
//
// SUBMISSION: two paths, see wave_draw().
//
// The original path streams every vertex into the command FIFO with
// rsxDrawVertex4f/4ub (immediate mode).  The comment that used to live here
// claimed vertex-array fetch was "unreliable on real hardware" and that this
// was why.  That claim is FALSE and cost this project a measured 2,129 us per
// frame -- the largest single item left in the Home frame.
// source/player/gpu/player_rsx.cpp draws every video frame from interleaved
// vertex arrays with three bound textures, and source/player/hud/hud_dim.cpp
// draws a COLOR0-carrying quad the same way, both on this console.  What broke
// the early attempt was a STALE BINDING, not the fetch unit.  Two disciplines
// fix it, and the vertex-array path below follows both:
//
//   * rsxInvalidateVertexCache() immediately before every rsxDrawVertexArray.
//   * Reset the attrib bindings after the last draw (COLOR0 re-bound at
//     stride 0, TEX0 left disabled), so nothing downstream inherits an array
//     binding it did not ask for.  hud_dim.cpp does exactly this.
//
// At 1920x1080 the wave is ~4,708 vertices; immediate mode costs two FIFO
// writes each.  The array path writes them once into RSX-local memory (PPU
// writes to VRAM measured 767 MB/s, faster than to main memory) and then
// issues 25 draw calls -- one per triangle strip -- against a single binding.
//
// Gated on /dev_hdd0/tmp/jellyfin_gpuwave.txt = 1 while it is unproven, for
// the same reason as the card and text gates: a bad binding wedges the GPU,
// which takes the console off the network and needs a power cycle.  Delete the
// file to fall back to immediate mode with no reflash.
static const u32   WAVE_COLOR[3]  = { 0x004A52A8, 0x006C5BD4, 0x003A4290 };
static const u8    WAVE_ALPHA[3]  = { 56, 42, 72 };
static const float WAVE_AMP[3]    = { 30.0f, 22.0f, 15.0f };
// Crest baselines as a fraction of screen height — kept low so the ribbons
// sit in the bottom quarter of the screen instead of climbing into the middle.
static const float WAVE_BASEY[3]  = { 0.78f, 0.85f, 0.91f };

// --- where the crest shape comes from ------------------------------------
//
// It used to be one sine per ribbon: a fixed shape sliding sideways, with
// WAVE_FREQ setting its wavelength and WAVE_DPHASE its drift.  Both of those
// constants are gone with it, along with s_wave_phase.  docs/wave-spec.md
// section 3b is explicit that the shape should come from a spline over a
// small control grid rather than from summed sines, and wave_field.h is that
// pipeline: a driven, damped spring chain per ribbon, resampled through a
// uniform cubic B-spline.  See tests/test_wave_field.c.
//
// Nothing below this line changed.  The field supplies a unitless
// displacement and wave_crest turns it into a screen row exactly as it always
// did, so wave_bg, wave_node, the NDC conversion, the strip layout, both
// submission paths and the gate file are all untouched.
static wf_field s_field;

// Two corrections turn a unitless displacement into the pixel excursion the
// sine used to have.  Both are needed, and the second one is not obvious.
//
//   WF_NOMINAL_PEAK.  The chain never uses the whole of its +/-1 range:
//   measured over 20,000 frames the peak wanders in [0.198, 0.653].  The sine
//   reached 1.0 before being multiplied by WAVE_AMP, so without this the
//   ribbons would be a third shallower than they are today.
//
//   WF_DRIVE[li].  MEASURED: chain amplitude is linear in drive, and
//   wave_field.h runs the back layers at 0.85 and 0.70 to make them calmer.
//   But WAVE_AMP ALREADY tapers them, 30 -> 22 -> 15 px.  Correcting with a
//   single scalar therefore applied the taper twice, and the measured spans
//   came out 59.4 / 37.2 / 20.8 px against the sine's 60 / 44 / 30 -- the
//   front ribbon right and the back two visibly flattened.  Dividing each
//   layer by its own drive puts all three back on their authored amplitude.
//
// What WF_DRIVE still does is what it is for: the back layers move more
// slowly and carry less fine detail.  It should not also be deciding how tall
// they are -- WAVE_AMP is what decides that, and now it is the only thing
// that does.
//
// One divide per (ribbon, column) per frame, 294 of them at 1080p.  Folding
// them into a table would trade that for file-scope dynamic initialisation,
// which is a worse thing to have on this target than a microsecond.
static inline float wave_field_px(int li, float fx, float W) {
    return wf_disp(&s_field, li, fx / W)
         * WAVE_AMP[li] / (WF_NOMINAL_PEAK * WF_DRIVE[li]);
}

// Seconds are not the unit here: wk_step's dt is the spec's TIMESTEP.
//
// The old phase advanced by a fixed WAVE_DPHASE per CALL, so the wave has
// always run at frame rate rather than at wall-clock rate.  Passing a fixed dt
// preserves exactly that -- including on a frame that takes 200 ms -- rather
// than quietly changing the animation into something time-based while the
// geometry underneath it is also changing.  Feeding a real frame delta is the
// better behaviour and it is a separate decision.
//
// The value matches the drift the ribbons had: the kernel advances its primary
// travelling wave at WK_W1 per unit time, so WK_W1 * dt = 0.0065 * 1.25 =
// 0.0081 rad per frame against the old WAVE_DPHASE[0] of 0.008 -- within 2%.
#define WAVE_FIELD_DT    1.25f

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

static u32   *s_wave_fp_buf     = NULL;
static u32    s_wave_fp_offset  = 0;

// --- vertex-array submission ---------------------------------------------
// Vertex layout is hud_dim.cpp's: 4 floats of position then 4 unsigned bytes of
// colour, which the attrib bindings below read exactly as that already-proven
// path does, so the wave's vertex program (4-component POS + COLOR0, same as
// the HUD dim program) needs no change at all.
//
// The one difference from hud_dim is that this struct is padded to 24 bytes
// rather than the natural 20 -- see the block comment on WaveVert.  The RSX
// side is unaffected: the stride passed to rsxBindVertexArrayAttrib follows
// sizeof, and the colour still sits at +16.
// This struct is 8-BYTE ALIGNED and its colour is ONE u32, not four u8 fields.
// Both are load-bearing, and the alignment is what actually keeps the console
// alive.
//
// MEASURED REASON FOR aligned(8).  This buffer is RSX local memory, and the PPU
// cannot issue a MISALIGNED 64-bit store to it -- doing so faults and takes the
// GPU down with it: black screen, console off the network, power cycle.
//
// Unpadded the struct is 20 bytes, so every odd vertex starts on a 4-byte but
// not 8-byte boundary.  GCC is free to merge two adjacent 4-byte fields into
// one `std`, and at those offsets that store is misaligned.  Whether it does so
// depends entirely on scheduling:
//
//   colours as compile-time constants -> it wrote the whole 80-byte block as
//     ten `std` at offsets 0,8,16,...,72.  All aligned. Worked, by luck.
//   colours as runtime reads of g_theme (Phase 1 of the XMB revamp) -> it wrote
//     per-vertex instead, emitting `std` at offsets 20, 28, 60 and 68.
//     Misaligned. Hung on the first frame, every time.
//
// Same values, same field widths -- only the scheduling moved.  aligned(8)
// makes sizeof 24 so every vertex starts 8-byte aligned and any merge GCC picks
// is safe. The stride passed to rsxBindVertexArrayAttrib follows sizeof, so the
// four padding bytes cost nothing but a slightly larger buffer.
//
// Do NOT drop the padding to "save memory", and do not assume a 4-byte-aligned
// struct in VRAM is safe because the current build happens not to merge stores.
//
// Colour is packed rather than four u8 fields for the neighbouring reason: the
// PPU cannot do sub-word stores to this memory either.
//
// MEASURED REASON.  This buffer lives in RSX local memory (rsxMemalign below),
// and the PPU cannot reliably issue sub-word stores to it -- a single-byte
// write wedges the GPU bus, taking the console off the network until it is
// power-cycled.
//
// As four u8 fields it USED to be safe only by accident: every colour written
// here was a compile-time constant, so GCC folded r/g/b/a into one 32-bit
// store.  The moment the palette became a runtime read of g_theme (Phase 1 of
// the XMB revamp) it could no longer fold them and emitted `stb` per channel --
// 57 byte-stores in wave_draw where the constant build had 25 -- and the first
// vertex of the first frame hung the console every time.
//
// Packing it explicitly makes the single aligned store a property of the code
// instead of a property of the optimiser. Big-endian PPC writes this u32 as
// bytes r,g,b,a at +0..+3, so the memory layout and the GCM_VERTEX_DATA_TYPE_U8
// binding at vo+16 are byte-for-byte what they always were.
//
// Anything else that writes vertex or texture data into VRAM must obey the same
// rule: whole aligned words, never bytes.
typedef struct { float x, y, z, w; u32 rgba; }
    __attribute__((aligned(8))) WaveVert;

#define WAVE_RGBA(r, g, b, a) (((u32)(r) << 24) | ((u32)(g) << 16) | \
                               ((u32)(b) <<  8) |  (u32)(a))

// Gradient quad (4) plus one strip per ribbon-slice, each two vertices per
// column.  This is the worst case; a 720p frame uses ~65 columns of it.
#define WAVE_MAX_VERTS  (4 + 3 * WAVE_NS * WAVE_MAX_COLS * 2)

// Two buffers, alternated on every call.  The RSX fetches the array
// asynchronously, so rebuilding the memory a queued draw has not consumed yet
// would tear the geometry.
//
// The rotation is owned here rather than taken from curr_fb deliberately:
// wave_draw() has six call sites across the XMB, the info panes, the music
// screen and the login OSK, and keying the buffer to the framebuffer index
// would alias if any frame ever drew the background twice.  Flipping per call
// gives every draw a buffer the previous one is finished with, whatever the
// call pattern, and costs a single XOR.
static WaveVert *s_wave_vbuf[2]     = { NULL, NULL };
static u32       s_wave_vbuf_off[2] = { 0, 0 };
static int       s_wave_vbuf_turn   = 0;
static bool      s_wave_varray      = false;
static bool      s_wave_blend       = false;   // mode 2

// Gate, with a mode rather than a second file so both can be flipped over FTP
// without another lookup:
//
//   absent / 0  immediate mode, baked opaque colours   (the original path)
//   1           vertex arrays, baked opaque colours
//   2           vertex arrays, GPU alpha blending      (see below)
//
// Mode 2 exists because the reason colours were baked in the first place was
// the belief that per-vertex alpha did not survive on this hardware -- the
// same comment block that declared vertex-array fetch unreliable, and wrong
// for the same reason.  Both shaders are pure passthrough
// (`MOV result.color, vertex.color` / `MOV result.color, fragment.color`), so
// alpha rides through untouched.  What was actually missing is that this
// function disables blending immediately before drawing the ribbons, so the
// alpha had nowhere to go and every ribbon came out solid.
#define GPUWAVE_FILE "jellyfin_gpuwave.txt"
static int gpuwave_mode(void) {
    FILE *f = fopen(jf_data_path(GPUWAVE_FILE), "r");
    if (!f) return 0;
    int v = 0;
    if (fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    return (v == 1 || v == 2) ? v : 0;
}

// Crest height (screen-space y) of ribbon li at horizontal position fx.
//
// The clamp stays even though the field cannot overshoot the way a sine could:
// WAVE_BASEY[2] is 0.91 and a full-amplitude excursion is under 24 px, so this
// never fires at any resolution the client runs at -- but W or H arriving as
// something absurd is a different failure, and a crest off the bottom of the
// screen is a worse one.
static inline float wave_crest(int li, float fx, float W, float H) {
    float wy = H * WAVE_BASEY[li] + wave_field_px(li, fx, W);
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
    // Re-seeding rather than zeroing: a chain zeroed in place is a FLAT LINE
    // that takes about t=20 to climb back to amplitude, so the old behaviour
    // (phases back to 0, shape unchanged) has no equivalent here.  wf_init
    // runs the warm-up, so this returns a field that is already moving.
    wf_init(&s_field, 0);
}

void wave_init(void) {
    // FIRST, before any of the early returns below.  ui_cpu_bg() and mode 0
    // both leave this function early, and an unseeded field reads flat -- so
    // seeding it further down would give the emulator and the immediate-mode
    // path three motionless straight ribbons while the vertex-array path
    // animated normally.  It costs one warm-up, once, at startup.
    wf_init(&s_field, 0);

    rsxFragmentProgram *fpo = (rsxFragmentProgram*)wave_fp_data;
    void *fp_ucode; u32 fp_size;
    rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fp_size);
    s_wave_fp_buf = (u32*)rsxMemalign(256, fp_size);
    memcpy(s_wave_fp_buf, fp_ucode, fp_size);
    rsxAddressToOffset(s_wave_fp_buf, &s_wave_fp_offset);

    if (ui_cpu_bg()) return;          // emulator draws the background on the CPU
    const int mode = gpuwave_mode();
    if (mode == 0) {
        plog("wave: vertex arrays disabled (jellyfin_gpuwave.txt absent/0) -- immediate mode");
        crash_log("wave: OFF (immediate mode)");
        return;
    }
    s_wave_blend = (mode == 2);
    // Either both buffers allocate or the feature stays off; a half-allocated
    // pair would give one framebuffer a working path and the other a null one.
    for (int i = 0; i < 2; i++) {
        s_wave_vbuf[i] = (WaveVert*)rsxMemalign(128, WAVE_MAX_VERTS * sizeof(WaveVert));
        if (!s_wave_vbuf[i]) {
            plog("wave: vertex buffer alloc FAILED -- staying on immediate mode");
            return;
        }
        rsxAddressToOffset(s_wave_vbuf[i], &s_wave_vbuf_off[i]);
    }
    s_wave_varray = true;
    {
        char msg[112];
        snprintf(msg, sizeof(msg), "wave: vertex arrays ON, mode %d (%s), %d verts max",
                 mode, s_wave_blend ? "GPU blending" : "baked opaque",
                 s_wave_blend ? (4 + 3 * WAVE_MAX_COLS * 2) : (int)WAVE_MAX_VERTS);
        plog(msg);
    }
    // Synchronous breadcrumb, and the ONLY one that survives: wave_init() runs
    // inside ui_init(), which main.cpp calls BEFORE plog_load_setting(), so
    // the plog line above is discarded on a cold boot.  ui_card_gpu_init() and
    // ui_text_gpu_init() were both moved out of ui_init() for exactly this;
    // until wave_init() follows them, crash_log is the instrument.
    crash_log(s_wave_blend ? "wave: ON (vertex arrays + GPU blend)"
                           : "wave: ON (vertex arrays, baked)");
}

// CPU rasterisation of the XMB background — gradient plus the three translucent
// ribbons — straight into the current framebuffer.  Mirrors the GPU wave_draw()
// math (grad_sample / wave_crest / over8) and tools/ui_preview/preview.c's
// cumulative per-pixel blend, including the animated phase, so the two paths
// look identical.  Used only when ui_cpu_bg() is true (emulator).
static void wave_draw_cpu(void) {
    u32 *fb = color_buffer[curr_fb];
    if (!fb) return;
    const int W = (int)display_width;
    const int H = (int)display_height;

    // Vertical gradient: one colour per row, then a flat row fill (fast).
    for (int y = 0; y < H; y++) {
        u8 r, g, b;
        grad_sample((float)y, (float)H, &r, &g, &b);
        u32 c = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        u32 *row = fb + (u32)y * W;
        for (int x = 0; x < W; x++) row[x] = c;
    }

    // Advance the animation exactly like the GPU path.
    {
        float ts, pert, drv;
        wave_audio_frame(&ts, &pert, &drv);
        wf_step(&s_field, WAVE_FIELD_DT * ts, pert, drv);
    }

    // Ribbons back-to-front, each composited over whatever is already in the
    // framebuffer (gradient + earlier ribbons) — same cumulative blend as
    // wave_bg().  Alpha fades linearly from the crest (WAVE_ALPHA) to 0 at the
    // screen bottom; step it per row to avoid a per-pixel divide.
    for (int li = 0; li < 3; li++) {
        u32 cr = (WAVE_COLOR[li] >> 16) & 0xFF;
        u32 cg = (WAVE_COLOR[li] >>  8) & 0xFF;
        u32 cb =  WAVE_COLOR[li]        & 0xFF;
        for (int x = 0; x < W; x++) {
            float cy = wave_crest(li, (float)x, (float)W, (float)H);
            int y0 = (int)cy;
            if (y0 < 0)  y0 = 0;
            if (y0 >= H) continue;
            // Fractional coverage of the crest pixel — without it the curve's
            // top edge lands on whole pixels and staircases; scaling the first
            // pixel's alpha by how much of it the ribbon actually covers
            // feathers the edge to match the crest's true sub-pixel position.
            float topcov = 1.0f - (cy - (float)y0);
            if (topcov < 0.0f) topcov = 0.0f;
            if (topcov > 1.0f) topcov = 1.0f;
            float denom = (float)(H - y0);
            if (denom < 1.0f) continue;
            float a_f  = (float)WAVE_ALPHA[li];
            float step = -(float)WAVE_ALPHA[li] / denom;
            u32  *col  = fb + (u32)y0 * W + x;
            for (int y = y0; y < H; y++, a_f += step, col += W) {
                u32 a = (u32)(y == y0 ? a_f * topcov : a_f);
                if (!a) continue;
                u32 bgp = *col;
                u32 ia  = 255 - a;
                u32 nr = (a * cr + ia * ((bgp >> 16) & 0xFF)) / 255;
                u32 ng = (a * cg + ia * ((bgp >>  8) & 0xFF)) / 255;
                u32 nb = (a * cb + ia * ( bgp        & 0xFF)) / 255;
                *col = (nr << 16) | (ng << 8) | nb;
            }
        }
    }
}

// One ribbon grid node: its NDC position and its pre-composited opaque colour.
// `fy` is the fraction of the way from this column's crest down to the screen
// bottom; `alpha` is the ribbon's opacity at that height.
//
// Both submission paths go through this, so the immediate-mode fallback and
// the vertex-array path cannot drift into drawing different pictures — which
// matters, because the only way to tell them apart is to look at a TV.
static inline void wave_node(int li, int ci, float fy, u8 alpha,
                             float W, float H,
                             const float *colx,
                             const float crest[3][WAVE_MAX_COLS],
                             float *out_x, float *out_y,
                             u8 *out_r, u8 *out_g, u8 *out_b) {
    const u8 cr = (WAVE_COLOR[li] >> 16) & 0xFF;
    const u8 cg = (WAVE_COLOR[li] >>  8) & 0xFF;
    const u8 cb =  WAVE_COLOR[li]        & 0xFF;
    float cy = crest[li][ci];
    float y  = cy + (H - cy) * fy;
    u8 br_, bg_, bb_;
    wave_bg(li, ci, y, H, crest, &br_, &bg_, &bb_);
    *out_x = (2.0f * colx[ci] / W) - 1.0f;
    *out_y = 1.0f - (2.0f * y / H);
    *out_r = over8(cr, br_, alpha);
    *out_g = over8(cg, bg_, alpha);
    *out_b = over8(cb, bb_, alpha);
}

// Push one immediate-mode vertex (colour latched first, position last — the
// position write commits the vertex, matching the HUD dim quad ordering).
static inline void wave_vtx(float x, float y, u8 r, u8 g, u8 b) {
    const u8    col[4] = { r, g, b, 255 };
    const float pos[4] = { x, y, 0.0f, 1.0f };
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    pos);
}

void wave_draw(void) {
    if (ui_cpu_bg()) { wave_draw_cpu(); return; }
    if (!s_wave_fp_buf) return;

    rsxVertexProgram  *vpo = (rsxVertexProgram*)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) wave_fp_data;

    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    // Off for the gradient in every mode; mode 2 turns it back on for the
    // ribbons once the gradient has landed.
    rsxSetBlendEnable(context, GCM_FALSE);

    float W = (float)display_width;
    float H = (float)display_height;

    // One step per wave_draw() call, which is where the phase advance used to
    // be and therefore keeps the animation rate exactly as it was -- including
    // on a screen that draws the background twice, where it always did double.
    //
    // The two literals used to be the spec's PERTURBATION and a full drive.
    // They are now the live values from wave_motion.h's stage B, mapped onto
    // this renderer's calibration by wave_render_map.h -- and, as that comment
    // predicted, replacing them is the whole of the renderer-side change.
    //
    // With no music playing, or with the gate off, these come back as the idle
    // set: perturb is WM_IDLE_PERTURB, which is the 0.02 that was written here
    // before, and dt_scale is exactly 1.0, so the drift rate is unchanged.
    // Only `drive` differs at rest (0.62 rather than 1.0), which is deliberate
    // -- see WRM_DRIVE_IDLE.
    //
    // wave_audio_frame() is safe to call twice in one frame; it is time-based
    // and the second call is a no-op that returns the same numbers, which is
    // what keeps the double-composited screens behaving as they always did.
    {
        float ts, pert, drv;
        wave_audio_frame(&ts, &pert, &drv);
        wf_step(&s_field, WAVE_FIELD_DT * ts, pert, drv);
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

    const u32 bgtop = XMB_BG_TOP, bgbot = XMB_BG_BOT;
    const u8 gtr=(bgtop>>16)&0xFF, gtg=(bgtop>>8)&0xFF, gtb=bgtop&0xFF;
    const u8 gbr=(bgbot>>16)&0xFF, gbg=(bgbot>>8)&0xFF, gbb=bgbot&0xFF;

    // Slice k spans the fraction [k/NS, (k+1)/NS] of the distance from this
    // column's crest down to the screen bottom.  One triangle strip per
    // (ribbon, slice), back-to-front, fully opaque.
    if (s_wave_varray) {
        s_wave_vbuf_turn ^= 1;
        WaveVert *v  = s_wave_vbuf[s_wave_vbuf_turn];
        u32       vo = s_wave_vbuf_off[s_wave_vbuf_turn];
        int       n  = 0;

        // Gradient quad first, so it occupies vertices [0,4).
        #define WV_PUT(px, py, pr, pg, pb, pa) do {             \
            v[n].x = (px); v[n].y = (py); v[n].z = 0.0f;        \
            v[n].w = 1.0f;                                      \
            v[n].rgba = WAVE_RGBA((pr), (pg), (pb), (pa));      \
            n++;                                                \
        } while (0)

        WV_PUT(-1.0f,  1.0f, gtr, gtg, gtb, 255);   // top-left
        WV_PUT(-1.0f, -1.0f, gbr, gbg, gbb, 255);   // bottom-left
        WV_PUT( 1.0f,  1.0f, gtr, gtg, gtb, 255);   // top-right
        WV_PUT( 1.0f, -1.0f, gbr, gbg, gbb, 255);   // bottom-right

        if (s_wave_blend) {
            // One quad per ribbon: constant tint, alpha ramping from the
            // crest opacity down to zero at the screen bottom.  The GPU
            // interpolates that ramp, which is why the WAVE_NS slicing is not
            // needed here -- the true fade is linear in y for a given column,
            // so a single quad is EXACT along every column edge and
            // approximates between columns exactly as the sliced version did.
            //
            // Per vertex this costs one NDC conversion and nothing else: no
            // grad_sample, no wave_bg, no over8. That arithmetic -- not the
            // FIFO -- was the 1,660 us the vertex-array change left behind.
            for (int li = 0; li < 3; li++) {
                const u8 cr = (WAVE_COLOR[li] >> 16) & 0xFF;
                const u8 cg = (WAVE_COLOR[li] >>  8) & 0xFF;
                const u8 cb =  WAVE_COLOR[li]        & 0xFF;
                for (int ci = 0; ci < ncols; ci++) {
                    float cx = (2.0f * colx[ci] / W) - 1.0f;
                    float cy = crest[li][ci];
                    WV_PUT(cx, 1.0f - (2.0f * cy / H), cr, cg, cb, WAVE_ALPHA[li]);
                    WV_PUT(cx, -1.0f,                  cr, cg, cb, 0);
                }
            }
        } else {
            for (int li = 0; li < 3; li++) {
                for (int k = 0; k < WAVE_NS; k++) {
                    float ft = (float)k       / WAVE_NS;
                    float fb = (float)(k + 1) / WAVE_NS;
                    u8 at = (u8)(WAVE_ALPHA[li] * (1.0f - ft));
                    u8 ab = (u8)(WAVE_ALPHA[li] * (1.0f - fb));
                    for (int ci = 0; ci < ncols; ci++) {
                        float xt, yt, xb, yb; u8 tr_,tg_,tb_, br_,bg_,bb_;
                        wave_node(li, ci, ft, at, W, H, colx, crest, &xt, &yt, &tr_,&tg_,&tb_);
                        wave_node(li, ci, fb, ab, W, H, colx, crest, &xb, &yb, &br_,&bg_,&bb_);
                        WV_PUT(xt, yt, tr_, tg_, tb_, 255);
                        WV_PUT(xb, yb, br_, bg_, bb_, 255);
                    }
                }
            }
        }
        #undef WV_PUT

        // Drain the PPU's write-gather buffer before the RSX reads these
        // vertices.  Same barrier, same reason, as ui_text_gpu.cpp's
        // submit_queue() -- the PPU has just written this buffer and the GPU is
        // about to fetch it, and those writes are still sitting in the gather
        // buffer until something forces them out.
        //
        // MEASURED REASON THIS IS NOT OPTIONAL.  Without it the path worked
        // only by accident: every colour written above used to be a
        // compile-time constant, so the stores had no dependencies, GCC hoisted
        // them well before the draw, and the buffer drained in time. Phase 1 of
        // the XMB revamp made the palette a runtime read of g_theme; the stores
        // then depended on a load, GCC scheduled them right up against the draw
        // calls, and the RSX fetched a partially-written vertex array and
        // wedged the GPU on the first frame -- black screen, console off the
        // network, power cycle to recover.
        //
        // The store widths and the values were identical either way; only the
        // scheduling moved.  Do not remove this because "it works without it".
        __asm__ __volatile__ ("sync" ::: "memory");

        // One binding for every draw below.  TEX0 is explicitly disabled
        // (stride 0, 0 elements) rather than left as whatever the last
        // textured draw set -- that stale binding is the actual bug behind
        // the "vertex arrays are unreliable" folklore.
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
            vo, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
            vo + 16, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
            0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

        // Gradient is opaque and must land before anything blends over it.
        rsxInvalidateVertexCache(context);
        rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);

        if (s_wave_blend) {
            // src*a + dst*(1-a), ribbons back to front -- algebraically the
            // same cumulative composite wave_bg() does on the CPU, and the
            // same blend the UI uses everywhere else.
            rsxSetBlendFunc(context,
                GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
            rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
            rsxSetBlendEnable(context, GCM_TRUE);
        }

        const u32 stripv  = (u32)(ncols * 2);
        const int nstrips = s_wave_blend ? 3 : (3 * WAVE_NS);
        for (int s = 0; s < nstrips; s++) {
            rsxInvalidateVertexCache(context);
            rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP,
                               4 + (u32)s * stripv, stripv);
        }

        // Release the colour array.  Everything the UI draws after the
        // background this frame -- cards, text, chrome, the dim quad --
        // submits inline, and a stale per-vertex COLOR0 array is what the
        // "unreliable fetch" folklore was actually describing.
        //
        // This is hud_dim.cpp's teardown verbatim: COLOR0 back to stride 0,
        // POS left bound.  That exact sequence is proven on this console, and
        // the failure mode for getting it wrong is a wedged GPU and a power
        // cycle, so it is copied rather than improved on.
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
            vo + 16, 0, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
    } else {
        // Background-gradient quad (XMB_BG_TOP -> XMB_BG_BOT), streamed inline.
        rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
        wave_vtx(-1.0f,  1.0f, gtr, gtg, gtb);   // top-left
        wave_vtx(-1.0f, -1.0f, gbr, gbg, gbb);   // bottom-left
        wave_vtx( 1.0f,  1.0f, gtr, gtg, gtb);   // top-right
        wave_vtx( 1.0f, -1.0f, gbr, gbg, gbb);   // bottom-right
        rsxDrawVertexEnd(context);

        for (int li = 0; li < 3; li++) {
            for (int k = 0; k < WAVE_NS; k++) {
                float ft = (float)k       / WAVE_NS;
                float fb = (float)(k + 1) / WAVE_NS;
                u8 at = (u8)(WAVE_ALPHA[li] * (1.0f - ft));
                u8 ab = (u8)(WAVE_ALPHA[li] * (1.0f - fb));
                rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
                for (int ci = 0; ci < ncols; ci++) {
                    float xt, yt, xb, yb; u8 tr_,tg_,tb_, br_,bg_,bb_;
                    wave_node(li, ci, ft, at, W, H, colx, crest, &xt, &yt, &tr_,&tg_,&tb_);
                    wave_node(li, ci, fb, ab, W, H, colx, crest, &xb, &yb, &br_,&bg_,&bb_);
                    wave_vtx(xt, yt, tr_, tg_, tb_);
                    wave_vtx(xb, yb, br_, bg_, bb_);
                }
                rsxDrawVertexEnd(context);
            }
        }
    }

    // Restore the UI's standard alpha-blend state for everything drawn after
    // the background this frame (ui_init configures the same).
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);
}

bool wave_gpu_blend_ready(void) { return s_wave_varray && s_wave_blend; }

// The faded hairline under the tab bar, as a blended GPU quad.
//
// MEASURED REASON THIS EXISTS.  The CPU version in ui_widgets.cpp reads the
// framebuffer once per pixel (`u32 bg = row[x]`) to composite its triangular
// alpha falloff, 1920 reads every frame.  It measured **1,351 us -- 704 ns per
// pixel**, which is the PPU's VRAM read cost almost exactly, and 76 % of the
// `cards` bucket.  It cost that on every screen, because the hairline is
// always full width.
//
// The falloff is `a = 72 * min(x, W-x) * 2 / W`: zero at both edges, peak at
// the centre, linear between.  That is exactly what the GPU interpolates
// across two quads, so THREE columns of vertices reproduce it with no
// approximation at all -- and no reads.  Six vertices, submitted inline.
void wave_draw_divider_gpu(int y_px, u8 r, u8 g, u8 b, u8 peak_alpha) {
    if (!s_wave_fp_buf) return;

    rsxVertexProgram   *vpo = (rsxVertexProgram*)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) wave_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    const float H  = (float)display_height;
    const float yt = 1.0f - (2.0f * (float)y_px       / H);
    const float yb = 1.0f - (2.0f * (float)(y_px + 1) / H);

    // colour latched first, position last -- the position write commits it.
    #define DV(px, py, pa) do {                                        \
        const u8    c4[4] = { r, g, b, (pa) };                         \
        const float p4[4] = { (px), (py), 0.0f, 1.0f };                \
        rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, c4);       \
        rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    p4);       \
    } while (0)

    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    DV(-1.0f, yt, 0);            DV(-1.0f, yb, 0);
    DV( 0.0f, yt, peak_alpha);   DV( 0.0f, yb, peak_alpha);
    DV( 1.0f, yt, 0);            DV( 1.0f, yb, 0);
    rsxDrawVertexEnd(context);
    #undef DV
}

// Full-screen dim quad — darkens the finished frame under a modal (the
// update popup).  Same inline immediate-mode path as the player's HUD dim
// quad (hud_dim.cpp): a blended black quad streamed straight into the FIFO,
// no vertex-array fetch, reusing the wave's resident passthrough programs.
// Fenced with rsxSync() before returning so the caller's CPU pixel writes
// (panel, text) may follow immediately.
void wave_dim_screen(u8 alpha) {
    if (!s_wave_fp_buf) return;

    rsxVertexProgram   *vpo = (rsxVertexProgram*)  wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*) wave_fp_data;

    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_wave_fp_offset, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    // wave_vtx() forces opaque vertices, so push these by hand (colour first,
    // position last — the position write commits the vertex).
    const u8    col[4] = { 0, 0, 0, alpha };
    const float tl[4]  = { -1.0f,  1.0f, 0.0f, 1.0f };
    const float tr[4]  = {  1.0f,  1.0f, 0.0f, 1.0f };
    const float bl[4]  = { -1.0f, -1.0f, 0.0f, 1.0f };
    const float br[4]  = {  1.0f, -1.0f, 0.0f, 1.0f };
    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_STRIP);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    tl);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    tr);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    bl);
    rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, col);
    rsxDrawVertex4f (context, GCM_VERTEX_ATTRIB_POS,    br);
    rsxDrawVertexEnd(context);

    rsxSync();
}
