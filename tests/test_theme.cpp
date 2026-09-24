// Host-side test for the runtime theming parser (source/ui/theme.cpp).
//
// Compiles the SAME theme.cpp the PS3 build compiles, against the SAME .ini
// files that ship in themes/, and cross-checks both against the values in
// design_handoff_ps3_xmb_revamp/README.md section 2.1.
//
// The point of the cross-check: the two built-in themes in theme.cpp are hand
// transcribed from that table, and the .ini files are the design's own. If a
// hex digit was mistyped in either, or if the inline-comment stripping breaks
// ("accent = AA5CC3  ; focus glow, ..." is how every colour line in the
// shipped files is written), these tests fail loudly instead of the console
// quietly drawing a wrong colour.
//
// Build/run:  make -f Makefile.host test_theme && ./test_theme

#include <stdio.h>
#include <string.h>

// ---- stand-ins for the PS3 side, before theme.cpp is pulled in ----
static char s_log[64][256];
static int  s_log_n = 0;
void plog(const char *msg) {
    if (s_log_n < 64) snprintf(s_log[s_log_n++], 256, "%s", msg);
}
void plog_start(void) {}
void plog_stop(void) {}
bool plog_enabled(void) { return true; }
void plog_set_enabled(bool) {}
void plog_load_setting(void) {}
void crash_log(const char *) {}

static char s_data_dir[512] = ".";
const char *jf_data_path(const char *name) {
    static char buf[768];
    snprintf(buf, sizeof(buf), "%s/%s", s_data_dir, name);
    return buf;
}

#include "../source/ui/theme.cpp"

// ---- tiny harness ----
static int g_fail = 0;

static void ck_u32(const char *what, u32 got, u32 want) {
    if (got == want) return;
    printf("  FAIL %-28s got %06X want %06X\n", what, got, want);
    g_fail++;
}

static void ck_int(const char *what, int got, int want) {
    if (got == want) return;
    printf("  FAIL %-28s got %d want %d\n", what, got, want);
    g_fail++;
}

static void ck_true(const char *what, bool cond) {
    if (cond) return;
    printf("  FAIL %s\n", what);
    g_fail++;
}

static bool write_file(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(body, f);
    fclose(f);
    return true;
}

// theme_load() takes a path, so probe bodies go through a scratch file.
static bool load_from_string(const char *body) {
    if (!write_file("tmp_probe.ini", body)) return false;
    bool r = theme_load("tmp_probe.ini");
    remove("tmp_probe.ini");
    return r;
}

// README section 2.1, XMB wave column — typed from the document, not from
// theme.cpp, so this is a genuine second source.
static void test_builtin_xmb_wave(void) {
    printf("built-in 0 (XMB wave) vs README section 2.1\n");
    theme_apply_builtin(0);
    ck_u32("bg_top",        g_theme.bg_top,        0x151A38);
    ck_u32("bg_bot",        g_theme.bg_bot,        0x05060C);
    ck_u32("accent",        g_theme.accent,        0xAA5CC3);
    ck_u32("accent_alt",    g_theme.accent_alt,    0x00A4DC);
    ck_u32("accent_deep",   g_theme.accent_deep,   0x8A47A9);
    ck_u32("wordmark",      g_theme.wordmark,      0x00A4DC);
    ck_u32("lk_mark_a",     g_theme.lk_mark_a,     0xAA5CC3);
    ck_u32("lk_mark_b",     g_theme.lk_mark_b,     0x00A4DC);
    ck_u32("lk_word_a",     g_theme.lk_word_a,     0x00A4DC);
    ck_u32("lk_word_b",     g_theme.lk_word_b,     0x4189D3);
    ck_u32("lk_word_c",     g_theme.lk_word_c,     0x7A70CA);
    ck_u32("lk_word_d",     g_theme.lk_word_d,     0xAA5CC3);
    ck_u32("text",          g_theme.text,          0xE9EBF5);
    ck_u32("text_dim",      g_theme.text_dim,      0x99A0BC);
    ck_u32("text_faint",    g_theme.text_faint,    0x5C6386);
    ck_u32("panel",         g_theme.panel,         0x191E3C);
    ck_u32("panel_hi",      g_theme.panel_hi,      0x232950);
    ck_u32("hairline",      g_theme.hairline,      0x2C3258);
    ck_u32("thumb_dim",     g_theme.thumb_dim,     0x12162C);
    ck_u32("track",         g_theme.track,         0x10142A);
    ck_u32("icon_idle",     g_theme.icon_idle,     0x646C96);
    ck_u32("white",         g_theme.white,         0xFFFFFF);
    ck_int("card_radius",   g_theme.card_radius,   4);
    ck_int("chip_radius",   g_theme.chip_radius,   999);
    ck_int("glow_alpha",    g_theme.glow_alpha,    115);
    ck_int("wave_alpha",    g_theme.wave_alpha,    28);
    ck_int("trim_alpha",    g_theme.trim_alpha,    0);
    ck_int("ramp_steps",    g_theme.ramp_steps,    12);
}

