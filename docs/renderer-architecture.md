# Renderer architecture audit

Answers §25 (A–L) of the PS3-native renderer brief, and proposes the Phase 1
patch.

Most of Phases 1–3 of that brief were already carried out earlier in this same
session, on real hardware. This document does not repeat the numbers; it
references `docs/spu-feasibility.md` (analysis) and
`tools/spubench/RESULTS-real-ps3.txt` (raw console output) and concentrates on
what the audit means for the architecture the brief describes.

**Headline, stated up front because it redirects the plan:** the measurements
contradict the brief's central premise. The brief assumes SPU-powered
animation is the win. On this console it is not — the animation arithmetic is
already free once vectorised, and the renderer's real cost is that *there is
almost no renderer*. The XMB issues about **28 RSX draw calls per frame and
binds zero textures**; every other pixel on screen is written by the PPU,
by hand, into video memory. Section K and the Phase 1 proposal follow from
that.

---

## A. Current renderer path

There is no render graph, no scene, no material system and no texture
pipeline. `ui_run_xmb()` (`source/ui/xmb/ui_xmb.cpp`) is the renderer:

```
waitflip()                        park until vsync
clearScreen()                     RSX: one clear
wave_draw()                       RSX: gradient quad + 3 ribbons, immediate mode
poll_buttons() / input
rsxSync()                         *** full GPU fence, PPU spins on usleep(30) ***
xmb_draw_cpu_phase()              PPU writes card pixels into VRAM
xmb_draw_text_phase()             PPU blends glyph pixels into VRAM
xmb_draw_hints() / xmb_draw_tabs()
flip()
```

Two structural properties matter more than anything else in the file:

1. **`rsxSync()` fences the GPU dead mid-frame.** Nothing overlaps it. The RSX
   idles through the expensive half of the frame and the PPU idles through the
   cheap half. There is zero CPU/GPU concurrency by construction.
2. **Input latency equals frame time.** `poll_buttons()` runs once per
   iteration, so any draw-time regression is felt directly as controller lag.
   This is the constraint that governs §9's motion budget.

The player has a *second*, different path (`source/player/`), and it is
architecturally better — see H.

## B. Current UI architecture

Immediate-mode, stateless, redrawn from scratch every frame. Screen state
lives in globals (`g_active_tab`, `g_sel`, `g_scroll_top`, `s_rows[]` in
`ui_home.cpp`); draw functions read them and paint.

There are no retained UI objects, so §8's "stable render graph / pools"
requirement is not a refactor of an existing object system — there is nothing
to convert. Card *bitmaps* are already pooled (`thumbnail_cache.cpp`, 32
fixed slots, no per-frame allocation), which is the part that would have been
expensive to add.

Animation today is essentially absent: `s_vscroll` pixel-scrolls the Home
shelves and `wave_phase[3]` advances the ribbons. Nothing else moves.

## C. Current RSX usage

Measured by grep across the tree:

| Directory | Texture calls | Draw submissions |
|---|---:|---|
| `source/ui` | **0** | 1 `rsxClearSurface`, 3 `rsxDrawVertexBegin` sites |
| `source/gfx` | **0** | — |
| `source/music` | **0** | — |
| `source/player` | 8 | video frame + HUD overlay |

**The XMB binds no textures at all.** Per frame it submits one clear plus 25
immediate-mode triangle strips (1 gradient quad + 3 ribbons × 8 slices), for
roughly **4,660 vertices** at 1920 wide — each pushed as two `rsxDrawVertex`
FIFO writes by the PPU.

Vertex *arrays* are not used **by the UI**: `ui_wave.cpp` records the
vertex-array-fetch path as unreliable on real hardware (the class of bug that
retired `HUD_DIM_GPU_ARRAY`), so everything is streamed inline into the
command FIFO. That FIFO lives in **main memory** (`rsxInit` is handed a 32 MB
`memalign` host region), so pushing vertices is ordinary cached writing, not
VRAM traffic.

### Correction: vertex arrays are not actually unreliable

I initially read that comment as "this hardware cannot be trusted with
vertex-array fetch", and told the user the biggest risk to the whole renderer
plan was that moving to textured, batched geometry might hit the same wall.
**That was wrong, and reading `source/player/gpu/player_rsx.cpp` settles it.**

