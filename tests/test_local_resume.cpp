// Host test for source/local/local_resume.c: the resume table of files on a drive.
//
//   make -f Makefile.host test_local_resume && ./test_local_resume

#include "local_resume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <set>
#include <string>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void policy() {
    printf("- what is worth resuming\n");
    CHECK(!lres_worth_resuming(0, 7200) && !lres_worth_resuming(29, 7200));
    CHECK(lres_worth_resuming(30, 7200));
    CHECK(lres_worth_resuming(3600, 7200));
    CHECK(lres_worth_resuming(6800, 7200));                          // 94.4 %, and more than a minute from the end
    CHECK(!lres_worth_resuming(6840, 7200));                         // 95 %
    CHECK(!lres_worth_resuming(7140, 7200));                         // the last minute
    CHECK(!lres_worth_resuming(7200, 7200));
    CHECK(lres_worth_resuming(100, 140) == false);                   // short file: within a minute of the end
    CHECK(lres_worth_resuming(100, 0));                              // length unknown: past 30 s is enough
    CHECK(!lres_worth_resuming(10, 0));
    CHECK(lres_worth_resuming(4000000000u, 0));
}

static void keys() {
    printf("- keys\n");
    const uint64_t k = lres_key("usb0:/Films/a.mkv", 123456789, 1700000000);
    CHECK(k == lres_key("usb0:/Films/a.mkv", 123456789, 1700000000));
    CHECK(k != lres_key("usb1:/Films/a.mkv", 123456789, 1700000000));      // another port
    CHECK(k != lres_key("usb0:/Films/b.mkv", 123456789, 1700000000));
    CHECK(k != lres_key("usb0:/Films/a.mkv", 123456790, 1700000000));      // replaced file: new size
    CHECK(k != lres_key("usb0:/Films/a.mkv", 123456789, 1700000001));      // or new time
    CHECK(lres_key("a", 1, 2) != lres_key("a", 2, 1));
    // no collisions among a few thousand plausible keys
    std::set<uint64_t> seen;
    for (int i = 0; i < 3000; i++) {
        char p[64];
        snprintf(p, sizeof p, "usb%d:/Folder %d/Film %d.mkv", i % 4, i / 7, i);
        seen.insert(lres_key(p, (uint64_t)i * 1000003u, 1600000000u + (uint64_t)i));
    }
    CHECK(seen.size() == 3000);
}

static void table() {
    printf("- the table\n");
    static LResTable t;
    memset(&t, 0, sizeof t);
    uint32_t s = 0, tot = 0;
    CHECK(!lres_find(&t, 1, &s, &tot));
    lres_update(&t, 1, 600, 7200);
    CHECK(t.n == 1 && lres_find(&t, 1, &s, &tot) && s == 600 && tot == 7200);
    lres_update(&t, 2, 900, 3000);
    lres_update(&t, 1, 650, 7200);                                         // played again: now the newest
    CHECK(t.n == 2 && t.e[0].key == 2 && t.e[1].key == 1);
    CHECK(lres_find(&t, 1, &s, nullptr) && s == 650);
    lres_update(&t, 1, 7190, 7200);                                        // finished: forgotten
    CHECK(t.n == 1 && !lres_find(&t, 1, &s, &tot));
    lres_update(&t, 2, 5, 3000);                                           // stopped at the start: forgotten
    CHECK(t.n == 0);
    lres_forget(&t, 99);                                                   // not there: nothing happens
    CHECK(t.n == 0);

    // the least recently played goes at 500
    for (int i = 1; i <= LRES_MAX; i++) lres_update(&t, (uint64_t)i, 100, 7200);
    CHECK(t.n == LRES_MAX && lres_find(&t, 1, nullptr, nullptr));
    lres_update(&t, 1, 200, 7200);                                         // 1 is now the newest, 2 the oldest
    lres_update(&t, 1000, 100, 7200);
    CHECK(t.n == LRES_MAX && !lres_find(&t, 2, nullptr, nullptr) && lres_find(&t, 1, nullptr, nullptr) && lres_find(&t, 1000, nullptr, nullptr));
    CHECK(t.e[LRES_MAX - 1].key == 1000);
}

