# Wave renderer: investigation notes

The source comments in `source/ui/render/ui_wave.cpp` and `wave_render_map.h`
state what the code does and the rules it must keep. This file keeps the
measurements and incidents behind those rules, so they are not lost.

## Emulator vs hardware

On a real PS3 the framebuffer is in RSX-local VRAM. PPU writes to it are
uncached, and a full-screen CPU fill costs about 150-180 ms, so the background
is drawn by the RSX. RPCS3 shows the GPU-rendered surface from its
render-target cache on flip and drops later CPU writes to the same buffer, so
on the emulator every CPU-drawn element disappeared. The fix is to composite
the whole frame on the CPU in emulator builds.

An earlier version decided this at startup by timing one full-screen CPU
write. PPU write-gathering beat the threshold on real hardware, so a retail
PS3 went down the CPU path. Every frame then read back uncached VRAM and the
UI crawled. That is why the choice is `BUILD_FOR_RPCS3` at compile time.

## Vertex arrays

A comment in `ui_wave.cpp` used to say vertex-array fetch was "unreliable on
real hardware", and immediate mode was kept because of it. That was wrong.
`player_rsx.cpp` draws every video frame from interleaved vertex arrays with
three textures bound, and `hud_dim.cpp` draws a COLOR0 quad the same way, both
on this console. The early failure was a stale attribute binding. The two
rules now in the code (invalidate the vertex cache before every array draw,
reset the bindings after the last one) fix it.

Measured: immediate mode cost 2,129 us per frame at 1080p for the ~4,708
vertex legacy wave (two FIFO writes per vertex). It was the largest single
item in the Home frame. PPU writes to VRAM measured 767 MB/s, faster than to
main memory, so writing the array once is cheap.

The same comment block claimed per-vertex alpha did not survive on this
hardware, which is why the legacy colours are baked. Also wrong: both shaders
are passthrough. The real cause was that `wave_draw()` turned blending off
just before drawing the ribbons. Gate mode 2 draws with GPU blending.

## RSX-local store rules

Both incidents happened in Phase 1 of the XMB revamp, when the palette changed
from compile-time constants to runtime reads of `g_theme`. The values and
field widths did not change. Only GCC's store scheduling did.

- **Sub-word stores.** `WaveVert` colour used to be four `u8` fields. With
  constant colours GCC merged r/g/b/a into one 32-bit store. With runtime
  colours it emitted `stb` per channel (57 byte stores in `wave_draw` against
  25), and the first vertex of the first frame hung the console every time.
  Colour is now one packed `u32`.
- **Misaligned 64-bit stores.** Unpadded, `WaveVert` is 20 bytes, so every odd
  vertex starts on a 4-byte boundary. With constant colours GCC wrote the block
  as ten aligned `std`s at offsets 0, 8, ... 72. With runtime colours it wrote
  per vertex and emitted `std` at offsets 20, 28, 60 and 68, which are
  misaligned: hang on the first frame. `aligned(8)` makes it 24 bytes.
- **The `sync` barrier.** With constant colours the stores were hoisted well
  before the draw and the write-gather buffer drained in time. With runtime
  colours they were scheduled right against the draw calls, the RSX fetched a
  half-written array, and the GPU wedged on the first frame.

Every failure looked the same: black screen, console off the network, power
cycle to recover. The rule for anything writing vertex or texture data to VRAM
is whole aligned words, never bytes, and `sync` before the RSX reads it.

## Crest amplitude

The crest used to be one sine per ribbon (`WAVE_FREQ`, `WAVE_DPHASE`,
`s_wave_phase`). It is now the `wave_field.h` spring chain. Two normalisations
put the chain back on the old sine's pixel heights:

- `WF_NOMINAL_PEAK`: over 20,000 frames the chain's peak wandered in
  [0.198, 0.653]. The sine reached 1.0, so without this the ribbons are about a
  third shallower.
- `WF_DRIVE[li]`: amplitude is linear in drive, and the back layers run at
  0.85 and 0.70. With a single correction the measured spans were
  59.4 / 37.2 / 20.8 px against the sine's 60 / 44 / 30, because `WAVE_AMP`
  already tapers 30 / 22 / 15 and the taper was applied twice.

`WAVE_FIELD_DT` 1.25 matches the old drift: WK_W1 x 1.25 = 0.0081 rad per
frame against `WAVE_DPHASE[0]` = 0.008.

## JellyWave cost

- First hardware run (2026-09-21): building the geometry cost 7,411 us of PPU
  per call, 848 ns per vertex for 8,740 vertices, against an estimate of
  400-700 us. The frame went from 16.68 to 21.3 ms and lost 60 Hz. The GPU was
  not the problem: the `sync` bucket (PPU waiting on the RSX fence) fell from
  4,135 to 3,375 us. So the fix was to build less often: a rebuild cadence of 3
  amortised the build to about 2,470 us a frame.
- 2026-09-24: a build still cost about 6.3 ms and landed on the render thread
  on rebuild calls. Screens with about 10 ms of other work missed vsync on
  every rebuild frame. `xmb: frame=22.2ms` is 16.7 + 16.7 + 33.3 over three,
  a 60-60-30 cadence. The build moved to a worker on the PPU's second hardware
  thread. The cadence then dropped from 3 to 2, since a rebuild costs the
  render thread about 0.1 ms either way.
- Speed: 100% looked too fast on a TV (the band moves about +/-320 px against
  the legacy ribbons' +/-30). The default went to 50%, then to 20% on
  2026-09-24 ("a lot slower, fluid and floaty, not choppy").
- The 59.94 Hz vertex-program blend (2026-09-26) exists because at a cadence of
  2 each build is shown twice, which read as choppy.

## Rim blend

The rim is additive, as the design draws it and as stages 1-6 shipped it
(9d2fec3). 14428bb moved it to the standard blend at alpha 255 while chasing
the strobe, which painted the near-black rim colours opaque over the body:
dark tubes along every rolled edge. The strobe was actually the reuse-frame
legacy write. The additive blend was restored.

## Tab-bar hairline

The CPU version in `ui_widgets.cpp` read the framebuffer once per pixel to
composite its alpha falloff: 1,920 reads a frame, measured at 1,351 us
(704 ns a pixel, the PPU's VRAM read cost). That was 76% of the `cards`
bucket, on every screen. `wave_draw_divider_gpu()` replaces it with six
vertices.

## Audio mapping

- Response gain: after the first hardware look ("barely noticed", against a
  brief that asked for "restrained"), gain became a runtime file.
- Distinct bands (2026-09-24): verdict "it all moves at a similar intensity;
  you can't tell the lows, mids and highs apart". The log showed `drive` going
  0.62 to 1.02 (every layer about 65% taller, together) and `ts` at 1.2-1.3
  (everything faster, together), while per-band heights only separated by
  +/-15% (amp 0.88-1.16). Those per-band heights were also built from mixed
  bands. `wrm_distinct` holds the shared terms near rest and gives each layer
  one band. Later the same day, "the bass could be a bit more sensitive" led to
  the per-layer `WRM_DB_FLOOR` / `WRM_DB_RESP`.