`rsx_draw_video_frame()` does exactly what the new renderer needs, every frame,
on this console:

```c
rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_POS,  0, s_vid_vbuf_off,      24, 4, F32, RSX);
rsxBindVertexArrayAttrib(context, GCM_VERTEX_ATTRIB_TEX0, 0, s_vid_vbuf_off + 16, 24, 2, F32, RSX);
...
bind_yuv3(...);                      /* three texture units */
rsxInvalidateVertexCache(context);   /* <-- the actual fix */
rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
```

Interleaved attributes at a 24-byte stride, multiple textures bound, several
passes with blend-state changes between them. It works because of
**`rsxInvalidateVertexCache()` before every draw** — the failure mode was a
stale binding, not a broken fetch unit. The video path found the right
discipline; the UI path simply predates it.

Two further conventions the working code establishes, both worth copying
verbatim rather than rediscovering:

- **Inline vertices: push TEX0 before POS. The POS write latches the vertex.**
  Documented independently in `player_rsx.cpp` and `hud_dim.cpp`.
- **Compose in main memory, upload, let the RSX composite.**
  `hud_overlay_alloc()` keeps `s_ovl_stage` (main, cached) and `s_ovl_tex`
  (`rsxMemalign`, sampled by the GPU) and runs the whole full-screen HUD that
  way at 1920×1080.

So every mechanism the new UI needs — textured quads, interleaved vertex
arrays, multiple bound textures, alpha blending, staged upload and GPU
compositing — **already exists and ships in this repository**. The renderer is
not a from-scratch build against unproven hardware; it is applying
`source/player/`'s working pattern to `source/ui/`.

One consequence worth stating plainly: once cards are RSX-textured quads,
**the focus-scale problem disappears**. Scaling becomes a vertex transform
instead of a rescaled CPU blit, so the whole `cpu_blit_bitmap_scaled` cost
class stops applying to it. The Bresenham fix in Part 1.3 remains correct for
anything still scaled on the CPU, but it stops being on the critical path for
the new design.

Consequence for §7 and §18: **overdraw control and texture atlases are
currently solving problems this renderer does not have.** There is no overdraw
because the RSX draws almost nothing, and no texture switching because there
are no textures. Both become real the moment UI drawing moves onto the RSX —
which is the point, but the instrumentation for them should land *with* that
move, not before it.

## D. Current SPU usage

**The application uses none.** No `sysSpuInitialize`, no thread group, no SPU
binary anywhere in `source/`.

The only SPU consumer is `cellVdec`, configured with `NUM_SPUS = 3`
(`source/video/vdec.cpp:121`). So:

| State | vdec | Free for the app |
|---|---:|---:|
| Browsing the XMB | 0 | **6** |
| Video playing | 3 | **3** |

Any persistent worker pool must therefore be correct and performant at
**three** workers, and must be joined before `vdec_open()`. This is the single
hardest constraint on §13's dynamic allocation model.

## E. Current frame loop

As in A. Now instrumented: the loop reports its own per-phase cost to
`player_log.txt` once a second when `plog=1` —

```
xmb: frame=..ms vsync=.. gpu=.. sync=.. cards=.. text=.. chrome=.. flip=..
     other=.. (us/frame) gl=.. bpx=.. opx=..
```

`bpx`/`opx` split glyph pixels into those that took the read-modify-write
branch (one uncached VRAM read each) versus the opaque store-only path. This
is the measurement that decides how urgent K is; see "What is still unknown".

## F. Current allocations

**The per-frame path is already allocation-free**, which is better than the
brief assumes. Verified by grep: the only `malloc` in `source/ui` outside the
glyph-cache arena is the one-shot `ps_sprites_preload()` and navigation-time
`detail_media_free()`.

Everything large is reserved at boot in a deliberate order (`main.cpp`),
because reservation order was load-bearing — *"a movie could fail to start
depending on what the UI had allocated"*. At 1920×1080:

