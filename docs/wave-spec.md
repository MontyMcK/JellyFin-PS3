# Wave and particle model — derived reference

Working notes for an **independent** Jellyfin implementation of the wave and
particle systems, derived from analysis of XMB `lines*.qrc` configuration
data supplied for reverse-engineering reference.

## Provenance and scope

Three QRC files were analysed (`lines.qrc`, `lines(4).qrc`, `lines28i.qrc`).
They were read **outside this repository** and nothing from them is, or will
be, committed.

What was read: the plain-text `NAME:type:VALUE` parameter table — numeric
tuning values and the relationships between them. That is a description of
*behaviour*, and it is the only thing needed to write an original
implementation.

What was **not** touched: `spurs/moyou/spline/spline.elf`,
`spurs/particles/particles/particles.elf`, the `.fpo`/`.vpo` shader objects,
the `.tga`/`.dds` textures. No Sony binary, shader or texture is extracted,
reproduced, or shipped. Per §5 of the renderer brief, these are investigated
as references only; the Jellyfin wave is original geometry, original shaders
and original constants.

The container itself, for the record: `"QRCC"` + big-endian u32 uncompressed
size + a raw zlib stream, wrapping a `"QRCF"` resource archive with a name
table and per-context overrides. Documenting the wrapper is a file-format
observation, not a reproduction of its contents.

## Shape of the data

The archive carries one parameter set per presentation context, overriding a
base:

```
black  bright  coldboot1  coldboot2  day  gameboot1..5
higure  initial_setting  music_1  night  welcome_1  welcome_2  yoake
```

Each context may override `BACKGROUND.mnu`, `HDR.mnu`, `LINE1.mnu` and
`PARTICLES.mnu`. Across the three files that gives **54 complete wave blocks
and 39 particle blocks** — enough repetition to tell a tuning knob from a
structural constant, which is the whole value of having several files rather
than one.

Note there is exactly one `LINE1`, not four. The four-band structure in the
Jellyfin design is our own choice; the reference distributes its lines
*within* one group via `SPACING`.

---

## 1. Wave model

Block order, which is also the evaluation order:

```
STATE SHADER                             enable + material
DAMPING LENGTH TENSION SPACING THINNESS  physics
BRIGHTNESS "MIPMAP BIAS" FRESNEL FALLOFF appearance
TIMESTEP PERTURBATION                    integration
POS XYZ  ANG XYZ  ANG ROT  END XYZ       transform
FFD SCALE1/2 XYZ  OFFSET XYZ  PARAM 1    lattice deformation
RED GREEN BLUE                           colour
```

### What is structural vs what is themed

This is the most useful thing the multi-file comparison gives, and it is not
guessable from a single file:

| Parameter | Across 54 blocks | Reading |
|---|---|---|
| `TENSION` | **constant 0.25** | structural — the spring constant is fixed |
| `END X/Y/Z` | **constant 3 / 0.4 / 0.2** | structural — fixed endpoint |
| `FFD SCALE1 Z` | **constant 1** | structural |
| `FFD OFFSET X/Y/Z` | **constant 0 / −0.47 / 0** | structural |
| `DAMPING` | 0.0001 – 0.0003 | barely varies |
| `LENGTH` | 0.306 – 0.496 | mild |
| `TIMESTEP` | 2 – 6.72 | **speed knob** |
| `PERTURBATION` | 0 – 0.1 | **liveliness knob** |
| `SPACING` | 0.298 – 408.3 | wide — a scale factor, not a pixel count |
| `BRIGHTNESS` `FRESNEL` `FALLOFF` `RGB` | wide | presentation only |

**The physics is essentially fixed and themes vary presentation.** That is the
design lesson worth taking: one simulation, many looks. It maps directly onto
the brief's §5 "one reusable wave representation + band-specific transforms"
and means our four bands should share one solver and differ only in transform
and colour.

### The simulation

`DAMPING` / `LENGTH` / `TENSION` / `TIMESTEP` / `PERTURBATION` describe a
**damped spring chain** integrated at a fixed timestep, with a small random
perturbation injected to keep it from settling. `DAMPING` at 1e-4 with
`TENSION` 0.25 is very lightly damped — the motion persists and drifts rather
than decaying, which is what makes it read as continuous rather than as a
loop.

Our `ui_wave.cpp` currently uses summed sines with a per-band phase advance
rather than a solver. Sines are cheaper and, for a band sitting in the bottom
quarter of the screen, visually sufficient. **The solver is worth adopting
only if the sine version looks mechanical in motion** — and that is a
judgement to make on a TV, not in a document.

### FFD

Two stages, applied to the generated curve:

