// canyon.h -- the "Canyon" music visualizer: a scrolling stereo spectrogram
// flown over as a landscape.
//
// WHAT THE PS3 DOES (fw 4.93, traced statically; see
// docs/canyon-visualizer.md for addresses)
//
//   * custom_render_plugin starts qgl_canyon_app.sprx with the audio-callback
//     table from soundvisualizer_plugin as its module-start argument.  Earth
//     (qgl_gaia_app) is started with NULL and the XMB wave never reads the
//     table: Canyon is the one stock visualizer that hears the music.
//   * Every frame (vtable+0x18 -> 0xd188) it pulls a spectrum (sv_spectrum.h)
//     and builds ONE 128-wide row from it:
//         row[i]      = L[int(i / 64 * 255)]          i = 0..63
//         row[64 + i] = R[int(255 - i / 64 * 255)]    i = 0..63
//     so the bass of each channel sits at the outer edges and the treble of
//     both meets in the middle: bass raises the canyon WALLS, treble the FLOOR.
//   * sub_943c: each column is multiplied by exp(c * tri(x)) (tri = 0 at the
//     edges, 1 in the middle -- a treble lift), the row is divided by
//     max(1, rowmax), and four 32-column means are kept (L-bass, L-treble,
//     R-treble, R-bass).  Distance advances by dt * POS SPEED and one row is
//     committed per unit of distance: POS SPEED is rows per second.
//   * sub_61cc (row commit): per-column lag, 0.7 in the middle to 0.995 at the
//     edges (walls move slowly, the floor quickly); four passes of a 0.25
//     Laplacian across columns; a smoothstep valley profile shaped by
//     VALLEY CENTRE / WIDTH / SHARP / HEIGHT.
//   * 128 x 128 history, one world unit per column and per row.
//
// WHAT IS OURS
//
// The drawing.  Sony's terrain/line/fog/blur shaders and its normal map are not
// used or reproduced; this file lights the heightfield on the PPU and emits
// plain coloured triangles through the app's existing passthrough programs,
// exactly as JellyWave does (wave_cam.h explains why that is the cheap path on
// this console).  The camera framing is ours too: the presets' CAMERA numbers
// assume Sony's shaders and scene graph, so only ZOOM (the field of view) is
// taken from them.  Colours, fog, terrain height, valley shape and speed come
// from the presets, which ui_canyon.cpp reads at RUNTIME from the console's own
// /dev_flash/vsh/resource/qgl/canyon.qrc -- nothing Sony-owned ships in the
// app.  Without that file the built-in CY_DEFAULT (our values) is used.
//
// House rules: header-only, pure C, deterministic, caller-owned state, no PS3
// headers.  Tested by tests/test_canyon.c.

#ifndef CANYON_H
#define CANYON_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CY_COLS 128
#define CY_ROWS 128

// ---------------------------------------------------------------- presets

typedef struct {
    // CANYON_COLOUR
    float col_r, col_g, col_b;      // 0..10
    float col_scale, col_bias;
    float fade;                     // FADE COLOUR
    float line_r, line_g, line_b;   // 0..1
    // CANYON_FOG
    float fog_min, fog_max;
    float fog_r, fog_g, fog_b;
    // CANYON_TERRAIN
    float height;                   // TERRAIN HEIGHT
    float scale_x;                  // SCALE X
    float valley_h, valley_c, valley_w, valley_s;
    // CANYON_CAMERA
    float speed;                    // POS SPEED (rows per second)
    float zoom;                     // ZOOM (vertical field of view, degrees)
    // CANYON_LINE
    float line_h, line_w;
} cy_preset;

// Our own look for when the console's canyon.qrc is unavailable (RPCS3, a
// console without it): Jellyfin's purple and blue over a deep navy haze.
static const cy_preset CY_DEFAULT = {
    /* col */ 4.2f, 2.4f, 7.6f, 1.35f, 0.16f, 1.0f,
    /* line */ 0.55f, 0.85f, 1.0f,
    /* fog */ 40.0f, 150.0f, 0.03f, 0.04f, 0.12f,
    /* terrain */ 60.0f, 1.0f, 0.8f, 0.0f, 0.0f, 0.4f,
    /* camera */ 22.0f, 68.0f,
    /* line */ 12.0f, 24.0f,
};

