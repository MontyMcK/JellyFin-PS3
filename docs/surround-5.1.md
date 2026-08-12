# 5.1 Surround Output — Design (Alpha)

Status: **design for review — no implementation yet**. Feature default **OFF**;
with it off, every code path is the shipped stereo path.

## 1. Summary

Add an opt-in "Surround (5.1)" mode to video playback: the server transcodes
audio to **AC-3 5.1**, the app decodes it in software on the PPU with
**liba52**, and writes 6 active channels into an **8-channel LPCM** PSL1GHT
audio port (the two unused rear-extension channels zeroed every block). The
PS3 OS mixes/downmixes according to the user's XMB Sound Settings. No
output-mode detection in v1 (upstream PSL1GHT has no `audioOut*` bindings —
verified: `ppu/sprx/libsysutil/exports.h` has 84 exports, none `audioOut*`;
`ppu/sprx/libaudio/exports.h` covers only `audioInit/Quit`, `audioPort*`,
`audioAdd*`, notify-queue calls). The user must enable **Linear PCM 5.1 ch**
in XMB → Sound Settings (HDMI / multi-AV; S/PDIF cannot carry 5.1 LPCM — see
§9 limitations).

There is no bitstream passthrough on this platform: Movian, the only shipped
multichannel PS3 homebrew audio backend, always writes float LPCM into the
audio port even when it configures the S/PDIF encoder to AC-3
(`movian/src/arch/ps3/ps3_audio.c:125-206`). We decode to float PCM ourselves.

## 2. Codec decision: AC-3 (Dolby Digital) via liba52

**Chosen: AC-3 5.1, decoded with liba52 (a52dec project), GPL-2.0-or-later —
GPL-compatible with this repo's GPLv3.**

- Small, pure C, no dependencies, float output (`sample_t = float` in the
  default build), realistic for a 3.2 GHz in-order PPU (it decoded fine on
  late-90s x86).
- Jellyfin/ffmpeg transcodes anything to AC-3 willingly; it is the native
  surround format of the PS3 era.
- Block-size fit: an AC-3 sync frame is 6 blocks × **256 samples** — one AC-3
  block is exactly one PS3 DMA block (`AUDIO_BLOCK_SAMPLES` = 256,
  `PSL1GHT/ppu/include/audio/audio.h:49`).
- Dynamic range: wire `a52_dynrng` with compression **disabled** by default
  (pass-through level), so quiet home-theater mixes are not crushed.

### Rejected alternatives

- **E-AC-3 (DD+)** — no small GPL-compatible standalone decoder exists
  (ffmpeg's eac3 decoder is inseparable from libavcodec; liba52 does not do
  E-AC-3). Its only advantage over AC-3 is bitrate efficiency, which is
  irrelevant on a LAN where the server transcodes anyway. Rejected.
- **AAC 5.1** — decodable via FAAD2 (GPL-2.0), but FAAD2 is an order of
  magnitude more code, decodes to a channel order that varies with the PCE in
  the bitstream (more remap risk, the highest-risk area of this feature), and
  its LC profile CPU cost on the PPU is higher than liba52's fixed pipeline.
  fdk-aac is licence-incompatible with GPL. Rejected.
- **FLAC 5.1 in TS** — libFLAC is BSD (compatible) and cheap to decode, but
  FLAC-in-MPEG-TS is not a standard mapping: ffmpeg's mpegts muxer refuses it
  / emits a private-stream mapping nothing else recognises, and Jellyfin's
  transcoding profiles do not offer FLAC in ts containers. Bitrate is also
  ~10× AC-3 for no audible gain over a lossy source. Rejected.

## 3. Channel mapping

### 3.1 PS3 8-channel LPCM order (derived, not recalled)

