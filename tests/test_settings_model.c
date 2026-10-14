// Host test for the Settings model (source/ui/settings_model.c): the rows,
// their sections and help text, and the layout and scroll rules.  Built twice,
// with the player statistics row compiled in and out:
//
//   make -f Makefile.host test_settings_model test_settings_model_nostats
#include <stdio.h>
#include <string.h>

#ifndef ENABLE_PLAYER_STATS
#define ENABLE_PLAYER_STATS 1
#endif
#include "settings_model.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void rows(void) {
    printf("- rows\n");
    const int n = settings_count();
    int seen[SET__COUNT] = { 0 };
    for (int i = 0; i < n; i++) {
        const setting_row *r = settings_row(i);
        CHECK(r != NULL);
        if (!r) continue;
        CHECK((int)r->id >= 0 && (int)r->id < SET__COUNT);
        seen[r->id]++;
        CHECK(settings_index_of(r->id) == i);
        // Labels fit the row.
        CHECK(strlen(r->label) >= 1 && strlen(r->label) <= 24);
        // Help is exactly two lines, each short enough for the panel.
        const char *nl = strchr(r->help, '\n');
        CHECK(nl != NULL && strchr(nl + 1, '\n') == NULL);
        if (nl) {
            CHECK((int)(nl - r->help) >= 1 && (int)(nl - r->help) <= 72);
            CHECK(strlen(nl + 1) >= 1 && strlen(nl + 1) <= 72);
            if ((int)(nl - r->help) > 72 || strlen(nl + 1) > 72)
                printf("  too long: %s\n", r->label);
        }
    }
    // Every id exactly once, except statistics when it is compiled out.
    for (int id = 0; id < SET__COUNT; id++) {
        const int want = (id == SET_STATS && !ENABLE_PLAYER_STATS) ? 0 : 1;
        CHECK(seen[id] == want);
        if (seen[id] != want) printf("  id %d seen %d times\n", id, seen[id]);
    }
    CHECK(n == SET__COUNT - (ENABLE_PLAYER_STATS ? 0 : 1));
    CHECK(settings_row(-1) == NULL && settings_row(n) == NULL);
    if (!ENABLE_PLAYER_STATS) CHECK(settings_index_of(SET_STATS) == -1);
}

static void sections(void) {
    printf("- sections\n");
    const int n = settings_count();
    // Grouped: a section never reappears after another has started.
    int closed[SEC__COUNT] = { 0 };
    int cur = -1;
    for (int i = 0; i < n; i++) {
        const int s = (int)settings_row(i)->section;
        if (s != cur) {
            CHECK(!closed[s]);
            if (cur >= 0) closed[cur] = 1;
            cur = s;
        }
    }
    // The first row of each non-empty section, and -1 for an empty one.
    for (int s = 0; s < SEC__COUNT; s++) {
        const int first = settings_section_first((setting_section)s);
        CHECK(first >= 0);                           // none is empty here
        if (first < 0) continue;
        CHECK((int)settings_row(first)->section == s);
        CHECK(first == 0 || (int)settings_row(first - 1)->section != s);
        CHECK(settings_section_label((setting_section)s)[0] != '\0');
    }
    CHECK(settings_section_first(SEC__COUNT) == -1);
    // Log Out sits last, in System: it is the most destructive action.
    CHECK(settings_index_of(SET_LOGOUT) == n - 1);
    // First focus is 1080p Playback, the first row of Playback.
    CHECK(settings_row(0)->id == SET_HD1080);

}

