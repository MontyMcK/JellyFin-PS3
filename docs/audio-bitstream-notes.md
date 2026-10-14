# Audio output: bitstream and routing notes

Measurements and incidents behind the rules in `source/audio/audio_bitstream.cpp`.
The code states the rules; this file keeps the evidence.

## The 5.1 centre channel and the AC-3 request (2026-09-18)

Chain: PS3 -> TV -> soundbar over ARC. On plain ARC only 2-channel PCM or
Dolby Digital gets through, so multichannel LPCM loses the centre and the
dialogue with it.

* `audioOutConfigure(AC-3)` returns 0 and the configuration reads back
  `encoder=1` on every playback. The read-back echoes what was asked for, like
  `getsockopt(SO_RCVBUF)` reporting a buffer nothing backs. It proves nothing.
* The soundbar showed no Dolby Digital indicator.
* The centre channel started working with the Dialogue setting on Normal.
  Before this it was only audible through the LoRo downmix.
* The output was already `ch=6 downmix=0` at startup, so encoder 0 -> 1 was the
  only audio-path change against v1.0.

The request is kept for its routing effect. It is not a claim that Dolby Digital
reaches the wire: whether the console also encodes depends on the PS3's Audio
Output Settings, and `audioOutGetState` cannot tell (see below).

## audioOutGetState is not authoritative (2026-09-29)

With the PS3's own Dolby Digital box ticked or not, `audioOutGetState().soundMode.type`
reads 0 (LPCM) while the soundbar shows DOLBY AUDIO. Nothing may branch on it to
decide what is on the wire. The 5.1 path logs it and the Dolby Digital path
ignores it.

## Dolby Digital passthrough, verified on hardware (2026-09-29)

`encoder = AUDIO_OUT_CODING_BITSTREAM (255)`, `channel = 2`, rc 0: the soundbar
shows DOLBY AUDIO, the centre speaks, and it works with the DD/LPCM boxes
unticked in the PS3's sound settings.

IEC 61937 bursts are checked byte for byte against ffmpeg's spdif muxer
(`tests/test_iec61937.c`, 640/448/192/96 kbps).

What does not work and must not ship:

* E-AC-3 passthrough: the soundbar stays silent.
* TrueHD bitstream: needs HBR, not reachable from cellAudio.
* DTS: the soundbar has no decoder; the configure is rejected.

## The 24p mode switch and the output configuration (2026-09-27)

A display mode change re-establishes the HDMI link and the system puts its own
audio configuration back (8 channels when the PS3's Audio Output Settings are on
automatic). That undid the 6-channel configure the centre channel depends on.
`audio_bitstream_reassert()` therefore re-applies exactly the configuration
`audio_bitstream_begin()` applied: 6 channels for the 5.1 request, 2 for
passthrough.

## A crashed session and the journal (2026-09-25)

The app died with the output changed (a hung paced writer), `audio_bitstream_end()`
never ran, and the console stayed on AC-3 at 2 channels: silent menus and music
in every later session, because each read the broken state as "what it was before".
The state changed from is now written to `jellyfin_audioout.txt` before the
change and removed after the restore; a launch that finds it restores it.
