<div align="center">
  <img src="ICON0.PNG" alt="JellyFin PS3 banner">
</div>

# JellyFin PS3

A native PS3 homebrew Jellyfin client written in C/C++ using PSL1GHT, targeting Evilnat CFW or HEN.

Goal: consumer-quality media playback on PS3, the second-best media player on the platform, modelled architecturally on Movian.

---

## Features

- XMB-style UI with animated wave background
- Browse Movies, TV Shows, and Collections libraries
- TV show browser, Series then Seasons then Episodes
- Collections browser, Collection then Movies
- Live search across your Jellyfin library (fires on every keystroke)
- Custom on-screen keyboard for login and search
- Item info overlay (overview, rating, genres, studios, video and audio stream info)
- Thumbnail cache to keep browsing responsive
- Open Sans and Material Icons font rendering via stb_truetype
- Hardware H.264 decode via PS3 VDEC (SPU-accelerated)
- Movian-style temporal frame blending, smooth 60fps display loop with crossfade between decoded 24fps frames
- Bresenham 2:3 pulldown, locked to hardware vsync via gcmSetVBlankHandler
- AV sync locked to within plus or minus 5ms using audio PTS clock plus EMA smoothing
- MP3 audio decode via minimp3 with PCM ring buffer
- Interleaved stereo DMA audio output at 48kHz
- Double-buffered RSX GPU blit via custom vertex and fragment shaders
- In-player HUD: one compact strip with a focusable control row (rewind, play/pause, fast-forward, AUDIO, CC), seek bar, and elapsed/remaining times
- Audio track and subtitle selection via popup menus (Jellyfin AudioStreamIndex / SubtitleStreamIndex); subtitles are burned in server-side (SubtitleMethod=Encode) since the PS3 has no subtitle renderer
- Title overlay in the top-left while paused
- Seek: R2/L2 tap skips 10s (rapid taps batch into one jump), hold pauses and scrubs the bar in 25s steps with a single reposition on release
- Every reposition (seek or track change) stops the server transcode, mints a fresh PlaySessionId via PlaybackInfo, and re-requests stream.ts at the target StartTimeTicks, with full decoder, audio, and jitter-buffer flush
- Jellyfin PlaybackInfo POST with PS3 H.264 transcode profile (720p)
- .pkg packaging via PSL1GHT built-in ppu_rules flow (APPID JFPS30000)
- Crash log written synchronously to /dev_hdd0/tmp/crash_log.txt
- Async ring-buffer logging system (player log at /dev_hdd0/tmp/player_log.txt)

---

## Requirements

- PS3 with Evilnat CFW or HEN (CEX)
- PSL1GHT toolchain (ppu-gcc at /usr/local/ps3dev/ppu/bin/)
- Jellyfin server reachable from your PS3 (local network recommended; port-forwarded remote also works)
- sfo.xml at ~/ps3dev/ps3py/sfo.xml (used by ppu_rules for the .pkg target)

---

## Building

```bash
# Build SELF only
make clean && make

# Build installable PKG
make pkg
```

Output: JellyFin---PS3.self and JellyFin---PS3.pkg

Transfer the SELF to your PS3 via FTP or USB and launch through webMAN or multiMAN, or install the PKG directly.

---

## Controls

### Menus

| Button      | Action                          |
|-------------|---------------------------------|
| X           | Select / confirm                |
| O           | Back                            |
| D-pad       | Navigate                        |
| L1 / R1     | Cycle tabs (prev/next page in the season browser) |
| Triangle    | Item info overlay               |

### Media Player

Press any button during playback to bring up the HUD. It auto-hides after 4 seconds while playing and stays up while paused.

| Button          | Action                                                                  |
|-----------------|-------------------------------------------------------------------------|
| Start           | Stop / exit player                                                      |
| Left / Right    | Move focus across the control row (Rew · Play/Pause · FF · AUDIO · CC) |
| X               | Activate the focused control (the reveal press is swallowed)           |
| X on Rew / FF   | Seek -10s / +10s                                                        |
| X on AUDIO / CC | Open the audio / subtitle track popup menu                              |
| R2 / L2 (tap)   | Skip +10s / -10s (taps within 1s batch into one seek)                   |
| R2 / L2 (hold)  | Pause and scrub the seek bar; the seek fires once on release            |

