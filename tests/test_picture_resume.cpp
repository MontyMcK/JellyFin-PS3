// Host test for the player's picture size (source/player/core/picture_size.h)
// and the "where did it stop" table (source/ui/xmb/ui_resume.cpp).
//
//   make -f Makefile.host test_picture_resume && ./test_picture_resume

#include "picture_size.h"
#include "ui_resume.h"

#include <stdio.h>
#include <string.h>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void picture(void) {
    printf("- picture size\n");
    PicSize p = { PIC_DEFAULT, PIC_DEFAULT };
    CHECK(pic_is_default(&p));
    // The axes move on their own.
    p = pic_step(p, -6, 0);
    CHECK(p.w == 94 && p.h == 100);
    p = pic_step(p, 0, -10);
    CHECK(p.w == 94 && p.h == 90);
    // Clamped at both ends, and a held key at the end changes nothing.
    for (int i = 0; i < 200; i++) p = pic_step(p, -1, -1);
    CHECK(p.w == PIC_MIN && p.h == PIC_MIN);
    for (int i = 0; i < 200; i++) p = pic_step(p, +1, +1);
    CHECK(p.w == PIC_MAX && p.h == PIC_MAX);

    // The saved file: round trip, clamping, and junk leaves the value alone.
    char buf[32];
    PicSize a = { 94, 88 }, b = { 100, 100 };
    pic_format(&a, buf, sizeof buf);
    CHECK(pic_parse(buf, &b) && b.w == 94 && b.h == 88);
    CHECK(pic_parse("10 500\n", &b) && b.w == PIC_MIN && b.h == PIC_MAX);
    PicSize keep = { 77, 66 };
    CHECK(!pic_parse("", &keep) && keep.w == 77 && keep.h == 66);
    CHECK(!pic_parse("garbage", &keep) && keep.w == 77);
    CHECK(!pic_parse("90", &keep) && keep.w == 77 && keep.h == 66);
    CHECK(!pic_parse(NULL, &keep));
}

static void aspect(void) {
    printf("- aspect\n");
    unsigned ow, oh;
    // A 16:9 film fitted to a 1440x1080 visual screen (4:3): full width, 810 high.
    pic_aspect_apply(PIC_FIT, 1440, 810, 1440, 1080, &ow, &oh);
    CHECK(ow == 1440 && oh == 810);
    // Fill: the height is the screen's, the width runs 1920 wide (240 cropped off each side).
    pic_aspect_apply(PIC_FILL, 1440, 810, 1440, 1080, &ow, &oh);
    CHECK(oh == 1080 && ow == 1920);
    // ... and the shape is the film's: 1920/1080 is 16:9.
    CHECK((unsigned long long)ow * 9 == (unsigned long long)oh * 16);
    // Stretch is the screen, whatever the film.
    pic_aspect_apply(PIC_STRETCH, 1440, 810, 1440, 1080, &ow, &oh);
    CHECK(ow == 1440 && oh == 1080);
    // A 4:3 film on a 4:3 screen: Fill changes nothing.
    pic_aspect_apply(PIC_FILL, 1440, 1080, 1440, 1080, &ow, &oh);
    CHECK(ow == 1440 && oh == 1080);
    // 2.40:1 on a 16:9 screen: Fill grows it past the sides, height is the screen's.
    pic_aspect_apply(PIC_FILL, 1920, 800, 1920, 1080, &ow, &oh);
    CHECK(oh == 1080 && ow >= 2592 && ow <= 2593);
    // A 4:3 film on a 16:9 screen (pillarbox): Fill fills the width, crops top and bottom.
    pic_aspect_apply(PIC_FILL, 1440, 1080, 1920, 1080, &ow, &oh);
    CHECK(ow == 1920 && oh == 1440);
    // Never smaller than the screen on both axes, and never below the fit.
    for (unsigned sh = 300; sh <= 1080; sh += 37) {
        pic_aspect_apply(PIC_FILL, 1920, sh, 1920, 1080, &ow, &oh);
        CHECK(ow >= 1920 && oh >= 1080 && ow >= 1920 && oh >= sh);
    }
    // Degenerate sizes do not divide by zero.
    pic_aspect_apply(PIC_FILL, 0, 0, 1920, 1080, &ow, &oh);
    CHECK(ow == 0 && oh == 0);
    // The saved name round trips; junk is Fit.
    for (int a = 0; a < PIC_ASPECTS; a++) CHECK(pic_aspect_parse(pic_aspect_name(a)) == a);
    CHECK(pic_aspect_parse("") == PIC_FIT && pic_aspect_parse("zoom") == PIC_FIT && pic_aspect_parse(NULL) == PIC_FIT);
    CHECK(pic_aspect_next(PIC_FIT) == PIC_FILL && pic_aspect_next(PIC_FILL) == PIC_STRETCH &&
          pic_aspect_next(PIC_STRETCH) == PIC_FIT);
    // A non-default aspect is not the default picture.
    PicSize p = { PIC_DEFAULT, PIC_DEFAULT, PIC_FILL };
    CHECK(!pic_is_default(&p));
}

static void rule(void) {
    printf("- resume rule\n");
    // Total 10 000 s: under 5 % (500 s) or 90 % (9 000 s) and over is a restart.
    CHECK(resume_secs_after(5, 10000) == 0);
    CHECK(resume_secs_after(499, 10000) == 0);
    CHECK(resume_secs_after(500, 10000) == 500);
    CHECK(resume_secs_after(8999, 10000) == 8999);
    CHECK(resume_secs_after(9000, 10000) == 0);
    // Under 10 s is nothing at any length; unknown length keeps only that rule.
    CHECK(resume_secs_after(9, 0) == 0);
    CHECK(resume_secs_after(10, 0) == 10);
    CHECK(resume_secs_after(3000, 0) == 3000);
    // A short clip: 5 % of 100 s is 5 s, so the 10 s floor decides.
    CHECK(resume_secs_after(10, 100) == 10);
    CHECK(resume_secs_after(90, 100) == 0);
}

static void table(void) {
    printf("- table\n");
    // Nothing played: the listed position stands.
    CHECK(xmb_resume_for("movie", 0) == 0);
    CHECK(xmb_resume_for("movie", 1234) == 1234);
    // Watched part way, backed out: the stale list entry (0) is overridden.
    xmb_resume_note("movie", 3000, 8000);
    CHECK(xmb_resume_for("movie", 0) == 3000);
    CHECK(xmb_resume_for("other", 0) == 0);
    // Finished: back to the start, even though the list said otherwise.
    xmb_resume_note("movie", 7900, 8000);
    CHECK(xmb_resume_for("movie", 3000) == 0);
    // Empty ids are ignored.
    xmb_resume_note("", 3000, 8000);
    xmb_resume_note(NULL, 3000, 8000);
    // Past the table size the oldest are forgotten, the newest kept.
    char id[16];
    for (int i = 0; i < 100; i++) {
        snprintf(id, sizeof id, "item%d", i);
        xmb_resume_note(id, 1000 + (uint32_t)i, 0);
    }
    CHECK(xmb_resume_for("item99", 0) == 1099);
    CHECK(xmb_resume_for("item50", 0) == 1050);
    CHECK(xmb_resume_for("item0", 7) == 7);          // fell off
    // Re-noting an item keeps one entry for it.
    xmb_resume_note("item99", 2000, 0);
    CHECK(xmb_resume_for("item99", 0) == 2000);
}

int main() {
    picture();
    aspect();
    rule();
    table();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
