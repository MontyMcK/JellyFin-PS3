// Host tests for the experience pass's pure-C cores:
//   render/art_colour.h   artwork accent extraction + cache
//   render/buffer_anim.h  the playback buffering presentation
//   render/experience.h   version selector words, ambient screensaver
//
//   make -f Makefile.host test_experience && ./test_experience

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../source/ui/render/art_colour.h"
#include "../source/ui/render/buffer_anim.h"
#include "../source/ui/render/experience.h"
#include "../source/ui/render/peek.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define UI_ACCENT 0x008F6FE8u

static void hue_of(uint32_t c, float *h, float *s, float *l) { art_rgb_to_hsl(c, h, s, l); }
static float hue_dist(float a, float b) { float d = fabsf(a - b); return d > 0.5f ? 1.0f - d : d; }

// ---------------------------------------------------------------- artwork ---

static void fill(uint32_t *px, int w, int h, uint32_t c) { for (int i = 0; i < w * h; i++) px[i] = c; }

static void test_art_dominant(void) {
    const int W = 200, H = 300;
    uint32_t *px = malloc(sizeof(uint32_t) * W * H);
    // A mostly black poster with a red block in the middle: red must win over
    // the black that covers most of it.
    fill(px, W, H, 0x00080808);
    for (int y = 100; y < 200; y++) for (int x = 50; x < 150; x++) px[y * W + x] = 0x00C02020;
    art_palette p;
    int ok = art_palette_extract(px, W, H, W, UI_ACCENT, &p);
    float h, s, l; hue_of(p.accent, &h, &s, &l);
    CHECK(ok == 1 && p.valid == 1, "red block decided (ok=%d)", ok);
    CHECK(hue_dist(h, 0.0f) < 0.03f, "accent is red, hue=%.3f", h);
    CHECK(l > 0.55f && l < 0.70f, "accent lightness usable on dark UI, l=%.3f", l);
    { float dh, ds, dl; hue_of(p.deep, &dh, &ds, &dl); CHECK(dl < 0.22f && hue_dist(dh, h) < 0.02f, "deep shade dark, same hue"); }
    { float gh, gs, gl; hue_of(p.glow, &gh, &gs, &gl); CHECK(gl > 0.38f && gl < 0.52f, "glow shade mid"); }

    // Blue sky over a large grey city: blue wins over grey.
    fill(px, W, H, 0x00707070);
    for (int y = 0; y < 90; y++) for (int x = 0; x < W; x++) px[y * W + x] = 0x002060D0;
    ok = art_palette_extract(px, W, H, W, UI_ACCENT, &p);
    hue_of(p.accent, &h, &s, &l);
    CHECK(ok && hue_dist(h, 0.6f) < 0.05f, "blue sky wins, hue=%.3f", h);

    // Two colours: the more saturated, larger one wins.
    fill(px, W, H, 0x0030A040);                       // green everywhere
    for (int y = 0; y < 40; y++) for (int x = 0; x < W; x++) px[y * W + x] = 0x00E0C020;
    ok = art_palette_extract(px, W, H, W, UI_ACCENT, &p);
    hue_of(p.accent, &h, &s, &l);
    CHECK(ok && hue_dist(h, 0.36f) < 0.06f, "green majority wins, hue=%.3f", h);

    // Stride larger than width is honoured (padding is not sampled).
    {
        const int S = 256;
        uint32_t *pp = malloc(sizeof(uint32_t) * S * H);
        fill(pp, S, H, 0x0000FF00);                   // green padding
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) pp[y * S + x] = 0x00D02080;
        art_palette q; art_palette_extract(pp, W, H, S, UI_ACCENT, &q);
        float qh, qs, ql; hue_of(q.accent, &qh, &qs, &ql);
        CHECK(hue_dist(qh, 0.9f) < 0.06f, "stride respected, hue=%.3f", qh);
        free(pp);
    }
    free(px);
}

