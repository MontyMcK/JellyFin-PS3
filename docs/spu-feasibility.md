# Cell SPU feasibility for the Jellyfin PS3 client

Status: **complete.** Every number below marked REAL PS3 RESULT came off the
console itself; the raw output is checked in at
`tools/spubench/RESULTS-real-ps3.txt`.

The question is whether the SPUs can make the new UI direction (dark cinematic
shelves, focus springs, ambient particles, wave ribbons) substantially more
fluid without costing video decode, audio decode, network throughput, AV sync,
memory headroom, RSX time or controller latency.

## The answer

**Yes — but not for the reason the brief assumes, and not by doing the
animation maths.**

Three measurements decide it:

| | REAL PS3 RESULT |
|---|---|
| PPU writes to video memory (memcpy) | **766 MB/s** |
| PPU *reads* from video memory | **7.7 MB/s** |
| SPU DMA into video memory | **3,314 MB/s** |

PPU reads from RSX local memory are **100× slower than writes**. That single
asymmetry is the dominant fact about this renderer, because the UI's text path
reads the framebuffer back for every anti-aliased glyph pixel.

And the animation arithmetic the brief is about turns out not to be a cost at
all once it is vectorised: 10,000 objects take **254 µs on the PPU with
AltiVec — 1.5 % of a 60 Hz frame**. Three SPUs do the same work in 68 µs.
Going from 1.5 % of a frame to 0.4 % buys nothing anyone can see.

So I was half wrong in my own first-pass hypothesis, and the measurement is
worth stating plainly against it: I expected the SPU to be a *capability
enabler* for effects the app could not otherwise afford, and not a fix for an
existing bottleneck. In fact the reverse is closer to true. The SPU **can**
fix the biggest existing bottleneck — it just fixes it by moving **pixels**,
not maths. An SPU can compose a tile in local store (where reads are free) and
push it to the framebuffer at 3.3 GB/s, which removes the read-modify-write
problem completely rather than merely making it faster.

The recommendation that follows from that is in Part 5, and the first phase of
it **does not use an SPU at all** — because the same problem has a cheaper
answer the codebase already contains.

---

## Part 1 — Audit of the existing engine

### 1.1 The renderer is a CPU rasteriser writing into video memory

This is the single most important fact about the codebase and it governs
everything that follows.

`source/ui/render/ui_draw.cpp` draws by writing 32-bit pixels straight into
`color_buffer[curr_fb]`, which is `rsxMemalign()` memory — **RSX local
memory (VRAM)**, not main memory. The per-frame sequence in
`ui_run_xmb()` (`source/ui/xmb/ui_xmb.cpp:...`) is:

```
waitflip()
clearScreen()            RSX
wave_draw()              RSX   — gradient + 3 ribbons, immediate mode
poll_buttons()           PPU
rsxSync()                *** full GPU fence, PPU spins on usleep(30) ***
xmb_draw_cpu_phase()     PPU  — every card blitted into VRAM
xmb_draw_text_phase()    PPU  — every glyph blended into VRAM
xmb_draw_hints/tabs()    PPU
flip()
```

Two structural consequences:

1. **There is no CPU/GPU overlap at all.** `rsxSync()` fences the GPU dead,
   then the PPU does all the remaining work alone. The GPU idles through the
   expensive half of the frame and the PPU idles through the cheap half.
2. **Input latency equals frame time.** `poll_buttons()` runs once per
   iteration of this loop, so whatever the frame costs is what a button press
   costs. Any regression in draw time is felt directly as controller lag.

### 1.2 Which pixel paths are cheap and which are not

Not all of the CPU drawing is equal, and the difference is large:

| Path | Code | Access pattern | Cost class |
|---|---|---|---|
| Card / poster blit | `cpu_blit_bitmap` (`ui_lists.cpp`) | `memcpy` main → VRAM | **write-combined; the fast one** |
| Flat rect fill | `drawRect` | sequential VRAM stores | write-combined |
| Text / icons | `blit_coverage` (`ui_text.cpp:238`) | **reads `row[sx]` back**, blends, stores | **read-modify-write on VRAM** |
| Alpha rect | `drawRectBlend` | reads `p[c]` back | read-modify-write on VRAM |
| Scaled thumb | `cpu_blit_bitmap_scaled` | see 1.3 | was pathological, now fixed |

PPU *writes* to RSX local memory are write-gathered and tolerable. PPU *reads*
from RSX local memory are uncached, unbatched and roughly two orders of
magnitude slower. Every anti-aliased glyph pixel in the UI does one such read.

The codebase already knows this — `ui_wave.cpp` records that *"a full-screen
CPU fill costs ~150-180 ms"* and that a runtime probe once misclassified real
hardware onto the CPU-composite path, at which point *"every frame then read
back uncached VRAM and the whole UI crawled."* That is the same effect, hit
from a different direction.

**The display is 1920×1080, not 1280×720.** The console's own crash log reads
`2.4 res 1920x1080 aspect=2 par=1/1`. The design brief names 1280×720 as the
target; the hardware is running 2.25× that many pixels, and every CPU pixel
cost in this document scales with it. A framebuffer is 8.29 MB, not 3.69 MB.
This is worth an explicit decision rather than an accident — see Part 10.

### 1.3 A real defect found in the audit, and fixed

`cpu_blit_bitmap_scaled()` computed each source column as
`(u32)((u64)col * bm->width / dw)` — **a 64-bit integer division per
destination pixel**. Confirmed in the generated code, not inferred:
`ppu-gcc -O2 -mcpu=cell` emits `divdu` inside the inner pixel loop. On the PPE
`divdu` is microcoded: tens of cycles, not pipelined, and it serialises the
pipeline around itself.

Today this path only carries search thumbnails and album art, so it is a minor
cost. **Under the new design it becomes the main path** — "selected-card
scale" and "spring/inertia focus transitions" mean every visible card is
rescaled every frame. At ~3.8 MB of card pixels per Home frame that is several
million serialising divides per frame.

