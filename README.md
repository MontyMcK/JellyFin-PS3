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
- 30fps frame-paced display with jitter buffer
- MP3 audio decode via ADEC
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

## File Structure

```
jellyfin-ps3/
└── source/
    ├── main.cpp              # Entry point
    ├── player.cpp/h          # Media player — thread management, display loop, frame pacing
    ├── video.cpp/h           # VDEC init, H.264 decode, jitter buffer, FPS detection
    ├── audio.cpp/h           # Audio port, DMA ring buffer, audio thread
    ├── adec.cpp/h            # MP3 decode via PS3 ADEC
    ├── stream.cpp/h          # HTTP MPEG-TS stream reader
    ├── http.cpp/h            # HTTP client
    ├── jellyfin_api.cpp/h    # Jellyfin REST API (auth, libraries, items)
    ├── ui.cpp/h              # XMB UI, font rendering, OSK, browse/search screens
    ├── bitmap.cpp/h          # Bitmap/image loading
    ├── timing.cpp/h          # Frame pacing and timing
    ├── rsxutil.cpp/h         # RSX GPU helpers
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
| Video playback | ✅ Working (30fps, no corruption) |
| Audio playback | ❌ In development |
| Frame pacing | ⚠️ Minor judder |

## License

MIT