static void test_art_deterministic(void) {
    const int W = 173, H = 259;
    uint32_t *px = malloc(sizeof(uint32_t) * W * H);
    unsigned seed = 12345;
    for (int i = 0; i < W * H; i++) { seed = seed * 1103515245u + 12345u; px[i] = seed & 0x00FFFFFF; }
    art_palette a, b;
    art_palette_extract(px, W, H, W, UI_ACCENT, &a);
    art_palette_extract(px, W, H, W, UI_ACCENT, &b);
    CHECK(memcmp(&a, &b, sizeof a) == 0, "same pixels, same palette");
    // The alpha byte is ignored.
    for (int i = 0; i < W * H; i++) px[i] |= 0xFF000000u;
    art_palette_extract(px, W, H, W, UI_ACCENT, &b);
    CHECK(a.accent == b.accent && a.deep == b.deep, "top byte ignored");
    free(px);
}

static void test_art_fallback(void) {
    art_palette p;
    CHECK(art_palette_extract(NULL, 10, 10, 10, UI_ACCENT, &p) == 0 && p.accent == UI_ACCENT && !p.valid,
          "NULL pixels fall back to the UI accent");
    uint32_t one = 0x00FF0000;
    CHECK(art_palette_extract(&one, 0, 10, 10, UI_ACCENT, &p) == 0 && p.accent == UI_ACCENT, "zero width");
    CHECK(art_palette_extract(&one, 10, -1, 10, UI_ACCENT, &p) == 0, "negative height");
    CHECK(art_palette_extract(&one, 10, 1, 5, UI_ACCENT, &p) == 0, "stride < width is invalid");
    CHECK(art_palette_extract(&one, 1, 1, 1, UI_ACCENT, &p) == 1 && p.valid, "a 1x1 red image works");

    // Greyscale / black / white images: fallback.
    const int W = 64, H = 64;
    uint32_t px[64 * 64];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint32_t v = (uint32_t)(x * 4); px[y * W + x] = v << 16 | v << 8 | v; }
    CHECK(art_palette_extract(px, W, H, W, UI_ACCENT, &p) == 0 && p.accent == UI_ACCENT, "greyscale falls back");
    fill(px, W, H, 0);
    CHECK(art_palette_extract(px, W, H, W, UI_ACCENT, &p) == 0, "black falls back");
    fill(px, W, H, 0x00FFFFFF);
    CHECK(art_palette_extract(px, W, H, W, UI_ACCENT, &p) == 0, "white falls back");
    // A tiny coloured speck in a grey image is not enough to decide.
    fill(px, W, H, 0x00606060);
    px[32 * W + 32] = 0x00FF0000;
    CHECK(art_palette_extract(px, W, H, W, UI_ACCENT, &p) == 0, "a speck of colour is not a palette");

    // Fallback shades share the UI accent's hue.
    art_palette f = art_palette_fallback(UI_ACCENT);
    float ah, as, al, dh, ds, dl;
    hue_of(UI_ACCENT, &ah, &as, &al); hue_of(f.deep, &dh, &ds, &dl);
    CHECK(f.accent == UI_ACCENT && hue_dist(ah, dh) < 0.02f && dl < 0.22f, "fallback deep is the accent's hue");
}