Replaced with exact Bresenham stepping (`q = W/dw`, `r = W%dw` hoisted out;
step and carry inside), which reproduces `floor(col*W/dw)` **bit for bit** —
verified over 32,085,102 (width, dest-width, clip-offset) positions with zero
mismatches — and leaves **no divide in either loop** (confirmed by
disassembling `obj/ui_lists.o`: all four remaining divides sit in the
prologue, before the row loop at `0x130` and the pixel loop at `0x1d0`). The
per-pixel horizontal clip test was hoisted out at the same time.

This fix is independent of anything to do with SPUs and is in the tree now.

### 1.4 Per-frame CPU pixel budget, measured from the real geometry

At the console's actual 1920×1080, from the constants in `ui_visuals.h` and
`ui_home.cpp`:

| Screen | Cards | Card size | CPU pixels/frame | Bytes |
|---|---|---|---|---|
| Movies grid (portrait) | 5 × 2 | 230 × 345 | 793,500 | 3.17 MB |
| Continue Watching (landscape) | 3 × 2 | 450 × 253 | 683,100 | 2.73 MB |
| Home (2 landscape + 1 portrait shelf) | 5+5+8 | 311×175 / 186×280 | ~961,000 | 3.84 MB |

Plus text: roughly 600–1500 glyphs per frame at ~150 px each, i.e. 0.1–0.2 M
pixels — but on the **read-modify-write** path, so their cost per pixel is the
one that matters.

The hypothesis the benchmark tests: **text, not cards, dominates the UI frame,
despite being ~5 % of the pixels.**

### 1.5 The player HUD already uses the right architecture

`hud_overlay_alloc()` (`source/player/hud/hud_draw.cpp:64`) allocates **two**
buffers: `s_ovl_stage` in main memory via `memalign`, and `s_ovl_tex` in VRAM
via `rsxMemalign`. The HUD composes into the cached main-memory staging buffer
and then hands the result to the RSX as a texture.

That is exactly the pattern the XMB does not use, and it is proof the app can
already do it. It matters a great deal for the integration plan (Part 10),
because a main-memory staging buffer is something an SPU can DMA into at full
speed, and VRAM is not.

### 1.6 Thread model

| Thread | Priority | Stack | Role |
|---|---|---|---|
| main / UI | 1001 | — | XMB, input, all UI drawing |
| `jf_audio` | 700 | 32 KB | PCM out |
| `jf_adec` | 750 | 64 KB | audio decode |
| `jf_decode` | 800 | 128 KB | demux / VDEC feed |
| `jf_upload` | 850 | 32 KB | frame upload |
| `jf_music` | 850 | 128 KB | music streaming |
| `jf_progress` | 1100 | 16 KB | server progress reports |
| `thumb_fetch` | 1500 | 64 KB | thumbnail HTTP + decode |

Lower number is higher priority on LV2. The UI thread is the *lowest*-priority
of the foreground threads, which is correct during playback and is why UI work
must never be on the critical path for audio.

### 1.7 Existing SPU usage

**The application itself uses no SPUs at all.** There is not one
`sysSpuInitialize`, `sysSpuThreadGroupCreate` or SPU binary in the tree.

The only SPU consumer is `cellVdec`, configured in `source/video/vdec.cpp:121`
with `NUM_SPUS = 3`. So the SPU availability picture is:

| State | SPUs used by vdec | Available to the app |
|---|---|---|
| Browsing the XMB (no playback) | 0 | **6** |
| Video playing | 3 | **3** |

This is the central budgeting fact for Part 3: any UI SPU worker must be
correct and performant with **three** SPUs, because three is what exists when
video is on screen, and it must give them back cleanly.

### 1.8 Memory, as it stands (1920×1080)

Main memory (~213 MB usable):

| Allocation | Size | Where |
|---|---|---|
| RSX host/IO region (`HOST_SIZE`) | 32 MB | main |
| VDEC arena | 96 MB (720p) / 64 MB (1080p) | main |
| Jitter buffer | 24 × 1.32 MB / 16 × 3.13 MB | main |
| HUD overlay staging | 8.29 MB | main |
| Thumbnail cache | 32 × ~455 KB ≈ 14.6 MB | main |
| Image decode arena | 4 MB | main |
| Glyph cache | 384 KB | main |
| **Total** | **≈ 187 MB** | |

RSX local memory (256 MB): 2 framebuffers (16.6 MB), depth (16.6 MB), HUD
texture (8.29 MB) — comfortable.

Main memory is the constrained resource, and `main.cpp` is explicit that the
reservation order exists because *"a movie could fail to start depending on
what the UI had allocated."* **Any SPU buffers come out of a budget with a few
megabytes of slack, not tens.**

---

## Part 2 — The benchmark

`tools/spubench/` — a standalone homebrew, deliberately *not* built into the
app, so the measurement is not distorted by the 96 MB VDEC arena or by
whatever the UI has done to the heap.

Built and installed to `/dev_hdd0/game/JFSPUBENCH`. It writes
`/dev_hdd0/tmp/spubench.txt`, flushing after every section so a fault still
leaves everything measured before it.

Workload, exactly as specified: 10,000 independent objects, each
`{x, y, vx, vy, rot, scale, alpha}` — plus `life`, which makes 8 fields and a
32-byte object, so the layout is a clean power of two and respawn is
expressible without a branch. Per object per frame: attraction to a centre,
2-D turbulence, velocity damping, Euler integrate, rotation from velocity,
life decay, alpha from life, scale breathing, branchless respawn. Roughly 40
multiply-adds and 3 sines.

**The sine is a shared 7th-order polynomial** (`source/anim_kernel.h`), used
identically by every implementation. Racing libm `sinf()` on the PPU against a
polynomial on the SPU would flatter the SPU by the difference between the two
functions and prove nothing.

