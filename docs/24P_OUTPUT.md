# Physical 1080p23.976 / 1080p24 output

Status: **implemented, opt-in; the mode switch is PROVEN on hardware (2026-09-24).** The mechanism was read
out of the firmware. It was not guessed, and it is not proven on a TV yet.

## TL;DR

- `cellVideoOutConfigure` (v1, what PSL1GHT calls `videoConfigure`) **cannot**
  select a refresh rate. VSH's handler hard-codes refresh = AUTO.
- `cellVideoOutConfigure2` (library `cellSysutilAvconfExt`) **can**. VSH reads a
  u16 refresh at config offset `0x0A` and accepts `0, 0x01, 0x02, 0x10, 0x20,
  0x40`. `0x10` and `0x20` are the 24Hz-family bits this console's TV advertises
  for 1080.
- It is a normal game-facing API. No CFW, root or kernel access is needed.
- Which of `0x10`/`0x20` is 23.976 and which is 24.000 is **not** named anywhere
  in the firmware. The app **measures** it: it times the vblank IRQ against the
  timebase, which separates the two clocks (1000 ppm apart) easily.

## How the evidence was obtained

The official 4.93 `PS3UPDAT.PUP` was downloaded from Sony's update server and
decrypted offline with a Python port of RPCS3's own firmware-install code
(`research24p/ps3fw.py`; keys are read at runtime from RPCS3's public
`key_vault.cpp`). The disassembly comes from `research24p/ppcdis.py` +
`tools/fwcap_analyze.py`. The console runs 4.92. The ABI of a shipped SDK call
does not change between 4.92 and 4.93, but the opt-in capture
(`avconf_capture.cpp`) can confirm it on the console itself.

### libsysutil_avconf_ext.sprx (10 KB) is only a marshaller

It imports nothing but CXML helpers and `cellSysutilPacketBegin/Write/End/Read`.

| export | FNID | code | what it does |
|---|---|---|---|
| cellVideoOutConfigure2 | 0x9faa12be | 0x1d7c | `(videoOut, cfg*, opt*, waitForEvent)`; cfg must be non-NULL (`0x8002b222`) and cfg+0xC (pitch) non-zero (`0x8002b223`); copies cfg (16 B) and opt (16 B, NULL allowed) **verbatim**; sends VSH packet **0x4b08** with a 0x28-byte payload `{videoOut, cfg[16], opt[16], waitForEvent}` |
| cellVideoOutGetResolutionAvailability2 | 0x21036f4e | 0x1c50 | (not needed) |
| cellVideoOutSetupDisplay | 0x269ffedd | 0x1858 | (not needed) |
| cellVideoOutGetScreenSize | 0xfaa275a4 | 0x1f28 | packet 0x4b0d |

v1 `cellVideoOutConfigure` in libsysutil.sprx (code 0x13bfc) sends packet
**0x4b07** with `{videoOut, cfg[16], opt (u32), waitForEvent}`. It also rejects
resolution ids 0x92 and 0xA1 on the client side.

### vsh.self is where the rate is decided

- The dispatcher at 0x127470 sends 0x4b07 to **0x126e18** and 0x4b08 to **0x126af8**.
- v1 handler: its mode converter call `sub_c7080(…, res, 1, 0)` passes refresh = 0
  (AUTO) at **every** call site. `sub_c7080` is just `li r8,1; b 0xc6f8c`.
- The Configure2 validator at **0x128000** reads cfg+0 (res), +1 (format),
  +2 (aspect), +3 (scan mode 2), **+0xA (refresh, u16)** and +0xC (pitch). The
  refresh must be one of `{0,1,2,0x10,0x20,0x40}`, else it returns `0x8002b226`.
  Note that 60 Hz (4) and 30 Hz (8) are *not* accepted.
