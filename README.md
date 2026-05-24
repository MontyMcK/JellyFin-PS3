![JellyFin PS3 banner](ICON0.PNG)

# JellyFin PS3

A native PS3 homebrew Jellyfin client written in C using PSL1GHT, targeting Evilnat CFW or HEN.

Goal: consumer-quality media playback on PS3 — the second-best media player on the platform, modelled architecturally on Movian.

---

## Features

- XMB-style UI with animated wave background
- Browse Movies, TV Shows, and Collections libraries
- TV show browser — Series → Seasons → Episodes
- Collections browser — Collection → Movies
- Live search across your Jellyfin library (fires on every keystroke)
- Custom on-screen keyboard for login and search
- Open Sans + Material Icons font rendering via stb_truetype
- Hardware H.264 decode via PS3 VDEC (SPU-accelerated)
- Movian-style temporal frame blending — smooth 60fps display loop with crossfade between decoded 24fps frames
- Bresenham 2:3 pulldown, locked to hardware vsync via `gcmSetVBlankHandler`
- AV sync locked to within ±5ms using audio PTS clock + EMA smoothing
- MP3 audio decode via minimp3 with PCM ring buffer
- Interleaved stereo DMA audio output at 48kHz
- Double-buffered RSX GPU blit via custom vertex/fragment shaders
- Jellyfin PlaybackInfo POST with PS3 H.264 transcode profile (720p)
- `.pkg` packaging via PSL1GHT built-in `ppu_rules` flow (APPID: `JFPS30000`)
- Crash log written to `/dev_hdd0/game/JFPS30000/USRDIR/crash_log.txt`
- Async ring-buffer logging system

---

## Requirements

- PS3 with Evilnat CFW or HEN (CEX)
- PSL1GHT toolchain (`ppu-gcc` at `/usr/local/ps3dev/ppu/bin/`)
- Jellyfin server reachable from your PS3 (local network recommended; port-forwarded remote also works)
- `sfo.xml` at `~/ps3dev/ps3py/sfo.xml` (used by `ppu_rules` for `.pkg` target)

---

## Building

```bash
# Build SELF only
make clean && make

# Build installable PKG
make pkg
```

Output: `JellyFin---PS3.self` and `JellyFin---PS3.pkg`

Transfer the SELF to your PS3 via FTP or USB and launch through webMAN or multiMAN, or install the PKG directly.

---

## Controls

### Menus

| Button      | Action                          |
|-------------|---------------------------------|
| X           | Select / confirm                |
| O           | Back                            |
| D-pad       | Navigate                        |
| L1 / R1     | Cycle tabs                      |
| Triangle    | Item info overlay               |
| L1 / R1     | Prev/next page (season browser) |

### Media Player

| Button | Action     |
|--------|------------|
| Start  | Stop / exit player |

### Search

| Button   | Action                              |
|----------|-------------------------------------|
| D-pad    | Move cursor on keyboard / in results|
| X        | Type character / play result        |
| Triangle | Toggle caps lock                    |
| O / CLEAR| Reset search, return to keyboard    |
| Down     | Jump from keyboard to results       |
| Up       | Jump from first result back to keyboard |

---

## Video Pipeline

```
HTTP stream (MPEG-TS)
        │
        ▼
  Decode thread
  stream_read() → 188-byte TS packets
        │
        ▼
  video_feed_ts()
  TS demuxer → PAT/PMT → PES reassembly
        │
        ├─ Audio PES → adec_push_pes() → minimp3 → PCM ring buffer
        │
        └─ Video PES → vdecDecodeAu() → VDEC (SPU H.264)
                │
                ▼
         VDEC callback
         vdec_pull_frame() → YUV → ARGB
                │
                ▼
         Jitter buffer (16 slots, ~28 MB at 720p)
         PTS + per-frame duration stored per slot
                │
                ▼
         Upload thread (priority 850)
         memcpy → RSX-local texture (double-buffered)
         Also uploads frame B for blend
                │
                ▼
         Display loop (Movian-style, 60fps)
         timing_flip_due() ← Bresenham accumulator
         gcmSetVBlankHandler ← hardware vsync at 59.94 Hz
                │
                ▼
         RSX GPU blit
         Crossfade shader blends frame A → B mid-pulldown
         rsxSync() → flip()
```

**FPS detection:** VDEC `frame_rate_code` maps to exact fractional fps (ISO 13818-2). Display refresh rate queried via `videoGetState` — 59.94 Hz detected and used for Bresenham accumulator.

**Temporal blending:** Each decoded frame carries a remaining duration (us). The display loop consumes vblank periods from that budget. When a frame's remaining duration falls below one vblank period and the next frame is available, the shader crossfades A→B based on the fractional time remaining — eliminating judder on 24fps content without a fixed pulldown pattern.

