// Host test for the chapter parser and menu labels (source/api/chapters.cpp).
//
// The JSON follows the shape of a Jellyfin 10.11 item asked for Fields=Chapters.
// Nothing on the development server carries chapters (its films are debrid
// streams), so the shape is the documented one, written by hand.
//
//   make -f Makefile.host test_chapters && ./test_chapters
#include <stdio.h>
#include <string.h>

#include "chapters.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int parse(const char *j, JFChapters *c) { return jf_parse_chapters(j, (int)strlen(j), c); }

int main() {
    JFChapters c;

    // A normal item: other fields around it, nested objects before and after.
    const char *item =
        "{\"Name\":\"A Film\",\"Id\":\"abc\",\"People\":[{\"Name\":\"Someone\",\"Id\":\"p1\"}],"
        "\"Chapters\":["
        "{\"StartPositionTicks\":0,\"Name\":\"Opening\",\"ImageDateModified\":\"2026-01-01T00:00:00Z\",\"ImageTag\":\"t1\"},"
        "{\"StartPositionTicks\":3000000000,\"Name\":\"The \\\"Heist\\\" {part 1}\"},"
        "{\"StartPositionTicks\":36610000000,\"Name\":\"\"},"
        "{\"StartPositionTicks\":72000000000}"
        "],\"MediaType\":\"Video\"}";
    CHECK(parse(item, &c) == 4);
    CHECK(c.ch[0].start_secs == 0.0 && !strcmp(c.ch[0].name, "Opening"));
    CHECK(c.ch[1].start_secs == 300.0 && !strcmp(c.ch[1].name, "The \"Heist\" {part 1}"));
    CHECK(c.ch[2].start_secs > 3660.9 && c.ch[2].start_secs < 3661.1 && c.ch[2].name[0] == '\0');
    CHECK(c.ch[3].start_secs == 7200.0 && c.ch[3].name[0] == '\0');

    // Where are we?
    CHECK(jf_chapter_index_at(&c, 0.0) == 0);
    CHECK(jf_chapter_index_at(&c, 299.0) == 0);
    CHECK(jf_chapter_index_at(&c, 299.6) == 1);        // half a second early counts as arrived
    CHECK(jf_chapter_index_at(&c, 300.0) == 1);
    CHECK(jf_chapter_index_at(&c, 3662.0) == 2);
    CHECK(jf_chapter_index_at(&c, 99999.0) == 3);
    JFChapters late; memset(&late, 0, sizeof late);
    late.n = 1; late.ch[0].start_secs = 30.0;
    CHECK(jf_chapter_index_at(&late, 10.0) == -1);     // before the first
    CHECK(jf_chapter_index_at(NULL, 10.0) == -1);

    // Labels.
    char lab[96];
    jf_chapter_label(&c.ch[0], 0, "Chapter %d", lab, sizeof lab);
    CHECK(!strcmp(lab, "1  Opening  0:00"));
    jf_chapter_label(&c.ch[1], 1, "Chapter %d", lab, sizeof lab);
    CHECK(!strcmp(lab, "2  The \"Heist\" {part 1}  5:00"));
    jf_chapter_label(&c.ch[2], 2, "Chapter %d", lab, sizeof lab);
    CHECK(!strcmp(lab, "3  Chapter 3  1:01:01"));      // no name: the fallback; over an hour: h:mm:ss
    jf_chapter_label(&c.ch[3], 3, "Kapitel %d", lab, sizeof lab);
    CHECK(!strcmp(lab, "4  Kapitel 4  2:00:00"));
    // A long name is cut, and never in the middle of a character.
    JFChapter lng; memset(&lng, 0, sizeof lng);
    snprintf(lng.name, sizeof lng.name, "%s", "0123456789012345678901234567890123456789");
    jf_chapter_label(&lng, 0, "Chapter %d", lab, sizeof lab);
    CHECK(!strcmp(lab, "1  012345678901234567890123456789...  0:00"));
    snprintf(lng.name, sizeof lng.name, "%s",
             "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86\xE3\x81\x88\xE3\x81\x8A\xE3\x81\x8B\xE3\x81\x8D"
             "\xE3\x81\x8F\xE3\x81\x91\xE3\x81\x93\xE3\x81\x95\xE3\x81\x97\xE3\x81\x99");   // 13 hiragana, 39 bytes
    jf_chapter_label(&lng, 0, "Chapter %d", lab, sizeof lab);
    CHECK(strstr(lab, "...") != NULL);
    for (const char *p = lab; *p; p++) {               // valid UTF-8 throughout
        const unsigned char u = (unsigned char)*p;
        if (u >= 0xE0 && u < 0xF0) { CHECK((p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80); p += 2; }
    }
    jf_chapter_label(&c.ch[0], 0, "Chapter %d", lab, 8);      // a tiny buffer: cut, terminated
    CHECK(strlen(lab) <= 7);

    // Not in order, repeated, negative: dropped; ordering kept.
    CHECK(parse("{\"Chapters\":[{\"StartPositionTicks\":100000000,\"Name\":\"b\"},"
                "{\"StartPositionTicks\":0,\"Name\":\"back\"},{\"StartPositionTicks\":100000000,\"Name\":\"same\"},"
                "{\"StartPositionTicks\":-5,\"Name\":\"neg\"},{\"StartPositionTicks\":200000000,\"Name\":\"c\"}]}", &c) == 2);
    CHECK(!strcmp(c.ch[0].name, "b") && !strcmp(c.ch[1].name, "c"));

    // Nothing to find.
    CHECK(parse("{\"Name\":\"x\"}", &c) == 0 && c.n == 0);
    CHECK(parse("{\"Chapters\":[]}", &c) == 0);
    CHECK(parse("{\"Chapters\":null}", &c) == 0);
    CHECK(parse("{\"Nested\":{\"Chapters\":[{\"StartPositionTicks\":5}]}}", &c) == 0);   // not the item's own
    CHECK(parse("[]", &c) == 0 && parse("", &c) == 0);
    CHECK(jf_parse_chapters(NULL, 0, &c) == 0);
    // BOM, and a reply cut off mid-object keeps the whole chapters before it.
    CHECK(parse("\xEF\xBB\xBF{\"Chapters\":[{\"StartPositionTicks\":0,\"Name\":\"a\"}]}", &c) == 1);
    CHECK(parse("{\"Chapters\":[{\"StartPositionTicks\":0,\"Name\":\"a\"},{\"StartPositionTicks\":9000000,\"Na", &c) == 1);

    // More than fit: the first JF_MAX_CHAPTERS.
    static char big[16384];
    strcpy(big, "{\"Chapters\":[");
    for (int i = 0; i < 80; i++) {
        char one[80];
        snprintf(one, sizeof one, "%s{\"StartPositionTicks\":%lld,\"Name\":\"c%d\"}", i ? "," : "", (long long)i * 600000000LL, i);
        strcat(big, one);
    }
    strcat(big, "]}");
    CHECK(parse(big, &c) == JF_MAX_CHAPTERS && !strcmp(c.ch[JF_MAX_CHAPTERS - 1].name, "c47"));

    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
