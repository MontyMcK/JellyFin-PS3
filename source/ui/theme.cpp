// Runtime theming — see theme.h.
//
// Phase 1 of the XMB revamp.  The palette used to be a block of #defines in
// ui_visuals.h; it is now this struct, and the shim macros in theme.h keep
// every existing draw call site compiling untouched.
//
// Parsing happens exactly twice per boot at most: once for the built-in that
// g_theme is statically initialised to (free — it is a static initialiser),
// and once more if the user picked a theme FILE.  Nothing here runs per frame.

#include "theme.h"
#include "jf_paths.h"
#include "plog.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/file.h>   // sysLv2FsOpenDir / ReadDir / CloseDir

extern void crash_log(const char *msg);

#define THEME_SETTINGS_FILE "jellyfin_theme.txt"

// Where theme .ini files are looked for, in scan order.  The install USRDIR
// carries anything shipped with the package; /dev_hdd0/tmp is the one place
// writable at runtime on real hardware (same reasoning as jf_paths.cpp), so it
// is where a user drops their own theme over FTP without rebuilding the pkg.
static const char *THEME_DIRS[] = {
    "/dev_hdd0/game/JFPS30000/USRDIR/themes",
    "/dev_hdd0/tmp/themes",
};
#define THEME_DIR_COUNT ((int)(sizeof(THEME_DIRS) / sizeof(THEME_DIRS[0])))

// -------------------------------------------------------
// Built-in themes
// -------------------------------------------------------
// Field order MUST match the struct in theme.h.  Positional rather than
// designated initialisers on purpose: designated initialisers for C++ are a
// GCC extension whose support varies by toolchain version, and this has to
// build on the ps3dev GCC without surprises.
// XMB wave is spelled as a macro so that g_theme below gets a CONSTANT
// initialiser from the same single source of truth.  `Theme g_theme =
// THEME_BUILTIN[0];` looked equivalent but is a struct COPY, which C++ treats
// as dynamic initialisation: it lands in .bss plus an init_array entry and is
// only correct if the crt walks init_array before main().  Spelled this way it
// lands in .data, already correct at the first instruction, with no dependence
// on static-init order at all.
#define THEME_INIT_XMB_WAVE {                                              \
        0x000B0E1EUL,   /* bg            RSX clear, under the ramp      */ \
        0x00151A38UL,   /* bg_top                                       */ \
        0x0005060CUL,   /* bg_bot                                       */ \
        0x00AA5CC3UL,   /* accent        focus, selection, progress     */ \
        0x0000A4DCUL,   /* accent_alt    chips, eyebrows, badges        */ \
        0x008A47A9UL,   /* accent_deep   primary-action ramp start      */ \
        0x0000A4DCUL,   /* wordmark      flat lockup fallback           */ \
        0x00E9EBF5UL,   /* text                                         */ \
        0x0099A0BCUL,   /* text_dim                                     */ \
        0x005C6386UL,   /* text_faint                                   */ \
        0x00191E3CUL,   /* panel                                        */ \
        0x00232950UL,   /* panel_hi                                     */ \
        0x002C3258UL,   /* hairline                                     */ \
        0x0012162CUL,   /* thumb_dim                                    */ \
        0x0010142AUL,   /* track                                        */ \
        0x00646C96UL,   /* icon_idle                                    */ \
        0x00FFFFFFUL,   /* white                                        */ \
        0x00000000UL,   /* trim          unused; trim_alpha = 0         */ \
        0x001A1F3EUL,   /* key_normal    OSK, pre-revamp values         */ \
        0x00F0F2FAUL,   /* key_sel                                      */ \
        0x0010142AUL,   /* key_label_sel                                */ \
        0x00AA5CC3UL,   /* lk_mark_a                                    */ \
        0x0000A4DCUL,   /* lk_mark_b                                    */ \
        0x0000A4DCUL,   /* lk_word_a                                    */ \
        0x004189D3UL,   /* lk_word_b                                    */ \
        0x007A70CAUL,   /* lk_word_c                                    */ \
        0x00AA5CC3UL,   /* lk_word_d                                    */ \
        0x00E4EBFAUL,   /* focus_ring    cool off-white, not white      */ \
        0x00412C73UL,   /* detail_band   item-detail header band        */ \
        0x0005060CUL,   /* scrim         options-cross backdrop, left   */ \
        0x0005060CUL,   /* scrim_2       ... and right                  */ \
        230,            /* scrim_a       0.90 * 255                     */ \
        168,            /* scrim_2_a     0.66 * 255                     */ \
        4, 999, 115, 28, 0, 12,                                            \
        "XMB wave"                                                         \
    }