static inline int cy__streq(const char *a, int an, const char *b) {
    int bn = (int)strlen(b);
    return an == bn && memcmp(a, b, (size_t)an) == 0;
}

static inline float cy__atof(const char *p, const char *end) {
    // Plain decimal parser ("-0.469999", "1e-05" does not occur in canyon
    // presets but is handled): no locale, no libc strtod differences.
    float sign = 1.0f, v = 0.0f, scale = 1.0f;
    if (p < end && (*p == '-' || *p == '+')) { if (*p == '-') sign = -1.0f; p++; }
    while (p < end && *p >= '0' && *p <= '9') v = v * 10.0f + (float)(*p++ - '0');
    if (p < end && *p == '.') {
        p++;
        while (p < end && *p >= '0' && *p <= '9') { scale *= 0.1f; v += (float)(*p++ - '0') * scale; }
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        int es = 1, e = 0;
        if (p < end && (*p == '-' || *p == '+')) { if (*p == '-') es = -1; p++; }
        while (p < end && *p >= '0' && *p <= '9') e = e * 10 + (*p++ - '0');
        while (e-- > 0) v = es > 0 ? v * 10.0f : v * 0.1f;
    }
    return sign * v;
}

// Apply one .mnu file ("#MNU_1.0" then "NAME:type:value" lines, CRLF or LF).
// Unknown keys are ignored.  Returns the number of keys applied.
static inline int cy_mnu_apply(cy_preset *p, const char *txt, int len) {
    static const struct { const char *k; int off; } K[] = {
        { "COLOUR R", offsetof(cy_preset, col_r) },   { "COLOUR G", offsetof(cy_preset, col_g) },
        { "COLOUR B", offsetof(cy_preset, col_b) },   { "COLOUR SCALE", offsetof(cy_preset, col_scale) },
        { "COLOUR BIAS", offsetof(cy_preset, col_bias) }, { "FADE COLOUR", offsetof(cy_preset, fade) },
        { "LINE R", offsetof(cy_preset, line_r) },    { "LINE G", offsetof(cy_preset, line_g) },
        { "LINE B", offsetof(cy_preset, line_b) },
        { "FOG MIN", offsetof(cy_preset, fog_min) },  { "FOG MAX", offsetof(cy_preset, fog_max) },
        { "FOG R", offsetof(cy_preset, fog_r) },      { "FOG G", offsetof(cy_preset, fog_g) },
        { "FOG B", offsetof(cy_preset, fog_b) },
        { "TERRAIN HEIGHT", offsetof(cy_preset, height) }, { "SCALE X", offsetof(cy_preset, scale_x) },
        { "VALLEY HEIGHT", offsetof(cy_preset, valley_h) }, { "VALLEY CENTRE", offsetof(cy_preset, valley_c) },
        { "VALLEY WIDTH", offsetof(cy_preset, valley_w) },  { "VALLEY SHARP", offsetof(cy_preset, valley_s) },
        { "POS SPEED", offsetof(cy_preset, speed) },  { "ZOOM", offsetof(cy_preset, zoom) },
        { "LINE HEIGHT", offsetof(cy_preset, line_h) }, { "LINE WIDTH", offsetof(cy_preset, line_w) },
    };
    int n = 0;
    const char *s = txt, *end = txt + len;
    while (s < end) {
        const char *e = s;
        while (e < end && *e != '\n' && *e != '\r') e++;
        const char *c1 = s;
        while (c1 < e && *c1 != ':') c1++;
        const char *c2 = c1 < e ? c1 + 1 : e;
        while (c2 < e && *c2 != ':') c2++;
        if (c1 < e && c2 < e) {
            for (unsigned k = 0; k < sizeof(K) / sizeof(K[0]); k++)
                if (cy__streq(s, (int)(c1 - s), K[k].k)) {
                    *(float *)((char *)p + K[k].off) = cy__atof(c2 + 1, e);
                    n++;
                    break;
                }
        }
        s = e;
        while (s < end && (*s == '\n' || *s == '\r')) s++;
    }
    return n;
}