static void test_art_cache(void) {
    static art_cache c;
    art_cache_reset(&c);
    uint32_t red[16 * 16], blue[16 * 16];
    fill(red, 16, 16, 0x00D02020); fill(blue, 16, 16, 0x002040D0);

    art_palette a = art_cache_lookup(&c, "item-a", red, 16, 16, 16, UI_ACCENT);
    CHECK(c.analyses == 1 && a.valid, "first lookup analyses");
    art_palette b = art_cache_lookup(&c, "item-a", blue, 16, 16, 16, UI_ACCENT);
    CHECK(c.analyses == 1 && a.accent == b.accent, "second lookup is cached (not re-analysed)");
    for (int i = 0; i < 1000; i++) art_cache_lookup(&c, "item-a", red, 16, 16, 16, UI_ACCENT);
    CHECK(c.analyses == 1, "1000 frames, still one analysis");

    // Not loaded yet: no pixels -> fallback, and NOT cached.
    art_palette n = art_cache_lookup(&c, "item-b", NULL, 0, 0, 0, UI_ACCENT);
    CHECK(!n.valid && n.accent == UI_ACCENT && c.analyses == 1, "missing art: fallback, no analysis");
    CHECK(!art_cache_get(&c, "item-b", NULL), "missing art is not cached");
    n = art_cache_lookup(&c, "item-b", blue, 16, 16, 16, UI_ACCENT);
    CHECK(n.valid && c.analyses == 2, "art that arrives later is analysed once");

    // A colourless image IS cached (as the fallback) so it is not re-analysed.
    uint32_t grey[16 * 16]; fill(grey, 16, 16, 0x00808080);
    art_cache_lookup(&c, "item-grey", grey, 16, 16, 16, UI_ACCENT);
    art_cache_lookup(&c, "item-grey", grey, 16, 16, 16, UI_ACCENT);
    CHECK(c.analyses == 3, "colourless art analysed once");

    // Empty id is never cached.
    art_cache_lookup(&c, "", red, 16, 16, 16, UI_ACCENT);
    art_cache_lookup(&c, "", red, 16, 16, 16, UI_ACCENT);
    CHECK(c.analyses == 5, "empty id: analysed each time, never stored");

    // LRU: fill beyond capacity; the recently used survives, the oldest goes.
    art_cache_reset(&c);
    char id[32];
    for (int i = 0; i < ART_CACHE_N; i++) { snprintf(id, sizeof id, "id%02d", i); art_cache_lookup(&c, id, red, 16, 16, 16, UI_ACCENT); }
    art_cache_get(&c, "id00", NULL);                   // touch the oldest
    art_cache_lookup(&c, "id-new", blue, 16, 16, 16, UI_ACCENT);
    CHECK(art_cache_get(&c, "id00", NULL), "touched entry survives eviction");
    CHECK(!art_cache_get(&c, "id01", NULL), "least recently used is evicted");
    CHECK(art_cache_get(&c, "id-new", NULL), "new entry present");

    // mix
    CHECK(art_mix(0x000000, 0xFFFFFF, 0.5f) == 0x808080, "mix midpoint");
    CHECK(art_mix(0x102030, 0x405060, 0.0f) == 0x102030 && art_mix(0x102030, 0x405060, 1.0f) == 0x405060, "mix ends");
}

// -------------------------------------------------------------- buffering ---

#define MS(x) ((uint64_t)(x) * 1000ull)

static int run_to_done(buf_anim *b, uint64_t *now, uint64_t step, uint64_t limit) {
    buf_frame f;
    for (uint64_t t = 0; t < limit; t += step) {
        *now += step;
        buf_anim_eval(b, *now, &f);
        if (f.done) return 1;
    }
    return 0;
}

static void test_buf_transitions(void) {
    buf_anim b; buf_frame f;
    uint64_t t = MS(1000);
    buf_anim_start(&b, t);
    buf_anim_eval(&b, t, &f);
    CHECK(b.phase == BUF_LOADING && f.pct == 0 && !f.done, "starts loading at 0%%");
    CHECK(f.ui_a < 0.01f, "fades in from nothing (no pop)");
    CHECK(f.arc > 0.1f && f.arc < 0.2f, "ring begins as a short arc (%.3f)", f.arc);

    // Loading: progress rises, ring rotates, displayed % eases and never drops.
    float last_angle = -1.0f; int last_pct = 0; int rotated = 0;
    for (int i = 1; i <= 200; i++) {
        t += MS(20);
        buf_anim_progress(&b, (float)i / 200.0f * 0.5f);
        if (i == 100) buf_anim_progress(&b, 0.1f);           // a wobble backwards
        buf_anim_eval(&b, t, &f);
        CHECK(f.pct >= last_pct, "percentage never decreases (%d < %d)", f.pct, last_pct);
        if (last_angle >= 0.0f && f.ring_angle != last_angle) rotated = 1;
        last_angle = f.ring_angle; last_pct = f.pct;
        CHECK(!f.done, "not done while loading");
        CHECK(f.ring_angle >= 0.0f && f.ring_angle < 1.0f, "angle in [0,1)");
    }
    CHECK(rotated, "ring rotates while loading");
    CHECK(f.pct >= 47 && f.pct <= 50, "displayed %% follows the real one (%d)", f.pct);
    CHECK(f.ui_a > 0.99f, "fully visible while loading");
    CHECK(f.mark_scale > 0.98f && f.mark_scale < 1.02f, "the mark's breath is subtle (%.3f)", f.mark_scale);

    // Ready: the arc closes, one pulse, fade, done.
    buf_anim_ready(&b, t);
    CHECK(b.phase == BUF_READY, "ready");
    float max_scale = 0.0f, min_ui = 1.0f, prev_angle = f.ring_angle, max_step = 0.0f;
    int closed = 0;
    for (int i = 0; i < 60; i++) {
        t += MS(10);
        buf_anim_eval(&b, t, &f);
        if (f.mark_scale > max_scale) max_scale = f.mark_scale;
        if (f.ui_a < min_ui) min_ui = f.ui_a;
        if (f.arc > 0.999f) closed = 1;
        float d = f.ring_angle - prev_angle; if (d < 0) d += 1.0f;
        if (d > max_step) max_step = d;
        prev_angle = f.ring_angle;
    }
    CHECK(closed, "the ring completes");
    CHECK(max_scale > 1.04f && max_scale < 1.09f, "one subtle final pulse (%.3f)", max_scale);
    CHECK(max_step < 0.01f, "rotation settles without a jolt (step %.4f)", max_step);
    CHECK(f.done && f.ui_a < 0.01f, "faded and done");
    CHECK(f.pct == 100, "ends at 100%% (%d)", f.pct);
    // Settled: angle no longer moves.
    float a1 = f.ring_angle; buf_anim_eval(&b, t + MS(500), &f);
    CHECK(fabsf(f.ring_angle - a1) < 1e-4f, "ring at rest after settling");
}

