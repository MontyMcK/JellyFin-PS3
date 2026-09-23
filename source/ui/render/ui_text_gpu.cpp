// GPU text runs -- see ui_text_gpu.h for the measurement that motivates this.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

#include "rsxutil.h"
#include "video_shaders.h"     // video_vp_data / video_fp_data (passthrough)
#include "ui.h"                // cpu_rt_on, g_cpu_clip_top/bot
#include "ui_text_gpu.h"
#include "plog.h"
#include "jf_paths.h"
#include "ui_wave.h"          // ui_cpu_bg

extern void crash_log(const char *msg);

// --- sizing ---------------------------------------------------------------
//
// ATLAS: a run at 22 px is roughly 190x26 -- 832-byte pitch x 26 rows, 21 KB.
// A busy screen is 40-60 distinct runs, so 4 MB holds about 200 of them: every
// run on screen, plus the colour variants the selection produces as it moves,
// with room to spare.  When it fills, the whole cache is dropped in one go
// rather than evicted entry by entry; that costs exactly one frame of the work
// this path replaces, which is what the frame used to cost every frame.
//
// RUN_MAX_W/H bound the staging buffer.  Anything larger falls back to the
// per-glyph CPU path, so an unexpectedly huge string degrades to today's
// behaviour instead of failing.
#define ATLAS_BYTES   (4u * 1024u * 1024u)
#define RUN_SLOTS     512            // power of two, open-addressed
#define RUN_KEY_MAX   120            // longer strings use the CPU path
#define RUN_MAX_W     1920
#define RUN_MAX_H     96             // biggest UI face is ~30 px; 96 is slack
#define QUEUE_MAX     512

// Texture pitch must be 64-byte aligned -- same rule the thumbnail VRAM
// mirrors follow (thumbnail_cache.cpp), and the same reason: RSX linear
// textures have no sub-64-byte pitch.
#define VRAM_PITCH(w) ((((u32)(w) * 4u) + 63u) & ~63u)

typedef struct {
    u32   hash;
    float px;
    u32   colour;
    u32   off;                 // RSX offset of this run's texture
    u32   pitch;               // bytes per row, 64-byte aligned
    s16   w, h, ox, oy;
    u8    face;
    u8    len;
    bool  used;
    char  key[RUN_KEY_MAX];
} RunSlot;

typedef struct {
    u16 slot;
    s16 x, y;                  // top-left of the ink, framebuffer pixels
    s16 clip_top, clip_bot;    // g_cpu_clip_* at the time it was queued
} RunDraw;

static RunSlot  s_runs[RUN_SLOTS];
static u32      s_run_count = 0;

static u8      *s_atlas      = NULL;   // rsxMemalign'd VRAM
static u32      s_atlas_off  = 0;
static u32      s_atlas_used = 0;
// A queued draw holds only a RunSlot index, and that slot's texture points
// into this atlas.  Resetting either while a frame still has queued draws
// would make an earlier label disappear or (worse) draw a later label at its
// coordinates.  Defer a needed reset until the next collection window, when
// the prior frame has been submitted.
static bool     s_reset_pending = false;

static u32     *s_stage      = NULL;   // main-memory rasterization scratch
static RunDraw  s_queue[QUEUE_MAX];
static u32      s_queued     = 0;

static bool     s_ready      = false;
static bool     s_window     = false;  // inside begin()/flush()
static u32      s_fp_off     = 0;
static u32     *s_fp_buf     = NULL;

static u32 s_st_runs, s_st_misses, s_st_bytes, s_st_flushes;