| | X | Y | Z |
|---|---:|---:|---:|
| `SCALE1` | 5.14 – 5.68 | 1.000 – 1.019 | 1 (fixed) |
| `SCALE2` | 2.83 – 3.20 | 0.996 – 1.944 | 1.999 – 3.418 |
| `OFFSET` | 0 | −0.47 | 0 |

`SCALE1` is a strong horizontal stretch (≈5.4×) with Y and Z left alone;
`SCALE2` then stretches X again (≈3×) and Z (≈2–3.4×). Net effect: a curve
generated in a small local space is blown out horizontally and pushed in
depth — which is how a short spline becomes a wide, receding ribbon.
`PARAM 1` runs −2 … −0.909.

For a 2-D UI band this collapses to a scale/offset on the generated vertices;
the depth term only matters if the bands are given perspective.

---

## 2. Particle model

### Steady-state population — the number that decides the architecture

`emit per frame` × `emit prob` gives the spawn rate; `1 / aging speed` gives
the lifetime in frames. Pairing them **within each block** (rather than
cross-multiplying the extremes, which would be meaningless):

| | Steady-state particles |
|---|---:|
| Minimum | 2,314 |
| **Median (and by far the most common)** | **2,798** |
| Maximum (one outlier config) | 34,412 |

Thirty-three of the 39 blocks sit at ~2,300–2,800. The 34,412 outlier pairs
the highest emission rate with the longest lifetime and is almost certainly a
boot-sequence spectacle rather than steady UI.

**So the reference ambient field is roughly 2,800 particles.** Against this
session's measurements on the actual console, that costs:

| | Time | Share of a 60 Hz frame |
|---|---:|---:|
| PPU AltiVec SoA @ 25.4 ns/object | **~71 µs** | **0.4 %** |
| 3 SPU workers | ~19 µs | 0.1 % |

This settles §6 of the brief: **the particle simulation does not need an
SPU.** It is a rounding error on the PPU once vectorised. I had been sizing
the benchmark around 10,000–20,000 objects; the reference implementation runs
an order of magnitude fewer, and the visual brief calls for "subtle", not
dense.

The open question for particles was never the physics — it is how the RSX
draws them, and that is still unmeasured (see `renderer-architecture.md`).
At ~2,800 particles that is 11,200 vertices/frame against the wave's existing
~4,660, so it is a real question but a far more tractable one than at 20,000.

### Forces

| Group | Parameters | Observed |
|---|---|---|
| Emission | `emit vel min/mul/var`, `cone angle`, `neg prob`, `vel zscale`, `per frame`, `prob` | cone 51.9° fixed; ~17–70/frame |
| Ageing | `aging speed`, `aging variance` | lifetime 290–990 frames |
| Drag | `friction` | 0.031 – 0.062 |
| Gravity | `gravity` | **−8e-6 … −6.8e-5** |
| Wind | `wind dir x/y/z`, `wind scale` | dir mostly +Y/+Z |
| Noise | `brownian scale` | 0.056 – 0.225 |
| Attractor | `spot pos x/y/z`, `spot attn x/y/z` | z fixed −7.6; attn y,z fixed 0 |

**Gravity is effectively zero** (−1e-5 order) and friction is low. These are
not falling particles — they are near-neutrally-buoyant motes carried by wind
and Brownian noise toward a single attractor. That is precisely the
"atmospheric, subtle" character the design asks for, and it is cheap: no
collision, no pairwise interaction, one attractor.

Only `spot attn x` varies (0–1); Y and Z attenuation are fixed at 0, so the
attractor pulls on one axis. Worth copying — it is what stops the field
collapsing into a ball.

### Shading and depth of field

`size near` / `size middle` / `size far`, plus `near focus`, `near
focus_dist`, `near focus_pow`, `near darkness`, `near fuzziness` and the
matching `far *` set, describe a **depth-of-field model**: particle size and
sharpness vary with distance, with separate near and far falloff curves.
`glare`, `glare scale`, `glare p1/p2` add a bloom contribution per particle.

This is the expensive half and the part §19 warns about. A defensible
reduction for our FULL mode is size-by-depth plus alpha-by-depth, and to skip
per-particle glare entirely — the wave's own rim light already supplies the
highlight the eye reads as glow.

---

## 3. The HDR block — deliberately not adopted

Each context also carries a full HDR/bloom chain: `EXPOSURE`, `WHITE LEVEL`,
`GLARE LEVEL`, `GLARE THRESH`, `GAUSSIAN RAD R/G/B` (separate radii per
channel), `GLARE SUM POW`, `TEX SIZE`, `TEX MAX MIP`, `TONEBEFORE`, `BLUR`.

