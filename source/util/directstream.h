#pragma once

// Settings > Direct Stream.  Auto (default) lets the server copy a source's
// video untouched when it already fits the request; Off sends
// AllowVideoStreamCopy=false so the server always re-encodes it.  A copied
// stream keeps the source's own encoding quirks (interlacing, bitrate
// spikes, odd timestamps) that the PS3's decoder can choke on.
// Persisted in jellyfin_directstream.txt as "0" (off) / "1" (auto).
bool directstream_enabled(void);
void directstream_set_enabled(bool on);   // set + persist immediately