Implementations compared:

| | Layout | Width |
|---|---|---|
| PPU scalar AoS | array of structs | 1 |
| PPU scalar SoA | struct of arrays | 1 |
| **PPU AltiVec SoA** | struct of arrays | 4 |
| **SPU SIMD SoA** | struct of arrays | 4 |

The PPU is given AltiVec because it *has* a 128-bit SIMD unit, and the
Makefile already turns on `-ftree-vectorize` for the audio decoders on exactly
that reasoning. A hand-vectorised SPU against a scalar PPU would measure the
vector width, not the processor.

Sections:

1. PPU baselines at N = 1,000 / 5,000 / 10,000 / 20,000
2. SPU/PPU agreement (bit-level cross-check — a wrong simulation is not a fast one)
3. SPU scaling, 1–5 workers, same four N
4. Single- vs double-buffered DMA; SoA vs AoS; chunk-size sweep 32→2048
5. Round-trip synchronisation cost: busy-poll vs signal-notification
6. **PPU memory bandwidth** — main→main, main→VRAM, VRAM fill, VRAM
   read-modify-write, VRAM read-only
7. SPU MFC bandwidth at 128 B → 16 KB
8. **Can an SPU DMA into RSX local memory at all?** (run last and in its own
   thread group: if the MFC cannot translate that mapping the group takes a
   storage exception, so everything else must already be on disk)
9. 60 Hz stability: 300-frame flip loops, bare / +PPU AltiVec 10k / +3 SPU
   workers 10k / +3 SPU workers overlapped with RSX work

### Local store, measured at link time

```
   text    data     bss     dec   filename
  11704     832  131600  144136   spubench_spu.elf
```

144,136 bytes of 262,144 — leaving ~118 KB for stack. The `bss` figure is the
double-buffered working set: 2 buffers × 8 fields × 4 B × 2048 objects =
128 KB.

That number was itself a finding. Declaring the SoA buffers, the AoS staging
area and the bandwidth scratch as three separate arrays **failed to link** —
`ld: .bss exceeds local store range`. They now alias one arena. A worker that
wants more than 256 KB of working set does not exist; it streams or it does
not run.

### A protocol bug found in review, before the hardware run

The PPU publishes a job by writing the command block then bumping `seq`, and
the worker polls `seq` over DMA. The first draft copied a freshly built
`SpuCmd` (with `seq = 0`) over the live block. A worker poll landing mid-copy
would read `seq == 0`, see it differ from `last_seq`, and run a job assembled
from a mix of the new command's fields and the previous one's. A mismatched
`(first, count)` pair is not a wrong answer — it is a DMA *past the end of the
arrays*, writing back over the heap.

Fixed by carrying the currently published `seq` through the struct copy and
publishing the new one afterwards, plus a bounds check on the worker side
(`count_max`) so a torn block is rejected rather than executed. Worth
recording because it is the characteristic hazard of this whole architecture:
on Cell the DMA engine will happily scribble anywhere the effective address
points.

---

## Deliverable 2 — measured PPU vs SPU animation

REAL PS3 RESULT. Timebase 79.8 MHz (12.531 ns/tick), display 1920×1080.
Best of 32 reps.

### PPU

| Objects | scalar AoS | scalar SoA | **AltiVec SoA** | AltiVec speed-up | % of a 16.67 ms frame |
|---:|---:|---:|---:|---:|---:|
| 1,000 | 342 µs | 338 µs | **25.5 µs** | 13.3× | 0.2 % |
| 5,000 | 1,731 µs | 1,689 µs | **127 µs** | 13.3× | 0.8 % |
| 10,000 | 3,546 µs | 3,427 µs | **254 µs** | 13.5× | 1.5 % |
| 20,000 | 7,615 µs | 7,127 µs | **673 µs** | 10.6× | 4.0 % |

**The headline is AltiVec, not the SPU.** 13.3× from vectorising a loop the
PPU was already capable of running that way. Note also that SoA barely helps
the *scalar* PPU (342 → 338 ns/object) — the layout change only pays once it
enables SIMD, which is a useful thing to know before restructuring anything.

### SPU

| Objects | Workers | Wall | Compute | DMA in | DMA out | Sync |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 1 | 162 µs | 109 | 1.9 | 48.7 | 2.4 |
| 10,000 | 2 | 92 µs | 54.6 | 3.6 | 32.3 | 1.4 |
| 10,000 | 3 | **68 µs** | 36.5 | 4.2 | 25.6 | 2.2 |
| 10,000 | 4 | 59 µs | 27.3 | 5.6 | 23.8 | 2.3 |
| 10,000 | 5 | 49 µs | 21.8 | 6.5 | 18.9 | 2.0 |
| 20,000 | 3 | 131 µs | 72.9 | 4.4 | 50.6 | 2.7 |

Compute scales **perfectly linearly** (109.2 / 54.6 / 36.5 / 27.3 / 21.8 —
that is 1/N to three figures). DMA-out does not (48.7 → 18.9), because it is
bandwidth-bound rather than work-bound, and it becomes the larger half of the
job past two workers.

Against the PPU's own best effort, at 10,000 objects: 1 SPU is 1.6×, 3 SPUs
are 3.7×, 5 SPUs are 5.2×.

### Correctness

> after 8 frames on 8192 objects, worst relative difference = 7.910e-05
> (field vy, index 4325) — PASS

The SPU and PPU are running the same simulation. Not bit-identical, and it
should not be: AltiVec's `vmaddfp` and the SPU's `spu_madd` are both fused
multiply-adds that round differently from a scalar multiply-then-add, and
after eight frames of feedback through velocity that shows up in the fifth
decimal place. 8e-05 on a value in pixels-per-second is far inside what any
renderer can express.

---

