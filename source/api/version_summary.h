#pragma once
// A short, readable name for one version (2026-09-27).  Debrid names are
// emoji-laden multi-line blurbs ("[PM] MediaFusion 1080p / BluRay REMUX / 38.8
// GB (36.1 Mbps) / RuTor / Russian / <filename>") that do not fit a row and
// half of which the fonts cannot draw.  From the RAW name (before any
// cleaning): resolution, source (REMUX first), 3D, HDR, video codec, audio +
// channels, size and language, joined with " Â· ", e.g.
//   "1080p Â· REMUX Â· DTS-HD MA 7.1 Â· 38.8 GB Â· Russian".
// Pure C, host-tested.  "Version" when nothing is recognised.
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void version_summary(const char *raw, char *out, size_t cap);
#ifdef __cplusplus
}
#endif
