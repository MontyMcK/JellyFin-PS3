#pragma once

// DTS-HD / DTS:X extension substream header — just enough of it to know how
// many bytes to skip.  Pure C, no PS3 headers, so tests/test_dts_exss.c can
// exercise it on the host: an off-by-one here does not produce a warning, it
// produces a decoder that resyncs on random bytes for the rest of the movie.
//
// Why skip rather than decode: the extension substreams carry the lossless
// XLL residual, the extra channel sets and the DTS:X objects, and no
// GPL-compatible decoder for them exists (docs/dts-hd.md §2). The core
// substream next to them is what plays.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Extension substream sync word, big-endian on the wire.
#define DTS_EXSS_SYNCWORD 0x64582025u

// p points at a candidate extension substream header (caller has already
// matched the sync word); avail is how many bytes are readable at p.
// On success writes the substream's TOTAL size in bytes — sync word
// included, i.e. exactly how far to advance — and returns true.
// Returns false if avail is too small to decide, or the header is implausible
// (treat that as a false sync and rescan from the next byte).
bool dts_exss_size(const uint8_t *p, int avail, uint32_t *out_size);

// Bytes of header that must be readable before dts_exss_size() can decide.
#define DTS_EXSS_MIN_HEADER 10

#ifdef __cplusplus
}
#endif
