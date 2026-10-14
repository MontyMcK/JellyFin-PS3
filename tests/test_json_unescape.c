// Host test for source/api/json_unescape.h: Jellyfin's escaped strings must
// reach the screen as the characters they name.
#include <stdio.h>
#include <string.h>
#include "../source/api/json_unescape.h"

static int g_fail = 0, g_n = 0;
#define CHECK(c, ...) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *dec(const char *body, char *out, int cap) {
    json_unescape(body, body + strlen(body), out, cap);
    return out;
}

int main(void) {
    char o[128];
    CHECK(!strcmp(dec("don\\u0027t\"", o, sizeof o), "don't"), "apostrophe: %s", o);
    CHECK(!strcmp(dec("DTS\\u002DHD MA \\u002B Atmos\"", o, sizeof o), "DTS-HD MA + Atmos"), "codec: %s", o);
    CHECK(!strcmp(dec("it\\u2019s\"", o, sizeof o), "it\xE2\x80\x99s"), "curly: %s", o);
    CHECK(!strcmp(dec("Am\\u00E9lie\"", o, sizeof o), "Am\xC3\xA9lie"), "accent");
    CHECK(!strcmp(dec("\\uD83C\\uDFAC x\"", o, sizeof o), "\xF0\x9F\x8E\xAC x"), "surrogate pair");
    CHECK(!strcmp(dec("a\\uD83Cb\"", o, sizeof o), "a\xEF\xBF\xBD" "b"), "lone high surrogate -> U+FFFD");
    CHECK(!strcmp(dec("say \\\"hi\\\" now\" tail", o, sizeof o), "say \"hi\" now"), "escaped quote: %s", o);
    CHECK(!strcmp(dec("a\\\\b\\/c\\nd\"", o, sizeof o), "a\\b/c\nd"), "simple escapes");
    CHECK(!strcmp(dec("\\u003Cb\\u003E \\u0026\"", o, sizeof o), "<b> &"), "html escapes: %s", o);
    CHECK(!strcmp(dec("raw \xC3\xA9 utf8\"", o, sizeof o), "raw \xC3\xA9 utf8"), "raw utf8 passes");
    CHECK(!strcmp(dec("bad \\uZZZZ\"", o, sizeof o), "bad ?ZZZZ"), "bad hex: %s", o);
    CHECK(!strcmp(dec("\"", o, sizeof o), ""), "empty");
    { const char *s = "unterminated"; json_unescape(s, s + strlen(s), o, sizeof o); CHECK(!strcmp(o, "unterminated"), "unterminated"); }
    // Return value: just past the closing quote.
    { const char *s = "ab\\\"c\" rest"; const char *r = json_unescape(s, s + strlen(s), o, sizeof o);
      CHECK(!strcmp(r, " rest") && !strcmp(o, "ab\"c"), "returns past quote"); }
    // Truncation never splits a character.
    { char t[5]; dec("ab\\u00E9\\u00E9\"", t, sizeof t); CHECK(!strcmp(t, "ab\xC3\xA9"), "truncate whole chars (%zu)", strlen(t)); }
    { char t[4]; dec("ab\xC3\xA9x\"", t, sizeof t); CHECK(!strcmp(t, "ab"), "truncate raw utf8: %s", t); }
    { char t[1]; dec("abc\"", t, sizeof t); CHECK(t[0] == 0, "cap 1"); }
    { char t[3]; dec("abcdef\"", t, sizeof t); CHECK(!strcmp(t, "ab"), "plain truncation"); }
    // Bounded by end, never by NUL beyond it.
    { const char s[] = "abc\\u0027def"; json_unescape(s, s + 4, o, sizeof o); CHECK(!strcmp(o, "abc"), "end bound drops a cut-off escape: %s", o); }
    printf("test_json_unescape: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
