# Bundled fonts

Every font compiled into this app, why it is here, and under what licence.

| Font | Used for | Licence | Licence text |
|---|---|---|---|
| Open Sans (Regular, Bold) | fallback only, and the default subtitle face | Apache 2.0 | upstream |
| Noto Sans Bold | optional subtitle face | SIL OFL 1.1 | [OFL-NotoSans.txt](OFL-NotoSans.txt) |
| Roboto Condensed Bold | optional subtitle face | Apache 2.0 | [Apache-2.0-RobotoCondensed.txt](Apache-2.0-RobotoCondensed.txt) |
| Tabler Icons | UI glyphs | MIT | upstream |
| Material Icons | UI glyphs | Apache 2.0 | upstream |
| Michroma | `--font-spec`: codec / quality values | SIL OFL 1.1 | upstream (Vernon Adams, Google Fonts) |
| Satoshi (Regular, Bold) | `--font-tab`: tab labels, clock, cast | ITF Free Font License | upstream (Fontshare) |
| **SCE-PS3 Rodin LATIN** | `--font-system` / `--font-tech`: every string, and the fallback for every other role | **none — Sony system font** | — |
| **Microgramma** | `--font-eyebrow`: eyebrows and section labels | **none — Linotype/Monotype commercial** | — |
| **GT America Expanded Bold** | `--font-display`: media titles | **none — Grilli Type commercial** | — |
| **Mata Bold** | the lockup wordmark, and nothing else | **none — unverified** | — |

## The four in bold cannot be redistributed

This file used to say, correctly, that Arial/Helvetica/Netflix Sans/Tiresias
"are all proprietary and cannot ship in a GPLv3 package". Three faces that went
in with the XMB revamp are in exactly that category, and saying so here is the
point of this file:

- **SCE-PS3 Rodin LATIN** is the PS3's own system font, extracted from console
  firmware. It is what makes the UI look like it belongs on the machine, and
  there is no licence under which it may be redistributed.
- **Microgramma** is a Linotype/Monotype retail face. The bundled
  `microgramma-web.ttf` is a web-font conversion, which does not change that.
- **GT America Expanded Bold** is a Grilli Type retail face, bundled as a
  99-glyph subset (see below).
- **Mata Bold** arrived in the design bundle with no licence statement at all,
  which is not the same as a permissive one. It draws exactly one string —
  "JELLYFIN" in the lockup — so it is also the cheapest of the four to
  substitute: one `face_of()` case and one embed.

They are in the tree because the design specifies them and the owner of this
build asked for them. That is a legitimate choice for a personal build. It does
mean **this package as built is not redistributable**, and a public release
would have to substitute open faces for those three. The renderer makes that
substitution cheap: each role resolves through `face_of()` in
`render/ui_text.cpp`, so swapping a face is a one-line change plus an embed.

## GT America Expanded Bold is a subset, and the renderer knows it

The bundled file is 15,860 bytes and carries **99 glyphs**: `A-Z`, `a-z`, `0-9`
and `! ( ) , . : ; ? _` and space. It has **no hyphen, ampersand, slash,
apostrophe, quote or accented character**.

As a single face that would render "Spider-Man" as "SpiderMan" and "Amélie" as
"Amlie", silently, because a missing codepoint is `.notdef` and `.notdef` is
usually a zero-width nothing. So `--font-display` is not one face: it is a
chain, per the design's own CSS (`"GT America Expanded", "Rodin", …`), and
`chain_of()` resolves it per codepoint with Rodin behind it. `tests/test_utf8`
section 7 is the guard — it asserts the subset really is missing those
characters, that they come back from Rodin, and that they land on the run's
baseline rather than their own.

## Why these three for subtitles

The typefaces people associate with subtitles — Arial, Helvetica, Netflix
Sans, Tiresias Screenfont — are all proprietary and cannot ship in a GPLv3
package. These are the open faces closest to them:

- **Open Sans** — closest to Helvetica/Arial in colour and width, and already
  bundled for the UI, so it costs nothing. The default.
- **Noto Sans** — Open Sans's sibling; a shade more open, slightly more even
  in rhythm.
- **Roboto Condensed** — narrower, so a long line of dialogue fits on one row
  instead of wrapping to two. Worth having on a 16:9 screen.

All three are used **bold**. Every broadcaster and streaming service sets
subtitles semibold or heavier, and weight does more for legibility across a
room than the choice between one humanist sans and another.

## Why the added faces are small

The full faces are 631 KB (Noto Sans Bold) and 502 KB (Roboto Condensed Bold)
— together roughly the size of the entire package. They are subset to what a
subtitle actually needs:

- ASCII, Latin-1 Supplement and Latin Extended-A
- the punctuation SubRip carries: curly quotes, en and em dashes, ellipsis,
  guillemets, inverted `?` and `!`, the OE ligature, arrows

That is 328 glyphs each, **20.8 KB and 20.2 KB** — 97% and 96% smaller, and
about 3.5% of the package between them.

Regenerate with `fonttools`:

```
pyftsubset NotoSans-Bold.ttf --output-file=NotoSans-Bold-subset.ttf \
  --unicodes=U+0020-007E,U+00A0-017F,U+2013,U+2014,U+2018,U+2019,U+201C,U+201D,U+2026,U+00AB,U+00BB,U+2039,U+203A,U+00A1,U+00BF,U+0152,U+0153,U+0178,U+2190,U+2192 \
  --layout-features= --no-hinting --desubroutinize \
  --drop-tables+=GSUB,GPOS,GDEF,DSIG,LTSH,VDMX,hdmx,kern
```

then convert to a C array the way `notosans_bold.h` was made. The music note
`U+266A`, which lyric subtitles sometimes use, is absent from both upstream
faces and so cannot be subset in.
