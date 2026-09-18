# Bundled fonts

Every font compiled into this app, why it is here, and under what licence.

| Font | Used for | Licence | Licence text |
|---|---|---|---|
| Open Sans (Regular, Bold) | the whole UI, and the default subtitle face | Apache 2.0 | upstream |
| Noto Sans Bold | optional subtitle face | SIL OFL 1.1 | [OFL-NotoSans.txt](OFL-NotoSans.txt) |
| Roboto Condensed Bold | optional subtitle face | Apache 2.0 | [Apache-2.0-RobotoCondensed.txt](Apache-2.0-RobotoCondensed.txt) |
| Tabler Icons | UI glyphs | MIT | upstream |
| Material Icons | UI glyphs | Apache 2.0 | upstream |

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
