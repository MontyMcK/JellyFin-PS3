#pragma once
#include <stdio.h>

// -------------------------------------------------------------------------
//  Picture size: the video's width and height as a percentage of the fitted
//  picture, each on its own axis.
//  Pure (no PS3 headers) so tests/test_picture_size.c compiles the code the
//  player runs.
//
//  The fitted picture is the aspect-correct letterbox (vid_gpu_build_quad).
//  This scales it about the centre of the screen: below 100 the whole frame
//  shrinks inside a CRT's overscan, above 100 it grows past the edges.  The
//  two axes are independent on purpose; a flat panel has no use for that and
//  leaves both at 100.
// -------------------------------------------------------------------------

#define PIC_MIN      50
#define PIC_MAX      110
#define PIC_DEFAULT  100

// How the fitted picture meets the screen (the Select button in the player).
//   FIT      the whole frame, with bars where the shapes differ
//   FILL     scaled up, shape kept, until nothing of the screen is bare: the
//            edges of the film are cropped (a 16:9 film on a 4:3 TV loses its sides)
//   STRETCH  squeezed or pulled to the screen's shape, nothing cropped
typedef enum { PIC_FIT = 0, PIC_FILL = 1, PIC_STRETCH = 2, PIC_ASPECTS = 3 } PicAspect;

typedef struct { int w, h; int aspect; } PicSize;

static inline int pic_clamp(int v) {
    return v < PIC_MIN ? PIC_MIN : v > PIC_MAX ? PIC_MAX : v;
}

static inline PicSize pic_step(PicSize p, int dw, int dh) {
    p.w = pic_clamp(p.w + dw);
    p.h = pic_clamp(p.h + dh);
    return p;
}

static inline int pic_is_default(const PicSize *p) {
    return p->w == PIC_DEFAULT && p->h == PIC_DEFAULT && p->aspect == PIC_FIT;
}

static inline int pic_aspect_next(int a) { return (a + 1) % PIC_ASPECTS; }

// "fill" -> PIC_FILL; anything unknown is FIT.
static inline int pic_aspect_parse(const char *s) {
    if (!s) return PIC_FIT;
    while (*s == ' ') s++;
    if (s[0] == 'f' && s[1] == 'i' && s[2] == 'l') return PIC_FILL;
    if (s[0] == 's' && s[1] == 't') return PIC_STRETCH;
    return PIC_FIT;
}

static inline const char *pic_aspect_name(int a) {
    return a == PIC_FILL ? "fill" : a == PIC_STRETCH ? "stretch" : "fit";
}

// The fitted picture (sw x sh, as vid_gpu_build_quad computed it for a dw x dh
// screen, in the same visual units) becomes ow x oh for `aspect`.  FILL may
// exceed the screen on one axis: that excess is the crop.
static inline void pic_aspect_apply(int aspect, unsigned sw, unsigned sh,
                                    unsigned dw, unsigned dh,
                                    unsigned *ow, unsigned *oh) {
    *ow = sw; *oh = sh;
    if (aspect == PIC_STRETCH) { *ow = dw; *oh = dh; return; }
    if (aspect != PIC_FILL || sw == 0 || sh == 0) return;
    if ((unsigned long long)dw * sh >= (unsigned long long)dh * sw) {
        // limited by width already: scale until the width is the screen's
        *ow = dw;
        *oh = (unsigned)(((unsigned long long)sh * dw + sw - 1) / sw);
    } else {
        *oh = dh;
        *ow = (unsigned)(((unsigned long long)sw * dh + sh - 1) / sh);
    }
}

// "94 90" -> {94, 90}, clamped.  Returns 1 on success; on failure *out is
// untouched.
static inline int pic_parse(const char *s, PicSize *out) {
    int w, h;
    if (!s || sscanf(s, "%d %d", &w, &h) != 2) return 0;
    out->w = pic_clamp(w);
    out->h = pic_clamp(h);
    return 1;
}

static inline void pic_format(const PicSize *p, char *buf, size_t cap) {
    snprintf(buf, cap, "%d %d\n", p->w, p->h);
}
