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
`synth_filter.c`,
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

`av_tx_init` / `av_tx_uninit` are stubbed in `../dcahd_compat.c`, which keeps
roughly 200 KB of transform code out of the build.

`synth_filter.c` **is** vendored, though — an earlier revision stubbed
`ff_synth_filter_init` too, on the assumption that the fixed path never needs
the QMF. That is only true when XLL is present. A **core-only** DTS stream
really does run `ff_dca_core_filter_fixed`, which calls through those function
pointers, and stubbing the init left them NULL. `tests/test_dts_hd.c` caught it
as a null-pointer crash on the first frame. `synth_filter_fixed` uses
`dcadct.c`, not `av_tx`; the float half of the file compiles but is never
called, because `AV_CODEC_FLAG_BITEXACT` pins the decoder to fixed point.

Fixed-point is the right path on principle anyway: lossless output requires
exact integer arithmetic, and it yields int32 samples — the same shape the
TrueHD path already feeds to its channel map.

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

## Correction: the core filter is NOT skipped when XLL is present

An earlier revision of this file claimed that `dcadec.c` *skips the core
synthesis filter entirely whenever an XLL substream is present*, quoting

```c
if (!(dca->packet & DCA_PACKET_XLL) && (ret = ff_dca_core_filter_fixed(s, 0)) < 0)
```

That was a misreading, and it mattered. In `decode_frame()` the XLL branch
does the opposite -- when a core substream is also present it calls
`ff_dca_core_filter_fixed()` **first**, then `ff_dca_xll_filter_frame()`:

```c
} else if (s->packet & DCA_PACKET_XLL) {
    if (s->packet & DCA_PACKET_CORE) {
        ...
        if ((ret = ff_dca_core_filter_fixed(&s->core, x96_synth)) < 0)
```

It has to. DTS-HD MA is frequently **residual-encoded**: the XLL substream
carries only the difference from the lossy core, and the lossless result is
core + residual. Real disc rips do this -- the Avatar remux tested here has
`residual_encode == 0` for all six channels, i.e. every channel is residual.
So the core decoder is on the lossless critical path, and anything that
corrupts the core corrupts the lossless output too.

## Two stubs that silently destroyed the output

`../dcahd_compat.c` used to define two upstream tables as zero-filled
placeholders, on the assumption nothing reachable read them. Both were read,
and neither failed loudly:

- **`ff_log2_tab[256]`** backs `av_log2_c()`, which shifts the value down to
  8 bits and then adds `ff_log2_tab[v]`. Zeroed, `av_log2()` returned only
  the multiple-of-8 part: `av_log2(0xff)` gave 0 instead of 7 and
  `av_log2(0x60f)` gave 8 instead of 10. `dcadec.c` derives `max_spkr` from
  `av_log2(ch_mask)`, and `dcaadpcm.c` derives `shift_bits` from
  `av_log2(max)`.
- **`ff_inverse[257]`** backs `FASTDIV(a,b)`, which `dca_core.c:538/543` uses
  to dequantize block codes. Zeroed, `FASTDIV` returned 0 for every input.
  It was also declared `uint64_t` here while upstream declares it
  `uint32_t`, so the indexing was wrong independently of the values.

Both are now vendored verbatim (`ff/libavutil/log2_tab.c` and
`ff/libavcodec/mathtables.c`, included from `../dcahd_compat.c`) rather than
re-stubbed, which removes the whole class of bug.

A third deviation was in the same file: `av_fast_mallocz()` re-`memset` an
already-large-enough buffer on every call. Upstream returns early and zeroes
only on actual (re)allocation. `dca_core.c:784` holds the core's subband
samples in such a buffer and the ADPCM predictor carries that history ACROSS
frames, so re-zeroing it per frame destroyed inter-frame prediction. Both
`av_fast_malloc` and `av_fast_mallocz` now follow upstream's `ff_fast_malloc`
exactly, including its `min_size + min_size/16 + 32` growth rule.

## How this is verified now

`tests/test_dts_xll_dump.c` decodes a REAL DTS-HD MA fixture and compares it
byte for byte against `ffmpeg -i x.dts -c:a pcm_s32le`. Because the format is
lossless that reference is exact, so the test is a true pass/fail.

The fixture does not have to be synthesised -- there is no free DTS-HD MA
encoder, which is why `test_dts_hd.c` could only ever cover the core -- it is
EXTRACTED from a disc rip with `ffmpeg -c:a copy`. See `tests/Makefile.host`.

With the three fixes above, an 8-second 5.1 24-bit fixture decodes bit-exact
on x86-64 AND on big-endian PPC64 (`powerpc64-linux-gnu-gcc -static` under
`qemu-ppc64`), 806 of 806 frames. Before them the five full-range channels
were near-full-scale noise (-3.5 dBFS against a -21 dBFS reference, 0.12
correlation) while LFE was roughly intact -- which is what "sounds wrong on
hardware" actually was.

Note the byte order was never the problem. x86-64 and big-endian PPC64
produced BYTE-IDENTICAL output both before and after the fix, so the bug was
always host-reproducible; it went unnoticed only because no host test fed the
decoder an XLL stream.

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

Wired and tested on the host; **not yet confirmed on hardware**.

`adec_dts.cpp` opens this decoder at the same time as libdca and, when it
comes up, switches `dts_stream` into packet mode so whole frames arrive here
instead of libdca getting a bare core. If it fails to open, or fails on 32
consecutive frames, the reader falls back to libdca — so the worst case is the
lossy audio that shipped before, never silence.

`tests/test_dts_hd.c` drives the whole chain (framing, decoder, fixed-point
output, channel map) with a distinct tone per speaker and asserts every PS3
slot is dominated by its own tone. Margins measured 35–78 dB.

The fixture is core-only DTS, because ffmpeg's `dca` encoder is the only free
DTS encoder and it cannot produce DTS-HD MA — **no free XLL encoder exists**.
So the host test proves the framing, the API, the fixed-point path and the
channel map, but it cannot synthesise an XLL substream. XLL itself is confirmed
by playing a real DTS-HD MA track and watching the stats overlay read
`dts-ma`, which only appears when a frame decoded from the XLL extension.

Generate the fixture with:

```
ffmpeg -f lavfi -i "sine=f=400:r=48000:d=4" -f lavfi -i "sine=f=700:r=48000:d=4"        -f lavfi -i "sine=f=1100:r=48000:d=4" -f lavfi -i "sine=f=60:r=48000:d=4"        -f lavfi -i "sine=f=1900:r=48000:d=4" -f lavfi -i "sine=f=2600:r=48000:d=4"        -filter_complex "[0:a][1:a][2:a][3:a][4:a][5:a]join=inputs=6:channel_layout=5.1:map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-BL|5.0-BR[a]"        -map "[a]" -c:a dca -strict -2 -ar 48000 tones51.dts
```

The explicit `map=` is not optional: `join`'s default input order is NOT the
layout's channel order, and leaving it out produces a fixture whose front three
channels are rotated. That cost a "failing" test run before a cross-check
against ffmpeg's own decoder showed our output matched it exactly.
