# Vendored liba52 (AC-3 decoder)

- **Upstream project:** a52dec / liba52 — http://liba52.sourceforge.net/
- **Obtained from:** Debian multimedia-team mirror,
  `https://salsa.debian.org/multimedia-team/a52dec.git`, commit `f3683fe`
  (upstream a52dec 0.8.0 lineage; the Debian tree tracks upstream source
  unmodified in these files).
- **Licence:** GPL-2.0-or-later (see each file header) — compatible with this
  application's GPL-3.0 licence.
- **Files taken verbatim** from `liba52/`: `parse.c`, `bit_allocate.c`,
  `bitstream.c`, `downmix.c`, `imdct.c`, `a52_internal.h`, `bitstream.h`,
  `tables.h`; from `include/`: `a52.h`, `attributes.h`, `mm_accel.h`.
- **Local additions (not upstream):** `config.h` in this directory replaces
  the autoconf-generated header with compiler-macro-derived equivalents so
  the same tree builds for the big-endian PPU and the little-endian host
  test harness. No upstream `.c`/`.h` file was modified.
- **Why liba52:** small pure-C AC-3 decoder with float output and no
  dependencies; an AC-3 audio block is 256 samples — exactly one PS3
  CellAudio DMA block. See `docs/surround-5.1.md` for the full codec
  decision record.