## Deliverable 4 — DMA, buffering, layout and local store

### Layout is worth 6.7×

REAL PS3 RESULT, 1 worker, 10,000 objects:

| Variant | Wall | Compute | DMA in | DMA out |
|---|---:|---:|---:|---:|
| SoA, double-buffered | **163 µs** | 109 | 1.9 | 48.8 |
| SoA, single-buffered | 185 µs | 109 | 26.1 | 47.6 |
| **AoS, double-buffered** | **1,082 µs** | 994 | 25.5 | 60.5 |

The brief describes an object as seven fields — an array-of-structs shape.
**That shape costs 6.7× on an SPU**, because every field access straddles a
quadword boundary and needs a shuffle before it can be SIMD'd. If any of this
ships, the particle state has to be stored struct-of-arrays.

Double buffering does exactly what Part 8 of the brief asks about: **DMA-in
drops from 26.1 µs to 1.9 µs — 93 % of the input transfer disappears behind
compute.** DMA-out does not hide (47.6 → 48.8), because the last write-back
still has to be waited on before the job can report completion. Worth knowing:
the achievable win from pipelining here is the input side only.

### Chunk size — where DMA overhead stops dominating

| Objects/chunk | Bytes in flight | Wall | DMA out |
|---:|---:|---:|---:|
| 32 | 1 KB | 293 µs | 130 µs |
| 64 | 2 KB | 225 µs | 88 µs |
| 128 | 4 KB | 187 µs | 64 µs |
| 256 | 8 KB | 169 µs | 53 µs |
| **512** | **16 KB** | **162 µs** | 49 µs |
| 1024 | 32 KB | 161 µs | 47 µs |
| 2048 | 64 KB | 162 µs | 47 µs |

**The crossover is 256–512 objects, i.e. 8–16 KB per transfer.** Below 128 the
per-transfer overhead dominates and the job costs nearly twice as much; above
512 there is nothing left to win. Anything shipped to an SPU should be sized
in that band.

### Raw MFC bandwidth

| Transfer size | LS → main | main → LS |
|---:|---:|---:|
| 128 B | 3,196 MB/s | 2,257 MB/s |
| 1 KB | 12,920 MB/s | 8,889 MB/s |
| 4 KB | 19,251 MB/s | 12,193 MB/s |
| 16 KB | **21,682 MB/s** | **13,448 MB/s** |

For scale: the PPU's own `memcpy` in main memory measured **398 MB/s**. The
MFC at 16 KB is **54× that**. Main memory is not remotely the constraint an
SPU faces; its own local store is.

### Synchronisation

| Mechanism | Best | Mean |
|---|---:|---:|
| Busy-poll a command block over DMA | 2.01 µs | 3.71 µs |
| `sysSpuThreadWriteSignal` (one LV2 syscall) | 2.16 µs | 3.17 µs |

Both are ~2–4 µs round trip, and **the syscall is not the expensive one** —
which is the opposite of what I expected and means a persistent worker does
not need to burn an SPU busy-polling. Signal notification is the right choice:
same latency, and the SPU is parked consuming no bus bandwidth between jobs.

### Local store

```
static (code + data) 144,272 bytes
stack pointer at     260,976
free between them    116,704 bytes of 262,144
```

Of the 144 KB static, **128 KB is the double-buffered working set** (2 × 8
fields × 4 B × 2048 objects) and only ~12 KB is code. The constraint is real
and it bites early: declaring the SoA buffers, the AoS staging area and the
bandwidth scratch as three separate arrays **failed to link** —
`ld: .bss exceeds local store range`. They now alias one arena.

---

## Deliverable 5 — recommendation for UI SPU architecture

### The governing measurement

REAL PS3 RESULT, PPU memory bandwidth:

| Operation | Time | Rate |
|---|---:|---:|
| main → main memcpy (4 MB) | 10.53 ms | 398 MB/s |
| main read-modify-write (4 MB) | 8.63 ms | 486 MB/s |
| main → VRAM memcpy (one framebuffer) | 5.47 ms | **767 MB/s** |
| VRAM flat fill, 4 B stores | 6.44 ms | 1,288 MB/s |
| **VRAM read-modify-write** | **1,446 ms** | **5.7 MB/s** |
| **VRAM read only** | **1,081 ms** | **7.7 MB/s** |

Writing to video memory is *faster than writing to main memory* — write
gathering works, and the card blits are on the right path already. Reading it
back is **100× slower**, and that is the path every glyph takes.

This also settles empirically that `color_buffer[]` really is in RSX local
memory and not in the host region: if it were main memory, reads would have
come back near 400 MB/s, not 7.7.

**A caveat I should not paper over:** that read loop was written `volatile`,
which forces one load and one store per iteration with no batching. The real
glyph blitter is not volatile, so the hardware may keep several reads in
flight and do better. The measured 7.7 MB/s is therefore a *floor*, and the
true glyph cost is somewhere between it and the ~400 MB/s main-memory figure.
It does not change the architecture — the fix is to stop reading VRAM, not to
read it more cleverly — but the exact multiplier on today's text cost is not
pinned down, and the honest next measurement is to time the real XMB frame
rather than a synthetic loop.

### What 3.84 MB of Home-screen cards costs

| Path | Time |
|---|---:|
| PPU memcpy at 767 MB/s (today) | 5.0 ms |
| SPU DMA at 3,314 MB/s | **1.2 ms** |

### Recommended phasing

**Phase 1 — no SPU.** In order of value per unit of risk:

1. **Stop reading VRAM.** Compose text (and anything alpha-blended) into a
   main-memory staging buffer and let the RSX composite it as a texture.
   The app already does exactly this for the player HUD
   (`hud_overlay_alloc()`, `source/player/hud/hud_draw.cpp:64`) — so this is
   adopting a pattern that already works on this hardware, not inventing one.
   Blending in main memory runs at 486 MB/s instead of 5.7.