- The converter at **0xc6b18** is the only function in VSH that honours a
  requested rate, and its only caller is the Configure2 handler. It maps
  `0x1→0x10000, 0x2→0x20000, 0x4→0x40000, 0x10→0x4000, 0x20→0x8000,
  0x40→0x100000` into the internal mode word. If the requested bit is not in
  the display mode's advertised rates, it falls back to AUTO (59.94 first)
  instead of failing.
- There is no per-caller permission check in the handler. Its gates are
  global-state reads (`c56b8`, `c56e8`), a check that rejects resolution 0x0E
  (`c5bec`) and a debug-override read (`c5ad8`).
- VSH statically links **libavset** (`avset_setvideomode`,
  `avset_getmonitorinfo`, …) and contains the `sys_uart` syscalls:
  `sys_uart_initialize` 0x5dece8, `send` 0x5ded6c and `receive` 0x5dedb8. That is
  the lowest layer: lv1's AV Manager, reached over the vuart. RPCS3's lv2 table
  marks syscalls 367–370 **ROOT**, so only VSH can talk to the AV Manager, and
  every game-side request is relayed through it.

### The struct, as VSH reads it

```c
typedef struct {            // CellVideoOutConfiguration2 (16 bytes)
    u8  resolution;         // +0
    u8  format;             // +1
    u8  aspect;             // +2
    u8  scanMode2;          // +3  0 auto, 1 interlace, 2 progressive
    u8  reserved[6];        // +4
    u16 refreshRates;       // +A  one of 0,1,2,0x10,0x20,0x40
    u32 pitch;              // +C
} videoConfiguration2;
```

v1 sends the same 16 bytes, but VSH's v1 path zeroes the scan-mode and refresh
fields (`sub_127f4c`).

### How Sony's own Blu-ray player fits

- `bdp_BDMV.self` (live 4.92 dump) has its own statically linked GCM. It issues
  `sys_rsx_context_attribute` directly (22 sites) and never uses package 0x100
  (display mode set).
- It contains no `sys_uart`, no sysutil packet code and no 0x4b07/0x4b08, so it
  **cannot** change the output timing itself.
- It chooses a display mode internally. Its enum at 0x8a773c lists `1080P_23_976`
  (15) and `1080P_24` (16) separately, plus the D5 23.976/24 Hz frame-packing
  modes. It also sets the vblank frequency to **SCANOUT**
  (`sys_rsx_context_attribute` 0x108, value 2).
- Exactly how its request reaches VSH was not traced, but VSH is the only process
  that can make the change.

## What the app does (`source/video/display_24p.cpp`)

Nothing, unless `/dev_hdd0/tmp/jellyfin_24p.txt` contains `1`. When enabled, at
playback start (after prefill, before any playback thread):

1. It gates on `dm_decide`: film content (24000/1001 or 24/1) whose rate was
   *detected* (not guessed); a 1920×1080 progressive output; and the TV
   advertising 0x10/0x20 for 1080. If any gate fails, it logs why and does
   nothing.
2. It loads `cellSysutilAvconfExt` (sysmodule 0x31) and checks the import
   resolved.
3. It draws a prompt and flips while the **original** mode is still up. No draw
   or flip happens again until the new mode is verified.
4. It times a **baseline** vblank at the original rate. This proves the
   measurement on this hardware and must read ~16683 µs at 59.94.
5. It writes the abort marker `jf_24p_pending.txt`, **then** calls
   `videoConfigure2` with refresh `0x10` or `0x20`.
6. It re-measures. If the IRQ still ticks at 59.94, it sets
   `gcmSetVBlankFrequency(SCANOUT)` (as the BD player does) and measures again.
   It keeps a bit only if the measured clock **equals the content's**: 23.976
   for 24000/1001 and 24.000 for 24/1. The bit→clock result is cached in
   `jf_24p_map.txt` as an ordering hint (it is still measured every time).
7. The first time on a TV, you must press X within 15 s. O or a timeout reverts
   and records the TV as failed (`jf_24p_confirmed.txt` = 0).
