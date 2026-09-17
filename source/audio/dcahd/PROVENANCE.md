# Vendored DTS decoder with DTS-HD MA lossless support (from FFmpeg)

- **Upstream project:** FFmpeg — https://ffmpeg.org/
- **Obtained from:** `https://github.com/FFmpeg/FFmpeg`, `master`, commit
  `68845e2`, 2026-09-17.
- **Licence:** LGPL-2.1-or-later (every file taken carries the LGPL header) —
  compatible with this application's GPL-3.0 licence. No GPL-only FFmpeg file
  is used.

## Why this exists alongside `../dca/` (libdca)

libdca decodes the **DTS core substream only**. It was written in 2004, years
before DTS-HD existed, and has no code for the extension substreams at all.
The lossless audio in a DTS-HD Master Audio track lives in the **XLL extension
substream**, so libdca can never reach it — the best it can do is the lossy
core, at up to 1509 kbps.

That is the whole asymmetry with TrueHD. MLP/TrueHD is natively a single
lossless stream, so decoding it at all means decoding it losslessly. DTS-HD MA
is a lossy core *plus* a lossless extension, and we only ever vendored a core
decoder.

FFmpeg's DCA decoder implements XLL (`dca_xll.c`), which is why it is here.

## What was taken

From `libavcodec/`: `dca.c`, `dca.h`, `dca_core.c`, `dca_core.h`,
`dca_exss.c`, `dca_exss.h`, `dca_xll.c`, `dca_xll.h`, `dcadec.c`, `dcadec.h`,
`dcadsp.c`, `dcadsp.h`, `dcadct.c`, `dcadct.h`, `dcahuff.c`, `dcahuff.h`,
`dcadata.c`, `dcadata.h`, `dcaadpcm.c`, `dcaadpcm.h`, `dcamath.h`,
`dca_sample_rate_tab.c`, `dca_sample_rate_tab.h`, `dca_syncwords.h`,
`dca_lbr.h`, `dcaenc.h` (the last only because `dcaadpcm.h` includes it).

Added to the shared `../mlp/ff/` tree: `libavcodec/unary.h`, `put_bits.h`,
`synth_filter.h`; `libavutil/fixed_dsp.c`, `fixed_dsp.h`, `float_dsp.h`,
`tx.h`, and a **stub** `downmix_info.h` (see below).

**No upstream file's contents were modified.**

## Deliberate omissions, and why they are safe

**`libavutil/tx` (the MDCT/FFT framework) is NOT vendored.** It is reached
only from the core decoder's *float* output path. DTS-HD MA decodes on the
**fixed-point** path, and there `dcadec.c` skips the core synthesis filter
entirely whenever an XLL substream is present:

```c
if (!(dca->packet & DCA_PACKET_XLL) && (ret = ff_dca_core_filter_fixed(s, 0)) < 0)
```

`av_tx_init` / `av_tx_uninit` / `ff_synth_filter_init` are stubbed in
`../dcahd_compat.c`. That keeps roughly 200 KB of transform code out of the
build. Fixed-point is also the correct path on principle: lossless output
requires exact integer arithmetic, and it yields int32 samples, which is the
same shape the TrueHD path already feeds to its channel map.

**`dca_lbr.c` (DTS Express) is NOT vendored.** LBR is a lossy low-bitrate
extension used for secondary audio and streaming profiles; it never carries
the lossless data this decoder exists to reach. Its entry points are stubbed
in `../dcahd_api.c` so the core declines LBR substreams cleanly rather than
pretending to decode them. This also drops its `bytestream`/MDCT dependencies.

**`downmix_info.h` is a local stub.** The real header includes `frame.h`,
whose full `AVFrame` collides with the minimal one the shim defines. The
decoder uses those types only to *export* downmix coefficients to a caller;
they never affect decoded audio, and this player maps channels to the PS3
layout itself. The stub lets `ff_dca_export_downmix_matrix` compile and
succeed harmlessly — it must SUCCEED rather than fail, because `dca_core.c`
and `dca_xll.c` both propagate its return value.

## Build shape

Two translation units, not one:

- `../dcahd_xll.c` — `dca_xll.c` alone.
- `../dcahd_api.c` — everything else.

They are split because `dca_xll.c` and `dca_core.c` both define a static
`get_array()`, which upstream never notices because they are separate TUs
there too. Everything they share across the boundary is `ff_`-prefixed and
external, so splitting costs nothing and modifies no upstream file.

`vlc.c`, `crc.c` and `reverse.c` are **not** compiled here — `mlp_api.c`
already compiles them and every symbol they export is external, so this
decoder links against that single copy.

This directory is on `INCLUDES` but deliberately **not** on `SOURCES`: the
PS3 Makefile compiles every `.c` under a `SOURCES` directory into one flat
`obj/`, and these files are `#include`d by the two wrappers instead.

## Footprint (PPC64 big-endian, -O2 -mcpu=cell)

| | |
|---|---|
| code + tables | ~200 KB |
| static RAM (bss) | ~130 KB |
| heap at runtime | a few hundred KB (XLL sample buffers + a 240 KB PBR buffer) |

Measured, not estimated. Comfortably affordable — free RAM during playback has
been observed at 16–33 MB.

## Status

Compiles and links for PPC64 big-endian, and the full application builds with
it in tree. **Not yet wired to the player**: it still needs an API entry point,
a channel map from FFmpeg's DCA speaker order to the PS3 CellAudio order, and
a bit-exactness test against `ffmpeg` on the host. Until then `adec_dts.cpp`
continues to use libdca's core-only path.