static void test_builtin_golden_age(void) {
    printf("built-in 1 (Golden Age) vs README section 2.1\n");
    theme_apply_builtin(1);
    ck_u32("bg_top",        g_theme.bg_top,        0x17120E);
    ck_u32("bg_bot",        g_theme.bg_bot,        0x040303);
    ck_u32("accent",        g_theme.accent,        0xE0A13C);
    ck_u32("accent_alt",    g_theme.accent_alt,    0xC8332E);
    ck_u32("accent_deep",   g_theme.accent_deep,   0x8A5F1E);
    ck_u32("wordmark",      g_theme.wordmark,      0xE8B45C);
    ck_u32("text",          g_theme.text,          0xF4EDE1);
    ck_u32("text_dim",      g_theme.text_dim,      0xB3A695);
    ck_u32("text_faint",    g_theme.text_faint,    0x6D6353);
    ck_u32("panel",         g_theme.panel,         0x1A1410);
    ck_u32("panel_hi",      g_theme.panel_hi,      0x271F17);
    ck_u32("hairline",      g_theme.hairline,      0x3A2E22);
    ck_u32("thumb_dim",     g_theme.thumb_dim,     0x1D1611);
    ck_u32("track",         g_theme.track,         0x221A13);
    ck_u32("icon_idle",     g_theme.icon_idle,     0x7A6D5C);
    ck_u32("white",         g_theme.white,         0xFFFAF2);
    ck_u32("trim",          g_theme.trim,          0xC9A66B);
    ck_int("card_radius",   g_theme.card_radius,   0);
    ck_int("chip_radius",   g_theme.chip_radius,   0);
    ck_int("glow_alpha",    g_theme.glow_alpha,    87);
    ck_int("wave_alpha",    g_theme.wave_alpha,    0);
    ck_int("trim_alpha",    g_theme.trim_alpha,    77);
    ck_int("ramp_steps",    g_theme.ramp_steps,    0);
}

// The strongest check: load the design's own .ini and require it to agree with
// the built-in transcribed from the same table.  Catches both a mistyped
// built-in and a parser that mangles the inline `;` comments.
static void test_ini_matches_builtin(const char *path, int builtin, bool has_lockup) {
    printf("%s vs built-in %d\n", path, builtin);

    Theme want;
    theme_apply_builtin(builtin);
    want = g_theme;

    theme_apply_builtin(0);              // start from something else on purpose
    if (!theme_load(path)) {
        printf("  FAIL theme_load(%s) returned false\n", path);
        g_fail++;
        return;
    }

    ck_u32("bg_top",      g_theme.bg_top,      want.bg_top);
    ck_u32("bg_bot",      g_theme.bg_bot,      want.bg_bot);
    ck_u32("accent",      g_theme.accent,      want.accent);
    ck_u32("accent_alt",  g_theme.accent_alt,  want.accent_alt);
    ck_u32("accent_deep", g_theme.accent_deep, want.accent_deep);
    ck_u32("wordmark",    g_theme.wordmark,    want.wordmark);
    ck_u32("text",        g_theme.text,        want.text);
    ck_u32("text_dim",    g_theme.text_dim,    want.text_dim);
    ck_u32("text_faint",  g_theme.text_faint,  want.text_faint);
    ck_u32("panel",       g_theme.panel,       want.panel);
    ck_u32("panel_hi",    g_theme.panel_hi,    want.panel_hi);
    ck_u32("hairline",    g_theme.hairline,    want.hairline);
    ck_u32("thumb_dim",   g_theme.thumb_dim,   want.thumb_dim);
    ck_u32("track",       g_theme.track,       want.track);
    ck_u32("icon_idle",   g_theme.icon_idle,   want.icon_idle);
    ck_u32("white",       g_theme.white,       want.white);
    ck_u32("trim",        g_theme.trim,        want.trim);
    ck_int("card_radius", g_theme.card_radius, want.card_radius);
    ck_int("chip_radius", g_theme.chip_radius, want.chip_radius);
    ck_int("glow_alpha",  g_theme.glow_alpha,  want.glow_alpha);
    ck_int("wave_alpha",  g_theme.wave_alpha,  want.wave_alpha);
    ck_int("trim_alpha",  g_theme.trim_alpha,  want.trim_alpha);
    ck_int("ramp_steps",  g_theme.ramp_steps,  want.ramp_steps);
    ck_true("name parsed from [theme]", g_theme.name[0] != '\0');

    if (has_lockup) {
        ck_u32("lk_mark_a", g_theme.lk_mark_a, want.lk_mark_a);
        ck_u32("lk_mark_b", g_theme.lk_mark_b, want.lk_mark_b);
        ck_u32("lk_word_a", g_theme.lk_word_a, want.lk_word_a);
        ck_u32("lk_word_b", g_theme.lk_word_b, want.lk_word_b);
        ck_u32("lk_word_c", g_theme.lk_word_c, want.lk_word_c);
        ck_u32("lk_word_d", g_theme.lk_word_d, want.lk_word_d);
    }
}