| Allocation | Size | Where |
|---|---:|---|
| RSX host/IO region | 32 MB | main |
| VDEC arena | 96 MB (720p) / 64 MB (1080p) | main |
| Jitter buffer | 24 × 1.32 MB / 16 × 3.13 MB | main |
| HUD overlay staging | 8.29 MB | main |
| Thumbnail cache | 32 × ~455 KB ≈ 14.6 MB | main |
| Image decode arena | 4 MB | main |
| Glyph cache arena | 384 KB | main |
| **≈ total** | **≈ 187 MB** of ~213 MB usable | |

RSX local memory (256 MB) is comfortable: 2 framebuffers + depth + HUD texture
≈ 41 MB.

**Main memory is the constrained resource, with only a few MB of genuine
slack.** §14's rules are already largely satisfied; the risk is not current
behaviour but anything new that wants a full-screen main-memory buffer
(7.91 MB at 1080p).

## G. Best location for persistent SPU workers

Owned by a new `source/spu/` module, created at XMB entry and joined before
`vdec_open()`. Not owned by the UI: the brief's §13 wants audio kernels to
coexist, and the pool must outlive any single screen.

A working implementation already exists at `tools/spubench/` — persistent
workers, DMA lists, double buffering, signal-notification wake-up, watchdog,
bounds-checked job blocks and PPU fallback. Promoting it is Phase 1 (below)
rather than writing one from scratch.

Measured properties that should shape its interface:

- Sync round trip **2–4 µs**, and `sysSpuThreadWriteSignal` (an LV2 syscall)
  is **no slower than busy-polling** — so park workers on signals; do not burn
  an SPU spinning.
- DMA crossover is **8–16 KB per transfer**. Below ~4 KB the job costs nearly
  double. This is §2's "no tiny DMA operations", with a number on it.
- Double buffering hides the **input** DMA (26.1 → 1.9 µs) but **not** the
  output (47.6 → 48.8), because the final write-back must be waited on.
- **Local store is the real limit**: 144 KB static, of which 128 KB is the
  double-buffered working set and only ~12 KB is code. Declaring three
  separate arrays overflowed outright (`ld: .bss exceeds local store range`).

## H. Best location for animation state

Struct-of-arrays, in a pool owned alongside the worker pool.

**AoS costs 6.7× more than SoA on an SPU** (1,082 µs vs 163 µs at 10,000
objects). The brief's §4 object description — position, scale, rotation,
alpha, colour, UV offset, UV scale — is an AoS shape. It must be stored
field-per-array or the SPU path is pointless.

Note also that SoA barely helps the *scalar* PPU (342 → 338 ns/object); it
only pays by enabling SIMD. That is worth knowing before restructuring
anything on the assumption that layout alone is the win.

## I. Best location for wave/particle buffers

Main memory, double-buffered, written by SPUs and read by the PPU as it
streams vertices into the FIFO.

**Not** VRAM. And specifically not via the tempting route of having the SPU
build an RSX command segment for `rsxSetCallCommand` — that is plausible and
unproven, and this codebase already has scar tissue from trusting an
RSX-side mechanism that misbehaved on hardware.

The existing wave is the right shape to extend: one canonical geometry
(`WAVE_MAX_COLS` columns × `WAVE_NS` slices) reused per band with different
parameters, which is exactly §5's "one reusable wave representation + band
transforms".

## J. Existing performance instrumentation

Before this session: the player had `player_stats.cpp` (the `hb:` heartbeat —
fps, ring fill, PCM fill, pulldown) and a stats overlay. **The XMB had
none** — which is why the SPU feasibility work had to estimate UI frame cost
from layout constants rather than measure it.

Added this session: the `xmb:` line in E, plus `tools/spubench/` as a
standalone measurement rig. Still missing, and needed before §7/§10 mean
anything: draw-call count, texture-switch count, overdraw ratio, RSX busy
time. Those should land with the move to RSX drawing.

## K. Biggest immediate bottleneck

Not the PPU's arithmetic. **PPU reads from RSX video memory**, measured on
this console:

| | REAL PS3 RESULT |
|---|---:|
| PPU → VRAM memcpy | 767 MB/s |
| PPU VRAM flat fill | 1,288 MB/s |
| **PPU read from VRAM** | **7.7 MB/s** |
| PPU main → main memcpy | 398 MB/s |
| **SPU DMA → VRAM** | **3,314 MB/s** |

