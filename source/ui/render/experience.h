// Small pure-C pieces of the experience pass, host-tested in
// tests/test_experience.c:
//
//   * the version selector's words (vpick_*): which version is the
//     recommended one and how it is described -- from metadata the client
//     already has, nothing invented;
//   * the ambient screensaver's state (amb_*): when it starts, how the UI
//     dissolves into it and back, and how the artwork drifts.
//
// No clock, no drawing, no allocation: the caller passes timestamps in.

#ifndef JF_EXPERIENCE_H
#define JF_EXPERIENCE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline float exp_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float exp_smooth(float t) {
    t = exp_clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// =============================================================================
// Version selector
// =============================================================================
//
// The existing mechanism plays source[0] unless the user picks another: that
// is Jellyfin's own order (the item's default version first), and it is what
// detail's Play already does.  So "recommended" is source[0] -- the selector
// only says so clearly.  No resolution, frame-rate or codec is inferred that
// the source does not state: the description is the version's own Name plus
// its default audio track's DisplayTitle, reworded.

// Index of the recommended version, or -1 when there are none.
static inline int vpick_recommended(int n_sources) { return n_sources > 0 ? 0 : -1; }

// Only worth a selector when there is a real choice.
static inline int vpick_has_choice(int n_sources) { return n_sources > 1; }

// A version's display name; an empty one becomes "Version N" (1-based).
static inline void vpick_name(const char *label, int idx, char *out, size_t cap) {
    if (!cap) return;
    while (label && (*label == ' ' || *label == '\t')) label++;
    if (label && label[0]) snprintf(out, cap, "%s", label);
    else                   snprintf(out, cap, "Version %d", idx + 1);
}

// "English - DTS-HD MA - 5.1 - Default" -> "English \xC2\xB7 DTS-HD MA \xC2\xB7 5.1".
// Jellyfin's DisplayTitle joins its parts with " - "; the words kept are the
// ones it gave, minus the Default / Forced / External flags, which describe
// the track's status rather than the audio.  Empty in -> empty out.
static inline void vpick_audio_words(const char *label, char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (!label) return;
    size_t o = 0;
    const char *p = label;
    while (*p) {
        const char *sep = strstr(p, " - ");
        const size_t len = sep ? (size_t)(sep - p) : strlen(p);
        // trim
        const char *s = p; size_t l = len;
        while (l && (*s == ' ')) { s++; l--; }
        while (l && s[l - 1] == ' ') l--;
        const int flag = (l == 7 && strncmp(s, "Default", 7) == 0) ||
                         (l == 6 && strncmp(s, "Forced", 6) == 0) ||
                         (l == 8 && strncmp(s, "External", 8) == 0);
        if (l && !flag) {
            const char *dot = "\xC2\xB7";
            if (o && o + 4 < cap) { out[o++] = ' '; memcpy(out + o, dot, 2); o += 2; out[o++] = ' '; }
            for (size_t i = 0; i < l && o + 1 < cap; i++) out[o++] = s[i];
            out[o] = 0;
        }
        if (!sep) break;
        p = sep + 3;
    }
    out[o < cap ? o : cap - 1] = 0;
}

// The recommended version's one-line description: its audio words when the
// version reports an audio track, else nothing (the name alone is shown).
static inline void vpick_describe(const char *audio_label, char *out, size_t cap) {
    vpick_audio_words(audio_label, out, cap);
}

// =============================================================================
// Ambient screensaver
// =============================================================================
//
// OFF -> (idle for amb_idle_us) -> IN: the UI dissolves under a dark cover.
// IN  -> ON: the UI is no longer drawn; the cover lifts part way so the wave
//        shows again, dimmed, and artwork drifts in over it.
// ON  -> (any input) -> OUT: the artwork fades and the cover closes, then the
//        UI is drawn again under a cover that lifts.  The waking press is
//        swallowed: it only brings the UI back.
// IN  -> (any input) -> straight to the second half of OUT from wherever the
//        cover had got to, so an interrupted dissolve reverses smoothly.

#define AMB_IDLE_US_DEFAULT  180000000ull   // 3 minutes
#define AMB_IN_US               700000ull
#define AMB_ON_LIFT_US         1600000ull
#define AMB_OUT1_US             300000ull   // artwork out, cover closes
#define AMB_OUT2_US             400000ull   // UI back, cover lifts
#define AMB_COVER_FULL          0.94f
#define AMB_COVER_ON            0.58f
#define AMB_SLIDE_US          16000000ull   // one artwork's time on screen
#define AMB_XFADE_US           1800000ull   // its fade in / out

enum { AMB_OFF = 0, AMB_IN, AMB_ON, AMB_OUT };

typedef struct {
    int      phase;
    uint64_t idle_us;       // threshold; 0 = never
    uint64_t t_input;       // last input
    uint64_t t_phase;       // current phase began
    uint64_t t_on;          // ON began (the drift clock)
    float    out_cover0;    // cover when OUT began
    int      out_skip1;     // OUT began from IN: no artwork to fade
} amb_state;

typedef struct {
    int   draw_ui;          // draw the normal screen this frame
    int   draw_ambient;     // draw the ambient layer this frame
    float cover;            // black over everything below the ambient layer
    float art_a;            // ambient layer opacity
} amb_frame;