Track menu: D-pad Up/Down to highlight, X to select, O to close. The active entry carries an accent dot; the CC menu has an Off entry at the top, and an active subtitle underlines the CC button. Picking a different track reopens the stream at the current position with the new track applied (subtitles can take a while to start the first time — the server extracts the track before the burn-in transcode begins).

Seeking, skipping, and track changes all use the same path: the player re-requests the transcode stream at the new position, flushes the decoder, audio, and jitter buffer, then resumes. The seek bar position follows the audio PTS clock (offset by the seek base), so it tracks the real playback time after a seek.

### Search

| Button   | Action                                  |
|----------|-----------------------------------------|
| D-pad    | Move cursor on keyboard / in results    |
| X        | Type character / play result            |
| Triangle | Toggle caps lock                        |
| O / CLEAR| Reset search, return to keyboard        |
| Down     | Jump from keyboard to results           |
| Up       | Jump from first result back to keyboard |

---

## Video Pipeline

```
HTTP stream (MPEG-TS)
        |
        v
  Decode thread
  stream_read() -> 188-byte TS packets
        |
        v
  video_feed_ts()
  TS demuxer -> PAT/PMT -> PES reassembly
        |
        +- Audio PES -> adec_push_pes() -> minimp3 -> PCM ring buffer
        |
        +- Video PES -> vdecDecodeAu() -> VDEC (SPU H.264)
                |
                v
         VDEC callback
         vdec_pull_frame() -> YUV -> ARGB
                |
                v
         Jitter buffer (16 slots, ~28 MB at 720p)
         PTS + per-frame duration stored per slot
                |
                v
         Upload thread (priority 850)
         memcpy -> RSX-local texture (double-buffered)
         Also uploads frame B for blend
                |
                v
         Display loop (Movian-style, 60fps)
         timing_flip_due() <- Bresenham accumulator
         gcmSetVBlankHandler <- hardware vsync at 59.94 Hz
                |
                v
         RSX GPU blit
         Crossfade shader blends frame A -> B mid-pulldown
         rsxSync() -> flip()
```

**FPS detection:** VDEC frame_rate_code maps to exact fractional fps (ISO 13818-2). Display refresh rate queried via videoGetState, 59.94 Hz detected and used for the Bresenham accumulator.

**Temporal blending:** Each decoded frame carries a remaining duration (us). The display loop consumes vblank periods from that budget. When a frame's remaining duration falls below one vblank period and the next frame is available, the shader crossfades A to B based on the fractional time remaining, eliminating judder on 24fps content without a fixed pulldown pattern.

**AV sync:** avsync_compute_diff() computes video PTS minus audio PTS. An EMA smooths it over time; avsync_biased_period() nudges each vblank period plus or minus 5000 us to correct drift. Locked = absolute EMA below 41 667 us (about 1 frame at 24fps).

---

## Seek Pipeline

Seek, rewind, and skip are modelled on Movian's flush-and-reposition path, adapted to a single-socket Jellyfin transcode (which is not byte-seekable).

```
User seeks (Left/Right, L2/R2, or Rew/FF button)
        |
        v
  Compute target = audio_clock + delta, clamp >= 0
  Convert to Jellyfin 100-ns ticks (StartTimeTicks)
        |
        v
  Stop decode thread (dec_run flag + join)
  Audio and upload threads keep running, idle on empty buffers
        |
        v
  Flush (mirrors Movian mp_flush):
    vdec_flush()        end + restart VDEC sequence, drop reference frames
    adec_flush()        empty PES queue + PCM ring, invalidate PTS cursors
    jbuf_clear()        drop all buffered decoded frames
    video_reset_demux() re-acquire PAT/PMT for the new segment
    avsync_reset()      forget the smoothed AV diff
        |
        v
  Re-request stream at new offset (StartTimeTicks)
  Re-prefill jitter buffer
        |
        v
  Respawn decode thread on the new socket
  Resume (audio clock re-seeds from the new segment's first PTS)
```

The audio clock drives the seek bar, so the bar jumps to the new position automatically once the first PTS of the new segment is decoded. A SEEK_REOPEN_VDEC compile define switches the decoder reset from sequence end/restart to a full vdec_close() + vdec_open() rebuild, a slower but bulletproof fallback for A/B testing on hardware.

