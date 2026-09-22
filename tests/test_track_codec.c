/*
 * Host-side unit test for track_label_is_dts() (source/util/track_codec.c),
 * the one decision that turns DTS mode on for a given track.  Saying yes on
 * a track that is not DTS invites the server to reach for ffmpeg's
 * experimental DTS encoder; saying no on a real DTS-HD MA / DTS:X track
 * costs the user the whole feature.  Both directions are worth a test.
 *
 * Build & run:  make -f Makefile.host && ./test_track_codec
 */
#include <stdio.h>

#include "track_codec.h"

static int g_failures = 0;

static void check(const char *what, const char *label, bool got, bool want) {
    if (got != want) {
        printf("FAIL %s(\"%s\"): got %s, want %s\n", what,
               label ? label : "(null)",
               got ? "true" : "false", want ? "true" : "false");
        g_failures++;
    }
}

static void expect(const char *label, bool want) {
    check("is_dts", label, track_label_is_dts(label), want);
}

static void expect_truehd(const char *label, bool want) {
    check("is_truehd", label, track_label_is_truehd(label), want);
}

static void expect_hd(const char *label, bool want) {
    check("is_hd_audio", label, track_label_is_hd_audio(label), want);
}

int main(void) {
    /* Jellyfin DisplayTitles that should select the DTS path */
    expect("English - DTS - 5.1 - Default", true);
    expect("English - DTS-HD MA - 5.1 - Default", true);
    expect("English - DTS-HD MA - 7.1", true);
    expect("Surround 7.1 - DTS:X - Default", true);
    expect("English - DTS-ES - 6.1", true);
    expect("English - DTS Express - 5.1", true);
    expect("dts", true);                       /* bare, lowercase */
    expect("English - DCA - 5.1", true);       /* ffmpeg's internal name */
    expect("Commentary (DTS)", true);          /* bracketed */

    /* Everything else must not */
    expect("English - EAC3 - 5.1 - Default", false);
    expect("English - AC3 - 5.1", false);
    expect("English - AAC - 2.0", false);
    expect("English - TrueHD - 7.1", false);   /* HD, but not this codec */
    expect("English - FLAC - 5.1", false);
    expect("", false);
    expect(NULL, false);

    /* ---- the Dolby side: TrueHD (and Atmos, which is TrueHD) ---- */
    expect_truehd("English - TrueHD - 7.1 - Default", true);
    expect_truehd("English - TrueHD Atmos - 7.1", true);
    expect_truehd("English - TRUEHD - 5.1", true);
    expect_truehd("English - TRUE-HD - 7.1", true);
    expect_truehd("English - MLP - 5.1", true);
    expect_truehd("truehd", true);

    /* E-AC-3 is Dolby too, and can even carry Atmos — but nothing here
     * decodes it, so it must NOT be requested as a copy. */
    expect_truehd("English - EAC3 - 5.1", false);
    expect_truehd("English - EAC3 Atmos - 5.1", false);
    expect_truehd("English - DD+ Atmos - 5.1", false);
    expect_truehd("English - AC3 - 5.1", false);
    expect_truehd("English - DTS-HD MA - 7.1", false);
    expect_truehd("", false);
    expect_truehd(NULL, false);

    /* The combined question routes each codec to its own decoder */
    expect_hd("English - DTS-HD MA - 5.1", true);
    expect_hd("English - TrueHD Atmos - 7.1", true);
    expect_hd("English - EAC3 - 5.1", false);
    expect_hd("English - AAC - 2.0", false);

    /* Word-boundary cases: "dts" inside a longer word is not a codec.
     * Track names carry film and studio names, so this matters. */
    expect("Bandts Live - AAC - 2.0", false);
    expect("English - ADTS - 2.0", false);     /* an AAC container, not DTS */
    expect("English - DTSX2 - 5.1", false);    /* not a codec name we know */

    if (g_failures) {
        printf("test_track_codec: %d FAILURES\n", g_failures);
        return 1;
    }
    printf("test_track_codec: DTS and TrueHD track detection correct\n");
    return 0;
}
