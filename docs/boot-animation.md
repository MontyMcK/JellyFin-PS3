# Cold-boot animation

What the console shows between the EBOOT starting and the XMB taking over:

```
BLACK  ->  the Jellyfin mark fades in, centred
       ->  it catches the light: a thin glint, a twinkle on its apex
       ->  it shrinks and glides into the top-left lockup, and the XMB rises
           out of the black behind it as it goes, bottom edge first
       ->  J-E-L-L-Y-F-I-N unfolds out of it to the right
       ->  the static XMB
```

The animation hides cold-start loading without adding to it. The data the
XMB's first frame needs loads *behind* the mark, and the XMB is live, so a
button press skips straight to it, for the whole of the handoff.

| file | what |
|---|---|
| `source/ui/render/boot_seq.h` | the timeline: phases, readiness, easing, geometry. Pure C, no RSX, host-tested |
| `source/ui/render/boot_anim.{h,cpp}` | the renderer and the integration calls |
| `source/gfx/jfmark_hd_png.h` | the lockup mark at 256 px (same SVG as the 80 px one) |
| `tests/test_boot_seq.c` | every path through the timeline |
| `tests/test_text_runs.cpp` phase 4 | the animated wordmark lands bit-exactly on the static one |
| `tools/bootanim_preview/` | an HTML player of the real timeline, for judging motion off-console |

## The startup it sits on

The cold boot, as measured on hardware (`player_log` 2026-09-22, mode 3,
1080p). From the first log line to the XMB's first frame takes about
1–1.5 s, and nothing in it is individually long:

| step | where | cost | during the animation |
|---|---|---|---|
| `init_screen`, `ioPadInit`, `ui_init` (font parse), theme, `card/text_gpu_init`, `wave_init`, settings loads, capability logging | `main.cpp` | ~0.1–0.3 s | before the first frame; the screen is black anyway |
| decode the 256 px mark, upload 5 pre-filtered sizes | `boot_anim_begin` | tens of ms | inside DARK |
| `http_init` (net module load) | `main.cpp` | short, blocking | a single frame holds |
| update check (GitHub, HTTPS) | worker thread; main **waits** | ~0.5–1 s, bounded by 2 s timeouts | **animated**: the wait loop pumps frames |
| `vdec_reserve_mem`, `jbuf_reserve`, `img_arena`, `ps_sprites_preload`, `thumb_cache_init` | `main.cpp` | short, blocking | one frame between each |
| `load_config` | `main.cpp` | file read | — |
| `detect_tabs` (library list) | was: `ui_run_xmb`, render thread | 1 request, up to 6 tries if the network is late | **moved behind the mark** on a worker |
| five Home rows | was: one blocking request **per XMB frame** | ~5 requests | **moved behind the mark** on the same worker |

The two rows at the end are the only structural change to startup. Before
this change the XMB's first five frames each blocked on an HTTP request, so
the UI filled in one row at a time. That would also have made any animation
over those frames stutter. `xmb_prepare()` now runs both on a worker thread
(`boot_anim_run`) while the main thread only draws. It is safe because
nothing else touches `responseBuffer`, `g_tabs` or the Home rows until the
join. `ui_run_xmb()` then skips its own `detect_tabs` once. If no library
comes back (the server is not answering) the Home prefetch is skipped, and
the XMB loads rows itself, as before, with its existing background retry.

Nothing else moved. The update check still finishes before the big
reservations, because its HTTPS pools must be freed first. The reservations
still happen while the heap is pristine. `ps_sprites_preload` stays where it
was.

## Phases

`BootPhase` in `boot_seq.h`.

| phase | shows | leaves when |
|---|---|---|
| `DARK` | black | 200 ms |
| `EMBLEM` | the mark fades in (ease-in-out, 900 ms) and settles 94% → 100% scale; a halo in the mark's own purple follows it in | 900 ms |
| `AWAIT` | the mark holds; the halo **breathes** (60–100%, 3.2 s period) so the screen never looks frozen; after 4 s, "Connecting to server" fades in under it | the XMB is drawing frames **and** the mark has been fully visible 350 ms |
| `SHINE` | before anything moves, the mark catches the light for about a second: a thin glint streaks across it (a 128 px highlight, added over the mark and clipped to its shape) and a small four-point star twinkles on its apex, peaking as the glint crosses it; the halo swells with the twinkle | handoff clock |
| `EMERGE` | the XMB appears from under a black veil whose soft edge (60% of the screen tall, smootherstep) rises from the bottom: the wave first, then the shelves, the chrome last. It is **timed to the dock**: the shelves stay 70%+ dark while the mark is still over them and are fully shown once it is ~75% of the way to the corner | handoff clock |
| `DOCK` | the mark shrinks (geometrically, so the rate looks constant) and travels a gentle arc: it lifts, then glides **left** into the lockup | handoff clock |
| `WORDMARK` | each letter slides into place from up to 0.9 em left, fading in, with a ~4.5% spring (`easeOutBack`, c1 = 1.1), staggered 45 ms left to right | handoff clock |
| `DONE` | nothing: the static lockup is drawn by its usual code | — |
| `DISMISS` | not going to the XMB (first run, login, network init failure): the mark fades back to black from wherever it was | 450 ms |