2. **Vectorise the animation state with AltiVec + SoA.** 13.3×, measured, and
   it needs no SPU, no fallback path and no new failure mode. At 10,000
   objects it is 1.5 % of a frame.
3. **The scaled-blit divide fix** (already in the tree, Part 1.3).

Phase 1 alone very likely delivers the design's fluidity target. It should be
done and measured *before* any SPU work, because if it succeeds the SPU case
gets weaker, and that is a result worth having.

**Phase 2 — SPU, only if Phase 1 leaves a gap.** A tile compositor: the SPU
pulls card bitmaps from main memory at 13.4 GB/s, composites with alpha
**inside local store** where reads are free, and DMAs finished tiles to the
framebuffer at 3.3 GB/s. This is the design the measurements actually point
at, and its value is in the 3.3 GB/s figure, not in any arithmetic.

**Do not build an SPU animation worker.** The measurement does not support it.
254 µs of PPU AltiVec is not a problem that needs three SPUs and a fallback
path to solve.

### Priority visual workloads, against the measurements

- **A. Background waves** — already on the RSX and already cheap (3,120
  vertices/frame through the immediate-mode FIFO, which lives in *main*
  memory, not VRAM). No SPU. No change.
- **B. Particle field** — a few thousand particles is ~100 µs of PPU AltiVec.
  Run it on the PPU. The open question is not the physics, it is how the RSX
  draws them; that is a FIFO-throughput question this benchmark did not
  measure.
- **C. Focus / card transforms** — a few dozen transforms per frame. This is
  noise at any of the measured rates. The cost was never the transform, it was
  the *rescaled blit* it implies (Part 1.3).
- **D. Music-reactive animation** — the existing 2048-point FFT runs on the
  music screen only and feeds ~24 band values per frame. Moving an FFT to an
  SPU to produce 24 floats would be a textbook case of using an SPU because it
  is there. Leave it.

---

## Deliverable 8 — memory budget for SPU work

Against the ~187 MB already committed in main memory (Part 1.8), with only a
few MB of genuine slack:

| Item | Size | Where |
|---|---:|---|
| SPU code + static data | 144 KB | local store, per SPU |
| — of which working set | 128 KB | local store |
| Local store left for stack | 117 KB | local store |
| Particle SoA field, 4,096 objects | 128 KB | main |
| Command + result blocks, 6 workers | 1.5 KB | main |
| Tile staging (Phase 2), 8 × 128×128×4 | 512 KB | main |
| **Total new main-memory cost** | **< 1 MB** | |

This fits. It is also worth stating what does *not*: anything that wanted a
full-screen main-memory shadow buffer costs 7.91 MB at 1080p — which the HUD
already pays once, and a second one would be uncomfortable at the current
reservation levels.

---

## Deliverable 9 — fallback architecture

The benchmark is itself the proof that the fallback shape works: it runs the
PPU sections, then attempts `sysSpuInitialize` / `sysSpuImageImport`, and every
SPU section is inside `if (spu)`. Had either failed it would have logged the
return code and produced PPU-only results rather than dying.

For the app:

1. **The PPU path is the implementation; the SPU path is an accelerator.**
   Never the only implementation of anything. This follows directly from the
   measurements — PPU AltiVec at 1.5 % of a frame is a perfectly good
   implementation, not a degraded mode.
2. **Fail closed at init.** `sysSpuInitialize`, `sysSpuImageImport`,
   `sysSpuThreadGroupCreate` and `sysSpuThreadGroupStart` all return status;
   any non-zero disables the accelerator for the session and logs the code.
3. **Watchdog every dispatch.** The benchmark uses a 5 s bound and returns 0
   on expiry. In the app the bound should be one frame: if the workers have
   not reported by the time the frame needs them, use the PPU result for that
   frame and disable the accelerator after a small number of misses. Never
   block a frame on an SPU.
4. **Bounds-check on the worker side.** A torn command block must be rejected,
   not executed — see the `count_max` guard in Part 2. On Cell the DMA engine
   will write wherever the effective address points.
5. **Give the SPUs back before playback.** `cellVdec` wants 3 of the 6. The UI
   worker group must be stopped and joined when a video starts, and this is
   *not* optional bookkeeping — it is the difference between a decode that
   fits and one that does not.
6. **Stability is measured, not assumed** — see below.

### Does SPU work disturb the frame loop?

REAL PS3 RESULT, 300 frames each:

| Configuration | Mean | Worst | > 16.7 ms | > 33.4 ms |
|---|---:|---:|---:|---:|
| Flip only | 16.64 ms | 16.75 | 56/300 | **0** |
| + PPU AltiVec, 10k objects | 16.63 ms | 16.95 | 94/300 | **0** |
| + 3 SPU workers, 10k objects | 16.58 ms | 16.77 | 51/300 | **0** |
| + 3 SPU workers overlapped with RSX | 16.62 ms | 16.75 | 60/300 | **0** |

Every configuration holds 60 Hz. **Not one frame in 1,200 was dropped**, and
the variation between configurations is smaller than the run-to-run noise
(the SPU rows actually score *better* than the bare flip loop, which is not a
real effect). Three SPUs working every frame cost the display loop nothing
measurable.

This is the safety result the brief asks for, and it is clean.

---

## Part 6 — TrueHD: profiled

Profile of the vendored FFmpeg MLP/TrueHD decoder, x86-64 host,
`-O2 -fno-inline`, 120 repetitions of the 5.1 fixture (`tests/prof_truehd.c`,
added for this):

| % time | Function | What it is | SPU suitability |
|---:|---|---|---|
| 35.2 | `read_huff_channels` | entropy decode of residuals | **hostile** — serial, bit-oriented, branchy |
| 33.0 | `mlp_filter_channel` | FIR + IIR predictor | friendly arithmetic, **but recursive** |
| 8.8 | `av_crc` | frame CRC | table lookup, serial |
| 6.6 | `ff_mlp_pack_output` | output packing | **ideal** — parallel |
| 6.6 | `truehd_map_block` | channel reorder + int32→float | **ideal** — parallel |