That is several full-screen passes with per-channel Gaussians. Sony could
afford it because the XMB *was* the application and had the whole machine.
This client also decodes video, decodes lossless audio and drives a network
at the same time.

**Recommendation: do not reproduce it.** §19 lists fullscreen bloom among the
things to treat as expensive, and the measured picture supports that — our
renderer's problem is already pixel traffic, not arithmetic. If a highlight is
wanted, bake it into the wave's rim term (one pass) rather than adding a
bloom chain.

The per-channel Gaussian radii (R 1.1–2.9, G 1.36–2.9, B 1.5–2.9 — blue
always widest) are worth noting as a *look* cue: the chromatic spread is what
gives XMB glare its colour fringe. That can be approximated in the rim shader
for free.

---

## 3b. The spline kernel — from the public reverse engineering

Sources read: `linkev/PlayStation-3-XMB` (`SPLINE_REVERSE_ENGINEER.md`,
`ps3xmbwave/spline-reverse.js`, `spline-settings.js`, `particles-settings.js`)
and `TheGammaSqueeze/xmb-web`. The PSX-Place threads and psdevwiki are behind
bot-detection walls that did not clear; the material below comes from the
GitHub sources plus search results, and the parameter semantics are corroborated
against our own QRC extraction.

### What is solidly established

`spline.elf` is an **SPU program** (Ghidra + SPU plugin, SHA256
`587f66dc…`, text 0x3060–0x8727, rodata 0x8760–0x9aef). So the XMB wave really
does run on an SPU — the architecture in the brief is the one Sony used.

**The curve is a uniform cubic B-spline.** Two independent lines of evidence:
the ELF carries the constant `0x3E2A5556` ≈ 0.16666667 = 1/6, and the
reimplementation uses the exact standard basis —

```
B0 = (1 - 3t + 3t² -  t³)/6      B2 = (1 + 3t + 3t² - 3t³)/6
B1 = (4      - 6t² + 3t³)/6      B3 = (             t³)/6
```

**Control table:** 361 entries (0x169) of 16 bytes at 0x9c00. 361 = 19², and
the index derivation traced out of the kernel is

```
index = 19 * (word >> 4) + (word & 0xF)
```

i.e. a **19 × 19 control grid** addressed by the high and low nibbles of a
word. Two constant basis tables sit at 0xd600 and 0xde00, precomputed once.

**Kernel shape** (FUN_000045c0): exactly **8 iterations**, output stride
**0x400 bytes**, and each iteration stores **eight 16-byte vectors** at offsets
`{0x000, 0x100, 0x200, 0x300, 0x010, 0x110, 0x210, 0x310}`. Four streams
0x100 apart, two quadwords each — ribbon geometry with thickness, not a single
line. Total output 8 × 0x400 = **8 KB per invocation**.

**Runtime inputs arrive by DMA, and they are tiny:**

| Block | Size | Role |
|---|---:|---|
| `b300` | **0x40 bytes (16 floats)** | coefficients blended into the table |
| `b380` | 0x2200 bytes | descriptor/control list for a small interpreter |

The four scalars per quadword of `b300` are extracted by byte-mask
(`0x010203`, `0x04050607`, `0x08090A0B`, `0x0C0D0E0F`), broadcast, and summed
against 16-byte coefficient blocks. Normalization constants live at 0x991c and
0x9940.

### What is *not* established, and matters

**The `b380` descriptor format was never decoded.** It is runtime-fed, not a
compiled constant, and the executor at LAB_00007b30 is an interpreter loop
whose semantics are unknown. The reimplementation's `SyntheticDescriptorSource`
**invents** that payload — its own comment says it "synthesizes PS3-like
spline control behavior", and the repo lists capturing the real b380 as the
outstanding work for 1:1 output.

So: the *pipeline shape* is real; the *content driving it* is a plausible
fabrication. Anyone treating `spline-reverse.js` as "the XMB algorithm" is
over-reading it. For our purposes that is fine — we need an original
implementation anyway, and the shape is the transferable part.

### The finding that changes the SPU case for the wave

**Sony's per-frame SPU input for the entire wave is 64 bytes**, plus a
descriptor list, and the output is 8 KB of geometry. That is *generative* work:
tiny input, compute in local store, modest output.

That is the opposite economics from the particle benchmark, which streamed
10,000 objects in and out and was dominated by DMA-out (48.7 µs of a 162 µs
job). Against our measured MFC figures, a wave job costs:

| | |
|---|---:|
| 64 bytes in | negligible |
| 8 KB out at 21.7 GB/s | **0.37 µs** |
| kernel: 8 iterations × (4 table samples + 8 vec4 mixes) | a few µs |