Writing to video memory is *faster than writing to main memory* — write
gathering works, and the card `memcpy` blits are already on the right path.
Reading it back is **~100× slower**, and that is the path every anti-aliased
glyph pixel takes in `blit_coverage()`.

For contrast, the workload the brief wants to move to SPUs: **10,000 animated
objects cost 254 µs on the PPU with AltiVec — 1.5 % of a 60 Hz frame** (13.3×
over scalar). Three SPUs do it in 68 µs. Moving 1.5 % of a frame to 0.4 % is
not a user-visible change; it is a rounding error with a fallback path
attached.

So the ordering the measurements support is:

1. Stop reading video memory.
2. Put UI drawing on the RSX (which is also what makes §7's overdraw budget a
   real thing rather than a hypothetical).
3. Then, if a gap remains, use SPUs — and use them for **pixels and geometry
   preparation**, where 3.3 GB/s is genuinely transformative, not for
   animation arithmetic that is already free.

## L. Minimal first implementation that can prove SPU value

Already built and run: `tools/spubench/`, a standalone homebrew testing
1,000 / 5,000 / 10,000 / 20,000 objects across PPU scalar AoS, PPU scalar SoA,
PPU AltiVec SoA and 1–5 SPU workers, with DMA and local-store breakdowns,
a chunk-size sweep, sync-cost comparison, memory-bandwidth measurements and a
60 Hz stability test.

It proves SPU value **exists** — 3.7× over the PPU's own best effort at 10,000
objects with 3 workers, scaling linearly in compute — and simultaneously
proves that value is **not needed for this workload**. Both halves of that
result are worth having.

It also established the safety property §23 asks for: across 1,200 measured
frames in four configurations, **not one frame exceeded 33.4 ms**, and three
SPU workers running every frame cost the display loop nothing measurable.

---

## What is still unknown

Three things, and I would rather name them than let the plan assume them:

1. **The real glyph cost.** The 7.7 MB/s figure came from a `volatile` loop,
   which forces one load and one store per pixel with no batching. The actual
   blitter is not volatile, so that is a *floor on the hardware's speed*, not
   the cost of this path. The `xmb:` line answers it; until it does, the
   urgency of K is bounded but not pinned.
2. **RSX FIFO throughput.** The existing wave sustains ~4,660 vertices/frame
   through immediate mode. A few thousand particles is 16,000+ vertices, and
   nothing measures where that path saturates. For §6 this is *the* open
   question — the particle physics is settled (~100 µs on the PPU), the
   drawing is not.
3. **50,000 particles.** The brief asks for it; the benchmark stops at 20,000.
   Extrapolating linearly gives ~1.7 ms PPU AltiVec / ~330 µs on 3 SPUs, but
   50,000 × 8 fields × 4 B = 1.6 MB, which is past any cache and may not
   extrapolate. Cheap to add to the existing rig.

## Compliance with §5 (clean room)

Nothing Sony-derived is in the tree. `tools/spubench/` is original code; the
wave in `ui_wave.cpp` is original procedural geometry with its own constants.
The parameter vocabulary quoted in the brief (DAMPING/TENSION/FRESNEL/…) is
used as a *mathematical* reference for an independent implementation. No
`spline.elf`, `particles.elf`, QRC, theme texture or firmware asset will be
added.

The research URLs in §4 have not been fetched yet — they are inputs to the
wave work (Phase 5), not to this audit, and I would rather read them when
they are actionable than summarise them speculatively now.

---

## Phase 1 — landed

All three items are in the tree and building. Nothing restructures the UI and
nothing changes default behaviour.

| Item | State |
|---|---|
| `source/spu/` job layer | **landed**, builds, kernel embedded |
| Benchmark extended to 50,000 | **landed**, deployed to the console |
| XMB per-frame cost line | **landed**, deployed, awaiting a session |
| In-app SPU self-test | **landed**, opt-in via `jellyfin_sputest.txt` |

`source/spu/` is ~10 entry points and no engine:

