# DTS / DTS-HD / DTS:X Support — Design & Implementation

Status: **implemented**, extends the `surround-5.1` feature. Default **OFF**
(the Surround setting must be cycled to "DTS"); with it off, every code path
is the AC-3 path or the shipped stereo path, unchanged.

Companion document: [surround-5.1.md](surround-5.1.md), which established the
8-channel LPCM output stage, the PS3 channel order and the decoder dispatch
this feature plugs into.

## 1. Summary

Add a third state to the Surround setting: **DTS**. When the selected audio
track is already a DTS track — plain DTS, DTS-ES, DTS-HD HRA, **DTS-HD MA** or
**DTS:X** — ask Jellyfin to **stream-copy** it instead of transcoding, demux it
out of the MPEG-TS, and decode its **core substream** on the PPU with
**libdca**, into the same 6-channel LPCM port AC-3 already uses.

Concretely, for a DTS-HD MA / DTS:X source this replaces

```
server: decode DTS-HD MA -> encode AC-3 5.1 @ 640 kbps -> PS3 decodes AC-3
```

with

```
server: copy the DTS track untouched          -> PS3 decodes the DTS core
                                                 (5.1, up to 1509 kbps)
```

— a higher audio bitrate, no audio transcode on the server, and the original
mix's own downmix metadata rather than a re-encode of it.

## 2. What "DTS-HD and DTS:X support" can and cannot mean here

This is the part worth being blunt about, because the honest answer is smaller
than the feature name suggests.

**No bitstream passthrough exists on this platform.** Established in
surround-5.1.md §1 and unchanged: PSL1GHT exposes no `audioOut*` binding, and
Movian — the only shipped multichannel PS3 homebrew audio backend — always
writes float LPCM into the audio port even when it configures the S/PDIF
encoder (`movian/src/arch/ps3/ps3_audio.c:125-206`). A PS3 homebrew app cannot
hand a receiver an encoded DTS-HD MA or DTS:X bitstream and let the receiver
decode it. Everything must be decoded on the PPU and output as LPCM.

**Nothing decodes the extension substreams.** A DTS-HD track is a DTS core
substream plus one or more extension substreams:

| Layer | Carries | Decodable here? |
|---|---|---|
| Core substream | 5.1, ≤1509 kbps, lossy | **Yes** — libdca |
| XLL extension | the lossless residual (DTS-HD MA) | No |
| XBR / X96 / XCh / XXCh | extra bitrate, 96 kHz, extra channels | No |
| LBR (DTS Express) | low-bitrate secondary audio | No |
| DTS:X objects (XLL-X) | object audio + renderer metadata | No |

libdca (VideoLAN) is the only small, dependency-free, GPL-compatible DTS
decoder, and it decodes the core only. The only implementation that decodes
XLL is ffmpeg's, which is inseparable from libavcodec — and lossless
multichannel XLL decode on a 3.2 GHz in-order PPU, alongside H.264, is not a
realistic budget anyway. DTS:X additionally needs the proprietary renderer to
turn objects into speaker feeds; no free implementation exists at all.

**Therefore: DTS-HD MA and DTS:X tracks play from their core.** 5.1, lossy, up
to 1509 kbps. Every DTS-HD track carries that core — it is what the format's
backward compatibility is for — so these tracks play, and they play better
than the 640 kbps AC-3 transcode that was the previous best. They do not play
losslessly, and DTS:X height/object placement is not reproduced. That ceiling
is a property of the hardware and of what free software exists, not of this
implementation.

### Rejected alternatives

- **Bitstream passthrough to the receiver** — impossible (above). Rejected.
- **Decode XLL for true lossless** — no GPL-compatible implementation, and the
  PPU budget is doubtful even if one existed. Rejected.
- **Ask the server to transcode TO DTS** (so any source gets DTS) — ffmpeg's
  `dca` encoder is experimental (`-strict -2`), worse than its AC-3 encoder at
  any bitrate, and Jellyfin does not offer it. Rejected; DTS is only ever
  requested as a **copy** of a track that is already DTS.
- **Server-side "strip the HD extensions, send only the core"** — Jellyfin has
  no such option; a copy is all-or-nothing. Rejected (the client skips the
  extensions instead, §4).