8. On success it hands the **measured** rate to timing
   (`timing_set_display_override`) and re-runs `timing_init`, so the existing
   Bresenham gate yields 1:1. The timing engine is otherwise untouched.
9. After every playback thread has been joined, it restores the original mode
   (Configure2 with the original bit, or v1 exactly as at boot), restores the
   vblank frequency and removes the marker.
10. If the app dies while switched, the next launch finds the marker and
    **turns 24p off**.

Seeks, audio switches and subtitle reopens never touch the mode. Each title is
its own session.

## Hardware test

Test on a TV that shows its input format on screen (the UT30's info banner, or
a receiver's display). Do not rely only on the log.

1. Install the PKG. With 24p still off, play one 23.976 title. Its `24p:` lines
   should read `attempt=no … 24p output is off`. That is the baseline.
2. Over FTP, write `1` to `/dev_hdd0/tmp/jellyfin_24p.txt`.
3. **A — 1080p 23.976 on a 24p TV:** play the title and expect the one-time
   prompt. After the picture returns, check the TV shows 1080p/24 and press X.
   In `player_log.txt`, grep `24p:`. Expect a `baseline vblank=16683.x`, a
   `SWITCH try` line with `clock=23.976`, `RESULT mode_switch=success …
   presentation=1:1`, and `timing: display=24000/1001 MEASURED`. In the `pd=`
   heartbeat, hold2/hold3 go to ~0 and the count lands in the third ("other")
   field, because each frame is now held for one vblank. At 1:1 that is correct,
   not a fault.
4. **G — exit:** stop playback. Expect `24p: REVERT (playback ended) … refresh
   now 0x01`, and the XMB-side UI back at 59.94.
5. **H — seek:** seek several times during A. There must be no `SWITCH` or
   `REVERT` lines between the start and the end.
6. **I — switching titles:** play 23.976 → 25/30/50/60 fps → 23.976. Only the
   film titles switch, and each revert happens at its own session end.
7. **C/D/E/F:** 30/25/50/60 fps content should give `candidate=no`.
8. **B — TV without 24p:** expect `display_support=no`, and no prompt or switch.
9. **720p-only or 1080i setups:** expect `candidate=no (output is not 1920x1080 /
   output is interlaced)`.
10. Record which bit measured as which clock. `jf_24p_map.txt` keeps it.

**If anything goes wrong:** press O or wait 15 s and it reverts. If the console
hangs, power it off. The next launch disables 24p by itself, or you can write
`0` to `jellyfin_24p.txt` over FTP. To ask again after a failed TV check, delete
`jf_24p_confirmed.txt`.

## Hardware result, 2026-09-24 (CECH-2503, 4.92, Panasonic UT30)

```
24p: baseline vblank=16685.042 us clock=59.940
24p: SWITCH try refresh=0x10 rc=0x00000000 state=0x10 vblank=41707.888 us clock=23.976
24p: REVERT (no answer within 15 s) rc=0x00000000 refresh now 0x01 (was 0x01)
```

- The TV's input switched to 24 Hz, and **refresh bit 0x10 = 23.976** (measured).
  So 0x20 is presumably 24.000.
- The game vblank IRQ follows the scan-out by default. SCANOUT was not needed.
- The revert worked cleanly.
- The screen stayed black because nothing was flipped after the switch, so the
  confirmation prompt was invisible. It is now redrawn once the new vblank has
  been measured. Flipping is safe at that point: the 09-18 hang happened only
  because the vblank had stopped.
- Playback then ended (`playing=0`) and the app returned to Home instead of
  continuing at 59.94. The cause is **not known yet**. A stream stall during the
  ~20 s pause is one possibility, not a finding. The lifecycle trace below is
  there to find the real cause.

## Lifecycle trace for the next test

Every line starts with `lc t=<ms>` (timebase milliseconds), so this command
prints one ordered timeline of the whole session:

```
grep "lc t=\|playing=0 reason=\|24p:\|playstate:\|stop_transcode:" player_log.txt
```

| Line | Where | What it shows |
|---|---|---|
| `play-session CREATED psid=` | player.cpp, after PlaybackInfo | The PlaySessionId this playback uses |
| `stream CONNECTED sock=` | player.cpp, after stream_open | The HTTP stream is open |
| `session[before_24p]`, `session[after_24p]` | player.cpp, around `d24_session_begin` | Player state plus a non-consuming `MSG_PEEK` of the socket |
| `24p: phase mode_switch_begin` / `_verified` / `_failed` | display_24p.cpp | The switch starts and is measured, or fails |
| `24p: phase prompt_redrawn_after_switch` | display_24p.cpp | The post-switch redraw returned (flip completed) |
| `24p: phase confirm_wait_begin`, `confirm_wait Ns` | display_24p.cpp, once a second | A `session[...]` snapshot for each second of the wait |
| `24p: phase confirm_wait_end ans= ... (TIMEOUT)` | display_24p.cpp | Answer, elapsed time, and whether it timed out |
| `24p: revert begin` / `revert done` | display_24p.cpp `revert()` | Revert to 59.94 and the rate it landed on |
| `24p: session_begin returned` | player.cpp | Whether the session continues at 24p |
| `sysutil: event status=` | main.cpp | Every sysutil event (XMB, exit request), not just EXIT_GAME |
| `stream: peer closed (...)` | stream.cpp | The server closed the TCP connection |
| `stream: server sent final 0-length chunk` | stream.cpp | The server ended the HTTP response on purpose |
| `stream: netRecv ERROR net_errno=` | stream.cpp | A socket error that the old code silently treated as a timeout |
| `playback: decode thread STARTED` | player.cpp | Decode thread spawned after the 24p phase |
| `decode: first packet after spawn` / `no data for Nms` / `stream_read FAILED` / `thread exit` | player_threads.cpp | Whether data still flows once playback resumes |
| `session[after_preroll]`, `playback: START`, `first frame displayed` | player.cpp | Playback actually resumed |
| `playback: STOP cause=` | player.cpp, main-loop exit | Which loop condition ended playback |
| `play-session DESTROY psid=` | player.cpp | The report of the stop and the transcode kill |
| `show_player: RETURN to UI` | player.cpp | Control goes back to the calling screen (Home, if launched from Home) |

How the trace separates the possible causes:

- **A. Player alive, stream stalls:** the socket shows `idle_open` during the
  wait, then `decode: no data for Nms` keeps repeating, with no `stream_read
  FAILED`.
- **B. Player explicitly stopped:** `playback: STOP cause=user_stop`, or a
  `playing=0 reason=` line from the player itself.
- **C. Play session destroyed:** `play-session DESTROY` or `stop_transcode:`
  appears *before* `playback: STOP`, or a `playstate:` report fails.
- **D. UI navigation to Home:** a `sysutil: event` line during the wait, or
  `show_player: RETURN to UI` with no STOP line before it.
- **E. Network/HTTP teardown:** `PEER_CLOSED` or `ERROR` in a
  `session[confirm_wait Ns]` probe, then `stream: peer closed` or `final
  0-length chunk`, then `decode: stream_read FAILED` and
  `playing=0 reason=stream_eof`.
- **F. Something else:** none of the above. `STOP cause=vdec_error` would be one
  example.

Before the test, delete `jf_24p_confirmed.txt`: the last run's timeout wrote `0`
to it, and with that value the app will not try the switch again.

## Still unproven

- Which of 0x10/0x20 is 23.976. By Sony's pattern (59.94 = 0x01 comes before
  60 = 0x04, and AUTO prefers 0x10 over 0x20), 0x10 is *likely* 23.976. The app
  does not rely on that.
- Whether the game-side vblank IRQ follows scan-out by default. The baseline and
  post-switch measurements answer this on the first run.
- Whether this TV actually syncs. The confirmation prompt answers that.
- The route by which Sony's Blu-ray process asks VSH for its mode.
