#pragma once

// Where to break a line of server text (a synopsis, a description).
//
// The text is UTF-8 and may be Japanese: kana and kanji are written without spaces, so a line
// may break after any of them, and a break must never fall inside a multi-byte sequence.
// Pure C so tests/test_line_break.c compiles this exact file.

#include <stddef.h>

typedef int (*lb_width_fn)(const char *utf8_char, void *ctx);   // pixel width of one character

// 1 for a character a line may break after without a space: CJK ideographs and kana, CJK
// punctuation and the full-width forms.  `p` points at its first byte.
static inline int lb_is_cjk(const unsigned char *p)
{
    if (p[0] < 0xE3 || p[0] > 0xEF) return 0;
    if (!p[1] || !p[2]) return 0;
    const unsigned cp = ((unsigned)(p[0] & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (unsigned)(p[2] & 0x3F);
    return (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

// The number of bytes of `p` that make up its next line: the longest prefix that fits `max_w`,
// broken at a space or after a CJK character; the whole of `p` when it all fits.  At most `cap`
// bytes (the caller's buffer), and at least one character, so a caller always advances.
static inline int lb_fit(const char *p, int max_w, int cap, lb_width_fn width, void *ctx)
{
    int fit = 0, brk = -1, wpx = 0;
    while (p[fit]) {
        const unsigned char c = (unsigned char)p[fit];
        const int n = c < 0x80 ? 1 : c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (fit + n > cap) break;                       // out of room in the caller's buffer
        char one[5] = { 0, 0, 0, 0, 0 };
        int j = 0;
        for (; j < n && p[fit + j]; j++) one[j] = p[fit + j];
        if (j < n) { fit += j; break; }                 // a truncated sequence at the very end: take it whole
        wpx += width(one, ctx);
        if (wpx > max_w) {
            if (c == ' ') brk = fit;                  // a space past the edge is where the line ends
            break;
        }
        if (c == ' ') brk = fit;
        fit += n;
        if (lb_is_cjk((const unsigned char *)p + fit - n)) brk = fit;
    }
    if (!p[fit]) return fit;                            // the rest fits
    if (brk > 0) return brk;
    return fit > 0 ? fit : (p[0] ? ((unsigned char)p[0] < 0xC0 ? 1 : (unsigned char)p[0] < 0xE0 ? 2 : (unsigned char)p[0] < 0xF0 ? 3 : 4) : 0);
}
