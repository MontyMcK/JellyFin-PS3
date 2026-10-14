// The depth engine's draw half: one stage renderer for every category.
//
// depth.h decides where every item is (and is host-tested); this draws what
// it decided, with the primitives the client already proves on hardware --
// constant-alpha card quads (ui_card_gpu_draw_ex), blended rects, and the
// wave's immediate-mode fans.  Nothing here reads video memory, allocates, or
// touches the JellyWave vertex path.
//
// A category hands the stage a range of item indices and a callback that
// resolves index -> DepthCard for this frame.  The stage draws, in the one
// order that reads as depth:
//
//   1. the focus halo's glow, BEHIND everything;
//   2. every card, FAR TO NEAR (by distance from the focus), each with its
//      veil and its reflection -- so an item flying forward lands on top of
//      anything it passes;
//   3. the halo's accent bands and frame, and thin progress strips, OVER.
//
// Home (ui_home_stage.inc) resolves cards on the column->queue swing, the
// libraries (xmb/ui_depth_lib.cpp) on the column->grid swing.  That callback
// is the ONLY thing that differs between them.
#ifndef JF_UI_DEPTH_H
#define JF_UI_DEPTH_H

#include <stddef.h>
#include "depth.h"
#include "thumbnail_cache.h"   // ThumbImg

// One item on a stage, this frame.
typedef struct {
    const char *img_id;      // thumbnail cache id (an episode may use its series')
    int         src_w, src_h;// the size it is cached at -- shared with the
                             // screen that normally shows it, so no refetch
    ThumbImg    img;
    depth_box   box;         // SCREEN px, with op / veil / refl
    float       halo;        // focus treatment, 0..1
    int         progress_pct;// 0 = none
    float       progress_a;  // the thin progress strip's opacity
    bool        selected;    // the CPU fallback's selection frame
    const char *tile_name;   // CPU fallback's letter tile (music), or NULL
} DepthCard;

typedef bool (*DepthCardFn)(const void *ctx, int i, DepthCard *out);

typedef struct {
    int         i0, i1;      // item indices [i0, i1)
    float       pos;         // the focus position (fractional while gliding)
    float       alpha;       // whole-stage opacity (category glide, detail)
    DepthCardFn card;
    const void *ctx;
} DepthStage;

// Screen transform for this frame (UIS scale + overscan).  Cheap; call per use.
depth_xform depth_xf(void);
// Authored coordinate -> screen px (rounded), through depth_xf().
int depth_sx(float ax);
int depth_sy(float ay);
// Screen x -> authored x (for anchors that come from the spine row).
float depth_ax(int sx);
// The stage's floor, screen px: the top of the hints bar.  Reflections end
// there (depth_refl_clamp); under overscan they would otherwise run into it.
int depth_floor_y(void);

// GPU phase (before the frame's rsxSync).  The caller owns the clip band.
void depth_stage_gpu(const DepthStage *s);

// CPU phase: the opaque fallback when the RSX card path is not up -- only
// cards at least `min_op` opaque and wholly on screen.  A no-op otherwise.
void depth_stage_cpu(const DepthStage *s, float min_op);

// Queue thumbnail requests for every card in [i0, i1) that resolves.  The
// cache is driven by what is asked for each frame, so call this every frame
// the stage is on screen (CPU phase).
void depth_stage_request(const DepthStage *s);

// The screen rect the focused card was last drawn at, noted by the stage (and
// by the gliding focus ring).  Item detail flies its poster out of it, so L3
// reads as the next depth rather than a screen swap.  False when nothing was
// noted in the last couple of frames.
void depth_note_focus_rect(int x, int y, int w, int h);
bool depth_last_focus_rect(int *x, int *y, int *w, int *h);
// The image the stage drew that focused card with (cache id, cached size,
// kind), so the Triangle peek turns over the very poster on screen.  Noted
// by the stage only; a rect noted without one (the gliding ring) clears it.
void depth_note_focus_card(const char *img_id, int src_w, int src_h, ThumbImg img);
bool depth_last_focus_card(char *img_id, int id_sz, int *src_w, int *src_h, ThumbImg *img);

// --- text -------------------------------------------------------------------

// Copy s into out, cut with "..." to fit max_w at (px, face) -- walking back to
// a UTF-8 lead byte so a character is never split.
void depth_fit_text(char *out, size_t cap, const char *s, float px, int face,
                    int max_w);

// The L1 label block beside a column (the canvas: "the label goes beside"):
// eyebrow + position in the spec face, a 30 px title, then `meta` (may be
// NULL) at the meta line.  a = opacity 0..1; text fades by colour, quantised
// so a fade reuses cached runs.
typedef void (*DepthMetaFn)(const void *ctx, int x, int y, int max_w, float a);
void depth_l1_label_draw(int shape, float anchor_x, float a,
                         const char *eyebrow, const char *pos,
                         const char *title, DepthMetaFn meta, const void *ctx);

#endif
