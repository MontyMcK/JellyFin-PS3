// Host test for source/local/lfs_path.cpp: path parsing and the browser's rules.
//
//   make -f Makefile.host test_lfs_path && ./test_lfs_path

#include "lfs_path.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string split_rest(const char *p, bool *ok, std::string *drive = nullptr) {
    char d[16], r[1100];
    *ok = lfs_path_split(p, d, sizeof d, r, sizeof r);
    if (drive) *drive = *ok ? d : "";
    return *ok ? r : "";
}

static void splitting() {
    printf("- split\n");
    bool ok; std::string drive;
    CHECK(split_rest("usb0:/Movies/a.mkv", &ok, &drive) == "/Movies/a.mkv" && ok && drive == "usb0");
    CHECK(split_rest("hdd:/video", &ok, &drive) == "/video" && ok && drive == "hdd");
    CHECK(split_rest("usb7:/", &ok, &drive) == "/" && ok && drive == "usb7");
    CHECK(split_rest("usb3:", &ok) == "/" && ok);
    CHECK(split_rest("usb1://a///b//", &ok) == "/a/b" && ok);            // slashes collapse
    CHECK(split_rest("usb1:/a b/\xC3\xA9", &ok) == "/a b/\xC3\xA9" && ok);
    split_rest("usb0:/a/../b", &ok);  CHECK(!ok);                          // no parent hops
    split_rest("usb0:/a/./b", &ok);   CHECK(!ok);
    split_rest("usb0:/..", &ok);      CHECK(!ok);
    split_rest("/usb0/a", &ok);       CHECK(!ok);                          // no drive
    split_rest("usb8:/a", &ok);       CHECK(!ok);                          // there is no port 8
    split_rest("usb:/a", &ok);        CHECK(!ok);
    split_rest("usb01:/a", &ok);      CHECK(!ok);
    split_rest("sd0:/a", &ok);        CHECK(!ok);
    split_rest("hdd0:/a", &ok);       CHECK(!ok);
    split_rest("", &ok);              CHECK(!ok);
    split_rest("dev_hdd0:/a", &ok);   CHECK(!ok);                          // lv2 paths are not ours
    CHECK(!lfs_path_split(nullptr, nullptr, 0, nullptr, 0));
    char d[16], r[8];
    CHECK(!lfs_path_split("usb0:/a/very/long/path", d, sizeof d, r, sizeof r));   // too small: refused, not cut
    CHECK(lfs_path_split("usb0:/a", d, sizeof d, nullptr, 0));                   // rest is optional
}

static void joining() {
    printf("- join, parent, leaf\n");
    char o[64];
    CHECK(lfs_path_join("usb0:/Movies", "a.mkv", o, sizeof o) && !strcmp(o, "usb0:/Movies/a.mkv"));
    CHECK(lfs_path_join("usb0:/", "Movies", o, sizeof o) && !strcmp(o, "usb0:/Movies"));
    CHECK(lfs_path_join("usb0:/Movies/", "a", o, sizeof o) && !strcmp(o, "usb0:/Movies/a"));
    CHECK(!lfs_path_join("usb0:/Movies", "a.mkv", o, 10));
    CHECK(lfs_path_join("usb0:/Movies", "a.mkv", o, 19));                 // exactly fits with the NUL
    CHECK(!lfs_path_join("usb0:/Movies", "a.mkv", o, 18));

    CHECK(lfs_path_parent("usb0:/Movies/Film/a.mkv", o, sizeof o) && !strcmp(o, "usb0:/Movies/Film"));
    CHECK(lfs_path_parent("usb0:/Movies/Film", o, sizeof o) && !strcmp(o, "usb0:/Movies"));
    CHECK(lfs_path_parent("usb0:/Movies", o, sizeof o) && !strcmp(o, "usb0:/"));
    CHECK(lfs_path_parent("usb0:/Movies/", o, sizeof o) && !strcmp(o, "usb0:/"));
    CHECK(!lfs_path_parent("usb0:/", o, sizeof o));
    CHECK(!lfs_path_parent("usb0:", o, sizeof o));
    CHECK(!lfs_path_parent("hdd:/", o, sizeof o));
    CHECK(!lfs_path_parent("nothing", o, sizeof o));

    CHECK(!strcmp(lfs_path_leaf("usb0:/Movies/a.mkv"), "a.mkv"));
    CHECK(!strcmp(lfs_path_leaf("usb0:/Movies/"), "Movies/") || !strcmp(lfs_path_leaf("usb0:/Movies/"), "Movies"));
    CHECK(!strncmp(lfs_path_leaf("usb0:/"), "usb0", 4));
    CHECK(!strncmp(lfs_path_leaf("hdd:/video"), "video", 5));
}

