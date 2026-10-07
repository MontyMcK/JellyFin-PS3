# Handoff: build and deploy the experience pass (for the local Claude)

## Task

Build and deploy `feature/xmb-spine` at commit `9bb092f` to the PS3. It was
written and host-tested in a cloud session. That session had no PS3 toolchain
and no network path to the console, so **it has never been compiled for the
PPU or run on hardware.**

Also read `SPINE-PLAN.md`. The last two sections, both dated 2026-09-24, cover
everything in this handoff.

## 1. Get the code

```bash
cd ~/jf-spine
git fetch origin
git checkout feature/xmb-spine
git pull --ff-only origin feature/xmb-spine
git log --oneline -1          # 9bb092f ui: hardware feedback round ...
```

`claude/kind-planck-uluzgt` is identical, and either branch will do.

The worktree may have uncommitted local changes. If `git pull` refuses, stop
and ask the user; do not discard them.

## 2. Clean serial build

```bash
make clean && make -j1 2>&1 | tee build.log
grep -c "warning:" build.log   # baseline census is 48
grep -E "error|warning:" build.log | grep -E "ui_art|ui_buffering|ui_ambient|ui_peek|json_unescape|peek\.h|art_colour|buffer_anim|experience\.h|jf_logo_geom|thumbnail_cache|hud_v3|facts_parse|ui_wave_panels"
make pkg                       # if deploying by package
```

**New translation units.** These are picked up by the Makefile's wildcard:

- `source/ui/render/ui_art.cpp`
- `source/ui/render/ui_buffering.cpp`
- `source/ui/xmb/ui_ambient.cpp`
- `source/ui/xmb/ui_peek.cpp`

**New headers:**

- `render/art_colour.h`
- `render/buffer_anim.h`
- `render/experience.h`
- `render/peek.h`
- `render/jf_logo_geom.h`
- `api/json_unescape.h`

**Likely compile issues.** These were checked only with host `g++
-fsyntax-only` against the PSL1GHT headers, so the PPU compiler may still
complain. Fix them minimally:

- missing includes;
- `-Wall` warnings in the new files (the goal is no new warnings);
- `slog_state` needs `slog.h` in `ui_peek.cpp` (it is included);
- `sinf`/`cosf` in `ui_wave_panels.cpp` (`math.h` is included);
- `BUILD_FOR_RPCS3` reaches `ui_buffering.cpp` through `ui_visuals.h`.

Host tests, which should all pass:

```bash
cd tests
for t in test_experience test_json_unescape test_facts test_theme test_spine test_depth test_layout test_wave_gel test_wave_light test_media_sources; do
  make -s -f Makefile.host -B $t && ./$t | tail -1
done
```

`make check` itself cannot pass: `test_bg_gradient.c` and `test_month_bg.c`
were never committed, and the decode tests need ffmpeg tone files.

## 3. Deploy

Deploy the way earlier spine builds were deployed:

- Stage `EBOOT.BIN` as `outputs/EBOOT.BIN.spine9`, and keep the current one
  as a rollback.
- FTP it to the console.
- `/dev_hdd0/tmp/jellyfin_spine.txt` must contain `1`.

Optional files, all in `/dev_hdd0/tmp/`:

| file | contents | effect |
| --- | --- | --- |
| `jellyfin_ambient.txt` | minutes, 0 = off | screensaver idle time; default 3 |
| `jellyfin_jwspeed.txt` | percent | JellyWave speed; the default is now 20 |
| `jellyfin_jwrebuild.txt` | 1–8 | JellyWave rebuild cadence; default 3; try 2 if motion steps |

**Rollback:** set the spine gate to 0, or restore the previous EBOOT.

Record the sha and the warning count in SPINE-PLAN.md, the same way spine7 and
spine8 were recorded.

## 4. What changed (so you know what to look at on the TV)

All of it is behind the spine gate. With the gate off, the strobe line
behaves exactly as before.

1. **Buffering screen** (`ui_buffering.cpp`, `player.cpp`):
   - the detail backdrop, darkened;
   - the vector Jellyfin mark with a thin ring in Jellyfin purple → blue;
   - BUFFERING and a percentage;
   - on ready, the ring closes, the mark pulses once and the screen fades.
2. **Artwork colour cache** (`art_colour.h`, `ui_art.cpp`). It drives the
   buffering glow, the music halo and the screensaver glow.
3. **Music screen:** the album accent tints the bars, the artist line and the
   seek bar. A halo and a floor wash breathe with the existing FFT bands.
4. **Screensaver** (`ui_ambient.cpp`). After 3 idle minutes on the XMB the UI
   dissolves and Home's cached posters drift across the screen. Any press
   dissolves it back, and that press does nothing else.
5. **Version picker:** RECOMMENDED shows source[0] with its audio words, then
   OTHER VERSIONS.
6. **JellyWave** (parameters only; the strobe-critical code is untouched):
   - speed 20;
   - 0.55× perturbation;
   - layer opacity 150/120/90;
   - neon cyan/violet rim and sheen instead of white.
7. **Player HUD:**
   - the top-right AUDIO pill is removed;
   - the audio chip names the track;
   - the chips are restyled as glass, with a volume meter and a subtitles dot.
8. **JSON escapes decoded** (`json_unescape.h`). This fixes `'` and
   similar escapes showing up in synopses and codec text.
9. **Golden Age** uses the Jellyfin lockup colours.
10. **Detail cast:** 2:3 portrait cards instead of circles.
11. **Triangle quick-peek** on the Movies and TV grids (`peek.h`,
    `ui_peek.cpp`):
    - the poster flips into a 4:3 panel with the synopsis and cast;
    - X opens detail, and Triangle, O or the d-pad closes it;
    - the data comes from the facts worker, which now also fetches
      Overview, People and OfficialRating.
12. **Home posters broken after playing an album:**
    - the thumbnail VRAM mirror is now sized for Home's square row (it
      overflowed by 256 bytes at 720p);
    - each slot gets integrity stamps;
    - `thumb_cache_verify_and_flush("music")` runs on leaving the music
      screen.

## 5. Hardware checks, in priority order

1. The app boots, the XMB draws, and there is no crash or strobe. Watch the
   `xmb:` cost line: `bpx` must not move against spine8.
2. **Playback:**
   - the buffering screen appears and animates;
   - Circle still means Start now;
   - the first video frame follows cleanly;
   - an error or cancel fades into the error screen;
   - the `preroll:` timings do not get worse.
3. **JellyWave:** smooth rather than stepped, more translucent, glowing rather
   than shiny.
4. **Text:** apostrophes and codec text ("DTS-HD MA") in Continue Watching and
   in synopses.
5. **Triangle peek:**
   - the flip is smooth;
   - O closes the peek and does not leave the tab;
   - X opens detail.
6. **Album, then back, then Home:** posters load. Report the log line
   `thumb: verify+flush after music: ready=N damaged px=A vram=B`. Non-zero
   `px` or `vram` confirms corruption.
7. **Display modes:** the HUD chips, the cast cards, Golden Age, and the
   screensaver after 3 minutes.

## Rules carried from the project

- Do not modify the JellyWave renderer's vertex buffers, `jw_upload()`, RSX
  ownership or synchronisation, topology, or the strobe fixes.
- Do not touch the audio DSP branch or the 24p work.
- Do not deploy without the user's go-ahead. They asked for this build and
  deploy on 2026-09-24.