`mlp_filter_channel` looks like the obvious SPU kernel and is not:

```c
for (i = 0; i < blocksize; i++) {
    for (order = 0; order < firorder; order++) accum += (int64_t)firbuf[order] * fircoeff[order];
    for (order = 0; order < iirorder; order++) accum += (int64_t)iirbuf[order] * iircoeff[order];
    result = ((accum >> filter_shift) + residual) & mask;
    *--firbuf = result;              /* <- feeds the next iteration */
    *--iirbuf = result - accum;      /* <- feeds the next iteration */
}
```

`result` re-enters the filter history, so it cannot be vectorised along
samples. The available parallelism is **across channels**, and the profile
shows only ~6.4 calls per access unit at a blocksize of 40 samples — about
480 multiply-accumulates each. That is far below the granularity at which
shipping work to another processor pays for the trip.

**Conclusion for TrueHD: leave it on the PPU.** Only ~13 % of decode time
(`pack_output` + `map_block`) is embarrassingly parallel. Even assuming the
filter could be batched per access unit and doubled in speed, Amdahl caps the
whole decoder at about **1.3×** — for a substantial restructuring of a
vendored decoder whose bit-exactness against ffmpeg is currently a *passing
test*, and which already works on hardware. The risk/benefit is bad and the
measurement says so.

---

## Part 7 — DTS-HD MA (XLL): structurally the better candidate

No host profile: DTS-HD MA cannot be synthesised (no free encoder exists — see
`HANDOFF.md`), so this is a **structural reading of `dca_xll.c` and
`dcadsp.c`, not a measurement**, and is labelled as such.

The field evidence for the cost is already in hand and is severe. From the
heartbeat logs: with XLL active, average **3.7 fps and only 11 % of frames at
≥22 fps, with the read-ahead ring full** — i.e. the bytes were arriving and the
PPU could not keep up. With XLL off, 23.0 fps / 94 %.

Structurally XLL is much friendlier than MLP:

| Kernel | Shape | Parallel? |
|---|---|---|
| Inverse adaptive prediction (`chs_filter_band_data`) | `err += buf[j+k] * coeff[...]`, recursive | serial per channel, **independent across up to 8 channels** |
| `decor_c` | `dst[i] += (src[i]*coeff + 4) >> 3` | **fully parallel** |
| `dmix_sub/add/scale/scale_inv` | int32 multiply-shift over `nsamples` | **fully parallel** |
| `filter0`/`filter1`/`assemble_freq_bands` | `dst[i] -= mulNN(src[i], coeff)` | parallel within each call |
| `chs_assemble_msbs_lsbs` | shift and merge | **fully parallel** |

And critically, the **granularity is right**: XLL carries `nframesamples` in
the hundreds per channel per frame, against MLP's 40. That is a DMA-sized job,
not a function call.

So the honest ordering is the reverse of the brief's: **DTS-HD MA is the
audio workload worth moving to an SPU, and TrueHD is not.** TrueHD already
works; XLL is disabled today partly because it is too slow and partly because
it is wrong on big-endian (open problem 1). The endianness bug must be fixed
first regardless — an SPU port of a decoder that produces wrong samples just
produces them faster.

---

## Deliverable 10 — concrete integration plan

Ordered by value per unit of risk, which given the measurements is **not** the
order the brief assumed.

### Step 0 — done

- Scaled-blit divide removed (Part 1.3). In the tree, exact, verified.
- Benchmark checked in at `tools/spubench/` with its raw output, so every
  number here is re-runnable and any future claim can be checked against a
  fresh run rather than against this document.

### Step 0.5 — the real XMB frame, measured

REAL PS3 RESULT, steady state, 1920×1080, microseconds per frame:

| Phase | µs | share of work |
|---|---:|---:|
| `vsync` — parked waiting for vblank | 8,490 | *(not work)* |
| **`gpu` — clear + wave submit** | **2,190** | **27 %** |
| `sync` — `rsxSync()` fence | 465 | 6 % |
| `cards` — thumbnail memcpy | 1,364 | 17 % |
| **`text` — glyph compositing** | **2,770** | **34 %** |
| `chrome` — hints + tab bar | 1,280 | 16 % |
| `flip` | 10 | — |
| `other` | 105 | — |
| **total work** | **8,190** | |
| **frame** | **16,680** (a locked 60 Hz) | |

Glyph counters: **120 glyphs, 4,445 blended pixels, 955 opaque**.

Three things this settles, two of which contradict what the estimate predicted.

**1. The VRAM read-back is real but small.** 2,770 µs over ~5,400 glyph pixels is
**513 ns/pixel** — within 30 % of the 697 ns/pixel the `volatile` loop in
Deliverable 5 predicted. The synthetic figure was *accurate*, not pessimistic.
What was wrong was my pixel count: I estimated 600–1,500 glyphs at ~150 px
each; the real screen draws **120 glyphs averaging 45 px of coverage**. So the
read-back costs 2.8 ms, not the 26–100 ms that estimate implied.

Against the decision rule stated here: text is 17 % of the frame and 34 % of
the work, on a frame with **51 % idle headroom**. That is worth fixing
eventually and is *not* urgent. Step 1 gets demoted.

**2. The biggest single cost is submitting vertices, and nobody predicted
that.** `gpu` = 2,190 µs is the PPU pushing ~4,660 immediate-mode wave
vertices into the FIFO plus one clear — **470 ns per vertex**. More than text.

That matters enormously for the new design, because it is the one cost that
*scales with the brief*:

| | vertices | PPU submit at 470 ns |
|---|---:|---:|
| wave today (3 bands) | 4,660 | 2.19 ms |
| wave at 4 bands | ~6,200 | ~2.9 ms |
| + 2,800 particles as quads | +11,200 | **+5.3 ms** |
| **total** | ~17,400 | **~8.2 ms** |

Add the existing 4 ms of cards, text and chrome and the frame is at ~12 ms
before any of it is drawn — against 16.68 ms. **Immediate-mode submission is
what breaks the new UI, not pixel blending.**

This also explains why Sony generates wave geometry on an SPU and does not
push it from the PPU (see `wave-spec.md` §3b): their per-frame input is 64
bytes. It is the same problem, and they solved it by not having the PPU touch
the vertices at all.

**Revised priority:** get vertex submission off the PPU — vertex arrays, which
`player_rsx.cpp` already proves work with `rsxInvalidateVertexCache()`. That is
now the first thing to do, ahead of the text staging buffer.

**Caveat, stated because it limits the conclusion:** during this run the
library never loaded (`detect_tabs: http=-1 for /Users/*/Views`), so the screen
was nearly empty and **`cards` = 1,364 µs is not representative**. A run
against a reachable server is needed before the card figure means anything.

### The same measurement with the library actually loaded

REAL PS3 RESULT, 23 frame samples across two screens, 3 libraries loaded.

| | Home | Library grid | Home, first frame |
|---|---:|---:|---:|
| `cards` | **6,446** | 4,132 | 10,791 |
| `text` | **4,693** | 2,890 | 4,721 |
| `gpu` (wave submit) | 2,195 | 2,199 | 2,279 |
| `chrome` | 1,653 | 2,121 | 1,763 |
| `sync` | 449 | 437 | 413 |
| **total work** | **15,661** | 11,918 | **20,226** |
| frame | 16.84 ms | 16.68 ms | **23.21 ms** |
| **utilisation** | **93 %** | 71 % | **over budget** |
| glyphs / blended px | 207 / 7,329 | 113 / 6,365 | 205 / 7,312 |

**Home runs at 93 % of a 60 Hz frame with about 1 ms to spare.** The first
frame after thumbnails arrive misses vsync outright at 23.21 ms.

This corrects the interim reading above, which was taken on a nearly-empty
screen and concluded that text was not worth fixing. With real content:

- **`cards` is the single largest cost — 6.4 ms, 41 % of the work.** Every
  visible thumbnail is `memcpy`'d from main memory into VRAM **every frame**,
  whether or not anything about it changed. The earlier 1.4 ms figure was an
  artifact of having almost nothing to draw, exactly as flagged.
- **`text` is second at 4.7 ms, 30 % of the work** — the VRAM
  read-modify-write path. "Step 1 gets demoted" was wrong; it was a conclusion
  drawn from a broken run and the real data reverses it.
- `gpu` is flat at ~2.2 ms across every screen, as expected: the wave does not
  care what the UI is showing.

### Result: cards moved to the RSX

REAL PS3 RESULT, same build, `jellyfin_gpucards.txt` off then on, Home screen:

| | CPU blit | RSX quads | change |
|---|---:|---:|---:|
| `cards` | 6,718 | **2,146** | **−68 %** |
| `text` | 4,693 | 4,649 | — |
| `gpu` | 2,195 | 2,138 | — |
| `chrome` | 1,653 | 1,633 | — |
| **work** | **15,845** | **11,758** | **−26 %** |
| idle headroom | ~1,000 | **4,920** | 5× |
| utilisation | 93 % | **70 %** | |

Frame stayed locked at 16.68 ms across six minutes. The phases that were not
touched did not move, which is the control that makes the saving credible.

**`cards` did not go to zero, and the residue is worth naming.** The GPU pass
draws the image; `xmb_draw_card` still runs afterwards and does:

- the selection border — four `drawRect` plus four **`drawRectBlend`** calls,
  and `drawRectBlend` is the VRAM read-modify-write path at ~5.7 MB/s. Around
  1,200 blended pixels per selected card.
- progress strips, and dim placeholders for cards still decoding
- `thumb_gpu_texture()` a second time (the CPU path calls it to decide whether
  to skip its blit), so every visible card does two locked linear scans of the
  32-slot table per frame instead of one

So the next bite out of `cards` is the same read-modify-write problem the text
phase has, not the image copy — which folds it into the text-staging work
rather than needing anything new.

There is **no headroom on Home**. Not "some" — about one millisecond. Adding a
fourth wave band and a few thousand particles to this renderer cannot work;
the frame is already full.

Roughly what the three fixes are worth, against the 15.7 ms Home frame:

| Change | Mechanism | Recovers |
|---|---|---:|
| Cards → RSX textured quads | upload on change, not per frame; RSX draws them | most of **6.4 ms** |
| Text → main-memory staging + upload | blend at 486 MB/s instead of 5.7 MB/s | most of **4.7 ms** |
| Wave → vertex arrays | stop paying 470 ns/vertex of PPU submission | part of **2.2 ms** |

That is on the order of 10–12 ms recovered from a 15.7 ms frame — which is
what creates the room the new design needs. **All three are renderer work and
none of them needs an SPU.**

The SPU's place remains where the measurements put it: `cards` at 6.4 ms is
PPU `memcpy` into VRAM at 767 MB/s, and an SPU DMAs into VRAM at 3,314 MB/s.
If, after the cards move to textures, upload bandwidth is still a cost, that
is the workload to hand over — and not before.

### In-app SPU verification

REAL PS3 RESULT, 4,096 objects, from the opt-in self-test:

```
sputest: PPU AltiVec 4096 objects, 108 us/frame
spu: pool up, 3 worker(s)
sputest: SPU x3 4096 objects, 29 us/frame (wall 24, compute 14, dma 9)
sputest: worst relative difference 6.473e-05 -- PASS
spu: pool down, SPUs released
```