static const Theme THEME_BUILTIN[THEME_BUILTIN_COUNT] = {
    THEME_INIT_XMB_WAVE,
    {   // 1 — Golden Age (proves the token set is complete)
        0x000C0907UL,   // bg
        0x0017120EUL,   // bg_top
        0x00040303UL,   // bg_bot
        0x00E0A13CUL,   // accent
        0x00C8332EUL,   // accent_alt
        0x008A5F1EUL,   // accent_deep
        0x00E8B45CUL,   // wordmark
        0x00F4EDE1UL,   // text
        0x00B3A695UL,   // text_dim
        0x006D6353UL,   // text_faint
        0x001A1410UL,   // panel
        0x00271F17UL,   // panel_hi
        0x003A2E22UL,   // hairline
        0x001D1611UL,   // thumb_dim
        0x00221A13UL,   // track
        0x007A6D5CUL,   // icon_idle
        0x00FFFAF2UL,   // white
        0x00C9A66BUL,   // trim          brass frame, trim_alpha = 77
        0x001A1410UL,   // key_normal    = panel
        0x00FFFAF2UL,   // key_sel       = white
        0x00221A13UL,   // key_label_sel = track
        0x00E8B45CUL,   // lk_mark_a
        0x00C8742EUL,   // lk_mark_b
        0x00C8742EUL,   // lk_word_a
        0x00D68C3FUL,   // lk_word_b
        0x00DFA34DUL,   // lk_word_c
        0x00E8B45CUL,   // lk_word_d
        0x00FFFAF2UL,   // focus_ring    = white; golden-age.ini sets no ring
        0x005A3A16UL,   // detail_band   deep brass, sits in the gold ramp
        0x00040303UL,   // scrim         = bg_bot
        0x00040303UL,   // scrim_2       = bg_bot
        230,            // scrim_a       0.90 * 255
        168,            // scrim_2_a     0.66 * 255
        0,              // card_radius
        0,              // chip_radius
        87,             // glow_alpha
        0,              // wave_alpha
        77,             // trim_alpha
        0,              // ramp_steps
        "Golden Age"
    },
};

// The live palette.  Spelled from the macro, not `= THEME_BUILTIN[0]`, so this
// is a CONSTANT initialiser in .data rather than a runtime struct copy run from
// init_array.  The handoff asked for a memcpy before init_screen(); this is
// correct earlier than that -- at the first instruction of the program -- and
// cannot be ordered wrong.
Theme g_theme = THEME_INIT_XMB_WAVE;

QualityMode g_quality = QUALITY_FULL;

// -------------------------------------------------------
// Small helpers
// -------------------------------------------------------
// plog() takes a plain string, so format first.  The printf attribute is the
// point of doing it this way: the first draft of this helper took a fixed
// (const char *, int) pair and two call sites passed a format with two %s and
// only one argument.  Let the compiler check it instead.
static void theme_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void theme_logf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    plog(buf);
}

static char *trim_ws(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = '\0';
    return s;
}

// Strip an inline `;` or `#` comment.  The shipped .ini files put the token's
// role after the value ("accent = AA5CC3  ; focus glow, ..."), so this is not
// optional — without it every colour in the file fails to parse.
static void strip_comment(char *s) {
    for (char *p = s; *p; p++) {
        if (*p == ';' || *p == '#') { *p = '\0'; return; }
    }
}

