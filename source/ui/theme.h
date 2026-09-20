/* source/ui/theme.h — Phase 1 of the XMB revamp.
 *
 * Replaces the compile-time palette in ui_visuals.h with a runtime struct.
 * Every existing draw call site keeps compiling unchanged via the shim macros
 * at the bottom, so this lands without a thousand-line find-and-replace.
 *
 * Based on design_handoff_ps3_xmb_revamp/source/theme.h, with three additions
 * the shipped header was missing (see design-import/ for the original):
 *
 *   1. Five colours that ui_visuals.h defines and the handoff header did not
 *      shim -- XMB_BG (the RSX clear, used by five clearScreen() call sites)
 *      and the three OSK key colours.  Left unshimmed they would have stayed
 *      compile-time constants and silently ignored the theme.  accent_deep was
 *      in the struct but had no macro.
 *   2. The lockup ramp tokens (lk_mark_a/b, lk_word_a..d).  README section 2.1
 *      and both .ini files define them; the struct had only a flat `wordmark`,
 *      which cannot express the cyan->violet gradient the lockup needs.
 *   3. theme_load_setting()/theme_save_setting(), matching the house pattern
 *      used by overscan/subfont/statsovl for persisted user choices.
 *
 * The three OSK colours and `bg` are OPTIONAL in a .ini: omitted, they fall
 * back to panel / white / track / bg_bot respectively, so a hand-written theme
 * file only has to carry the tokens the design document actually lists.
 */

#ifndef JF_THEME_H
#define JF_THEME_H

#include <ppu-types.h>
#include <stdbool.h>

#define THEME_BUILTIN_COUNT 2    /* 0 = XMB wave, 1 = Golden Age */
#define THEME_MAX_ENTRIES   16   /* built-ins + files, the theme picker's cap */

typedef struct {
    /* colours — 0xRRGGBB, the renderer owns alpha */
    u32 bg;            /* RSX clear, sits under the ramp               */
    u32 bg_top, bg_bot;
    u32 accent;        /* focus glow, selection, tab underline         */
    u32 accent_alt;    /* chips, eyebrows, progress fills              */
    u32 accent_deep;   /* ramp start for the single primary action     */
    u32 wordmark;      /* "Jellyfin" in the lockup (flat fallback)     */
    u32 text, text_dim, text_faint;
    u32 panel, panel_hi, hairline, thumb_dim, track;
    u32 icon_idle, white, trim;

    /* OSK keys — optional in a .ini, defaulted from panel/white/track */
    u32 key_normal, key_sel, key_label_sel;

    /* lockup ramp — Jellyfin's official AA5CC3 -> 00A4DC pair, run twice.
     * The mark keeps the source logo's gradient axis (12,30 -> 72,63);
     * the wordmark mirrors it, cyan on the left to violet on the right. */
    u32 lk_mark_a, lk_mark_b;
    u32 lk_word_a, lk_word_b, lk_word_c, lk_word_d;

    /* Focus and scrim, added by the 2026-09-20 design revision.
     *
     * focus_ring is a cool off-white that replaces pure white on focused
     * cards.  It exists as its own token because the focus ring had been
     * borrowing XMB_KEY_SEL -- the OSK *keyboard key* colour -- which tied two
     * unrelated things to one value.
     *
     * The two scrims are the options-cross backdrop, left and right ends of a
     * horizontal gradient.  They are the first tokens with an alpha: the .ini
     * spells them "RRGGBB,0.90", and the alpha is stored here as 0-255. */
    u32 focus_ring;

    /* The item-detail header band.  Not a token the handoff defines -- it was a
     * hardcoded 412C73 in ui_info.cpp, which left a violet band sitting on a
     * gold screen under Golden Age.  The XMB wave value below is that exact
     * colour, so nothing changes under the shipping theme; this only gives
     * other themes a way to say what it should be.  If the design later names
     * this, take its value. */
    u32 detail_band;

    u32 scrim, scrim_2;
    u8  scrim_a, scrim_2_a;

    /* style scalars */
    u8  card_radius;   /* 0 | 4 | 8                                    */
    u16 chip_radius;   /* 999 = pill                                   */
    u8  glow_alpha;    /* 0-255, 0 = ring only                         */
    u8  wave_alpha;    /* 0-255, 0 = flat background                   */
    u8  trim_alpha;    /* 0 = trim off                                 */
    u8  ramp_steps;    /* primary-action sweep segments, 0 = flat      */

    char name[32];
} Theme;

typedef struct {
    char name[32];
    char path[256];    /* empty for built-ins */
} ThemeEntry;

/* The live palette.  Statically initialised to built-in 0 (XMB wave) so the
 * very first frame is already themed -- the handoff asked for a memcpy before
 * init_screen(), but a static initialiser gets there earlier and cannot be
 * ordered wrong.  theme_load_setting() then applies the user's choice, and it
 * runs after plog_load_setting() so that its one log line is not discarded. */