**AV sync:** `avsync_compute_diff()` computes video PTS minus audio PTS. An EMA smooths it over time; `avsync_biased_period()` nudges each vblank period ±5000 µs to correct drift. Locked = |EMA| < 41 667 µs (~1 frame at 24fps).

---

## Audio Pipeline

```
Audio PES packets (MP3, type 0x03)
        │
        ▼
  adec_push_pes()
  PES header stripped (9 + buf[8] bytes)
        │
        ▼
  minimp3 decode loop
  mp3dec_decode_frame() → 1152 samples/frame
  short PCM → float32, interleaved L/R
        │
        ▼
  PCM ring buffer (8192 sample-pairs, ~170ms at 48kHz)
        │
        ▼
  Audio thread (priority 750)
  sysEventQueueReceive() → DMA event
  Blocks up to 100ms waiting for 256 samples
        │
        ▼
  PS3 audio DMA (8 blocks × 256 samples, 48kHz)
  Interleaved layout: L0 R0 L1 R1 … (per SDK spec)
```

**AV clock:** `audio_get_clock_us()` returns PTS-based time once the first PES with a valid PTS is decoded; falls back to DMA block counter at startup.

---

## Threading Model

| Thread         | Priority | Role                                         |
|----------------|----------|----------------------------------------------|
| Display (main) | default  | Bresenham gate, RSX blit, flip, input poll   |
| Decode         | 800      | TS demux, VDEC submit, jitter buffer fill     |
| Upload         | 850      | memcpy jitter buffer → RSX texture (A + B)   |
| Audio          | 750      | DMA event loop, PCM ring drain               |
| Async log      | default  | Ring-buffer drain to player_log.txt          |

---

## File Structure

```
JellyFin---PS3/
├── Makefile
├── ICON0.PNG
└── source/
    ├── main.cpp              # Entry point, crash_log
    ├── player.cpp/h          # Playback orchestrator — thread management, display loop
    ├── video.cpp/h           # VDEC init, H.264 decode, jitter buffer, fps detection
    ├── audio.cpp/h           # Audio port, DMA ring buffer, audio thread, PTS clock
    ├── adec.cpp/h            # MP3 decode via minimp3, PCM ring buffer
    ├── stream.cpp/h          # HTTP MPEG-TS reader (chunked transfer, TS ring buffer)
    ├── http.cpp/h            # HTTP client
    ├── jellyfin_api.cpp/h    # Jellyfin REST API (auth, libraries, items, PlaybackInfo)
    ├── ui.cpp/h              # Main menu, browse/search loop, OSK, tab detection
    ├── ui_visuals.cpp/h      # XMB draw calls — tabs, item lists, keyboard, search results
    ├── ui_wave.cpp/h         # Animated wave background (RSX)
    ├── bitmap.cpp/h          # Image loading
    ├── timing.cpp/h          # Frame pacing, Bresenham accumulator, AV sync EMA
    ├── rsxutil.cpp/h         # RSX helpers, shader blit
    ├── video_shaders.h       # Vertex/fragment shader ucode (YUV→ARGB + crossfade blend)
    ├── wave_shaders.h        # Wave background shader ucode
    ├── plog.h                # Async logging
    ├── minimp3.h             # Embedded MP3 decoder
    ├── stb_truetype.h        # TTF rasterizer
    ├── opensans_regular.h    # Open Sans Regular (embedded)
    ├── opensans_bold.h       # Open Sans Bold (embedded)
    ├── material_icons.h      # Material Icons (embedded)
    └── font8x8.xpm           # Fallback bitmap font
```

---

## Status

| Feature                  | Status                                                            |
|--------------------------|-------------------------------------------------------------------|
| Login / Auth             | ✅ Working                                                        |
| Movie browsing           | ✅ Working                                                        |
| TV show browsing         | ✅ Working (Series → Seasons → Episodes)                          |
| Collections browsing     | ✅ Working                                                        |
| Search                   | ✅ Working (live, keystroke-driven)                               |
| Item info overlay        | ✅ Working (overview, rating, genres, studios, video/audio info)  |
| Video playback           | ✅ Working (720p H.264, Movian-style 60fps display loop)          |
| Temporal frame blending  | ✅ Working (crossfade shader, eliminates 2:3 judder)              |
| Audio playback           | ✅ Working (48kHz stereo MP3, zero silence blocks)                |
| AV sync                  | ✅ Locked (±5ms via PTS clock + EMA bias)                         |
| PlaybackInfo / transcode | ✅ Working (H.264 720p profile, PlaySessionId extracted)          |
| PKG packaging            | ✅ Working (`make pkg`, APPID `JFPS30000`)                        |
| Music library            | ⚠️ not implemented                                                |

---

## Logging

During playback, async log output is written to `player_log.txt` in the app's working directory. The crash log at `/dev_hdd0/game/JFPS30000/USRDIR/crash_log.txt` is written synchronously at key lifecycle checkpoints and survives crashes that prevent the async logger from flushing.

---

## License

MIT