static inline void cy_preset_lerp(cy_preset *out, const cy_preset *a, const cy_preset *b, float t) {
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)out;
    for (unsigned i = 0; i < sizeof(cy_preset) / sizeof(float); i++)
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

// ----------------------------------------------------------- QRCF archive
//
// canyon.qrc = "QRCC" + u32 BE size + zlib stream; inflated it is a "QRCF"
// archive.  Header (BE u32): +0x08 toc offset, +0x0C toc size, +0x10 name
// table offset, +0x28 data offset.  A file entry is 0x3C bytes: +0x00 = 0xF,
// +0x1C/+0x20 = 0x14/6, +0x24 data offset, +0x28 size, +0x34 name offset (the
// name string follows a 4-byte back-pointer).

static inline uint32_t cy__be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

typedef void (*cy_qrcf_fn)(void *ctx, const char *name, const uint8_t *data, uint32_t len);

// Walk every file in an inflated QRCF image.  Returns files visited, or -1
// if the header does not check out.
static inline int cy_qrcf_walk(const uint8_t *d, uint32_t n, cy_qrcf_fn fn, void *ctx) {
    if (n < 0x40 || memcmp(d, "QRCF", 4) != 0) return -1;
    uint32_t toc = cy__be32(d + 8), tocsz = cy__be32(d + 12);
    uint32_t nm = cy__be32(d + 16), dat = cy__be32(d + 0x28);
    if (toc > n || tocsz > n - toc || nm > n || dat > n) return -1;
    int files = 0;
    for (uint32_t e = toc; e + 0x3C <= toc + tocsz; e += 4) {
        if (cy__be32(d + e) != 0xF || cy__be32(d + e + 0x1C) != 0x14 || cy__be32(d + e + 0x20) != 6)
            continue;
        uint32_t off = cy__be32(d + e + 0x24), sz = cy__be32(d + e + 0x28);
        uint32_t no = nm + cy__be32(d + e + 0x34) + 4;
        if (no >= n || dat + off > n || sz > n - dat - off) continue;
        const char *name = (const char *)d + no;
        uint32_t k = no;
        while (k < n && d[k]) k++;
        if (k >= n) continue;
        fn(ctx, name, d + dat + off, sz);
        files++;
        e += 0x3C - 4;
    }
    return files;
}

// Builds the preset table from a canyon.qrc image: the base files
// ("canyon/CANYON_*.mnu") and every "override/<NAME>/canyon/*.mnu" on top.
#define CY_MAX_PRESETS 64
typedef struct {
    cy_preset base;
    int       n;
    char      name[CY_MAX_PRESETS][16];
    cy_preset p[CY_MAX_PRESETS];
    int       keys;                  // keys applied in total (sanity)
} cy_bank;

static inline void cy__bank_base(void *ctx, const char *name, const uint8_t *d, uint32_t len) {
    cy_bank *b = (cy_bank *)ctx;
    if (strncmp(name, "canyon/CANYON_", 14) == 0)
        b->keys += cy_mnu_apply(&b->base, (const char *)d, (int)len);
}

static inline void cy__bank_over(void *ctx, const char *name, const uint8_t *d, uint32_t len) {
    cy_bank *b = (cy_bank *)ctx;
    if (strncmp(name, "override/", 9) != 0) return;
    const char *pn = name + 9, *slash = strchr(pn, '/');
    if (!slash || strncmp(slash, "/canyon/CANYON_", 15) != 0) return;
    int ln = (int)(slash - pn);
    if (ln <= 0 || ln >= 16) return;
    int i;
    for (i = 0; i < b->n; i++)
        if ((int)strlen(b->name[i]) == ln && memcmp(b->name[i], pn, (size_t)ln) == 0) break;
    if (i == b->n) {
        if (b->n >= CY_MAX_PRESETS) return;
        memcpy(b->name[i], pn, (size_t)ln);
        b->name[i][ln] = 0;
        b->p[i] = b->base;
        b->n++;
    }
    b->keys += cy_mnu_apply(&b->p[i], (const char *)d, (int)len);
}