static void test_buf_immediate(void) {
    buf_anim b; buf_frame f;
    uint64_t t = MS(5000);
    buf_anim_start(&b, t);
    buf_anim_ready(&b, t + MS(16));                   // ready on the next frame
    buf_anim_eval(&b, t + MS(32), &f);
    CHECK(!f.done && f.ui_a > 0.0f, "an instant ready still shows (no one-frame flash)");
    uint64_t now = t + MS(16);
    int frames_visible = 0;
    for (;;) { now += MS(16); buf_anim_eval(&b, now, &f); if (f.done) break; if (f.ui_a > 0.2f) frames_visible++; }
    uint64_t total = now - t;
    CHECK(total >= BUF_MIN_SHOW_US, "held for the minimum presentation (%llu us)", (unsigned long long)total);
    CHECK(total <= BUF_MIN_SHOW_US + BUF_FADE_AT_US + BUF_FADE_US + MS(20), "and no longer (%llu us)", (unsigned long long)total);
    CHECK(frames_visible >= 10, "visible for several frames (%d)", frames_visible);
}

static void test_buf_long(void) {
    buf_anim b; buf_frame f;
    uint64_t t = 0;
    buf_anim_start(&b, t);
    // 90 s of loading at 20 ms frames with slow progress: never done, angle
    // keeps moving smoothly, no drift outside ranges.
    float prev = 0.0f, max_step = 0.0f;
    for (int i = 0; i < 4500; i++) {
        t += MS(20);
        buf_anim_progress(&b, (float)i / 4500.0f);
        buf_anim_eval(&b, t, &f);
        float d = f.ring_angle - prev; if (d < 0) d += 1.0f;
        if (i > 0 && d > max_step) max_step = d;
        prev = f.ring_angle;
        if (f.done) break;
        CHECK(f.arc <= 0.77f, "arc bounded while loading");
    }
    CHECK(!f.done, "a long load is never done by itself");
    CHECK(max_step < 0.0081f, "rotation step per 20 ms frame stays small (%.4f)", max_step);
    // Frame gaps (a blocked frame) do not break the angle (time-based).
    buf_anim_eval(&b, t + MS(3000), &f);
    CHECK(f.ring_angle >= 0.0f && f.ring_angle < 1.0f, "large dt safe");
    buf_anim_ready(&b, t + MS(3000));
    uint64_t now = t + MS(3000);
    CHECK(run_to_done(&b, &now, MS(20), MS(2000)), "outro completes after a long load");
    CHECK(now - (t + MS(3000)) <= BUF_FADE_AT_US + BUF_FADE_US + MS(20), "no min-hold after a long load");
}

