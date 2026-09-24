#include "../../build_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include "rsxutil.h"
#include "wave_shaders.h"
#include "wave_field.h"
#include "wave_gel.h"          /* JellyWave: pulls wave_cam.h + wave_light.h */
#include "bg_gradient.h"
#include "timing.h"
#include "month_bg.h"
#include "ui_wave_audio.h"
#include "music_fft.h"         // spectrum bands: the sheets are the visualiser
#include "music_player.h"      // music_is_active
#include "ui_wave.h"
#include "ui_visuals.h"
#include "ui_strobe_test.h"
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

// The background, as four corners rather than a top and a bottom.  See
// source/ui/render/bg_gradient.h for why, and tests/test_bg_gradient.c for the
// proof that a two-stop quad still comes out of the bilinear sampler as the
// same vertical ramp this file drew before.
//
// REBUILT AT THE TOP OF EVERY DRAW, NOT CACHED.  It is two reads of g_theme and
// a struct copy.  Caching would need an invalidation hook on theme_cycle() --
// and on the day/night clock once a month table exists -- and a stale
// background is a bug that survives a theme change and looks for all the world
// like the theme picker is broken.  The static initialiser is only so that a
// caller arriving before the first refresh gets black rather than garbage.
static bg_quad s_bg = { { 0, 0, 0, 0 } };

static inline void wave_bg_refresh(void) {
    // month_bg_current() hands back exactly bg_from_two(top, bot) when no month
    // table is present, which is the shipping configuration -- so this is a
    // no-op change to the picture until somebody puts jellyfin_months.ini on
    // the console.  See source/ui/render/month_bg.h for why the numbers are not
    // in the build.
    s_bg = month_bg_current(XMB_BG_TOP, XMB_BG_BOT);
}

// Sample the background gradient at screen-space (u in [0,1], y in [0,H]),
// returning the three 8-bit channels.  Mirrors the gradient quad and
// tools/ui_preview/preview.c exactly.
//
// UNDITHERED ON PURPOSE.  This feeds the ribbon compositing, where the result
// is an INPUT to an alpha blend rather than a pixel.  Dithering here would put
// the noise through the blend and then dither the result again, which doubles
// it in exactly the region -- under the ribbons -- where the eye is already
// being given something to look at.  The dither goes on at the point the
// gradient becomes a pixel; see wave_draw_cpu().
static inline void grad_sample(float u, float y, float H, u8 *r, u8 *g, u8 *b) {
    float v = (H > 1.0f) ? (y / (H - 1.0f)) : 0.0f;
    bg_sample(&s_bg, u, v, r, g, b);
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
#define WAVE_LEGACY_VERTS  (4 + 3 * WAVE_NS * WAVE_MAX_COLS * 2)

// --- JellyWave's budget (mode 3) -----------------------------------------
//
// Each layer is emitted as ONE triangle strip covering all JW_SECTION strips
// of the lofted section, joined by degenerate pairs -- see the emit loop in
// wave_draw() for why that is safe here.  So per layer:
//
//   body  JW_SECTION strips * 2 * JW_STATIONS, plus 2 vertices per join
//   rim   6 strips (the ones carrying the rolled edge) on the same plan
//
// At JW_SECTION 12 and JW_STATIONS 80 that is 1,942 + 970 = 2,912 vertices and
// TWO draw calls per layer, so 8,740 vertices and 7 draws for the whole
// background including the gradient.  Against the legacy baked path's 4,708
// vertices and 25 draws that is 1.9x the geometry for 3.5x FEWER draw calls,
// which is the trade the degenerate joins buy.
//
// The buffer is sized for the larger of the two paths because both are
// compiled in and the gate picks between them at runtime.
#define JW_BODY_VERTS   (JW_SECTION * 2 * JW_STATIONS + (JW_SECTION - 1) * 2)
#define JW_RIM_STRIPS   6
#define JW_RIM_VERTS    (JW_RIM_STRIPS * 2 * JW_STATIONS + (JW_RIM_STRIPS - 1) * 2)
#define JW_TOTAL_VERTS  (4 + JW_LAYERS * (JW_BODY_VERTS + JW_RIM_VERTS))

#define WAVE_MAX_VERTS  ((WAVE_LEGACY_VERTS > JW_TOTAL_VERTS) \
                         ? WAVE_LEGACY_VERTS : JW_TOTAL_VERTS)

// Where the JellyWave body/rim stream starts in the RSX buffer.
//
// MEASURED REASON.  The RSX drew the strips at low vertex indices (below
// about 650) from stale data instead of from what the PPU uploaded -- most
// likely what the UI's immediate-mode text/card/icon draws left behind, which
// rsxInvalidateVertexCache() does not clear.  The far layer's body sat at
// indices 4-1945, so its first strips were drawn with the wrong positions:
// huge triangles, ~13 ms of GPU per frame, the XMB at 30 fps, and the strobe.
//
//   far body at index 4 (baseline)            13,237 us   strobe
//   same data at index 1024                      279 us   clean
//   same indices, data 24 KB further in       13,240 us   strobe
//   body off                                       0 us   clean
//
// Keyed on the index, not the data or the layer.  4096 rather than the 1024
// tested leaves headroom for busier screens (Settings, Search).  The gap is
// never written or uploaded: 96 KB of VRAM per buffer, nothing per frame.
// The gradient quad stays at [0,4).  The staging array is not gapped; the
// stream is placed at the base only when it is uploaded.
#define JW_VBASE        4096
#define WAVE_VBUF_VERTS (JW_VBASE + WAVE_MAX_VERTS)

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
static bool      s_wave_jelly       = false;   // mode 3 or 4 (shared pipeline)
static bool      s_wave_sheets      = false;   // mode 4: XMB light sheets

// JellyWave's CPU-side scratch: ONE layer's finished vertices, reused for each
// of the three.  It is plain main memory, not the RSX buffer -- jw_vert is not
// WaveVert and deliberately never goes near video memory, so the one dangerous
// layout stays in the one file that documents why it is dangerous.
static jw_vert   s_jw[JW_VERTS];

/*
 * Finished WaveVert stream in ordinary PPU/main memory.
 *
 * IMPORTANT: this is the only place the JellyWave WaveVert stream is built.
 * The RSX-local vertex buffers are WRITE-ONLY from the PPU.  Never read a
 * WaveVert back from s_wave_vbuf[].
 */
static WaveVert s_jw_stage[WAVE_MAX_VERTS];

static u64 s_jw_upload_us = 0;
static u32 s_jw_repaired  = 0;
static u32 s_jw_dropped   = 0;

/*
 * Upload the completed stream using whole aligned 64-bit stores only.
 *
 * WaveVert is 24 bytes and aligned(8), so every vertex and the complete
 * JellyWave stream are 64-bit aligned.  may_alias prevents strict-aliasing
 * from turning this into an unsafe typed access on the PPU.
 */
typedef u64 jw_upload_word __attribute__((may_alias));

static inline void jw_upload(WaveVert *dst, const WaveVert *src, u32 count)
{
    volatile jw_upload_word *d = (volatile jw_upload_word *)dst;
    const jw_upload_word *s = (const jw_upload_word *)src;
    u32 words = (count * (u32)sizeof(WaveVert)) / 8u;
    u32 i;

    for (i = 0; i < words; i++)
        d[i] = s[i];

    __asm__ __volatile__("sync" ::: "memory");
}

// Rolling cost, logged once a second.  The xmb: frame line's `gpu` bucket
// measures SUBMISSION, not RSX work, so the only honest things to report from
// here are what the PPU spent building the geometry and how much of it there
// was; the GPU's own cost shows up as `vsync` shrinking, which is read on the
// far side.
static u64 s_jw_gen_us   = 0;
static u32 s_jw_frames   = 0;
static u32 s_jw_rebuilds = 0;
static u32 s_jw_verts    = 0;
static u32 s_jw_draws    = 0;

// The draw ranges of whatever is CURRENTLY in the vertex buffer: [slot][0] is
// the body, [slot][1] the rim, slot 0 the furthest layer.  These are file
// scope rather than locals because a reuse frame draws last build's ranges --
// see JW_REBUILD below.
static u32 s_jw_off[JW_LAYERS][2];
static u32 s_jw_cnt[JW_LAYERS][2];
static int s_jw_have_geom = 0;     // 0 until the first build lands

// --- measured on hardware, 2026-09-21 -------------------------------------
//
// The first hardware run said the geometry costs 7,411 us of PPU per call to
// build -- 848 ns for each of its 8,740 vertices -- against an estimate of
// 400-700 us.  The frame went 16.68 ms to 21.3 ms and lost its 60 Hz lock.
//
// THE GPU WAS NOT THE PROBLEM, and the log says so unambiguously: `sync`, the
// bucket where the PPU stalls on the RSX fence, FELL from 4,135 us to 3,375.
// Had rasterisation become expensive it would have risen. It fell because the
// PPU now takes so much longer that the GPU gains slack. 8,740 vertices, 7
// draws and the extra blended fill cost the RSX nothing measurable.
//
// So the cost to attack is the per-vertex arithmetic, and the cheapest way to
// attack it is not to do it as often.