// Two passes so every override starts from the complete base.
static inline int cy_bank_load(cy_bank *b, const uint8_t *qrcf, uint32_t n) {
    memset(b, 0, sizeof(*b));
    b->base = CY_DEFAULT;
    if (cy_qrcf_walk(qrcf, n, cy__bank_base, b) < 0) return -1;
    cy_qrcf_walk(qrcf, n, cy__bank_over, b);
    return b->n;
}

// Clamp a preset into the range this renderer frames well.  Sony's numbers
// are for Sony's camera; ours is fixed, so the extremes are pulled in.
static inline void cy_preset_sanitise(cy_preset *p) {
#define CY_CL(v, lo, hi) do { if (!((v) >= (lo))) (v) = (lo); if ((v) > (hi)) (v) = (hi); } while (0)
    CY_CL(p->col_r, 0.0f, 10.0f); CY_CL(p->col_g, 0.0f, 10.0f); CY_CL(p->col_b, 0.0f, 10.0f);
    CY_CL(p->col_scale, 0.3f, 3.0f); CY_CL(p->col_bias, 0.0f, 0.8f); CY_CL(p->fade, 0.0f, 1.0f);
    CY_CL(p->line_r, 0.0f, 1.0f); CY_CL(p->line_g, 0.0f, 1.0f); CY_CL(p->line_b, 0.0f, 1.0f);
    CY_CL(p->fog_r, 0.0f, 1.0f); CY_CL(p->fog_g, 0.0f, 1.0f); CY_CL(p->fog_b, 0.0f, 1.0f);
    CY_CL(p->fog_min, 0.0f, 110.0f); CY_CL(p->fog_max, p->fog_min + 20.0f, 260.0f);
    CY_CL(p->height, 28.0f, 90.0f); CY_CL(p->scale_x, 0.6f, 1.6f);
    CY_CL(p->valley_h, 0.0f, 1.5f); CY_CL(p->valley_c, -0.5f, 0.5f);
    CY_CL(p->valley_w, 0.0f, 1.0f); CY_CL(p->valley_s, 0.0f, 4.0f);
    CY_CL(p->speed, 8.0f, 40.0f); CY_CL(p->zoom, 50.0f, 80.0f);
#undef CY_CL
    // A preset whose line is black would glow nothing: lift it toward white.
    float lm = p->line_r + p->line_g + p->line_b;
    if (lm < 0.6f) { p->line_r += 0.3f; p->line_g += 0.3f; p->line_b += 0.3f; }
}

// ---------------------------------------------------------------- terrain

typedef struct {
    float hist[CY_ROWS][CY_COLS];   // committed rows, lagged + smoothed
    int   head;                     // index of the NEWEST row in hist
    float cur[CY_COLS];             // the lagged row (Sony's per-column state)
    float acc[CY_COLS];             // sum of targets since the last commit
    int   acc_n;
    float quarter[4];               // L-bass, L-treble, R-treble, R-bass
    float frac;                     // 0..1: progress toward the next row
    float treble_lift;              // Sony's c in exp(c * tri(x))
    int   rows_pushed;
} cy_state;

static inline void cy_init(cy_state *s) {
    memset(s, 0, sizeof(*s));
    s->treble_lift = 0.9f;
}