`movian/src/arch/ps3/ps3_audio.c:271-296` receives libav
`AV_CH_LAYOUT_7POINT1` interleaved float frames. libav orders channels by
ascending channel-mask bit: `FL FR FC LFE BL BR SL SR`. Movian loads each
frame as two vectors — `v1` = floats 0–3 (`FL FR FC LFE`, untouched), `v2` =
floats 4–7 (`BL BR SL SR`) — and applies `vec_perm` with byte mask
`{0x8..0xb, 0xc..0xf, 0x0..0x3, 0x4..0x7}`, i.e. word order `[2][3][0][1]`,
turning `BL BR SL SR` into `SL SR BL BR`. Therefore the PS3 expects:

```
slot: 0    1    2    3     4          5          6          7
      FL   FR   FC   LFE   SL (surr)  SR (surr)  BL (rear)  BR (rear)
```

For 5.1 content: fill slots 0–5, **zero slots 6–7 in every block** (the DMA
ring is not cleared by hardware; stale data there is audible).

### 3.2 liba52 output order → PS3 order

liba52 emits **planar** output: `a52_samples()` returns a buffer of 256
floats per channel per block. Verified in the a52dec source (Debian salsa
mirror of a52dec 0.8.x):

- `liba52/parse.c:762` — when `A52_LFE` is in the output flags the working
  pointer is shifted by 256 (`samples += 256`) and LFE coefficients are
  written at `samples - 256` (`parse.c:824`), i.e. **plane 0 of the output
  buffer is LFE**; full-bandwidth channel *i* occupies plane *i+1*.
- `liba52/downmix.c:246-250` — `CONVERT (A52_3F2R, A52_STEREO)` applies
  `clev` (centre level) to `coeff[1]` and `slev` (surround level) to
  `coeff[3]` and `coeff[4]`, proving the 3F2R full-bandwidth order is
  **0=L, 1=C, 2=R, 3=SL, 4=SR** (and `CONVERT (A52_3F2R, A52_3F2R)`,
  `downmix.c:166`, is a passthrough, so requesting 3F2R preserves it).

Remap table (a52 plane → PS3 slot), shipped as data with this derivation in
a comment, and exercised by a host-side unit test (§8.2) before it ever runs
on console:

| a52 plane | channel | PS3 slot |
|---|---|---|
| 0 | LFE | 3 |
| 1 | L   | 0 |
| 2 | C   | 2 |
| 3 | R   | 1 |
| 4 | SL  | 4 |
| 5 | SR  | 5 |
| — | (zero) | 6, 7 |

The decoder requests `A52_3F2R | A52_LFE | A52_ADJUST_LEVEL` (level 1.0) so
liba52 itself downmixes any non-5.1 AC-3 (mono/stereo/3F/2F2R sources) into a
known layout; whatever acmod arrives, the output slot meaning is fixed by the
request flags, not by the stream.

## 4. Architecture

```
ts_demux (PMT: 0x03/0x04 → MP3, 0x81/0x06+desc → AC-3)
   └─ adec_push_pes()  ── PES queue (compressed, absorbs sub-burn burst)
        └─ adec thread ── decoder dispatch (runtime, per PMT stream type)
             ├─ minimp3 (existing)  → stereo frames
             └─ liba52  (new)       → 6-ch frames in PS3 slot order
                  └─ PCM ring  (frames of `ch` floats)
                       └─ audio_write_pcm(): pad to 8 ch, zero 6–7, volume,
                          DMA block write (256 frames regardless of ch)
```

- **`audio_open(int channels)`** (2 or 8). Every hardcoded `2 *` in block
  addressing (`audio.cpp:161,199,215`) and `apply_volume` becomes the runtime
  channel count. Stereo path keeps `AUDIO_BLOCK_8`; the 8-ch port uses
  `AUDIO_BLOCK_16` (Movian uses 16, `ps3_audio.c:94`; 8 blocks is 42 ms of
  runway, thin for a software 5.1 decode). `audio_get_clock_us()` already
  expresses hardware latency in frames via `s_num_blocks` — unchanged math,
  stays frame-based.