// --- speed ----------------------------------------------------------------
//
// A JellyWave-only multiplier on wf_step's dt, as a percentage.
//
// It exists because the old ribbons moved +/-30 px and this band moves about
// +/-320, so the SAME angular drift rate reads as far more agitated -- the
// wave looked too fast on a TV at a rate that was correct for the geometry it
// replaced.  WAVE_FIELD_DT itself is not touched: it is calibrated against the
// drift the original sine had, the legacy modes still run on it, and changing
// it would silently re-time mode 2 as well.
//
// Slowing down also calms the texture for free, which is the other half of
// what the wave needed.  wave_kernel.h injects its perturbation scaled by
// sqrt(h), so a smaller dt puts in proportionally less broadband noise per
// frame as well as advancing the travelling waves less.
//
// 9 is the default: 100 was judged too fast on hardware, and 50, 25 and 12
// still read as busy against the XMB's slow drift. Override in /dev_hdd0/tmp/ without a
// rebuild -- the whole point of the file is that "elegant" is a judgement made
// on a TV, not at a compiler.
#define JWSPEED_FILE  "jellyfin_jwspeed.txt"
#define JW_SPEED_DEF  9
static float s_jw_speed = JW_SPEED_DEF / 100.0f;

static int jwspeed_setting(void) {
    FILE *f = fopen(jf_data_path(JWSPEED_FILE), "r");
    if (!f) return JW_SPEED_DEF;
    int v = JW_SPEED_DEF;
    if (fscanf(f, "%d", &v) != 1) v = JW_SPEED_DEF;
    fclose(f);
    if (v < 1)   v = 1;          // 0 would freeze the wave entirely
    if (v > 400) v = 400;
    return v;
}

// --- rebuild cadence ------------------------------------------------------
//
// Rebuild the geometry every Nth call and let the RSX re-draw the buffer it
// already has in between.  At N = 3 the 7,411 us build amortises to about
// 2,470 us a frame, which is what buys the 60 Hz lock back.
//
// THIS IS SAFE WITH THE DOUBLE BUFFER, and in fact safer than what it
// replaces.  The two buffers exist because the RSX fetches asynchronously, so
// rewriting memory a queued draw has not consumed yet would tear the geometry.
// Today every call flips and rewrites, so a buffer is reused after ONE
// intervening frame.  With a cadence the flip happens only on a rebuild, so
// the buffer being written was last read N frames ago -- longer, not shorter.
// A reuse frame writes nothing at all and needs no `sync` barrier.
//
// IT IS ONLY DEFENSIBLE BECAUSE THE WAVE IS SLOW.  Sampling a slow curve at
// 20 Hz and holding each sample for three frames is invisible; doing it to
// something with fast detail would judder. The two settings are therefore
// related, and lowering JW_SPEED is what makes a higher N free.//
// The gradient quad lives in the same buffer and is rebuilt with it, so a
// theme change takes up to N calls to appear -- 50 ms at N = 3. That is the
// one visible cost and it is well under a frame of human latency.
#define JWREBUILD_FILE  "jellyfin_jwrebuild.txt"
#define JW_REBUILD_DEF  3
static int s_jw_rebuild_every = JW_REBUILD_DEF;
static int s_jw_rebuild_phase = 0;

static int jwrebuild_setting(void) {
    FILE *f = fopen(jf_data_path(JWREBUILD_FILE), "r");
    if (!f) return JW_REBUILD_DEF;
    int v = JW_REBUILD_DEF;
    if (fscanf(f, "%d", &v) != 1) v = JW_REBUILD_DEF;
    fclose(f);
    if (v < 1) v = 1;            // 1 = rebuild every call, the old behaviour
    if (v > 8) v = 8;            // beyond this the motion visibly steps
    return v;
}

// Gate, with a mode rather than a second file so both can be flipped over FTP
// without another lookup:
//
//   0           immediate mode, baked opaque colours   (the original path)
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
// The background dither gate.  Separate file from the wave's, so the two can
// be flipped independently over FTP: the dither is a global RSX state change
// and the wave's submission path is not, so they fail in different ways and
// bisecting them together would be guesswork.
//
// Default is ON.  Unlike the wave gates, this binds no buffer and changes no
// binding -- it sets one register that is turned off again a few draws later
// -- so its failure mode is "the gradient looks slightly different", not a
// wedged GPU.  Write 0 to the file to disable it.
#define BGDITHER_FILE "jellyfin_bgdither.txt"
static int s_bg_dither = 1;

static int bgdither_setting(void) {
    FILE *f = fopen(jf_data_path(BGDITHER_FILE), "r");
    if (!f) return 1;                       // absent = on
    int v = 1;
    if (fscanf(f, "%d", &v) != 1) v = 1;
    fclose(f);
    return v ? 1 : 0;
}

// Mode 3 is JellyWave: the approved translucent-gel design, lofted in 3-D on
// the PPU and projected through wave_cam.h's fixed camera.  It reuses this
// file's proven vertex-array machinery unchanged -- same WaveVert, same
// bindings, same sync barrier, same teardown -- and adds exactly one piece of
// RSX state the other modes do not use: an additive blend function for the
// rim pass, set and put back between draws.
//
// It shares the gate file rather than taking a new one so a bad frame is one
// character away from mode 2 over FTP, with no reflash.
#define GPUWAVE_FILE "jellyfin_gpuwave.txt"
static int gpuwave_mode(void) {
    // Absent = mode 4, the XMB light sheets the new UI is designed around.
    // The file still overrides it: 3 is the JellyWave gel ribbons, 0 drops
    // back to immediate mode with no reflash.
    FILE *f = fopen(jf_data_path(GPUWAVE_FILE), "r");
    if (!f) return 4;
    int v = 4;
    if (fscanf(f, "%d", &v) != 1) v = 4;
    fclose(f);
    return (v >= 1 && v <= 4) ? v : 0;
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
static void wave_bg(int upto, int ci, float u, float y, float H,
                    const float crest[3][WAVE_MAX_COLS], u8 *r, u8 *g, u8 *b) {
    grad_sample(u, y, H, r, g, b);
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

    // Also before the early returns: the gradient quad is drawn on the
    // immediate-mode path too, so reading this only after the mode-0 return
    // would leave the dither off in exactly the configuration the client
    // ships with the gate file absent.
    s_bg_dither = bgdither_setting();
    month_bg_load();
    wave_bg_refresh();

    rsxFragmentProgram *fpo = (rsxFragmentProgram*)wave_fp_data;
    void *fp_ucode; u32 fp_size;
    rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fp_size);
    s_wave_fp_buf = (u32*)rsxMemalign(256, fp_size);
    memcpy(s_wave_fp_buf, fp_ucode, fp_size);
    rsxAddressToOffset(s_wave_fp_buf, &s_wave_fp_offset);

    const int mode = gpuwave_mode();
    if (ui_cpu_bg()) {
        // The emulator draws the background on the CPU (see ui_cpu_bg), but
        // it still honours the JellyWave choice: wave_draw_cpu rasterises the
        // same geometry in software.  No RSX buffers are needed for that.
        s_wave_jelly  = (mode == 3 || mode == 4);
        s_wave_sheets = (mode == 4);
        if (s_wave_jelly) {
            s_jw_speed         = (float)jwspeed_setting() / 100.0f;
            s_jw_rebuild_every = jwrebuild_setting();
            s_jw_rebuild_phase = 0;
            s_jw_have_geom     = 0;
        }
        crash_log(s_wave_sheets ? "wave: CPU (XMB sheets, software raster)"
                : s_wave_jelly  ? "wave: CPU (JellyWave, software raster)"
                                : "wave: CPU (flat ribbons)");
        return;
    }
    if (mode == 0) {
        plog("wave: vertex arrays disabled (jellyfin_gpuwave.txt = 0) -- immediate mode");
        crash_log("wave: OFF (immediate mode)");
        return;
    }
    s_wave_blend = (mode == 2 || mode == 3);
    s_wave_jelly  = (mode == 3 || mode == 4);
    s_wave_sheets = (mode == 4);
    // Either both buffers allocate or the feature stays off; a half-allocated
    // pair would give one framebuffer a working path and the other a null one.
    for (int i = 0; i < 2; i++) {
        s_wave_vbuf[i] = (WaveVert*)rsxMemalign(128, WAVE_VBUF_VERTS * sizeof(WaveVert));
        if (!s_wave_vbuf[i]) {
            plog("wave: vertex buffer alloc FAILED -- staying on immediate mode");
            return;
        }
        rsxAddressToOffset(s_wave_vbuf[i], &s_wave_vbuf_off[i]);
    }
    s_wave_varray = true;
    {
        char msg[144];
        const char *what = s_wave_sheets ? "XMB sheets"
                         : s_wave_jelly ? "JellyWave 3D"
                         : s_wave_blend ? "GPU blending" : "baked opaque";
        int verts = s_wave_jelly ? (int)JW_TOTAL_VERTS
                  : s_wave_blend ? (4 + 3 * WAVE_MAX_COLS * 2)
                                 : (int)WAVE_LEGACY_VERTS;
        snprintf(msg, sizeof(msg), "wave: vertex arrays ON, mode %d (%s), %d verts",
                 mode, what, verts);
        plog(msg);
    }
    if (s_wave_jelly) {
        char msg[192];
        int  sp = jwspeed_setting();
        s_jw_speed         = (float)sp / 100.0f;
        s_jw_rebuild_every = jwrebuild_setting();
        s_jw_rebuild_phase = 0;
        s_jw_have_geom     = 0;
        snprintf(msg, sizeof(msg),
                 "wave: JellyWave %d stations x %d section, %d verts/layer, "
                 "%d layers, %d draws",
                 JW_STATIONS, JW_SECTION, JW_BODY_VERTS + JW_RIM_VERTS,
                 JW_LAYERS, 1 + JW_LAYERS * 2);
        plog(msg);
        snprintf(msg, sizeof(msg),
                 "wave: JellyWave speed=%d%% rebuild=every %d call%s "
                 "(%s / %s)",
                 sp, s_jw_rebuild_every, s_jw_rebuild_every == 1 ? "" : "s",
                 JWSPEED_FILE, JWREBUILD_FILE);
        plog(msg);
    }
    // Synchronous breadcrumb, and the ONLY one that survives: wave_init() runs
    // inside ui_init(), which main.cpp calls BEFORE plog_load_setting(), so
    // the plog line above is discarded on a cold boot.  ui_card_gpu_init() and
    // ui_text_gpu_init() were both moved out of ui_init() for exactly this;
    // until wave_init() follows them, crash_log is the instrument.
    crash_log(s_wave_sheets ? "wave: ON (XMB sheets)"
            : s_wave_jelly ? "wave: ON (JellyWave 3D)"
            : s_wave_blend ? "wave: ON (vertex arrays + GPU blend)"
                           : "wave: ON (vertex arrays, baked)");
}

