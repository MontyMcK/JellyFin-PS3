# The spine — implementation plan (2026-09-23)

Plan for README §2.9 "Depth model — the spine" from the 2026-09-21 export
(`design-import-v2/`, copied from `C:\NewDownloads\Jellyfin PS3 UI Revamp\`).
`UI-BRIEF.md`'s rules all apply. `UI-HANDOFF.md` (09-20) predates this export
and is stale; do not plan from it.

**Status (2026-09-23 evening):** S0 through spine6 are built, and spine7 — the
reusable depth engine every category now uses — is committed on
`feature/xmb-spine` in the WSL repo (worktree `~/jf-spine`). Nothing since
spine2 has been seen on a TV. Read "spine7" at the end first. Section 1 lists
the original decisions; each stage says which ones it waited on.

---

## 0. What the design actually says

Sources, in authority order: README §2.9 (it calls itself "source of truth"),
then the two mockups that carry the spine, `uploads/dirE.html` (L1 Home) and
`uploads/dirD.html` (L2). None of the five `.dc.html` canvases draw the spine —
they are unchanged from v1 apart from a stripped runtime script.

Three levels, one composition. Down descends, Up / ○ ascends, Left / Right
moves within the level. L1 names, L2 describes, L3 states every fact.

| constant | L1 Home | L2 Category | L3 Item |
| --- | --- | --- | --- |
| spine icon centre y | 232 | 141 | 74 |
| active icon | 64 | 56 | 48 |
| active label | 15 | 13.5 | 13 |
| spine glow alpha | 0.12 | 0.09 | 0.06 |
| header divider | not drawn | y=214 | y=144 |
| focused artwork | 128×192 | 200×300 | 216×324 + backdrop |
| title size | 30 | 36 | 25 |
| stride · active x | 200 · 230 | same | same |
| idle falloff | 0.62/0.46/0.33/0.22/0.14 | same | same |

- Idle icons 44×44, 1.4px stroke, `#39406a`. Active in accent purple, 1.16×
  scale, two-stage bloom, uppercase Satoshi label at 0.04em, 26×2 accent
  underline, over an elliptical glow band (dirE: `ellipse 28% 50%` of a 112px
  band centred on the active slot).
- The geometry never re-lays-out; it slides up and shrinks. One float animates
  0→1→2, ~280 ms slow-in/slow-out, no spring or overshoot.
- The item column swings from vertical (L1) to horizontal (L2). At L1 the
  column's lower half is drawn before the wave, so the ribbons pass over it
  ("sinking into the horizon"). Items tip further back as they descend.
- ✕ opens detail and never plays. Playback starts only from detail. △ plays
  the trailer. There is no separate △ overlay and no fourth level.
- L3 is the existing item detail screen wearing the depth-2 spine.

### Measured from the mockups (script, not eyeballed)

| | dirE (L1) | dirD (L2) | README §2.9 |
| --- | --- | --- | --- |
| icon centre y | 232 | 141 | 232 / 141 |
| stride | 128 | 128 | **200** |
| active x | 152 | 152 | **230** |
| idle icon | 26 | 26 | **44** |
| active icon | 36 | 32 | **64 / 56** |
| falloff | 0.62/0.46/0.33/0.22/0.14 | same | same |
| focused art | 128×192 at (88, 300) | — | 128×192 |

**The y-positions and falloff agree everywhere; the spine's SIZE does not.** The
README is 1.5–1.8× the mockups, and not uniformly. That is decision D1.

---

## 1. Decisions needed

| # | question | my default | blocks |
| --- | --- | --- | --- |
| D1 | Spine size: README (200/230/44/64) or mockups (128/152/26/36)? | README. It is the later, self-declared authority, and "too small on the TV" is this UI's recurring complaint. Both are one table in `spine.h`, so switching later is one edit. | S1 |
| D2 | L2 content: keep today's screens (Home shelves, library grid, sub-screens) under the depth-1 spine, or adopt dirD's focused item + facts + receding queue? | Keep today's screens for now; dirD's L2 becomes a later stage. It keeps README §8.1 ("no functional behaviour changes") true for as long as possible. | S1, S5 |
| D3 | ✕ opens / never plays, and △ = trailer. This IS a behaviour change, against §8.1. The client has no trailer support; Jellyfin's remote trailers are YouTube URLs the PS3 cannot play, so only local trailers could work. | Adopt ✕-opens. △ = local trailer when one exists, otherwise hidden from the hint bar (never shown disabled, matching the HUD's stats-hint rule). | S3 |
| D4 | The wave. The §2.9 table says bottom 190 / 228 / one third; the paragraph below it says "the wave does not move between levels". | Doesn't move — the paragraph explains *why* (it is the reference everything moves through). Treat the table row as a mistake. | S4 |
| D5 | Where do TV seasons → episodes, collection contents and music drill-down live, given "no fourth level"? | L2, as today, with the breadcrumb. The spine stays at depth-1 geometry. | S1 |
| D6 | Search and Settings at L1: what is their column? | No column; Down / ✕ goes straight to the L2 screen. | S4 |
| D7 | L1 needs a column for every library before it is visited. | Lazy: fetch on first focus at L1, the same as the tab fetch today, with the column drawn as skeletons until it lands. Prefetching all libraries at boot costs one request per library on a slow first launch. | S4 |
| D8 | "Tips further back as they descend": a perspective tilt. A trapezoid drawn as two textured triangles warps affinely (visible seam), and §4.2 bans 3D tilt on focus. | Approximate with scale + alpha falloff down the column. Revisit only if it reads flat on a TV. | S4 |

README §3.1 and §8.3 Phase 2 still specify the tab strip (stride 110, icon 26,
label active-only). §2.9 supersedes them but was not applied to them. The
upstream README should be fixed in Claude Design so the next export does not
reopen this.

---

## 2. Architecture

**`source/ui/render/spine.h` — header-only pure C, host-tested.** The same
pattern as `wave_gel.h`: all geometry, no drawing.

- `spine_level_t` holds the per-level triples above as a table, in 1280×720
  authoring units. It is the ONE place D1 is answered.
- `spine_ease(t)` is slow-in/slow-out. Use smoothstep or cubic in-out; no
  overshoot, so the output is monotonic and clamped to [0, 1].
- `spine_eval(depth_f, &out)` lerps every triple at a float depth in [0, 2].
  The "not drawn" divider at L1 becomes alpha 0, not a special case.
- `spine_slot_x(i, active_i)` = `active_x + (i - active_i) * stride`. Slots left
  of the active one can go negative (the row is cropped at the left edge by
  design).
- `spine_idle_alpha(dist)` = the 5-step falloff; 0 beyond distance 5.

**`source/ui/render/ui_spine.cpp` — the draw code.** It replaces
`xmb_draw_tabs()` when the gate is on.

- **GPU phase (before `rsxSync`):** the glow band and the two-stage bloom, as
  blended fans. Reuse `wave_draw_glow_gpu()` from the 09-22 WIP (saved in
  `outputs/wip-2026-09-22-full.patch`), widened to separate `rx`/`ry` for the
  ellipse. 14 vertices per fan, so three fans cost nothing.
- **Text phase:** icons via `drawIcon()` and the active label via
  `drawTTF_tracked()`. Both are cached runs, so a settled spine uploads nothing.
- The underline is an opaque `drawRect` (write-only, fine).
- Everything goes through `UIS_W` / `UIS_H` / `UIS_TF` plus `XMB_OX` / `XMB_OY`.
  `test_layout`'s `test_one_scale()` must cover the new constants.

**Navigation state:** one `int g_depth` target (0..2) and one `float g_depth_f`
that eases toward it. Every consumer reads `g_depth_f`, never a per-screen
flag. `xmb_nav_depth()` from the WIP is the seed of this, but it conflates
"inside a TV series" with "at L2". Under D5 those are different things.

**Chrome anchors become depth-driven.** `XMB_DIVIDER_Y` / `XMB_CONTENT_Y` are
used in 7 files (21 uses). Make them read the evaluated spine rather than
constants. `test_layout` must then assert the fit at each level, not once.
**Consequence to flag early:** at L2 the divider moves from 144 to 214, which
takes 70px (at 720) from the 2-row library grid, so cards shrink by ~20%. If
that reads badly, it is an argument for dirD's single-row L2 (D2).