static void kinds() {
    printf("- media kinds\n");
    CHECK(lfs_media_kind_of("Film.mkv") == LFS_KIND_VIDEO);
    CHECK(lfs_media_kind_of("FILM.MKV") == LFS_KIND_VIDEO);
    CHECK(lfs_media_kind_of("a.b.c.ts") == LFS_KIND_VIDEO);
    CHECK(lfs_media_kind_of("00001.m2ts") == LFS_KIND_VIDEO);
    CHECK(lfs_media_kind_of("clip.MTS") == LFS_KIND_VIDEO);
    CHECK(lfs_media_kind_of("01 Song.flac") == LFS_KIND_MUSIC);
    CHECK(lfs_media_kind_of("Song.Mp3") == LFS_KIND_MUSIC);
    CHECK(lfs_media_kind_of("x.wav") == LFS_KIND_MUSIC);
    CHECK(lfs_media_kind_of("notes.txt") == LFS_KIND_OTHER);
    CHECK(lfs_media_kind_of("poster.jpg") == LFS_KIND_OTHER);
    CHECK(lfs_media_kind_of("mkv") == LFS_KIND_OTHER);                    // no dot
    CHECK(lfs_media_kind_of(".ts") == LFS_KIND_OTHER);                    // no name
    CHECK(lfs_media_kind_of("films.mkv.part") == LFS_KIND_OTHER);
    CHECK(lfs_media_kind_of("Film.mp4") == LFS_KIND_OTHER);               // not in 3.2
    CHECK(lfs_media_kind_of("") == LFS_KIND_OTHER);
    CHECK(lfs_media_kind_of("a.ts ") == LFS_KIND_OTHER);

    CHECK(lfs_name_hidden(".Trashes") && lfs_name_hidden("._Film.mkv") && lfs_name_hidden(".DS_Store"));
    CHECK(lfs_name_hidden("$RECYCLE.BIN") && lfs_name_hidden("$MFT"));
    CHECK(lfs_name_hidden("System Volume Information") && lfs_name_hidden("system volume information"));
    CHECK(lfs_name_hidden("lost+found") && lfs_name_hidden(""));
    CHECK(!lfs_name_hidden("Movies") && !lfs_name_hidden("a.$b") && !lfs_name_hidden("Films (2019)"));
}

static void ordering() {
    printf("- natural order\n");
    CHECK(lfs_natural_cmp("Episode 2", "Episode 10") < 0);
    CHECK(lfs_natural_cmp("Episode 10", "Episode 2") > 0);
    CHECK(lfs_natural_cmp("a", "A") == 0);
    CHECK(lfs_natural_cmp("b", "A") > 0);
    CHECK(lfs_natural_cmp("a01", "a1") == 0);                             // leading zeros do not count
    CHECK(lfs_natural_cmp("a007", "a8") < 0);
    CHECK(lfs_natural_cmp("a", "a1") < 0);
    CHECK(lfs_natural_cmp("track 9.flac", "track 10.flac") < 0);
    CHECK(lfs_natural_cmp("1 Intro", "2 Song") < 0);
    CHECK(lfs_natural_cmp("", "a") < 0 && lfs_natural_cmp("a", "") > 0 && lfs_natural_cmp("", "") == 0);
    CHECK(lfs_natural_cmp("99999999999999999999", "100000000000000000000") < 0);   // longer than any integer
    CHECK(lfs_natural_cmp("0", "00") == 0);
    CHECK(lfs_natural_cmp("a0", "a") > 0);

    // folders first, then natural; equal-ignoring-case names still have a fixed order
    CHECK(lfs_entry_cmp(true, "zeta", false, "alpha.mkv") < 0);
    CHECK(lfs_entry_cmp(false, "alpha.mkv", true, "zeta") > 0);
    CHECK(lfs_entry_cmp(true, "a", true, "B") < 0);
    CHECK(lfs_entry_cmp(false, "a.mkv", false, "A.mkv") != 0);
    CHECK(lfs_entry_cmp(false, "a.mkv", false, "A.mkv") == -lfs_entry_cmp(false, "A.mkv", false, "a.mkv"));

    std::vector<std::string> v = { "S01E10", "S01E2", "s01e1", "S01E02", "S2E1", "Extras" };
    std::sort(v.begin(), v.end(), [](const std::string &a, const std::string &b) {
        return lfs_entry_cmp(false, a.c_str(), false, b.c_str()) < 0; });
    CHECK((v == std::vector<std::string>{ "Extras", "s01e1", "S01E02", "S01E2", "S01E10", "S2E1" }));
}

int main() {
    splitting();
    joining();
    kinds();
    ordering();
    printf("lfs path: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