// Build the complete JellyWave body/rim stream into s_jw_stage, starting at
// index n, and record each layer's draw ranges in s_jw_off / s_jw_cnt.
// Returns the index one past the last vertex written.  Shared by the RSX path
// (wave_draw) and the emulator's CPU rasteriser (wave_draw_cpu), so the two
// cannot drift into drawing different geometry.
static int jw_emit_stream(float W, float H, int n)
{
    #define JW_EMIT(Q, A, USE_RIM) do {                         \
        const jw_vert *q_ = (Q);                                \
        s_jw_stage[n].x = q_->x;                                \
        s_jw_stage[n].y = q_->y;                                \
        s_jw_stage[n].z = 0.0f;                                 \
        s_jw_stage[n].w = 1.0f;                                 \
        s_jw_stage[n].rgba = (USE_RIM)                         \
            ? WAVE_RGBA(q_->rr, q_->rg, q_->rb, 255)            \
            : WAVE_RGBA(q_->r, q_->g, q_->b, (A));              \
        n++;                                                    \
    } while (0)

    for (int slot = 0; slot < JW_LAYERS; slot++) {
        const int       li = JW_LAYERS - 1 - slot;
        const jw_layer *L  = &JW_LAYER[li];
        int order[JW_SECTION];
        int pass, s, i;

        s_jw_cnt[slot][0] = s_jw_cnt[slot][1] = 0;
        s_jw_off[slot][0] = s_jw_off[slot][1] = 0;

        // LINKED: every layer rides the near layer's displacement, so the
        // three ribbons move as one stacked band.  Given a field each, they
        // drifted independently and cut across one another, and the near
        // layer (alpha 255) left a hard edge wherever it passed over the
        // others.  Each layer keeps its own depth, offset, scale, phase and
        // colour; the near layer's gain applies because it is the near
        // layer's curve, and it lands every layer on the design's peak.
        jw_layer Lk = *L;
        Lk.disp_gain = JW_LAYER[0].disp_gain;
        if (!jw_build_layer(&Lk, s_field.sy[0], WF_SAMPLES,
                            W / H, s_jw, JW_VERTS))
            continue;

        {
            int repaired = 0;
            if (!jw_sanitize_layer(s_jw, &repaired)) {
                s_jw_dropped++;
                continue;
            }
            s_jw_repaired += (u32)repaired;
        }

        jw_strip_order(s_jw, order);

        for (pass = 0; pass < 2; pass++) {
            int started = 0;
            const jw_vert *tail = NULL;

            s_jw_off[slot][pass] = (u32)n;

            for (s = 0; s < JW_SECTION; s++) {
                const int j  = order[s];
                const int j2 = (j + 1) % JW_SECTION;

                if (pass == 1 && !jw_strip_has_rim(j))
                    continue;

                if (n + 2 * JW_STATIONS + 2 > WAVE_MAX_VERTS)
                    break;

                if (started) {
                    /*
                     * Degenerate join, entirely from CPU/main memory.
                     * NEVER do v[n] = v[n-1] here: v is the RSX-local
                     * destination and PPU readback from it is unsafe.
                     */
                    JW_EMIT(tail, L->alpha, pass);
                    JW_EMIT(&s_jw[0 * JW_SECTION + j],
                            L->alpha, pass);
                }

                started = 1;

                for (i = 0; i < JW_STATIONS; i++) {
                    JW_EMIT(&s_jw[i * JW_SECTION + j],
                            L->alpha, pass);
                    JW_EMIT(&s_jw[i * JW_SECTION + j2],
                            L->alpha, pass);
                }

                tail = &s_jw[(JW_STATIONS - 1) * JW_SECTION + j2];
            }

            s_jw_cnt[slot][pass] =
                (u32)n - s_jw_off[slot][pass];
        }
    }

    #undef JW_EMIT
    return n;
}

// --- JellyWave in software (emulator only) --------------------------------
//
// RPCS3 cannot present a frame that mixes RSX drawing with CPU writes into the
// same bound colour buffer: it logs "Cannot invalidate a currently bound render
// target!" every frame and shows one or the other, which reads as the wave
// flickering or vanishing.  So under ui_cpu_bg() the whole frame stays on the
// CPU, and JellyWave is drawn by rasterising the exact triangle strips the RSX
// would get, with the same src-alpha / one-minus-src-alpha blend.  Never runs
// on hardware, where the framebuffer is uncached VRAM.