// golden-age.ini carries none of bg / key_* , so they must be derived from
// that file's own palette rather than inherited from XMB wave.
static void test_optional_token_derivation(void) {
    printf("optional tokens derive from the file's own palette\n");
    theme_apply_builtin(0);
    if (!theme_load("../themes/golden-age.ini")) {
        printf("  FAIL could not load golden-age.ini\n"); g_fail++; return;
    }
    ck_u32("bg <- bg_bot",            g_theme.bg,            g_theme.bg_bot);
    ck_u32("key_normal <- panel",     g_theme.key_normal,    g_theme.panel);
    ck_u32("key_sel <- white",        g_theme.key_sel,       g_theme.white);
    ck_u32("key_label_sel <- track",  g_theme.key_label_sel, g_theme.track);
    ck_true("did not inherit XMB wave bg", g_theme.bg != 0x0B0E1E);
}

static void test_malformed_refused(void) {
    printf("malformed files are refused whole\n");

    struct { const char *what; const char *body; } bad[] = {
        { "short hex",      "[colors]\naccent = AA5C\n" },
        { "non-hex digit",  "[colors]\naccent = GGGGGG\n" },
        { "no equals",      "[colors]\naccent AA5CC3\n" },
        { "unclosed [",     "[colors\naccent = AA5CC3\n" },
        { "empty key",      "[colors]\n = AA5CC3\n" },
        { "scalar range",   "[style]\nglow_alpha = 300\n" },
        { "scalar garbage", "[style]\nwave_alpha = soon\n" },
    };

    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        theme_apply_builtin(1);                       // Golden Age is live
        if (!write_file("tmp_bad.ini", bad[i].body)) continue;
        bool loaded = theme_load("tmp_bad.ini");
        remove("tmp_bad.ini");

        if (loaded) {
            printf("  FAIL %s was accepted\n", bad[i].what);
            g_fail++;
        }
        // and the previous theme must still be standing, untouched
        if (g_theme.accent != 0xE0A13C) {
            printf("  FAIL %s clobbered the live theme (accent %06X)\n",
                   bad[i].what, g_theme.accent);
            g_fail++;
        }
    }
}