static inline void amb_init(amb_state *s, uint64_t now, uint64_t idle_us) {
    memset(s, 0, sizeof *s);
    s->phase = AMB_OFF; s->idle_us = idle_us; s->t_input = now; s->t_phase = now;
}

static inline float amb_cover_at(const amb_state *s, uint64_t now);

// Advance.  Returns 1 when this frame's input must NOT reach the UI (it woke
// the screensaver, or the screensaver is still dissolving away).
static inline int amb_tick(amb_state *s, uint64_t now, int any_input) {
    if (any_input) {
        s->t_input = now;
        if (s->phase == AMB_ON) {
            s->out_cover0 = amb_cover_at(s, now);
            s->out_skip1  = 0;
            s->phase = AMB_OUT; s->t_phase = now;
            return 1;
        }
        if (s->phase == AMB_IN) {
            s->out_cover0 = amb_cover_at(s, now);
            s->out_skip1  = 1;
            s->phase = AMB_OUT; s->t_phase = now;
            return 1;
        }
    }
    switch (s->phase) {
    case AMB_OFF:
        if (s->idle_us && now - s->t_input >= s->idle_us) { s->phase = AMB_IN; s->t_phase = now; }
        return 0;
    case AMB_IN:
        if (now - s->t_phase >= AMB_IN_US) { s->phase = AMB_ON; s->t_phase = now; s->t_on = now; }
        return 0;
    case AMB_OUT: {
        const uint64_t d = s->out_skip1 ? AMB_OUT2_US : AMB_OUT1_US + AMB_OUT2_US;
        if (now - s->t_phase >= d) { s->phase = AMB_OFF; s->t_phase = now; return 0; }
        return 1;
    }
    default:
        return 0;
    }
}

static inline float amb_cover_at(const amb_state *s, uint64_t now) {
    const float t = (float)(now - s->t_phase);
    switch (s->phase) {
    case AMB_IN:  return AMB_COVER_FULL * exp_smooth(t / (float)AMB_IN_US);
    case AMB_ON:  return AMB_COVER_FULL - (AMB_COVER_FULL - AMB_COVER_ON) *
                         exp_smooth(t / (float)AMB_ON_LIFT_US);
    default:      return 0.0f;
    }
}

static inline void amb_eval(const amb_state *s, uint64_t now, amb_frame *f) {
    const float t = (float)(now - s->t_phase);
    f->draw_ui = 1; f->draw_ambient = 0; f->cover = 0.0f; f->art_a = 0.0f;
    switch (s->phase) {
    case AMB_IN:
        f->cover = amb_cover_at(s, now);
        break;
    case AMB_ON:
        f->draw_ui = 0; f->draw_ambient = 1;
        f->cover = amb_cover_at(s, now);
        f->art_a = exp_smooth((t - 200000.0f) / (float)AMB_ON_LIFT_US);
        break;
    case AMB_OUT:
        if (!s->out_skip1 && t < (float)AMB_OUT1_US) {
            const float u = exp_smooth(t / (float)AMB_OUT1_US);
            f->draw_ui = 0; f->draw_ambient = 1;
            f->cover = s->out_cover0 + (AMB_COVER_FULL - s->out_cover0) * u;
            f->art_a = 1.0f - u;
        } else {
            const float t2 = s->out_skip1 ? t : t - (float)AMB_OUT1_US;
            const float c0 = s->out_skip1 ? s->out_cover0 : AMB_COVER_FULL;
            f->cover = c0 * (1.0f - exp_smooth(t2 / (float)AMB_OUT2_US));
        }
        break;
    default:
        break;
    }
}

// Which artwork is up and how it sits, t_us after ON began.  n candidates
// rotate every AMB_SLIDE_US; each drifts slowly one way (alternating per
// slide), grows 4%, and fades in and out.  All deterministic in t.
typedef struct {
    int   idx;        // candidate index, -1 when n == 0
    float dx, dy;     // drift, authored px
    float scale;      // 1.00 .. 1.04
    float a;          // this artwork's opacity
} amb_slide;

static inline void amb_slide_at(uint64_t t_us, int n, amb_slide *out) {
    out->idx = -1; out->dx = out->dy = 0.0f; out->scale = 1.0f; out->a = 0.0f;
    if (n <= 0) return;
    const uint64_t k = t_us / AMB_SLIDE_US;
    const float u = (float)(t_us % AMB_SLIDE_US) / (float)AMB_SLIDE_US;   // 0..1
    out->idx = (int)(k % (uint64_t)n);
    const float dir = (k & 1) ? -1.0f : 1.0f;
    out->dx = dir * (-18.0f + 36.0f * u);
    out->dy = -6.0f + 12.0f * u;
    out->scale = 1.0f + 0.04f * u;
    const float ms = (float)(t_us % AMB_SLIDE_US);
    const float fin  = exp_smooth(ms / (float)AMB_XFADE_US);
    const float fout = exp_smooth(((float)AMB_SLIDE_US - ms) / (float)AMB_XFADE_US);
    out->a = n == 1 ? 1.0f : (fin < fout ? fin : fout);
}

#ifdef __cplusplus
}
#endif

#endif
