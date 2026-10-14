// Host test for source/local/sub_conv.c: Matroska subtitle blocks as text lines and as .sup segments.
//
//   make -f Makefile.host test_sub_conv && ./test_sub_conv

#include "sub_conv.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string srt(const std::string &in, int cap = 200) {
    char out[512];
    const int n = sub_srt_block_text((const uint8_t *)in.data(), (int)in.size(), out, cap < 512 ? cap : 512);
    CHECK(n == (int)strlen(out));
    return out;
}
static std::string ass(const std::string &in, int cap = 200) {
    char out[512];
    const int n = sub_ass_block_text((const uint8_t *)in.data(), (int)in.size(), out, cap < 512 ? cap : 512);
    CHECK(n == (int)strlen(out));
    return out;
}

static void text_blocks() {
    printf("- SRT blocks\n");
    CHECK(srt("Hello") == "Hello");
    CHECK(srt("Hello\nworld") == "Hello\nworld");
    CHECK(srt("Hello\r\nworld\r\n") == "Hello\nworld");
    CHECK(srt("<i>Hello</i> <b>there</b>") == "Hello there");
    CHECK(srt("{\\an8}Top line") == "Top line");
    CHECK(srt("<font color=\"#ff0000\">red</font>") == "red");
    CHECK(srt("a < b and c > d") == "a < b and c > d");               // not tags
    CHECK(srt("a <b and no close") == "a <b and no close");
    CHECK(srt("one\n\n\ntwo") == "one\ntwo");                         // blank lines collapse
    CHECK(srt("\n\nstart") == "start");
    CHECK(srt("   \n") == "");                                        // nothing to show
    CHECK(srt("") == "");
    CHECK(srt("<i></i>") == "");
    CHECK(srt(std::string("with\0nul", 8)) == "withnul");
    CHECK(srt("caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC") == "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC");
    // cut at the room given, between characters
    const std::string long_text = srt(std::string(300, 'x'), 200);
    CHECK(long_text.size() == 199);
    CHECK(srt("ab\xC3\xA9", 4) == "ab");                              // the 2-byte character does not fit in 3 bytes
    CHECK(srt("ab\xC3\xA9", 5) == "ab\xC3\xA9");
    char tiny[1];
    CHECK(sub_srt_block_text((const uint8_t *)"abc", 3, tiny, 1) == 0 && tiny[0] == '\0');
    CHECK(sub_srt_block_text((const uint8_t *)"abc", 3, tiny, 0) == 0);

    printf("- ASS blocks\n");
    CHECK(ass("0,0,Default,,0,0,0,,Hello there") == "Hello there");
    CHECK(ass("12,1,Default,Speaker,10,10,30,fx,Hello, there, friends") == "Hello, there, friends");     // commas in the text stay
    CHECK(ass("0,0,Default,,0,0,0,,Line one\\NLine two") == "Line one\nLine two");
    CHECK(ass("0,0,Default,,0,0,0,,Line one\\nLine two") == "Line one\nLine two");
    CHECK(ass("0,0,Default,,0,0,0,,no\\hbreak") == "no break");
    CHECK(ass("0,0,Default,,0,0,0,,{\\i1}italic{\\i0} and {\\pos(10,20)\\c&H00FF00&}green") == "italic and green");
    CHECK(ass("0,0,Default,,0,0,0,,{\\p1}m 0 0 l 10 0 10 10 0 10{\\p0}visible") == "visible");           // vector drawing
    CHECK(ass("0,0,Default,,0,0,0,,{\\p4}m 0 0 l 1 1") == "");
    CHECK(ass("0,0,Default,,0,0,0,,a{\\p0}b") == "ab");
    CHECK(ass("0,0,Default,,0,0,0,,{\\p1}x{\\p0} y {\\p2}z{\\p0} w") == "y  w");
    CHECK(ass("0,0,Default,,0,0,0,,\\N\\NStart") == "Start");
    CHECK(ass("0,0,Default,,0,0,0,,End\\N\\N") == "End");
    CHECK(ass("0,0,Default,,0,0,0,,") == "");
    CHECK(ass("0,0,Default,,0,0,0") == "");                            // fewer than eight commas: not an event
    CHECK(ass("just text") == "");
    CHECK(ass("") == "");
    CHECK(ass("0,0,Default,,0,0,0,,{unclosed text") == "{unclosed text");
    CHECK(ass("0,0,Default,,0,0,0,,back\\slash") == "back\\slash");    // an unknown escape stays as written
    CHECK(ass("0,0,Default,,0,0,0,,caf\xC3\xA9", 200) == "caf\xC3\xA9");
    CHECK(ass("0,0,Default,,0,0,0,,ab\xC3\xA9", 4) == "ab");
    CHECK(ass(std::string("0,0,Default,,0,0,0,,") + std::string(400, 'y'), 200).size() == 199);
}

