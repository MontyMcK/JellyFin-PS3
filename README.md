# Jellyfin PS3

A lightweight Jellyfin client for the PlayStation 3, written in C/C++ using PSL1GHT. Browse your Jellyfin media library and play content directly from your PS3.

---

## Features

- Log in to any Jellyfin server with username and password
- Browse your media libraries
- Search for content
- Play video using the PS3's hardware video decoder
- Config is saved to HDD so you don't have to re-enter your server/credentials on every launch
- Crash log written to `/dev_hdd0/tmp/crash_log.txt` for debugging

---

## Requirements

### To build
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT) SDK installed and `PSL1GHT` environment variable set
- PS3 toolchain (`ppu-lv2-g++`, etc.)

### To run
- A PlayStation 3 with custom firmware (CFW) or HEN
- A running [Jellyfin](https://jellyfin.org/) server accessible on your local network

---

## Building

```bash
export PSL1GHT=/path/to/PSL1GHT
make
```

This produces:
- `jellyfin-ps3.elf` — raw ELF binary
- `jellyfin-ps3.self` — signed SELF for running on PS3

To clean build artefacts:
```bash
make clean
```

To send directly to a PS3 running ps3load:
```bash
make run
```

---

## Installation

1. Copy `jellyfin-ps3.self` to your PS3 via FTP or USB
2. Launch it through your CFW file manager or homebrew launcher

---

## Usage

1. On first launch you'll be prompted to enter your Jellyfin server URL (e.g. `http://192.168.1.100:8096`)
2. Enter your username and password
3. Use the D-pad to navigate, **Cross** to select, **Circle** to go back

Your server address, token, and user ID are saved automatically after login.

---

## Controls

| Button | Action |
|--------|--------|
| D-pad | Navigate menus |
| Cross (X) | Select / Confirm |
| Circle (O) | Back / Cancel |
| Start | — |
| Select | — |

---

## Project Structure

```
jellyfin-ps3/
├── source/
│   ├── main.cpp          # Entry point, init and main loop
│   ├── jellyfin_api.cpp  # Jellyfin REST API calls, login, browsing
│   ├── jellyfin_api.h
│   ├── http.cpp          # HTTP client (PS3 net library wrapper)
│   ├── http.h
│   ├── ui.cpp            # Rendering, input handling, menus
│   ├── ui.h
│   ├── player.cpp        # Video playback via PS3 vdec
│   ├── player.h
│   ├── bitmap.cpp        # Bitmap/image rendering
│   ├── bitmap.h
│   ├── rsxutil.cpp       # RSX GPU init and screen flip
│   ├── rsxutil.h
│   └── font8x8.xpm       # Embedded bitmap font
├── build/                # Compiled object files (generated)
├── Makefile
├── jellyfin-ps3.elf
└── jellyfin-ps3.self
```

---

## Debugging

If the app crashes, check `/dev_hdd0/tmp/crash_log.txt` on your PS3's HDD. The log records each major startup step so you can tell exactly where it failed.

---

## Limitations

- Video format support is limited to what the PS3 hardware decoder supports (H.264, MPEG-2, etc.)
- No thumbnail/artwork display
- Item list capped at 100 items per page
- HTTPS servers may have certificate issues depending on firmware

---

## License

No license specified. All rights reserved by the author unless stated otherwise.
