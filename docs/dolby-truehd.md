# Dolby TrueHD / Atmos Support — Design & Implementation

Status: **implemented**, extends the `surround-5.1` feature alongside
[dts-hd.md](dts-hd.md). Default **OFF** (Settings → Surround must be cycled to
"HD"); with it off, every code path is the AC-3 path or the shipped stereo
path, unchanged.

## 1. Summary

The Surround setting's third state, **HD**, asks Jellyfin to **stream-copy**
the source's own HD audio track instead of transcoding it. That state already
covered DTS; this document covers the Dolby half:

| Source track | What now happens |
|---|---|
| **TrueHD** | Copied intact, decoded here **losslessly** to 5.1 or **7.1** |
| **TrueHD Atmos** | Same — the lossless bed plays; the objects are not rendered |
| DTS / DTS-HD / DTS:X | Copied, 5.1 core decoded (dts-hd.md) |
| **E-AC-3 (DD+)**, incl. DD+ Atmos | **Unchanged**: AC-3 5.1 transcode |
| anything else | Unchanged: AC-3 5.1 transcode |

This is a bigger win than the DTS half. DTS-HD MA can only be played from its
lossy core, but TrueHD decodes **bit-exactly**: what reaches the PS3's DACs is
the studio master's bed, verified sample-for-sample against FFmpeg's own
decoder (§5), and at 7.1 rather than 5.1.

## 2. Codec decision: FFmpeg's MLP decoder

**Chosen: libavcodec's MLP/TrueHD decoder, LGPL-2.1-or-later** — compatible
with this repo's GPLv3. Vendored under `source/audio/mlp/`; see
`PROVENANCE.md` there for the exact file list.

This is the one place where the pattern the AC-3 and DTS paths established —
"vendor a small standalone library" — could not be followed, because **no
standalone TrueHD decoder exists**. liba52 and libdca are self-contained
projects; MLP has only ever been implemented freely inside FFmpeg. So the
decoder comes from libavcodec together with the minimum of its framework:

- **Taken verbatim:** `mlpdec.c`, `mlp.c`, `mlp_parse.c`, `mlpdsp.c` and their
  headers, plus `get_bits.h`, `vlc.c`, `crc.c`, `reverse.c` and the
  header-only libavutil files those pull in. No upstream file is modified.
- **Replaced (≈200 lines, `mlp_compat.c` + a few headers):** `av_log`,
  allocation, six `AVChannelLayout` helpers, and a `ff_get_buffer` that hands
  the decoder a caller-owned PCM block. Pulling in real libavcodec/libavutil
  instead would mean vendoring most of two libraries for one decoder.
- **Compiled as one translation unit** (`mlp_api.c` `#include`s the vendored
  `.c` files): upstream keeps every entry point `static` behind an `FFCodec`
  table this app does not build, and the PS3 Makefile compiles every `.c`
  under a `SOURCES` directory into one flat `obj/`, which the vendored tree
  stays out of.

### Cost on the console

- **Memory:** the decoder context is **13.6 KB** and the stream reader 27 KB,
  both static — no malloc in the audio path. (The PCM ring grew 512 KB to
  carry a 7.1 program; see §3.)
- **CPU:** measured on the host at roughly 36× realtime for 5.1. The PPU is
  an in-order core and will be several times slower per clock, but video
  decoding is on the PS3's hardware VDEC, so the PPU has the headroom. **Not
  measured on hardware** — see §6.

### Rejected alternatives

- **Bitstream passthrough to a receiver** — impossible on this platform for
  the same reason as DTS (dts-hd.md §2, surround-5.1.md §1): nothing in
  PSL1GHT can hand a receiver an encoded stream. Rejected.
- **Rendering Atmos objects** — no free renderer exists, and object rendering
  needs the listener's actual speaker layout, which a PS3 app cannot know.
  The bed is what plays. Rejected.
- **Decoding E-AC-3 (Dolby Digital Plus) too** — the same argument would
  apply, but its decoder is another large chunk of libavcodec, and unlike
  TrueHD there is nothing to gain in kind: DD+ is lossy, so the existing
  server-side AC-3 5.1 transcode is already a fair rendition of it.
  Rejected; DD+ (including DD+ Atmos) keeps taking the AC-3 path.
- **Transcoding TO TrueHD server-side** so any source benefits — ffmpeg's
  TrueHD encoder is experimental and tops out at 5.1, and re-encoding a lossy
  source losslessly gains nothing. Rejected; TrueHD is only ever *copied*.

## 3. Channel mapping and 7.1

The vendored decoder emits **interleaved int32** in FFmpeg's native order
(ascending channel-mask bit order), not planar floats in a codec-specific
order, so it gets its own map (`truehd_map.c`) rather than sharing the AC-3 or
DTS one. Two details carry the risk:

- A 5.1 TrueHD mix is reported as `AV_CH_LAYOUT_5POINT1_BACK` — its surround
  pair is labelled **back**. Those channels are the 5.1 surrounds and belong
  in the PS3's surround slots 4/5, not its rear slots 6/7.
- A 7.1 mix has both pairs, and FFmpeg orders them **backs first**
  (BL BR at indices 4/5, SL SR at 6/7) because the back channels have lower
  mask bits. The PS3 wants the opposite. That swap is exactly the `vec_perm`
  movian applies in `ps3_audio.c`, and it is what makes 7.1 come out right.

Supporting 7.1 end to end also meant widening three things that the AC-3/DTS
paths had fixed at a 5.1 program: the PCM ring (now 8 channels, 2 MB), the
output stage's staging buffer and channel clamp, and what
`audio_output_channels()` reports (now the port's *capacity*, 8, rather than a
fixed 6). A 5.1 TrueHD track still fills six slots and the output stage zeroes
the rear pair every block, exactly as before.

## 4. Server negotiation

Identical in shape to the DTS half (dts-hd.md §5), with `truehd` added:

- **Device profile:** the `body_*_hd` blobs advertise `ac3,mp3,dts,truehd` and
  a `dts,truehd` CodecProfile with an 8-channel ceiling.
- **Stream URL:** for a track whose label says TrueHD,
  `AudioCodec=ac3,truehd,mp3`, `AllowAudioStreamCopy=true`,
  `MaxAudioChannels=8`, and no `AudioBitrate`.

`track_label_is_truehd()` is what gates it, and it deliberately does **not**
match "Atmos" on its own: a Dolby Atmos track can be TrueHD *or* E-AC-3, and
asking for a copy of the E-AC-3 one would leave the app with a stream it
cannot decode.

**PMT:** ffmpeg's mpegts muxer writes stream type **0x83** for TrueHD in both
its DVB and m2ts paths (`mpegtsenc.c`, `STREAM_TYPE_BLURAY_AUDIO_TRUEHD`).

**Framing:** TrueHD access units are found by the 12-bit length field in each
unit header, after locking onto the major sync word `0xF8726FBA` at offset 4
of a unit — derived from `libavcodec/mlp_parser.c` and implemented in
`truehd_stream.c`, which also re-hunts for that sync after a run of failed
units rather than walking further into misaligned data.

**Bandwidth:** a copied TrueHD track is several Mbps on top of the video —
fine on a LAN, a bad idea over the internet. This is the one way HD mode can
be *worse* than the AC-3 transcode, and it is the same trade the DTS half
makes.

## 5. Verification

Host-side (`tests/`, `make -f Makefile.host check`), using the same vendored
decoder the PS3 build compiles:

| Test | Asserts |
|---|---|
| `test_truehd_decode` | A real ffmpeg-encoded TrueHD 5.1 file decodes through framing → decoder → channel map with every PS3 slot dominated by its own tone (≥87 dB margin), **and** the samples match ffmpeg's own decode **bit-for-bit** — the strongest check available, since TrueHD is lossless. |
| `test_truehd_map` | Every layout the decoder can report maps to the right PS3 slots: 5.1 in both its "back" and "side" spellings, the 7.1 back/side swap, 7.1 folded to 5.1, quad, 4.0 with a mono back centre, 2.1, mono to centre, the normalised stereo downmix, and rejection of a mask that disagrees with the channel count. |
| `test_track_codec` | TrueHD/Atmos/MLP labels select the copy path; E-AC-3, DD+ Atmos, AC-3 and DTS labels do not. |

Result (2026-09-16, mingw-w64 14.2 host): all pass, bit-exact over 96 000
frames × 6 channels. The AC-3 and DTS suites still pass unchanged.

### Not verified here

- **Anything on hardware.** No PPU cross-compile was possible in this session
  (the machine's WSL toolchain would not start), so the PS3-side C++ was
  syntax-checked with stub headers, not built or run. PPU decode cost, A/V
  sync with copied audio, and 7.1 output over HDMI are all still open.
- **7.1 decode end to end** — ffmpeg's TrueHD *encoder* only goes to 5.1, so
  no 7.1 test file could be generated here. The 7.1 channel map is unit-tested;
  the decoder's own 7.1 support is upstream FFmpeg's.
- **A real Jellyfin server honouring the TrueHD copy request.**
- **Atmos specifically** — no Atmos sample was available. It decodes through
  the identical path (an Atmos TrueHD track is a TrueHD track); what is
  untested is only that assumption.

On hardware, the stats overlay line is the thing to read: `truehd 8/8` is a
lossless 7.1 bed, `truehd 6/8` a lossless 5.1 one, `ac3 6/8` means the server
transcoded instead of copying, `mp3 2/2` means it refused surround entirely.