// One spectrum frame (sv_spectrum.h output, 256 bands per side, 0..3) into
// the row target.  Call once per UI frame while music plays.
static inline void cy_feed(cy_state *s, const float *L, const float *R) {
    float row[CY_COLS], mx = 1.0f;
    for (int i = 0; i < 64; i++) {
        row[i]      = L[(int)((float)i * 0.015625f * 255.0f)];
        row[64 + i] = R[(int)(255.0f - (float)i * 0.015625f * 255.0f)];
    }
    for (int i = 0; i < CY_COLS; i++) {
        float tri = 1.0f - 2.0f * fabsf(0.5f - (float)i * 0.0078125f);
        row[i] *= expf(s->treble_lift * tri);
        if (row[i] > mx) mx = row[i];
    }
    for (int i = 0; i < CY_COLS; i++) {
        row[i] /= mx;
        s->acc[i] += row[i];
    }
    s->acc_n++;
    for (int q = 0; q < 4; q++) {
        float sum = 0.0f;
        for (int i = 0; i < 32; i++) sum += row[q * 32 + i];
        s->quarter[q] = (s->quarter[q] + sum) * 0.03125f;
    }
}

// Silence / pause: the target is flat, so the land settles at the lag rate.
static inline void cy_feed_silence(cy_state *s) {
    s->acc_n++;
    for (int q = 0; q < 4; q++) s->quarter[q] *= 0.95f;
}

// Per-column lag per committed row: CY_LAG_MID in the middle (the treble
// floor) to CY_LAG_EDGE at the edges (the bass walls).  Sony's row commit
// reads as 0.7 .. 0.995 -- at 22 rows/s that is a ~9 s time constant on the
// walls, which on a TV would read as "the canyon ignores the bass".  These are
// faster until a hardware look says otherwise.
#define CY_LAG_MID  0.55f
#define CY_LAG_EDGE 0.90f

static inline void cy__commit(cy_state *s) {
    float t[CY_COLS];
    for (int i = 0; i < CY_COLS; i++) {
        float target = s->acc_n ? s->acc[i] / (float)s->acc_n : s->cur[i];
        float x   = 2.0f * ((float)i * 0.0078125f - 0.5f);
        float lag = CY_LAG_MID + (CY_LAG_EDGE - CY_LAG_MID) * fabsf(x);
        s->cur[i] += (target - s->cur[i]) * (1.0f - lag);
        t[i] = s->cur[i];
        s->acc[i] = 0.0f;
    }
    s->acc_n = 0;
    for (int pass = 0; pass < 4; pass++) {
        float o[CY_COLS];
        for (int i = 0; i < CY_COLS; i++) {
            float l = t[i > 0 ? i - 1 : 0], r = t[i < CY_COLS - 1 ? i + 1 : CY_COLS - 1];
            o[i] = t[i] + 0.25f * (l + r - 2.0f * t[i]);
        }
        memcpy(t, o, sizeof(t));
    }
    s->head = (s->head + 1) % CY_ROWS;
    memcpy(s->hist[s->head], t, sizeof(t));
    s->rows_pushed++;
}

// Advance by dt seconds at the preset's speed; commits whole rows.
static inline void cy_step(cy_state *s, const cy_preset *p, float dt) {
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    s->frac += dt * p->speed;
    int guard = 0;
    while (s->frac >= 1.0f && guard++ < 8) {
        s->frac -= 1.0f;
        cy__commit(s);
    }
    if (s->frac >= 1.0f) s->frac = 0.0f;
}

// Row `age` (0 = newest), column i -> terrain value (0..~1.x, before height).
static inline float cy_val(const cy_state *s, int age, int i) {
    return s->hist[(s->head - age + CY_ROWS) % CY_ROWS][i];
}

// ------------------------------------------------------------------ camera

typedef struct {
    float ex, ey, ez;                   // eye
    float rx[3], ux[3], fx[3];          // right, up, forward
    float sx, sy;                       // projection scales
} cy_cam;