// --- the gate -------------------------------------------------------------
//
// Gated, for the same reason ui_card_gpu.cpp is: this binds RSX textures
// from a part of the app that historically bound none, and a bad bind on this
// hardware does not fail politely -- it wedges the GPU, takes the console off
// the network and needs a power cycle.  On by default now that the new UI is
// built on it; put "0" in /dev_hdd0/tmp/jellyfin_gputext.txt to go back.
#define GPUTEXT_FILE "jellyfin_gputext.txt"
static bool gputext_enabled(void)
{
    // RPCS3 drops CPU writes into an RSX-owned frame, so the emulator keeps
    // the whole frame on the CPU (see ui_cpu_bg in ui_wave.cpp).
    if (ui_cpu_bg()) return false;
    // Absent = on; write 0 to the file to fall back to the CPU path.
    FILE *f = fopen(jf_data_path(GPUTEXT_FILE), "r");
    if (!f) return true;
    int v = 1;
    bool on = (fscanf(f, "%d", &v) != 1 || v == 1);
    fclose(f);
    return on;
}

void ui_text_gpu_init(void)
{
    if (s_ready) return;
    if (!gputext_enabled()) {
        plog("text_gpu: disabled (jellyfin_gputext.txt = 0) -- CPU glyph blits");
        crash_log("text_gpu: OFF (CPU glyphs)");
        return;
    }

    s_stage = (u32 *)malloc((size_t)RUN_MAX_W * RUN_MAX_H * 4u);
    if (!s_stage) {
        plog("text_gpu: staging alloc FAILED -- staying on the CPU path");
        return;
    }

    s_atlas = (u8 *)rsxMemalign(128, ATLAS_BYTES);
    if (!s_atlas) {
        free(s_stage); s_stage = NULL;
        plog("text_gpu: VRAM atlas alloc FAILED -- staying on the CPU path");
        return;
    }
    rsxAddressToOffset(s_atlas, &s_atlas_off);

    // Private copy of the passthrough fragment program, so this does not
    // depend on when the video path happens to have uploaded its own.
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)video_fp_data;
    void *ucode; u32 size;
    rsxFragmentProgramGetUCode(fpo, &ucode, &size);
    s_fp_buf = (u32 *)rsxMemalign(256, size);
    if (!s_fp_buf) {
        rsxFree(s_atlas); s_atlas = NULL;
        free(s_stage);    s_stage = NULL;
        plog("text_gpu: fragment program alloc FAILED -- staying on the CPU path");
        return;
    }
    memcpy(s_fp_buf, ucode, size);
    rsxAddressToOffset(s_fp_buf, &s_fp_off);

    s_ready = true;
    {
        char line[96];
        snprintf(line, sizeof(line),
                 "text_gpu: ready -- %u KB atlas, text runs go through the RSX",
                 (unsigned)(ATLAS_BYTES / 1024u));
        plog(line);
    }
    // crash_log is synchronous and survives a power cycle; plog's buffered
    // tail is the first thing lost if a bad bind takes the console down.
    crash_log("text_gpu: ON (RSX run textures)");
}

bool ui_text_gpu_ready(void) { return s_ready; }

void ui_text_gpu_stats_reset(void)
{
    s_st_runs = s_st_misses = s_st_bytes = s_st_flushes = 0;
}

void ui_text_gpu_stats_get(u32 *runs, u32 *misses, u32 *bytes, u32 *flushes)
{
    if (runs)    *runs    = s_st_runs;
    if (misses)  *misses  = s_st_misses;
    if (bytes)   *bytes   = s_st_bytes;
    if (flushes) *flushes = s_st_flushes;
}

// --- run cache ------------------------------------------------------------

static u32 run_hash(const char *text, int len, float px, int face, u32 colour)
{
    u32 h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (u8)text[i]; h *= 16777619u; }
    h ^= (u32)(px * 4.0f + 0.5f) * 40503u;
    h ^= (u32)face * 2246822519u;
    h ^= colour * 2654435761u;
    h ^= h >> 15;
    return h;
}

static void runs_flush_all(void)
{
    memset(s_runs, 0, sizeof(s_runs));
    s_run_count  = 0;
    s_atlas_used = 0;
    s_reset_pending = false;
    plog("text_gpu: run cache flushed (atlas full)");
}