**So the wave has the right shape for an SPU in a way the particle field does
not.** This refines the recommendation in `spu-feasibility.md`: that document
concluded the SPU's UI value was in moving pixels, based on a
state-streaming workload. A generative geometry job is a third category, and
it is cheap on both ends.

It is still not *urgent* — our sine-based wave is already cheap — but if the
four-band design with motes and caustics grows expensive, this is the one UI
workload whose structure genuinely suits an SPU.

### Parameter semantics, corroborated

`spline-settings.js` uses values that are **exactly** ones from our QRC
extraction — `damping: 0.0001`, `length: 0.306001`, `spacing: 407.658`,
`perturbation: 0.0998587`, `ffdScale1X: 5.67726`, `ffdScale1Y: 1.00077`,
`ffdScale2X: 2.82755`, `ffdOffsetY: -0.469999` — all inside the ranges we
measured across 54 blocks. Independent confirmation that both readings are of
the same system.

Published parameter meanings (community documentation), worth recording
because one is a **naming trap**:

| Parameter | Effect |
|---|---|
| `LENGTH` | higher → longer wave |
| `SPACING` | **lower** → longer wave (inverse) |
| `THINNESS` | **higher → THICKER wave** (the name is backwards) |
| `BRIGHTNESS` | higher → brighter |

Two places the reimplementation deliberately retuned rather than copied:
`tension` 0.12 (QRC: constant 0.25) and `timeStep` 1.0 (QRC: 2–6.72). Its
added structure is worth borrowing for our four bands — a primary band
amplitude plus a secondary frequency (`bandSecondaryFreq: 7.0`,
`bandSecondaryAmp: 0.025`) and **two travelling waves** at different speeds
and amplitudes (`travelSpeed1: 0.25 / travelAmp1: 0.014`,
`travelSpeed2: 0.15 / travelAmp2: 0.008`), then a soft clip. That is a cheap
recipe for motion that does not read as a loop, and it needs no solver.

Shading knobs: `fresnelPower: 4.0`, `fresnelScale: 0.5`, `opacity: 0.7`,
`brightness: 0.98`.

### Particle count — independently confirmed

`particles-settings.js` ships `count: 2000` with a slider maximum of 4000.
Our QRC derivation gave a **median steady state of ~2,798**. Two unrelated
derivations landing at 2,000–2,800 settles the figure: **this is a
~2–3 thousand particle system**, and on the PPU with AltiVec that is well
under 100 µs.

### One approach we must NOT copy

`xmb-web` reproduces the wave by **capturing real geometry from RPCS3**
(`wave_geo.bin`, "the real clip-space cloth geometry", plus captured keyframe
sequences) and porting `lines.qrc`'s fragment shaders directly. That is
effective and it is exactly what §5 forbids here: shipping captured Sony
geometry and translated Sony shaders. We generate our own geometry from our
own constants. Noted so nobody reaches for it as a shortcut later.

## 4. What this means for the Jellyfin implementation

1. **One solver, four bands.** The reference keeps physics constant and varies
   presentation; our bands should share a solver and differ in transform,
   colour and phase. Matches §5.
2. **~2,800 particles, not tens of thousands.** Sizes the whole system, and
   puts it comfortably on the PPU.
3. **Particles need no SPU.** 0.4 % of a frame vectorised. The SPU case for
   the UI rests on pixel movement, not simulation — unchanged from
   `spu-feasibility.md`.
4. **Keep gravity ~0, friction low, one attractor.** Cheap and it is what
   produces the intended drift.
5. **Skip the bloom chain.** Fold the highlight into the rim term.
6. **Adopt `TIMESTEP` and `PERTURBATION` as the two exposed knobs** — they are
   what actually vary the motion's character, and they map cleanly onto our
   FULL / REDUCED / MINIMAL modes (§11) as a speed and a liveliness dial.
7. **Use a uniform cubic B-spline over a small control grid**, not summed
   sines, if the sine version reads as mechanical. That is what the reference
   does, the basis is four cheap polynomials, and it gives continuity for free.
8. **Layer two travelling waves over the band** rather than one — different
   speeds and amplitudes, then a soft clip. Cheap, and it is what stops the
   motion looking periodic without needing a physics solver.
9. **The wave, not the particles, is the SPU candidate** if one is ever
   needed: generative work with a 64-byte input and an 8 KB output, which is
   the shape an SPU is actually good at.
10. **`THINNESS` means thickness.** If we keep the name, document it; better,
    do not keep the name.

Nothing here requires the reference implementation to be present at runtime,
and nothing here is copied from it.
