// Host test for the viewer's Live TV arrangement (source/api/livetv_prefs.c).
//
//   make -f Makefile.host test_livetv_prefs && ./test_livetv_prefs
#include <stdio.h>
#include <string.h>

#include "livetv_prefs.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const int BASES[] = { 1, 100, 200, 1000, 2000, 7000, 19000 };
static const int NB = (int)(sizeof BASES / sizeof BASES[0]);

static void hidden(void) {
    printf("- hiding\n");
    LtPrefs p; lp_init(&p);
    CHECK(!lp_is_hidden(&p, 7000));
    lp_set_hidden(&p, 7000, true);
    lp_set_hidden(&p, 7000, true);                       // twice: one entry
    CHECK(lp_is_hidden(&p, 7000) && p.n_hidden == 1);
    lp_set_hidden(&p, 2000, true);
    lp_set_hidden(&p, 7000, false);
    CHECK(!lp_is_hidden(&p, 7000) && lp_is_hidden(&p, 2000) && p.n_hidden == 1);
    lp_set_hidden(&p, 5, false);                         // not there: nothing
    lp_set_hidden(&p, 0, true);                          // 0 is "none"
    CHECK(p.n_hidden == 1);
    for (int i = 1; i < 100; i++) lp_set_hidden(&p, 100 + i, true);
    CHECK(p.n_hidden == LP_MAX_HIDDEN);                  // full: the rest are ignored
}

static void recent(void) {
    printf("- recent\n");
    LtPrefs p; lp_init(&p);
    lp_note_recent(&p, "aaa"); lp_note_recent(&p, "bbb"); lp_note_recent(&p, "ccc");
    CHECK(p.n_recent == 3 && !strcmp(p.recent[0], "ccc") && !strcmp(p.recent[2], "aaa"));
    lp_note_recent(&p, "aaa");                           // again: to the front, still one
    CHECK(p.n_recent == 3 && !strcmp(p.recent[0], "aaa") && !strcmp(p.recent[1], "ccc") &&
          !strcmp(p.recent[2], "bbb"));
    CHECK(lp_recent_rank(&p, "bbb") == 2 && lp_recent_rank(&p, "zzz") == LP_MAX_RECENT);
    char id[16];
    for (int i = 0; i < 30; i++) { snprintf(id, sizeof id, "ch%d", i); lp_note_recent(&p, id); }
    CHECK(p.n_recent == LP_MAX_RECENT);
    CHECK(!strcmp(p.recent[0], "ch29") && !strcmp(p.recent[LP_MAX_RECENT - 1], "ch18"));
    CHECK(lp_recent_rank(&p, "aaa") == LP_MAX_RECENT);   // fell off
    // Re-noting one in the middle of a full list.
    lp_note_recent(&p, "ch20");
    CHECK(p.n_recent == LP_MAX_RECENT && !strcmp(p.recent[0], "ch20") && !strcmp(p.recent[1], "ch29"));
    // Things that are not ids are refused.
    const int n = p.n_recent;
    lp_note_recent(&p, ""); lp_note_recent(&p, NULL); lp_note_recent(&p, "has space");
    lp_note_recent(&p, "new\nline"); lp_note_recent(&p, "0123456789012345678901234567890123456789");
    CHECK(p.n_recent == n && !strcmp(p.recent[0], "ch20"));
}