// Copy a rasterized run from main-memory staging into the VRAM atlas.
//
// Rasterizing straight into video memory would be exactly the mistake this
// whole path exists to avoid: glyph compositing is scattered read-modify-write
// and VRAM reads are ~100x slower than writes.  Staging in main memory keeps
// the RMW where it is cheap and turns the VRAM traffic into one sequential
// write per row, which is the case write-gathering handles well (767 MB/s
// measured, faster than main->main memcpy).
static bool atlas_upload(const u32 *src, int w, int h, u32 *out_off, u32 *out_pitch)
{
    const u32 pitch = VRAM_PITCH(w);
    const u32 need  = pitch * (u32)h;
    const u32 padded_need = (need + 127u) & ~127u;

    if (padded_need > ATLAS_BYTES) return false;        // absurd: use the CPU path
    if (s_atlas_used + padded_need > ATLAS_BYTES) {
        if (s_queued) {
            // The queue still references this generation of the atlas.  Let
            // this one run use the established CPU fallback and reset before
            // the next frame instead of invalidating earlier queued draws.
            s_reset_pending = true;
            return false;
        }
        runs_flush_all();
    }

    u8 *dst = s_atlas + s_atlas_used;
    for (int row = 0; row < h; row++)
        memcpy(dst + (size_t)row * pitch, src + (size_t)row * w, (size_t)w * 4u);

    *out_off   = s_atlas_off + s_atlas_used;
    *out_pitch = pitch;
    s_atlas_used += padded_need;                        // keep offsets aligned
    s_st_bytes   += need;
    return true;
}

// Face id for icon entries.  Out of band with the real faces (0..4) so the
// existing hash and the memcmp on `key` separate icons from text with no
// change to either.
#define RUN_FACE_ICON 0xEE

// Find, or build, the cached run for this exact (text, px, face, colour).
//
// `icon_cp` >= 0 selects the icon seam instead of the text one: the key bytes
// are then the codepoint rather than a string, and measurement/rasterization
// go through ttf_icon_box()/ttf_icon_raster(), which skip the gamma bake.
static const RunSlot *run_get(const char *text, int len, float px, int face,
                              u32 colour, int icon_cp)
{
    const u32 mask = RUN_SLOTS - 1;
    const u32 hash = run_hash(text, len, px, face, colour);
    u32 i = hash & mask;

    for (u32 p = 0; p < RUN_SLOTS; p++, i = (i + 1) & mask) {
        RunSlot *s = &s_runs[i];
        if (s->used) {
            if (s->hash == hash && s->len == (u8)len && s->px == px &&
                s->face == (u8)face && s->colour == colour &&
                memcmp(s->key, text, (size_t)len) == 0)
                return s;
            continue;                                   // collision: keep probing
        }

        // Miss.  Measure, rasterize into staging, upload.
        TtfRunBox box;
        if (icon_cp >= 0) {
            if (!ttf_icon_box(icon_cp, px, &box)) return NULL;  // no ink
        } else {
            if (!ttf_run_box(text, px, face, &box)) return NULL;
        }
        if (box.w <= 0 || box.h <= 0)            return NULL;
        if (box.w > RUN_MAX_W || box.h > RUN_MAX_H) return NULL;

        // A dense table probes badly; drop it before it gets there, the same
        // way the glyph cache does.
        if ((s_run_count + 1) * 10 > RUN_SLOTS * 7) {
            if (s_queued) {
                // See atlas_upload(): table and atlas form one cache
                // generation, so neither may be reset mid-queue.
                s_reset_pending = true;
                return NULL;
            }
            runs_flush_all();
            i = hash & mask;
            s = &s_runs[i];
        }

        if (icon_cp >= 0) ttf_icon_raster(icon_cp, px, colour, s_stage, &box);
        else              ttf_run_raster(text, px, face, colour, s_stage, &box);

        u32 off, pitch;
        if (!atlas_upload(s_stage, box.w, box.h, &off, &pitch)) return NULL;

        // runs_flush_all() inside atlas_upload() may have just emptied the
        // table, so re-derive the slot rather than trusting the one above.
        i = hash & mask;
        while (s_runs[i].used) i = (i + 1) & mask;
        s = &s_runs[i];

        s->hash   = hash;
        s->px     = px;
        s->colour = colour;
        s->off    = off;
        s->pitch  = pitch;
        s->w      = (s16)box.w;  s->h  = (s16)box.h;
        s->ox     = (s16)box.ox; s->oy = (s16)box.oy;
        s->face   = (u8)face;
        s->len    = (u8)len;
        s->used   = true;
        memcpy(s->key, text, (size_t)len);
        s_run_count++;
        s_st_misses++;
        return s;
    }
    return NULL;   // table full even after a flush -- cannot happen in practice
}

