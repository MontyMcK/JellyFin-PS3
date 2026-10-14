// Host test for source/local/local_music.c: a folder of music files as an album.
//
//   make -f Makefile.host test_local_music && ./test_local_music

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "local_music.h"
#include "lfs_path.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static LmTrack track(const char *path, const char *title = "", const char *artist = "", const char *album = "",
                     int no = 0, int disc = 0, uint32_t secs = 0) {
    LaMeta m;
    memset(&m, 0, sizeof m);
    m.kind = LAF_FLAC;
    snprintf(m.title, sizeof m.title, "%s", title);
    snprintf(m.artist, sizeof m.artist, "%s", artist);
    snprintf(m.album, sizeof m.album, "%s", album);
    m.track_no = no; m.disc_no = disc; m.duration_secs = secs;
    LmTrack t;
    lm_track_fill(&t, path, &m);
    return t;
}

static void test_fill() {
    printf("- records\n");
    LaMeta m;
    memset(&m, 0, sizeof m);
    m.kind = LAF_MP3; m.track_no = 4; m.disc_no = 2; m.duration_secs = 187; m.pic_off = 1234; m.pic_len = 5678;
    snprintf(m.title, sizeof m.title, "T"); snprintf(m.artist, sizeof m.artist, "A"); snprintf(m.album, sizeof m.album, "B");
    LmTrack t;
    lm_track_fill(&t, "usb0:/Music/a.mp3", &m);
    CHECK(!strcmp(t.path, "usb0:/Music/a.mp3") && !strcmp(t.title, "T") && !strcmp(t.artist, "A") && !strcmp(t.album, "B"));
    CHECK(t.track_no == 4 && t.disc_no == 2 && t.duration_secs == 187 && t.kind == LAF_MP3 && t.pic_off == 1234 && t.pic_len == 5678);
    // a path longer than the field is cut, not overrun
    char longp[400];
    memset(longp, 'a', sizeof longp - 1);
    longp[sizeof longp - 1] = '\0';
    lm_track_fill(&t, longp, &m);
    CHECK(strlen(t.path) == sizeof t.path - 1);
}

static void test_order() {
    printf("- the order of an album\n");
    LmTrack t[8];
    int n = 0;
    t[n++] = track("usb0:/a/10 Ten.flac", "", "", "", 10);
    t[n++] = track("usb0:/a/zz untagged b.flac");
    t[n++] = track("usb0:/a/02 Two.flac", "", "", "", 2);
    t[n++] = track("usb0:/a/01 One.flac", "", "", "", 1);
    t[n++] = track("usb0:/a/aa untagged a.flac");
    t[n++] = track("usb0:/a/d2t1.flac", "", "", "", 1, 2);                 // disc 2
    t[n++] = track("usb0:/a/x.flac", "", "", "", 3, 1);                    // disc 1 written out
    t[n++] = track("usb0:/a/Track 9.flac");
    lm_sort(t, n);
    // disc 1 (an untagged disc is disc 1) by track number, the untagged tracks after them by name, then disc 2
    const char *want2[] = { "01 One.flac", "02 Two.flac", "x.flac", "10 Ten.flac", "aa untagged a.flac", "Track 9.flac", "zz untagged b.flac", "d2t1.flac" };
    for (int i = 0; i < n; i++) CHECK(!strcmp(lfs_path_leaf(t[i].path), want2[i]));
    // natural order of names with the same number
    LmTrack u[3] = { track("usb0:/b/Song 10.flac"), track("usb0:/b/Song 9.flac"), track("usb0:/b/song 2.flac") };
    lm_sort(u, 3);
    CHECK(!strcmp(lfs_path_leaf(u[0].path), "song 2.flac") && !strcmp(lfs_path_leaf(u[1].path), "Song 9.flac") && !strcmp(lfs_path_leaf(u[2].path), "Song 10.flac"));
    // a consistent order: sorting twice, or from the other end, gives the same
    LmTrack v[3] = { u[2], u[0], u[1] };
    lm_sort(v, 3);
    for (int i = 0; i < 3; i++) CHECK(!strcmp(v[i].path, u[i].path));
    CHECK(lm_cmp(&u[0], &u[0]) == 0 && lm_cmp(&u[0], &u[1]) < 0 && lm_cmp(&u[1], &u[0]) > 0);
    lm_sort(u, 0);
    lm_sort(u, 1);
}

