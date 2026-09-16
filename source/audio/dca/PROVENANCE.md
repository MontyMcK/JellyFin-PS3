# Vendored libdca (DTS Coherent Acoustics core decoder)

- **Upstream project:** libdca (formerly libdts) — https://www.videolan.org/developers/libdca.html
- **Obtained from:** VideoLAN git, `https://code.videolan.org/videolan/libdca.git`,
  commit `95c4bd8baee808ec563120f463111e2cd6cd240e` (0.0.7 lineage).
- **Licence:** GPL-2.0-or-later (see each file header) — compatible with this
  application's GPL-3.0 licence.
- **Files taken verbatim** from `libdca/`: `parse.c`, `downmix.c`,
  `bitstream.c`, `bitstream.h`, `dca_internal.h`, `tables.h`,
  `tables_adpcm.h`, `tables_fir.h`, `tables_huffman.h`,
  `tables_quantization.h`, `tables_vq.h`; from `include/`: `dca.h`.
- **Local changes (not upstream):**
  - The three `.c` files are **renamed** — `parse.c` → `dca_parse.c`,
    `downmix.c` → `dca_downmix.c`, `bitstream.c` → `dca_bitstream.c`. Contents
    are byte-for-byte upstream. The rename is forced by the build: the PS3
    Makefile compiles every `.c` under `SOURCES` into one flat `obj/`
    directory, and liba52 (`source/audio/a52/`) already contributes
    `parse.o`, `downmix.o` and `bitstream.o`. Without the rename the two
    vendored decoders would silently overwrite each other's objects.
  - `config.h` in this directory replaces the autoconf-generated header with
    compiler-macro-derived equivalents, so the same tree builds for the
    big-endian PPU and the little-endian host test harness.
  No upstream `.c`/`.h` file content was modified.
- **Why libdca:** the only small, dependency-free, GPL-compatible DTS decoder.
  Pure C, float output, and — like liba52 — it emits **256 samples per
  channel per block**, which is exactly one PS3 CellAudio DMA block
  (`AUDIO_BLOCK_SAMPLES`, `PSL1GHT/ppu/include/audio/audio.h:49`).
- **What it does NOT decode:** the DTS-HD extension substreams (XLL/lossless,
  XBR, X96, DTS Express) and DTS:X object audio. libdca decodes the
  backward-compatible **core substream** only, up to 5.1. The app therefore
  plays DTS-HD MA and DTS:X tracks at core quality (see
  `docs/dts-hd.md` §2 for why nothing better is possible on this platform)
  and skips the extension substreams in `adec_dts.cpp`.