static inline void cy_cam_make(cy_cam *c, const cy_preset *p, float aspect, float bob) {
    const float H = p->height;
    c->ex = 0.0f; c->ey = 0.62f * H + bob; c->ez = -14.0f;
    float tx = 0.0f, ty = 0.06f * H, tz = 118.0f;
    float f0 = tx - c->ex, f1 = ty - c->ey, f2 = tz - c->ez;
    float fl = 1.0f / sqrtf(f0 * f0 + f1 * f1 + f2 * f2);
    c->fx[0] = f0 * fl; c->fx[1] = f1 * fl; c->fx[2] = f2 * fl;
    // right = forward x world-up(0,1,0)
    float r0 = -c->fx[2], r2 = c->fx[0];
    float rl = 1.0f / sqrtf(r0 * r0 + r2 * r2);
    c->rx[0] = -r0 * rl; c->rx[1] = 0.0f; c->rx[2] = -r2 * rl;
    // up = forward x right (right = +x, forward ~ +z  =>  up ~ +y)
    c->ux[0] = c->fx[1] * c->rx[2] - c->fx[2] * c->rx[1];
    c->ux[1] = c->fx[2] * c->rx[0] - c->fx[0] * c->rx[2];
    c->ux[2] = c->fx[0] * c->rx[1] - c->fx[1] * c->rx[0];
    float fov = p->zoom * 3.14159265f / 180.0f;
    c->sy = 1.0f / tanf(0.5f * fov);
    c->sx = c->sy / aspect;
}

// ------------------------------------------------------------------- emit

typedef struct { float x, y, z, w; uint32_t rgba; uint32_t pad; }
    __attribute__((aligned(8))) cy_vert;

#define CY_RGBA(r, g, b, a) (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | \
                             ((uint32_t)(b) << 8) | (uint32_t)(a))

static inline uint32_t cy__pack(float r, float g, float b, float a) {
    float v[4] = { r, g, b, a };
    uint32_t o[4];
    for (int k = 0; k < 4; k++) {
        float x = v[k] * 255.0f + 0.5f;
        o[k] = x <= 0.0f ? 0u : x >= 255.0f ? 255u : (uint32_t)x;
    }
    return CY_RGBA(o[0], o[1], o[2], o[3]);
}

static inline void cy__proj(const cy_cam *c, float x, float y, float z, cy_vert *v) {
    float dx = x - c->ex, dy = y - c->ey, dz = z - c->ez;
    float vx = dx * c->rx[0] + dy * c->rx[1] + dz * c->rx[2];
    float vy = dx * c->ux[0] + dy * c->ux[1] + dz * c->ux[2];
    float vz = dx * c->fx[0] + dy * c->fx[1] + dz * c->fx[2];
    if (vz < 0.5f) vz = 0.5f;           // never behind the eye (see cy_emit)
    v->x = vx * c->sx;
    v->y = vy * c->sy;
    v->w = vz;
    v->z = 0.5f * vz;                   // mid-range: depth is unused, never clipped
    v->pad = 0;
}

typedef struct {
    int sky_off, sky_n;                 // TRIANGLE_STRIP, blend off
    int land_off, land_n;               // TRIANGLE_STRIP (degenerate-joined)
    int line_off, line_n;               // TRIANGLE_STRIP, additive
} cy_counts;

// Vertices cy_emit needs at most.
#define CY_SKY_VERTS  4
#define CY_LAND_VERTS ((CY_ROWS - 1) * (CY_COLS * 2 + 2))
#define CY_LINE_VERTS (CY_COLS * 2 + 2)
#define CY_MAX_VERTS  (CY_SKY_VERTS + CY_LAND_VERTS + CY_LINE_VERTS)