static void file(void) {
    printf("- the file\n");
    LtPrefs p; lp_init(&p);
    lp_set_hidden(&p, 7000, true); lp_set_hidden(&p, LP_RECENT, true);
    lp_note_recent(&p, "11111111111111111111111111111111");
    lp_note_recent(&p, "22222222222222222222222222222222");
    p.last = 200;
    char buf[1024];
    const int n = lp_format(&p, buf, sizeof buf);
    CHECK(n > 0 && n == (int)strlen(buf));
    LtPrefs q;
    CHECK(lp_parse(buf, &q) == 5);
    CHECK(q.n_hidden == 2 && lp_is_hidden(&q, 7000) && lp_is_hidden(&q, LP_RECENT));
    CHECK(q.n_recent == 2 && !strcmp(q.recent[0], "22222222222222222222222222222222") &&
          !strcmp(q.recent[1], "11111111111111111111111111111111"));
    CHECK(q.last == 200);
    char again[1024];
    CHECK(lp_format(&q, again, sizeof again) == n && !strcmp(again, buf));
    // Too small a buffer is an error, never a half-written file.
    CHECK(lp_format(&p, buf, 10) == -1);
    CHECK(lp_format(&p, buf, 0) == -1);

    // Junk, CRLF, duplicates, bad keys.
    CHECK(lp_parse("hide 7000\r\nhide x\nhide 0\nhide -9\nlast 0\nlast abc\nrecent a b\nfoo bar\n\n"
                   "recent has/slash\nrecent dup\nrecent dup\nlast 1000", &q) == 4);
    CHECK(lp_is_hidden(&q, 7000) && q.n_hidden == 1 && q.n_recent == 2 && q.last == 1000);
    CHECK(lp_parse("", &q) == 0 && q.n_hidden == 0 && q.n_recent == 0 && q.last == 0);
    CHECK(lp_parse(NULL, &q) == 0);
    // More recents than fit: the first LP_MAX_RECENT (the newest) win.
    char big[2048] = "";
    for (int i = 0; i < 30; i++) { char l[32]; snprintf(l, sizeof l, "recent c%d\n", i); strcat(big, l); }
    lp_parse(big, &q);
    CHECK(q.n_recent == LP_MAX_RECENT && !strcmp(q.recent[0], "c0"));
}

static void pages(void) {
    printf("- pages\n");
    LtPrefs p; lp_init(&p);
    int keys[32];
    // A new viewer: My Channels, then every category (no recents yet).
    int n = lp_build_pages(&p, BASES, NB, keys, 32);
    CHECK(n == 1 + NB && keys[0] == LP_FAVS && keys[1] == 1 && keys[n - 1] == 19000);
    // Opens on the first category, not on an empty My Channels.
    CHECK(lp_pick_page(keys, n, 0) == 1);
    // A viewer who has watched something gets Recently watched after My Channels.
    lp_note_recent(&p, "abc");
    n = lp_build_pages(&p, BASES, NB, keys, 32);
    CHECK(n == 2 + NB && keys[0] == LP_FAVS && keys[1] == LP_RECENT && keys[2] == 1);
    // Hidden pages go.
    lp_set_hidden(&p, 7000, true); lp_set_hidden(&p, 2000, true); lp_set_hidden(&p, LP_FAVS, true);
    n = lp_build_pages(&p, BASES, NB, keys, 32);
    CHECK(n == 1 + NB - 2 && keys[0] == LP_RECENT);
    for (int i = 0; i < n; i++) CHECK(keys[i] != 7000 && keys[i] != 2000 && keys[i] != LP_FAVS);
    CHECK(lp_visible_categories(&p, BASES, NB) == NB - 2);
    // Everything hidden: the categories come back, so the tab is never empty.
    for (int i = 0; i < NB; i++) lp_set_hidden(&p, BASES[i], true);
    CHECK(lp_visible_categories(&p, BASES, NB) == 0);
    n = lp_build_pages(&p, BASES, NB, keys, 32);
    CHECK(n == 1 + NB && keys[0] == LP_RECENT && keys[1] == 1);
    // Room: never writes past `max`.
    CHECK(lp_build_pages(&p, BASES, NB, keys, 3) == 3);
    CHECK(lp_build_pages(&p, BASES, 0, keys, 32) == 1);          // no categories at all
    // Picking: the page asked for, else the first category, else 0.
    int k[] = { LP_FAVS, LP_RECENT, 1, 100 };
    CHECK(lp_pick_page(k, 4, 100) == 3 && lp_pick_page(k, 4, LP_RECENT) == 1);
    CHECK(lp_pick_page(k, 4, 7000) == 2);                       // hidden since: the first category
    CHECK(lp_pick_page(k, 2, 0) == 0 && lp_pick_page(k, 0, 5) == 0);
}

int main() {
    hidden();
    recent();
    file();
    pages();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