// Exactly six hex digits, nothing else.  Returns false on anything malformed,
// which refuses the whole file rather than silently drawing a wrong colour.
static bool parse_rgb(const char *v, u32 *out) {
    if (strlen(v) != 6) return false;
    u32 acc = 0;
    for (int i = 0; i < 6; i++) {
        char c = v[i];
        u32 d;
        if      (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else return false;
        acc = (acc << 4) | d;
    }
    *out = acc & 0x00FFFFFFUL;   // the renderer owns alpha
    return true;
}

// "RRGGBB" or "RRGGBB,0.90" -- the scrim tokens the 2026-09-20 design revision
// added are the first with an alpha.  A bare colour keeps alpha at 255, so the
// form stays backward compatible with every other colour key.
static bool parse_rgba(const char *v, u32 *out, u8 *alpha) {
    const char *comma = strchr(v, ',');
    if (!comma) {
        *alpha = 255;
        return parse_rgb(v, out);
    }

    char rgb[8];
    size_t n = (size_t)(comma - v);
    if (n >= sizeof(rgb)) return false;
    memcpy(rgb, v, n);
    rgb[n] = '\0';
    if (!parse_rgb(rgb, out)) return false;

    // Alpha as a 0..1 float.  strtod rather than a hand-rolled parse so "1",
    // "1.0" and ".66" all work; anything trailing is malformed.
    const char *a = comma + 1;
    while (*a == ' ' || *a == '\t') a++;
    if (!*a) return false;
    char *end = NULL;
    double f = strtod(a, &end);
    if (end == a) return false;
    while (*end == ' ' || *end == '\t') end++;
    if (*end) return false;
    if (f < 0.0 || f > 1.0) return false;
    *alpha = (u8)(f * 255.0 + 0.5);
    return true;
}

static bool parse_int(const char *v, int lo, int hi, int *out) {
    if (!*v) return false;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || *trim_ws(end)) return false;
    if (n < lo || n > hi) return false;
    *out = (int)n;
    return true;
}

// -------------------------------------------------------
// theme_apply_builtin
// -------------------------------------------------------
void theme_apply_builtin(int index) {
    if (index < 0 || index >= THEME_BUILTIN_COUNT) index = 0;
    g_theme = THEME_BUILTIN[index];
}

