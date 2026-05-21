[33mcommit 8d6bacea069dc05eddea27a444da882dd60dc65e[m
Author: Tuggs_Bunny <moe.mckeefry@gmail.com>
Date:   Sun May 17 20:50:10 2026 +1200

    Updated README

[1mdiff --git a/README.md b/README.md[m
[1mnew file mode 100644[m
[1mindex 0000000..64f23f9[m
[1m--- /dev/null[m
[1m+++ b/README.md[m
[36m@@ -0,0 +1,197 @@[m
[32m+[m[32m<p align="center">[m
[32m+[m[32m  <img src="ICON0.PNG" width="200"/>[m
[32m+[m[32m</p>[m
[32m+[m
[32m+[m[32m# JellyFin PS3 Client[m
[32m+[m
[32m+[m[32mA native PS3 homebrew Jellyfin client written in C++ using PSL1GHT, targeting Evilnat CFW or HEN.[m
[32m+[m
[32m+[m[32m## Features[m
[32m+[m
[32m+[m[32m- XMB-style main menu with animated wave background[m
[32m+[m[32m- Browse Movies, TV Shows, and Collections libraries[m
[32m+[m[32m- TV show browser — Series → Seasons → Episodes[m
[32m+[m[32m- Collections browser — Collection → Movies[m
[32m+[m[32m- Search across your entire Jellyfin library[m
[32m+[m[32m- Custom on-screen keyboard for login and search[m
[32m+[m[32m- Open Sans font rendering via stb_truetype[m
[32m+[m[32m- Material Icons tab icons[m
[32m+[m[32m- HTTP streaming of transcoded video via MPEG-TS[m
[32m+[m[32m- Hardware H.264 decode via PS3 VDEC (SPU-accelerated)[m
[32m+[m[32m- 24fps frame-paced display with Bresenham 2:3 pulldown and hardware vsync[m
[32m+[m[32m- MP3 audio decode via minimp3 with PCM ring buffer[m
[32m+[m[32m- Interleaved stereo DMA audio output at 48kHz[m
[32m+[m[32m- Double-buffered RSX GPU blit via vertex/fragment shaders[m
[32m+[m[32m- Async logging system[m
[32m+[m
[32m+[m[32m## Requirements[m
[32m+[m
[32m+[m[32m- PS3 with Evilnat CFW or HEN (CEX)[m
[32m+[m[32m- PSL1GHT toolchain (`ppu-g++` at `/usr/local/ps3dev/ppu/bin/`)[m
[32m+[m[32m- Jellyfin server accessible from your PS3 (local network recommended, port-forwarded remote server also works)[m
[32m+[m
[32m+[m[32m## Building[m
[32m+[m
[32m+[m[32m```bash[m
[32m+[m[32mmake clean && make[m
[32m+[m[32m```[m
[32m+[m
[32m+[m[32mOutput: `jellyfin-ps3.self`[m
[32m+[m
[32m+[m[32mTransfer to PS3 via FTP or USB and launch through webMAN or multiMAN.[m
[32m+[m
[32m+[m[32m## Controls[m
[32m+[m
[32m+[m[32m### Menus[m
[32m+[m[32m| Button | Action |[m
[32m+[m[32m|--------|--------|[m
[32m+[m[32m| X | Select |[m
[32m+[m[32m| O | Back |[m
[32m+[m[32m| D-pad | Navigate |[m
[32m+[m[32m| L1 / R1 | Cycle tabs |[m
[32m+[m[32m| Triangle | Info overlay |[m
[32m+[m
[32m+[m[32m### Media Player[m
[32m+[m[32m| Button | Action |[m
[32m+[m[32m|--------|--------|[m
[32m+[m[32m| Start | Exit player |[m
[32m+[m
[32m+[m[32m## Video Pipeline[m
[32m+[m
[32m+[m[32m```[m
[32m+[m[32mHTTP stream (MPEG-TS)[m
[32m+[m[32m        │[m
[32m+[m[32m        ▼[m
[32m+[m[32m  Decode thread[m
[32m+[m[32m  stream_read() → 188-byte TS packets[m
[32m+[m[32m        │[m
[32m+[m[32m        ▼[m
[32m+[m[32m  video_feed_ts()[m
[32m+[m[32m  TS demuxer → PAT/PMT parsing → PES reassembly[m
[32m+[m[32m        │[m
[32m+[m[32m        ├─ Audio PES → adec_push_pes() → minimp3 → PCM ring buffer[m
[32m+[m[32m        │[m
[32m+[m[32m        └─ Video PES → vdecDecodeAu() → VDEC (SPU-accelerated H.264)[m
[32m+[m[32m                │[m
[32m+[m[32m                ▼[m
[32m+[m[32m         VDEC callback[m
[32m+[m[32m         vdec_pull_frame() → YUV frame → ARGB conversion[m
[32m+[m[32m                │[m
[32m+[m[32m                ▼[m
[32m+[m[32m         Jitter buffer (16 slots, ~32 MB)[m
[32m+[m[32m         PTS stored per slot[m
[32m+[m[32m                │[m
[32m+[m[32m                ▼[m
[32m+[m[32m         Upload thread (priority 850)[m
[32m+[m[32m         memcpy → RSX-local texture (double-buffered)[m
[32m+[m[32m                │[m
[32m+[m[32m                ▼[m
[32m+[m[32m         Display thread[m
[32m+[m[32m         timing_flip_due() ← Bresenham accumulator (24000/1001 fps)[m
[32m+[m[32m         gcmSetVBlankHandler ← hardware vsync at 60000/1001 Hz[m
[32m+[m[32m                │[m
[32m+[m[32m                ▼[m
[32m+[m[32m         RSX GPU blit[m
[32m+[m[32m         vertex + fragment shaders → fullscreen quad[m
[32m+[m[32m         rsxSync() → flip()[m
[32m+[m[32m```[m
[32m+[m
[32m+[m[32m**FPS detection:** VDEC `frame_rate_code` maps to exact fractional fps (ISO 13818-2 table). Display refresh rate queried dynamically via `videoGetState` — 59.94Hz detected and used in Bresenham accumulator for correct 2:3 pulldown cadence.[m
[32m+[m
[32m+[m[32m**Frame pacing:** Bresenham accumulator fires `s_flip_trigger` at the exact vblank edge where a new frame is due. Display thread consumes the trigger via `timing_flip_due()`. `flip_late` logged if the trigger is consumed more than one vblank late.[m
[32m+[m
[32m+[m[32m**GPU blit:** RSX vertex/fragment shaders scale the YUV→ARGB texture to fit the display. `rsxSync()` before `flip()` ensures the RSX finishes all blit work before the framebuffer is presented.[m
[32m+[m
[32m+[m[32m## Audio Pipeline[m
[32m+[m
[32m+[m[32m```[m
[32m+[m[32mAudio PES packets (MP3, type 0x03)[m
[32m+[m[32m        │[m
[32m+[m[32m        ▼[m
[32m+[m[32m  adec_push_pes()[m
[32m+[m[32m  PES header stripped (9 + buf[8] bytes)[m
[32m+[m[32m        │[m
[32m+[m[32m        ▼[m
[32m+[m[32m  minimp3 decode loop[m
[32m+[m[32m  mp3dec_decode_frame() → 1152 samples per frame[m
[32m+[m[32m  short PCM → float32, interleaved L/R pairs[m
[32m+[m[32m        │[m
[32m+[m[32m        ▼[m
[32m+[m[32m  PCM ring buffer (8192 sample-pairs, ~170ms at 48kHz)[m
[32m+[m[32m        │[m
[32m+[m[32m        ▼[m
[32m+[m[32m  Audio thread (priority 750)[m
[32m+[m[32m  sysEventQueueReceive() → DMA event[m
[32m+[m[32m  Blocks up to 100ms waiting for 256 samples[m
[32m+[m[32m        │[m
[32m+[m[32m        ▼[m
[32m+[m[32m  PS3 audio DMA (8 blocks, 256 samples each, 48kHz)[m
[32m+[m[32m  Interleaved layout: L0 R0 L1 R1 ... (per SDK spec)[m
[32m+[m[32m```[m
[32m+[m
[32m+[m[32m**Sample rate:** PS3 audio hardware runs at 48kHz. Stream is requested at 48kHz via `AudioSampleRate=48000` in the stream URL.[m
[32m+[m
[32m+[m[32m**DMA layout:** PS3 audio expects interleaved samples (L R L R). Each DMA block is 256 sample-pairs written as sequential float32 pairs. Audio thread blocks until a full 256-sample block is available before writing, preventing silence gaps that would corrupt pitch.[m
[32m+[m
[32m+[m[32m**Audio clock:** `audio_get_clock_us()` returns `(s_audio_blocks * 256ULL * 1000000ULL) / 48000ULL` — microseconds of audio played since start. Used as AV sync reference. PTS stored per jitter buffer slot via `s_jbuf_pts[]`.[m
[32m+[m
[32m+[m[32m## Threading Model[m
[32m+[m
[32m+[m[32m| Thread | Priority | Role |[m
[32m+[m[32m|--------|----------|------|[m
[32m+[m[32m| Display (main) | default | Bresenham gate, RSX blit, flip |[m
[32m+[m[32m| Decode | 800 | TS demux, VDEC submit, jitter buffer fill |[m
[32m+[m[32m| Upload | 850 | memcpy jitter buffer → RSX texture |[m
[32m+[m[32m| Audio | 750 | DMA event loop, PCM ring drain |[m
[32m+[m
[32m+[m[32m## File Structure[m
[32m+[m
[32m+[m[32m```[m
[32m+[m[32mjellyfin-ps3/[m
[32m+[m[32m└── source/[m
[32m+[m[32m    ├── main.cpp              # Entry point[m
[32m+[m[32m    ├── player.cpp/h          # Media player — thread management, display loop, frame pacing[m
[32m+[m[32m    ├── video.cpp/h           # VDEC init, H.264 decode, jitter buffer, FPS detection[m
[32m+[m[32m    ├── audio.cpp/h           # Audio port, DMA ring buffer, audio thread[m
[32m+[m[32m    ├── adec.cpp/h            # MP3 decode via minimp3, PCM ring buffer[m
[32m+[m[32m    ├── stream.cpp/h          # HTTP MPEG-TS stream reader[m
[32m+[m[32m    ├── http.cpp/h            # HTTP client[m
[32m+[m[32m    ├── jellyfin_api.cpp/h    # Jellyfin REST API (auth, libraries, items)[m
[32m+[m[32m    ├── ui.cpp/h              # XMB UI, font rendering, OSK, browse/search screens[m
[32m+[m[32m    ├── bitmap.cpp/h          # Bitmap/image loading[m
[32m+[m[32m    ├── timing.cpp/h          # Frame pacing, Bresenham accumulator, hardware vsync[m
[32m+[m[32m    ├── rsxutil.cpp/h         # RSX GPU helpers, shader blit[m
[32m+[m[32m    ├── plog.h                # Async logging[m
[32m+[m[32m    ├── minimp3.h             # Embedded MP3 decoder[m
[32m+[m[32m    ├── stb_truetype.h        # TTF font rasterizer[m
[32m+[m[32m    ├── opensans_regular.h    # Open Sans Regular (embedded)[m
[32m+[m[32m    ├── opensans_bold.h       # Open Sans Bold (embedded)[m
[32m+[m[32m    ├── material_icons.h      # Material Icons (embedded)[m
[32m+[m[32m    └── font8x8.xpm           # Fallback bitmap font[m
[32m+[m[32m```[m
[32m+[m
[32m+[m[32m## Status[m
[32m+[m
[32m+[m[32mWork in progress. Library browsing, login, TV show browser, collections browser, and video playback are functional.[m
[32m+[m
[32m+[m[32m| Feature | Status |[m
[32m+[m[32m|---------|--------|[m
[32m+[m[32m| Login / Auth | ✅ Working |[m
[32m+[m[32m| Movie browsing | ✅ Working |[m
[32m+[m[32m| TV show browsing | ✅ Working |[m
[32m+[m[32m| Collections browsing | ✅ Working |[m
[32m+[m[32m| Search | ⚠️ In progress |[m
[32m+[m[32m| Video playback | ✅ Working (24fps, hardware vsync, RSX GPU blit) |[m
[32m+[m[32m| Audio playback | ✅ Working (48kHz stereo, zero silence blocks) |[m
[32m+[m[32m| AV sync | ✅ Working (audio clock infrastructure in place) |[m
[32m+[m[32m| Frame pacing | ⚠️ Working on PTS-driven scheduling to replace Bresenham accumulator |[m
[32m+[m[32m| Jellyfin transcoding | ⚠️ PlaybackInfo session flow in progress |[m
[32m+[m
[32m+[m[32m## Known Limitations[m
[32m+[m
[32m+[m[32m- 24fps content on 60Hz displays has inherent 2:3 pulldown cadence — this will be addressed by PTS-driven scheduling[m
[32m+[m[32m- Jellyfin PlaybackInfo POST returns 404 in some configurations, falling back to direct stream[m
[32m+[m
[32m+[m[32m## License[m
[32m+[m
[32m+[m[32mMIT[m