// valley(x): smoothstep((|x - centre| - width) * sharp), x in -1..1.
static inline float cy__valley(const cy_preset *p, float x) {
    float t = (fabsf(x - p->valley_c) - p->valley_w) * p->valley_s;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// Height of row `age`, column i, in world units.  The newest row grows in
// with `frac` so rows emerge from the horizon instead of popping.
static inline float cy_height(const cy_state *s, const cy_preset *p, int age, int i, float *valley_tab) {
    float v = cy_val(s, age, i);
    if (age == 0) v *= s->frac;
    return p->height * (v + p->valley_h * valley_tab[i]);
}

// bright: 0..1 multiplies every colour (the music screen dims the land while
// its UI is up and fades it on the way in and out).  Land and sky are opaque;
// only the line carries alpha (it is drawn additively).
static inline int cy_emit(const cy_state *s, const cy_preset *p, float aspect,
                          float bright, cy_vert *out, int cap, cy_counts *cnt) {
    memset(cnt, 0, sizeof(*cnt));
    if (cap < CY_MAX_VERTS) return 0;
    const float bass = 0.5f * (s->quarter[0] + s->quarter[3]);
    const float treb = 0.5f * (s->quarter[1] + s->quarter[2]);
    cy_cam cam;
    cy_cam_make(&cam, p, aspect, 1.5f * bass);

    const float fr = p->fog_r, fg = p->fog_g, fb = p->fog_b;
    const float br = p->col_r * 0.1f, bg = p->col_g * 0.1f, bb = p->col_b * 0.1f;
    int n = 0;

    // Sky: the fog colour, a touch darker overhead.
    cnt->sky_off = n;
    {
        const float sx[4] = { -1.0f, 1.0f, -1.0f, 1.0f }, sy[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
        for (int k = 0; k < 4; k++) {
            cy_vert *v = &out[n++];
            v->x = sx[k]; v->y = sy[k]; v->z = 0.5f; v->w = 1.0f; v->pad = 0;
            float d = sy[k] > 0.0f ? 0.55f : 1.0f;
            v->rgba = cy__pack(fr * d * bright, fg * d * bright, fb * d * bright, 1.0f);
        }
    }
    cnt->sky_n = 4;

    float valley_tab[CY_COLS], xw[CY_COLS];
    for (int i = 0; i < CY_COLS; i++) {
        float x = 2.0f * ((float)i / (float)(CY_COLS - 1) - 0.5f);
        valley_tab[i] = cy__valley(p, x);
        xw[i] = x * 64.0f * p->scale_x;
    }

    // Light from above and slightly in front of the viewer.
    const float L0 = -0.25f, L1 = 0.85f, L2 = -0.46f;
    const float Ll = 1.0f / sqrtf(L0 * L0 + L1 * L1 + L2 * L2);

    // Land, far to near (painter's order: a nearer row only ever covers a
    // farther one when the eye looks along +z from above the first row).
    cnt->land_off = n;
    // Each row's heights are computed ONCE and slid through a four-row window
    // (age-1, age, age+1, age+2): the normals need both neighbours of both
    // rows of a strip.  This loop is 33k vertices a frame on the PPU; measured
    // on hardware at 19 ms when every vertex recomputed its neighbours' heights
    // (each a ring modulo) and read its degenerate copy back out of RSX memory.
    // The output is byte-identical to that version (tests/test_canyon.c).
    float rows4[4][CY_COLS];
    float *hM = rows4[0], *hA = rows4[1], *hB = rows4[2], *hC = rows4[3];
    for (int i = 0; i < CY_COLS; i++) {
        hA[i] = cy_height(s, p, 0, i, valley_tab);
        hB[i] = cy_height(s, p, 1, i, valley_tab);
        hC[i] = CY_ROWS > 2 ? cy_height(s, p, 2, i, valley_tab) : hB[i];
    }
    const float dxs     = 2.0f * (128.0f * p->scale_x / 127.0f);
    const float nlL     = Ll;
    for (int age = 0; age < CY_ROWS - 1; age++) {
        // hA = row `age` (farther), hB = row age+1 (nearer), hM/hC the
        // rows either side of them (the edge row stands in for a missing one).
        const float zA = (float)(CY_ROWS - 1 - age) - s->frac;
        const float zB = zA - 1.0f;
        const float *vA = s->hist[(s->head - age + CY_ROWS) % CY_ROWS];
        const float *vB = s->hist[(s->head - age - 1 + 2 * CY_ROWS) % CY_ROWS];
        const int haveM = age > 0, haveC = age + 2 < CY_ROWS;
        int first = 1;
        for (int i = 0; i < CY_COLS; i++) {
            for (int side = 0; side < 2; side++) {
                const float *h = side ? hB : hA;
                const float z  = side ? zB : zA;
                float hl = h[i > 0 ? i - 1 : i], hr = h[i < CY_COLS - 1 ? i + 1 : i];
                float hn = side ? hA[i] : (haveM ? hM[i] : h[i]);
                float hf = side ? (haveC ? hC[i] : h[i]) : hB[i];
                float nx = -(hr - hl) / dxs, nz = -(hn - hf) / 2.0f, ny = 1.0f;
                float nl = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz);
                float lam = (nx * L0 + ny * L1 + nz * L2) * nl * nlL;
                if (lam < 0.0f) lam = 0.0f;
                float v   = side ? vB[i] : vA[i];
                float lum = p->col_bias + p->col_scale * v;
                if (lum > 1.6f) lum = 1.6f;
                float shade = (0.12f + lum) * (0.45f + 0.75f * lam);
                float r = br * shade, g = bg * shade, b = bb * shade;
                // Loud ground glows in the line colour: the stand-in for
                // Sony's HDR glare pass, and what makes a hit read as a hit.
                float em = (v - 0.30f) * 1.6f;
                if (em > 0.0f) {
                    if (em > 1.0f) em = 1.0f;
                    em *= em;
                    r += p->line_r * em * 0.85f; g += p->line_g * em * 0.85f; b += p->line_b * em * 0.85f;
                }
                // Fog by distance from the eye along z, toward the sky colour.
                float dist = z - cam.ez;
                float f = (dist - p->fog_min) / (p->fog_max - p->fog_min);
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                f *= p->fade;
                r += (fr - r) * f; g += (fg - g) * f; b += (fb - b) * f;
                cy_vert *vt = &out[n];
                cy__proj(&cam, xw[i], h[i], z, vt);
                vt->rgba = cy__pack(r * bright, g * bright, b * bright, 1.0f);
                if (first) { out[n + 1] = out[n]; n++; first = 0; }   // degenerate in
                n++;
            }
        }
        out[n] = out[n - 1]; n++;                                     // degenerate out
        // Slide the window one row nearer.
        float *t = hM; hM = hA; hA = hB; hB = hC; hC = t;
        if (age + 3 < CY_ROWS)
            for (int i = 0; i < CY_COLS; i++) hC[i] = cy_height(s, p, age + 3, i, valley_tab);
        else
            memcpy(hC, hB, sizeof(rows4[0]));
    }
    cnt->land_n = n - cnt->land_off;

    // The line: the newest row's profile, lifted and glowing -- the moment
    // the music is heard is the edge the land is being drawn from.
    cnt->line_off = n;
    {
        const float z = (float)(CY_ROWS - 1) - s->frac;
        const float lift = 0.06f * p->line_h;
        const float thick = 1.4f + 0.04f * p->line_w;
        float glow = 0.55f + 0.9f * treb;
        if (glow > 1.0f) glow = 1.0f;
        const uint32_t cc = cy__pack(p->line_r, p->line_g, p->line_b, glow * bright);
        const uint32_t ce = cy__pack(p->line_r, p->line_g, p->line_b, 0.0f);
        int first = 1;
        for (int i = 0; i < CY_COLS; i++) {
            float y = p->height * (cy_val(s, 0, i) * s->frac + cy_val(s, 1, i) * (1.0f - s->frac)
                                   + p->valley_h * valley_tab[i]) + lift;
            cy_vert *a = &out[n];
            cy__proj(&cam, xw[i], y + thick, z, a);  a->rgba = ce;
            if (first) { out[n + 1] = out[n]; n++; first = 0; }
            n++;
            cy_vert *b = &out[n];
            cy__proj(&cam, xw[i], y, z, b);          b->rgba = cc;
            n++;
        }
        out[n] = out[n - 1]; n++;
    }
    cnt->line_n = n - cnt->line_off;
    return n;
}

#endif