// -------------------------------------------------------
// theme_load — parse one .ini into a temp, swap in only on success
// -------------------------------------------------------
// Seeded from built-in 0 so a missing key keeps the built-in value, per the
// handoff.  Nothing touches g_theme until the whole file has parsed clean, so
// a malformed theme leaves the previous one standing.
bool theme_load(const char *ini_path) {
    FILE *f = fopen(ini_path, "r");
    if (!f) {
        theme_logf("theme: cannot open %s", ini_path);
        return false;
    }

    Theme t = THEME_BUILTIN[0];
    t.name[0] = '\0';

    // Which of the optional tokens the file set explicitly.  The rest are
    // derived from this file's own palette afterwards, so a theme that omits
    // them tracks its own colours instead of inheriting XMB wave's.
    bool set_bg = false, set_kn = false, set_ks = false, set_kl = false;
    bool set_fr = false;

    char line[512];
    int lineno = 0;
    bool ok = true;

    while (ok && fgets(line, sizeof(line), f)) {
        lineno++;
        strip_comment(line);
        char *s = trim_ws(line);
        if (!*s) continue;

        if (*s == '[') {                       // section header — ignored
            if (!strchr(s, ']')) { ok = false; break; }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) { ok = false; break; }         // not key = value
        *eq = '\0';
        char *k = trim_ws(s);
        char *v = trim_ws(eq + 1);
        if (!*k) { ok = false; break; }

        // [theme]
        if      (!strcmp(k, "name"))   { snprintf(t.name, sizeof(t.name), "%s", v); }
        else if (!strcmp(k, "author")) { /* metadata, ignored */ }

        // [colors]
        else if (!strcmp(k, "bg"))            { ok = parse_rgb(v, &t.bg); set_bg = true; }
        else if (!strcmp(k, "bg_top"))        { ok = parse_rgb(v, &t.bg_top); }
        else if (!strcmp(k, "bg_bot"))        { ok = parse_rgb(v, &t.bg_bot); }
        else if (!strcmp(k, "accent"))        { ok = parse_rgb(v, &t.accent); }
        else if (!strcmp(k, "accent_alt"))    { ok = parse_rgb(v, &t.accent_alt); }
        else if (!strcmp(k, "accent_deep"))   { ok = parse_rgb(v, &t.accent_deep); }
        else if (!strcmp(k, "wordmark") ||
                 !strcmp(k, "brand_wordmark")) { ok = parse_rgb(v, &t.wordmark); }
        else if (!strcmp(k, "text"))          { ok = parse_rgb(v, &t.text); }
        else if (!strcmp(k, "text_dim"))      { ok = parse_rgb(v, &t.text_dim); }
        else if (!strcmp(k, "text_faint"))    { ok = parse_rgb(v, &t.text_faint); }
        else if (!strcmp(k, "panel"))         { ok = parse_rgb(v, &t.panel); }
        else if (!strcmp(k, "panel_hi"))      { ok = parse_rgb(v, &t.panel_hi); }
        else if (!strcmp(k, "hairline"))      { ok = parse_rgb(v, &t.hairline); }
        else if (!strcmp(k, "thumb_dim"))     { ok = parse_rgb(v, &t.thumb_dim); }
        else if (!strcmp(k, "track"))         { ok = parse_rgb(v, &t.track); }
        else if (!strcmp(k, "icon_idle"))     { ok = parse_rgb(v, &t.icon_idle); }
        else if (!strcmp(k, "white"))         { ok = parse_rgb(v, &t.white); }
        else if (!strcmp(k, "trim"))          { ok = parse_rgb(v, &t.trim); }
        else if (!strcmp(k, "key_normal"))    { ok = parse_rgb(v, &t.key_normal); set_kn = true; }
        else if (!strcmp(k, "key_sel"))       { ok = parse_rgb(v, &t.key_sel); set_ks = true; }
        else if (!strcmp(k, "key_label_sel")) { ok = parse_rgb(v, &t.key_label_sel); set_kl = true; }
        else if (!strcmp(k, "lk_mark_a"))     { ok = parse_rgb(v, &t.lk_mark_a); }
        else if (!strcmp(k, "lk_mark_b"))     { ok = parse_rgb(v, &t.lk_mark_b); }
        else if (!strcmp(k, "lk_word_a"))     { ok = parse_rgb(v, &t.lk_word_a); }
        else if (!strcmp(k, "lk_word_b"))     { ok = parse_rgb(v, &t.lk_word_b); }
        else if (!strcmp(k, "lk_word_c"))     { ok = parse_rgb(v, &t.lk_word_c); }
        else if (!strcmp(k, "lk_word_d"))     { ok = parse_rgb(v, &t.lk_word_d); }
        else if (!strcmp(k, "focus_ring"))    { ok = parse_rgb(v, &t.focus_ring); set_fr = true; }
        else if (!strcmp(k, "detail_band"))   { ok = parse_rgb(v, &t.detail_band); }
        else if (!strcmp(k, "scrim"))         { ok = parse_rgba(v, &t.scrim,   &t.scrim_a); }
        else if (!strcmp(k, "scrim_2"))       { ok = parse_rgba(v, &t.scrim_2, &t.scrim_2_a); }

        // [style]
        else if (!strcmp(k, "card_radius")) { int n; ok = parse_int(v, 0, 8,   &n); t.card_radius = (u8)n; }
        else if (!strcmp(k, "chip_radius")) { int n; ok = parse_int(v, 0, 999, &n); t.chip_radius = (u16)n; }
        else if (!strcmp(k, "glow_alpha"))  { int n; ok = parse_int(v, 0, 255, &n); t.glow_alpha  = (u8)n; }
        else if (!strcmp(k, "wave_alpha"))  { int n; ok = parse_int(v, 0, 255, &n); t.wave_alpha  = (u8)n; }
        else if (!strcmp(k, "trim_alpha"))  { int n; ok = parse_int(v, 0, 255, &n); t.trim_alpha  = (u8)n; }
        else if (!strcmp(k, "ramp_steps"))  { int n; ok = parse_int(v, 0, 64,  &n); t.ramp_steps  = (u8)n; }

        // Unknown keys are ignored rather than fatal, so a theme written
        // against a later phase's token set still loads on this build.
        else { theme_logf("theme: unknown key '%s', ignored", k); }
    }
    fclose(f);

    if (!ok) {
        theme_logf("theme: %s malformed at line %d - refused, previous theme kept",
                   ini_path, lineno);
        return false;
    }

    if (!t.name[0])
        snprintf(t.name, sizeof(t.name), "%s", "Custom");

    // Derive the optional tokens the file did not set.
    if (!set_bg) t.bg            = t.bg_bot;
    if (!set_kn) t.key_normal    = t.panel;
    if (!set_ks) t.key_sel       = t.white;
    if (!set_kl) t.key_label_sel = t.track;
    // A theme written before the 2026-09-20 revision has no focus_ring; pure
    // white is what the ring was then, so that stays the fallback.
    if (!set_fr) t.focus_ring    = t.white;

    g_theme = t;
    return true;
}

