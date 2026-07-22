<div align="center">
  <img src="ICON0.PNG" alt="JellyFin PS3">

  # JellyFin PS3

  **A native Jellyfin client for the PlayStation 3.**

  Browse and play your whole Jellyfin library — movies, TV, and music — from the
  couch, with a UI that feels like it belongs on the console.

  Version **2.2.1 (beta)** · C/C++ · PSL1GHT · Evilnat CFW / HEN

</div>

---

## Screenshots

<div align="center">

**Home**

<img src="docs/screenshots/home.png" alt="Home screen with Continue Watching, Next Up and Recently Added rows" width="80%">

**Movies**

<img src="docs/screenshots/movies.png" alt="Movies library as a poster grid with an A–Z jump bar" width="80%">

**Now Playing (Music)**

<img src="docs/screenshots/music.png" alt="Now Playing music screen with album art and a real-time spectrum visualizer" width="80%">

</div>

---

## What it does

- **Movies, TV, Collections and Music**, browsed as poster/still card grids with an
  A–Z jump bar and scrollbars that show where you are in the library.
- **A home shelf** that mirrors the Jellyfin web app — Continue Watching, Next Up,
  and Recently Added rows.
- **Hardware H.264 playback** through the PS3's VDEC, with a smooth 60 fps display
  loop and temporal frame blending so 24 fps content doesn't judder.
- **AV sync locked to ±5 ms** off the audio clock.
- **Resume and progress reporting** — where you stop on the PS3 shows up as
  Continue Watching everywhere else, and the next episode auto-advances.
- **In-player HUD** with a seek bar, transport controls, and audio/subtitle track
  menus (subtitles are burned in server-side).
- **Seek, skip and scrub** — tap to jump 10 s, hold to scrub the bar.
- **A full music player** — Albums / Artists / Playlists / Genres / Songs, a play
  queue with shuffle, and a Now Playing screen with a real-time 28-band spectrum
  visualizer that reacts to what you actually hear.
- **Live search**, an on-screen keyboard, item info overlays, and a thumbnail cache
  to keep browsing snappy.
- **Update check at launch** — a quiet popup when a newer release is out.

---

## Requirements

- A PS3 running **Evilnat CFW** or **HEN** (CEX)
- A **Jellyfin server** reachable from the console (local network recommended)
- To build: the **PSL1GHT toolchain** (`ppu-gcc` under `/usr/local/ps3dev/`) and
  `sfo.xml` at `~/ps3dev/ps3py/sfo.xml` (used for the `.pkg` target)

---

## Install

Grab `JellyFin---PS3.pkg` from the [latest release](../../releases/latest) and
install it, or copy the `.self` over FTP/USB and launch it through webMAN or
multiMAN.

## Build from source

```bash
make clean && make      # SELF only
make pkg                # installable PKG
```

Output: `JellyFin---PS3.self` and `JellyFin---PS3.pkg`.

> When cutting a release, bump `APP_VERSION` in `source/net/update_check.h` to match
> the release tag so the update check compares correctly.

---

## Controls

**Menus** — `X` select · `O` back · `D-pad` navigate · `L1`/`R1` switch tabs ·
`△` item info.
In Music, press `Up` from the top row to reach the sub-tab header.

**Video player** — press any button to show the HUD (auto-hides after 4 s).
`Left`/`Right` move across the control row, `X` activates it. `R2`/`L2` tap to
skip ±10 s or hold to scrub. `Start` stops. `Select` jumps to the next episode
during the last-90 s prompt.

**Music player** — `Left`/`Right` move across the transport row; `Right` past
Shuffle enters the UP NEXT queue. `L1`/`R1` previous/next track, `△` toggles
shuffle, `R2`/`L2` seek, `Select` opens the full-queue overlay.

**Search** — type on the on-screen keyboard, `Down` jumps to results, `△` toggles
caps.

<details>
<summary>Full button reference</summary>

### Menus

| Button   | Action                                             |
|----------|----------------------------------------------------|
| X        | Select / confirm                                   |
| O        | Back                                               |
| D-pad    | Navigate                                           |
| L1 / R1  | Cycle tabs (prev/next page in the season browser)  |
| Triangle | Item info overlay                                  |

### Video player

| Button          | Action                                                            |
|-----------------|-------------------------------------------------------------------|
| Start           | Stop / exit player                                                |
| Left / Right    | Move focus across the control row (Rew · Play/Pause · FF · AUDIO · CC) |
| X               | Activate the focused control                                      |
| R2 / L2 (tap)   | Skip +10 s / -10 s (taps within 1 s batch into one seek)          |
| R2 / L2 (hold)  | Pause and scrub the seek bar; seek fires once on release          |
| Select          | Jump to the next episode/movie (during the NEXT prompt, last 90 s)|