**Gate:** `/dev_hdd0/tmp/jellyfin_spine.txt`, read in `main.cpp` after
`plog_load_setting()`. 0 or missing = the current tab strip, 1 = the spine. Pair
the plog with a `crash_log()`. The spine binds no new GPU state (fans on the
existing wave program), so a bad value costs a relaunch, not a power cycle.
It is still gated because it changes every screen.

---

## 3. Stages

Each stage ships alone, gated, and is measured against the `xmb:` line
(`bpx` must not move; `tm`/`tkb` must settle to 0).

**S0 — DONE 2026-09-23.** `source/ui/render/spine.h` + `tests/test_spine.c`
(in `Makefile.host`'s `all`; on this PC run it with `mingw32-make -B -f
Makefile.host test_spine`, then `./test_spine.exe`). Passes with `-Wextra`,
compiles as C++, and four planted faults (a table typo, a removed clearance
factor, an overshooting ease, a swapped falloff) each fail it. It found two
things:

- **The divider cut through the label on L1→L2.** It fades in at y=214 while
  the underline is still sliding up past that line. Its alpha is now scaled by
  the underline's clearance, so it first shows at depth 0.913 and settled
  screens are unchanged.
- **The L1 poster moves down in S4.** With the README-size spine, the L1
  underline ends at y≈303, and dirE's poster top is y=300 (drawn for its
  smaller spine). Put the L1 art at ≥ ~311.

Label and underline offsets and the glow ellipse came from dirD by script,
because the README omits them.

**S0 — geometry only, host (original brief).** Write `spine.h` + `tests/test_spine.c`:

- triples at exactly 0/1/2 equal the table;
- easing is monotonic with no overshoot;
- the falloff table is right;
- all 7 slots are on-screen at 720p and 1080p for D1's numbers.

No console. *Waits on D1; can start with the README numbers.*

**S1 — BUILT + DEPLOYED 2026-09-23, awaiting a TV check.**

- **Files:** `render/ui_spine.{h,cpp}`. `drawIconA()` in `ui_text.cpp` gives
  real icon opacity through the run colour's top byte (0 = unchanged for every
  existing caller). `wave_draw_glow_gpu()` now takes separate rx/ry.
  `XMB_DIVIDER_Y` is gated in `ui_visuals.h`.
- **Idle colour:** derived as 25% `hairline` → `icon_idle`, not hard-coded.
- **Hints:** unchanged. ✕ still does what it did, and there is no options
  cross yet, so §2.9's `✕ Open · ▫ Options` would describe buttons that do
  not exist.
- **The bloom:** not measured, because the design gives no numbers. It is two
  round fans (radius 1.0 and 0.625 × the active icon, alpha 36 and 64);
  judge it on the TV.
- **test_layout:** gained a spine pass. It reports the design's cost as notes:
  2 list rows (not 3) with overscan at 720p/480p, the farthest slot skipped
  under overscan, and the left neighbour skipped at 576i.
- **Build:** 109 objects, 48 warnings (none new).
- **Deployed:** `outputs/EBOOT.BIN.spine1` (sha 7be16ba3…), with
  `jellyfin_spine.txt=1`. Rollback: the gate to 0, or
  `outputs/EBOOT.BIN.pre-spine1`.
- **Not committed:** the glow function is interleaved in `ui_wave.cpp` with
  the untested JellyWave safety work, and that deploys with it.

**S1 on hardware (2026-09-23):** the spine renders. The user's verdict: "looks
better, but it is still not multiple layers". That moved the base layer forward.

**Base layer (S4, first cut) — BUILT + DEPLOYED 2026-09-23 as
`outputs/EBOOT.BIN.spine2`, awaiting a TV check.** The navigation follows
1etu/XMP (MIT, `packages/xmb-plugin/src/nav.ts`):

- **At base:** Left/Right and L1/R1 walk the tabs (clamped, not wrapped).
  Down or ✕ enters a tab.
- **Inside a tab:** Up from its top row, or ○ at its root, returns to base.
  Search leaves on Up from the keyboard's top row only, because ○ already
  means "clear" there. Sub-screens keep their own Up and ○.
- **Drawing at base:** the spine at y=232, no divider, and a column under the
  active icon. The focused poster is 128×192 at y=318, not dirE's 300. The
  next item is shown at 0.72× and dimmed by a GPU rect. The title block
  (eyebrow, 30 px display title, meta) sits beside the poster.
- **Motion:** the depth is latched once per frame. The spine moves over
  280 ms, content cuts at depth 0.5, and the divider fades in once the label
  has cleared it.
- **Not done:** the column sinking behind the wave (the wave's first draw is
  an opaque full-screen gradient), scrolling the column at base, and the
  L2→L3 item level.

**spine2 on the TV:** the user reported base-layer posters never loading
(they load in L2), motion "way too static", the A–Z rail tiny, and the whole
thing not elegant.

**spine3 — BUILT, NOT DEPLOYED (the user said to hold).**
`outputs/EBOOT.BIN.spine3`, sha ab94eb52….

- **Motion:** the XMB's approach curve (`spine_approach_*` in spine.h, gains
  from XMP's verified table, frame-rate independent, host-tested at 60, 36 and
  20 fps). The row position glides in slots; icon size, colour, falloff,
  labels and underline are continuous in the fractional distance. The depth
  approaches over 320 ms. The column slides and fades with its category and
  lifts out on entering a tab.
- **Posters:** the column now requests the tab's OWN grid or Home-row card
  size, and draws it scaled with a new LINEAR-filtered
  `ui_card_gpu_draw_scaled`. The two levels share cache slots, so L1 loads
  exactly when L2 does. The root cause of spine2's misses is not proven: the
  size budget (~67k px), the VRAM slot size and the fetch buffer all check
  out, and the spine2 log was overwritten.
- **A–Z rail:** fixed pitch (13 px type on a 17 px pitch, README 3.3 plus one
  step) instead of two grid rows. Colours follow README 3.3 (idle
  `text_faint`, current section `text`, jump cursor `accent_alt`); every
  letter used to be `hairline`.
- **Still static:** L2 content cuts in at depth 0.5, and focus movement
  inside the grid and Home shelves snaps (README 4.1 asks for 120 ms).

**spine4 — BUILT, NOT DEPLOYED.** `outputs/EBOOT.BIN.spine4`, sha
c66907d1…, 48 warnings. It fixes the two "still static" items above. All of it
is inert with the gate off.

- **Content glide:** `spine_content_dy()`. The tab's content rises the last
  48 px into place over the second half of a level move. `XMB_CONTENT_Y` is
  the drawn position; `XMB_CONTENT_Y_REST` / `XMB_GRID_Y0_REST` feed every
  SIZE (card height, visible rows, grid space, Home card height, the A–Z
  pitch), so nothing resizes and no thumbnail is refetched mid-glide.
  `test_layout test_glide_sizes()` guards that.
- **Focus ring:** `spine_focus_ring_gpu()` replaces `ui_card_gpu_selection`
  in the grid and on Home. It approaches the focused card's drawn rect (200
  ms, the XMB value), and snaps when the screen changes or a frame was
  missed.
- **Grid scroll:** `spine_grid_motion()` plus a draw window in `ui_lists.cpp`
  (`xmb_grid_motion`). Rows ease up and down under a clip to the grid band.
  The scrollbar keeps the logical position. A page slide (a jump bigger
  than a screen) snaps.
- **Home scroll:** eased copies of `s_vscroll` and each row's `scroll`. A
  card sliding off an edge keeps its GPU image, while its CPU rects and text
  drop the moment it is partly off screen, because they take unsigned x.

**spine5 — BUILT, NOT DEPLOYED.** `outputs/EBOOT.BIN.spine5`, sha
7c49ebf5…, 48 warnings. It works from the CURRENT canvas: `design-import-v3/`,
measured in `design-import-v3/MEASURED-detail-hud.md`. Everything is behind
the spine gate; with the gate off, the old detail page and HUD run unchanged.

- **A–Z rail:** with the spine on, it is a 15-letter window centred on the
  current letter, at 17/24 px authored. The window glides and fades toward
  its ends. This is a deliberate departure from the static 27-row rail,
  because under the spine the static rail is capped at ~19 px letters.
- **Item detail (L3):** `xmb_show_item_info_v3` in `ui_info.cpp`.
  - The backdrop and poster are uploaded to VRAM once
    (`ui_gpu_tex_upload`, card-gpu slots 0 and 1) and drawn scaled, LINEAR.
    The scrims are GPU ramps (`wave_draw_ramp_gpu`); chips, buttons and
    selectors are rounded fans (`wave_draw_rrect*_gpu`).
  - All text goes through the run cache, and the focus ring glides between
    controls.
  - Controls: Resume / Play from start / Mark as watched (a new POST to
    `/Users/{u}/PlayedItems/{id}`), Version, Quality.
  - The spine rises to L3 on entry and returns to L2 on exit.
  - Dropped relative to the old page: More Like This, the tagline and the
    facts rows. The canvas has none of them.
- **Player HUD:** `hud_body_v3` in `player/hud/hud_v3.inc`, on the same
  compose-on-change overlay. It has the scrim, the 30 px title, audio / vol /
  subs chips (which are also the old focus slots), the full-width progress
  bar with times, the audio-path pill at top right, and the hints.
  - New buttons with the gate on: △ Tracks, ○ Stop (new `HUD_ACTION_STOP`),
    □ Stats (toggles `statsovl`).
- **Not done:** Home's L2 "queue recedes" layout (00 · Depth model, L2
  frame), and the L1 column's tipped items drawn before the wave. (Home's L2
  queue is done in spine6.)

**spine6 — BUILT, NOT DEPLOYED.** `outputs/EBOOT.BIN.spine6`, sha
91f830e8…, 111 objects, 48 warnings (a clean rebuild), header 0480. This is
spine5 plus Home's L2 queue. Host tests pass: test_facts (new), test_spine,
test_layout and test_media_sources.

- **Home stage** (`xmb/ui_home_stage.inc`, included by `ui_home.cpp`). With
  the gate on, Home is ONE stage drawn at every depth. Each item's box is
  the depth-weighted blend of its L1 column slot and its L2 queue slot, so
  the column swings into the queue (the canvas: "swings from vertical to
  horizontal rather than cross-fading").
  - Canvas numbers: the focus 200×300 at x=64 with a 46 px reflection; the
    queue at 104/92/80/70 wide at .62/.48/.36/.26 on y≈552; the facts column
    520 wide at x=304 (eyebrow + "3 of 12", 36 px title, meta, CONTAINER /
    VIDEO / SUBTITLES, AUDIO chip + "+ other track", progress, "WATCHED ·
    LEFT").
  - Motion: Left/Right slides the queue, and Up/Down changes category with a
    vertical glide. Each row remembers its position. Empty rows are skipped.
- **Decided here:**
  - Posters in every row, with episodes shown by their SERIES poster and
    name ("S1 E4 · name").
  - The halo follows the item nearest the focus.
  - The facts fade while the queue moves.
  - Square rows are 1.2× wide, and the queue shifts right to clear the
    facts.
  - Coming back from detail, the queue settles 40 px into place.
- **True translucency:** `ui_card_gpu_draw_ex`, with constant-alpha blending
  via `rsxSetBlendColor` (NEW RSX STATE, unproven on hardware), plus UV
  flips for reflections, drawn as 6 strips.
- **Facts worker** (`api/api_facts.cpp` + `api/facts_parse.cpp`, host-tested):
  - fetches `Items?Ids=…&Fields=MediaStreams` on its own thread and its own
    buffer, 12-entry cache;
  - words: "DTS-HD MA 5.1", "TRUEHD ATMOS 7.1", "4K HEVC HDR10", "PGS ENG";
  - also feeds the detail page's audio / video chips, whose blue outline is
    now lossless-only.
- **Thumbnail slots** grow to fit the queue's source size
  (`xmb_queue_src`), capped at 120 k px (about +6% over the ~113 k px a 1080p slot holds today).
- **X is detail's only door (the canvas's Resolved box):**
  - X on the base layer opens the column's item (Down still enters);
  - X / △ in the queue open detail;
  - detail hands back the level it was opened from.
- **Home no longer refetches** Continue Watching / Next Up every time the
  spine passes over it. It refetches only when `g_play_gen` changes (a
  playback start or Mark as watched).

**S1 — the spine at L2, static (original brief).** Every existing screen draws the depth-1 spine
instead of the tab strip:

- divider at 214 and content anchors moved;
- glow band + bloom;
- hints per §2.9 (`✚ Nav · ✕ Open · ▫ Options · ○ Back`), but ✕ keeps its
  current meaning until S3, so the ✕ label stays "Select" until then.

This is the first thing to see on a TV. *Waits on D1, D2, D5.*

**S2 — L3 in item detail, cut.** `xmb_show_item_info()` runs its OWN frame loop
(own `waitflip`/`flip`), so it must draw the depth-2 spine and the divider at
144 itself, in its own GPU and text phases. Transitions into and out of it cut
for now. Check that the detail layout (poster top 172) clears a spine at y=74.

**S3 — ✕ opens, never plays.**

- Every grid/shelf ✕ that calls `xmb_play_*` (`ui_home.cpp:401/403`,
  `ui_nav.cpp` TV episodes ~278, collections ~348) opens detail instead.
- △ becomes trailer-or-nothing.
- Detail's own ✕ keeps playing.

This is a behaviour change: its own commit, done after S2 so detail already
looks right. *Waits on D3.*

**S4 — L1, the new screen.**

- The spine at y=232 as content, no divider.
- The vertical column under the active slot, with the focused art 128×192 at
  the intersection, fixed. The column slides underneath it.
- The label block beside it: eyebrow + title 30 + meta.
- Left/Right changes category, Down → L2, ✕ → detail.

Draw order changes: the column's lower half must be issued BEFORE `wave_draw()`
so the ribbons pass over it. Today `wave_draw()` runs straight after
`clearScreen()`, ahead of the whole GPU phase. That is a real reordering of a
load-bearing sequence; do it behind the gate and check that `sync` does not
move. *Waits on D4, D6, D7, D8.*

**S5 — transitions.**

- `g_depth_f` eases over 280 ms.
- The spine slides and shrinks.
- L1→L2 swings each card's rect from its column slot to its grid/shelf slot, as
  a lerp of card quads on the GPU.

Risk: animating type sizes makes every in-between frame a text-cache miss
(runs are keyed on size). Measure `tm`/`tkb` during a transition. If uploads
spike, draw text at the endpoint size and cross-fade instead of scaling it.
L2↔L3 stays a cut until detail is folded into the main loop, which is a separate
refactor and not part of this plan.

**Later, not planned here:** dirD's L2 layout (if D2 says so), and folding
detail into the main loop so L2↔L3 can animate.

---

## 4. Budget

The spine itself is cheap:

| item | cost |
| --- | --- |
| icons + label | 8 cached runs |
| glows | 3 fans × 14 verts |
| underline | 1 opaque rect |

It adds nothing meaningful to a frame. The real frame problem is elsewhere:
JellyWave mode 3 measured 19.7–22.5 ms on hardware (7.4 ms of PPU geometry per
rebuild, amortised by `jellyfin_jwrebuild.txt`). Measure spine stages against
**mode 2**, so their cost is not buried in the wave's.

The only spine stages with real risk are:

- **S4:** the draw reordering;
- **S5:** text-cache churn during a transition.

---

## 5. Housekeeping before S1

- The 09-22 working tree mixes two things. The JellyWave safety/upload work
  (`ui_wave.cpp`, `wave_gel.h`, `test_wave_gel.c`) is consistent with the new
  ribbon's "alpha blend only" and should be committed on its own once built. The
  tab-strip WIP (`ui_widgets.cpp`, `ui_xmb.cpp`, `ui_visuals.h`) matches neither
  the old strip nor the spine and should be restored to HEAD. Its glow function
  lives in `ui_wave.cpp` and survives. Full backup:
  `outputs/wip-2026-09-22-full.patch`.
- Rewrite or retire `UI-HANDOFF.md` so the next session does not plan from it.

---

## spine7 — the depth engine (2026-09-23), BUILT, NOT DEPLOYED

**Where it lives now.** Everything above spine1 was only ever an uncommitted
working tree in the OneDrive fork (branch `feature/spine6-strobe-integrated`),
built on 3e86481 without the 50 JellyWave strobe commits. It is now committed
on **`feature/xmb-spine`** in the WSL repo, worktree **`~/jf-spine`**, based on
428d54a (the strobe line, same base as `feature/cold-boot-animation`):

| commit | what |
| --- | --- |
| 7f7b903 | S0 (`spine.h`, `test_spine.c`), cherry-picked |
| 3cbab08 | spine1–6 ported unchanged in behaviour |
| f95e618 | the depth engine (this section) |

The OneDrive working tree was not modified. **The JellyWave renderer is not
touched**: spine6's own 09-22 JellyWave safety edits (`ui_wave.cpp`
`jw_upload`/`s_jw_stage`, `wave_gel.h` sanitize, `test_wave_gel.c`) were left
out, and the strobe line's renderer stands. The spine's immediate-mode helpers
(elliptical glow, rounded rect, alpha ramp) moved to a new
`render/ui_wave_panels.cpp`; `ui_wave.cpp` only gained `wave_imm_bind()`
appended at the end of the file — the program + blend preamble its own divider
and glow already use. No vertex buffer, upload, fence, draw count, primitive or
blend-state change to the wave.

### Audit of spine6 (what was real, what was placeholder)

| piece | state found |
| --- | --- |
| `spine.h` level table, eval, approach motion, falloff | real, host-tested |
| spine row, glow band, bloom, labels, underline | real |
| level motion, content glide, focus-ring glide, grid scroll easing | real |
| Home stage (column → queue swing, facts, reflections, translucency) | real, but its geometry and state were Home-only file statics in `ui_home_stage.inc`, untested |
| library L1 column | **placeholder**: two opaque posters, always the library's FIRST item, a dimming rect instead of opacity, cut to the grid at depth 0.5 |
| per-category focus | Home rows only; `xmb_switch_tab()` zeroed every library's selection |
| L2 → L3 | the level move started before detail's blocking fetch, so its first frame carried the whole fetch as dt and the spine landed in one step |

### What the engine is

**`render/depth.h`** — pure C, host-tested, no display/theme/RSX/allocation.
Every item is placed by its distance k from the category's focus; every layout
is `k -> depth_box` (position, size, opacity, veil, reflection):

- `depth_column_slot` — L1 column (128×192, 106×159, 86×129, colder veils).
- `depth_queue_slot` — Home's L2 queue (spine6's numbers, unchanged).
- `depth_grid_cell` — a library's L2 grid cell, exactly as `grid_cell_pos()`.
- the swing: `depth_column_queue_box`, `depth_column_grid_box`, `depth_swing`.
- `depth_rows` — the category glide; `depth_step_visitable` /
  `depth_nearest_visitable` / `depth_at_top` — empty categories skipped.
- `depth_focus_mem` — one fixed slot per category, clamped on restore.
- fades: `depth_l1_label_alpha`, `depth_l2_facts_alpha`, `depth_over_l2`,
  `depth_queue_settle`, `depth_halo`; layouts: `depth_l1_label_layout`,
  `depth_facts_layout`; `depth_mix_q` (16-step colour, run-cache friendly);
  `depth_xform` (matches UIS_W/UIS_H + overscan); `depth_refl_clamp`.

**`render/ui_depth.cpp`** — one stage renderer. A category hands it an index
range and a callback (index → `DepthCard`); it draws halo glow behind, cards
far to near with veil + reflection, bands + progress over, and notes the focus
rect. Only existing primitives: `ui_card_gpu_draw_ex` (constant alpha),
`ui_rect_gpu_draw`, the immediate-mode fans. No VRAM reads, no allocation (the
card list is a fixed stack array), no vertex buffers.

### What uses it

- **Home** (`xmb/ui_home_stage.inc`): now resolves its cards through the
  engine. `test_depth` carries spine6's `q1_slot`/`q2_slot`/`q_box` verbatim
  and asserts the engine returns the identical box for every slot, shape and
  swing — the move changed no geometry.
- **Movies / Shows / Music / Collections / Playlists**
  (`xmb/ui_depth_lib.cpp`, new): Home's L1 column (remembered focus + two
  receding items, translucent, halo, label block) and a **continuous swing
  into their real grid cells**; the rest of the page unfolds out of the column.
  At `DEPTH_HANDOFF_E` (0.999: every card within ~1 px of its cell at 1080p)
  the grid takes the screen back and draws exactly as before — titles,
  scrollbar, A–Z, paging, focus ring. Going up, the stage takes over on the
  first frame the swing leaves 1. The content glide is off for these tabs
  (`depth_stage_tab`): the swing is their motion. Sub-screens (seasons,
  collection contents, music drill-down) swing the same way.
- **Decided: libraries keep the grid at L2** (the canvas's "02 · Library tab"),
  not Home's queue — the grid carries paging, A–Z and sort for large libraries.
  The engine makes this one callback, so a queue L2 for libraries later is a
  layout swap, not a new screen.
- **Focus memory**: `xmb_switch_tab()` calls `spine_focus_leave/enter`; each
  library remembers `g_sel`/`g_scroll_top` (forgotten when its paged window or
  filter is dropped). X on the base layer opens that item (was item 1).
- **Thumbnails at L1**: the column's three always; the rest of the page only
  once the row has settled on the category or the swing has started — walking
  the spine costs three fetches per library, not a page.
- **L2 → L3**: detail starts the level move after its first fetch
  (`spine_clock_reset`), flies its poster out of the card it was opened from
  (`depth_last_focus_rect`, noted by the stage or the focus ring) and fades its
  backdrop in (`ui_gpu_tex_draw_a`). Closing hands back the level it came from;
  Home's queue settles 40 px, a library returns to its grid with its focus.

**Found and fixed by the new test_layout depth pass:** under 8% overscan at
720p and 480p, spine6's queue-focus reflection ran up to 30 px into the hints
bar (the stage is pushed down by the inset while the hints bar is pushed up).
Reflections now end at the hints bar; nothing changes without overscan.

### Tests and build

- `test_depth` (new): 6,360 checks — spine6 equivalence, swing endpoints, grid
  hand-over ≤ 1.5 px at 1080p, fit at 720p/1080p, rows, skip-empty, focus
  memory, fades, colour quantisation, transform, reflection floor.
- `test_layout`: new depth pass through the real `ui_visuals.h` anchors at 7
  resolutions/overscans.
- Host suite: **32/32 pass** after `make -B`. `make check` itself cannot
  succeed on this branch **or its base**: `test_bg_gradient.c`,
  `test_month_bg.c` and `month_bg_table.h` are named by `Makefile.host` but
  were never committed anywhere. Use `~/run_host_spine.sh` (runs each test on
  its own). `check` now also runs `test_spine`, `test_depth`, `test_facts`.
- PS3 clean serial build: **115 objects, 48 warnings** — the pre-existing
  census, none in new or edited files; new files also clean under `-Wextra`
  with the real PSL1GHT headers. NPDRM header `0480`. **Not deployed.**
  Staged: `outputs/EBOOT.BIN.spine7` (sha 77196770...) and
  `outputs/JellyFin-PS3-spine7.pkg`, built clean from 9976952. Needs
  `jellyfin_spine.txt` = 1; 0 or missing is the strobe line exactly.

### Needs a TV (nothing from spine3 on has been seen on hardware)

1. **Constant-alpha card blending** (`rsxSetBlendColor`, spine6): every
   translucent poster, veil-free fade and reflection depends on it. New RSX
   state, unproven.
2. The library **L1 column and swing**, and the **hand-over**: titles,
   scrollbar and A–Z appear at the hand-over rather than fading — judge it.
3. **Frame cost** of the swing (up to ~16 translucent quads + halo) against the
   `xmb:` line — `bpx` must not move, `tm`/`tkb` settle to 0. Measure against
   JellyWave mode 2.
4. **Focus memory** across Left/Right at L1 and L1/R1 at L2.
5. The **detail arrival**: poster flight timing, backdrop fade.
6. Overscan: the shortened reflection at 720p/480p.
7. Everything spine3–6 already listed (approach feel, A–Z window, v3 detail,
   v3 HUD, facts worker).

### Still open

- Tipped L1 items (11° / 19°) — scale + veil stand in (D8).
- The column drawn BEFORE the wave — blocked while JellyWave's strobe work is
  open (reordering its first draw is off limits).
- Grid chrome (titles, scrollbar, A–Z) fading in at the hand-over instead of
  appearing.
- Detail still runs its own frame loop; folding it into the main loop is what
  would let L3's text arrive with its poster.
- Search and Settings have no column (D6); the ▫ Options cross does not exist.
- Library L2 as a queue, if the grid reads wrong under the spine (engine-ready).
- Merging: `feature/cold-boot-animation` hooks `ui_run_xmb` and
  `xmb_draw_topbar`; this branch changed the three XMB phase functions and made
  `xmb_grid_view()` public. The strobe session's uncommitted `ui_wave.cpp`
  overlaps only at end-of-file (`wave_imm_bind`).

---

## 2026-09-24 — playback regression and screen sizing (cd662dd), BUILT, NOT DEPLOYED

Two separate investigations; neither was caused by the other.

### Playback: not the UI — the quality step did not include its audio

Evidence, read together: the console's `player_log.txt` (five 1080p "25 Mbps"
sessions, a spine-on mode-2 build) and the Jellyfin server's own ffmpeg logs
on this PC (`C:\ProgramData\Jellyfin\Server\log`, PS3 clock ≈ PC + 5 h 34 m).

| | observed |
| --- | --- |
| server request | `-b:v 25000000 -maxrate 25000000`, **DTS-HD MA 5.1 copied** on top |
| server output | averaged **27.6 Mbps** (peak 28.5); ffmpeg ran at 16–18× realtime |
| console receive | ~25 Mbps (the known ceiling), `rxw` 70–99% (blocked in netRecv) |
| read-ahead ring | 34 MB, draining ~3 Mbps: 186k → 56k packets in a minute |
| picture | a flat 24.0 fps whenever the ring held data; stalls only at `ring=0` |

- `source/player`, `net`, `video`, `audio`, `api`: **zero changes** between the
  pre-JellyWave build (4c1a63b) and the strobe base; the spine adds only HUD
  buttons. Static memory grew ~0.5 MB; the ring was 34 MB (bigger than the
  09-18 known-good 24 MB), so heap was not the constraint.
- Nothing from the XMB, spine, depth stage or wave runs per frame during video:
  the player loop draws video + HUD only. The facts worker is a 25 Hz one-byte
  poll. The audio-reactive tap is fed by the music player only (`lvl=` in the
  video heartbeat is the player's own meters).
- Later sessions dropped to ~1 Mbps with `rxw` 99% while the progress POSTs to
  the same server also failed (`http=-1`). That is delivery, not the console
  being busy. It was not reproduced.

**Fix:** `player/core/stream_budget.h`. A step now means the whole stream: the
audio (its `BitRate`, or for a copied lossless track at least 3.0 Mbps DTS-HD
/ 4.5 Mbps TrueHD, because Jellyfin reports only the DTS core) plus ~3% TS
framing come out of it. For the measured case, video 21.25 Mbps and ~23.9 on
the wire instead of ~27.7. "Original" is still uncapped. Logged as
`stream budget: ...`. **`jellyfin_abudget.txt` = 0 puts the old request back**
for an A/B.

### Screen size: the canvas was scaled to the display, then translated

`uis_w/uis_h/uis_tf` scaled the 1280×720 canvas to the **whole** framebuffer,
and `XMB_OX/OY` only added the inset. The left/top edges moved in while the
right/bottom edges moved **out** by the same amount, so no calibration could fit.
Everything drawn at an authored position overflowed right/bottom by one inset:
the spine, the depth stage, item detail, and the redesigned player HUD (`v3x()`).
Pad-anchored chrome (`XMB_ITEM_PAD`, `XMB_BOTTOM_PAD`) did not overflow, which
put two geometries on one screen.

**Model now:** physical framebuffer → safe rect (framebuffer minus the inset on
every edge) → the logical 1280×720 canvas scaled into it, geometry **and** type.
There is one transform (`ui_visuals.h` "THE SAFE AREA"), and `depth_xform_make`
matches it. With no calibration it is integer-identical to before. Video and
full-bleed backdrops stay full-bleed. The old raw-pixel HUD and the boot/auth
screens keep their own symmetric insets.

`test_layout` `test_safe_area()`: 1080p/720p/576i/480p × 0/2/2.5/3/5/8%.
Margins are symmetric and the canvas centre is the screen centre. An authored
edge and a pad-anchored edge land on the same pixel. The HUD seek bar is
symmetric, and the depth transform matches UIS. The earlier overscan-only
artefacts (shortened reflections, skipped spine slot, 2-row lists) are gone.
The console's calibration file reads `30` (3.0%).

### Tests / build

Host suite 33/33 (new `test_stream_budget`, a `BitRate` fixture in
`test_media_sources`, `test_safe_area`). PS3 clean serial build: 115 objects,
48 warnings (unchanged census), header `0480`. Staged: `outputs/EBOOT.BIN.spine8`.

### Needs the console

1. Play the same film at 25 Mbps with Surround HD. The log should show
   `stream budget: step=25000000 video=21250000 audio=3000000 (dts...)`, the
   ring should stay above 0, and the server's ffmpeg output should average
   ≤ ~24 Mbps. A/B with `jellyfin_abudget.txt` = 0.
2. Re-calibrate overscan once. Everything now shrinks into the safe rect, so the
   value that fits is the true one (probably lower than the 2–3% used to
   compensate). Check the XMB, spine, detail page and the player HUD at that value.
3. The 1 Mbps sessions: if they recur, check the network and server at the time
   (both connections failed together).

---

## 2026-09-24 — the experience pass, BUILT (host-checked), NOT DEPLOYED

A UI polish pass on top of spine8. Everything is behind `jellyfin_spine.txt`
= 1; with the gate off, every screen below is exactly as it was. **Not
touched:** the JellyWave renderer (`ui_wave.cpp` has no change;
`ui_wave.h` gained declarations only), its vertex buffers, `jw_upload()`, RSX
ownership and fences, the audio-reactive wave (`ui_wave_audio.*`,
`wave_audio.h`), the music DSP (`music_fft.*`), the 24p work, and the player's
decode, audio, network and display paths.

**Dropped by request:** picture-frame pause mode and welcome-back. Focus
memory is unchanged.

### New pure-C cores (host-tested: `tests/test_experience.c`, 5,920 checks)

| file | what |
| --- | --- |
| `render/art_colour.h` | artwork accent extraction and a 24-entry LRU cache |
| `render/buffer_anim.h` | the buffering presentation's state machine and every animated value |
| `render/experience.h` | the version selector's words (`vpick_*`), the ambient screensaver's state and drift (`amb_*`) |
| `render/jf_logo_geom.h` | the Jellyfin mark, tessellated by `tools/gen_jf_logo_geom.py` from the official SVG paths |

### 1. Buffering screen (`render/ui_buffering.{h,cpp}`, `player/core/player.cpp`)

The old startup was a series of text screens: the item name plus
"Initializing decoder…", "Connecting…", "Buffering 47% (O: start now)". With
the gate on, it is now one continuous presentation from Play to the first frame:

- **Layers:** the detail page's backdrop, full screen and darkened. If there
  is no backdrop, its poster cropped to a wide band; if neither, a pool of the
  accent's deep shade. Then a black veil ramp, a breathing glow, a thin ring
  track, and a gradient arc turning slowly (one turn per 2.6 s) in
  Jellyfin's own colours: #AA5CC3 purple at the transparent tail to #00A4DC
  blue at the head. Closed, it is purple → blue → purple round its two
  halves, so there is no seam. At the centre is the Jellyfin mark, graphic
  only. The eyebrow reads PREPARING, CONNECTING,
  "Waiting for the server · 12s", then BUFFERING with the percentage under it.
  The title sits bottom-left, and the O hint shows Start now or Cancel.
- **Motion:**
  - The arc grows with the read-ahead ring's fill, and the displayed
    percentage eases (220 ms) and never decreases.
  - When ready, the arc closes (220 ms), the rotation decelerates to rest with
    no jolt, the mark pulses once (+7%), and everything fades (250 ms).
  - The loading phase lasts exactly as long as the player loads. An almost
    instant ready is held to 450 ms counted from the first frame, so there is
    no one-frame flash; a slow start adds nothing.
  - A cancel or error fades in 180 ms before the error screen.
- **Artwork:** reused only when `ui_gpu_tex` is tagged with THIS item's id.
  Detail now tags both slots. After an auto-advance to the next episode the
  slots still hold the previous episode's art, so that start gets the plain
  background, never the wrong picture.
- **Cost:** GPU only, from what is already in VRAM. Per frame that is one
  textured quad, three ramp stops, two glow fans, two ring strips (≤ 340
  vertices) and the mark (244 vertices, immediate mode), then one `rsxSync`
  for the text. There is no decode, allocation, framebuffer read or vertex
  buffer. Pre-roll draws at ≤ 30 fps (`buffering_frame_paced(33 ms)`) and
  waits only for its own previous flip. The blocking steps before pre-roll
  (vdec_open, stream_open, prefill) each show one frame, as before.
- **Emulator:** frames keep `player_startup_flip()`'s RPCS3 `rsxSync` rule. No
  NV3089 bitmap-font blits are used; the text is TTF runs.

### 2. Artwork colour (`render/art_colour.h`, `render/ui_art.{h,cpp}`)

- **Sampling:** at most 24×24 points on a grid inside an 8% border, from the
  bitmap already in MAIN memory, never VRAM.
- **Choosing the colour:** samples are binned into 12 hue buckets, weighted by
  saturation² × brightness. Near-black and near-white samples do not vote. The
  winning bucket is smoothed with its neighbours and its mean colour is pushed
  into a readable band, giving three shades: accent (L 0.62), glow (0.46) and
  deep (0.16).
- **Fallback:** the theme accent exactly. It is used when there are no pixels,
  when the image is invalid, or when less than ~8% of it is clearly coloured.
- **Cache:** analysed once per image id. An image that has not loaded yet is
  not cached; a colourless one is cached as the fallback.
- **Fed from:** detail's poster, before it goes to VRAM, and the thumbnail
  cache (music covers, screensaver posters).
- **Used by:** the buffering screen's glow and no-art background (the ring
  itself is always Jellyfin's colours), the music atmosphere, and the
  screensaver's halo.

### 3. Version selector (`xmb/ui_info.cpp`)

It uses the same modal, input and result as before. The version Play already
uses (source[0], Jellyfin's default order) now stands under **RECOMMENDED**
with a PLAY tag. Its description is its default audio track's DisplayTitle,
reworded ("English · DTS-HD MA · 5.1", without the Default/Forced/External
flags). The rest are listed under **OTHER VERSIONS**, each with its audio
words, in a 6-row window with a range mark. An empty name becomes "Version N".
No resolution or frame rate is shown, because a source does not report them
per version (`JFMediaSource` has none). The detail page's Version selector is
widened to 260; its label reads "Recommended" (accent) before the name when the recommended one is
chosen. Items with a single version still show no selector.

### 4. Music atmosphere (`music/music_screen.cpp`)

- **Colour:** the album's accent tints the visualiser bars, the artist line
  and the seek fill.
- **Light:** two GPU layers over the wave. One is a halo behind the cover that
  grows about 8% and brightens with the music. The other is a floor wash in the
  deep shade.
- **What drives them:** `music_viz_bands()`, the bars' own data, averaged over
  the lower two thirds. Attack is 90 ms and release 700 ms, so it swells
  rather than flickers, and it falls to rest when paused.
- **Unchanged:** no new DSP, no change to the audio pipeline or the wave.

### 5. Ambient screensaver (`xmb/ui_ambient.cpp`)

- **When it starts:** after 3 idle minutes on the XMB. Set this in
  `jellyfin_ambient.txt`, in minutes; 0 turns it off. A modal or time spent in
  another screen (detail, music, a film) counts as activity.
- **Going in:** the UI dissolves under a black cover over 0.7 s. The cover
  then lifts to 58%, so the wave shows dimmed, while one poster at a time
  drifts in. Each poster is on screen for 16 s: ±18 px, +4%, a 1.8 s
  cross-dissolve, a glow in its own colour and a faint reflection. The lockup,
  the clock and one line of metadata stay up.
- **Coming back:** any button dissolves it back in 0.7 s. That press does
  nothing else.
- **Artwork:** posters Home already has (Continue Watching, Next Up, Recently
  Added Movies and Shows; 4 each, de-duplicated). They are requested at the
  Home queue's cache size, so nothing new is fetched. If Home was never
  visited there are no posters, only the dimmed wave and clock.
- **Cost:** while it runs, the XMB's own phases are skipped, so a frame is
  cheaper than a normal one. The dissolve is one blended quad after the text
  flush, FIFO-ordered, with no extra fence.

### Tests and checks

- `test_experience` (new, in `all` and `check`): 5,920 checks, 0 failures.
  - **Artwork:** dominant colour (red on black, blue sky over grey, the
    majority hue), stride, determinism, the alpha byte ignored, NULL/zero/
    negative/stride-invalid input, greyscale/black/white/speck fallback,
    caching (one analysis over 1,000 lookups, late art analysed once,
    colourless art cached, LRU).
  - **Buffering:** 0 → loading → ready, % never decreasing, rotation, one
    subtle pulse, no jolt at settle, an instant ready held and not flashed, a
    90 s load, a 3 s frame gap, cancel (quick, the ring not completed, final),
    and determinism.
  - **Version words:** one, several and missing metadata.
  - **Screensaver:** idle, dissolve, swallow, an interrupted dissolve, off, and
    slides.
- Host suite: 29 of 29 runnable tests pass. The five decode tests and
  `test_dts_stream` need ffmpeg tone files (no ffmpeg in the environment that
  ran them). `test_bg_gradient` / `test_month_bg` are still missing from the
  tree, as before.
- **PS3 build: NOT RUN.** There was no PSL1GHT toolchain in the environment.
  Instead, every new and edited translation unit, plus every file in
  `source/ui`, `source/player` and `source/music`, passed a host
  `g++ -fsyntax-only` against the real PSL1GHT headers. The only exception is
  `theme.cpp`, which is untouched and trips on a newlib/glibc `MAXPATHLEN`
  difference. That is a type and API check, not a PPU build. **Do the clean
  serial build first**; the new objects are `ui_art.o`, `ui_buffering.o` and
  `ui_ambient.o`.

### Needs the console / TV

1. **Clean PS3 build.** The census should stay at 48 warnings. Check none are
   in the new files.
2. **Buffering screen on hardware:**
   - The first frame after Play shows the detail backdrop darkened, with the
     ring and mark centred.
   - The ring turns smoothly through a long pre-roll, and `preroll:` timings do
     not move against spine8 (the draw is ≤ 30 fps, GPU only).
   - Circle = Start now still works.
   - The outro is ~0.45 s and the first video frame follows cleanly, with no
     stale frame and no flash.
   - Errors: stream_open cancel and a vdec failure fade quickly into the error
     screen.
   - Next-episode auto-advance shows the plain background.
3. **Mark and ring legibility at TV distance:** the mark is 68 px wide and the
   ring 128 px, both authored at 720p. Check the ring's 3 px stroke at 480p/576i.
4. **Accent choice on real posters:** check that it reads well and is never
   muddy. The fallback is the theme accent.
5. **Music:** the halo and floor breathe with the music without pumping. Check
   the frame cost against spine8 (two fans and a ramp).
6. **Screensaver:**
   - It starts after 3 minutes and a press dissolves it back with the press
     swallowed.
   - Posters load (cached at the Home queue's size) and drift smoothly.
   - Constant-alpha posters rely on `rsxSetBlendColor`, the same unproven state
     as spine6's.
   - The reflection's mirrored UVs.
   - `xmb:` cost while ambient: `bpx` must not move.
7. **Version selector with 2+ versions:** the layout, the 6-row window, and
   that X plays the one chosen.

---

## 2026-09-24 (later) — hardware feedback round, BUILT (host-checked), NOT DEPLOYED

This round answers the first TV look at the spine build. Merged into
`feature/xmb-spine` alongside the experience pass.

### JellyWave: slower, floatier, translucent, neon (`ui_wave.cpp`, `wave_gel.h`, `wave_light.h`)

**Parameters only.** No vertex buffer, `jw_upload()`, fence, topology, draw
count or blend-state change; the strobe fixes stand.

- **Speed:** `JW_SPEED_DEF` 50 → **20**. It is still overridable with
  `jellyfin_jwspeed.txt`. Moving slower also shrinks the step between rebuilds
  (every 3rd call), which is what read as choppy.
- **Floatier:** JellyWave takes 0.55× of the solver's broadband perturbation.
  The fine ripple is what made a slow wave look agitated.
- **Translucency:** layer opacity 255/235/179 → **150/120/90**. The additive rim
  pass is unchanged in kind, so the edges keep their glow while the bodies
  become coloured glass.
- **Neon, not white:**
  - The rim colour went from near-white D8F4FF to Jellyfin cyan (0.18, 0.84,
    1.0), and the fringe now runs to violet (0.76, 0.36, 1.0) instead of pale
    blue.
  - `JW_RIM_MIX` 0.20 → 0.14 and `JW_RIMPASS_I` 0.62 → 0.55.
  - The specular and Fresnel sheen now take a neon cyan (`JW_GLOW_*`) instead
    of the key light's near-white, and `JW_SPEC_I` went 0.62 → 0.34.
- **Tests:** all ten wave suites pass unchanged, and the calibration bands in
  `test_wave_light` still hold.

### Player HUD (`hud_v3.inc`, `player.cpp`, `player_menu.cpp`)

- The top-right audio-path pill is gone; it only ever read AUDIO.
- The audio chip now names the track in the version selector's words
  ("English · TrueHD Atmos · 7.1"). It is set at start and on every track
  change; with the gate off, the old HUD still reads AUDIO.
- The chips are restyled as XMB glass:
  - dark translucent body, hairline border, a one-pixel light catch along the
    top;
  - a small spaced key (AUDIO / VOLUME / SUBTITLES) over the value;
  - a volume meter along the chip floor, and an accent dot when subtitles are
    on;
  - an accent_alt border and a soft accent halo on focus.
  - They are composed on change like the rest of the HUD, so there is no
    per-frame cost.

### Text: escapes, not fonts (`api/json_unescape.h`, test `test_json_unescape`)

- **Cause:** Jellyfin's serialiser writes an apostrophe as `'`, `+` as
  `+`, and non-ASCII as `\uXXXX`. The client's string readers copied the
  raw bytes, so synopses showed `don't`, codec words picked up escape
  digits between their letters, and an escaped `\"` ended a string early.
- **Fix:** one decoder handles every escape, including surrogate pairs, and
  writes UTF-8, which the renderer already draws. It never splits a character
  when it truncates. It is used by `json_get_string`, `json_get_in_range`
  (jellyfin_api.cpp), `xmb_json_str_range` and `xmb_json_first_arr_str`
  (ui_json.cpp), the array readers in api_detail.cpp, and the facts parser.
  The font is unchanged.
- **Tests:** `test_json_unescape` (19 checks); `test_facts` and
  `test_media_sources` pass.

### Golden Age keeps the Jellyfin lockup (`theme.cpp`, `themes/golden-age.ini`)

The built-in Golden Age table and the ini use XMB wave's `wordmark` and
`lk_*`, so it gets the cool mark and the Jellyfin wordmark ramp. The gold mark
raster is no longer selected by any shipped theme. `test_theme` is updated,
with the reason, from README 2.1's gold values.

### Cast portraits: 2:3 cards (`ui_info.cpp`)

The circles are replaced by 58×87 portraits with a hairline frame, the XMB's
card shape. 2:3 is a headshot's native aspect, so no face is cropped. The
names sit under them and still clear the selector row.

### Triangle quick-peek on Movies / TV (`render/peek.h`, `xmb/ui_peek.cpp`)

- **What it does:** Triangle on a grid poster turns it over. The card spins
  about its vertical axis (width × |cos θ|, one face swap edge-on, a 6% swell
  at mid-turn) while it lifts out of the grid and grows into a centred 4:3
  panel, 450 authored tall.
- **The back:** dark glass with a hairline in the artwork's accent, the
  poster, the cast (the first 4 actors), the title, a meta line, the tech line
  (lossless audio in accent_alt), and the synopsis wrapped once per item.
- **Controls:** X opens full detail (or a series' seasons). Triangle, O or the
  d-pad turns it back into its grid slot. Open takes 520 ms and close 380 ms;
  an interrupted open or close reverses continuously.
- **Data:** the facts worker now also asks for `Overview,People,OfficialRating`
  (`ItemFacts.overview/rating/cast/cast_id`, tested in `test_facts` 6–7), so
  opening a peek never blocks a frame and the text fades in when it lands. No
  backdrop is loaded, and the poster is the grid's cached thumbnail.
- **Draw order:** it draws after the frame's text flush (GPU, then one
  `rsxSync`, then its own text window). The fence exists only while a peek is
  on screen. It takes input before `spine_try_back`, so O closes the peek
  instead of leaving the tab.
- **Tests:** `test_experience` +337 checks (one face swap, edge-on at the turn,
  exact landing, continuity when interrupted).
- **Other tabs:** they keep Triangle = detail.

### Home posters blank after playing an album (`thumbnail_cache.cpp`, `music_screen.cpp`)

- **Not reproduced here.** One mechanism fits every symptom. Home keeps
  re-touching its own cache entries at the queue size, so an entry damaged
  while the music screen ran is never refetched. The TV tab asks at the grid
  size and gets fresh copies.
- **Changes:**
  1. **A real VRAM overflow, fixed.** At 720p Home's square music row (244×244)
     needs 249,856 mirror bytes against the 249,600 each slot had, so its copy
     ran 256 bytes into the next slot's texture. Slots are now sized for the
     largest square any request can make.
  2. **Integrity stamps.** Each slot hashes the first 16 words of its decoded
     pixels and of its VRAM mirror when they are written.
  3. **On leaving the music screen**, `thumb_cache_verify_and_flush()` checks
     every READY slot against its stamps, logs `thumb: verify+flush after
     music: ready=N damaged px=A vram=B`, and empties the cache so what is on
     screen next is fetched fresh.
- **Hardware check:** play an album, back out, and read that line. Non-zero
  `px` or `vram` points at corruption in that memory. Zeros with the bug gone
  mean the flush alone was enough.

### Needs the console / TV

1. **JellyWave at speed 20:** fluid, no visible stepping at rebuild-every-3.
   If it still steps, try `jellyfin_jwrebuild.txt` = 2 and read the frame cost.
   Judge the translucency and whether the neon rim reads as glow, not shine.
2. **The HUD chips at TV distance:** the 8.5 px keys, and the long audio
   label's clip at 300 px.
3. **Synopses and codec words:** apostrophes, accents and "DTS-HD MA" in
   Continue Watching and on detail.
4. **Golden Age:** the cool mark and the Jellyfin wordmark.
5. **The cast cards'** layout on detail.
6. **The peek:**
   - smoothness of the turn (mid-turn the card is drawn LINEAR-scaled from the
     grid thumbnail);
   - the extra `rsxSync` while it is open;
   - X into detail, and O not leaving the tab.
7. **Album → back → Home:** posters load, and the `thumb: verify+flush` line.

### spine9 — built and deployed 2026-09-24

PS3 clean serial build of `9bb092f` + one fix: 119 objects, 48 warnings
(unchanged census), header `0480`. The first build had 49: GCC newly flagged
the existing `thumb_cache_init` "VRAM mirrors FAILED" snprintf (72–81 bytes into
`buf[80]`), so the buffer is now 96. Host tests: all 10 listed in the handoff pass.

EBOOT sha1 `78395337cf35b35737dbf5f1be7dc71beed3916c` (sha256 `0c4ec502…`),
1,578,272 bytes. Staged as `outputs/EBOOT.BIN.spine9` + `JellyFin-PS3-spine9.pkg`.
FTP'd to .202 and read back to verify; `jellyfin_spine.txt` = 1. The console
had been running another session's EBOOT (1,474,960 bytes, 10:20), not spine8.
It is kept as `outputs/EBOOT.BIN.pre-spine9-console`.

### spine10 — strobe fix + additive rim restored, deployed 2026-09-24

spine9 strobed on hardware and showed black accents at first. Its log (written
3 min after the push) had `sync` ≈ 14.5 ms and 28–33 ms frames, falling to about
1 ms only in the `jellywave-body-off` profile. That is the known reuse-frame
signature. **feature/xmb-spine was based on 428d54a and never had the fixes.**
Cherry-picked from the strobe line:

- 955f218 → 77fed10: the empty `else if (jellywave)` reuse guard, and the
  strobe-test harness made inert. It was still cycling profiles, which is why
  the wave changed look partway through.
- 241357a → 6cde40a: the rim pass is additive again. The opaque rim from 14428bb
  is the black accents.

Clean serial build: 119 objects, 48 warnings, header `0480`. EBOOT sha1
`4d7c53b7…` (sha256 `c8ab5dca…`). Staged as `outputs/EBOOT.BIN.spine10`,
FTP'd and verified. Expected on the TV: `sync` about 2.7 ms and frames about 22 ms,
as on the verified 7ed1f910 build.

### spine12 — Triangle peek wired everywhere + cold boot + UI sfx, deployed 2026-09-24

spine10 on the TV: strobe fixed (`sync` ≈ 3.4 ms, frames 16.7–22 ms), wave
right. Two faults remained:

- **Triangle opened detail, like X.** The peek existed but only the Movies/TV
  grid root called it. The log never showed that path (`outer: triangle` lines
  appear only there). Home's handler still said "X opens; so does Triangle".
  bbe4c2e: Triangle peeks on Home, the base column, every non-music library
  grid, the TV/collection sub-grids and Search. X from the peek does that
  screen's X. The depth stage now notes the focused card's image
  (`depth_note_focus_card`), so the spin turns over the poster on screen.
  New log line: `peek: open <id> type=... img=... WxH/kind`.
- **No cold boot.** feature/cold-boot-animation had never been merged here.
  eddcd6b merges fd2ee94 (4 add/add conflicts, both kept; boot hook first in
  the input chain).

Also merged feature/ui-sfx (d318a3c, clean). That session had deployed
spine10-uisfx at 12:26. spine11 overwrote it at 12:45; it is kept as
`outputs/EBOOT.BIN.pre-spine11-console-1226`.

Clean serial build: 121 objects, 48 warnings, `0480`; 12 host tests pass.
EBOOT sha256 `1eb0a24f…`, 1,731,216 bytes; `outputs/EBOOT.BIN.spine12`.

### spine13 — user reverts + 3D wordmark, deployed 2026-09-24

spine12 on the TV: everything working, including sounds, the boot animation,
the peek (9 opens logged) and `verify+flush after music ... px=0 vram=0`.
Requested in 03a9a53:

- Music: the cover-art halo and floor wash are removed. The album accent tint
  (bars, artist, seek) stays.
- Version picker: the RECOMMENDED / OTHER VERSIONS modal and the
  "Recommended" selector label are reverted to the plain list and "Version".
- L1: no blue eyebrow on library, Search or Settings tabs. It repeated the tab
  name; only the white title shows now. Home keeps its row eyebrow.
- JELLYFIN wordmark: two darkened-ramp extrusion layers over the drop shadow.

Clean serial build: 121 objects, 47 warnings (the version picker's
format-truncation site is gone with it), `0480`; 12 host tests pass.
`outputs/EBOOT.BIN.spine13`, 1,725,344 bytes.

### spine14 — lag pass from the spine13 log, deployed 2026-09-24 (acb8ee5)

What the spine13 log showed, and what changed:

| symptom | cause in the log | fix |
| --- | --- | --- |
| 6-10 s freeze backing out of a film | `Stopped http=-1` after 5 s, then `stop_transcode` 5 s; the stream socket was still open (512 KB rcvbuf on a 128 KB libnet pool) | socket closed first; reports on a worker; **Returning** screen (buffering look) covers the teardown, 2.5 s cap; skipped on episode auto-advance; next playback joins a live report first |
| all posters gone after playback | `thumb_cache_shutdown` dropped every slot | slots with a valid VRAM mirror survive, stamp-checked (top/middle/bottom rows) on return; pixels refilled in the background (`px_valid`); log `thumb: kept N posters through playback (damaged K)` |
| 0.5-1.1 s first Home frame back | `xmb: frame=556ms / 1136ms other=...`: Home rows fetched with blocking HTTP on the render thread, behind http_request's global mutex | Home rows fetched by a worker (`jf_homerow`); a fetch that predates a playback is discarded for the dynamic rows |
| steady 22.2 ms frames on busy screens | 16.7+16.7+33.3: the 6.3 ms JellyWave build on every rebuild call missed vsync | build moved to `jf_jwgen` on the PPU's other hardware thread; stage/fence/upload untouched; rebuild default 2 (console file set to 2) |
| music screen ~25 fps, wave lagging with audio | all text CPU-composited (VRAM reads) + 453 px CPU cover blit | text through the RSX (`ui_text_gpu_begin/flush`), cover from its VRAM mirror; heartbeat logs `gpu/sync/draw us/frame` |
| hitch every few seconds | `run cache flushed (atlas full)` / `glyph cache flushed` every 2-5 s | atlas 12 MB (falls back to 4) / 1024 slots; glyph arena 640 KB / 4096 slots |
| posters dropped after music | verify+flush always flushed (5 runs: 0 damage) | only stamp-failing images are dropped |

Clean serial build: 121 objects, 47 warnings, `0480`; 12 host tests pass.
`outputs/EBOOT.BIN.spine14`, 1,729,280 bytes (sha256 `f9b84ca6…`).

To check in the next log: `jellywave: ... worker=1 late=0`, `xmb: frame=16.7ms`
on Home, `show_player: exit took N ms (reports done)`, `thumb: kept N posters`,
`music_screen: ... draw=` well under 10 ms.

### spine15 — distinct bands, calm wave, music focus mode, deployed 2026-09-24 (01aca02)

feature/jellywave2 (the audio session's build, live as jellywave2-spine14) was
merged first (9ceb0ca), so spine15 contains it.  On top of it:

- **Distinct lows/mids/highs** (`wrm_distinct`, wave_render_map.h). The log had
  drive 0.62->1.02 and ts 1.2-1.3 shared by every layer against +-15% per band.
  Shared terms are now held near rest. Each layer follows one band group with its
  own timing: near = lows (60/420 ms), middle = mids (45/260 ms), far = highs
  (15/120 ms). Heights run 0.78 .. 1.75 / 1.65 / 2.10, with brightness per
  layer. `test_distinct_framing` measures those caps in the same box.
- **Speed**: console `jellyfin_jwspeed.txt` 50 -> 20. Music no longer speeds
  the wave (ts pinned to 1.0).
- **Music leave hang**: `music_stop()` no longer joins the stream thread
  (which was stuck in HTTP on a failing network). It is joined by the next
  `music_start()` / `show_player()`. crash_log breadcrumbs m0..m9.
- **Music screen**: breadcrumb removed. Focus mode starts after 4 s without
  input: Up Next fades out and the controls go to 35%, eased both ways. Leaving
  dissolves to the wave over 350 ms.

121 objects, 47 warnings, `0480`; 16 host tests pass.  `outputs/EBOOT.BIN.spine15`,
1,735,712 bytes.  Next log: `wave: ... amp=a/b/c ... L=x/y/z` should show the
three layers moving independently during music.

### spine16 — motes, bass, layout, deployed 2026-09-25 (cb33124)

spine15 verdict: audio reaction "at its best" -- keep wrm_distinct.  Added: motes (stage 9) while music plays (320 additive hex glints, own buffers, behind the JellyWave fence); lows floor 0.18 x1.4 (caps unchanged); music cover/text at 0.225 H; jwspeed file 14.  `outputs/EBOOT.BIN.spine16`, 1,743,328 bytes.  Log: `wave: motes ready` at boot.

### spine17 — snow field, sub-bass shocks, loading screen, deployed 2026-09-25 (97b6fa6)

spine16 verdict: audio "pretty much perfect"; asks: sub-bass shocks through the whole wave, loading screen on X-to-details, album open animation, Up Next art visible when selected, particles screen-wide like snow (reference lines.qrc relationships).  Done: wave_snow.h (700, DOF, sub-bass kick), wrm_distinct shock (to caps, accent total capped), loading_run (details + album), music intro fade, highlight around GPU art.  `outputs/EBOOT.BIN.spine17`, 1,743,552 bytes; 17 host tests.