// -------------------------------------------------------
// theme_scan — built-ins, then every *.ini in the theme dirs
// -------------------------------------------------------
static bool ends_with_ini(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return false;
    const char *e = name + n - 4;
    return (e[0] == '.' &&
            (e[1] == 'i' || e[1] == 'I') &&
            (e[2] == 'n' || e[2] == 'N') &&
            (e[3] == 'i' || e[3] == 'I'));
}

int theme_scan(ThemeEntry *out, int max) {
    int n = 0;

    for (int i = 0; i < THEME_BUILTIN_COUNT && n < max; i++) {
        snprintf(out[n].name, sizeof(out[n].name), "%s", THEME_BUILTIN[i].name);
        out[n].path[0] = '\0';
        n++;
    }

    for (int d = 0; d < THEME_DIR_COUNT && n < max; d++) {
        s32 fd = -1;
        if (sysLv2FsOpenDir(THEME_DIRS[d], &fd) != 0) continue;

        sysFSDirent ent;
        u64 read = 0;
        while (n < max && sysLv2FsReadDir(fd, &ent, &read) == 0 && read != 0) {
            if (ent.d_name[0] == '.') continue;
            if (!ends_with_ini(ent.d_name)) continue;

            // Precisions are for -Wformat-truncation, not safety -- snprintf already
            // bounds this.  sysFSDirent::d_name is MAXPATHLEN+1, so without them GCC
            // assumes a 1025-byte name and warns on every one of these.
            snprintf(out[n].path, sizeof(out[n].path), "%.200s/%.50s",
                     THEME_DIRS[d], ent.d_name);

            // Label it with the file's own [theme] name when it has one, so
            // the picker shows "Midnight" rather than "midnight.ini".
            out[n].name[0] = '\0';
            FILE *f = fopen(out[n].path, "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    strip_comment(line);
                    char *s = trim_ws(line);
                    char *eq = strchr(s, '=');
                    if (!eq) continue;
                    *eq = '\0';
                    if (!strcmp(trim_ws(s), "name")) {
                        snprintf(out[n].name, sizeof(out[n].name), "%s", trim_ws(eq + 1));
                        break;
                    }
                }
                fclose(f);
            }
            if (!out[n].name[0]) {
                snprintf(out[n].name, sizeof(out[n].name), "%.31s", ent.d_name);
                char *dot = strrchr(out[n].name, '.');
                if (dot) *dot = '\0';
            }
            n++;
        }
        sysLv2FsCloseDir(fd);
    }
    return n;
}

// -------------------------------------------------------
// Persisted user choice
// -------------------------------------------------------
// The NAME is stored, not an index: a new file theme appearing in the scan
// would otherwise shift every slot and silently switch the user's theme.
static char s_wanted[32] = "";

const char *theme_current_name(void) { return g_theme.name; }

void theme_save_setting(void) {
    FILE *f = fopen(jf_data_path(THEME_SETTINGS_FILE), "w");
    if (!f) return;
    fprintf(f, "%s\n", g_theme.name);
    fclose(f);
}

// DIAGNOSTIC (black-screen investigation, 2026-09-19).  Dumps the live palette
// to crash_log.txt, which survives a wedge -- plog's thread does not.  If these
// read as zeros the constant initialiser is not reaching .data; if they read
// correctly the palette is fine and the fault is downstream.
static void theme_crash_dump(const char *tag) {
    char buf[192];
    snprintf(buf, sizeof(buf),
             "%s bg=%06lX top=%06lX bot=%06lX acc=%06lX key=%06lX name='%s'",
             tag,
             (unsigned long)g_theme.bg,     (unsigned long)g_theme.bg_top,
             (unsigned long)g_theme.bg_bot, (unsigned long)g_theme.accent,
             (unsigned long)g_theme.key_normal, g_theme.name);
    crash_log(buf);
}