---

## HUD Overlay and the dim strip

The in-player HUD draws a darkened strip along the bottom of the screen behind the controls. Getting this right was non-trivial on RSX:

- The first approach drew the strip as a GPU quad using vertex arrays (rsxBindVertexArrayAttrib + rsxDrawVertexArray). This hard-froze the console when paused, because the GPU stalled fetching a stale TEX0 attribute array left bound by the video path.
- The second approach blended the strip on the PPU directly into the framebuffer. It never froze, but color_buffer is CPU-writable RSX VRAM, and per-pixel read-modify-write over the bus pushed frame time from about 16.7ms to about 183ms (around 5fps) whenever the HUD was visible.
- The current approach matches how Movian draws its overlays: a GPU quad with vertices submitted inline into the command FIFO (rsxDrawVertexBegin / rsxDrawVertex4f / rsxDrawVertexEnd). Inline submission has no vertex-array fetch, so it cannot wedge on a stale binding, and it stays on the GPU, so frame time returns to one vblank.

Three implementations are kept behind compile defines so the fast path can be swapped instantly if needed:

| Define              | Path                                                  |
|---------------------|-------------------------------------------------------|
| (none, default)     | Inline GPU quad, fast and freeze-proof                |
| HUD_DIM_CPU         | CPU pixel blend, slow but bulletproof fallback        |
| HUD_DIM_GPU_ARRAY   | Original array-fetch quad, known to freeze, test only |

A single rsxSync() fences the prior video draw before the HUD reprograms RSX state, which is also required to avoid a paused-state collision with in-flight video commands.

---

## Audio Pipeline

```
Audio PES packets (MP3, type 0x03)
        |
        v
  adec_push_pes()
  PES header stripped (9 + buf[8] bytes)
        |
        v
  minimp3 decode loop
  mp3dec_decode_frame() -> 1152 samples/frame
  short PCM -> float32, interleaved L/R
        |
        v
  PCM ring buffer (8192 sample-pairs, ~170ms at 48kHz)
        |
        v
  Audio thread (priority 750)
  sysEventQueueReceive() -> DMA event
  Blocks up to 100ms waiting for 256 samples
        |
        v
  PS3 audio DMA (8 blocks x 256 samples, 48kHz)
  Interleaved layout: L0 R0 L1 R1 ... (per SDK spec)
```

**AV clock:** audio_get_clock_us() returns PTS-based time once the first PES with a valid PTS is decoded; falls back to the DMA block counter at startup. After a seek the PTS cursor is invalidated, so the clock re-seeds from the new segment.

---

## Threading Model

| Thread         | Priority | Role                                                       |
|----------------|----------|------------------------------------------------------------|
| Display (main) | default  | Bresenham gate, RSX blit, flip, input poll, seek control   |
| Decode         | 800      | TS demux, VDEC submit, jitter buffer fill                  |
| Upload         | 850      | memcpy jitter buffer to RSX texture (A + B)                |
| Audio          | 750      | DMA event loop, PCM ring drain                             |
| Async log      | default  | Ring-buffer drain to player_log.txt                        |

On seek, only the decode thread is stopped and respawned; the audio and upload threads stay alive and idle on empty buffers.

---

## File Structure

Source is organised into subdirectories by domain.

