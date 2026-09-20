#pragma once
#include <ppu-types.h>

// GPU text runs.
//
// MEASURED REASON THIS EXISTS.  With card images on the RSX, text is what is
// left: on Home the frame spends text=4,650 us + chrome=1,630 us, and the
// per-glyph instrumentation in ui_text.cpp says that frame drew gl=208 glyphs
// covering bpx=7,365 blended pixels and opx=1,351 opaque ones.
//
//     7,365 blended pixels x 4 bytes = 29,460 bytes read back from RSX local
//     memory, at the 7.7 MB/s tools/spubench measured = 3,826 us.
//
// That is 82% of the text bucket, and it matches the measurement almost
// exactly -- so the cost of text is not rasterizing it, and it is not writing
// it.  It is the READ half of the read-modify-write that anti-aliased
// compositing does against video memory, and the PPU reads video memory about
// 100x slower than it writes it.
//
// A full-screen main-memory staging layer does NOT fix this: 1920x1080x4 is
// 8.29 MB, and at the measured 767 MB/s PPU->VRAM write rate that is 10.8 ms
// of upload to save 4.65 ms of blending.  The arithmetic is in
// docs/spu-feasibility.md and it is why that approach was dropped.
//
// What does fix it is making the unit of work a TEXT RUN instead of a pixel.
// A label is one small texture -- "Continue Watching" at 22 px is about
// 190x26 -- and the RSX composites it with no PPU reads at all.  Better, a
// run is CACHEABLE in a way a glyph blit never was: the string, size, face and
// colour of a label do not change between frames, so a warm screen uploads
// nothing and costs four vertices per label.  Only the first frame after a
// label's text changes pays anything.
//
// FORMAT.  Runs are stored A8R8G8B8 with the text colour baked into RGB and
// coverage in A, which means the existing video_vp/video_fp passthrough
// programs draw them as-is -- no new shaders, nothing new to get wrong on
// hardware that takes the whole console down when a GPU state bind is wrong.
// A one-channel B8 texture would be 4x smaller, but it needs its own fragment
// program and its own remap, and 4x smaller only helps on a cache MISS.  That
// trade is worth revisiting once this path has proven itself; see
// docs/renderer-architecture.md.
//
// GAMMA.  ui_text.cpp composites in linear light (the s_g2l/s_l2g LUTs) so
// glyph edges do not go muddy.  RSX fixed-function blending is plain 8-bit.
// The two are reconciled by baking the correction into the stored alpha:
// storing l2g[a] instead of a is EXACTLY equivalent for any text colour over a
// black background, because l2g(a*g2l(fg)/255) == (l2g(a)/255)*fg, and a close
// approximation as the background lightens.  The XMB background is dark, so
// this reproduces what is on screen today.

// ---- lifecycle -----------------------------------------------------------

// Allocate the VRAM atlas and read the gate file.  Call once from main.cpp,
// AFTER plog_load_setting() -- ui_init() runs before the logger exists, which
// is why ui_card_gpu_init() was moved out of it too.
void ui_text_gpu_init(void);

// True when the atlas is up and the gate file says 1.
bool ui_text_gpu_ready(void);

// ---- the collecting window -----------------------------------------------
//
// drawTTF only defers to the GPU BETWEEN begin() and flush().  Outside that
// window it blits on the CPU exactly as it always has, so every screen that
// does not opt in -- the player HUD, the login OSK, the music screen, the item
// overlay -- keeps working untouched.  That is deliberate: a queued run that
// nobody flushes is invisible text, and making the window explicit means only
// the code that promises to flush can queue.
//
// begin() must be called AFTER the frame's rsxSync(), flush() BEFORE flip().
void ui_text_gpu_begin(void);

// Submit every queued run and close the window.
//
// No rsxSync() afterwards: flip() puts the flip command in the FIFO behind
// these quads, and waitflip() at the top of the next frame is the wait.  A
// caller that intends to keep writing framebuffer pixels after flushing must
// fence itself -- see ui_text_gpu_flush_fenced().
void ui_text_gpu_flush(void);

// flush() plus rsxSync(), for the mid-frame case: something is about to draw
// OVER the text that has been queued so far (the update popup dims the whole
// screen and then lays an opaque panel on it).  Leaves the window OPEN, so
// text drawn after it still defers.
void ui_text_gpu_flush_fenced(void);

// Queue one run.  Returns true when it was queued and the caller must draw
// nothing; false means fall back to the per-glyph CPU path.  Called from
// drawTTF_face(); no other caller should need it.
bool ui_text_gpu_run(u32 x, u32 y, const char *text, float px, u32 colour,
                     int face);

// Queue one ICON glyph.  Same contract as ui_text_gpu_run(); called from
// drawIcon().
//
// Icons get their own entry point rather than being passed to the run path as
// a one-character string, for two reasons that both bite silently:
//
//   * ttf_run_box()/ttf_run_raster() walk the string BYTE BY BYTE
//     (`cp = (unsigned char)*p`), with no UTF-8 decoding.  Icon codepoints
//     live in the Private Use Area, so a UTF-8 encoding of one would be read
//     as three separate Latin-1 glyphs.
//   * Icons composite with the PLAIN 8-bit blend, not the linear-light one
//     text uses, so they must NOT get the l2g[] gamma bake that
//     ttf_run_raster() applies.  Sharing the raster would shift every icon's
//     edge weight.
//
// The cache key is the codepoint, so the existing (px, colour, face) keying
// applies unchanged and a settled screen uploads nothing.
bool ui_text_gpu_icon(u32 x, u32 y, int codepoint, float px, u32 colour);

// Per-second counters for the xmb: cost line.  runs = quads submitted,
// misses = runs that had to be rasterized and uploaded, bytes = uploaded.
void ui_text_gpu_stats_reset(void);
void ui_text_gpu_stats_get(u32 *runs, u32 *misses, u32 *bytes, u32 *flushes);

// ---- rasterization hooks, implemented in ui_text.cpp ---------------------
// ui_text.cpp owns stb_truetype, the face table and the glyph cache; this
// module owns video memory and the RSX.  These two calls are the seam.

typedef struct {
    int w, h;      // ink extent of the whole run, in pixels
    int ox, oy;    // offset from the drawTTF (x, y) anchor to the ink's corner
} TtfRunBox;

// Measure a run's ink box.  False when the font is unavailable or the string
// has no ink (all spaces), in which case there is nothing to draw.
bool ttf_run_box(const char *text, float px, int face, TtfRunBox *box);

// Rasterize a run into a tight w*h A8R8G8B8 buffer: RGB = colour, A =
// gamma-adjusted coverage.  dst must hold box->w * box->h pixels.
void ttf_run_raster(const char *text, float px, int face, u32 colour,
                    u32 *dst, const TtfRunBox *box);

// The same seam for a single icon glyph.  ttf_icon_raster() stores RAW
// coverage, not l2g[coverage] -- see ui_text_gpu_icon() above.
bool ttf_icon_box(int codepoint, float px, TtfRunBox *box);
void ttf_icon_raster(int codepoint, float px, u32 colour,
                     u32 *dst, const TtfRunBox *box);