- **Port lifecycle**: each playback session opens its own port
  (`player.cpp:261`, `music_player.cpp:432`), so: video player opens 8 ch
  when the surround setting is ON, 2 ch otherwise; the **music player always
  opens 2 ch — untouched**. No mid-session port reopen, no new DMA
  event-queue transitions.
- **Source contract**: `audio_set_source()` becomes "frames of N floats";
  the source advertises its frame width. When the port is 8 ch but the
  decoder produces stereo (server refused AC-3 → MP3), `audio_write_pcm()`
  zero-pads each 2-float frame to 8 floats at DMA-copy time. Stereo content
  through the 8-ch port plays FL/FR only — correct, and the OS downmix
  handles the rest.
- **Decoder dispatch is runtime, from the PMT**, not compile-time: the PMT
  stream type selects minimp3 or liba52 per session, recorded in player
  state so a seek (which rebuilds the URL via `build_stream_url()` and
  reopens the stream, `player_seek.cpp:251`) re-selects the same decoder.
  If the PMT delivers MP3 despite an AC-3 request, playback degrades to
  working stereo, never to noise.
- **No new threads.** liba52 runs on the existing adec thread
  (`adec_thread_fn`), which already ends in `sysThreadExit(0)`.

## 5. Demux + server negotiation

- `ts_demux.cpp`: accept PMT stream type `0x81` (ATSC AC-3, what ffmpeg's
  mpegts muxer writes), and `0x06` private-data with an AC-3 descriptor
  (DVB tag 0x6A or ATSC registration). `0x87` (E-AC-3) is **recognised and
  logged but rejected** (we cannot decode it) so a misconfigured server shows
  up in `player_log.txt` as a named stream type, not as silence. Every PMT
  audio entry is logged with its stream type.
- `api_detail.cpp`: surround ON adds a **parallel** DeviceProfile blob —
  `"AudioCodec":"ac3,mp3"`, `"MaxAudioChannels":"6"`, plus a `VideoAudio`
  CodecProfile for `ac3` with `AudioChannels <= 6`. Both `body_sd` and
  `body_hd`. The shipped blobs remain byte-identical for surround OFF.
- `player_session.cpp:build_stream_url()`: surround ON →
  `&AudioCodec=ac3&AudioBitrate=640000&AudioSampleRate=48000&MaxAudioChannels=6`.
  (448000 is the fallback lever if PES sizing or PPU load argues for it —
  DVD-standard 5.1 rate.)
- Verified against a real Jellyfin server with `PlaybackInfo` + `ffprobe`
  of the actual `stream.ts` before merge (§8.3).

## 6. Buffer sizing and memory budget

Frame counts (time depths) are load-bearing for the sub-burn fix and stay;
only the per-frame width changes.

| Buffer | Today | Surround build | Delta |
|---|---|---|---|
| PCM ring | `float[65536 × 2]` = 512 KB | `float[65536 × 6]` = 1.5 MB (static, max width; stereo uses 2/6 of it) | +1 MB |
| High-water | 48000 frames (1.0 s) | unchanged (frames) | 0 |
| PES queue | 256 × 8192 B = 2 MB | slot size OK (640 kbps AC-3 sync frame ≈ 2560 B < 8192); **slot count must be re-derived** | see below |
| liba52 state | — | `a52_state_t` + internal buffers, ~O(10 KB) static | ~+16 KB |

**PES queue depth — the one open sizing question.** The queue exists to hold
the ~10 s front-loaded audio burst of a `SubtitleMethod=Encode` reopen
(`adec.cpp:16-44`). Depth is time-based: the measured MP3 worst case was 80
PES deep because ffmpeg batches ~5 MP3 frames per PES (max 2894 B). AC-3 at
one 32 ms sync frame per PES would need ~313 slots for the same 10 s — over
the current 256. Whether ffmpeg batches AC-3 frames per PES must be
**measured, not assumed**: first RPCS3 run with the sub-burn case reports
`adec_pes_hwm` (depth + max PES bytes) for AC-3, and the slot count is set
from that telemetry with the same >2.5–3× margin the MP3 sizing used.
Provisional ceiling if arithmetic holds: 512 slots = 4 MB (+2 MB). All
allocations static, peak logged via `meminfo` at port open.