static void test_buf_cancel(void) {
    buf_anim b; buf_frame f;
    uint64_t t = MS(100);
    buf_anim_start(&b, t);
    buf_anim_progress(&b, 0.3f);
    t += MS(800);
    buf_anim_eval(&b, t, &f);
    buf_anim_cancel(&b, t);
    CHECK(b.phase == BUF_CANCELLED, "cancelled");
    buf_anim_progress(&b, 0.9f);                       // ignored after cancel
    buf_anim_ready(&b, t);                             // ignored after cancel
    CHECK(b.phase == BUF_CANCELLED && b.target < 0.31f, "cancel is final");
    uint64_t now = t; float max_arc = 0;
    for (;;) { now += MS(10); buf_anim_eval(&b, now, &f); if (f.arc > max_arc) max_arc = f.arc; if (f.done) break; }
    CHECK(now - t <= BUF_CANCEL_US + MS(10), "cancel fades quickly");
    CHECK(max_arc < 0.99f, "cancel does not complete the ring");
    // Idle anim is done.
    buf_anim z; memset(&z, 0, sizeof z);
    CHECK(buf_anim_done(&z, 0), "idle is done");
}

static void test_buf_deterministic(void) {
    buf_anim a, b; buf_frame fa, fb;
    buf_anim_start(&a, 777); buf_anim_start(&b, 777);
    uint64_t t = 777;
    for (int i = 0; i < 300; i++) {
        t += MS(17);
        float p = (float)((i * 37) % 101) / 100.0f;
        buf_anim_progress(&a, p); buf_anim_progress(&b, p);
        if (i == 250) { buf_anim_ready(&a, t); buf_anim_ready(&b, t); }
        buf_anim_eval(&a, t, &fa); buf_anim_eval(&b, t, &fb);
        CHECK(memcmp(&fa, &fb, sizeof fa) == 0, "identical inputs, identical frame (i=%d)", i);
    }
}

// ---------------------------------------------------------- version picker ---

static void test_vpick(void) {
    char out[128];
    // one version
    CHECK(vpick_recommended(1) == 0 && !vpick_has_choice(1), "one version: no selector");
    CHECK(vpick_recommended(0) == -1 && !vpick_has_choice(0), "no versions");
    // multiple
    CHECK(vpick_recommended(3) == 0 && vpick_has_choice(3), "several: the first is recommended");

    vpick_audio_words("English - DTS-HD MA - 5.1 - Default", out, sizeof out);
    CHECK(strcmp(out, "English \xC2\xB7 DTS-HD MA \xC2\xB7 5.1") == 0, "audio words: '%s'", out);
    vpick_audio_words("Dolby TrueHD + Dolby Atmos 7.1", out, sizeof out);
    CHECK(strcmp(out, "Dolby TrueHD + Dolby Atmos 7.1") == 0, "single part kept: '%s'", out);
    vpick_audio_words("Default", out, sizeof out);
    CHECK(out[0] == 0, "only a flag -> empty");
    vpick_audio_words(" - AAC -  - Forced - Stereo", out, sizeof out);
    CHECK(strcmp(out, "AAC \xC2\xB7 Stereo") == 0, "empty parts and flags dropped: '%s'", out);

    // missing metadata
    vpick_audio_words(NULL, out, sizeof out);   CHECK(out[0] == 0, "NULL audio label");
    vpick_audio_words("", out, sizeof out);     CHECK(out[0] == 0, "empty audio label");
    vpick_name("", 1, out, sizeof out);         CHECK(strcmp(out, "Version 2") == 0, "empty name -> Version 2");
    vpick_name(NULL, 0, out, sizeof out);       CHECK(strcmp(out, "Version 1") == 0, "NULL name -> Version 1");
    vpick_name("  ", 4, out, sizeof out);       CHECK(strcmp(out, "Version 5") == 0, "blank name -> Version 5");
    vpick_name("Director's Cut", 1, out, sizeof out); CHECK(strcmp(out, "Director's Cut") == 0, "name kept");

    // Truncation never overruns and stays terminated.
    char small[8];
    vpick_audio_words("English - DTS-HD MA - 5.1", small, sizeof small);
    CHECK(strlen(small) < sizeof small, "truncated safely ('%s')", small);
}

// ---------------------------------------------------------------- ambient ---