```c
bool jf_spu_available(void);
bool jf_spu_start(int workers);       // clamps to what vdec leaves free
void jf_spu_stop(void);               // MUST precede vdec_open()
bool jf_spu_dispatch(const JfSpuJob *job);
bool jf_spu_collect(u32 deadline_us); // false -> caller uses its PPU path
void jf_spu_step_ppu(...);            // AltiVec SoA, the default implementation
void jf_spu_selftest(void);           // opt-in, proves the pool in-app
```

SPU kernel footprint: **5,040 B text + 832 B data + 66,064 B bss = 71,936 B**
of the 256 KB local store, leaving ~190 KB free. `CHUNK_MAX` is 1,024 objects
(64 KB double-buffered), on the flat part of the measured chunk curve.

**Nothing calls `jf_spu_start()` in the normal path**, deliberately. The pool
exists, is tested and is available; its first real consumer should be chosen
on evidence rather than because the infrastructure now exists. On the current
measurements that consumer is the tile compositor, not an animation worker.

Safety properties carried over from the benchmark, each learned the hard way:
signal-notification wake-up, 8–16 KB chunking, input-side double buffering, a
one-frame watchdog that disables the pool rather than stalling a frame,
bounds-checked job blocks, and the publish protocol that carries the live
`seq` through the struct copy — zeroing it lets a worker run a job assembled
from two different commands and DMA past the end of an array.

### Original proposal, for the record

Deliberately small, and it does **not** restructure the UI.

**1. Promote the job system: `tools/spubench/` → `source/spu/`.**

```
source/spu/jf_spu.h        // ~10 entry points, no engine
source/spu/jf_spu.cpp      // pool lifetime, dispatch, watchdog, fallback
source/spu/kernels/        // SPU-side binaries, one per workload
```

Interface kept to roughly:

```c
bool jf_spu_available(void);
bool jf_spu_start(int workers);      // clamps to what vdec leaves free
void jf_spu_stop(void);              // MUST be called before vdec_open()
int  jf_spu_dispatch(const JfSpuJob *job, int n_workers);
bool jf_spu_collect(int deadline_us); // false -> caller uses its PPU path
```

Carrying over, because each was learned the hard way: signal-notification
wake-up, 8–16 KB chunking, input-side double buffering, the one-frame
watchdog, bounds-checked job blocks, and the publish protocol that carries the
live `seq` through the struct copy (zeroing it lets a worker run a job
assembled from two different commands and DMA past the end of an array).

**2. Extend the benchmark** to 50,000 particles and add RSX-side counters
(draw calls, vertices, FIFO bytes, RSX busy time) so §7 and §10 have something
to report.

**3. Collect the `xmb:` frame line** from a real session and settle question 1
above.

**What I would not do yet:** build the persistent animation evaluator (§4).
Until the frame line shows where time actually goes, an SPU animation worker
is a solution measured at 1.5 % of a frame, and the brief's own instruction is
to measure where performance is uncertain.

---

## Phase 2 — text on the RSX

With card images moved to the GPU, the frame cost line on Home settled at:

```
vsync=4,930  gpu=2,137  sync=1,020  cards=2,145  text=4,650  chrome=1,630
flip=9  other=153    gl=208  bpx=7,365  opx=1,351
```

`text` was 42 % of all work, and the instrumentation says exactly why. 208
glyphs put ink on 8,716 pixels, of which **7,365 took the blend branch** — one
uncached 4-byte read from RSX local memory each. At the 7.7 MB/s
`tools/spubench` measured for PPU reads from video memory:

```
7,365 px x 4 B = 29,460 B / 7.7 MB/s = 3,826 us
```

3,826 of 4,650 µs. The cost of text is neither rasterizing it nor writing it;
it is the *read* half of read-modify-write compositing against video memory.

### Why not a full-screen staging layer

The obvious fix — compose the whole UI in main memory and upload once — does
not survive arithmetic. 1920×1080×4 is 8.29 MB; at the measured 767 MB/s
PPU→VRAM write rate that is **10.8 ms of upload to save 4.65 ms of blending**,
on a 16.68 ms frame. Rejected on that basis, not on taste.

### What landed instead: one texture per text run

`source/ui/render/ui_text_gpu.{h,cpp}`. The unit of work becomes a **string**,
not a pixel. `drawTTF` hands the whole run to the RSX as a small texture and
the hardware composites it — no PPU reads at all.