static void categories(void) {
    printf("- categories\n");
    const int n = settings_count();
    CHECK(settings_category_count() == SEC__COUNT);
    int total = 0;
    for (int i = 0; i < settings_category_count(); i++) {
        const setting_section sec = settings_category_at(i);
        CHECK((int)sec == i);                                  // in section order
        CHECK(settings_category_index(sec) == i);
        const int rc = settings_category_row_count(sec);
        CHECK(rc >= 1);
        total += rc;
        // Its rows, in order, all belong to it, and map back to their position.
        for (int k = 0; k < rc; k++) {
            const int row = settings_category_row(sec, k);
            CHECK(row >= 0 && row < n);
            if (row < 0) continue;
            CHECK(settings_row(row)->section == sec);
            CHECK(settings_row_in_category(row) == k);
        }
        CHECK(settings_category_row(sec, rc) == -1);
        CHECK(settings_category_row(sec, -1) == -1);
        CHECK(settings_category_row(sec, 0) == settings_section_first(sec));
    }
    CHECK(total == n);                                          // every row is in exactly one category
    CHECK(settings_category_at(-1) == SEC__COUNT && settings_category_at(SEC__COUNT) == SEC__COUNT);
    CHECK(settings_category_index(SEC__COUNT) == -1 && settings_category_index((setting_section)-1) == -1);
    CHECK(settings_row_in_category(-1) == -1 && settings_row_in_category(n) == -1);
    // The ones the screen's icons are chosen for.
    CHECK(settings_row(settings_category_row(SEC_DOWNLOADS, 0))->id == SET_DL_LOCATION);
    CHECK(settings_row(settings_category_row(SEC_SYSTEM, settings_category_row_count(SEC_SYSTEM) - 1))->id == SET_LOGOUT);
}

static int fully_visible(int n, int sel, int item_h, int gap, int band_h, int scroll) {
    const int top = sel * (item_h + gap) - scroll;
    return top >= 0 && top + item_h <= band_h && scroll >= 0 &&
           scroll <= (n * item_h + (n - 1) * gap > band_h ? n * item_h + (n - 1) * gap - band_h : 0);
}

static void scrolling(void) {
    printf("- scrolling\n");
    // The sizes the screen runs at: 720p, 1080p, a band barely taller than one
    // item, and a short 480 line band; with 6 and with 7 items.
    static const struct { int item_h, gap, band_h; } G[] = {
        { 64, 8, 330 }, { 96, 12, 560 }, { 64, 8, 64 }, { 43, 5, 150 }, { 52, 8, 400 },
    };
    for (unsigned g = 0; g < sizeof G / sizeof G[0]; g++) {
        for (int n = 1; n <= 8; n++) {
            int scroll = 0;
            for (int sel = 0; sel < n; sel++) {                 // down, as the d-pad does
                scroll = settings_scroll_to(n, sel, G[g].item_h, G[g].gap, G[g].band_h, scroll);
                CHECK(fully_visible(n, sel, G[g].item_h, G[g].gap, G[g].band_h, scroll));
            }
            for (int sel = n - 1; sel >= 0; sel--) {            // and back up
                scroll = settings_scroll_to(n, sel, G[g].item_h, G[g].gap, G[g].band_h, scroll);
                CHECK(fully_visible(n, sel, G[g].item_h, G[g].gap, G[g].band_h, scroll));
            }
            for (int from = 0; from < n; from++) {              // any jump from any position
                const int s0 = settings_scroll_to(n, from, G[g].item_h, G[g].gap, G[g].band_h, 0);
                for (int sel = 0; sel < n; sel++)
                    CHECK(fully_visible(n, sel, G[g].item_h, G[g].gap, G[g].band_h,
                                        settings_scroll_to(n, sel, G[g].item_h, G[g].gap, G[g].band_h, s0)));
            }
            // An item already on screen does not move the list.
            const int s1 = settings_scroll_to(n, 0, G[g].item_h, G[g].gap, G[g].band_h, 0);
            CHECK(settings_scroll_to(n, 0, G[g].item_h, G[g].gap, G[g].band_h, s1) == s1);
        }
    }
    // Everything fits: nothing scrolls, whatever was asked.
    CHECK(settings_scroll_to(6, 5, 64, 8, 6 * 64 + 5 * 8, 40) == 0);
    CHECK(settings_scroll_to(3, 2, 64, 8, 1000, 0) == 0);
    // Nothing to select: the scroll stays.
    CHECK(settings_scroll_to(0, 0, 64, 8, 300, 17) == 17);
    CHECK(settings_scroll_to(6, 9, 64, 8, 300, 17) == 17);
    CHECK(settings_scroll_to(6, -1, 64, 8, 300, 17) == 17);
}

int main(void) {
    rows();
    sections();
    categories();
    scrolling();
    printf("settings model (stats %s): %d checks, %d failed\n",
           ENABLE_PLAYER_STATS ? "in" : "out", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