static void test_ambient(void) {
    amb_state s; amb_frame f;
    uint64_t t = 0;
    amb_init(&s, t, 10000000ull);                       // 10 s for the test
    // Input keeps it off.
    for (int i = 0; i < 100; i++) { t += MS(100); CHECK(amb_tick(&s, t, i % 20 == 0) == 0, "input reaches the UI while off"); }
    CHECK(s.phase == AMB_OFF, "still off with regular input");
    // Idle -> IN -> ON.
    t += MS(10001); amb_tick(&s, t, 0);
    CHECK(s.phase == AMB_IN, "idle starts the dissolve");
    amb_eval(&s, t + MS(350), &f);
    CHECK(f.draw_ui && f.cover > 0.2f && f.cover < 0.8f, "UI dissolving under the cover");
    t += AMB_IN_US; amb_tick(&s, t, 0);
    CHECK(s.phase == AMB_ON, "then ambient");
    amb_eval(&s, t, &f);
    CHECK(!f.draw_ui && f.draw_ambient && fabsf(f.cover - AMB_COVER_FULL) < 0.01f, "handover under a full cover");
    amb_eval(&s, t + AMB_ON_LIFT_US + MS(300), &f);
    CHECK(fabsf(f.cover - AMB_COVER_ON) < 0.01f && f.art_a > 0.99f, "settled: wave dimmed, art visible");
    // Wake: the press is swallowed, the UI comes back.
    t += MS(5000);
    CHECK(amb_tick(&s, t, 1) == 1, "waking press swallowed");
    CHECK(s.phase == AMB_OUT, "dissolving back");
    amb_eval(&s, t + MS(150), &f);
    CHECK(!f.draw_ui && f.art_a < 1.0f, "art fades first");
    amb_eval(&s, t + AMB_OUT1_US + MS(10), &f);
    CHECK(f.draw_ui && f.cover > 0.8f, "UI returns under the cover");
    CHECK(amb_tick(&s, t + MS(200), 0) == 1, "still swallowing mid-dissolve");
    amb_tick(&s, t + AMB_OUT1_US + AMB_OUT2_US, 0);
    CHECK(s.phase == AMB_OFF, "back to normal");
    amb_eval(&s, t + AMB_OUT1_US + AMB_OUT2_US, &f);
    CHECK(f.draw_ui && f.cover == 0.0f, "no cover left");

    // Input during IN reverses smoothly from where the cover was.
    amb_init(&s, 0, 1000000ull);
    amb_tick(&s, MS(1000), 0);
    CHECK(s.phase == AMB_IN, "in");
    amb_eval(&s, MS(1350), &f); float c = f.cover;
    CHECK(amb_tick(&s, MS(1350), 1) == 1 && s.phase == AMB_OUT, "interrupted");
    amb_eval(&s, MS(1350), &f);
    CHECK(f.draw_ui && fabsf(f.cover - c) < 0.01f, "no jump in the cover (%.3f vs %.3f)", f.cover, c);
    amb_tick(&s, MS(1350) + AMB_OUT2_US, 0);
    CHECK(s.phase == AMB_OFF, "interrupted dissolve ends quickly");

    // idle 0 = never.
    amb_init(&s, 0, 0);
    amb_tick(&s, 1000000000000ull, 0);
    CHECK(s.phase == AMB_OFF, "disabled never starts");

    // Slides: deterministic, rotate, stay in bounds.
    amb_slide a, b;
    amb_slide_at(0, 0, &a); CHECK(a.idx == -1, "no candidates");
    amb_slide_at(MS(8000), 1, &a); CHECK(a.idx == 0 && a.a == 1.0f, "one candidate stays up");
    amb_slide_at(MS(1000), 3, &a); amb_slide_at(MS(1000), 3, &b);
    CHECK(memcmp(&a, &b, sizeof a) == 0, "slide deterministic");
    amb_slide_at(AMB_SLIDE_US + MS(8000), 3, &a); CHECK(a.idx == 1, "next slide");
    amb_slide_at(3 * AMB_SLIDE_US + MS(8000), 3, &a); CHECK(a.idx == 0, "wraps");
    amb_slide_at(MS(8000), 3, &a); CHECK(a.a > 0.99f, "mid-slide fully visible");
    amb_slide_at(0, 3, &a); CHECK(a.a < 0.01f, "slide fades in");
    for (uint64_t tt = 0; tt < 5 * AMB_SLIDE_US; tt += MS(250)) {
        amb_slide_at(tt, 4, &a);
        CHECK(fabsf(a.dx) <= 18.0f && fabsf(a.dy) <= 6.0f && a.scale >= 1.0f && a.scale <= 1.04f, "drift bounded");
    }
}