static void text() {
    printf("- the text form\n");
    static LResTable a, b;
    memset(&a, 0, sizeof a);
    lres_update(&a, 0xDEADBEEF12345678ull, 1234, 7200);
    lres_update(&a, 7, 99, 0);
    lres_update(&a, 0, 31, 100);                                           // key 0 is a key like another
    char buf[4096];
    const int n = lres_format(&a, buf, sizeof buf);
    CHECK(n > 0 && (int)strlen(buf) == n);
    CHECK(lres_parse(&b, buf) && b.n == 3);
    bool same = true;
    for (int i = 0; i < 3; i++) if (a.e[i].key != b.e[i].key || a.e[i].secs != b.e[i].secs || a.e[i].total != b.e[i].total) same = false;
    CHECK(same);
    CHECK(lres_format(&a, buf, 10) == -1);                                // does not fit
    CHECK(lres_format(&a, buf, 20) == -1);

    // damaged input: bad lines skipped, the rest kept; no header = not ours
    const char *bad = "jf-local-resume 1\n"
                      "00000000000000aa 100 7200\n"
                      "garbage\n"
                      "00000000000000bb 100\n"                            // two fields
                      "00000000000000cc 100 7200 extra\n"                 // four
                      "bb 100 7200\n"                                     // key too short
                      "\n"
                      "00000000000000dd 200 7200\n"
                      "00000000000000aa 300 7200\n";                      // a repeat: the newer line wins
    CHECK(lres_parse(&b, bad) && b.n == 2);
    uint32_t s = 0;
    CHECK(lres_find(&b, 0xaa, &s, nullptr) && s == 300 && lres_find(&b, 0xdd, &s, nullptr) && s == 200);
    CHECK(b.e[0].key == 0xdd && b.e[1].key == 0xaa);
    CHECK(!lres_parse(&b, "something else\n00000000000000aa 100 7200\n") && b.n == 0);
    CHECK(!lres_parse(&b, ""));
    CHECK(lres_parse(&b, "jf-local-resume 1\n") && b.n == 0);
    // more lines than the table holds: the newest 500 stay
    std::string big = "jf-local-resume 1\n";
    for (int i = 1; i <= 700; i++) { char l[64]; snprintf(l, sizeof l, "%016x 100 7200\n", i); big += l; }
    CHECK(lres_parse(&b, big.c_str()) && b.n == LRES_MAX && b.e[0].key == 201 && b.e[LRES_MAX - 1].key == 700);
}

static void files() {
    printf("- the file\n");
    char dir[] = "/tmp/lresXXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    std::string path = std::string(dir) + "/resume.txt";
    static LResTable a, b;
    memset(&a, 0, sizeof a);
    CHECK(!lres_load_file(&b, path.c_str()) && b.n == 0);                  // missing
    lres_update(&a, 11, 600, 7200);
    lres_update(&a, 22, 700, 7200);
    CHECK(lres_save_file(&a, path.c_str()));
    CHECK(lres_load_file(&b, path.c_str()) && b.n == 2 && b.e[1].key == 22);
    CHECK(access((path + ".tmp").c_str(), F_OK) != 0);                     // the temp file is gone
    lres_update(&a, 33, 800, 7200);
    CHECK(lres_save_file(&a, path.c_str()));                               // replaces
    CHECK(lres_load_file(&b, path.c_str()) && b.n == 3 && b.e[2].key == 33);
    // a save interrupted before the swap: only the .tmp is left, and it is read
    CHECK(rename(path.c_str(), (path + ".tmp").c_str()) == 0);
    CHECK(lres_load_file(&b, path.c_str()) && b.n == 3);
    unlink((path + ".tmp").c_str());
    // a file that is not ours
    FILE *f = fopen(path.c_str(), "wb");
    fputs("hello\n", f); fclose(f);
    CHECK(!lres_load_file(&b, path.c_str()) && b.n == 0);
    // the directory is not there: a save fails, nothing is left behind
    CHECK(!lres_save_file(&a, "/tmp/lres-no-such-dir-xyz/resume.txt"));
    // a full table fits the file
    memset(&a, 0, sizeof a);
    for (int i = 1; i <= LRES_MAX; i++) lres_update(&a, 0xFFFFFFFFFFFF0000ull + (uint64_t)i, 4200000000u, 4294967295u);
    CHECK(a.n == 0);                                                       // (a position within the last minute: not stored)
    for (int i = 1; i <= LRES_MAX; i++) lres_update(&a, 0xFFFFFFFFFFFF0000ull + (uint64_t)i, 3000000000u, 4294967295u);
    CHECK(a.n == LRES_MAX && lres_save_file(&a, path.c_str()) && lres_load_file(&b, path.c_str()) && b.n == LRES_MAX);
    unlink(path.c_str());
    rmdir(dir);
}

int main() {
    policy();
    keys();
    table();
    text();
    files();
    printf("local resume: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