### Music player

| Button             | Action                                                        |
|--------------------|---------------------------------------------------------------|
| Left / Right       | Move focus across the transport row                           |
| Right past Shuffle | Enter the UP NEXT queue                                        |
| Up / Down (queue)  | Scroll the full remaining queue                               |
| X                  | Activate control / play the highlighted queue track           |
| Triangle           | Toggle shuffle                                                 |
| L1 / R1            | Previous / next track (previous restarts when >3 s in)        |
| R2 / L2            | Seek (tap batches, hold scrubs)                               |
| Select             | Full-queue overlay                                            |
| O / Start          | Stop playback and return to the library                       |

### Search

| Button    | Action                                  |
|-----------|-----------------------------------------|
| D-pad     | Move cursor on keyboard / in results    |
| X         | Type character / play result            |
| Triangle  | Toggle caps lock                        |
| O / CLEAR | Reset search, return to keyboard        |
| Down      | Jump from keyboard to results           |
| Up        | Jump from first result back to keyboard |

</details>

---

## How it works

The architecture follows [Movian](https://github.com/andoma/showtime) closely —
Movian's player was the reference this whole app was built from.

**Video.** An HTTP MPEG-TS transcode stream is demuxed on a decode thread; H.264
access units go to the PS3 VDEC (SPU-accelerated), decoded frames land in a
16-slot jitter buffer, and a Movian-style 60 fps display loop blits them via the
RSX. A crossfade shader blends between decoded 24 fps frames using a Bresenham
accumulator locked to hardware vsync, so there's no 2:3 pulldown judder.

**Audio.** MP3 is decoded with minimp3 into a PCM ring and pushed to the PS3 audio
DMA at 48 kHz. The audio PTS drives the master clock; an EMA keeps AV sync inside
±5 ms.

**Seeking.** Jellyfin's transcode isn't byte-seekable, so seeks mirror Movian's
flush-and-reopen path: stop the decode thread, flush the decoder / audio / jitter
buffer, re-request the stream at the new `StartTimeTicks`, and resume. The audio
clock re-seeds from the new segment, so the seek bar snaps to the right spot.

**Music.** The music player reuses the same audio port through a pluggable PCM
source, pulling Jellyfin's audio transcode endpoint. The spectrum visualizer taps
samples at the DMA read cursor — not at decode time — so the bars match what's
coming out of the speakers rather than what's buffered ahead.

<details>
<summary>Deeper technical notes (pipelines, HUD, threading, file layout)</summary>

### Video pipeline

```
HTTP stream (MPEG-TS)
        |
        v
  Decode thread: stream_read() -> 188-byte TS packets -> video_feed_ts()
  TS demuxer -> PAT/PMT -> PES reassembly
        |
        +- Audio PES -> adec_push_pes() -> minimp3 -> PCM ring
        |
        +- Video PES -> vdecDecodeAu() -> VDEC (SPU H.264)
                |
                v
         VDEC callback: vdec_pull_frame() -> YUV -> ARGB
                |
                v
         Jitter buffer (16 slots, ~28 MB at 720p, PTS + duration per slot)
                |
                v
         Upload thread -> RSX-local texture (double-buffered, A + B for blend)
                |
                v
         Display loop (60fps): Bresenham gate + gcmSetVBlankHandler @ 59.94 Hz
                |
                v
         RSX blit: crossfade shader blends A -> B mid-pulldown, flip()
```

- **FPS detection:** VDEC `frame_rate_code` maps to exact fractional fps
  (ISO 13818-2); refresh rate from `videoGetState` (59.94 Hz).
- **Temporal blending:** each frame carries a remaining duration; when it drops
  below one vblank and the next frame is ready, the shader crossfades on the
  fractional remainder — no fixed pulldown pattern.
- **AV sync:** `avsync_compute_diff()` = video PTS − audio PTS, EMA-smoothed;
  each vblank period is nudged ±5000 µs to correct drift. Locked below ~41.6 ms.

### Seek pipeline

Modelled on Movian's `mp_flush` reposition path:

```
Compute target = audio_clock + delta -> Jellyfin 100-ns ticks (StartTimeTicks)
Stop decode thread (audio + upload keep running, idle on empty buffers)
Flush: vdec_flush() · adec_flush() · jbuf_clear() · video_reset_demux() · avsync_reset()
Re-request stream at new StartTimeTicks -> re-prefill jitter buffer
Respawn decode thread -> resume (audio clock re-seeds from new segment)
```

`SEEK_REOPEN_VDEC` swaps the decoder reset for a full close/open rebuild — slower
but bulletproof, for hardware A/B testing.

### HUD overlay

The in-player HUD is composed with the CPU into a staging buffer **only when its
content changes**, uploaded to an RSX texture, and drawn each frame as one
alpha-blended quad over the video — so a visible seek bar costs the display loop
almost nothing. This was the endpoint of several iterations (a vertex-array quad
that froze the console on pause, a CPU framebuffer blend that dropped to ~5 fps, a
per-frame CPU draw that halved the frame rate). The earlier dim-strip paths are
kept behind compile defines for hardware testing:

| Define            | Path                                                  |
|-------------------|-------------------------------------------------------|
| (none, default)   | Inline GPU quad, fast and freeze-proof                |
| HUD_DIM_CPU       | CPU pixel blend, slow but bulletproof fallback        |
| HUD_DIM_GPU_ARRAY | Original array-fetch quad, known to freeze, test only |

While paused with no input, the display loop stops redrawing entirely — the frame
is pixel-identical, so it just polls input at vblank rate until something changes.

### Audio & burned-in subtitles

Selecting a subtitle track switches the server to `SubtitleMethod=Encode`, which
front-loads several seconds of audio while the subtitle-burning encoder warms up.
A 256-slot PES queue holds that burst compressed and the decoder back-pressures on
the PCM highwater, so nothing is dropped and playback stays in sync (this used to
skip ~10 s ahead on real hardware).

### Threading model

| Thread         | Priority | Role                                                     |
|----------------|----------|----------------------------------------------------------|
| Display (main) | default  | Bresenham gate, RSX blit, flip, input poll, seek control |
| Decode         | 800      | TS demux, VDEC submit, jitter buffer fill                |
| Upload         | 850      | memcpy jitter buffer to RSX texture (A + B)              |
| Audio          | 750      | DMA event loop, PCM ring drain                           |
| Progress       | 1100     | POST position to Jellyfin every ~10 s                    |
| Async log      | 1200     | Ring-buffer drain to `player_log.txt` (when enabled)     |

On seek, only the decode thread is stopped and respawned.

### Source layout

```
source/
|-- api/      Jellyfin REST surface (auth, browse, detail, playstate)
|-- audio/    Audio port + DMA ring, minimp3 decode
|-- cache/    Thumbnail caching
|-- gfx/      RSX helpers, shaders, embedded fonts, stb_image/truetype
|-- music/    Audio-only engine, FFT visualizer, Now Playing screen
|-- net/      HTTP client, GitHub update check
|-- player/   Core loop, HUD, GPU draw, decode/upload/audio threads, TS stream
|-- ui/       XMB UI: input, OSK, home shelf, browse, search, settings, rendering
|-- util/     Frame pacing / AV sync, async logging
`-- video/    Session glue, TS demux, VDEC, jitter buffer
```

</details>

---

## Logging

Debug logging is **off by default** and toggled from the Settings tab (the choice
persists in `/dev_hdd0/tmp/jellyfin_settings.txt`). When on, output goes to
`/dev_hdd0/tmp/player_log.txt`. A crash log is always written synchronously to
`/dev_hdd0/tmp/crash_log.txt`, and the launch update check traces to
`/dev_hdd0/tmp/update_detection.txt`.

---

## Acknowledgments

- **[Movian](https://github.com/andoma/showtime)** (formerly Showtime) — this whole
  app was built using Movian's media player as a reference. The video display loop,
  the temporal frame blending, and the flush-and-reopen seek path all follow
  Movian's design. Huge thanks to Andreas Öman and the Movian contributors.
- **[ps3dev](https://github.com/ps3dev)** — for keeping
  **[PSL1GHT](https://github.com/ps3dev/PSL1GHT)** alive: the SDK, toolchain, and
  libraries that make open PS3 homebrew possible. PSL1GHT is MIT-licensed,
  Copyright (c) 2011 PSL1GHT Development Team.
- The authors of the embedded libraries — **minimp3**, **stb_image**,
  **stb_truetype** — and the **Jellyfin** project.

---

## License

JellyFin PS3 is free software licensed under the **GNU General Public License v3.0**
(or, at your option, any later version). See the [LICENSE](LICENSE) file for the
full text.

Copyright (C) 2026 Montague McKeefry

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

Bundled third-party components retain their own licenses — notably the PSL1GHT SDK
(MIT), which is GPLv3-compatible.