// ------------------------------------------------------------------- peek ---

static void test_peek(void) {
    peek_anim a; memset(&a, 0, sizeof a);
    const peek_rect card = { 100, 200, 150, 225 };
    const peek_rect pan  = peek_panel(640, 360, 450);
    CHECK(fabsf(pan.w / pan.h - 4.0f / 3.0f) < 1e-4f, "panel is 4:3");
    CHECK(peek_progress(&a, 0) == 0.0f && peek_tick(&a, 0) == PEEK_OFF, "starts off");

    for (int i = 0; i <= 100; i++) {
        const float e = i / 100.0f;
        CHECK(fabsf(peek_sinpi(e) - sinf(3.14159265f * e)) < 0.002f, "sinpi %.2f", e);
        CHECK(fabsf(peek_abscospi(e) - fabsf(cosf(3.14159265f * e))) < 0.002f, "abscospi %.2f", e);
    }

    uint64_t t = 1000000;
    peek_open(&a, t);
    peek_frame f = peek_eval(peek_progress(&a, t), card, pan);
    CHECK(fabsf(f.r.x - card.x) < 0.5f && fabsf(f.r.w - card.w) < 0.5f && !f.back,
          "opens from the card, front up");
    int swaps = 0, last_back = 0; float min_w = 1e9f, last_lift = -1;
    for (int i = 1; i <= 60; i++) {
        t += 10000;
        peek_tick(&a, t);
        f = peek_eval(peek_progress(&a, t), card, pan);
        if (f.back != last_back) { swaps++; last_back = f.back; }
        if (f.r.w < min_w) min_w = f.r.w;
        CHECK(f.lift >= last_lift, "lift monotonic");
        last_lift = f.lift;
        CHECK(f.r.w >= 0.0f && f.r.h > 0.0f, "sane rect");
    }
    CHECK(swaps == 1, "exactly one face swap (%d)", swaps);
    CHECK(min_w < 20.0f, "edge-on at the turn (%.1f)", min_w);
    CHECK(a.phase == PEEK_OPEN, "open after the open time");
    CHECK(fabsf(f.r.x - pan.x) < 0.5f && fabsf(f.r.w - pan.w) < 0.5f && fabsf(f.r.h - pan.h) < 0.5f,
          "lands exactly on the panel");
    CHECK(f.back && f.content_a > 0.99f, "back face, text in");
    CHECK(peek_eval(0.6f, card, pan).content_a == 0.0f, "no text mid-turn");

    peek_close(&a, t);
    t += 60000;
    f = peek_eval(peek_progress(&a, t), card, pan);
    CHECK(f.content_a < 1.0f, "text leaves as it closes");
    for (int i = 0; i < 60; i++) { t += 10000; peek_tick(&a, t); }
    CHECK(a.phase == PEEK_OFF, "closed");
    f = peek_eval(peek_progress(&a, t), card, pan);
    CHECK(fabsf(f.r.x - card.x) < 0.5f && !f.back, "lands back in the grid");

    peek_open(&a, 5000000);
    const float u_mid = peek_progress(&a, 5000000 + 200000);
    const peek_frame before = peek_eval(u_mid, card, pan);
    peek_close(&a, 5000000 + 200000);
    const peek_frame after = peek_eval(peek_progress(&a, 5000000 + 200000), card, pan);
    CHECK(fabsf(before.r.x - after.r.x) < 0.01f && fabsf(before.r.w - after.r.w) < 0.01f,
          "close mid-open is continuous");
    const uint64_t t2 = 5000000 + 300000;
    const float u2 = peek_progress(&a, t2);
    peek_open(&a, t2);
    CHECK(fabsf(peek_progress(&a, t2) - u2) < 1e-6f, "open mid-close is continuous");
    peek_anim z; memset(&z, 0, sizeof z); peek_close(&z, 1); CHECK(z.phase == PEEK_OFF, "close when off");
}

int main(void) {
    test_art_dominant();
    test_art_deterministic();
    test_art_fallback();
    test_art_cache();
    test_buf_transitions();
    test_buf_immediate();
    test_buf_long();
    test_buf_cancel();
    test_buf_deterministic();
    test_vpick();
    test_ambient();
    test_peek();
    printf("test_experience: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