SHINE, EMERGE, DOCK and WORDMARK are **overlapping windows on one clock** that starts
when AWAIT ends, so the handoff reads as one motion:

```
handoff ms   0      300  580   920 950        1770 1780         2475
SHINE        |===========|                         glint 300-950, twinkle 340-940
EMERGE                         |=================|  (850 ms)
DOCK                              |====================|  (900 ms)
WORDMARK                                        |============|
```

## Readiness signals

| signal | raised by | meaning |
|---|---|---|
| `BOOT_SIG_XMB` | `boot_anim_xmb_frame()`, every XMB frame | the XMB is drawing real frames under the veil |
| `BOOT_SIG_LEAVE` | `boot_anim_leave()` in `main.cpp` | startup goes to server URL / login / an error screen |
| `BOOT_SIG_SKIP` | any new button press in an XMB frame | jump to DONE (the press is swallowed) |

Readiness is not a guess. `BOOT_SIG_XMB` comes from the XMB loop itself,
which only starts once the library list and Home rows are loaded. So the
reveal never uncovers a half-built UI.

## Timing rules

- **The clock pauses rather than jumps.** Each frame advances it by the real
  frame time, clamped to 50 ms, so a frame that stalls on a blocking init
  step holds the animation instead of skipping part of a fade.
- **A minimum, no padding.** Before the XMB is ready the mark gets at least
  DARK + EMBLEM + HOLD = 1.45 s, which is about what cold init takes anyway
  (measured). If init is faster, the XMB is already running under the veil,
  and only its reveal waits for the rest of the 350 ms hold (a button press
  skips it). That is the only wait the animation adds, and it only happens
  on a faster boot than any seen so far.
- **A fixed handoff, and it never blocks.** 2.5 s after readiness (1 s of it the shine), with the
  XMB live underneath. Any press ends it at once. A press made *before* the
  XMB exists is discarded rather than stored, so it cannot throw away the
  reveal later.
- **Nothing wedges.** Every phase except AWAIT ends on time alone. AWAIT
  waits for the app's own readiness, which the app blocks on regardless;
  what it guarantees instead is a moving screen and, after 4 s, a status line.
  `test_boot_seq` runs 3,000 random interleavings of readiness, dismissal,
  skips and stalls (plus NaN and negative frame times) and asserts every one
  reaches DONE within a bound measured from readiness.
- **No pops.** The same test checks every visible quantity frame to frame at
  60 Hz and fails if any moves more than ~10% of its range in one frame (the
  pace of a 170 ms linear fade), across every phase boundary, a dismiss from
  any point, and the handoff from AWAIT.

## Rendering

It is cheaper than the XMB it covers, and it adds no new GPU technique:

- **Veil and fades**: blended black quad strips, inline vertices, through the
  wave's passthrough programs (the `wave_dim_screen` / hairline technique).
  The veil is at most 7 rows = 14 vertices.
- **Halo**: the existing `wave_draw_glow_gpu` fan.
- **The large mark**: one textured quad through the video passthrough
  programs, bound exactly as the player binds a frame (LINEAR, CLAMP), with
  premultiplied texels and `ONE, ONE_MINUS_SRC_ALPHA`. The texture is one of
  five pre-filtered sizes (256, 181, 128, 91, 64: a √2 manual mip chain,
  since linear-layout RSX textures take no hardware mips), so the sampler
  never minifies by more than 1.41:1 and the shrinking mark does not sparkle.
  VRAM: ~0.5 MB, freed at DONE.
