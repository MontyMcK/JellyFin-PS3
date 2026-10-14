// Host test for the line breaker (source/ui/render/line_break.h): spaces, CJK text without spaces,
// accents, and never a break inside a UTF-8 sequence.
//
//   make -f Makefile.host test_line_break
#include <stdio.h>
#include <string.h>

#include "line_break.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// A character is 10 px, a CJK character 20 px.
static int width(const char *ch, void *ctx) { (void)ctx; return lb_is_cjk((const unsigned char *)ch) ? 20 : 10; }

static int valid_utf8_cut(const char *s, int n) {   // the cut at n is on a character boundary
    return n >= (int)strlen(s) || (((unsigned char)s[n]) & 0xC0) != 0x80;
}

static void latin(void) {
    printf("- latin\n");
    const char *t = "the quick brown fox";
    CHECK(lb_fit(t, 1000, 200, width, NULL) == (int)strlen(t));                 // all of it fits
    CHECK(lb_fit(t, 90, 200, width, NULL) == 9);                                // "the quick" -- broken at the space
    CHECK(lb_fit("abcdefghijklmno", 50, 200, width, NULL) == 5);                // one long word: cut where it stops fitting
    CHECK(lb_fit("x", 1, 200, width, NULL) == 1);                               // too narrow for even one: still advances
    CHECK(lb_fit("", 100, 200, width, NULL) == 0);
    CHECK(lb_fit("abcdef", 1000, 4, width, NULL) == 4);                         // the caller's buffer
}

static void accents(void) {
    printf("- accents\n");
    const char *t = "caf\xC3\xA9 cr\xC3\xA8me br\xC3\xBBl\xC3\xA9" "e";         // café crème brûlée
    for (int w = 10; w <= 200; w += 10) {
        const int n = lb_fit(t, w, 200, width, NULL);
        CHECK(n >= 1 && valid_utf8_cut(t, n));
    }
    CHECK(lb_fit(t, 1000, 200, width, NULL) == (int)strlen(t));
    // 11 characters wide would fit, but the cap lands inside a two-byte character
    CHECK(valid_utf8_cut(t, lb_fit(t, 1000, 4, width, NULL)));
}

static void japanese(void) {
    printf("- japanese\n");
    // 日本語のあらすじです。 -- 11 characters of 20 px, no spaces
    const char *t = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x81\x82\xE3\x82\x89\xE3\x81\x99\xE3\x81\x98"
                    "\xE3\x81\xA7\xE3\x81\x99\xE3\x80\x82";
    const int len = (int)strlen(t);
    CHECK(lb_fit(t, 1000, 200, width, NULL) == len);
    CHECK(lb_fit(t, 100, 200, width, NULL) == 15);       // five characters of 20 px fit in 100
    CHECK(lb_fit(t, 119, 200, width, NULL) == 15);       // a sixth needs 120
    CHECK(lb_fit(t, 120, 200, width, NULL) == 18);
    // Every line of a repeated wrap is a whole number of characters and the pieces rebuild the text.
    const char *p = t;
    int total = 0, lines = 0;
    while (*p) {
        const int n = lb_fit(p, 60, 200, width, NULL);
        CHECK(n >= 3 && valid_utf8_cut(p, n));
        p += n;
        total += n;
        lines++;
    }
    CHECK(total == len && lines == 4);
    // Mixed: a Latin word then kana
    const char *m = "Spy \xE3\x82\xB9\xE3\x83\x91\xE3\x82\xA4\xE3\x81\xAE\xE7\x89\xA9\xE8\xAA\x9E";   // Spy スパイの物語
    CHECK(lb_fit(m, 70, 200, width, NULL) == 4 + 3);     // "Spy " then one kana (the space breaks first; kana fits after)
    CHECK(valid_utf8_cut(m, lb_fit(m, 75, 200, width, NULL)));
}

static void odd(void) {
    printf("- truncated and odd input\n");
    CHECK(lb_fit("ab\xE3\x81", 1000, 200, width, NULL) == 4);              // a truncated sequence at the end is taken whole
    CHECK(lb_is_cjk((const unsigned char *)"\xE3\x81\x82") == 1);          // あ
    CHECK(lb_is_cjk((const unsigned char *)"\xEF\xBC\xA1") == 1);          // Ａ (full-width)
    CHECK(lb_is_cjk((const unsigned char *)"\xC3\xA9") == 0);              // é
    CHECK(lb_is_cjk((const unsigned char *)"a") == 0);
    CHECK(lb_is_cjk((const unsigned char *)"\xE2\x80\xBA") == 0);          // ›
    CHECK(lb_is_cjk((const unsigned char *)"\xE3\x81") == 0);              // cut short
}

int main(void) {
    latin();
    accents();
    japanese();
    odd();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
