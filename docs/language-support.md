# Language support

Status: **built for 3.2, not yet seen on a console.** English, 日本語, Português (Brasil), Deutsch, Français, Español.

## How a string gets translated

A string is written in English where it is used and looked up by that text (`source/i18n/i18n.h`):

```cpp
drawTTF(x, y, TR("Continue Watching"), ...);              // looked up now
snprintf(buf, n, TR("%d episodes, about %s."), n, size);   // the format is translated, the arguments are not
static const Hint h[] = {{'X', TRN("Open")}};              // a static table: marked here, tr() at the draw
```

`TR(s)` is `tr(s)`; `TRN(s)` is `s` and only marks the literal for `tools/i18n.py`. A string with no catalog row,
or any text the app did not write (a title, a description, a track name from the server), comes back unchanged, so
passing server text through `tr()` is harmless — and is what lets a library called "Movies" read "Filme".

The catalog is `source/i18n/catalog/*.json`, one object per file:
`{ "English text": { "ja": "...", "pt-BR": "...", "de": "...", "fr": "...", "es": "..." } }`.
`python3 tools/i18n.py gen` writes `source/i18n/i18n_tables.c` from it (committed, sorted for `tr()`'s binary search);
`python3 tools/i18n.py check` is part of the host suite and fails when

- a `TR`/`TRN` string has no row, or a row is used nowhere,
- a row lacks a language, has other printf conversions than the English, or a different number of line breaks, or
  changes leading/trailing spaces,
- `i18n_tables.c` is stale.

To **add a string**: wrap the literal, add a row to the right catalog file, run `gen`, run `check`.
To **add a language**: add it to `i18n_lang` and the two tables in `i18n.c`, to `LANGS` in `tools/i18n.py`, a column in
every row, the choice in `i18n_pref_label`, and the glyphs (below); `i18n_from_system` maps the console's language.

## What the rules are, and why

- **Translate where the text is made, or where it is drawn — never where it is compared.** The audio/subtitle labels
  from the server are parsed by words ("Default", "Forced", `vpick_audio_words`, `ui_info.cpp`), and `strcmp` on item
  types ("Video", "Series") and `strstr(stream_last_error(), "Cancelled")` are logic. None of these are wrapped. The
  stream errors keep two copies for that reason: `stream_last_error()` (English) and `stream_last_error_ui()`.
- **Plurals are two keys** (`"%d episode added…"` / `"%d episodes added…"`), chosen by the caller. A language that
  needs no distinction has the same text in both.
- **Strings that are logged are not translated** (`plog`), so a log from any console reads the same.
- **Not translated:** track and codec names, the player stats overlay, theme and visualiser names, the on-screen
  keyboards' key layout (Latin letters only, as before: a Japanese user cannot type kana into Search or the login),
  and the package's own title.

## Fonts

The Latin faces (Rodin and the others) cover Latin-1, which is every accent these languages use
(`tests/test_i18n_text.cpp` checks every translated character against the real faces). Kana and kanji come from
`source/gfx/notosansjp.h`: Noto Sans JP, subset to the characters the catalog uses, the kana, CJK punctuation, the
full-width forms and the 2,965 kanji of JIS X 0208 level 1 — 685 KB. It is the **last link of every face chain**
(`chain_of` in `ui_text.cpp`), so it also draws Japanese titles and subtitles from the server, which were silently
dropped before. Kanji outside level 1 (rare names) are still dropped. Regenerate with `tools/gen_jp_font.py` when a
translation needs a character the subset lacks; the test says when. Licence and provenance: `source/gfx/fonts/LICENSES.md`.

The Japanese face is scaled by Rodin's em, not its own ascent + descent (which is 1.45 em against Rodin's 0.97, and
would draw kana about a third smaller). The factor is fixed, not derived from the role's own face, because the glyph
cache keys a glyph by font and pixel size only.

## Layout

`tests/test_i18n_text.cpp` measures every Settings label and help line with the real faces at the 720p sizes
`ui_settings.cpp` draws them at. Measured on 10-04: widest label 270 px (French) of 420; widest help line 700 px
(Spanish) of 732, which is the one to watch when a Settings row is added or reworded. Other screens were not measured:
German is the longest on the buttons and hints, and the hint bar right-aligns, so a long set pushes its start leftwards
rather than clipping.

## Known limits

- Translations were written, not reviewed by native speakers. The Japanese is polite form throughout.
- Text that is already a stored string when the language changes (a duration shown on a card, a drive's name) keeps the
  old language until it is built again. The Settings row takes effect at once for everything drawn per frame.
- The 8x8 bitmap-font screens (`drawText`) switch to the TTF face when the text has any non-ASCII byte; English text
  there still draws in the bitmap font.