## 3. Codec decision: libdca

**Chosen: libdca (VideoLAN), GPL-2.0-or-later — GPL-compatible with this
repo's GPLv3.** See `source/audio/dca/PROVENANCE.md` for the exact commit and
file list.

- Pure C, no dependencies, float output — the same shape as liba52, which this
  codebase already vendors and builds for both the PPU and the host tests.
- Block size fits the output stage exactly: libdca emits **256 samples per
  channel per block** (`dca_blocks_num()` blocks per frame), and one PS3
  CellAudio DMA block is 256 sample frames.
- Cost on the PPU is dominated by the 32-band QMF (~1000 double MACs per 32
  output samples per channel): roughly 8 M MAC/s for 5.1 at 48 kHz, which is a
  small fraction of a 3.2 GHz PPU — the same order as liba52's IMDCT.

### Two traps, both found by reading the source

1. **Channel order is not AC-3's.** liba52 emits `LFE, L, C, R, SL, SR`
   (LFE **first**). libdca emits `C, L, R, SL, SR, LFE` — centre **first**,
   LFE **last**. Both halves of that were derived from libdca's own code and
   are documented at the top of `source/audio/dts_map.c`; the two maps
   therefore cannot share an implementation, and each has its own tone test.
2. **`dca_dynrng()` is not `a52_dynrng()`.** The AC-3 path calls
   `a52_dynrng(state, NULL, NULL)` to disable dynamic-range compression. The
   obvious analogue corrupts the stream: `dca_dynrng()` zeroes
   `state->dynrange` (`dca_parse.c:1289`), which is not an "apply DRC" switch
   but the **bitstream flag** the subframe parser reads at `dca_parse.c:708`
   to decide whether an 8-bit dynamic-range coefficient is present — clearing
   it desyncs the bit reader on any stream that carries one. libdca never
   applies the coefficient to samples anyway, so there is nothing to disable.
   The decoder deliberately does not call it, with a comment saying why.

## 4. Elementary-stream handling: skipping the extensions

`source/audio/dts_stream.c` reads the elementary stream:

- Scan for a **core sync word** (`dca_syncinfo()` accepts all four framings:
  16/14-bit, big/little-endian), wait until the whole frame is buffered, decode
  it, emit its blocks.
- Scan for the **extension substream sync word** `0x64582025`, read just enough
  of its header to learn its total size, and skip exactly that many bytes. The
  header field order is taken from ffmpeg's `ff_dca_exss_parse()`
  (`libavcodec/dca_exss.c`) and implemented in `source/audio/dts_exss.c`.
- Extension substreams are **never buffered** — they can be far larger than the
  32 KB carry buffer, so a byte counter carries the skip across PES boundaries.
- Implausible headers (substream smaller than its own header) are treated as a
  false sync and rescanned from the next byte, so a random `0x64582025` inside
  audio data cannot make the decoder skip through real frames.

This module is pure C with a callback for output, so the host tests drive the
whole state machine (§6) — the framing is where a DTS-HD stream differs from
everything else this app has demuxed, and it is not observable in a build log.

**DTS-HD / DTS:X identification** is deliberately *not* done by parsing the
extension substream's asset descriptors. The track's identity already arrives
from the server in the MediaStream `DisplayTitle` ("English - DTS-HD MA - 5.1"),
which is authoritative and free; the bitstream side only needs to know that an
extension substream exists, which the sync word already says. The stats overlay
shows `dts-hd` when one has been seen and `dts` when none has.

## 5. Server negotiation

Two places have to agree, or the server quietly sends something else:

- **Device profile** (PlaybackInfo, `api_detail.cpp`): the `body_*_dts` blobs
  advertise `dts` in the transcoding profile's `AudioCodec` list and add a
  `dts` CodecProfile with an 8-channel ceiling. Without this the server will
  not consider a DTS copy at all.
- **Stream URL** (`player_session.cpp`): `AllowAudioStreamCopy=true`,
  `AudioCodec=ac3,dts,mp3`, `MaxAudioChannels=8`, and **no** `AudioBitrate`.

Each of those four is load-bearing:

| Parameter | Why |
|---|---|
| `AllowAudioStreamCopy=true` | Without it the server transcodes, and a "dts" preference then lands on ffmpeg's experimental DTS encoder. |
| `dts` present in the list | Copy eligibility is a membership test against the requested codec list. |
| `ac3` listed **first** | Order does not affect copy eligibility, but it decides what an actual transcode encodes to — and that must be AC-3. |
| no `AudioBitrate` | Jellyfin checks the requested audio bitrate before allowing a copy; a 1509 kbps core (let alone a multi-Mbps HD stream) fails a 640 kbps ceiling and is silently demoted to a transcode. |
| `MaxAudioChannels=8` | DTS-HD MA / DTS:X tracks are routinely 7.1, and a 6-channel ceiling refuses to copy them. Copying is right: the core this app decodes is 5.1 either way. |

The DTS request is only made when the **selected track is actually DTS**,
decided from its label by `track_label_is_dts()` (`source/util/track_codec.c`,
host-tested). For any other track the URL is byte-identical to the AC-3 one —
which is why DTS mode is a superset of AC-3 mode and never plays worse.

### PMT / demux

ffmpeg's `mpegts` muxer writes stream type **0x82** for `AV_CODEC_ID_DTS`
regardless of whether the copied track is plain DTS, HD HRA, HD MA or DTS:X
(`libavformat/mpegtsenc.c`, `get_dvb_stream_type()`). The demuxer also accepts
0x85/0x86/0x8A (Blu-ray-derived, ffmpeg's m2ts mode) and private-stream 0x06
with a DVB DTS descriptor (0x7B) or a `DTS1`/`DTS2`/`DTS3`/`DTSH`/`DTSE`
registration descriptor. All select the same decoder: they all carry a core.

### The coreless case

A DTS-HD MA track *can* legally exist with no core (Blu-ray always has one;
re-encodes need not). Nothing here can decode that, and the failure mode would
be silence. So the decoder reports it (`adec_dts_no_core()`: 512 KB of
extension-only data and not one core frame), and the player responds by
vetoing DTS **for the rest of the session** and reopening at the current
position — the same 0-delta reopen a track change uses — which re-negotiates
the stream as an AC-3 5.1 transcode. The veto is session-scoped: the next
title tries DTS again.

## 6. Verification

Host-side (`tests/`, `make -f Makefile.host check`) — the same vendored libdca
and the same map/framing sources the PS3 build compiles:

| Test | Asserts |
|---|---|
| `test_dts_map` | Every granted channel config maps to the right PS3 slots, including LFE-plane placement, the -3 dB mono-surround rule, and silence for configs that cannot be granted. |
| `test_dts_decode` | A real ffmpeg-encoded DTS 5.1 file with a distinct tone per speaker decodes through libdca + the map with every PS3 slot dominated by its own tone. Also range-checks the output level. |
| `test_dts_stream` | A synthesised DTS-HD-shaped stream (core frame, extension substream, core frame, …, including an extension larger than the carry buffer) decodes to **byte-identical PCM** to the plain core stream, at feed chunk sizes from 1 byte to 64 KB, with zero bad frames. |
| `test_dts_exss` | Extension substream sizing: short and wide header forms, maxima, truncated input, bad sync, implausible sizes. |
| `test_track_codec` | DTS track detection from Jellyfin DisplayTitles, both directions, including word-boundary false positives. |

Results (2026-09-16, mingw-w64 14.2 host): all pass; the tone test routes all
6 slots with ≥62 dB margin; the stream test matches the reference PCM exactly
at every chunk size; the AC-3 tests still pass unchanged.

### Not verified here (hardware required)

- Playback on a real PS3: PPU decode cost of the DTS core alongside H.264,
  A/V sync with copied (not transcoded) audio, and the 8-channel port output.
- A real Jellyfin server honouring the copy request for a DTS-HD MA / DTS:X
  track (the negotiation above is derived from Jellyfin/ffmpeg behaviour, not
  from a live round trip).
- The coreless-track fallback, which needs a DTS-HD MA file without a core.

The stats overlay line is the first thing to read on hardware: `dts-hd 6/6`
means the whole path worked, `dts 6/6` means a core-only DTS track,
`ac3 6/6` means the server refused the copy and transcoded, `mp3 2/2` means it
refused surround altogether.