static void test_tolerances(void) {
    printf("tolerated input\n");

    theme_apply_builtin(0);
    ck_true("unknown key does not refuse the file",
            load_from_string("[theme]\nname = Probe\n"
                                          "[colors]\naccent = 112233\n"
                                          "future_token = 445566\n"));
    ck_u32("accent still parsed", g_theme.accent, 0x112233);

    theme_apply_builtin(0);
    ck_true("inline comment + tabs + CRLF",
            load_from_string("[colors]\r\n"
                                          "\taccent\t=\tAA5CC3\t; trailing note\r\n"
                                          "panel = 191E3C   # hash comment\r\n"));
    ck_u32("accent through comment", g_theme.accent, 0xAA5CC3);
    ck_u32("panel through comment",  g_theme.panel,  0x191E3C);

    theme_apply_builtin(0);
    ck_true("lowercase hex", load_from_string("[colors]\naccent = aa5cc3\n"));
    ck_u32("lowercase parsed", g_theme.accent, 0xAA5CC3);

    theme_apply_builtin(0);
    ck_true("blank file loads (all defaults kept)",
            load_from_string("\n\n; only a comment\n\n"));
    ck_u32("kept built-in accent", g_theme.accent, 0xAA5CC3);
}

// Tokens added by the 2026-09-20 design revision.  scrim/scrim_2 are the first
// with an alpha, spelled "RRGGBB,0.90" in the .ini.
static void test_focus_and_scrim(void) {
    printf("focus_ring + scrim tokens (2026-09-20 revision)\n");

    theme_apply_builtin(0);
    ck_u32("built-in focus_ring", g_theme.focus_ring, 0xE4EBFA);
    // detail_band is NOT a handoff token: it preserves the value ui_info.cpp
    // had hardcoded, so the shipping theme must be byte-identical to before.
    ck_u32("built-in detail_band", g_theme.detail_band, 0x412C73);
    ck_u32("built-in scrim",      g_theme.scrim,      0x05060C);
    ck_u32("built-in scrim_2",    g_theme.scrim_2,    0x05060C);
    ck_int("built-in scrim_a",    g_theme.scrim_a,    230);   // 0.90 * 255
    ck_int("built-in scrim_2_a",  g_theme.scrim_2_a,  168);   // 0.66 * 255

    // The shipped .ini must agree with the built-in transcribed from it.
    theme_apply_builtin(1);
    if (!theme_load("../themes/xmb-wave.ini")) {
        printf("  FAIL xmb-wave.ini did not load\n"); g_fail++; return;
    }
    ck_u32("ini focus_ring", g_theme.focus_ring, 0xE4EBFA);
    ck_u32("ini detail_band", g_theme.detail_band, 0x412C73);
    ck_u32("ini scrim",      g_theme.scrim,      0x05060C);
    ck_u32("ini scrim_2",    g_theme.scrim_2,    0x05060C);
    ck_int("ini scrim_a",    g_theme.scrim_a,    230);
    ck_int("ini scrim_2_a",  g_theme.scrim_2_a,  168);

    // A theme predating the revision has no focus_ring; it must fall back to
    // pure white, which is what the ring was before.
    theme_apply_builtin(0);
    ck_true("pre-revision theme loads",
            load_from_string("[colors]\nwhite = FFEEDD\naccent = 112233\n"));
    ck_u32("focus_ring falls back to white", g_theme.focus_ring, 0xFFEEDD);

    // Alpha spellings that must work.
    theme_apply_builtin(0);
    ck_true("alpha 1.0",  load_from_string("[colors]\nscrim = 102030,1.0\n"));
    ck_int("alpha 1.0 -> 255", g_theme.scrim_a, 255);
    theme_apply_builtin(0);
    ck_true("alpha .5",   load_from_string("[colors]\nscrim = 102030,.5\n"));
    ck_int("alpha .5 -> 128", g_theme.scrim_a, 128);
    theme_apply_builtin(0);
    ck_true("no alpha",   load_from_string("[colors]\nscrim = 102030\n"));
    ck_int("bare colour -> 255", g_theme.scrim_a, 255);
    ck_u32("bare colour parsed",  g_theme.scrim, 0x102030);

    // ...and spellings that must be refused, leaving the live theme standing.
    const char *bad[] = {
        "[colors]\nscrim = 102030,1.5\n",      // alpha out of range
        "[colors]\nscrim = 102030,-0.2\n",     // negative
        "[colors]\nscrim = 102030,\n",         // comma, no alpha
        "[colors]\nscrim = 102030,abc\n",      // alpha not a number
        "[colors]\nscrim = 1020,0.5\n",        // short colour
        "[colors]\nscrim = 102030,0.5x\n",     // trailing junk
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        theme_apply_builtin(1);                       // Golden Age live
        if (load_from_string(bad[i])) {
            printf("  FAIL accepted bad scrim: %s", bad[i]);
            g_fail++;
        }
        if (g_theme.accent != 0xE0A13C) {
            printf("  FAIL bad scrim clobbered the live theme\n");
            g_fail++;
        }
    }
}