// --- the collecting window ------------------------------------------------

void ui_text_gpu_begin(void)
{
    if (!s_ready) return;
    // Drop the queue FIRST, then recycle: once nothing references a slot the
    // reset is unconditionally safe, and the previous frame's quads are long
    // since consumed -- flip() queued the buffer swap behind them and
    // waitflip() plus rsxSync() have both been through since.
    s_queued = 0;
    if (s_reset_pending) runs_flush_all();
    s_window = true;
}

// Append one resolved slot to the frame's draw queue.  Shared by the text and
// icon entry points so they cannot drift in how they anchor the quad.
static void queue_run(const RunSlot *r, u32 x, u32 y)
{
    RunDraw *d = &s_queue[s_queued++];
    d->slot     = (u16)(r - s_runs);
    d->x        = (s16)((int)x + r->ox);
    d->y        = (s16)((int)y + r->oy);
    d->clip_top = (s16)g_cpu_clip_top;
    d->clip_bot = (s16)g_cpu_clip_bot;
    s_st_runs++;
}

bool ui_text_gpu_run(u32 x, u32 y, const char *text, float px, u32 colour,
                     int face)
{
    if (!s_ready || !s_window) return false;
    // While a CPU compose target is bound (the player HUD's overlay), drawing
    // must land in that main-memory buffer, not on the screen.
    if (cpu_rt_on()) return false;
    if (s_queued >= QUEUE_MAX) return false;
    if (!text || !*text) return true;          // nothing to draw either way

    size_t len = strlen(text);
    if (len == 0 || len > RUN_KEY_MAX) return false;

    const RunSlot *r = run_get(text, (int)len, px, face, colour, -1);
    if (!r) return false;

    queue_run(r, x, y);
    return true;
}

bool ui_text_gpu_icon(u32 x, u32 y, int codepoint, float px, u32 colour)
{
    if (!s_ready || !s_window) return false;
    if (cpu_rt_on()) return false;
    if (s_queued >= QUEUE_MAX) return false;
    if (codepoint <= 0) return false;

    // The codepoint IS the key.  Four raw bytes rather than a UTF-8 encoding,
    // so nothing downstream is tempted to read it as text.
    char key[4];
    key[0] = (char)( (u32)codepoint        & 0xFF);
    key[1] = (char)(((u32)codepoint >>  8) & 0xFF);
    key[2] = (char)(((u32)codepoint >> 16) & 0xFF);
    key[3] = (char)(((u32)codepoint >> 24) & 0xFF);

    const RunSlot *r = run_get(key, 4, px, RUN_FACE_ICON, colour, codepoint);
    if (!r) return false;

    queue_run(r, x, y);
    return true;
}