static void test_titles() {
    printf("- names to show\n");
    char o[128];
    auto title_of = [&](const char *path, const char *tag = "") { LmTrack t = track(path, tag); lm_display_title(&t, o, sizeof o); return o; };
    CHECK(!strcmp(title_of("usb0:/m/whatever.flac", "Tagged Title"), "Tagged Title"));
    CHECK(!strcmp(title_of("usb0:/m/03 - Song Name.flac"), "Song Name"));
    CHECK(!strcmp(title_of("usb0:/m/03. Song Name.mp3"), "Song Name"));
    CHECK(!strcmp(title_of("usb0:/m/03) Song.wav"), "Song"));
    CHECK(!strcmp(title_of("usb0:/m/1-04 Song.flac"), "Song"));
    CHECK(!strcmp(title_of("usb0:/m/1-04 - Song.flac"), "Song"));
    CHECK(!strcmp(title_of("usb0:/m/07_Song_Name.mp3"), "Song Name"));
    CHECK(!strcmp(title_of("usb0:/m/01 Song.mp3"), "Song"));                // a leading zero: a track number
    CHECK(!strcmp(title_of("usb0:/m/10 Years.mp3"), "10 Years"));           // not one
    CHECK(!strcmp(title_of("usb0:/m/2112.flac"), "2112"));
    CHECK(!strcmp(title_of("usb0:/m/1984 - Title.flac"), "1984 - Title"));
    CHECK(!strcmp(title_of("usb0:/m/05.flac"), "05"));                      // nothing after the number: it is the name
    CHECK(!strcmp(title_of("usb0:/m/05 - .flac"), "05 -"));
    CHECK(!strcmp(title_of("usb0:/m/Plain Name.FLAC"), "Plain Name"));
    CHECK(!strcmp(title_of("usb0:/m/Dots.In.Name.flac"), "Dots.In.Name"));
    CHECK(!strcmp(title_of("usb0:/m/.flac"), ".flac"));
    CHECK(!strcmp(title_of("usb0:/m/noext"), "noext"));
    CHECK(!strcmp(title_of("hdd:/music/Café.flac"), "Caf\xC3\xA9"));
    // the output is cut to the room there is
    LmTrack t = track("usb0:/m/A long long name.flac");
    char small[8];
    lm_display_title(&t, small, sizeof small);
    CHECK(!strcmp(small, "A long"));                                       // cut to 7, the space it ends on dropped
    lm_display_title(&t, small, 0);
}

static void test_stem_and_albums() {
    printf("- stems and one-album folders\n");
    char o[96];
    lm_stem_title("usb0:/m/02 - Name_Here.mp3", o, sizeof o);
    CHECK(!strcmp(o, "Name Here"));
    lm_stem_title("plain.flac", o, sizeof o);
    CHECK(!strcmp(o, "plain"));
    lm_stem_title("x", o, 0);
    LmTrack a[3] = { track("a/1.flac", "", "", "One Album"), track("a/2.flac", "", "", ""), track("a/3.flac", "", "", "One Album") };
    CHECK(lm_is_one_album(a, 3) && lm_is_one_album(a, 0));
    a[1] = track("a/2.flac", "", "", "Another");
    CHECK(!lm_is_one_album(a, 3));
    LmTrack u[2] = { track("a/1.flac"), track("a/2.flac") };
    CHECK(lm_is_one_album(u, 2));                                          // untagged: as good as one
}

