# Canyon — the PS3 music visualizer, rebuilt

The stock XMB music player offers three visualizers: **Earth** (Q-Games' "Gaia",
fw 2.10), **Canyon** (the colourful one), and **Wave** (the XMB lines with the
`override/music_1` preset). This client now has Canyon. On the music screen,
**tap Square** to switch between Wave and Canyon (the choice is saved in
`jellyfin_visualizer.txt`), and **hold Square** (~half a second) for the next
Canyon preset. (L2 / R2 were the first binding; the music screen now uses them
to seek, as the video player does.)

## What the firmware does (4.93, traced statically)

| Piece | Where | What |
|---|---|---|
| audio tap | `soundvisualizer_plugin.sprx` | The audio player pushes PCM into a ring of 10 timestamped packets (0x5E00 bytes each, the module's 256 KB BSS). A table of 5 callbacks goes to `custom_render_plugin` through interface 1, slot +0xC (code `0x31f8`). |
| callbacks | sv `0x12ac / 0x1334 / 0x14b4 / 0x310 / 0x318` | latency, waveform (unused in 4.93), **spectrum**, stub, right-stick x/y |
| who gets them | custom_render `0x35b8` | They are passed as the **module-start argument of `qgl_canyon_app.sprx` only**. `qgl_gaia_app` (Earth) is started with NULL, and the lines/wave code never reads them. |
| analyser | sv `sub_1c9c` | → `source/music/sv_spectrum.h` (every constant is listed there) |
| row build | canyon `0xd188` (vtable +0x18) | 64 L bins low→high, then 64 R bins high→low: **bass makes the walls, treble the floor** |
| row pipeline | canyon `sub_943c`, `sub_61cc` | treble lift `exp(c·tri)`, normalise by `max(1,rowmax)`, four quarter means, one row per unit of distance (`POS SPEED` = rows/s), per-column lag, 4× Laplacian, smoothstep valley |
| presets | `dev_flash/vsh/resource/qgl/canyon.qrc` | 57 overrides (`canyon01-21`, `Voyage01-11`, `Bir_A01..C04`, `Blue01-04`, `Volcano01-04`, `Volvic01-05`), each 6 plain-text `.mnu` files |

The tools are `research24p/ps3fw.py` (PUP → decrypted SPRX) plus a linear
disassembler that annotates `lis/addi` strings and floats. No Sony code or data
is in this repository.

## What is ours

- **Rendering.** CPU-lit heightfield, plain coloured triangles through the
  existing passthrough programs, the JellyWave vertex layout and the same
  binding discipline as `wave_draw()`. Sony's shaders, normal map and
  bloom/feedback passes are not used. Loud ground glows in the line colour as
  a stand-in for the HDR glare.
- **Camera.** It is fixed. The presets' CAMERA numbers assume Sony's scene, so
  only `ZOOM` is used.
- **Wall lag.** `CY_LAG_MID/EDGE` are 0.55/0.90 rather than the traced
  0.7/0.995. At 22 rows/s, Sony's value gives the walls about a 9 s time
  constant. Revisit after a TV look.
- **The line.** It traces the newest row's profile at the horizon, because
  that edge is where the music currently being heard enters the land.

## Runtime

- Presets are read from the console's own `canyon.qrc` the first time Canyon is
  used, inflated with stb's zlib and parsed (≈7 KB kept). Without the file (for
  example on RPCS3), a built-in Jellyfin-coloured look is used.
- Two RSX vertex buffers of 33,028 vertices × 24 B (≈1.6 MB total) are
  allocated on first use and kept.
- Log lines: `canyon: N presets, K keys from canyon.qrc`, `canyon: ready ...`,
  and every 300 frames `canyon: <us>/frame (PPU) preset=<name> rows=... q=...`
  (q = the four quarter energies: L-bass, L-treble, R-treble, R-bass).

## Checks

- `tests/test_canyon.c` (host): window/remap shape, silence reads 0, sine bands
  and levels, MNU parse, QRCF walk, walls up for bass, the walls respond within
  1.5 s, silence settles, emit counts and vertex sanity. Mutants that break the
  remap or restore the 9 s lag both fail it.
- `tools/canyon_preview/preview.c`: a software rasteriser that draws exactly
  what `cy_emit()` hands the RSX, over a synthetic mix. Point it at an inflated
  `canyon.qrc` (not committed) to see the console's presets.

## Unverified

Nothing has been seen on a TV yet. The per-frame PPU cost is estimated at
~1–2 ms (the log line measures it). The GPU fill cost is not measured.