extern Theme g_theme;

void theme_apply_builtin(int index);         /* 0 = XMB wave, 1 = Golden Age  */
bool theme_load(const char *ini_path);       /* false = malformed, old kept   */
int  theme_scan(ThemeEntry *out, int max);   /* built-ins + themes/ dirs      */

/* Persisted user choice (house pattern -- see util/overscan.cpp).  Stores the
 * theme NAME, so a file theme survives being re-scanned into a different slot. */
void theme_load_setting(void);               /* call once, after plog is up   */
void theme_save_setting(void);
const char *theme_current_name(void);

/* Advance to the next theme in theme_scan() order and persist the choice.
 * Skips a file that fails to parse rather than stalling on it, so one bad
 * .ini in the folder cannot trap the user on a single theme. */
void theme_cycle(void);

/* Quality modes — see section 7 of the handoff README.  Phase 8 owns the
 * per-effect behaviour; what lives here is the mode itself, the forced-MINIMAL
 * playback clamp, and the frame-time watchdog. */
typedef enum { QUALITY_FULL = 0, QUALITY_REDUCED, QUALITY_MINIMAL } QualityMode;
extern QualityMode g_quality;
void quality_set(QualityMode m);
QualityMode quality_effective(void);         /* MINIMAL while playback is up  */
void quality_set_playback(bool active);      /* playback forces MINIMAL       */
void quality_frame_tick(float frame_ms);     /* auto-downgrade watchdog       */

/* ---- transitional shims: one per colour previously in ui_visuals.h ----
 *
 * Every draw site that used to read a compile-time colour now reads g_theme,
 * so the whole UI follows the live theme with no call-site changes.
 *
 * NOTE for anyone adding a colour: a macro here turns a compile-time constant
 * into a runtime load, which changes how GCC schedules and merges the stores
 * that consume it.  Where that value ends up written into RSX local memory,
 * see the WaveVert comment in render/ui_wave.cpp -- an unpadded struct there
 * turned this into misaligned 64-bit stores and hung the console.
 */

#define XMB_BG            (g_theme.bg)
#define XMB_BG_TOP        (g_theme.bg_top)
#define XMB_BG_BOT        (g_theme.bg_bot)
#define XMB_ACCENT        (g_theme.accent)
#define XMB_ACCENT_ALT    (g_theme.accent_alt)
#define XMB_ACCENT_DEEP   (g_theme.accent_deep)
#define XMB_TEXT          (g_theme.text)
#define XMB_TEXT_DIM      (g_theme.text_dim)
#define XMB_TEXT_FAINT    (g_theme.text_faint)
#define XMB_PANEL         (g_theme.panel)
#define XMB_PANEL_HI      (g_theme.panel_hi)
#define XMB_HAIRLINE      (g_theme.hairline)
#define XMB_THUMB_DIM     (g_theme.thumb_dim)
#define XMB_TRACK         (g_theme.track)
#define XMB_ICON_IDLE     (g_theme.icon_idle)
#define XMB_WHITE         (g_theme.white)
#define XMB_TRIM          (g_theme.trim)   /* with g_theme.trim_alpha */
#define XMB_KEY_NORMAL    (g_theme.key_normal)
#define XMB_KEY_SEL       (g_theme.key_sel)
#define XMB_KEY_LABEL_SEL (g_theme.key_label_sel)

/* The focused-card ring.  Use this, NOT XMB_KEY_SEL, for anything that means
 * "the user is pointing at this" -- the OSK's selected key is a different
 * thing that merely happened to share a colour. */
#define XMB_FOCUS_RING    (g_theme.focus_ring)
#define XMB_DETAIL_BAND   (g_theme.detail_band)

/* The lockup ramp (README section 2.1).  The MARK's pair is baked into the
 * rasters in gfx/jfmark_png.h -- an SVG with a bevel, an edge stroke and a
 * clipped image is not something this renderer can tint -- so XMB_LK_MARK_A is
 * read for one purpose here: its warmth picks which of the two rasters a theme
 * gets.  The WORDMARK ramp is live, sampled per glyph across the word. */
#define XMB_LK_MARK_A     (g_theme.lk_mark_a)
#define XMB_LK_MARK_B     (g_theme.lk_mark_b)
#define XMB_LK_WORD_A     (g_theme.lk_word_a)
#define XMB_LK_WORD_B     (g_theme.lk_word_b)
#define XMB_LK_WORD_C     (g_theme.lk_word_c)
#define XMB_LK_WORD_D     (g_theme.lk_word_d)
#define XMB_WORDMARK      (g_theme.wordmark)   /* flat, for REDUCED/MINIMAL */


#endif /* JF_THEME_H */
