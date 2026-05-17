<p align="center">
  <img src="ICON0.PNG" width="200"/>
</p>

# JellyFin PS3 Client

A native PS3 homebrew Jellyfin client written in C++ using PSL1GHT, targeting Evilnat CFW or HEN.

## Features

- XMB-style main menu with animated wave background
- Browse Movies, TV Shows, and Collections libraries
- TV show browser — Series → Seasons → Episodes
- Collections browser — Collection → Movies
- Search across your entire Jellyfin library
- Custom on-screen keyboard for login and search
- Open Sans font rendering via stb_truetype
- Material Icons tab icons
- HTTP streaming of transcoded video via MPEG-TS
- Hardware H.264 decode via PS3 VDEC (SPU-accelerated)
- 24fps frame-paced display with Bresenham 2:3 pulldown and hardware vsync
- MP3 audio decode via minimp3 with PCM ring buffer
- Interleaved stereo DMA audio output at 48kHz
- Double-buffered RSX GPU blit via vertex/fragment shaders
- Async logging system

## Requirements

- PS3 with Evilnat CFW or HEN (CEX)
- PSL1GHT toolchain (`ppu-g++` at `/usr/local/ps3dev/ppu/bin/`)
- Jellyfin server accessible from your PS3 (local network recommended, port-forwarded remote server also works)

## Building

```bash
make clean && make
```

Output: `jellyfin-ps3.self`

Transfer to PS3 via FTP or USB and launch through webMAN or multiMAN.

## Controls

### Menus
| Button | Action |
|--------|--------|
| X | Select |
| O | Back |
| D-pad | Navigate |
| L1 / R1 | Cycle tabs |
| Triangle | Info overlay |

### Media Player
| Button | Action |
|--------|--------|
| Start | Exit player |

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
  TS demuxer → PAT/PMT parsing → PES reassembly
        │
        ├─ Audio PES → adec_push_pes() → minimp3 → PCM ring buffer
        │
        └─ Video PES → vdecDecodeAu() → VDEC (SPU-accelerated H.264)
                │
                ▼
         VDEC callback
         vdec_pull_frame() → YUV frame → ARGB conversion
                │
                ▼
         Jitter buffer (16 slots, ~32 MB)
         PTS stored per slot
                │
                ▼
         Upload thread (priority 850)
         memcpy → RSX-local texture (double-buffered)
                │
                ▼
         Display thread
         timing_flip_due() ← Bresenham accumulator (24000/1001 fps)
         gcmSetVBlankHandler ← hardware vsync at 60000/1001 Hz
                │
                ▼
         RSX GPU blit
         vertex + fragment shaders → fullscreen quad
         rsxSync() → flip()
```

**FPS detection:** VDEC `frame_rate_code` maps to exact fractional fps (ISO 13818-2 table). Display refresh rate queried dynamically via `videoGetState` — 59.94Hz detected and used in Bresenham accumulator for correct 2:3 pulldown cadence.

**Frame pacing:** Bresenham accumulator fires `s_flip_trigger` at the exact vblank edge where a new frame is due. Display thread consumes the trigger via `timing_flip_due()`. `flip_late` logged if the trigger is consumed more than one vblank late.

**GPU blit:** RSX vertex/fragment shaders scale the YUV→ARGB texture to fit the display. `rsxSync()` before `flip()` ensures the RSX finishes all blit work before the framebuffer is presented.

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
  mp3dec_decode_frame() → 1152 samples per frame
  short PCM → float32, interleaved L/R pairs
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
  PS3 audio DMA (8 blocks, 256 samples each, 48kHz)
  Interleaved layout: L0 R0 L1 R1 ... (per SDK spec)
```

**Sample rate:** PS3 audio hardware runs at 48kHz. Stream is requested at 48kHz via `AudioSampleRate=48000` in the stream URL.

**DMA layout:** PS3 audio expects interleaved samples (L R L R). Each DMA block is 256 sample-pairs written as sequential float32 pairs. Audio thread blocks until a full 256-sample block is available before writing, preventing silence gaps that would corrupt pitch.

**Audio clock:** `audio_get_clock_us()` returns `(s_audio_blocks * 256ULL * 1000000ULL) / 48000ULL` — microseconds of audio played since start. Used as AV sync reference. PTS stored per jitter buffer slot via `s_jbuf_pts[]`.

## Threading Model

| Thread | Priority | Role |
|--------|----------|------|
| Display (main) | default | Bresenham gate, RSX blit, flip |
| Decode | 800 | TS demux, VDEC submit, jitter buffer fill |
| Upload | 850 | memcpy jitter buffer → RSX texture |
| Audio | 750 | DMA event loop, PCM ring drain |

## File Structure

```
jellyfin-ps3/
└── source/
    ├── main.cpp              # Entry point
    ├── player.cpp/h          # Media player — thread management, display loop, frame pacing
    ├── video.cpp/h           # VDEC init, H.264 decode, jitter buffer, FPS detection
    ├── audio.cpp/h           # Audio port, DMA ring buffer, audio thread
    ├── adec.cpp/h            # MP3 decode via minimp3, PCM ring buffer
    ├── stream.cpp/h          # HTTP MPEG-TS stream reader
    ├── http.cpp/h            # HTTP client
    ├── jellyfin_api.cpp/h    # Jellyfin REST API (auth, libraries, items)
    ├── ui.cpp/h              # XMB UI, font rendering, OSK, browse/search screens
    ├── bitmap.cpp/h          # Bitmap/image loading
    ├── timing.cpp/h          # Frame pacing, Bresenham accumulator, hardware vsync
    ├── rsxutil.cpp/h         # RSX GPU helpers, shader blit
    ├── plog.h                # Async logging
    ├── minimp3.h             # Embedded MP3 decoder
    ├── stb_truetype.h        # TTF font rasterizer
    ├── opensans_regular.h    # Open Sans Regular (embedded)
    ├── opensans_bold.h       # Open Sans Bold (embedded)
    ├── material_icons.h      # Material Icons (embedded)
    └── font8x8.xpm           # Fallback bitmap font
```

## Status

Work in progress. Library browsing, login, TV show browser, collections browser, and video playback are functional.

| Feature | Status |
|---------|--------|
| Login / Auth | ✅ Working |
| Movie browsing | ✅ Working |
| TV show browsing | ✅ Working |
| Collections browsing | ✅ Working |
| Search | ⚠️ In progress |
| Video playback | ✅ Working (24fps, hardware vsync, RSX GPU blit) |
| Audio playback | ✅ Working (48kHz stereo, zero silence blocks) |
| AV sync | ✅ Working (audio clock infrastructure in place) |
| Frame pacing | ⚠️ Working on PTS-driven scheduling to replace Bresenham accumulator |
| Jellyfin transcoding | ⚠️ PlaybackInfo session flow in progress |

## Known Limitations

- 24fps content on 60Hz displays has inherent 2:3 pulldown cadence — this will be addressed by PTS-driven scheduling
- Jellyfin PlaybackInfo POST returns 404 in some configurations, falling back to direct stream

## License

MIT