```
JellyFin---PS3/
|-- Makefile
|-- ICON0.PNG
`-- source/
    |-- main.cpp                  # Entry point, synchronous crash_log
    |-- api/
    |   |-- jellyfin_api.cpp/h     # Jellyfin REST API surface and shared state
    |   |-- api_auth.cpp           # Login / authentication
    |   |-- api_browse.cpp         # Libraries, items, seasons, episodes, search
    |   `-- api_detail.cpp         # Item detail, PlaybackInfo, transcode URL
    |-- audio/
    |   |-- audio.cpp/h            # Audio port, DMA ring buffer, audio thread, PTS clock
    |   |-- adec.cpp/h             # MP3 decode via minimp3, PCM ring buffer, adec_flush
    |   `-- minimp3.h              # Embedded MP3 decoder
    |-- cache/
    |   `-- thumbnail_cache.cpp/h  # Thumbnail caching for browse views
    |-- gfx/
    |   |-- rsxutil.cpp/h          # RSX helpers, shader blit, framebuffer access
    |   |-- bitmap.cpp/h           # Image loading
    |   |-- video_shaders.h        # Video vertex/fragment ucode (YUV to ARGB + crossfade)
    |   |-- hud_dim_shaders.h      # HUD dim-quad vertex/fragment ucode
    |   |-- hud_dim_vp.vasm        # HUD dim vertex program source
    |   |-- hud_dim_fp.vasm        # HUD dim fragment program source
    |   |-- wave_shaders.h         # Wave background shader ucode
    |   |-- stb_truetype.h         # TTF rasterizer
    |   |-- stb_image.h            # Image decoder
    |   |-- opensans_regular.h     # Open Sans Regular (embedded)
    |   |-- opensans_bold.h        # Open Sans Bold (embedded)
    |   `-- font8x8.xpm            # Fallback bitmap font
    |-- net/
    |   `-- http.cpp/h             # HTTP client
    |-- player/
    |   |-- player.cpp/h           # Playback orchestrator, display loop, seek control
    |   |-- player_internal.h      # Shared player state, DecodeCtx
    |   |-- player_threads.cpp     # Decode / upload / audio thread bodies
    |   |-- player_gpu.cpp         # vid_gpu_draw wrapper
    |   |-- player_rsx.cpp/h       # RSX frame draw (blit + crossfade)
    |   |-- player_hud.cpp/h       # HUD overlay, seek bar, dim strip, input mapping
    |   `-- stream.cpp/h           # HTTP MPEG-TS reader (chunked transfer, TS ring)
    |-- ui/
    |   |-- ui.cpp/h               # Main menu, browse/search loop, OSK, tab detection
    |   |-- ui_nav.cpp             # Navigation logic
    |   |-- ui_search.cpp          # Search loop
    |   |-- ui_info.cpp            # Item info overlay
    |   |-- ui_visuals.cpp/h       # XMB draw calls
    |   |-- ui_wave.cpp/h          # Animated wave background (RSX)
    |   |-- material_icons.h       # Material Icons (embedded)
    |   `-- iconic_psx.h           # PSX-style iconography
    |-- util/
    |   |-- timing.cpp/h           # Frame pacing, Bresenham accumulator, AV sync EMA
    |   `-- plog.cpp/h             # Async ring-buffer logging
    `-- video/
        `-- video.cpp/h            # VDEC init, H.264 decode, jitter buffer, fps detection, flush
```

---

## Status

| Feature                  | Status                                                            |
|--------------------------|-------------------------------------------------------------------|
| Login / Auth             | Working                                                           |
| Movie browsing           | Working                                                           |
| TV show browsing         | Working (Series, Seasons, Episodes)                               |
| Collections browsing     | Working                                                           |
| Search                   | Working (live, keystroke-driven)                                  |
| Item info overlay        | Working (overview, rating, genres, studios, video/audio info)     |
| Thumbnail cache          | Working                                                           |
| Video playback           | Working (720p H.264, Movian-style 60fps display loop)             |
| Temporal frame blending  | Working (crossfade shader, eliminates 2:3 judder)                 |
| Audio playback           | Working (48kHz stereo MP3, zero silence blocks)                   |
| AV sync                  | Locked (plus or minus 5ms via PTS clock + EMA bias)               |
| HUD overlay              | Working (inline GPU dim quad, no freeze, full-speed)              |
| Seek / rewind / skip     | Working (tap-to-skip + hold-to-scrub, StartTimeTicks re-request + full pipeline flush) |
| Audio / subtitle tracks  | Working (popup menus; subtitles burned in server-side)            |
| PlaybackInfo / transcode | Working (H.264 720p profile, PlaySessionId extracted)             |
| PKG packaging            | Working (make pkg, APPID JFPS30000)                               |
| Music library            | Not implemented                                                   |

---

## Logging

During playback, async log output is written to /dev_hdd0/tmp/player_log.txt. The crash log at /dev_hdd0/tmp/crash_log.txt is written synchronously at key lifecycle checkpoints (including per-step seek and HUD checkpoints) and survives crashes that prevent the async logger from flushing. Reading it from the bottom up pinpoints the exact step that failed on hardware.

---

## License

MIT