The decisive property is not that a run is cheaper to draw. It is that a run
is **cacheable in a way a glyph blit never was**: a label's text, size, face
and colour do not change between frames, so a settled screen uploads nothing
and costs four vertices per label. Only the first frame after a label's
content changes pays anything.

| Decision | Why |
|---|---|
| A8R8G8B8, colour baked into RGB, coverage in A | The existing `video_vp`/`video_fp` passthrough programs draw it as-is. **No new shaders** — nothing new to get wrong on hardware that takes the console off the network when a GPU bind is wrong. B8 would be 4× smaller but needs its own fragment program and remap, and 4× smaller only helps on a cache *miss*. |
| Stored alpha is `l2g[a]`, not `a` | `blit_coverage` composites in linear light; RSX fixed-function blending is plain 8-bit. `l2g(a·g2l(fg)/255) == (l2g(a)/255)·fg`, so baking `l2g` in is **exact for any text colour over black** and close as the background lightens. The XMB background is dark. |
| Rasterize into main memory, then `memcpy` to VRAM | Rasterizing into VRAM would be the very mistake this path exists to avoid — glyph compositing is scattered read-modify-write. Staging keeps the RMW where it is cheap and makes the VRAM traffic one sequential write per row, which write-gathering handles at 767 MB/s. |
| An explicit `begin()`/`flush()` window | A queued run that nobody submits is *invisible text*. Only the XMB loop, which promises to flush, is allowed to queue; every other screen (player HUD, login OSK, music, item overlay) keeps the per-glyph path untouched. |
| Submit after the CPU phase, no fence | `flip()` queues the buffer swap behind the quads and `waitflip()` next frame is the wait, so the RSX gets the rest of the vblank. Precedent: `hud_dim_rect` already interleaves RSX work with CPU pixel writes. |
| A full atlas defers its reset to the next frame | A queued draw holds a slot *index*. Emptying the table mid-frame would let a later run take a slot an earlier queued draw still points at, and draw the wrong words at the right coordinates. Instead the atlas fills, further runs fall back to the CPU blit for the rest of that frame, and the reset happens at the next `begin()`. |

Gated on `/dev_hdd0/tmp/jellyfin_gputext.txt` = 1, same shape as
`jellyfin_gpucards.txt`. With the file absent nothing is allocated and
`drawTTF` behaves exactly as before.

### Verification: `tests/test_text_runs`

The run path duplicates `drawTTF_face`'s walk — same kerning, same `(int)xf`
pen truncation, same glyph-cache lookups. A duplicated walk that drifts is a
real hazard, and "it looked right on the TV" does not distinguish a correct
run from one that is a pixel to the left or drops a kerned overlap.

So `ui_text.cpp` is compiled **on the host** (`tests/hoststub/` stands in for
the PSL1GHT headers) and the two paths are compared pixel for pixel, white on
black — where the algebra above makes them exactly equivalent.

```
-- phase 1: single glyphs (no overlap possible) --
cases           : 8,550          (ASCII 32..126 x 9 sizes x 2 faces x 5 offsets)
inked pixels    : 447,782
ink-set mismatch: 0
pixels differing: 0, max channel delta 0
PASS (bit-exact)

-- phase 2: multi-glyph strings --
cases           : 1,800          (180 of them correctly had no ink)
inked pixels    : 1,008,958
ink-set mismatch: 0
pixels differing: 1,575 (0.1561%), max channel delta 6
```

Phase 1 is the strong claim: a lone glyph has no boxes to overlap, so the two
paths must agree **bit for bit** — which pins the pen position, the bitmap
box, the `l2g` identity and the blend formula all at once. It does, over
447,782 pixels.

That makes phase 2's residue attributable: every differing pixel is one two
glyph boxes *share*, and on those the two paths are not merely different, they
are differently **accurate**. The CPU path composites the second glyph onto the
first through the gamma LUTs, so it reads back a value it has already
quantised — it computes `g2l[l2g[a1]]`, and that round trip is lossy. The run
path combines coverage first and maps through `l2g` once at the end, so it
never makes the round trip. The residue is bounded by that quantisation, lands
on 0.16 % of pixels at ≤ 6/255, and favours the new path.