static std::vector<uint8_t> seg(uint8_t type, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> s = { type, (uint8_t)(payload.size() >> 8), (uint8_t)payload.size() };
    s.insert(s.end(), payload.begin(), payload.end());
    return s;
}

static void pgs_blocks() {
    printf("- PGS blocks\n");
    std::vector<uint8_t> block;
    const std::vector<uint8_t> pcs(19, 0x11), wds(10, 0x22), pds(7, 0x33), ods(300, 0x44), end;
    for (auto &s : { seg(0x16, pcs), seg(0x17, wds), seg(0x14, pds), seg(0x15, ods), seg(0x80, end) }) block.insert(block.end(), s.begin(), s.end());
    std::vector<uint8_t> out(2000);
    const int n = sub_pgs_block_to_sup(block.data(), (int)block.size(), 0x01234567, out.data(), (int)out.size());
    CHECK(n == (int)(block.size() + 5 * 10));                          // each segment gains the 10-byte "PG" header, PTS and DTS
    // walk the result as a .sup stream
    int pos = 0, count = 0;
    const uint8_t types[5] = { 0x16, 0x17, 0x14, 0x15, 0x80 };
    const size_t sizes[5] = { 19, 10, 7, 300, 0 };
    bool ok = true;
    while (pos + 13 <= n && count < 5) {
        const uint8_t *p = &out[(size_t)pos];
        const size_t size = (size_t)((p[11] << 8) | p[12]);
        if (p[0] != 'P' || p[1] != 'G' || p[2] != 0x01 || p[3] != 0x23 || p[4] != 0x45 || p[5] != 0x67 || p[6] || p[7] || p[8] || p[9] ||
            p[10] != types[count] || size != sizes[count]) ok = false;
        for (size_t k = 0; k < size; k++) if (p[13 + k] != (count == 0 ? 0x11 : count == 1 ? 0x22 : count == 2 ? 0x33 : 0x44)) ok = false;
        pos += 13 + (int)size;
        count++;
    }
    CHECK(ok && count == 5 && pos == n);
    // too small an output: nothing half-written is reported as a success
    CHECK(sub_pgs_block_to_sup(block.data(), (int)block.size(), 1, out.data(), 100) == -1);
    CHECK(sub_pgs_block_to_sup(block.data(), (int)block.size(), 1, out.data(), n - 1) == -1);
    CHECK(sub_pgs_block_to_sup(block.data(), (int)block.size(), 1, out.data(), n) == n);
    // a block cut short in its last segment keeps the whole segments before it
    CHECK(sub_pgs_block_to_sup(block.data(), (int)block.size() - 100, 1, out.data(), (int)out.size()) == 13 + 19 + 13 + 10 + 13 + 7);
    CHECK(sub_pgs_block_to_sup(block.data(), 2, 1, out.data(), (int)out.size()) == 0);
    CHECK(sub_pgs_block_to_sup(block.data(), 0, 1, out.data(), (int)out.size()) == 0);
    // a lying length stops the walk
    std::vector<uint8_t> lie = { 0x16, 0xFF, 0xFF, 1, 2, 3 };
    CHECK(sub_pgs_block_to_sup(lie.data(), (int)lie.size(), 1, out.data(), (int)out.size()) == 0);
}

int main() {
    text_blocks();
    pgs_blocks();
    printf("sub conv: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
