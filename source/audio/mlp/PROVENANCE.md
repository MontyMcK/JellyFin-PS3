# Vendored MLP / TrueHD decoder (from FFmpeg)

- **Upstream project:** FFmpeg — https://ffmpeg.org/
- **Obtained from:** `https://github.com/FFmpeg/FFmpeg`, `master` as of
  2026-09-16.
- **Licence:** LGPL-2.1-or-later (every file taken here carries the LGPL
  header) — compatible with this application's GPL-3.0 licence. No
  GPL-only FFmpeg file is used.
- **Why FFmpeg's:** it is the only free MLP/TrueHD decoder in existence.
  Unlike AC-3 (liba52) and DTS (libdca) there is no small standalone library
  for this format, so the decoder is taken from libavcodec together with the
  minimum of its framework. See `docs/dolby-truehd.md` §2.

## Files taken verbatim

From `libavcodec/`: `mlpdec.c`, `mlp.c`, `mlp.h`, `mlp_parse.c`,
`mlp_parse.h`, `mlpdsp.c`, `mlpdsp.h`, and, under `ff/libavcodec/`:
`get_bits.h`, `vlc.h`, `vlc.c`, `bitstream.h`, `mathops.h`, `defs.h`,
`codec_id.h`, `packet.h`.

From `libavutil/`, under `ff/libavutil/`: `crc.c`, `crc.h`, `vlc`'s and
`get_bits`' header dependencies (`attributes.h`, `attributes_internal.h`,
`avassert.h`, `avutil.h`, `bswap.h`, `buffer.h`, `channel_layout.h`,
`common.h`, `dict.h`, `error.h`, `ffmath.h`, `frame.h`, `internal.h`,
`intfloat.h`, `intmath.h`, `intreadwrite.h`, `log.h`, `macros.h`,
`mathematics.h`, `mem.h`, `mem_internal.h`, `opt.h`, `pixdesc.h`,
`pixfmt.h`, `qsort.h`, `rational.h`, `reverse.c`, `reverse.h`,
`samplefmt.h`, `thread.h`, `version.h`).

**No upstream file's contents were modified.**

## Local additions (not upstream)

- `ff/config.h`, `ff/config_components.h`, `ff/libavutil/avconfig.h` —
  hand-written replacements for the files FFmpeg's `configure` generates,
  deriving byte order and feature flags from compiler macros so the same tree
  builds for the big-endian PPU and the little-endian host test harness.
- `ff/libavcodec/avcodec.h`, `codec_internal.h`, `decode.h`, `profiles.h`,
  `internal.h` — a minimal stand-in for libavcodec's framework headers,
  declaring only the handful of types and calls `mlpdec.c` touches
  (`AVCodecContext`, `AVFrame`, `ff_get_buffer`, …). These replace, rather
  than wrap, the real headers: pulling in libavcodec's actual API would drag
  in the whole library.
- `../mlp_compat.c` — the runtime half of that stand-in: `av_log`,
  allocation, the six `AVChannelLayout` helpers the decoder calls, and a
  `ff_get_buffer` that hands the decoder a caller-owned PCM block (no
  refcounted frames, no buffer pool).
- `../mlp_api.c` — compiles the whole decoder as ONE translation unit
  (it `#include`s the vendored `.c` files) and exposes init/decode/flush.
  Two reasons: `mlpdec.c` keeps every entry point `static` because it is
  normally reached through an `FFCodec` table this app does not build, and
  the PS3 Makefile compiles every `.c` under a source directory into one flat
  `obj/`, which this keeps out of.  The vendored directory is therefore NOT
  listed in the Makefile's `SOURCES`; only its headers are on the include
  path.

## What it decodes

MLP and **TrueHD**, losslessly, up to 8 channels — including the TrueHD
substream inside a **Dolby Atmos** track (the Atmos object metadata rides in
an extension this decoder ignores, and what comes out is the 5.1/7.1 bed).
Dolby Digital Plus (E-AC-3) is a different codec and is **not** covered by
this tree; DD+ sources still take the AC-3 transcode path.