**3.7× over the PPU's own vectorised path**, matching the standalone
benchmark, with the two implementations agreeing to 6.5e-05. The pool comes
up, dispatches, collects, and gives the SPUs back cleanly inside the real
application.

### Step 1 — stop reading video memory *(demoted — see above)*

Deliverable 5's caveat has to be closed before the renderer is restructured on
the strength of it. The XMB loop now reports its own per-phase cost to
`player_log.txt` once a second when `plog=1`:

```
xmb: frame=..ms vsync=.. gpu=.. sync=.. cards=.. text=.. chrome=.. flip=..
     other=.. (us/frame) gl=.. bpx=.. opx=..
```

`cards` is the memcpy phase, `text` the glyph phase, `sync` the `rsxSync()`
stall. `bpx` and `opx` split glyph pixels into those that took the
read-modify-write branch (one uncached video-memory read each) and those that
hit the opaque store-only path — and that ratio is the actual open question,
because if most glyph coverage is fully opaque then the read path costs far
less than the pixel count alone suggests.

The counters accumulate per glyph rather than per pixel, so the instrument
does not measurably change what it measures.

**Decision rule.** If `text` turns out to be a small share of `frame`, Step 1
is not worth the disruption and the honest conclusion is that the synthetic
`volatile` loop overstated it — say so and move on. Only if `text` dominates
does the rest of this plan follow.

### Step 1 — stop reading video memory *(no SPU)*

The single largest win available, and it needs no new failure mode.

- Add a main-memory staging layer for the XMB the way the player HUD already
  has one: compose text and alpha-blended chrome into `memalign` memory, then
  hand it to the RSX as a texture.
- `blit_coverage()` and `drawRectBlend()` then blend against main memory at
  486 MB/s instead of video memory at 5.7–7.7 MB/s.
- Card blits stay exactly as they are — `memcpy` into VRAM at 767 MB/s is
  already the fast path and should not be touched.
- **Measure the XMB frame time before and after.** This is also the
  measurement that pins down the caveat in Part 5: if frame time barely moves,
  the `volatile` read loop overstated the glyph cost and that is worth knowing.

### Step 2 — vectorise animation state *(no SPU)*

- Store whatever the new design animates (focus springs, shelf offsets,
  particles) struct-of-arrays.
- Port the update to AltiVec using `source/anim_kernel.h`'s shape as the
  reference — 13.3× measured, and `ppu_step_soa_vmx()` in
  `tools/spubench/source/bench_ppu.c` is a working implementation to copy.
- Budget: 10,000 objects = 254 µs = 1.5 % of a frame.

### Step 3 — decide whether an SPU is still needed

With Steps 1 and 2 done, re-measure. If the frame is comfortable, **stop
here** — and record that the answer to the brief's question was "the PPU, used
properly". That is a legitimate outcome and the measurements currently point
at it.

### Step 4 — only if Step 3 leaves a gap: the tile compositor

- One SPU thread group, **3 workers** (what is free during playback), created
  once at XMB entry and joined before `vdec_open()`.
- Job = one screen tile. SPU DMAs the card bitmaps it needs from main memory
  (13.4 GB/s), composites with alpha in local store, DMAs the tile to the
  framebuffer (3.3 GB/s).
- Chunk the transfers at 8–16 KB, the measured crossover.
- Signal notification for wake-up (2.16 µs, and the SPU parks between jobs).
- Double-buffer the input side — that is where pipelining actually paid
  (26.1 µs → 1.9 µs); do not expect the output side to hide.
- Every fallback rule in Deliverable 9 applies.

### Audio — separate track, different order from the brief

- **TrueHD: do nothing.** Profiled; capped near 1.3× by Amdahl; the
  restructuring risk is worse than the win. See Part 6.
- **DTS-HD MA: the candidate, but not yet.** Structurally right (Part 7) —
  parallel int32 loops at hundreds of samples per channel per frame. But the
  prerequisite is coverage, not code: the current verification rests on one
  5.1/24-bit/48 kHz fixture that is not in the repo. Untested configurations
  are 16-bit (the `dcahd_widen_s16p` path), 96 kHz, 7.1 and non-residual
  streams. Widen that before moving any of it to another processor.

---

## Part 12 — Credentials

Checked: no Jellyfin token, password or API key appears in the benchmark or in
anything added here. Nothing added reads or logs `jellyfin_config.txt`.

---

## Part 13 — Emulator vs hardware

Every number in this document is labelled:

- **REAL PS3 RESULT** — every performance figure in Deliverables 2, 4, 5 and
  9. `spubench.txt` is written by the console itself and says so in its
  header; the display mode, timebase frequency and local-store figures come
  from the same run. Raw output: `tools/spubench/RESULTS-real-ps3.txt`.
- **HOST RESULT (x86-64)** — the TrueHD profile in Part 6. Used only for the
  *distribution* of time across functions, which is a property of the
  algorithm, never for absolute cost.
- **STRUCTURAL, NOT MEASURED** — the XLL analysis in Part 7.
- **Derived** — the per-frame pixel budgets in Part 1.4 are computed from the
  layout constants at the console's real 1920×1080, then multiplied by
  measured bandwidths. The geometry is exact; the multiplication assumes the
  synthetic bandwidth figures transfer to the real draw loop, which is the
  caveat called out in Deliverable 5.

No RPCS3 results are used anywhere.

## One thing this did not measure

RSX FIFO throughput. Part 4B of the brief asks about a particle field the RSX
draws, and the immediate-mode path pushes two `rsxDrawVertex` calls per vertex
into a command buffer in *main* memory. The existing wave already sustains
3,120 vertices/frame that way, but a few thousand particles is 16,000+
vertices and nothing here establishes where that path saturates. If the
particle field is built, that is the measurement to take first — the physics
is settled (it is ~100 µs on the PPU) and the drawing is not.