Worst-case total new cost ≈ **+3 MB** static. Current app peak leaves >100 MB
of the 213 MB user space free (meminfo telemetry in player_log.txt); budget
is comfortable, but the number is logged and reported in the PR.

## 7. Settings, persistence, diagnostics

- New Settings row **"Surround 5.1 (Alpha)"**, default **OFF**, modelled
  line-for-line on the `hd1080` pattern (`source/util/hd1080.{h,cpp}`,
  `ui_settings.cpp` row, persisted as `jellyfin_surround.txt` via
  `jf_data_path()`).
- `plog` at open: `audio_open: ch=%d blocks=%d`; first decoded AC-3 frame:
  `adec_ac3: hz=%d acmod=%d lfe=%d bitrate=%d`; PMT: every audio stream type
  seen.
- Player stats overlay (`player_stats.cpp`) gains: negotiated codec,
  channel count, sample rate, ring fill (already %-based vs high-water).

## 8. Verification plan

1. **Static**: `-Wall` clean; diff of all surround-OFF paths shown to be
   byte-identical (same object code for the shipped blobs / audio path).
2. **Host-side unit test** (`tests/`): liba52 compiled natively, decode a
   small AC-3 file, assert the a52→PS3 remap against per-channel sine
   references generated by ffmpeg. The channel map is the highest-risk piece
   and is fully testable off-console.
3. **RPCS3** (`BUILD_FOR_RPCS3 1`) against a real Jellyfin server: capture
   `player_log.txt`; check `audio_ratio` (pcm vs silence blocks), `adec_pts`
   drift, `adec_ac3` line, PMT stream-type log.
4. **A/V sync**: ≥10 min play, report drift; 5× seek, sync recovers, codec
   selection survives.
5. **Sub-burn**: subtitle track on (forces `SubtitleMethod=Encode`), audio
   must not jump ahead; report `adec_pes_hwm` for AC-3 PES sizing (§6).
6. **Fallbacks**: stereo-only source; server refusing AC-3 → both must play
   stereo, not silence.
7. **Regressions**: music player unchanged; stereo movie with surround OFF
   byte-identical path.
8. On-hardware listening checklist + per-channel test-tone clip supplied
   with the PR.

## 9. Known limitations (v1)

- **HDMI (or multi-AV analog) only for 5.1.** S/PDIF LPCM is 2-ch; routing
  5.1 over optical needs the OS AC-3/DTS encoder, which Movian reaches via
  `audioOutConfigure` — bindings that upstream PSL1GHT does not export.
  Phase-4 detection/encoder config is deferred per plan (local-fork SDK
  additions with cited FNIDs, compiled out by default) and is **not in v1**.
- User must tick **Linear PCM 5.1 ch 44.1/48 kHz** in XMB → Settings → Sound
  Settings → Audio Output Settings; README gains this note.
- E-AC-3 sources play stereo (server transcodes to AC-3 only when the
  profile says so; if it ships E-AC-3 anyway we reject the PID and log it).
- Music playback is stereo by design; unchanged.

## 10. Facts still to verify from source (tracked)

1. ffmpeg mpegts AC-3 PES batching → `adec_pes_hwm` telemetry (§6).
2. Real-server PMT stream type for AC-3 (`0x81` expected) — dumped from an
   actual Jellyfin `stream.ts` (§5).
3. Whether XMB "Dolby Digital 5.1" S/PDIF users get OS-side encoding from an
   8-ch LPCM port without `audioOutConfigure` — not verifiable from source;
   listed as a limitation instead of assumed.

(The liba52 output channel order, originally on this list, was verified from
source — see §3.2.)
