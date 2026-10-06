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

typedef struct { int w, h; } PicSize;

static inline int pic_clamp(int v) {
    return v < PIC_MIN ? PIC_MIN : v > PIC_MAX ? PIC_MAX : v;
}

static inline PicSize pic_step(PicSize p, int dw, int dh) {
    p.w = pic_clamp(p.w + dw);
    p.h = pic_clamp(p.h + dh);
    return p;
}

static inline int pic_is_default(const PicSize *p) {
    return p->w == PIC_DEFAULT && p->h == PIC_DEFAULT;
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