void theme_load_setting(void) {
    theme_crash_dump("theme@entry");
    FILE *f = fopen(jf_data_path(THEME_SETTINGS_FILE), "r");
    if (f) {
        if (fgets(s_wanted, sizeof(s_wanted), f)) {
            char *s = trim_ws(s_wanted);
            if (s != s_wanted) memmove(s_wanted, s, strlen(s) + 1);
        } else {
            s_wanted[0] = '\0';
        }
        fclose(f);
    }

    // No saved choice: the static initialiser already has built-in 0 live.
    if (!s_wanted[0]) {
        crash_log("theme: default (XMB wave)");
        plog("theme: no saved choice, using built-in 0 (XMB wave)");
        return;
    }

    ThemeEntry entries[THEME_MAX_ENTRIES];
    int n = theme_scan(entries, THEME_MAX_ENTRIES);

    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, s_wanted) != 0) continue;

        if (!entries[i].path[0]) {
            theme_apply_builtin(i);
            crash_log("theme: builtin applied");
            theme_logf("theme: '%s' (built-in %d)", g_theme.name, i);
        } else if (theme_load(entries[i].path)) {
            crash_log("theme: file applied");
            theme_logf("theme: '%s' from %s", g_theme.name, entries[i].path);
        } else {
            // theme_load already logged why; built-in 0 stands.
            crash_log("theme: file refused, default kept");
        }
        return;
    }

    theme_logf("theme: saved choice '%s' not found, using built-in 0", s_wanted);
    crash_log("theme: saved choice missing");
}

// Next theme in scan order (built-ins first, then files), wrapping.
//
// A file that fails to parse is SKIPPED rather than selected: theme_load()
// leaves the previous theme standing on a malformed file, so without the skip
// one bad .ini in the folder would look like the cycle being stuck.
void theme_cycle(void) {
    ThemeEntry e[THEME_MAX_ENTRIES];
    int n = theme_scan(e, THEME_MAX_ENTRIES);
    if (n <= 0) return;

    int cur = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(e[i].name, g_theme.name) == 0) { cur = i; break; }

    for (int step = 1; step <= n; step++) {
        int j = (cur + step) % n;
        if (!e[j].path[0]) {                 // built-in: slot index IS the index
            theme_apply_builtin(j);
            theme_save_setting();
            theme_logf("theme: cycled to '%s' (built-in %d)", g_theme.name, j);
            return;
        }
        if (theme_load(e[j].path)) {
            theme_save_setting();
            theme_logf("theme: cycled to '%s' from %s", g_theme.name, e[j].path);
            return;
        }
        theme_logf("theme: skipping malformed '%s'", e[j].path);
    }
}

// -------------------------------------------------------
// Quality modes (section 7) — the mode itself; Phase 8 wires the effects
// -------------------------------------------------------
static bool s_playback = false;
static int  s_slow_frames = 0;

void quality_set(QualityMode m) {
    if (m < QUALITY_FULL)    m = QUALITY_FULL;
    if (m > QUALITY_MINIMAL) m = QUALITY_MINIMAL;
    g_quality = m;
    s_slow_frames = 0;
}

void quality_set_playback(bool active) {
    s_playback = active;
    s_slow_frames = 0;
}

// Playback wins every argument — section 7 says forced to MINIMAL, always.
QualityMode quality_effective(void) {
    return s_playback ? QUALITY_MINIMAL : g_quality;
}

// Auto-downgrade watchdog: 30 consecutive frames over 20ms outside playback
// drops one level.  Never climbs back automatically within a session, so a
// single rough patch cannot start the UI oscillating between tiers.
void quality_frame_tick(float frame_ms) {
    if (s_playback || g_quality >= QUALITY_MINIMAL) { s_slow_frames = 0; return; }

    if (frame_ms <= 20.0f) { s_slow_frames = 0; return; }

    if (++s_slow_frames < 30) return;

    s_slow_frames = 0;
    g_quality = (QualityMode)(g_quality + 1);
    theme_logf("quality: 30 frames over 20ms - dropped to %s",
               g_quality == QUALITY_REDUCED ? "REDUCED" : "MINIMAL");
}