static void test_scan_finds_files(void) {
    printf("theme_scan lists built-ins then files\n");
    ThemeEntry e[THEME_MAX_ENTRIES];
    int n = theme_scan(e, THEME_MAX_ENTRIES);
    ck_true("at least the two built-ins", n >= 2);
    ck_true("slot 0 is XMB wave",   strcmp(e[0].name, "XMB wave") == 0);
    ck_true("slot 1 is Golden Age", strcmp(e[1].name, "Golden Age") == 0);
    ck_true("built-ins have no path", e[0].path[0] == '\0' && e[1].path[0] == '\0');
}

// Settings > Theme cycles through theme_scan() order and persists the choice.
// On the host, theme_scan finds only the two built-ins (the PS3 theme dirs do
// not exist here), so this exercises wrap and persistence.
static void test_theme_cycle(void) {
    printf("theme_cycle\n");

    theme_apply_builtin(0);
    ck_true("starts on XMB wave", strcmp(g_theme.name, "XMB wave") == 0);

    theme_cycle();
    ck_true("cycles to Golden Age", strcmp(g_theme.name, "Golden Age") == 0);
    ck_u32("and the palette really changed", g_theme.accent, 0xE0A13C);

    theme_cycle();
    ck_true("wraps back to XMB wave", strcmp(g_theme.name, "XMB wave") == 0);
    ck_u32("palette restored", g_theme.accent, 0xAA5CC3);

    // The choice is persisted by NAME, and reloading it must land back on the
    // same theme rather than an index that moved.
    theme_cycle();                                   // -> Golden Age, saved
    theme_apply_builtin(0);                          // stomp the live palette
    theme_load_setting();                            // read the saved name back
    ck_true("saved choice restores by name",
            strcmp(g_theme.name, "Golden Age") == 0);

    remove("jellyfin_theme.txt");
}

static void test_quality_watchdog(void) {
    printf("quality watchdog\n");

    quality_set(QUALITY_FULL);
    quality_set_playback(false);
    for (int i = 0; i < 29; i++) quality_frame_tick(25.0f);
    ck_int("29 slow frames do not trip", (int)g_quality, (int)QUALITY_FULL);
    quality_frame_tick(25.0f);
    ck_int("30th slow frame drops a level", (int)g_quality, (int)QUALITY_REDUCED);

    // A fast frame resets the run, so a single rough patch cannot cascade.
    for (int i = 0; i < 29; i++) quality_frame_tick(25.0f);
    quality_frame_tick(5.0f);
    for (int i = 0; i < 29; i++) quality_frame_tick(25.0f);
    ck_int("run reset by a fast frame", (int)g_quality, (int)QUALITY_REDUCED);

    // Never climbs back on its own.
    for (int i = 0; i < 600; i++) quality_frame_tick(2.0f);
    ck_int("no automatic recovery", (int)g_quality, (int)QUALITY_REDUCED);

    // Playback forces MINIMAL and suspends the watchdog.
    quality_set(QUALITY_FULL);
    quality_set_playback(true);
    ck_int("playback forces MINIMAL", (int)quality_effective(), (int)QUALITY_MINIMAL);
    for (int i = 0; i < 120; i++) quality_frame_tick(40.0f);
    ck_int("watchdog idle during playback", (int)g_quality, (int)QUALITY_FULL);
    quality_set_playback(false);
    ck_int("restores the chosen mode after playback",
           (int)quality_effective(), (int)QUALITY_FULL);
}

int main(void) {
    test_builtin_xmb_wave();
    test_builtin_golden_age();
    test_ini_matches_builtin("../themes/xmb-wave.ini",   0, true);
    test_ini_matches_builtin("../themes/golden-age.ini", 1, false);
    test_optional_token_derivation();
    test_malformed_refused();
    test_tolerances();
    test_focus_and_scrim();
    test_scan_finds_files();
    test_theme_cycle();
    test_quality_watchdog();

    if (g_fail) { printf("\ntest_theme: %d FAILURES\n", g_fail); return 1; }
    printf("\ntest_theme: all checks passed\n");
    return 0;
}
