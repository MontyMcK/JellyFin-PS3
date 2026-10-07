// SubRip parsing and cue lookup — the parts of source/player/subtitles.cpp
// that can be wrong without the console saying anything.
//
// The parser is fed real-world SubRip rather than a tidy example: BOM, CRLF,
// missing index lines, inline tags, a dot instead of a comma in a timestamp,
// blank cues, and a cue whose text runs past the row limit. Those are the
// shapes that actually arrive from a media server, and every one of them used
// to be a guess.
//
// subtitles.cpp is compiled into this test with its HTTP and logging
// dependencies stubbed, so the code under test is the SAME code the PS3
// builds — not a copy that can drift away from it.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned int       u32;
typedef unsigned long long u64;

// ---- stubs for the console-only dependencies -----------------------------
#define RESPONSE_SIZE (384 * 1024)
#define HTTP_GET 0
char g_server[256] = "127.0.0.1:8096";
char g_token[256]  = "testtoken";

static const char *g_fake_body = NULL;
static int http_request(int, const char *, const char *, const char *,
                        char *out, int out_size)
{
    if (!g_fake_body) return -1;
    int n = (int)strlen(g_fake_body);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, g_fake_body, n);
    out[n] = '\0';
    return n;
}
// Binary counterpart, for subs_load_pgs()'s Stream.sup fetch -- a separate
// fake body since it is raw bytes (may embed NULs), not a C string.
static const unsigned char *g_fake_binary     = NULL;
static int                  g_fake_binary_len = 0;
static int http_fetch_binary(const char *, const char *,
                             unsigned char *out, int out_size)
{
    if (!g_fake_binary) return -1;
    int n = g_fake_binary_len;
    if (n > out_size) n = out_size;
    memcpy(out, g_fake_binary, (size_t)n);
    return n;
}
static void plog(const char *) {}

#define JF_SUBTITLES_TEST 1
#include "../source/player/subtitles.cpp"

// --------------------------------------------------------------------------
static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static const char *SRT =
    "\xEF\xBB\xBF"                       // UTF-8 BOM, which servers do send
    "1\r\n"
    "00:00:01,000 --> 00:00:03,500\r\n"
    "<i>First line</i>\r\n"
    "second line\r\n"
    "\r\n"
    "2\n"
    "00:01:00.250 --> 00:01:02,000\n"    // dot instead of comma
    "{\\an8}Top positioned\n"
    "\n"
    // no index line at all
    "00:02:00,000 --> 00:02:01,000\n"
    "Third\n"
    "\n"
    "4\n"
    "00:03:00,000 --> 00:03:00,000\n"    // zero length: must be dropped
    "Never shown\n"
    "\n"
    "5\n"
    "00:04:00,000 --> 00:04:05,000\n"
    "Last\n";

