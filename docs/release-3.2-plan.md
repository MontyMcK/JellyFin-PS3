# JellyFin-PS3 3.2 — implementation plan

Written 2026-10-03 for a Sonnet session to implement. Scope decided with the
user: **fixes + pending merges, Dolby Digital passthrough as a real menu option,
a reorganised Settings screen, offline downloads (merged and finished),
Favourites, whole-season download, Live TV / IPTV, USB / external drive
playback (NTFS, exFAT, FAT32; MKV, m2ts, ts; FLAC/MP3/WAV), a hard 10 GB
free-space floor for downloads, and the ui_wave statics refactor.**

3.2 is now a big release. Every item below is independently shippable.
The order in §1 is chosen so each step lands on a stable base, and so the one
truly risky merge (offline) happens early, while there is time to TV-test it.

Base: `origin/main` = `32219a5` (3.1 + README). Work branch: `release/3.2` in
the WSL worktree `~/jf-r32` (created from `origin/main` by the planning
session; this file is committed there as `docs/release-3.2-plan.md`).

---

## 0. Ground rules (read before touching anything)

These come from incidents on this project. Each one has cost a day before.

1. **Build in WSL, from a clean export, never from a shared tree.**
   ```bash
   wsl -e bash -c 'cd ~/jf-r32 && rm -rf ~/jf-r32-build && mkdir ~/jf-r32-build && git archive HEAD | tar -x -C ~/jf-r32-build && cd ~/jf-r32-build && export PS3DEV=/root/ps3dev PSL1GHT=/root/ps3dev PATH=/root/ps3dev/bin:/root/ps3dev/ppu/bin:$PATH && make pkg 2>&1 | tail -20'
   ```
   Commit first, then build — the archive only contains committed work. Other
   sessions' uncommitted files have been compiled into builds before.
   Use `wsl -e bash -c '...'` from the Bash tool for one-liners; for anything
   with backslashes, pipe a script through stdin, and never put anything
   load-bearing on line 1 of that script (a BOM eats it).
2. **The only EBOOT that boots** from `/dev_hdd0/game` is
   `obj/pkg/USRDIR/EBOOT.BIN` (from `make pkg`). Not `*.self`, not
   `*.fake.self`. "An error occurred during the start operation" = wrong file.
3. **Host tests:** `make -B -f tests/Makefile.host all && make -f tests/Makefile.host check`.
   Without `-B` a stale binary can report green. Grep the log for
   `undefined|error:|\*\*\*` rather than trusting the exit code.