static void test_album() {
    printf("- the album's name and artist\n");
    char o[128];
    {
        LmTrack t[4] = { track("a/1.flac", "", "X", "Greatest Hits"), track("a/2.flac", "", "X", "Greatest Hits"),
                         track("a/3.flac", "", "X", "Bonus"), track("a/4.flac", "", "X", "") };
        lm_album_name(t, 4, "Folder_Name", o, sizeof o);
        CHECK(!strcmp(o, "Greatest Hits"));
        lm_album_artist(t, 4, o, sizeof o);
        CHECK(!strcmp(o, "X"));
    }
    {   // untagged: the folder, underscores as spaces
        LmTrack t[2] = { track("a/1.flac"), track("a/2.flac") };
        lm_album_name(t, 2, "My_Old_Album", o, sizeof o);
        CHECK(!strcmp(o, "My Old Album"));
        lm_album_artist(t, 2, o, sizeof o);
        CHECK(o[0] == '\0');
        lm_album_name(t, 2, NULL, o, sizeof o);
        CHECK(o[0] == '\0');
        lm_album_name(t, 0, "F", o, sizeof o);
        CHECK(!strcmp(o, "F"));
    }
    {   // artists: 7 of 10 is the album's; fewer is a compilation
        LmTrack t[10];
        for (int i = 0; i < 10; i++) t[i] = track("a/x.flac", "", i < 7 ? "Band" : "Guest", "");
        lm_album_artist(t, 10, o, sizeof o);
        CHECK(!strcmp(o, "Band"));
        for (int i = 0; i < 10; i++) t[i] = track("a/x.flac", "", i < 6 ? "Band" : "Guest", "");
        lm_album_artist(t, 10, o, sizeof o);
        CHECK(!strcmp(o, "Various Artists"));
        // untagged tracks do not count against the one that is tagged
        LmTrack u[5] = { track("a/1.flac", "", "Solo"), track("a/2.flac"), track("a/3.flac", "", "Solo"), track("a/4.flac"), track("a/5.flac") };
        lm_album_artist(u, 5, o, sizeof o);
        CHECK(!strcmp(o, "Solo"));
    }
    {
        LmTrack t[3] = { track("a/1.flac"), track("a/2.flac"), track("a/3.flac") };
        CHECK(lm_pick_art(t, 3) == -1);
        t[1].pic_len = 10;
        t[2].pic_len = 20;
        CHECK(lm_pick_art(t, 3) == 1);
        CHECK(lm_pick_art(t, 0) == -1);
    }
}

static void test_art_registry() {
    printf("- the cover registry\n");
    char k1[32], k2[32];
    lm_art_register("usb0:/a/1.flac", 100, 2000, k1, sizeof k1);
    lm_art_register("usb0:/a/cover.jpg", 0, 0, k2, sizeof k2);
    CHECK(lm_art_is_key(k1) && lm_art_is_key(k2) && strcmp(k1, k2) != 0);
    LmArt a;
    CHECK(lm_art_find(k1, &a) && !strcmp(a.path, "usb0:/a/1.flac") && a.off == 100 && a.len == 2000);
    CHECK(lm_art_find(k2, &a) && !strcmp(a.path, "usb0:/a/cover.jpg") && a.off == 0 && a.len == 0);
    // keys that are not ours, or malformed
    CHECK(!lm_art_is_key("abc") && !lm_art_is_key(NULL) && !lm_art_is_key("lm"));
    CHECK(!lm_art_find("0123456789abcdef", &a) && !lm_art_find("lm:", &a) && !lm_art_find("lm:0", &a) && !lm_art_find("lm:12x", &a));
    CHECK(!lm_art_find("lm:99999999", &a) && !lm_art_find("logo:xyz", &a));
    // the oldest are replaced once the table is full
    char keys[12][32];
    for (int i = 0; i < 12; i++) {
        char p[40];
        snprintf(p, sizeof p, "usb0:/x/%d.jpg", i);
        lm_art_register(p, 0, 0, keys[i], sizeof keys[i]);
    }
    CHECK(!lm_art_find(k1, &a) && !lm_art_find(keys[0], &a) && !lm_art_find(keys[3], &a));
    CHECK(lm_art_find(keys[4], &a) && !strcmp(a.path, "usb0:/x/4.jpg"));
    CHECK(lm_art_find(keys[11], &a) && !strcmp(a.path, "usb0:/x/11.jpg"));
    // the key is cut to the room, not overrun
    char tiny[4];
    lm_art_register("usb0:/y.jpg", 0, 0, tiny, sizeof tiny);
    CHECK(strlen(tiny) == 3);
    lm_art_register("usb0:/y.jpg", 0, 0, NULL, 0);
}

static void *hammer(void *arg) {
    const int id = (int)(intptr_t)arg;
    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        char p[40], k[32];
        snprintf(p, sizeof p, "usb0:/t%d/%d.jpg", id, i);
        lm_art_register(p, (uint64_t)i, (uint32_t)i, k, sizeof k);
        LmArt a;
        if (lm_art_find(k, &a)) {                         // may already be replaced by the others; when found it is intact
            if (strcmp(a.path, p) != 0 || a.off != (uint64_t)i || a.len != (uint32_t)i) bad++;
        }
    }
    return (void *)(intptr_t)bad;
}

static void test_threads() {
    printf("- from several threads\n");
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, hammer, (void *)(intptr_t)i);
    int bad = 0;
    for (int i = 0; i < 4; i++) { void *r; pthread_join(th[i], &r); bad += (int)(intptr_t)r; }
    CHECK(bad == 0);
}

int main() {
    test_fill();
    test_order();
    test_titles();
    test_stem_and_albums();
    test_album();
    test_art_registry();
    test_threads();
    printf("local music: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