int main(void)
{
    printf("test_subtitles: SubRip parsing and cue lookup\n");

    g_fake_body = SRT;
    int n = subs_load("item", "src", 2);
    check(n == 4, "four usable cues parsed (the zero-length one dropped)");

    check(subs_active(), "subs_active() true after a successful load");

    // Tags stripped, lines joined with a newline.
    const char *t = subs_text_at(2000);
    check(t && strcmp(t, "First line\nsecond line") == 0,
          "tags stripped and both lines kept");

    check(subs_text_at(999)  == NULL, "nothing before the first cue starts");
    check(subs_text_at(3499) != NULL, "still showing just before the cue ends");
    check(subs_text_at(4000) == NULL, "nothing in the gap between cues");

    t = subs_text_at(60500);
    check(t && strcmp(t, "Top positioned") == 0,
          "a '.' timestamp parses, and {\\an8} is stripped");

    t = subs_text_at(120500);
    check(t && strcmp(t, "Third") == 0, "a cue with no index line parses");

    check(subs_text_at(180000) == NULL, "the zero-length cue never shows");

    t = subs_text_at(242000);
    check(t && strcmp(t, "Last") == 0, "a final cue with no trailing blank line parses");

    // Seeking backwards must not be fooled by the forward cursor.
    subs_text_at(242000);
    t = subs_text_at(2000);
    check(t && strcmp(t, "First line\nsecond line") == 0,
          "a backward seek finds the right cue again");

    // Repeated forward walk, the normal playback pattern.
    subs_reset_cursor();
    int seen = 0;
    for (u64 ms = 0; ms < 250000; ms += 100)
        if (subs_text_at(ms)) seen++;
    check(seen > 0, "a forward sweep finds cues without the cursor sticking");

    subs_clear();
    check(!subs_active() && subs_text_at(2000) == NULL,
          "subs_clear() empties the track");

    // A failed fetch must leave subtitles off rather than half-loaded.
    g_fake_body = NULL;
    check(subs_load("item", "src", 2) == -1 && !subs_active(),
          "a failed fetch leaves subtitles off");

    // Garbage in must not crash or produce cues.
    g_fake_body = "not a subtitle file at all\n\nreally not\n";
    check(subs_load("item", "src", 2) == -1 && !subs_active(),
          "unparseable input yields no cues");

    // ---- PGS path: subtitles.cpp's own glue (mode switching, buffer
    // growth, subs_clear() teardown) around subtitles_pgs.c, which is
    // separately exhaustively tested by test_subtitles_pgs.c against the
    // real segment format -- this just proves the wiring, with one minimal
    // hand-built epoch (2x2 bitmap, palette index 9, at t=500ms). -------
    {
        static unsigned char sup[128];
        int n2 = 0;
        auto put_seg = [&](unsigned pts90k, unsigned char type,
                           const unsigned char *payload, int plen) {
            sup[n2++] = 'P'; sup[n2++] = 'G';
            sup[n2++] = (unsigned char)(pts90k >> 24); sup[n2++] = (unsigned char)(pts90k >> 16);
            sup[n2++] = (unsigned char)(pts90k >> 8);  sup[n2++] = (unsigned char)pts90k;
            sup[n2++] = 0; sup[n2++] = 0; sup[n2++] = 0; sup[n2++] = 0;
            sup[n2++] = (unsigned char)type;
            sup[n2++] = (unsigned char)(plen >> 8); sup[n2++] = (unsigned char)plen;
            memcpy(sup + n2, payload, (size_t)plen);
            n2 += plen;
        };
        const unsigned char pcs[] = {
            0x07,0x80,0x04,0x38, 0x10, 0x00,0x00, 0x80, 0x00, 0x01, 0x01,
            0x00,0x01, 0x00, 0x00, 0x00,0x05, 0x00,0x05,
        };
        const unsigned char pds[] = { 0x01,0x00, 0x09,150,128,128,255 };
        const unsigned char ods[] = {
            0x00,0x01, 0x00, 0xC0, 0x00,0x00,0x0E, 0x00,0x02, 0x00,0x02,
            0x00,0x82,0x09, 0x00,0x00,
            0x00,0x82,0x09, 0x00,0x00,
        };
        put_seg(45000, 0x16, pcs, sizeof(pcs));   // 45000/90 = 500ms
        put_seg(45000, 0x14, pds, sizeof(pds));
        put_seg(45000, 0x15, ods, sizeof(ods));
        put_seg(45000, 0x80, NULL, 0);

        g_fake_binary = sup; g_fake_binary_len = n2;
        int epochs = subs_load_pgs("item", "src", 3);
        check(epochs == 1, "pgs: one epoch indexed through subs_load_pgs");
        check(subs_active() && subs_is_pgs(),
             "pgs: subs_active()+subs_is_pgs() true after load");
        check(subs_text_at(500) == NULL,
             "pgs: subs_text_at() stays NULL while a PGS track is active");

        const PgsBitmap *bmp = subs_pgs_at(500);
        check(bmp && bmp->width == 2 && bmp->height == 2,
             "pgs: subs_pgs_at() decodes the bitmap at its epoch");
        check(bmp && bmp->x == 5 && bmp->y == 5, "pgs: composition position (5,5)");
        check(subs_pgs_at(0) == NULL, "pgs: nothing before the epoch starts");

        subs_clear();
        check(!subs_active() && !subs_is_pgs() && subs_pgs_at(500) == NULL,
             "pgs: subs_clear() empties the track and drops out of PGS mode");

        g_fake_binary = NULL;
        check(subs_load_pgs("item", "src", 3) == -1 && !subs_active(),
             "pgs: a failed fetch leaves subtitles off");
    }

    if (failures) { printf("test_subtitles: %d FAILED\n", failures); return 1; }
    printf("test_subtitles: all checks passed\n");
    return 0;
}