// Bind one run's texture.  Same descriptor as ui_card_gpu's bind_card, with
// NEAREST filtering for the same reason: the CPU blit this replaces did no
// filtering, and the brief is explicit that the existing design must not
// shift.  Runs are drawn 1:1, so there is nothing to gain from linear anyway.
static void bind_run(const RunSlot *r)
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
    tex.width    = (u16)r->w;
    tex.height   = (u16)r->h;
    tex.depth    = 1;
    tex.location = GCM_LOCATION_RSX;
    tex.pitch    = r->pitch;
    tex.offset   = r->off;

    rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
    rsxLoadTexture(context, 0, &tex);
    rsxTextureControl(context, 0, GCM_TRUE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(context, 0, 0,
        GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST,
        GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(context, 0,
        GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
        GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void submit_queue(void)
{
    if (!s_queued) return;

    // These quads BLEND, so the RSX reads back the pixels the PPU has just
    // written to the framebuffer this frame.  Those writes went through the
    // PPU's write-gather buffer; drain it before the RSX can look.  In
    // practice rsxFlushBuffer() in flip() would order it anyway -- that is why
    // scanout never shows a torn CPU draw -- but a barrier here is free once a
    // frame and makes the dependency explicit rather than incidental.
    __asm__ __volatile__ ("sync" ::: "memory");

    rsxVertexProgram   *vpo = (rsxVertexProgram *)  video_vp_data;
    rsxFragmentProgram *fpo = (rsxFragmentProgram *)video_fp_data;
    void *vp_ucode; u32 vp_size;
    rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
    rsxLoadVertexProgram(context, vpo, vp_ucode);
    rsxSetVertexAttribOutputMask(context, vpo->output_mask);
    rsxLoadFragmentProgramLocation(context, fpo, s_fp_off, GCM_LOCATION_RSX);

    rsxSetDepthTestEnable(context, GCM_FALSE);
    rsxSetDepthWriteEnable(context, GCM_FALSE);
    rsxSetBlendFunc(context,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
        GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
    rsxSetBlendEquation(context, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(context, GCM_TRUE);

    const float W = (float)display_width, H = (float)display_height;
    int cur_top = -1, cur_bot = -1;

    for (u32 q = 0; q < s_queued; q++) {
        const RunDraw *d = &s_queue[q];
        const RunSlot *r = &s_runs[d->slot];
        if (!r->used) continue;          // cache was flushed mid-frame

        // The vertical scissor the CPU primitives honour through
        // cpu_row_clipped() has to mean the same thing here, or a Home shelf
        // scrolling out of its band would write its labels over the tab bar.
        int top = d->clip_top, bot = d->clip_bot;
        if (top < 0) top = 0;
        if (bot <= 0 || bot > (int)display_height) bot = (int)display_height;
        if (bot <= top) { top = 0; bot = (int)display_height; }
        if (top != cur_top || bot != cur_bot) {
            rsxSetScissor(context, 0, (u16)top,
                          (u16)display_width, (u16)(bot - top));
            cur_top = top; cur_bot = bot;
        }

        bind_run(r);

        const float x0 = (2.0f * (float)d->x            / W) - 1.0f;
        const float x1 = (2.0f * (float)(d->x + r->w)   / W) - 1.0f;
        const float y0 = 1.0f - (2.0f * (float)d->y          / H);
        const float y1 = 1.0f - (2.0f * (float)(d->y + r->h) / H);

        // TEX0 before POS: the POS write latches the vertex.  Backwards and
        // the attribute is silently dropped -- same rule as ui_card_gpu.cpp.
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
    }

    // Leave the scissor covering the whole surface.  A stale scissor would
    // silently clip the wave and the video quad on later frames, which reads
    // as a rendering fault rather than the state leak it is.
    rsxSetScissor(context, 0, 0, (u16)display_width, (u16)display_height);
    s_queued = 0;
    s_st_flushes++;
}

void ui_text_gpu_flush(void)
{
    if (!s_ready) return;
    submit_queue();
    s_window = false;
}

void ui_text_gpu_flush_fenced(void)
{
    if (!s_ready || !s_queued) return;
    submit_queue();
    rsxSync();   // CPU framebuffer writes follow; they must not race the quads
}
