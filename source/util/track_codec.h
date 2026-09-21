#pragma once

// Does this audio track's label name a DTS codec?
//
// The label is Jellyfin's MediaStream DisplayTitle, e.g.
//   "English - DTS-HD MA - 5.1 - Default"
//   "Surround 7.1 - DTS:X"
//   "English - EAC3 - 5.1"
// and it is the only codec information the client has before it opens the
// stream.  The answer decides whether the stream URL may ask the server to
// stream-COPY the track (which is the only way a DTS-HD MA / DTS:X track ever
// reaches this app — ffmpeg's DTS encoder is experimental and Jellyfin will
// not transcode TO DTS).  Getting it wrong in the optimistic direction is not
// harmless: asking for "dts" on a track that is not DTS invites the server to
// reach for that encoder instead of copying.  So this asks a narrow question
// and answers it conservatively.
//
// Pure C, no PS3 headers — exercised by tests/test_track_codec.c.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool track_label_is_dts(const char *label);

// Same question for Dolby TrueHD — including a Dolby Atmos track, which is a
// TrueHD track whose label usually says both ("English - TrueHD Atmos - 7.1").
// E-AC-3 / Dolby Digital Plus is deliberately NOT included: nothing here
// decodes it, so those tracks must keep taking the AC-3 transcode path.
bool track_label_is_truehd(const char *label);

// Either of the above: the track can be requested as a stream copy and
// decoded locally.
bool track_label_is_hd_audio(const char *label);

#ifdef __cplusplus
}
#endif