4. **Compile-check a single PS3 file** without a full build (catches what host
   stubs can't):
   ```bash
   ppu-g++ -c -O2 -Wall -mcpu=cell -I/root/ps3dev/ppu/include \
     $(for d in source source/ui source/ui/render source/gfx source/util; do echo -n " -I$d"; done) \
     -o /tmp/x.o source/ui/render/ui_wave.cpp
   ```
5. **Deploying** (only when the user asks to test on the TV):
   `powershell -File JellyFin-PS3\tools\ps3push\ftp.ps1 -Local <EBOOT.BIN>`
   (console `192.168.0.202`). The script prints a "before" sha — the console
   is shared with other sessions; if "before" isn't the build you last pushed,
   say so before overwriting. Run `ListAgents` first.
6. **Never commit `data/jf_spu_kernel.bin`** — every build rewrites it.
   `git checkout data/jf_spu_kernel.bin` before `git add`.
7. **Comments state rules, not history.** No dates, no "long shot", no
   "hardware, 2026-09-29", no quoted TV verdicts, no "used to". Incidents and
   measurements go in `docs/*-notes.md`. Never write "identical",
   "bit-exact" or "pixel-perfect" unless a test asserts it. Several commits
   being ported below violate this — clean them as you port.
8. **Line endings:** the repo is LF. Before every commit run
   `git diff --cached --stat` and check no file shows a whole-file rewrite.
9. **Don't push, tag or publish a GitHub release** without the user saying so.
10. Commit messages end with
    `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` (or whichever
    model is implementing).

---

## 1. Work items at a glance

| # | Item | Source | Size | Risk | TV test? |
|---|------|--------|------|------|----------|
| A1 | Paused-player / PS-button overlay freeze | cherry-pick `aaf4382` | XS | low | yes (quick) |
| A2 | Chunked-body decode fix (empty details page + hung retry) | uncommitted in `~/jf-bits` | XS | low | no |
| A3 | Send Log to Server + logging on by default | `8ddb523` (CRLF-normalise) | S | low | yes |
| B1 | Dolby Digital passthrough as an Audio Output value | port from `feature/bitstream` minus experiments | M | med | **yes** |
| B2 | Bitstream bug fixes: reassert channel, false "LPCM intact" log | new | S | low | with B1 |
| C1 | Settings: table-driven model + host test | new | M | med | — |
| C2 | Settings: sections, headers, L2/R2 section jump, Left/Right value step | new | M | med | yes |
| D1 | ui_wave.cpp statics → state structs | new, separate branch | L (mechanical) | **high** | **yes, long** |
| E1 | Offline downloads: merge `origin/feature/offline-downloads` onto main | merge, 11 conflicts | L | **high** | **yes** |
| E2 | Offline: spine details-page button, Downloads section in Settings, list posters | new | M | med | yes |
| E3 | **HDD safety: 10 GB always kept free, fail closed** | new | S | low | yes (quick) |
| U1 | USB: port mohasi NTFS + exFAT readers (read-only) | vendor + port | M | **high** | yes |
| U2 | USB: `lfs` read-only file layer, hotplug | new | M | med | yes |
| U3 | USB: .ts/.m2ts, **MKV demuxer**, local track choice + subtitles | new | **L** | **high** | **yes** |
| U4 | USB: local music (FLAC/MP3/WAV, quality resampler, albums) | new | M | med | yes |
| U5 | USB: "Media" tab, browser, file details, local resume | new | M | med | yes |
| F1 | Favourites: star toggle on details, Favourites row on Home | new | S | low | yes |
| G1 | Download whole season | new (needs E) | S–M | med | yes |
| H1 | Live TV: API + parsing + host tests | new | M | low | — |
| H2 | Live TV: tab, channel list (now/next), guide grid | new | L | med | yes |
| H3 | Live TV: playback (live mode in the player, channel up/down, close stream) | new | M | **high** | **yes** |
| R  | Release 3.2 (version, changelog, fresh-install test, pkg) | — | S | — | yes |

**Order:** A → **E1 + E3** → B → C (with E2's Settings rows) → E2 → F1 → G1 →
H1 → H2 → H3 → U1 → U2 → U3 → U4 → U5 → R.

E3 lands **with** E1, in the same TV build. No build that can download may
ever reach the console without the 10 GB floor.

U1's host-side port and tests have no dependencies. They can run in parallel
from the start, on a branch `feature/usb-fs`, merged when U2 begins.

- E1 goes **before** B because offline moves the stream-URL decision out of
  `build_stream_url()` into `player/stream/stream_request.cpp`. Do that move
  once, on main's current code, then add Dolby Digital to the new home.
  Doing B first would mean resolving the Dolby Digital URL changes inside the
  hardest conflict.
- F1 goes before H because channel favourites reuse the same API helper.
- **Ask the user for a TV session after E1, after C/E2/F1/G1, and after H3.**
  Don't stack everything into one untested build.

D1 runs on its own branch in parallel and only merges if it passes a TV soak
before release day. **3.2 ships without D1 rather than waiting for it**: it
has no user-visible value.

---

## A. Pending fixes

### A1. Paused player freezes when the PS button is pressed

**Bug:** paused + idle, the player loop stops flipping. The system draws its
PS-button overlay only on the app's flips, so the overlay was invisible but
ate every button — the player looked frozen.

**Fix (exists):** `git cherry-pick aaf4382` (on `feature/bitstream`; the same
change as `9ad37d5` on `fix/paused-overlay` — take `aaf4382`, it's the clean
one). It adds `volatile u32 g_sys_overlay` in `main.cpp`, set on
`SYSUTIL_DRAW_BEGIN`, cleared on `SYSUTIL_DRAW_END`, and the paused-idle gate in
`player.cpp` skips while it's set.

**Clean while porting:** the comment in `player.cpp` says
"(hardware, 2026-09-29)" — rewrite as a rule: *"Keep flipping while the system
overlay is up: it is drawn on our flips, so idling here would hide it while it
still takes input."*

**Also check (5 min):** grep for other idle loops that stop flipping
(`usleep` inside loops that skip `flip()` / `waitflip`): music screen paused,
buffering screen, error screen. Apply the same `!g_sys_overlay` guard to any
that gate their flip. List what you checked in the commit message.

Do **not** take anything else from `fix/paused-overlay` — the reassert modes
(`d0e8b28`…`27f760e`) were a disproven theory.

### A2. Chunked body that isn't chunk-framed

Uncommitted in `~/jf-bits/source/net/http.cpp` (`dechunk()`): if a response
says `Transfer-Encoding: chunked` but the body doesn't start with a hex chunk
size, keep the body as-is instead of decoding it to 0 bytes. Seen on an item
reply: 85 KB discarded, the details page empty, then a retry that hung.

Port it to `release/3.2` with the comment rewritten (no date):
```c
// A body announced as chunked that does not start with a chunk-size line is
// not chunk-framed; decoding it would discard it. Keep it as it is.
if (len > 0 && !isxdigit((unsigned char)body[0])) return len;
```
Make sure `<ctype.h>` is included. If `tests/test_invariant_http.cpp` can reach
`dechunk` (it's `static` today), add a case; if it can't, don't widen the
linkage just for this.

**Discard** the other uncommitted change in `~/jf-bits` (`surround.cpp`,
withdrawing Bitstream from the menu) — superseded by B1. Don't modify
`~/jf-bits` itself; copy the hunk.

### A3. Send Log to Server + logging on by default

Commit `8ddb523` on `feature/tester-diagnostics` (worktree `~/jf-v32`).
Contents are good; the problem is that it rewrote `source/ui/xmb/ui_nav.cpp`
with CRLF line endings (1607-line diff, only 5 real lines).

Port:
```bash
git cherry-pick -n 8ddb523
git show 8ddb523 -- source/ui/xmb/ui_nav.cpp | ...   # or simply:
git checkout HEAD -- source/ui/xmb/ui_nav.cpp
# re-apply the 4-line change by hand (row 15 -> Send Log action, stats -> row 16)
git diff --cached --stat   # ui_nav.cpp must show ~5 lines, not 1600
```
Note that C1 replaces the row-index code anyway — if you're doing C1
straight after, it's fine to port only the non-UI parts here
(`log_upload.cpp/.h`, `http_post_text`, the plog default) and wire the row in C1.

**Checks:**
- `grep -rn "api_key\|ApiKey\|Token=" source/` and confirm no plog line can
  contain the access token (today the token goes in the auth header and the
  stream URL has none — keep it that way; the uploaded log ends up on GitHub
  issues).
- The upload thread is `THREAD_JOINABLE` and joined before restarting — good.
  (On this SDK flag 0 means *detached*; don't "simplify" it to 0.)
- Help text currently uses `--`; use an en dash or reword.

---

## B. Dolby Digital passthrough

### Background (why this is a menu option)

The user's chain is PS3 → TV → soundbar over ARC/eARC. On **plain ARC** only
2-channel PCM or Dolby Digital gets through, so multichannel LPCM loses the
centre (a speaker-map test proved only L/R arrive). On eARC, LPCM 5.1 works.
Plain-ARC soundbars are very common, so **sending the film's Dolby Digital
track untouched** (IEC 61937 bursts over a 2-channel output) is the only way
those users get 5.1 with a centre. Proven on hardware: configure
`encoder=AUDIO_OUT_CODING_BITSTREAM (255)`, `channel=2`, rc=0, soundbar shows
DOLBY AUDIO, centre works — even with the DD/LPCM boxes unticked in the PS3's
sound settings. `audioOutGetState().soundMode.type` is **unreliable** (says 0
while the wire is DD) — never branch on it.

What doesn't work and must **not** ship: E-AC-3 passthrough (soundbar silent),
TrueHD bitstream (needs HBR, not reachable from cellAudio), DTS (soundbar has
no decoder; the configure is rejected).

### B1. Port

Source commits on `feature/bitstream` (base = main):

| commit | take? |
|---|---|
| `aaf4382` pause fix | already taken in A1 |
| `e93b815` AC-3 passthrough core (iec61937.c/h, adec_ac3 packing, audio.cpp no-gain path, file modes 6/7) | **yes** |
| `6a0a912` Bitstream surround mode, AC-3 track stream-copy, `track_label_is_ac3` | **yes**, minus the `hdprobe` part |
| `28858e7` hdprobe restores config | no (drop hdprobe entirely) |
| `c3d9cb3`, `a27b448` E-AC-3 | **no** |
| `bca8ae8` chantest | **no** |

Recommended mechanics: `git cherry-pick -n e93b815 6a0a912`, then strip, then
commit as one or two clean commits. Then `git diff origin/main` and check for
any surviving `eac3`, `chantest`, `hdprobe`, `jellyfin_eac3pt` — there must be
none. `tests/test_iec61937.c` + its Makefile.host entry come with `e93b815`;
keep them. Its "E-AC-3 header is rejected by `iec61937_pack_ac3`" case
stays — that's exactly the guard we want; drop only cases that exercise the
removed E-AC-3 *packing* path, if any.

**User-facing shape:**
- Setting file stays `jellyfin_surround.txt`, value `4` = Dolby Digital
  (`SURROUND_BITSTREAM` keeps its number, so nothing saved shifts).
- Audio Output cycle: `Stereo → 5.1 → [7.1] → Dolby Digital → Stereo`.
  Label **"Dolby Digital"** (not "Bitstream"). Update `surround.h` comments to
  match (the header still says "three states" and talks about the menu as
  "Off / AC-3 / HD" history — rewrite the header comment as current rules).
- `bitstream_mode()` maps `SURROUND_BITSTREAM` → `BITSTREAM_PASSTHROUGH_FORCE`
  (the proven semantics: send bursts even though GetState claims LPCM).
  `jellyfin_bitstream.txt` research values still override, as on main.
- Stream URL: **after E1 this decision lives in
  `stream_request_resolve()` / `stream_url_build()`** (`player/stream/stream_request.cpp`),
  not in `build_stream_url()`. Add `bool passthrough` to `StreamPrefs`
  (set in `stream_prefs_current()` from `audio_passthrough_wanted()`) and an
  `ac3_copy` flag to `StreamRequest`, so downloads and playback agree. A
  download made in Dolby Digital mode then carries the AC-3 track, which is
  what that user's soundbar wants. Add `test_offline`/`test_stream_request`
  cases for it. The rules themselves: DD tracks →
  `AudioCodec=ac3&AudioSampleRate=48000&MaxAudioChannels=6`, `copy_audio=true`,
  no `AudioBitrate` (a ceiling below the track rate demotes a copy to a
  transcode). Every other track → the existing AC-3 640 kbps transcode request.
  Never a TrueHD/DTS copy in this mode.
- `show_player`: passthrough opens a **2-channel** port.
- `adec_set_codec`: if passthrough is active and the codec isn't AC-3, end
  the bitstream config and decode to stereo LPCM (with one plog line).
- Volume: no gain on IEC bursts (`audio.cpp` skips `apply_volume`). **New:**
  the player's volume slider must not pretend to work — when
  `audio_passthrough_active()`, the slider either doesn't open or shows
  "Volume is set on your soundbar/receiver" (pick whichever is fewer lines in
  the HUD code; check both legacy and spine HUDs).
- Dialogue Boost has no effect in passthrough (nothing is decoded). In
  Settings, draw its value faint as `n/a` while Audio Output is Dolby Digital;
  its help text gains: "Not available with Dolby Digital passthrough."
- Music player is unaffected (stereo by design) — verify `music_screen` never
  calls `audio_passthrough_request(true)`.
- `audio_close()` calls `audio_passthrough_request(false)` and
  `audio_bitstream_end()` so the XMB/next app never inherits encoder 255.

**Help text** (two lines, ≤ ~70 chars each, matches the panel):
```
Stereo, 5.1 or 7.1 over HDMI. Dolby Digital sends the film's Dolby
track untouched: use it if your soundbar drops the centre channel.
```

### B2. Bitstream fixes

1. **Reassert uses the wrong channel count.** `audio_bitstream_reassert()`
   (called after the 24p switch and revert in `display_24p.cpp`) hard-codes
   `want.channel = 6`. In passthrough that must be `2` with encoder 255.
   Fix: `audio_bitstream_begin()` stores the `audioOutConfiguration` it
   applied (`s_applied_cfg`); `reassert()` re-applies exactly that. Log line
   keeps the rc but drops the GetState "wire type" claim (see 2).
2. **False log wording.** `audio_bitstream.cpp` logs
   `"routing as %s, wire stays type=%u (LPCM intact)"` and nearby comments
   claim the wire stays LPCM. That's false whenever Dolby Digital is ticked in
   the PS3's sound settings (the AC-3 request then becomes real DD), and
   GetState can't tell. Reword the log to
   `"bitstream: requested %s (GetState type=%u, not authoritative)"` and fix
   the comments at ~lines 109 and 319 on main to say the request *may* become
   real Dolby Digital depending on the console's sound settings.
3. **Do not change** the 5.1/7.1 default AC-3 "routing" request behaviour in
   3.2. It's how every current user gets a centre channel; whether it ends up
   PCM or DD depends on ARC vs eARC and console settings, and that isn't
   something to re-tune now.

### B tests

- Host: `test_iec61937` (burst header Pa/Pb/Pc/Pd, 6144-byte frame for
  48 kHz AC-3, zero padding, byte order); `test_track_codec` gains
  `track_label_is_ac3` cases ("Dolby Digital 5.1", "AC3 5.1", "E-AC-3" must be
  **false**, "Dolby Digital Plus" must be **false**, "TrueHD" false).
- TV (user): see §T.

---

## C. Settings reorganised

Today: 16–17 rows in one list, and the row index is a magic number in three
places (`SETTINGS_LABELS` / `SETTINGS_ICONS` / `SETTINGS_HELP` arrays and the
`if (i == N)` draw chain in `ui_settings.cpp`, the `if (g_settings_sel == N)`
chain in `ui_nav.cpp xmb_input_settings`). Every new row shifted every
number (the changelogs are full of "stats moved to row 15").

### C1. Table-driven model

New file `source/ui/settings_model.c` + `.h` — **pure C, no PS3/RSX
includes**, so it host-tests:

```c
typedef enum {            // stable ids; never reorder-dependent
    SET_LOGOUT, SET_DEBUG_LOG, SET_SCREEN_SIZE, SET_HD1080, SET_AUDIO_OUT,
    SET_DIALOGUE, SET_SUB_FONT, SET_SUB_COLOUR, SET_THEME, SET_PARTICLES,
    SET_DAYNIGHT, SET_WAVE_INT, SET_AUTOSKIP, SET_24HZ, SET_UPDATE,
    SET_SEND_LOG, SET_STATS, SET_DOWNLOADS, SET_OFFLINE_LIB, SET__COUNT
} setting_id;

typedef enum { SEC_PLAYBACK, SEC_AUDIO, SEC_SUBTITLES, SEC_DOWNLOADS,
               SEC_DISPLAY, SEC_SYSTEM, SEC__COUNT } setting_section;

typedef struct {
    setting_id      id;
    setting_section section;
    const char     *label;
    const char     *help;   // exactly two lines, '\n'-separated
} setting_row;

int                settings_count(void);            // visible rows (stats row compiled out => absent)
const setting_row *settings_row(int i);             // i in [0, count)
int                settings_index_of(setting_id);   // -1 if absent
const char        *settings_section_label(setting_section);
int                settings_section_first(setting_section); // row index, -1 if empty
```

The UI side (`ui_settings.cpp`) keeps a parallel table keyed by `setting_id`
with the PS3-specific bits: icon, `value(char *buf, int n, bool *accent)`,
`activate()`, `step(int dir)`. `xmb_input_settings` and the draw loop become
loops over the table — no row numbers anywhere. `XMB_SETTINGS_COUNT` goes
away; its users (`ui_xmb.cpp slog_menu_tick`, `ui_nav.cpp`, `ui_spine.cpp`
"Up at row 0 leaves") call `settings_count()`. `g_settings_sel` stays a plain
row index, so the spine's "Up from the first row leaves the tab" keeps working
unchanged.

**No setting file names or values change.** This is presentation only.

### C2. Sections and controls

Order and grouping:

| Section | Rows |
|---|---|
| **Playback** | 1080p Playback (Alpha) · 24Hz Output · Auto Skip |
| **Audio** | Audio Output · Dialogue Boost |
| **Subtitles** | Subtitle Font · Subtitle Colour |
| **Downloads** | Downloads (`2 active` / `1 failed` / `None`) · Offline Library (`3` / `Empty`) (from E1; both `Unavailable` with no writable store) |
| **Display** | Screen Size · Theme · Day / Night Palette · Wave Intensity · Menu Particles |
| **System** | Software Update · Send Log to Server · Debug Logging · Player Stats Overlay · Log Out |

Log Out moves to the bottom (it was first only because it was the original
row; it's the most destructive action and needn't be the default focus).
First focus is therefore 1080p Playback.

**Layout** (in `ui_settings.cpp`):
- Section header: label in `UIS_TF(13)`, `XMB_TEXT_FAINT`, letter-spaced
  uppercase (e.g. `PLAYBACK`), at `list_x + UIS_W(20)`, with a 1 px
  `XMB_HAIRLINE` rule from the end of the text to the list's right edge.
  Header block height `UIS_H(34)`; rows keep `SET_ROW_H` / current pitch.
- Replace the uniform-pitch window (`settings_first_row`/`settings_row_y`)
  with a computed layout: one pass builds `y[]` for every header and row
  (content coordinates), then a scroll offset in pixels keeps the selected row
  fully inside `[rows_top, display_height - XMB_BOTTOM_PAD]` **and**, when the
  selected row is the first of its section, keeps that section's header
  visible too. Scroll eases (`~120 ms`, the same easing the grids use) rather
  than snapping, if a shared easing helper exists; otherwise snap.
- Draw only items that intersect the visible band (clip with the existing
  scissor helpers if present; otherwise skip partial items).
- `settings_open_help_peek()` takes its rect from the new layout.
- The legacy (non-spine) in-place help panel keeps working.
- Must look right at 720p and 1080p and with Screen Size overscan applied.

**Controls:**
- Up/Down: rows (headers are skipped — they're never selectable).
- **L2 / R2: jump to the first row of the previous/next section** (free in
  menus today — verify with `git grep "BTN_PRESSED(l2)" source/ui/xmb`).
- X: activate / next value (unchanged behaviour).
- **Left / Right: previous / next value** on value rows; ignored on action
  rows (Log Out, Screen Size, Software Update, Send Log). Toggles flip on
  either. Needs a direction-aware step in each module:
  `surround_step(int dir)`, `centermix_step`, `subfont_step`, `subcolor_step`,
  `theme_step`, Wave Intensity (`(lvl + dir + 4) % 4`). Implement as
  `xxx_step(int dir)` with `xxx_cycle()` = `xxx_step(+1)`, persisting once per
  step. Don't loop the forward cycle n-1 times (that writes the file n-1 times).
- Triangle help, O-closes-help, L1/R1 tabs: unchanged.
- Update the bottom hint bar for Settings: `Triangle About · L2/R2 Section`.

### C tests

`tests/test_settings_model.c` (add to `tests/Makefile.host` `all:`):
- every id in `[0, SET__COUNT)` appears exactly once (except `SET_STATS`
  when `ENABLE_PLAYER_STATS` is 0 — build the test both ways, `-DENABLE_PLAYER_STATS=0/1`);
- rows are grouped: a section never reappears after another starts;
- every help string has exactly one `'\n'` and each line ≤ 72 chars;
- every label ≤ 24 chars;
- `settings_section_first` returns the first row of each non-empty section.

Layout math is easiest to test if the scroll function is pure too: put
`settings_scroll_for(sel, item_y[], item_h[], n, band_top, band_bottom)` in
`settings_model.c` and assert the selected row is fully visible for every
`sel` at 720 and 1080 heights, and that the header is visible when `sel` is a
section's first row.

---

## E. Offline downloads

Branch `origin/feature/offline-downloads` (`c16507b`), written by a cloud
session: 7 commits, ~10.9k lines, Stages 1–5 complete and host-tested (45k
checks, ASan/UBSan clean). **It has never run on a console.** It's based on
`15ec9fe` (09-19), so it predates the spine, 24p, the seek/stream fixes and
most of the details page. The design doc is `docs/offline-downloads.md` on
that branch. Read §2, §5a, §7a, §11c, §11d and §12 before starting.

Summary: from an item's page, DOWNLOAD fetches a PS3-playable `.ts` (the
same stream decision Play uses) to `/dev_hdd0/jellyfin_offline/items/<id>/`.
It's resumable, survives reboots, and pauses while you stream anything heavier
than 480p. It plays offline through the existing player (`show_player_offline`),
from an Offline Library that works with no server, which is also offered
when sign-in fails.

### E1. Merge

```bash
git merge --no-ff origin/feature/offline-downloads   # on release/3.2, after A
```
A trial merge onto main gave **11 conflicted files**: `Makefile` (2 hunks),
`tests/Makefile.host` (4), `jellyfin_api.h` (1), `net/http.cpp` (1),
`player/core/player.cpp` (9), `player_seek.cpp` (2), `player_session.cpp` (2,
one ~190 lines), `player/stream/stream.h` (1), `ui_settings.cpp` (1),
`ui_visuals.h` (1), `ui_info.cpp` (1). Everything under `source/offline/`
and `tests/test_offline.cpp` is new and merges clean.

**General rule for every conflict: main's behaviour wins. Offline's change is
re-applied on top of it.** Main has three weeks of hardware-verified fixes in
these files. The offline branch has none.

Per file:

1. **`player_session.cpp` vs `stream_request.cpp` (the hard one).** Offline
   moved the whole stream decision out of `build_stream_url()` into
   `player/stream/stream_request.{h,cpp}` (pure: `StreamPrefs` →
   `StreamSelection` → `StreamRequest` → URL). Since then main changed that
   decision a lot: quality steps that include their audio, "Original" quality,
   copy at every quality, console-side text/PGS subtitles (no burn-in cost),
   MediaSource threading, `LiveStreamId`, stream budget kinds, version
   summary.
   **Resolution: redo the extraction from main's current `build_stream_url()`.**
   Take main's function body, move it statement by statement into
   `stream_request_resolve()` / `stream_url_build()`, add any `StreamPrefs`
   fields main's code now reads (anything it took from a global), and leave
   `build_stream_url()` as the thin wrapper the offline branch made it.
   Keep offline's API shape (`dl_request.cpp` and `test_offline.cpp` call it).
   Then **prove equivalence**: add `tests/test_stream_request.c` with ~15
   golden URLs derived by hand from main's pre-merge `build_stream_url()`.
   Cover Stereo / 5.1 / 7.1 × {Original, 1080p step, 720p step, 480p step} ×
   {AC-3 track, TrueHD track, DTS track} × {no subs, text sub, PGS sub}, plus
   a version with a `LiveStreamId`. Assert byte-for-byte equality, parameter
   order included. Write the expected strings *before* resolving the
   conflict, from main's code, so they test main's behaviour, not the merge's.
2. **`stream.h` / `stream.cpp`.** Offline adds a local-file source (handle
   tagged `0x40000000`) plus `stream_close()` / `stream_set_timeout()`
   wrappers. **Bug in the offline branch:** its `stream_set_timeout()` and
   reopen path call libc `setsockopt(SO_RCVTIMEO)` on a `netSocket` fd, with
   the old timeout struct. That is exactly the 09-27 root cause (8-byte
   `{u32,u32}` where lv2 wants a 16-byte `struct timeval`, via libc rather than
   `netSetSockOpt`): the timeout silently never applied, causing 2–3 s
   blocking reads, 24p judder and laggy seeks. Main uses
   `netSetSockOpt(..., SO_RCVTIMEO, &tv /* struct timeval */, sizeof tv)` plus
   `netPoll` before reads/headers. The wrappers must call main's code. After
   merging, `git grep -n "setsockopt(" source/player source/offline` must
   show no `SO_RCVTIMEO`/`SO_SNDTIMEO` through libc.
3. **`player.cpp` (9 hunks).** Offline split `show_player()` into a shared
   `show_player_run(..., local)`, where every local difference is an explicit
   `if (local)` (see the table in its doc §11c). Keep main's body: 24p switch,
   bounded waitflips, seek-wait spinner, watchdog, 1:1 presentation, report
   thread, the A1 pause fix and Dolby Digital audio_open. Then re-insert
   offline's `if (local)` branches. Get the list from
   `git diff 15ec9fe origin/feature/offline-downloads -- source/player/core/player.cpp`
   (77 lines). For local playback also confirm:
   - the 24p switch still works (local files know their fps from the TS);
   - no `report_*` / stop-transcode / PlaybackInfo call runs with `local`
     (an unreachable server blocks for its connect timeout);
   - `dl_playback_begin()`/`dl_playback_end()` wrap **every** exit path, including
     main's newer exits (seek-wait cancel, watchdog bail, next-episode chain).
4. **`player_seek.cpp`.** Local seeks go through `stream_local` (interpolation
   search in the file). Online seeks keep main's seek retry, `player_seek_wait`
   and async old-transcode stop. `dl_playback_begin(url)` is called for every
   new seek URL.
5. **`net/http.cpp`.** Offline adds `http_open_socket()` (connect under the
   resolver lock). Keep main's file (dechunk fix from A2, `struct timeval`
   timeouts) and add only the new function.
6. **`ui_info.cpp`.** Offline put its DOWNLOAD button on the **legacy** info
   page. Keep it there (the legacy page still exists with the spine off), but
   the real work is E2's spine page.
7. **`ui_settings.cpp`, `ui_visuals.h`.** If C is already done, the two rows
   become `SET_DOWNLOADS` / `SET_OFFLINE_LIB` in the table. If not, resolve
   minimally and let C absorb them. (In the recommended order E1 comes before
   C. Resolve minimally.)
8. **`main.cpp`** merged without conflict, but check it semantically. Offline
   hooks `do_login()` failure to offer the Offline Library. Main's login is
   now `ui_osk_login`. Make sure the offer appears on the real failure path
   (server unreachable **and** auth rejected), and that "Try again" is still
   the default.
9. **Makefiles:** union of both. `source/offline` must be in the PS3
   `SOURCES`. Check `.DEFAULT_GOAL` is still `$(BUILD)` (an earlier target-order
   change once made `make` build nothing).
10. Rename the branch's tool images out of `tools/ui_preview/` only if they
    collide. Otherwise keep them.

**Done when:** a clean `make pkg` and `make -B -f tests/Makefile.host all check`
pass, including `test_offline` and the new `test_stream_request`. Then
**a TV session (§T-E)** before building anything on top.

### E2. Offline UI on the spine, and polish

1. **Spine details page** (`xmb_show_item_info_v3` in `ui_info.cpp`, the one
   users actually see). Its action rows are `row0[]` (Resume/Play, Start,
   Watched) and `row1[]` (Version, Quality). Add `F3_DOWNLOAD` to **row1**,
   after Quality, for Movie/Episode/Video. Row 0 stays the "watch now" row.
   Reuse the legacy page's logic verbatim: `dl_find` polled every 250 ms,
   `dl_ui_item_label()` for the text, `dl_ui_item_action()` for what X does,
   a thin progress bar along the button's bottom edge, and a 3 s toast. Draw
   the button with the same GPU rrect/outline calls as Version/Quality, and
   the focus ring with `spine_focus_ring_gpu`. Toast text becomes
   "Added to Downloads (Settings › Downloads)". Check `ax[]`/`aww[]`/`sx_[]`
   array sizes when adding a slot: row arrays are fixed-size today.
2. **Settings › Downloads section** (C's table): Downloads and Offline Library
   open offline's existing screens (`ui_downloads.cpp`).
3. **The Downloads/Offline screens under the spine.** They're blocking overlay
   loops drawn with CPU `drawRect`/`drawTTF` over the wave, like the resume
   prompt, which works under the spine. Verify that they render with
   `gputext`/`gpucards` on, that they call `spine_frame_begin()`/flip the
   way the resume prompt does, and that menu SFX play. Restyle their colours
   to the current theme tokens (`XMB_PANEL`, `XMB_ACCENT` …) and replace any
   hard-coded `0x00131630UL`-style constants. Wrap sizes in `UIS_W/H/TF`;
   the branch predates the UI-scale fix and uses raw pixels.
4. **Posters in the two lists** (doc §10 follow-up): decode `poster.jpg` off
   the UI thread. Reuse the library grid's background thumb path
   (`thumb_cache_*`) with a `file://`-style source if it has one. Otherwise
   add a tiny loader thread that decodes into the same cache. Never decode on
   the render thread.
5. **Offline Library from the XMB without a server**: already reachable via
   the sign-in-failure offer. Also add it as a **Home row "Downloaded"**
   (first 25 completed items, landscape cards from the posters) when the
   library isn't empty. Cross plays offline.
6. Hint bars: Downloads list `X Pause/Resume · □ Cancel/Delete · O Back`. On
   these screens `□` means the row's secondary action, **not** the visualiser
   cycle. Make sure `draw_hints_vis` doesn't append the visualiser hint there.

### E3. HDD safety: downloads never fill the console's drive

**User requirement:** downloads must stop before the internal HDD gets low.
**At least 10 GB always stays free.** A PS3 with a full HDD can't save
games, install updates or packages, or keep its system caches. Users must
never get there because of this app.

What the offline branch does today (`dl_manager.cpp`) isn't enough:
- `reserve_bytes` is **1 GB**;
- `space_ok()` returns **true when free space is unknown**
  (`DL_FREE_UNKNOWN`, e.g. libsysfs failed to load or `sysFsGetFreeSize` errored).
  That means *no limit at all*;
- for a transcode of unknown length `size_hint` is 0, so enqueue checks only
  the reserve, not what the file will need;
- running out mid-transfer marks the item **failed** (`DL_ERR_NO_SPACE`).

Required behaviour:

1. **Floor:** `reserve_bytes = 10 GiB` (`DL_RESERVE_MIN`). An optional
   `jellyfin_dlreserve.txt` (whole GB) may **raise** it and is clamped to
   `>= 10`. Nothing can lower it. Log the effective value at service start.
2. **Fail closed.** If free space can't be read (unknown, error, block size
   0, or a value larger than the volume's total size), **no download starts
   or continues.** The active one pauses with a new reason
   `DL_ERR_SPACE_UNKNOWN`, shown as "Can't check free HDD space: downloads
   paused". If `sysModuleLoad(SYSMODULE_FS)` fails at service start, the store
   reports downloads **unavailable**. Already-downloaded files still play.
3. **Check at enqueue against everything already committed.**
   `need = estimate(new) + Σ remaining(estimate) of every queued/active item`.
   Refuse when `free − reserve < need`. Estimates:
   - a direct copy → the MediaSource `Size`;
   - a transcode → `(vbitrate + abitrate) / 8 × runtime × 1.15`, using the
     `StreamRequest` the download will use;
   - no runtime and no size → refuse with "Size unknown, can't check space"
     rather than guessing 0.
   The refusal text gives the numbers: "Not enough space: needs 6.2 GB,
   3.1 GB available (10 GB is always kept free)".
4. **Check while writing:** before each 64 MB (`space_check_bytes`, already
   64 MB; at ~3 MB/s that's every ~20 s) and on **every** failed write.
   Continue only while `free − reserve ≥ 64 MB`. Never write within the
   reserve, even by one chunk.
5. **Pause, don't fail.** Hitting the floor pauses the item (state PAUSED,
   reason `no_space`) with its partial file kept. No other queued item starts
   while below the floor. When the user frees space, *Resume* continues by
   Range. Downloads list banner: "Paused: the HDD is down to 10 GB free".
6. **Show it:** the Downloads screen header reads "HDD: 42.0 GB free ·
   downloads keep 10 GB free". The item page's Download button shows the
   refusal reason as its toast. G's season sheet uses the same `need`
   calculation and **refuses** (not merely warns) when the whole season doesn't
   fit. Offer "Download the first N that fit" instead.
7. **Log** every decision: `dl: space free=…MB reserve=…MB need=…MB -> ok|refused|paused|unknown`.
8. The rule covers every byte the downloader writes: media, artwork and
   records. Artwork and records are tiny, but they go through the same
   `space_ok` gate.

Tests (`test_offline.cpp`, fake platform's scripted free bytes):
- refused at enqueue when below floor + need;
- unknown free space → refused / paused;
- an implausible value (> total) → treated as unknown;
- the reserve file can raise but can't lower the floor;
- crossing the floor mid-transfer → PAUSED with the partial kept, then
  resumes after free space rises;
- the queue-wide commitment refuses the 3rd item when two queued ones already
  use the room;
- the season "first N that fit" calculation.

Hardware check (part of T-E): the logged `free=` matches XMB › Settings ›
System Settings › System Information free space, within rounding. Using
`/dev_hdd0/tmp/` as the fallback root is the same disk, so the floor applies
unchanged.

---

## F. Favourites

1. **API** (`source/api/api_userdata.cpp/.h`, new, mirroring
   `info_mark_played`):
   `bool jf_set_favourite(const char *item_id, bool fav)` →
   `POST` (fav) / `DELETE` (unfav) `%s/Users/%s/FavoriteItems/%s`, 2xx = ok.
   Use `http_request` with `HTTP_DELETE`. If that verb doesn't exist, add it to
   `http.cpp` (`"DELETE"` method string, no body). Bump `g_play_gen` on success
   so Home re-fetches. Move `info_mark_played` here too, as
   `jf_set_played(id, true)`.
2. **Parse `UserData.IsFavorite`** in the details fetch
   (`api_detail.cpp`), into `XMBItemDetail.is_favourite`.
3. **Spine details page:** add `F3_FAVOURITE` to `row0` after Watched. It's a
   toggle button: `ICON_STAR` (already in the Tabler subset; there's no heart,
   so don't regenerate the font) plus "Favourite". When set, the icon is
   filled with the accent and the label reads "Favourited". Optimistic toggle:
   flip immediately, revert with a toast if the request fails.
4. **Home row "Favourites"**: `GET /Users/{uid}/Items?Filters=IsFavorite&Recursive=true&IncludeItemTypes=Movie,Series,Episode&SortBy=SortName&Limit=25&Fields=PrimaryImageAspectRatio`.
   Place it after Next Up. Only show it when non-empty. Same fetch/thumb path
   as the existing Home rows (`ui_home.cpp`, `HOME_ROW_MAX 25`). `HOME_ROWS_N`
   grows by one. Check every array sized by it.
5. Legacy (non-spine) info page: add the same toggle only if it's ≤ 30 lines.
   Otherwise skip it, since the spine is the default UI.
6. Live TV (H) reuses `jf_set_favourite` for channels.

Tests: a host test for the URL builder of `jf_set_favourite` (method + path)
if the API file can be compiled on the host with the existing stubs.

---

## G. Download a whole season

Needs E.

1. **Trigger:** on the TV seasons screen (`g_tv_depth == 1` in `ui_nav.cpp`),
   **Triangle** on a season opens a small action sheet:
   `Download Season 2 (10 episodes)` / `Cancel`. Triangle at depth 1 does
   nothing today (depth 2 = episode info). `□` is taken menu-wide by the
   visualiser cycle, so don't use it. Reuse an existing modal pattern (the
   resume choice / confirm dialog style). Add `Triangle Download season` to
   that screen's hint bar only when downloads are available
   (`dl_manager_ready()`).
2. **Queuing runs on a worker thread**, never the render thread. Each episode
   needs its detail + media sources, and `http_request` holds a global mutex.
   New `source/offline/dl_season.cpp`:
   - `dl_season_start(series_id, season_id)` fetches the season's episodes
     (`/Shows/{series}/Episodes?SeasonId=&UserId=&Fields=MediaSources`). Then, per
     episode, in order:
     - skip it if `dl_find()` already has it (any state except failed/cancelled);
     - fetch the detail and versions the details page loads;
     - pick version 0 (what Play would stream), or the version matching the
       series' remembered choice if `match_version` applies (main's next-episode
       logic);
     - call `dl_download_item()`.
   - A joinable thread (like `log_upload`) publishes progress
     `{done, total, failed}` for a toast: "Queuing 3 of 10…" → "10 episodes added
     to Downloads". It stops early on `DL_ERR_NO_SPACE`.
   - `dl_season_cancel()` sets a flag checked between episodes.
3. **Disk space:** before starting, sum the episodes' `Size` (from
   MediaSources, when it's a copy) or estimate from runtime × the request's
   bitrate. If that exceeds free space (`dl:` root free MB), say so in the sheet
   ("Needs ~14 GB, 9 GB free") and still allow it. The manager already fails
   individual items cleanly on ENOSPC.
4. Offline Library groups nothing new: episodes already show
   "Series  S2 E3".

Tests (host, fake platform from `tests/dl_fake.cpp`): skip-already-queued,
cancel midway, and the stop on no-space error.

---

## H. Live TV / IPTV

**Test environment already exists.** The user's server (10.11.11, `.194:8096`)
has an M3U tuner "Test channels" →
`C:\ProgramData\Jellyfin\Server\iptv\test-channels.m3u`: four public HLS
streams (NASA TV ×2, Red Bull TV, Apple bipbop test pattern). **No guide
(EPG) provider is configured**, so `/LiveTv/Programs` is empty. To test the
guide, generate a local XMLTV file
`C:\ProgramData\Jellyfin\Server\iptv\test-guide.xml` with 3 days of fake
programmes for `tvg-id` nasa1/nasa2/redbull/bipbop (a small Python script in
`tools/`). Then **ask the user** before adding it as a listing provider (Dashboard
› Live TV › TV Guide Data Providers › XMLTV, or `POST /LiveTv/ListingProviders`
with an admin token). That's a server config change.

Get the API token as `[[jellyfin-server-layout]]` describes: the console's
`/dev_hdd0/tmp/jellyfin_config.txt` (host, user, token, user id). **Never call
`AuthenticateByName`**: it logs the PS3 out.

How Jellyfin exposes it: a user view with `CollectionType = "livetv"`;
channels at `/LiveTv/Channels`; programmes at `/LiveTv/Programs`; playback
via `PlaybackInfo` on the **channel id** with `AutoOpenLiveStream=true`,
returning a MediaSource with a `LiveStreamId`. The stream URL carries
`LiveStreamId`; on exit, `POST /LiveStreams/Close?LiveStreamId=`. The app
already threads `live_stream_id` through `JFMediaSource`, `player_seek.cpp`
(`AutoOpenLiveStream` for plugin sources) and the URL builder, which helps a lot.

### H1. API layer (pure, host-tested)

New `source/api/livetv.cpp/.h`, a hand-rolled parser in the style of
`media_sources.cpp`:

```c
typedef struct {
    char id[40], name[96], number[12];
    char logo_tag[40];               // ImageTags.Primary, "" if none
    bool favourite;                  // UserData.IsFavorite
    // current programme (AddCurrentProgram=true), may be empty
    char now_title[128];
    u64  now_start_ticks, now_end_ticks;
} JFChannel;

typedef struct {
    char id[40], channel_id[40], name[128], episode_title[96];
    u64  start_ticks, end_ticks;     // UTC, 100 ns since 0001-01-01
    bool is_movie, is_series, is_news, is_sports, is_kids, is_live, is_premiere;
} JFProgram;

int livetv_parse_channels(const char *json, int len, JFChannel *out, int max, int *total);
int livetv_parse_programs(const char *json, int len, JFProgram *out, int max, int *total);
u64 jf_ticks_from_iso8601(const char *s);   // "2026-10-03T19:30:00.0000000Z"
```
Fetch helpers (console side) in `jellyfin_api.cpp`:
- `GET /LiveTv/Channels?UserId=&EnableImages=true&ImageTypeLimit=1&AddCurrentProgram=true&EnableUserData=true&SortBy=SortName&Limit=200`
  (sort by channel number when numbers exist: `SortBy=ChannelNumber` isn't
  universally supported, so sort client-side by numeric `Number`).
- `GET /LiveTv/Programs?UserId=&ChannelIds=<csv>&MinEndDate=<now>&MaxStartDate=<now+3h>&SortBy=StartDate&EnableImages=false&Limit=400`.
  **Mind the 384 KB `RESPONSE_SIZE` cap.** It already truncated big replies
  (memory: debrid episode details). Fetch the guide in channel pages of ~15
  and in 3-hour windows. Check for truncation (missing closing `]}`) and log it.
- The console clock is the "now": `sysGetCurrentTime` / the same clock
  Day/Night uses. It must be UTC for comparing ticks. Verify the timezone
  handling in a test.

Host tests `tests/test_livetv.c`: fixtures captured from the user's server
(`curl` the two endpoints with the token above → `tests/fixtures/livetv_*.json`;
**strip the token, user id and server address from the fixtures**). Also a
hand-made programmes fixture with overlapping/gapped/zero-length programmes.
Test the ISO-8601 parser across the 7-digit fraction, `Z`, and a missing
fraction.

### H2. The Live TV tab

1. **Tab kind:** in `ui_fetch.cpp` where `CollectionType` maps to a tab kind,
   map `"livetv"` → new `TABKIND_LIVETV` (icon `ICON_TV`, label "Live TV").
   Today it falls through to `TABKIND_GENERIC` and would browse the view as a
   folder. Add the kind to every `switch (xmb_kind(tab))`. The compiler will list
   them with `-Wswitch` if the switches have no `default`. Otherwise grep.
   Spine: the tab joins the category row like any other.
2. **Channel list** (tab root): one row per channel, the same row rhythm as the
   music list or Settings rows:
   - Number in tabular figures, channel logo (`/Items/{id}/Images/Primary?tag=&maxHeight=…`
     through the existing thumb cache, landscape-ish logos letterboxed into a
     fixed 96×54 slot), channel name;
   - On the right: the current programme title and a thin progress bar
     (now − start)/(end − start), plus "Next: 20:30 Title" in faint text when the
     guide has it. Without EPG data, just the name.
   - Favourite channels first when any exist (star icon), then the rest.
     Triangle toggles favourite (`jf_set_favourite`, from F).
   - X = watch. Re-fetch the list every 60 s while the tab is open, and on
     return from the player, so now/next stays current.
3. **Guide grid.** **Right** from the channel list slides into the guide, and
   Left at the guide's first column comes back. That's spatial, like the PS3's
   own TV guide. (`□` is taken menu-wide by the visualiser cycle.)
   - Rows = channels (same order, logos pinned on the left, 8 visible), columns =
     time, 30-minute grid lines, a 3-hour window starting at the previous half
     hour, and a vertical "now" line in the accent.
   - Programme cells are rounded rects (`wave_draw_rrect_outline_gpu`) with the
     title clipped; the focused cell gets `spine_focus_ring_gpu`. Left/Right
     moves between programmes on the row (scrolling time), Up/Down moves
     channels, keeping the time position.
   - X on a programme airing now = watch the channel. X on a future programme =
     a details peek (title, time, episode title, overview via
     `GET /LiveTv/Programs/{id}?UserId=`). No recording in 3.2.
   - Guide data loads on a worker as the window moves (page of channels ×
     3 h). Cells not loaded yet draw as empty panels. Never block the render
     thread on `http_request` (render-thread HTTP behind the global mutex is a
     known lag source).
   - Hidden entirely when the server has no programmes for any channel. Then
     Right does nothing, and the list shows names only.
4. Search: no change in 3.2 (channels don't come back from item search
   reliably).

### H3. Live playback

Entry: `xmb_play_channel(const JFChannel *ch)` → `show_player()` with a
`JFItem` whose type is `"TvChannel"`. The player gets a `ps.live` flag set
from the item type.

1. **Open:** PlaybackInfo on the channel id with `AutoOpenLiveStream=true` (the
   same POST + device profile the player already sends, plus `UserId`). Use
   the returned MediaSource's `Id` and `LiveStreamId` and build the URL through
   the shared stream decision (it already appends `LiveStreamId`). Live
   sources from HLS take several seconds before the first byte (the server
   starts ffmpeg on the HLS input). Show the existing buffering screen with
   the channel logo and name, and give the header wait a **20 s** budget (not
   the VOD one). Circle cancels.
2. **`ps.live` behaviour** (every place the player assumes a duration):
   - The HUD shows a `LIVE` badge in the accent where the time/duration
     would be, plus the current programme title and its progress if known.
     There's no seek bar.
   - Seeking is disabled: L2/R2, the scrub bar, chapter/skip-segment logic, auto
     skip, the next-episode countdown, trickplay. Guard each, and grep
     `total_secs` uses; anything dividing by it must handle 0.
   - Pause: allowed. On resume after more than 10 s paused, reopen at the
     live edge (close + PlaybackInfo again) rather than playing a stale
     server buffer. Simplest correct behaviour: show a "Returning to live"
     spinner.
   - No resume position: don't report progress ticks as a resume point. Do
     report Playing/Stopped with `LiveStreamId` so the dashboard shows it.
   - **Channel up/down:** with the HUD hidden, **Up/Down** (and the remote's
     CH+/CH− if `ui_input` exposes them; check the BD remote keymap) switch
     to the previous/next channel in list order. Show a channel banner
     (number, logo, name, now title) for 3 s. Implement it as stop + open
     next channel inside the player loop (close the old LiveStream first), and
     debounce 600 ms so rapid presses only open the final channel.
   - EOF on a live stream isn't the end: the server's ffmpeg restarted or the
     source hiccuped. Reopen up to 3 times with backoff 1/2/4 s, then show the
     error screen. The stream-read watchdog must treat a live stall the same
     way.
   - The 24p auto switch must stay off for live (frame rate is 25/50/59.94).
     It already only engages for 23.976, but make the guard explicit (`!ps.live`).
   - Audio: HLS IPTV is usually AAC. The server transcodes per the Audio
     Output setting exactly as for VOD. Dolby Digital mode → AC-3 transcode.
     Nothing special.
   - Interlaced sources (DVB/ATSC tuners, not these HLS tests): the request
     must not allow direct copy of interlaced H.264. Have the decision force a
     video transcode for `TvChannel` items, so the server deinterlaces.
     Bitrate per the quality setting. Keep it simple: live = always transcode
     video.
3. **Close:** on every exit path, `POST /LiveStreams/Close?LiveStreamId=…`,
   on the existing report thread, never blocking exit. Also stop the
   transcode (`DELETE /Videos/ActiveEncodings?DeviceId=&PlaySessionId=`) as VOD does.
4. **Downloads:** never offered for channels (`dl_capable` is already
   Movie/Episode/Video only). A live stream counts as **heavy** for the download
   gate (`dl_playback_begin`), so downloads pause while watching TV.

Tests: host tests for the HUD/state helpers you make pure (e.g. a
`live_progress(now, start, end)` clamp, channel-order next/prev with
wrap-around, debounce). The rest is TV.

---

## U. USB / external drive playback (no server needed)

Play the user's own files from **USB drives formatted NTFS, exFAT or
FAT32**, and from the internal HDD: **MKV, .ts and .m2ts video**, and **FLAC,
MP3, WAV music**. It needs no Jellyfin server or sign-in. Full-bitrate
Blu-ray remuxes play untouched with lossless audio, which the ~25 Mbps
server path can't do.

**Read first:** `docs/local-media.md` on `origin/feature/offline-downloads`
(scoping doc, now merged by E1). It's accurate except for two things: main now
renders text and PGS subtitles on the console, and NTFS/exFAT come from mohasi
below, not libntfs_ext.

Hard limits, to be stated in the UI, not discovered by users:
- H.264 only (up to Level 4.1/4.2, what VDEC takes).
- **HEVC/H.265, 4K, 10-bit and VC-1 can't play**: the PS3 has no decoder.
  The file row shows "HEVC (can't play on PS3)" in faint text, and opening it
  explains why. Never let it reach VDEC.
- MPEG-2 video: not in 3.2.
- Audio output is 48 kHz: other rates are resampled.

**The drives are read only.** The app never writes to a USB drive: no resume
files, no thumbnails, no `.DS_Store`-style droppings. All local state goes
to `/dev_hdd0/tmp/` (subject to nothing large: resume positions only).

### U1. Filesystems: port mohasi's NTFS + exFAT readers

Source: **github.com/mohasi/ps3-dev**, `libs/simple-lib-core/`:
`src/ntfs.c` (5.8k lines), `src/exfat.c` (2.9k), `src/vfs.c`, `src/vfs-init.c`,
`include/{ntfs,exfat,vfs,vfs-internal,storage-device,cellfs}.h`.
**Licence Apache-2.0**, which may be included in this GPL-3 project. Keep
their headers and add the NOTICE text to `LICENSE`/README credits ("NTFS and
exFAT readers from mohasi/ps3-dev, Apache-2.0"). Pin the commit hash you
vendored in `third_party/mohasi_fs/VERSION`.

How it works: raw sector reads through the lv2 storage syscalls
(`sys_storage_open` 600, `close` 601, `read` 602, `get_device_info` 609).
USB mass storage device ids are `0x10300000000000A + port` (ports 0–5;
6–7 use `…1F +`). `get_device_info` is non-DMA, so it's safe to poll for
hotplug. Then hand-written NTFS (MFT, runlists, `$I30` index; MBR/GPT
partition offsets) and exFAT parsing, endian-safe through explicit LE readers.
These are the same syscalls ManaGunZ/webMAN use, and they need HEN/CFW,
which this app already requires.

Port into `third_party/mohasi_fs/`, with **minimal edits**, each marked
`// jf-port:`:
- `scCallN(...)` → PSL1GHT `lv2syscallN` (`ppu-lv2.h`; `lv2syscall7` exists).
  Return the syscall result exactly as `scCall` did.
- `thread.h` lwmutex helpers → `sysLwMutexCreate/Lock/Unlock` (`sys/mutex.h`).
- `string-utilities.h` (`memCopy`, `memSet`, `utf16ToUtf8`, `strCmpICase`) →
  a small `jf_port_strings.h` over libc. Keep their `utf16ToUtf8`
  implementation if it isn't trivial.
- `cell/rtc.h` (timestamps for the **write** path) and `sys/timer.h` →
  PSL1GHT equivalents, or delete them along with the write path.
- **Remove the write path entirely**: every function that allocates clusters,
  writes MFT records/bitmaps/directory entries, and any `STORAGE_WRITE` (603).
  Then `grep -n "603\|STORAGE_WRITE\|allocateClusters\|write" third_party/mohasi_fs`
  must find no live write code. Mount read-only, always. Delete rather than
  `#if 0`, so nobody can switch it back on.
- Drop `disc-mount`, the Blu-ray/TOC helpers in `storage-device.h`, and the
  `cellfs.c` backend if our own seam (U2) covers `/dev_usb`/`/dev_hdd0`.
- Read buffers must be 32-byte aligned (their header says so): use
  `memalign(128, …)`.

Host tests (`tests/test_mohasi_fs.c`). Build 64 MB NTFS and exFAT images in
WSL as root:
- NTFS: `mkntfs --fast --force img`, mount with `ntfs-3g`, copy files in;
- exFAT: `mkfs.exfat img`, `mount -o loop -t exfat`, copy files in.

If WSL can't mount them, make them on Windows instead (a VHD via diskpart,
format, copy, detach). Commit the images **compressed** (≤ 1 MB each) under
`tests/fixtures/fs/`. Run the ported readers on the host with a fake
`readStorageRaw` that reads the image file. Assert directory listings,
file sizes, the SHA-256 of a 3 MB fragmented file, Unicode names, a
non-zero partition offset (MBR and GPT), and files > 4 GB (sparse
image). This also proves the big-endian port: run it under
`qemu-ppc64` with `powerpc64-linux-gnu-gcc -static` (both installed).

### U2. One read-only file layer: `source/local/lfs.{h,cpp}`

```c
typedef enum { LFS_HDD, LFS_USB_FAT, LFS_USB_NTFS, LFS_USB_EXFAT } lfs_kind;
typedef struct { char id[16];        // "usb0", "hdd"
                 char label[48];     // volume label or "USB drive 1"
                 lfs_kind kind; u64 total, free; } lfs_drive;

int  lfs_drives(lfs_drive *out, int max);         // present now (hotplug-polled)
int  lfs_list(const char *path, lfs_entry *out, int max, int offset); // "usb0:/Movies"
int  lfs_open(const char *path);                  // handle or <0
int  lfs_read(int h, u64 off, void *buf, u32 n);  // positional, bytes or <0
u64  lfs_size(int h);
void lfs_close(int h);
```
Backends: FAT32 USB (`/dev_usb00N`) and the internal HDD via `sysLv2Fs*`
(lv2 mounts FAT32 itself); NTFS and exFAT via U1. Probe order for a present
USB port: lv2 FAT32 mount exists → FAT; else try the NTFS probe, then exFAT.
Internal HDD root offered: `/dev_hdd0/video` and `/dev_hdd0/music` plus
`/dev_hdd0/jellyfin_local` (created if missing). Not the whole HDD, since
system folders are noise and risk.

- Hotplug: a background thread polls `get_device_info` per port every 2 s,
  mounts and unmounts. The UI reads a generation counter, not the devices.
- Unplug mid-read: `lfs_read` returns an error, the player shows "The drive
  was removed", and nothing crashes or hangs. Every syscall wait must be
  bounded.
- Thread safety: one lwmutex per mounted volume (mohasi already serialises).
- Throughput: reads ≥ 256 KB per call (a remux needs ~6 MB/s, USB 2 gives
  ~20–30). Log `lfs: usb0 ntfs read 64 MB in X ms` once per mount as a quick
  benchmark.

The offline branch's `stream_open_file()` / `stream_local` read with
`sysLv2Fs` directly. **Switch them to `lfs_*`** so downloads (HDD) and USB
files share one path.

### U3. Containers

1. **.ts:** plays through E1's local path as-is.
2. **.m2ts** (192-byte BD packets): strip the 4-byte TP_extra_header per
   packet in the local reader, then it's .ts. Detect by sync bytes at offsets
   4 and 196. `stream_local`'s seek index works on the stripped stream.
3. **MKV: new demuxer `source/video/mkv_demux.{h,cpp}`.** The large piece.
   Design per `docs/local-media.md` §4b:
   - EBML: Segment, SeekHead, Info (TimestampScale, Duration), Tracks,
     Cues, Clusters, SimpleBlock/BlockGroup, and all three lacing modes
     (Xiph, fixed, EBML).
   - Video `V_MPEG4/ISO/AVC`: avcC from CodecPrivate → SPS/PPS. Rewrite
     length-prefixed NALs to Annex B and re-send SPS/PPS before every keyframe.
     Reject `V_MPEGH/ISO/HEVC`, `V_MS/VFW/FOURCC` (VC-1) and anything else
     with a reason.
   - Audio: `A_TRUEHD`, `A_DTS` (incl. HD MA), `A_AC3`, `A_EAC3` (no decoder:
     fall back to another track or say so), `A_AAC` (libfaad, in portlibs;
     GPL-2+, fine), `A_FLAC` (libFLAC), `A_PCM/INT/LIT`, `A_MPEG/L3` (minimp3),
     `A_VORBIS` (libvorbis, if cheap; otherwise unsupported in 3.2).
   - Subtitles: `S_TEXT/UTF8` (SRT), `S_TEXT/ASS` (strip styling to text),
     `S_HDMV/PGS`. Feed them to the existing console renderers. Today
     `subs_load()` / `subs_load_pgs()` fetch from the server, so add
     `subs_load_cues_local(...)` / a PGS feed that takes demuxed blocks.
     Read `player/subtitles.h` and `subtitles_pgs.h` first, and keep their
     index/lazy-decode design.
   - Output: hand the decoders **exactly** what `ts_demux` hands them
     (same AU/PES structs, same 90 kHz PTS), so `vdec`/`adec` don't change.
     Read `video/ts_demux.h` and how `player.cpp` consumes `ts_process` before
     designing the interface. Bounded memory: no per-frame malloc, and the AU
     ceiling from H.264 L4.1 (`ts_demux.h`).
   - Seek: Cues; without Cues, a bounded cluster scan (interpolate by size,
     then refine), the same approach as `stream_local`.
   - Frame rate from DefaultDuration (or the SPS VUI), so the 24p auto switch
     works for 23.976 files.
   - Host tests `tests/test_mkv_demux.cpp`: small fixtures made with ffmpeg in
     WSL (`-t 3`, 64×64 H.264 + AC-3/TrueHD/FLAC/AAC/PCM, laced audio, no-Cues
     file, SRT and PGS tracks). Goldens = AU count, sizes, PTS and SHA-1 per AU.
     Fixtures committed, ≤ 200 KB each.
4. **Track choice** (local files have no server to choose): list audio tracks
   with language + codec. Default to the best decodable one: TrueHD/DTS-HD MA →
   DTS → AC-3 → others, then honour the Audio Output setting the way the
   server path does (Stereo downmixes; Dolby Digital passes AC-3 through and
   decodes others). Subtitles off by default, but forced tracks on. The
   existing HUD audio/subtitle menus drive it. They're inert for downloads
   (one track), but live for local files.

### U4. Local music

- A PCM source switch in the music player (`audio_set_source()` exists):
  **FLAC** (libFLAC, 16/24-bit, up to 8 ch → stereo for music), **MP3**
  (minimp3), **WAV**.
- **Resampler:** the current linear `frame_to_48k` isn't acceptable for a
  lossless library. Add a windowed-sinc polyphase resampler
  (`source/audio/resample.{h,c}`) for 44.1→48 (L/M = 160/147), 88.2/96/176.4/192→48.
  32–48 taps, Kaiser window, coefficients precomputed at init.
  48 kHz input = pass-through (bit-exact). Host test: THD+N of a 1 kHz sine
  44.1→48 below −90 dB, and passband flatness.
- Album view: a folder of audio files = an album. Track order from tags
  (FLAC Vorbis comments TRACKNUMBER, ID3 TRCK), else filename. Art from the
  FLAC PICTURE block, else `cover.jpg`/`folder.jpg`. Decode art off the
  render thread. Gapless between tracks of one folder.
- Visualisers (JellyWave/JellyDrop/Canyon) read the PCM, so they work
  unchanged. Verify.

### U5. UI: the "Media" tab

- New `TABKIND_LOCAL`, label **"Media"**, icon `ICON_COLLECTIONS`. It's
  **always present** (the internal HDD is always there), placed after Live TV.
  Also reachable from the sign-in-failure screen ("Open Offline" →
  Offline Library + Media), so it works with no server at all.
- Root: one card per drive (label, kind "NTFS · 1.8 TB", free space), plus
  "Downloads" (E's Offline Library) as the first card. A drive appears or
  disappears live when plugged or unplugged, with the menu SFX.
- Folder browser: rows with icon (folder / video / music / unsupported),
  name, size and duration (duration lazily probed for visible rows only).
  Sorted folders first, natural sort. X opens, O goes up. Only playable
  extensions and folders are shown: `.mkv .ts .m2ts .mts .flac .mp3 .wav`.
  A **jump bar** (existing component) for long folders.
- Video file → a light details page: file name (cleaned: dots/underscores →
  spaces, year detected), duration, video codec/resolution, audio tracks,
  subtitle tracks, then Play / Resume. No server metadata in 3.2.
- Music folder → album page (art, track list) → plays in the existing music
  screen.
- **Resume:** `/dev_hdd0/tmp/jellyfin_local_resume.txt`, keyed by
  `path + size + mtime`, atomic writer from `dl_store`, ≤ 500 entries (LRU).
  It's written to the HDD, never to the USB drive.
- Live TV and USB playback share nothing else. Downloads count as heavy for
  the download gate (`dl_playback_begin_local(heavy)`) so they pause during
  local playback too. It's disk I/O, not the network, but the HDD is the same
  disk the download writes to, and a 40 Mbps remux shouldn't compete.

### U order

U1 (filesystems + host tests) → U2 (lfs + switch stream_local) → U3 .ts/.m2ts →
**TV check (T-U1)** → U3 MKV → U3 track choice + subs → **TV check (T-U2)** →
U4 music → U5 polish → **TV check (T-U3)**. U5's basic browser is needed from
the first TV check, so build a minimal list first and polish it last.

---

## D1. ui_wave.cpp statics → state structs (separate branch)

The open item from the 09-27 external review: `source/ui/render/ui_wave.cpp`
(2,717 lines) has ~109 file-scope mutable statics. Goal: group them into a
dozen named state structs, **no behaviour change**.

Branch `refactor/wave-state` off `release/3.2` once A–C are committed.

**Grouping** (line numbers are `origin/main`):

| struct (static instance) | members |
|---|---|
| `wave_audio_state s_au` | `s_amp[3] s_lum s_lum3[3] s_thick s_acc s_def s_band_lvl[3] s_band_kick` (104–113, 189–190) |
| `wave_nav_state s_nv` | `s_nav s_nav_init s_nav_fx s_nav_us` (144–147) |
| `jd_state s_jd` | `s_jd_loaded … s_jd_toggles` (177–186) |
| `vis_state s_vs` | `s_vis s_vis_off s_reveal_t0` (216–218) |
| `tint_state s_tn` | `s_art_rgb s_art_k s_art_on s_art_us s_album_rgb s_tint_rgb` (378–400, 929) |
| `wave_gpu s_gpu` | `s_bg s_bg_dim s_wave_fp_buf s_wave_fp_offset s_wave_vbuf[2] s_wave_vbuf_off[2] s_wave_vbuf_turn s_wave_varray s_wave_blend s_wave_jelly` (493–597) |
| `jw_render s_jr` (render thread only) | `s_jw[] s_jw_stage[] s_jw_disp[] s_jw_upload_us s_jw_repaired s_jw_dropped s_jw_off s_jw_cnt s_jw_have_geom s_jw_drawn s_jw_ex_stage s_jw_fresh` |
| `jw_stats s_js` | `s_jw_gen_us s_jw_frames s_jw_rebuilds s_jw_verts s_jw_draws s_jw_late s_jw_lerp_n s_jw_marker` |
| `jw_worker s_jk` (**shared with the worker thread**) | `s_jw_wdisp[] s_jw_wscratch[] s_jw_job_sy s_jw_job_aspect s_jw_job_look s_jw_back_* s_jw_job s_jw_worker_run s_jw_worker s_jw_tid` (465–466, 1287–1301) |
| `jw_interp s_ji` | `s_jw_interp_on … s_jw_t_frame` (1405–1415) |
| `wave_tune s_tu` | `s_jw_speed s_jw_rebuild_every s_jw_rebuild_phase s_jw_front_body s_bg_dither` |
| `snow_state s_sn` | `s_snow s_snow_spr[] s_mote_* s_obst_n` (674–698) |

`s_field` (already one object) and the `static const` tables stay as they are.

**Rules:**
- One struct per commit. After each: PS3 compile-check `ui_wave.cpp` (§0.4),
  `make -B` host tests, and a full `make pkg` every 3–4 commits.
- Pure renames: `s_jd_morph` → `s_jd.morph`. No reordering of statements, no
  "while I'm here" fixes, no changes to initial values (designated
  initialisers must reproduce every non-zero initial value — e.g.
  `s_nav_fx = {0,0,0,1}`, `s_lum = 1`, `s_amp = {1,1,1}`, `s_vis = -1`,
  `s_jw_speed = JW_SPEED_DEF/100`, `s_bg_dither = 1`, `s_mote_lvl = 1`,
  `s_jw_t_frame = 1`, `s_jw_job_aspect = 1`, `s_tint_rgb = {1,1,1}`).
  Write a checklist of every non-zero initialiser before starting and tick it off.
- Keep `volatile` on `s_jw_job` and `s_jw_worker_run` **as members**
  (`volatile int job;`), and keep any `__attribute__((aligned(N)))` on arrays
  (RSX/DMA-touched buffers need it — a misaligned store into RSX memory has
  wedged the GPU before). Add `_Static_assert(offsetof(...) % 16 == 0, ...)`
  for every array that is memcpy'd/uploaded to RSX.
- Do not touch the reuse-frame / blend branch logic around `s_jw_stage` —
  that's where the strobe bug was (a reuse frame fell into the legacy blend
  branch and overwrote `s_jw_stage[0..581]`); rename only.
- Size check per commit: `ppu-size` of `ui_wave.o` text/data/bss before and
  after — `bss+data` must match to within padding, `text` within ~1 %.
- Don't reflow the history comments; they were already moved to
  `docs/wave-renderer-notes.md`.

**Done when:** user TV soak (§T-D) passes. If it isn't TV-verified by release
day, 3.2 ships without it and the branch carries into 3.3.

---

## T. Hardware test script (for the user — one session, ~30 min)

Ask the user to run these; keep `player_log.txt` (now uploadable via
Settings → Send Log to Server, which is itself test T1).

**T1 Send Log:** Settings → System → Send Log to Server → "Sent".
Check Jellyfin Dashboard → Logs has `upload_PS3_*.log` with both runs.
**T2 Pause + PS button:** play anything, pause, wait 5 s, press PS → overlay
visible, navigable, "Return to game" resumes control.
**T3 Dolby Digital, DD track:** Audio Output = Dolby Digital; play a film with
a Dolby Digital 5.1 track → soundbar shows DOLBY AUDIO, centre speaks,
log has `copy_audio=true` and `AudioCodec=ac3`. Seek twice → still DD.
**T4 Dolby Digital, non-DD track** (TrueHD/DTS/AAC film) → still DOLBY AUDIO
(server transcode to AC-3), centre OK.
**T5 Dolby Digital + 24p** (24Hz Output = Auto, a 24 fps film) → after the
mode switch the log shows `re-asserted ... ch=2` (not 6) and audio is DD;
exit to menu → menu sounds normal (no encoder 255 leak).
**T6 Volume slider** in DD mode shows the soundbar message / doesn't open.
**T7 Back to 5.1:** set 5.1, play the same film → behaves exactly as 3.1.
**T8 Music** in DD mode plays stereo normally.
**T9 Settings:** sections render at 1080p and 720p, with Screen Size at a
non-zero overscan; L2/R2 jump sections; Left/Right step values; Triangle help
on a few rows; Up from the first row returns to the tab spine; values persist
after a restart.
**T-E Offline (after E1, again after E2):**
- the log line `dl: root ... free=...MB` names `/dev_hdd0/jellyfin_offline`;
- download a film (DOWNLOAD on its details page) and watch the progress in
  Settings › Downloads;
- start streaming a 1080p film mid-download: the download shows
  "Paused while streaming", then resumes when you stop;
- power-cycle the PS3 mid-download: it resumes and doesn't restart from 0;
- unplug the network (or stop the Jellyfin service): launch, then sign-in fails
  and offers "Open Offline", and the film plays offline with seeking and audio;
- the stream URL log lines for an online film match the golden URLs from
  `test_stream_request` (proves the extraction kept 3.1's requests);
- delete the download; the space comes back.

**T-F Favourites:** star a film and a series on their details pages; a
Favourites row appears on Home; un-star it and the row updates. Check the
Jellyfin web UI agrees.
**T-G Season:** Triangle on a season → Download Season (N episodes) →
"N episodes added", all queued in Downloads; repeating it skips the ones
already queued.
**T-H Live TV:**
- the Live TV tab appears; the four test channels show logos and names;
- watch NASA TV: the buffering screen shows the channel, the picture starts
  within ~20 s, and the HUD shows LIVE with no seek bar;
- Up/Down switches channels with a banner, and rapid presses only open the last one;
- pause 30 s, then resume: it returns to live;
- exit: the Jellyfin dashboard shows no lingering live stream or transcode;
- with the test XMLTV guide added: now/next on the list, the guide grid via
  Right, the now line, a future programme's details;
- Live TV with Dolby Digital output: DOLBY AUDIO on the soundbar.

**T-E3 HDD floor:**
- the log shows `reserve=10240MB`, and `free=` matches System Information;
- create `jellyfin_dlreserve.txt` containing a value just under the current
  free space minus a small film's size: the download is refused with numbers;
- delete the file, then fill the HDD to ~10.5 GB free with a dummy file over
  FTP and start a download: it pauses at the floor, the partial is kept, and
  after deleting the dummy *Resume* continues. **Delete the dummy file
  afterwards.**

**T-U1 (filesystems + .ts/.m2ts):**
- plug in an NTFS drive, an exFAT drive and a FAT32 stick: all three appear
  under Media with labels and free space;
- unplug one: it disappears without a hang;
- play a 40 Mbps .m2ts remux from NTFS with TrueHD: no stalls, lossless audio;
- pull the drive mid-play: "The drive was removed", back to the menu;
- confirm on a PC that the drives are unchanged (no new files).

**T-U2 (MKV):**
- a MakeMKV remux (H.264 + TrueHD + PGS) plays, seeks, and switches audio
  and subtitle tracks;
- a 23.976 MKV triggers 24 Hz;
- an HEVC MKV shows the "can't play on PS3" message;
- an AAC and a FLAC-audio MKV both play;
- Resume works after leaving and coming back.

**T-U3 (music):** a 16/44.1 FLAC album and a 24/96 FLAC play gapless with
art, the visualisers react, and MP3 and WAV play too.

**T-D (D1 build only):** JellyWave in menus 10 min; JellyDrop + snow with
music 10 min; Square cycles visualisers incl. Canyon; Off → JellyWave reveal;
a 24p film start/stop; watch for strobing, black frames, or a stuck wave.

---

## R. Release 3.2

1. `source/net/update_check.h`: `APP_VERSION "3.2"`.
2. `CHANGELOG.md`: new `## 3.2` section (draft below), user-facing, no
   internals.
3. `README.md`: controls table (Settings L2/R2, Left/Right) and the Audio
   Output description (Dolby Digital).
4. **Fresh-install test (mandatory — 3.0 shipped broken for new users).** On
   the console, rename every `/dev_hdd0/tmp/jellyfin_*.txt` except
   `jellyfin_config.txt` and `jellyfin_device_id.txt` to `.txt.devbak`,
   launch, and confirm: new interface, JellyWave, GPU posters/text, logging on,
   Audio Output = Stereo, Settings opens on 1080p Playback. **Restore the
   `.devbak` files afterwards.**
5. Clean build from `git archive` (§0.1); copy
   `obj/pkg/USRDIR/EBOOT.BIN`-containing `.pkg` to `release/JellyFin-PS3.pkg`
   and `outputs/JellyFin-PS3v3.2.pkg` (overwrite, don't add `-v2` files).
6. Merge `release/3.2` → `main` (fast-forward), and `feature/xmb-spine` ff to
   match, as 3.1 did. **Ask the user before pushing**, and before creating the
   GitHub Release `v3.2` (the in-app update check reads the latest release tag,
   so publishing it notifies every user).

### CHANGELOG draft

```markdown
## 3.2

### New
- **Live TV.** If your Jellyfin server has Live TV set up (a tuner, or an IPTV
  M3U playlist), a Live TV tab lists your channels with what's on now. Press
  `→` for the TV guide. While watching, `↑`/`↓` change channel. Favourite
  channels (`△`) come first.
- **Downloads.** *Download* on a film or episode's page saves a copy to the
  PS3's hard drive. It pauses while you stream and picks up where it left off,
  even after a restart. Watch downloads from Settings › Downloads › Offline
  Library, or from the Downloaded row on Home. When the server can't be
  reached at startup, the app offers your downloads instead.
- **Play from USB drives, no server needed.** The new Media tab plays your own
  MKV, .m2ts and .ts films and FLAC, MP3 and WAV music from USB drives
  formatted **NTFS, exFAT or FAT32**, or from the PS3's hard drive. Blu-ray
  remuxes play at full bitrate with their lossless TrueHD / DTS-HD MA audio
  and subtitles. Drives are only ever read, never written to. (H.264 video
  only: HEVC/4K and VC-1 files can't play on a PS3.)
- **Download a whole season**: `△` on a season.
- Downloads always leave at least **10 GB free** on the PS3's hard drive,
  and pause rather than fill it.
- **Favourites.** Star films and shows on their page; they get their own row
  on Home.
- **Dolby Digital output.** Settings → Audio → Audio Output has a new
  *Dolby Digital* choice. It sends the film's Dolby Digital track to your
  soundbar or receiver untouched (other tracks are converted to Dolby Digital
  by your server). Use it if your soundbar is connected through the TV's
  ARC port and the centre channel — where the dialogue is — goes missing in 5.1.
  Volume is then set on the soundbar.
- **Send Log to Server.** Settings → System → Send Log to Server uploads the
  diagnostic log to your Jellyfin server (Dashboard → Logs), so you can attach
  it to a bug report without FTP. Diagnostic logging is now on by default.
- **Settings has sections** — Playback, Audio, Subtitles, Display and System.
  `L2`/`R2` jump between sections, and `←`/`→` change a value in either
  direction (`X` still steps forward).

### Fixes
- Pressing the PS button while a video was paused could leave the player
  looking frozen; the system menu now appears.
- Some items opened to an empty details page and then hung on retry when the
  server's reply was framed unusually.
```

---

## Things deliberately left out of 3.2

- Live TV recording/DVR, timers, catch-up, and channel search.
- USB: BDMV folder playlists, MPEG-2 video, E-AC-3 decoding, ALAC/M4A,
  any write support for USB drives, a scanned local library index/search, and
  server-style metadata for local files.
- E-AC-3 / TrueHD / DTS bitstream — not reachable or not decodable on the
  test chain.
- Changing how the 5.1/7.1 AC-3 routing request behaves.
- The `jellyfin_bitstream.txt` research modes on main stay as they are
  (file-only, undocumented).
- psl1ght-extras upstream patches, Moonlight work, Android TV port.

## Backlog for 3.3+ (discussed 10-03, not scheduled)

Rough sizes; none designed yet.
- **Quick Connect sign-in + profile picker.** Approve a 6-character code on a
  phone instead of typing on the OSK (`/QuickConnect/Initiate`,
  `/QuickConnect/Connect`, `/Users/AuthenticateWithQuickConnect`), plus a
  "who's watching" picker from `/Users/Public`. S–M. Highest value per line
  of anything here.
- **Chapters**: a chapter list with images in the player, and next/previous chapter. M.
- **Theme music on details pages** (`/Items/{id}/ThemeSongs`), with JellyWave
  reacting to it. S–M; shares the audio port with the UI sounds.
- **Synced lyrics** in the music player (`/Audio/{id}/Lyrics`, LRC timing). M.
- **Sleep timer** (30/60/90 min, end of episode). S.
- **Live TV recordings/DVR** (needs server-side recording set up). M–L.
- USB follow-ups: BDMV playlists, MPEG-2, E-AC-3, a local library index with
  posters fetched from the server by title match.
