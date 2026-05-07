# JellyFin PS3 Client

<p align="center">
  <img src="ICON0.PNG" width="300"/>
</p>

A native PS3 homebrew Jellyfin client written in C++ using PSL1GHT, targeting Evilnat CFW.

## Features

- Browse Jellyfin libraries directly from your PS3
- Custom on-screen keyboard for login
- HTTP streaming of transcoded video via MPEG-TS
- Hardware H.264 decode via PS3 VDEC (SPU-accelerated)
- Dedicated network, decode, and audio threads
- MP3 audio decode via ADEC
- 30fps frame-paced display with jitter buffer

## Requirements

- PS3 with Evilnat CFW or HEN (CEX)
- PSL1GHT toolchain (`ppu-g++` at `/usr/local/ps3dev/ppu/bin/`)
- Jellyfin server (accessible on local network recomended)

## Building

```bash
make clean && make
```

Output: `jellyfin-ps3.self`

Transfer to PS3 via FTP or USB and launch through webMAN or multiMAN.

## Controls

| Button | Action |
|--------|--------|
| X | Select |
| O | Back |
| Start | Exit media player |

## File Structure

```
jellyfin-ps3/
└── source/
    ├── main.cpp            # Entry point
    ├── player.cpp/h        # Media player — thread management, display loop, frame pacing
    ├── video.cpp/h         # VDEC init, H.264 decode, jitter buffer, FPS detection
    ├── audio.cpp/h         # Audio port, DMA ring buffer, audio thread
    ├── adec.cpp/h          # MP3 decode via PS3 ADEC
    ├── stream.cpp/h        # HTTP MPEG-TS stream reader
    ├── http.cpp/h          # HTTP client
    ├── jellyfin_api.cpp/h  # Jellyfin REST API (auth, libraries, items)
    ├── ui.cpp/h            # UI rendering, on-screen keyboard
    ├── bitmap.cpp/h        # Bitmap/image loading
    ├── timing.cpp/h        # Frame pacing and timing
    ├── rsxutil.cpp/h       # RSX GPU helpers
    ├── plog.h              # Logging
    ├── minimp3.h           # Embedded MP3 decoder
    └── font8x8.xpm         # Bitmap font
```

## Status

Work in progress. library browsing, and login are functional. Audio playback and Video playback active development.

## License

MIT