// Edge function for the directed edge a->b, evaluated at p.
static inline float jw_edge(float ax, float ay, float bx, float by,
                            float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// Top-left fill rule for a triangle wound so its area is positive: a pixel
// centre exactly on a shared edge belongs to one triangle only.  Without it,
// adjacent triangles of a translucent strip would blend that pixel twice.
static inline bool jw_top_left(float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    return (dy < 0.0f) || (dy == 0.0f && dx > 0.0f);
}

static inline u32 jw_div255(u32 t) { return (t + 1 + (t >> 8)) >> 8; }

static void jw_raster_tri(u32 *fb, int W, int H,
                          const WaveVert *va, const WaveVert *vb,
                          const WaveVert *vc, bool additive) {
    // NDC to pixels, y down -- the same mapping the viewport applies.
    float ax = (va->x + 1.0f) * 0.5f * W, ay = (1.0f - va->y) * 0.5f * H;
    float bx = (vb->x + 1.0f) * 0.5f * W, by = (1.0f - vb->y) * 0.5f * H;
    float cx = (vc->x + 1.0f) * 0.5f * W, cy = (1.0f - vc->y) * 0.5f * H;
    u32 ca = va->rgba, cb = vb->rgba, cc = vc->rgba;

    float area = jw_edge(ax, ay, bx, by, cx, cy);
    if (area > -1e-4f && area < 1e-4f) return;       // degenerate strip join
    if (area < 0.0f) {                               // strips alternate winding
        float t;
        t = bx; bx = cx; cx = t;
        t = by; by = cy; cy = t;
        u32 tc = cb; cb = cc; cc = tc;
        area = -area;
    }

    int y0 = (int)floorf(fminf(ay, fminf(by, cy)));
    int y1 = (int)ceilf (fmaxf(ay, fmaxf(by, cy)));
    if (y0 < 0) y0 = 0;
    if (y1 > H) y1 = H;
    if (y0 >= y1) return;

    const bool tl[3] = { jw_top_left(bx, by, cx, cy),
                         jw_top_left(cx, cy, ax, ay),
                         jw_top_left(ax, ay, bx, by) };
    const float inv = 1.0f / area;

    // Each channel is a plane over the triangle: value = C + X*px + Y*py.
    // Barycentric weight of a: w0/area, with w0 = e0x*px + e0y*py + e0c, etc.
    const float ex[3] = { -(cy - by) * inv, -(ay - cy) * inv, -(by - ay) * inv };
    const float ey[3] = {  (cx - bx) * inv,  (ax - cx) * inv,  (bx - ax) * inv };
    const float ec[3] = { jw_edge(bx, by, cx, cy, 0.0f, 0.0f) * inv,
                          jw_edge(cx, cy, ax, ay, 0.0f, 0.0f) * inv,
                          jw_edge(ax, ay, bx, by, 0.0f, 0.0f) * inv };
    float chX[4], chY[4], chC[4];
    int   chLo[4], chHi[4];
    for (int k = 0; k < 4; k++) {
        const int sh = 24 - 8 * k;
        const int ia_ = (int)((ca >> sh) & 0xFF);
        const int ib_ = (int)((cb >> sh) & 0xFF);
        const int ic_ = (int)((cc >> sh) & 0xFF);
        const float va_ = (float)ia_, vb_ = (float)ib_, vc_ = (float)ic_;
        chX[k] = ex[0] * va_ + ex[1] * vb_ + ex[2] * vc_;
        chY[k] = ey[0] * va_ + ey[1] * vb_ + ey[2] * vc_;
        chC[k] = ec[0] * va_ + ec[1] * vb_ + ec[2] * vc_;
        // Interpolation can never leave the corners' range.  On a sliver
        // (a sheet going edge-on) the plane is steep enough that a pixel
        // centre just past the edge extrapolates far outside it -- that was
        // the bright fleck where two edges met.
        chLo[k] = ia_ < ib_ ? (ia_ < ic_ ? ia_ : ic_) : (ib_ < ic_ ? ib_ : ic_);
        chHi[k] = ia_ > ib_ ? (ia_ > ic_ ? ia_ : ic_) : (ib_ > ic_ ? ib_ : ic_);
    }

    for (int y = y0; y < y1; y++) {
        const float py = (float)y + 0.5f;
        // The covered span on this row: each edge is linear in x, so solve
        // for where all three weights are non-negative instead of scanning a
        // bounding box -- the strips are long slanted slivers, and their boxes
        // are mostly empty.
        float lo = 0.0f, hi = (float)W;
        bool empty = false;
        for (int e = 0; e < 3 && !empty; e++) {
            const float w0 = ec[e] + ey[e] * py;       // weight at px = 0
            if (ex[e] > 0.0f) {
                const float xr = -w0 / ex[e];          // w >= 0 for px >= xr
                if (xr > lo) lo = xr;
            } else if (ex[e] < 0.0f) {
                const float xr = -w0 / ex[e];          // w >= 0 for px <= xr
                if (xr < hi) hi = xr;
            } else if (w0 < 0.0f || (w0 == 0.0f && !tl[e])) {
                empty = true;
            }
        }
        if (empty) continue;
        // Pixel x is covered when its centre x+0.5 lies in [lo, hi).
        int xs = (int)ceilf(lo - 0.5f);
        int xe = (int)ceilf(hi - 0.5f);
        if (xs < 0) xs = 0;
        if (xe > W) xe = W;
        if (xs >= xe) continue;

        const float px = (float)xs + 0.5f;
        float r = chC[0] + chX[0] * px + chY[0] * py;
        float g = chC[1] + chX[1] * px + chY[1] * py;
        float b = chC[2] + chX[2] * px + chY[2] * py;
        float a = chC[3] + chX[3] * px + chY[3] * py;
        u32 *row = fb + (u32)y * (u32)W;
        for (int x = xs; x < xe; x++,
             r += chX[0], g += chX[1], b += chX[2], a += chX[3]) {
            int ai = (int)(a + 0.5f);
            ai = ai < chLo[3] ? chLo[3] : ai > chHi[3] ? chHi[3] : ai;
            if (ai <= 0) continue;
            int ri = (int)(r + 0.5f), gi = (int)(g + 0.5f), bi = (int)(b + 0.5f);
            ri = ri < chLo[0] ? chLo[0] : ri > chHi[0] ? chHi[0] : ri;
            gi = gi < chLo[1] ? chLo[1] : gi > chHi[1] ? chHi[1] : gi;
            bi = bi < chLo[2] ? chLo[2] : bi > chHi[2] ? chHi[2] : bi;
            const u32 d  = row[x];
            u32 nr, ng, nb;
            if (additive) {                  // src*a + dst, clamped
                nr = ((d >> 16) & 0xFF) + jw_div255((u32)ri * (u32)ai);
                ng = ((d >>  8) & 0xFF) + jw_div255((u32)gi * (u32)ai);
                nb = ( d        & 0xFF) + jw_div255((u32)bi * (u32)ai);
                if (nr > 255) nr = 255;
                if (ng > 255) ng = 255;
                if (nb > 255) nb = 255;
            } else {                         // src*a + dst*(1-a)
                const u32 ia = 255u - (u32)ai;
                nr = jw_div255((u32)ri * (u32)ai + ((d >> 16) & 0xFF) * ia);
                ng = jw_div255((u32)gi * (u32)ai + ((d >>  8) & 0xFF) * ia);
                nb = jw_div255((u32)bi * (u32)ai + ( d        & 0xFF) * ia);
            }
            row[x] = (nr << 16) | (ng << 8) | nb;
        }
    }
}

// Draw the current JellyWave stream (s_jw_stage + s_jw_off/s_jw_cnt) into fb,
// layer by layer, body then rim -- the RSX path's draw order and blends.
static void jw_raster_stream(u32 *fb, int W, int H) {
    for (int slot = 0; slot < JW_LAYERS; slot++) {
        for (int pass = 0; pass < 2; pass++) {
            const u32 off = s_jw_off[slot][pass];
            const u32 cnt = s_jw_cnt[slot][pass];
            for (u32 i = 0; i + 2 < cnt; i++)
                jw_raster_tri(fb, W, H, &s_jw_stage[off + i],
                              &s_jw_stage[off + i + 1],
                              &s_jw_stage[off + i + 2],
                              pass == 1 || s_wave_sheets);
        }
    }
}

// --- mode 4: XMB light sheets ------------------------------------------------
//
// The PS3's own XMB wave, rather than JellyWave's lit gel tubes: several thin
// translucent sheets of white light that cross one another freely.  Every
// sheet is drawn ADDITIVELY, so an overlap only ever gets brighter -- there is
// no draw order and no hard edge where one passes over another.  What the eye
// reads as the wave's structure is the brightness at each sheet's two edges
// and, above all, wherever a sheet twists edge-on to the viewer: there its
// two edges meet (half-width through zero) and the whole sheet collapses into
// a bright line, as in the XMB.
//
// Motion comes from the same spring field as every other mode (so the speed
// file and the music reaction still drive it); each sheet reads a different
// window of it so no two move in lockstep.  Output goes through the JellyWave
// stream and draw ranges unchanged -- 6 sheets in the 3 x 2 [slot][pass]
// ranges -- so the RSX path and the emulator's rasteriser both draw it.
#define XW_SHEETS 6
#define XW_COLS   96

// Field layer l at u in [0,1], linearly interpolated and normalised so the
// layer's nominal peak is about 1 (the layers are driven at different
// strengths -- see the peaks measured in wave_gel.h).
static inline float xw_field(int l, float u) {
    static const float norm[3] = { 1.0f / 0.65f, 1.0f / 0.556f, 1.0f / 0.455f };
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    const float f = u * (float)(WF_SAMPLES - 1);
    int k = (int)f;
    if (k > WF_SAMPLES - 2) k = WF_SAMPLES - 2;
    const float fr = f - (float)k;
    const float *d = s_field.sy[l];
    return (d[k] + (d[k + 1] - d[k]) * fr) * norm[l];
}

// The music visualiser, folded into the sheets.  Each sheet owns one slice of
// the spectrum -- sheet 0 the deep bass up to sheet 5 the treble, out of the
// same 28 FFT bands that used to drive the Now Playing bars -- and moves ONLY
// with it: its shape deepens, it lifts through the middle (the ends stay put),
// it widens and it brightens.  So a kick moves the bass sheet and a hi-hat the
// treble one, independently.
//
// Each sheet is normalised against its own recent peak (a slow automatic
// gain), because treble carries a fraction of the bass's energy and would
// otherwise barely move.  Fast attack, slow release, per sheet.  With no music
// the levels ease to zero and the sheets settle into their ordinary drift.
// The bands are log-spaced 40 Hz - 16 kHz, ~1.24x apart, so band b starts
// near 40 * 1.24^b Hz.  The bottom two sheets overlap on purpose so the kick
// and bass region (roughly 40-200 Hz) has two sheets to move, not one sliver.
static const int XW_BAND_LO[XW_SHEETS] = { 0, 4, 7, 11, 16, 21 };
static const int XW_BAND_HI[XW_SHEETS] = { 5, 8, 11, 16, 21, 28 };   // exclusive
// Extra gain per sheet: bass reads weak after the spectrum's per-frame
// normalisation (it is always present, so it is rarely the frame's peak).
static const float XW_SHEET_GAIN[XW_SHEETS] = { 1.6f, 1.3f, 1.0f, 1.0f, 1.0f, 1.0f };
// A bass pulse shared by every sheet, from bands 0-6 (~40-150 Hz), so the whole
// band thumps on a kick.  Its own peak forgets fast (~1 s) so a steady bass
// line does not normalise itself away the way the per-sheet gain would.
#define XW_BASS_HI      7
#define XW_BASS_SHARE   0.35f
static float s_xw_bass_peak = 0.0f;
static float s_xw_lev[XW_SHEETS];    // smoothed, normalised 0..1
static float s_xw_vel[XW_SHEETS];    // the spring's velocity

// How far a sheet at full level moves.  All four in one place so the
// sensitivity is one edit: shape deepening (fraction of the idle shape),
// lift through the middle (fraction of screen height), widening and glow.
#define XW_MUSIC_SHAPE 0.80f
#define XW_MUSIC_LIFT  0.035f
#define XW_MUSIC_SWELL 0.55f
#define XW_MUSIC_GLOW  0.55f

// Each sheet follows its level through a critically damped spring: quick to
// track a change, but it glides into it -- no snap, no overshoot, no jitter.
// A one-pole ease had to choose between following the beat and staying
// smooth; the spring's momentum gets both.  XW_MUSIC_RATE is its natural
// frequency in rad/s: higher follows tighter (a hit lands in ~4/rate s).
#define XW_MUSIC_RATE  26.0f
static float s_xw_peak[XW_SHEETS];   // slow per-sheet peak for the gain

static void xw_update_bands(float dt) {
    // ABSOLUTE band levels (music_viz_raw), each sheet normalised against its
    // own recent peak.  The normalised bands the bars used divide every frame
    // by its loudest band -- usually the bass -- which flattened the low end
    // into a constant; on absolute levels a kick is the jump it really is.
    float norm[MUSIC_VIZ_BANDS], raw[MUSIC_VIZ_BANDS];
    const bool on = music_is_active();
    if (on) { music_viz_bands(norm); music_viz_raw(raw); }
    // A floor for every gain, relative to the loudest level heard lately, so
    // a quiet passage cannot blow noise up to full scale.
    static float s_all_peak = 0.0f;
    float bass = 0.0f;
    if (on) {
        float mx = 0.0f;
        for (int b = 0; b < MUSIC_VIZ_BANDS; b++) if (raw[b] > mx) mx = raw[b];
        s_all_peak = mx > s_all_peak ? mx : s_all_peak * 0.999f;
        float sum = 0.0f;
        for (int b = 0; b < XW_BASS_HI; b++) sum += raw[b];
        const float m = sum / (float)XW_BASS_HI;
        s_xw_bass_peak = m > s_xw_bass_peak ? m : s_xw_bass_peak * 0.985f;
        const float fl = 0.05f * s_all_peak;
        bass = m / (s_xw_bass_peak > fl ? s_xw_bass_peak : fl);
        // Emphasise the hit over the sustain: square it, so a kick at the
        // peak drives fully and the body of the note sits well below.
        bass *= bass;
    }
    for (int k = 0; k < XW_SHEETS; k++) {
        float target = 0.0f;
        if (on && s_all_peak > 0.0f) {
            float sum = 0.0f;
            for (int b = XW_BAND_LO[k]; b < XW_BAND_HI[k]; b++) sum += raw[b];
            const float m = sum / (float)(XW_BAND_HI[k] - XW_BAND_LO[k]);
            // ~1.5 s memory: long enough to hold a song's level, short
            // enough that each hit still stands out from the one before.
            s_xw_peak[k] = m > s_xw_peak[k] ? m : s_xw_peak[k] * 0.993f;
            const float fl = 0.03f * s_all_peak;
            target = m / (s_xw_peak[k] > fl ? s_xw_peak[k] : fl);
            target = target * target;            // hits over sustain
            target = target * XW_SHEET_GAIN[k] + XW_BASS_SHARE * bass;
            if (target > 1.0f) target = 1.0f;
        }
        // Critically damped spring, in small steps so it stays stable
        // across a slow frame.
        const float w = XW_MUSIC_RATE;
        float left = dt;
        while (left > 0.0f) {
            const float h = left > 0.008f ? 0.008f : left;
            left -= h;
            s_xw_vel[k] += (w * w * (target - s_xw_lev[k])
                            - 2.0f * w * s_xw_vel[k]) * h;
            s_xw_lev[k] += s_xw_vel[k] * h;
        }
        if (s_xw_lev[k] < 0.0f) { s_xw_lev[k] = 0.0f; s_xw_vel[k] = 0.0f; }
        if (s_xw_lev[k] > 1.2f) s_xw_lev[k] = 1.2f;
    }
}

// The scroll: a travelling wave in every sheet, advanced by the clock, so the
// band keeps flowing across the screen whatever the spring field and the music
// are doing.  The field alone mostly swells in place at these speeds, and with
// the music reaction on top nothing read as moving.  Scaled by the speed file
// relative to its default, so jellyfin_jwspeed.txt still governs it.
#define XW_SCROLL_RAD_S 0.35f      // radians per second at the default speed
static float s_xw_scroll  = 0.0f;
static u64   s_xw_last_us = 0;

static int xw_emit_sheets(float W, float H, int n) {
    {
        const u64 now = timing_get_us();
        float dt = s_xw_last_us ? (float)(now - s_xw_last_us) * 1e-6f : 0.0f;
        if (dt > 0.1f) dt = 0.1f;               // a stall must not jump it
        s_xw_last_us = now;
        xw_update_bands(dt);
        s_xw_scroll += dt * XW_SCROLL_RAD_S
                     * (s_jw_speed / ((float)JW_SPEED_DEF / 100.0f));
        if (s_xw_scroll > 6283.2f) s_xw_scroll -= 6283.2f;   // keep sinf exact
    }
    // Rows across a sheet, bottom edge to top edge: position t in [-1, 1],
    // and which way (if any) the row is pushed OUTSIDE the edge.  The two
    // outer rows sit XW_AA_PX beyond each edge at alpha 0, so the bright edge
    // line fades out over a couple of pixels instead of stopping on a
    // polygon boundary -- anti-aliasing in the geometry itself, which the RSX
    // and the emulator's rasteriser both draw the same way.  The edge rows
    // are close to their inner neighbours so the edge is a thin line, not a
    // wide ramp; the interior is faint.
    #define XW_ROWS  7
    #define XW_AA_PX 2.5f
    static const float ROW_T[XW_ROWS]   = { -1.0f, -1.0f, -0.86f, 0.0f, 0.86f, 1.0f, 1.0f };
    static const float ROW_OUT[XW_ROWS] = { -1.0f,  0.0f,  0.0f,  0.0f, 0.0f,  0.0f, 1.0f };
    // Colour moves between the theme's accent (Jellyfin purple) and
    // accent_alt (cyan) -- see the cycle below -- read per rebuild so a theme
    // change follows.  Edge lines are lifted towards white so they read
    // as light; the faint interior keeps the full tint.
    const u32   ca = XMB_ACCENT, cb2 = XMB_ACCENT_ALT;
    const float car = (float)((ca >> 16) & 0xFF), cag = (float)((ca >> 8) & 0xFF),
                cab = (float)(ca & 0xFF);
    const float cbr = (float)((cb2 >> 16) & 0xFF), cbg = (float)((cb2 >> 8) & 0xFF),
                cbb = (float)(cb2 & 0xFF);

    for (int k = 0; k < XW_SHEETS; k++) {
        const int slot = k >> 1, pass = k & 1;
        const int la = k % 3, lb = (k + 1) % 3;
        // Each sheet reads its own window of the field and its own phase.
        const float ua = 0.05f * (float)k;
        const float ub = 0.28f - 0.045f * (float)k;
        const float ph = 1.7f * (float)k;

        s_jw_off[slot][pass] = (u32)n;
        if (n + (XW_ROWS - 1) * (2 * XW_COLS + 2) > WAVE_MAX_VERTS) {
            s_jw_cnt[slot][pass] = 0;
            continue;
        }

        // Per column: centre and signed half-width, in pixels.
        float yc[XW_COLS], hw[XW_COLS], xs[XW_COLS];
        for (int c = 0; c < XW_COLS; c++) {
            const float xn = -0.02f + 1.04f * (float)c / (float)(XW_COLS - 1);
            xs[c] = xn;
            // Higher on the left, lower on the right, as the XMB's band runs.
            yc[c] = H * (0.60f + 0.14f * xn + 0.010f * ((float)k - 2.5f))
                  + H * 0.075f * xw_field(la, ua + 0.70f * xn);
            // Signed: where it passes through zero the sheet is edge-on.
            // Each sheet scrolls at its own rate so they drift past each other.
            const float sc = s_xw_scroll * (1.0f + 0.12f * (float)k);
            yc[c] += H * 0.035f * sinf(6.2832f * 0.8f * xn - sc + ph);
            hw[c] = H * 0.050f * (0.30f + 0.85f * xw_field(lb, ub + 0.70f * xn)
                                  + 0.35f * sinf(6.2832f * (1.1f * xn) - 0.8f * sc + ph));
            // Music: this sheet's own slice of the spectrum.  Its shape
            // deepens, it lifts through the middle with the ends anchored,
            // and it widens.
            const float lev = s_xw_lev[k];
            const float env = sinf(3.14159f * (xn < 0.0f ? 0.0f : xn > 1.0f ? 1.0f : xn));
            yc[c] += H * 0.075f * XW_MUSIC_SHAPE * lev * xw_field(la, ua + 0.70f * xn);
            yc[c] -= H * XW_MUSIC_LIFT * lev * env;
            hw[c] *= 1.0f + XW_MUSIC_SWELL * lev;
        }

        const int first = n;
        for (int r = 0; r < XW_ROWS - 1; r++) {
            if (r > 0) {
                // Degenerate join from the end of the last band to the
                // start of this one: zero-area triangles, drawn as nothing.
                s_jw_stage[n] = s_jw_stage[n - 1]; n++;
            }
            for (int c = 0; c < XW_COLS; c++) {
                // How edge-on the sheet is here: 1 when its edges meet.
                float thin = fabsf(hw[c]) / (H * 0.050f);
                thin = thin > 1.0f ? 0.0f : (1.0f - thin);
                thin *= thin;
                for (int e = 0; e < 2; e++) {
                    const int   ri = r + e;
                    const float t  = ROW_T[ri];
                    const float at = fabsf(t);
                    float a;
                    if (ROW_OUT[ri] != 0.0f) a = 0.0f;                  // AA fringe
                    else if (at >= 0.999f)   a = 90.0f;                 // edge line
                    else if (at > 0.5f)      a = 26.0f + 70.0f * thin;  // just inside
                    else                     a = 16.0f + 90.0f * thin;  // interior
                    a *= 1.0f + XW_MUSIC_GLOW * s_xw_lev[k];            // music
                    // Fade the sheet out over its last few pixels as it
                    // goes edge-on.  Thinner than a pixel, all its rows pile
                    // onto one pixel (a blown-out fleck) or fall between
                    // pixels (a gap) -- a speck-gap-speck at every twist.
                    {
                        float pin = fabsf(hw[c]) / 6.0f;
                        if (pin > 1.0f) pin = 1.0f;
                        a *= pin * pin * (3.0f - 2.0f * pin);
                    }
                    // Fade the ends so the band enters and leaves the screen
                    // softly instead of being cut by its edges.
                    const float xn = xs[c];
                    float endf = 1.0f;
                    if (xn < 0.06f)  endf = (xn + 0.02f) / 0.08f;
                    if (xn > 0.94f)  endf = (1.02f - xn) / 0.08f;
                    if (endf < 0.0f) endf = 0.0f;
                    a *= endf;
                    // Outward is the side the edge faces, which flips when
                    // the half-width goes negative through a twist.  Ramped,
                    // not a hard sign: flipping the fringe from one side to
                    // the other between two columns crossed its triangles
                    // into a bow-tie, a bright fleck at every twist.
                    float sgn = hw[c] / XW_AA_PX;
                    if (sgn >  1.0f) sgn =  1.0f;
                    if (sgn < -1.0f) sgn = -1.0f;
                    const float px = xn * W;
                    const float py = yc[c] + t * hw[c]
                                   + ROW_OUT[ri] * sgn * XW_AA_PX;
                    // One left-to-right gradient for the whole band --
                    // accent (purple) at the left, accent_alt (cyan) at the
                    // right, the same on every sheet -- that scrolls
                    // sideways.  Half a cosine cycle spans the screen, so it
                    // is a loop: what leaves one side comes back in without
                    // a jump.
                    const float hx = 0.5f - 0.5f * cosf(3.14159f * xn
                                                        - 0.6f * s_xw_scroll);
                    const float lift = (at >= 0.999f) ? 0.35f : 0.20f;
                    const float tr = car + (cbr - car) * hx;
                    const float tg = cag + (cbg - cag) * hx;
                    const float tb = cab + (cbb - cab) * hx;
                    WaveVert *v = &s_jw_stage[n];
                    v->x = (2.0f * px / W) - 1.0f;
                    v->y = 1.0f - (2.0f * py / H);
                    v->z = 0.0f;
                    v->w = 1.0f;
                    v->rgba = WAVE_RGBA((u8)(tr + (255.0f - tr) * lift),
                                        (u8)(tg + (255.0f - tg) * lift),
                                        (u8)(tb + (255.0f - tb) * lift),
                                        (u8)(a > 255.0f ? 255.0f : a));
                    if (r > 0 && c == 0 && e == 0) {
                        // Second half of the join: repeat the band's start.
                        s_jw_stage[n + 1] = *v;
                        n++;
                    }
                    n++;
                }
            }
        }
        s_jw_cnt[slot][pass] = (u32)(n - first);
    }
    #undef XW_ROWS
    #undef XW_AA_PX
    return n;
}

// The emulator's JellyWave background, cached in main memory.  s_cpu_grad is
// the dithered gradient alone, redrawn only when the theme's corners or the
// display size change; s_cpu_bgc is gradient + wave, redrawn only when the
// wave geometry is rebuilt (every s_jw_rebuild_every calls).  Every other call
// is one copy into the framebuffer.  Without this the emulator spent ~60 ms a
// call redrawing an image that had not changed.
static u32    *s_cpu_grad = NULL;
static u32    *s_cpu_bgc  = NULL;
static int     s_cpu_w = 0, s_cpu_h = 0;
static bg_quad s_cpu_key;
static bool    s_cpu_grad_ok = false;

// Make sure both caches exist at W x H.  False if they could not be had, in
// which case the caller draws straight into the framebuffer as before.
static bool jw_cpu_cache(int W, int H) {
    if (s_cpu_grad && s_cpu_w == W && s_cpu_h == H) return true;
    free(s_cpu_grad); free(s_cpu_bgc);
    s_cpu_grad = (u32 *)malloc((size_t)W * H * sizeof(u32));
    s_cpu_bgc  = (u32 *)malloc((size_t)W * H * sizeof(u32));
    s_cpu_grad_ok = false;
    if (!s_cpu_grad || !s_cpu_bgc) {
        free(s_cpu_grad); free(s_cpu_bgc);
        s_cpu_grad = s_cpu_bgc = NULL;
        s_cpu_w = s_cpu_h = 0;
        return false;
    }
    s_cpu_w = W; s_cpu_h = H;
    return true;
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

    wave_bg_refresh();
    const u64 cpu_t0 = timing_get_us();

    // JellyWave draws the gradient into its cache, and only when it changed.
    const bool cached = s_wave_jelly && jw_cpu_cache(W, H);
    u32 *gdst = cached ? s_cpu_grad : fb;
    bool grad_changed = true;
    if (cached) {
        grad_changed = !s_cpu_grad_ok ||
                       memcmp(&s_cpu_key, &s_bg, sizeof(s_bg)) != 0;
        s_cpu_key     = s_bg;
        s_cpu_grad_ok = true;
    }

    // Four-corner gradient with an ordered dither, per pixel.
    //
    // WHAT THIS REPLACED, AND WHY.  It used to be one grad_sample() per row
    // and a flat fill across it.  That was fast, and it was also the worst
    // possible shape for the artefact: with one colour per row, every 8-bit
    // quantisation boundary in a smooth full-screen ramp becomes a
    // dead-straight horizontal line all the way across the screen.  The
    // reference dithers here too -- its HDR block carries DITHER = 1/255,
    // exactly one output code.
    //
    // THE COST IS THREE ADDS A PIXEL, not a bilinear evaluation.  For a fixed
    // row the field is affine in u, so the two edge colours are the only
    // samples needed and everything between them is a running sum.  The dither
    // itself is one table lookup and one compare per channel.
    //
    // This path runs only under BUILD_FOR_RPCS3 (see ui_cpu_bg), where the
    // framebuffer is ordinary host memory -- it is not on the hardware frame
    // budget, where the RSX draws the same gradient as a gouraud quad.
    for (int y = 0; grad_changed && y < H; y++) {
        const float v = (H > 1) ? ((float)y / (float)(H - 1)) : 0.0f;
        float lr, lg, lb, rr, rg, rb, dr, dg, db;
        u32 *row = gdst + (u32)y * W;

        bg_sample_f(&s_bg, 0.0f, v, &lr, &lg, &lb);
        bg_sample_f(&s_bg, 1.0f, v, &rr, &rg, &rb);
        dr = (W > 1) ? (rr - lr) / (float)(W - 1) : 0.0f;
        dg = (W > 1) ? (rg - lg) / (float)(W - 1) : 0.0f;
        db = (W > 1) ? (rb - lb) / (float)(W - 1) : 0.0f;

        for (int x = 0; x < W; x++) {
            row[x] = ((u32)bg_dither_channel(lr, x, y) << 16)
                   | ((u32)bg_dither_channel(lg, x, y) <<  8)
                   |  (u32)bg_dither_channel(lb, x, y);
            lr += dr; lg += dg; lb += db;
        }
    }

    // Advance the animation exactly like the GPU path.
    {
        float ts, pert, drv;
        wave_audio_frame(&ts, &pert, &drv);
        wf_step(&s_field,
                WAVE_FIELD_DT * ts * (s_wave_jelly ? s_jw_speed : 1.0f),
                pert, drv);
    }

    if (s_wave_jelly) {
        // Same rebuild cadence as the RSX path: resample the geometry every
        // s_jw_rebuild_every calls and redraw the last build in between.
        bool rebuild = true;
        // The sheets are cheap to build and carry the music, so they
        // rebuild every call; the cadence is for JellyWave's gel geometry.
        if (!s_wave_sheets && s_jw_rebuild_every > 1 && s_jw_have_geom) {
            rebuild = (s_jw_rebuild_phase == 0);
            if (++s_jw_rebuild_phase >= s_jw_rebuild_every)
                s_jw_rebuild_phase = 0;
        }
        if (rebuild) {
            s_jw_verts     = (u32)(s_wave_sheets
                           ? xw_emit_sheets((float)W, (float)H, 0)
                           : jw_emit_stream((float)W, (float)H, 0));
            s_jw_have_geom = 1;
        }
        const u64 cpu_t1 = timing_get_us();
        if (!cached) {
            jw_raster_stream(fb, W, H);
        } else {
            if (rebuild || grad_changed) {
                memcpy(s_cpu_bgc, s_cpu_grad, (size_t)W * H * sizeof(u32));
                jw_raster_stream(s_cpu_bgc, W, H);
            }
            memcpy(fb, s_cpu_bgc, (size_t)W * H * sizeof(u32));
        }
        {
            static u64 grad_us = 0, jw_us = 0, last_log = 0;
            static u32 calls = 0;
            const u64 now = timing_get_us();
            grad_us += cpu_t1 - cpu_t0;
            jw_us   += now - cpu_t1;
            calls++;
            if (now - last_log >= 1000000ULL) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "wave_cpu: grad+build=%lluus raster=%lluus per call (%u calls)",
                         (unsigned long long)(grad_us / calls),
                         (unsigned long long)(jw_us / calls), calls);
                plog(msg);
                grad_us = jw_us = 0; calls = 0; last_log = now;
            }
        }
        return;
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
    // The column's horizontal position, which the background now depends on:
    // colx[] is in pixels and the sampler wants [0,1].  W is never zero here --
    // wave_draw() returns early on a degenerate display size before building
    // colx at all.
    wave_bg(li, ci, colx[ci] / W, y, H, crest, &br_, &bg_, &bb_);
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
    const bool jellywave = s_wave_jelly && !strobe_test_disable_jellywave();

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

    // The RSX's own dither unit, which is what the CPU path emulates in
    // software (see wave_draw_cpu).  The gradient quad is the one thing this
    // client draws that is large, smooth and low-contrast enough to band, and
    // the hardware will dither the gouraud interpolator's output into the
    // framebuffer for free if it is asked to.
    //
    // GATED, because it is a global piece of RSX state and everything the UI
    // draws afterwards inherits it -- text and card blits included, where a
    // dither is not wanted.  It is therefore turned off again below, before
    // this function returns, rather than left on.  Delete
    // jellyfin_bgdither.txt to get the previous behaviour back with no
    // reflash, in the same spirit as the wave's own gate.  Read once at
    // startup, not per frame -- gpuwave_mode() is handled the same way and for
    // the same reason: this runs six times a frame across the XMB.
    if (s_bg_dither) rsxSetDitherEnable(context, GCM_TRUE);

    float W = (float)display_width;
    float H = (float)display_height;

    // The background's four corners, read fresh from the theme every frame.
    wave_bg_refresh();

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
    //
    // s_jw_speed applies ONLY in mode 3 and is exactly 1.0 everywhere else, so
    // the legacy modes keep the drift rate WAVE_FIELD_DT was calibrated for.
    // See the JW_SPEED block for why JellyWave wants its own.
    //    // The solver still steps on EVERY call even when the geometry is rebuilt
    // less often.  That is deliberate: the chain stays continuous and the
    // audio analyser keeps its cadence; only the SAMPLING of the curve drops
    // to the rebuild rate.  Stepping it in bigger jumps instead would change
    // the physics rather than the sampling.
    {
        float ts, pert, drv;
        wave_audio_frame(&ts, &pert, &drv);
        wf_step(&s_field,
                WAVE_FIELD_DT * ts * (jellywave ? s_jw_speed : 1.0f),
                pert, drv);
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
    //
    // JellyWave does not use crests at all -- it lofts a 3-D section along the
    // spine instead of stroking a 2-D curve -- so this is skipped there.  It
    // is 294 wave_field_px() calls a frame at 1080p, each carrying a divide,
    // and computing them for a path that will not read them is the kind of
    // cost that is invisible in a profile because it is spread over three
    // loops.
    static float crest[3][WAVE_MAX_COLS];
    if (!jellywave) {
        for (int li = 0; li < 3; li++)
            for (int ci = 0; ci < ncols; ci++)
                crest[li][ci] = wave_crest(li, colx[ci], W, H);
    }

    // The background quad's four corners.
    //
    // This used to be a top colour and a bottom colour, each written to two
    // vertices.  It is four independent corners now, which costs the RSX
    // nothing whatsoever -- the quad already emitted four vertices and the
    // gouraud interpolator already had to interpolate between them; it was
    // simply being handed the same colour twice.  With a two-stop theme
    // bg_from_two() puts the old values back in that arrangement and the
    // output is unchanged.
    const u8 gtlr=(s_bg.c[BG_TL]>>16)&0xFF, gtlg=(s_bg.c[BG_TL]>>8)&0xFF, gtlb=s_bg.c[BG_TL]&0xFF;
    const u8 gtrr=(s_bg.c[BG_TR]>>16)&0xFF, gtrg=(s_bg.c[BG_TR]>>8)&0xFF, gtrb=s_bg.c[BG_TR]&0xFF;
    const u8 gblr=(s_bg.c[BG_BL]>>16)&0xFF, gblg=(s_bg.c[BG_BL]>>8)&0xFF, gblb=s_bg.c[BG_BL]&0xFF;
    const u8 gbrr=(s_bg.c[BG_BR]>>16)&0xFF, gbrg=(s_bg.c[BG_BR]>>8)&0xFF, gbrb=s_bg.c[BG_BR]&0xFF;

    // Slice k spans the fraction [k/NS, (k+1)/NS] of the distance from this
    // column's crest down to the screen bottom.  One triangle strip per
    // (ribbon, slice), back-to-front, fully opaque.
    if (s_wave_varray) {
        // Rebuild, or re-draw what is already in the buffer?  Only JellyWave
        // has a cadence; every other mode rebuilds on every call exactly as it
        // always did.  The first call after init must build whatever the
        // cadence says, or the first frames would draw an empty buffer.
        bool jw_rebuild = true;
        // Sheets rebuild every call (cheap, and they carry the music).
        if (jellywave && !s_wave_sheets && s_jw_rebuild_every > 1 && s_jw_have_geom) {
            jw_rebuild = (s_jw_rebuild_phase == 0);
            if (++s_jw_rebuild_phase >= s_jw_rebuild_every)
                s_jw_rebuild_phase = 0;
        }

        // The RSX consumes vertex arrays asynchronously.  The two-buffer
        // rotation is normally enough, but JellyWave can leave a buffer queued
        // for several frames while the PPU rebuilds the other one.  If the RSX
        // falls more than one buffer behind, rotating back after only three
        // wave_draw calls can overwrite a buffer that is still being fetched.
        //
        // DIAGNOSTIC SAFETY FENCE: before reusing a JellyWave buffer, wait until
        // all previously queued RSX work has consumed the old buffer.  This is
        // intentionally conservative.  If this removes the real-PS3 strobe,
        // the next optimization is a per-buffer fence rather than removing the
        // safety entirely.
        if (jw_rebuild) {
            if (jellywave && s_jw_have_geom)
                rsxSync();
        }

        WaveVert *v  = jellywave
                     ? s_jw_stage
                     : s_wave_vbuf[s_wave_vbuf_turn];
        u32       vo = s_wave_vbuf_off[s_wave_vbuf_turn];
        int       n  = 0;

        // Gradient quad first, so it occupies vertices [0,4).
        #define WV_PUT(px, py, pr, pg, pb, pa) do {             \
            v[n].x = (px); v[n].y = (py); v[n].z = 0.0f;        \
            v[n].w = 1.0f;                                      \
            v[n].rgba = WAVE_RGBA((pr), (pg), (pb), (pa));      \
            n++;                                                \
        } while (0)

        // The gradient shares the buffer with the ribbons, so on a reuse frame
        // it is held along with them.  That is what costs a theme change up to
        // one rebuild interval to appear -- 50 ms at the default cadence.
        if (jw_rebuild) {
            WV_PUT(-1.0f,  1.0f, gtlr, gtlg, gtlb, 255);   // top-left
            WV_PUT(-1.0f, -1.0f, gblr, gblg, gblb, 255);   // bottom-left
            WV_PUT( 1.0f,  1.0f, gtrr, gtrg, gtrb, 255);   // top-right
            WV_PUT( 1.0f, -1.0f, gbrr, gbrg, gbrb, 255);   // bottom-right
        }

        if (jellywave && jw_rebuild) {
            // STROBE ISOLATION TEST 4: build the real JellyWave geometry,
            // but the submission below will draw only the first body strip
            // of the furthest layer. This isolates basic JellyWave geometry
            // from the merged multi-strip/degen-join path.
            u64 jw_t0 = timing_get_us();

            /*
             * Build the COMPLETE JellyWave stream in main memory.
             *
             * Nothing below reads from the RSX-local destination buffer.
             * This is deliberately separated from the upload step below.
             */
            n = s_wave_sheets ? xw_emit_sheets(W, H, n)
                              : jw_emit_stream(W, H, n);


            s_jw_gen_us += timing_get_us() - jw_t0;
            s_jw_rebuilds++;
            s_jw_verts = (u32)n;
            s_jw_have_geom = 1;
        } else if (s_wave_blend) {
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

        /*
         * JellyWave is always uploaded from main-memory staging into the
         * alternate RSX buffer.  The full RSX fence happens BEFORE reusing
         * the destination buffer; the upload itself is write-only.
         *
         * This deliberately happens on reuse calls too.  With two buffers,
         * the destination is the buffer the RSX used on the previous call,
         * so the fence makes the ownership transition explicit.
         */
        if (jellywave) {
            u64 jw_up0 = timing_get_us();

            /*
             * DIAGNOSTIC: the destination RSX buffer must no longer be
             * in use before the PPU overwrites it.  The sync inside
             * jw_upload() only orders PPU stores; it does NOT wait for
             * the RSX to finish consuming the previous contents.
             */
            rsxSync();

            s_wave_vbuf_turn ^= 1;

            v  = s_wave_vbuf[s_wave_vbuf_turn];
            vo = s_wave_vbuf_off[s_wave_vbuf_turn];

            // Gradient at [0,4), the ribbons at JW_VBASE (see its note).
            jw_upload(v, s_jw_stage, 4);
            if (s_jw_verts > 4)
                jw_upload(v + JW_VBASE, s_jw_stage + 4, s_jw_verts - 4);
            s_jw_upload_us += timing_get_us() - jw_up0;
        }

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
        //
        // Skipped on a JellyWave REUSE call only, and only because nothing was
        // written: there are no stores in the gather buffer to drain, and the
        // barrier that made those stores visible ran on the build that put the
        // geometry there.  Every call that writes a single vertex still takes
        // it.
        if (jw_rebuild && !jellywave)
            __asm__ __volatile__ ("sync" ::: "memory");

        // The gradient is the first four vertices in the same reusable
        // array as the JellyWave geometry. Keep it on the array-fetch path:
        // the previous frame leaves POS array-bound, so submitting this quad
        // with rsxDrawVertex* would mix immediate vertices with a live POS
        // binding. That can make the first primitive fetch stale data and
        // manifest as a black/colour-strobing frame on real hardware.
        //
        // Drawing the gradient from the same array also makes rebuild and
        // reuse frames identical: on a reuse frame vertices [0,4) are already
        // present in the buffer, so no CPU writes or extra synchronization are
        // needed.
        //
        // Bind POS/COLOR first, then draw the gradient while blending is still
        // disabled. TEX0 is explicitly disabled rather than inherited.
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS, 0,
            vo, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_COLOR0, 0,
            vo + 16, (u8)sizeof(WaveVert), 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0,
            0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

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

        if (jellywave) {
            // Explicitly establish JellyWave's hardware-validated standard
            // alpha blend state. Do not inherit whatever state the previous
            // UI/background operation left behind.
            rsxSetBlendFunc(context,
                GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
            rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
            rsxSetBlendEnable(context, GCM_TRUE);

            // Draw the finished JellyWave stream uploaded from CPU staging.
            //
            // IMPORTANT: s_jw_off/s_jw_cnt describe the CPU-built stream;
            // the RSX buffer is write-only from the PPU. Never read it back.
            rsxInvalidateVertexCache(context);
            s_jw_draws = 1;   // gradient plus the body/rim strips sent below

            for (int slot = 0; slot < JW_LAYERS; slot++) {
                for (int pass = 0; pass < 2; pass++) {
                    if ((pass == 0 && strobe_test_disable_jellywave_body()) ||
                        (pass == 1 && strobe_test_disable_jellywave_rim()))
                        continue;
                    const u32 count = s_jw_cnt[slot][pass];
                    if (!count)
                        continue;

                    // Body: ordinary src-alpha blend.  Rim: ADDITIVE, src*a +
                    // dst -- rimColor is a highlight that is black away from
                    // the rolled edge, so an opaque blend (as 14428bb tried
                    // while chasing the strobe) paints dark bands over the
                    // ribbons instead of a glossy edge.
                    rsxSetBlendFunc(context,
                        GCM_SRC_ALPHA,
                        (pass || s_wave_sheets) ? GCM_ONE : GCM_ONE_MINUS_SRC_ALPHA,
                        GCM_SRC_ALPHA,
                        (pass || s_wave_sheets) ? GCM_ONE : GCM_ONE_MINUS_SRC_ALPHA);
                    rsxDrawVertexArray(context,
                        GCM_TYPE_TRIANGLE_STRIP,
                        s_jw_off[slot][pass] - 4 + JW_VBASE,
                        count);
                    s_jw_draws++;
                }
            }
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
        wave_vtx(-1.0f,  1.0f, gtlr, gtlg, gtlb);   // top-left
        wave_vtx(-1.0f, -1.0f, gblr, gblg, gblb);   // bottom-left
        wave_vtx( 1.0f,  1.0f, gtrr, gtrg, gtrb);   // top-right
        wave_vtx( 1.0f, -1.0f, gbrr, gbrg, gbrb);   // bottom-right
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

    // And put the dither back the way everything downstream expects to find
    // it.  Leaving it on would silently apply it to every card blit, glyph and
    // chrome quad drawn after the background -- the same class of mistake as
    // leaving a vertex-array binding live, which is what the "unreliable
    // fetch" folklore in this file actually was.
    if (s_bg_dither) rsxSetDitherEnable(context, GCM_FALSE);

    // JellyWave's own cost line, once a second.
    //
    // WHAT THIS CAN AND CANNOT TELL YOU.  `gen` is real: the PPU microseconds
    // spent lofting, lighting and projecting the three layers, measured around
    // the build loop.  `verts` and `draws` are exact.  THE RSX's OWN COST IS
    // NOT HERE and cannot be -- the xmb: frame line's `gpu` bucket measures
    // submission, not rasterisation, and this client has no GPU timer.  Read
    // the RSX side from `vsync` in that line instead: vsync is the part of the
    // 60 Hz budget the frame did NOT use, so if JellyWave costs the GPU real
    // time, vsync shrinks by it.  Compare a mode 2 capture against a mode 3
    // one on the same screen.
    //
    // COUNTED PER CALL, NOT PER FRAME.  wave_draw() has six call sites and the
    // screens that composite the background twice call it twice, exactly as
    // they always did; `gen` is therefore the cost of ONE build, and a screen
    // that draws the background twice pays it twice.  That is the same
    // behaviour the legacy path has -- it rebuilds every vertex on each call
    // too -- but it is worth knowing when reading the number against a frame
    // budget.
    if (jellywave && ++s_jw_frames >= 60) {
        char msg[192];
        u32  nb = s_jw_rebuilds ? s_jw_rebuilds : 1;
        snprintf(msg, sizeof(msg),
                 "jellywave: gen=%lluus/build amort=%lluus/call "
                 "up=%lluus/call verts=%u draws=%u rebuild=1/%d "
                 "speed=%d%% repaired=%u dropped=%u (%d calls, %d builds)",
                 (unsigned long long)(s_jw_gen_us / nb),
                 (unsigned long long)(s_jw_gen_us / s_jw_frames),
                 (unsigned long long)(s_jw_upload_us / s_jw_frames),
                 s_jw_verts, s_jw_draws, s_jw_rebuild_every,
                 (int)(s_jw_speed * 100.0f + 0.5f),
                 (unsigned)s_jw_repaired, (unsigned)s_jw_dropped,
                 (int)s_jw_frames, (int)s_jw_rebuilds);
        plog(msg);
        s_jw_gen_us     = 0;
        s_jw_upload_us  = 0;
        s_jw_frames     = 0;
        s_jw_rebuilds   = 0;
    }
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

// A 12-segment radial fan (centre plus a closed 12-point ring): 14 vertices
// total.  The perimeter alpha is zero, so the RSX performs the falloff without
// reading or blending pixels on the PPU.
void wave_draw_glow_gpu(int cx_px, int cy_px, int radius_px,
                        u8 r, u8 g, u8 b, u8 peak_alpha) {
    static const float ring[13][2] = {
        { 1.000000f,  0.000000f }, { 0.866025f,  0.500000f },
        { 0.500000f,  0.866025f }, { 0.000000f,  1.000000f },
        {-0.500000f,  0.866025f }, {-0.866025f,  0.500000f },
        {-1.000000f,  0.000000f }, {-0.866025f, -0.500000f },
        {-0.500000f, -0.866025f }, { 0.000000f, -1.000000f },
        { 0.500000f, -0.866025f }, { 0.866025f, -0.500000f },
        { 1.000000f,  0.000000f },
    };

    if (!s_wave_fp_buf || radius_px <= 0 || !peak_alpha) return;

    rsxVertexProgram   *vpo = (rsxVertexProgram*)wave_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram*)wave_fp_data;
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

    const float W = (float)display_width;
    const float H = (float)display_height;
    #define GV(px, py, pa) do {                                        \
        const u8 c4[4] = { r, g, b, (pa) };                            \
        const float p4[4] = { (2.0f * (px) / W) - 1.0f,                \
                               1.0f - (2.0f * (py) / H), 0.0f, 1.0f }; \
        rsxDrawVertex4ub(context, GCM_VERTEX_ATTRIB_COLOR0, c4);       \
        rsxDrawVertex4f(context, GCM_VERTEX_ATTRIB_POS, p4);           \
    } while (0)

    rsxDrawVertexBegin(context, GCM_TYPE_TRIANGLE_FAN);
    GV((float)cx_px, (float)cy_px, peak_alpha);
    for (int i = 0; i < 13; i++)
        GV((float)cx_px + ring[i][0] * radius_px,
           (float)cy_px + ring[i][1] * radius_px, 0);
    rsxDrawVertexEnd(context);
    #undef GV
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