- **Gated like every other RSX texture path.** The textured mark is used only
  when `jellyfin_gpucards.txt` or `jellyfin_gputext.txt` is already on, i.e.
  when RSX textures have been proven on this console. Otherwise the mark is
  drawn by the CPU. Over black it is write-only, which is cheap. Over the
  XMB it is blended after a fence, which is slow but only lasts the ~0.6 s
  the large mark crosses a revealed screen. The halo is skipped on that path.
- **The small mark and the wordmark are the lockup's own code.** Once the
  mark is within 1.6× its final size and the veil is gone, `xmb_draw_topbar()`
  draws it through `xmb_draw_mark()`, posed. The letters go through
  `drawTTF_ramp_posed()`, which is `drawTTF_ramp` with a per-letter pen offset
  and coverage scale. At rest it is **bit-identical** to `drawTTF_ramp`
  (test_text_runs phase 4: 3 sizes × 3 backgrounds × 4 pen positions, shadow
  included). There is one lockup, not a copy: the animation's last frame and
  the first static frame are the same calls with the same arguments.
- **Order in an XMB frame.** The XMB draws its complete frame as always. The
  top bar asks `boot_anim_lockup_pose()` which parts of the lockup exist yet
  and where. After `ui_text_gpu_flush()`, `boot_anim_xmb_overlay()` adds the
  veil and the large mark on top. JellyWave is untouched: `wave_draw()` runs
  where it always did, and the boot only draws after it.

Per-frame cost: pre-XMB frames are a clear, at most three quads and a fan,
roughly nothing. XMB frames add a veil strip and one quad, plus a fence and a
small CPU blit in the last part of the dock.

## Handoff to the XMB

```
main.cpp                                   ui_xmb.cpp (per frame)
--------                                   ----------------------
boot_anim_begin()      black, decode
  pump ... http_init ... pump
  update-check wait: pump every vsync
  reservations, pump between
load_config
boot_anim_run(xmb_prepare)  <- worker: detect_tabs + Home rows,
                               main thread pumps frames
show_main_menu() -> ui_run_xmb()
                                           poll_buttons()
                                           boot_anim_xmb_frame()  signal XMB,
                                             step, pose, eat input
                                           ...XMB draws, top bar reads pose...
                                           ui_text_gpu_flush()
                                           boot_anim_xmb_overlay()  veil + mark
                                           flip()
boot_anim_finish()     (after the XMB returns: release, idempotent)
```

The first XMB frame is identical to the last pumped one (a full-strength veil
over the XMB is black), so the switch from pumped frames to XMB frames cannot
be seen.

`boot_anim_leave()` runs before the server URL screen, before `do_login()` and
on the `http_init` failure path. It fades the mark out, and the screen that
follows draws itself exactly as it did before.

## Switches

- `/dev_hdd0/tmp/jellyfin_bootanim.txt`: `0` turns the animation off and
  brings back the old "Starting..." splash byte for byte. Missing or `1`
  means on. Read once at startup, so it applies on relaunch.
- The emulator build (`BUILD_FOR_RPCS3`) always runs with it off. Its
  CPU-composited background does not mix with GPU overlay quads.

## Previewing off-console

```bash
cd tools/bootanim_preview
cc -O2 -I../../source/ui/render -o dump_frames dump_frames.c -lm
./dump_frames 1400 33.3 > normal.json        # XMB ready at 1.4 s, 30 fps XMB
./dump_frames 7000 33.3 > slow.json          # slow network
./dump_frames 1400 33.3 2400 > skip.json     # button press mid-reveal
python3 mkpreview.py --frames normal=normal.json slow=slow.json skip=skip.json \
        --backdrop any_1080p_xmb_screenshot.png --out preview.html
```

The frames come from the real `boot_seq.h`. The mark and the Mata face come
from the repo's own headers. The backdrop is a still, and the halo and text
are drawn by the browser, so use it for timing and motion, not pixels.

## What only a TV can answer

- The textured mark's first bind on hardware: the path the player uses, but a
  new texture. Check `crash_log.txt` for `boot: ON (RSX mark)`, and the
  `boot:` lines in `player_log.txt`.
- Whether the veil's soft edge bands on a real panel (it is an 8-bit
  gradient over ~650 rows; the wave's dither unit is not enabled for it).
- Whether the shine reads as a glint and not a flash on a real panel, and
  whether 2.5 s of handoff feels deliberate or slow at TV distance. The
  constants are all at the top of `boot_seq.h`.
- The halo's 12-segment fan at 30% alpha: smooth enough, or faceted?
- 480p/576p framing: the layout is in fractions of the screen height, but
  pixel aspect is ignored, as the XMB's lockup ignores it.
