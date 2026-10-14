// Host test for the Wake-on-LAN packet and MAC text (source/net/wol.c).
//
//   make -f Makefile.host test_wol && ./test_wol
#include <stdio.h>
#include <string.h>

#include "wol.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool is(const char *t, const uint8_t want[6]) {
    uint8_t m[6];
    return wol_parse_mac(t, m) && !memcmp(m, want, 6);
}

int main() {
    const uint8_t mac[6] = { 0x1C, 0x69, 0x7A, 0x12, 0x34, 0xAB };

    // The ways a MAC address gets written.
    CHECK(is("1C:69:7A:12:34:AB", mac));
    CHECK(is("1c:69:7a:12:34:ab", mac));
    CHECK(is("1C-69-7A-12-34-AB", mac));
    CHECK(is("1C697A1234AB", mac));
    CHECK(is("1c69.7a12.34ab", mac));
    CHECK(is("  1C:69:7A:12:34:AB  \r\n", mac));
    CHECK(is("1C:69:7A:12:34:AB\n", mac));

    // Not a MAC address.
    const char *bad[] = {
        "", " ", "1C:69:7A:12:34", "1C:69:7A:12:34:AB:CD", "1C:69:7A:12:34:A", "1:69:7A:12:34:AB",
        "1C:69-7A:12:34:AB", "1C:69:7A:12:34:GG", "1C697A1234A", "1C697A1234ABC", "1C 69 7A 12 34 AB",
        "1C:69:7A:12:34:AB x", "1c69.7a12.34a", "1c69.7a.1234ab", "::::::", "hello",
        "00:00:00:00:00:00", "FF:FF:FF:FF:FF:FF", "000000000000", "ffffffffffff",
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint8_t m[6] = { 1, 2, 3, 4, 5, 6 };
        CHECK(!wol_parse_mac(bad[i], m));
        CHECK(m[0] == 1 && m[5] == 6);                     // a failed parse writes nothing
    }
    CHECK(!wol_parse_mac(NULL, (uint8_t *)"123456"));

    // Text round trip.
    char txt[24];
    wol_format_mac(mac, txt, sizeof txt);
    CHECK(!strcmp(txt, "1C:69:7A:12:34:AB"));
    CHECK(is(txt, mac));
    wol_format_mac(mac, txt, 8);                           // a small buffer: cut, terminated
    CHECK(strlen(txt) == 7);

    // The magic packet: six 0xFF, then the MAC sixteen times, 102 bytes.
    uint8_t pkt[WOL_PACKET_LEN];
    memset(pkt, 0, sizeof pkt);
    wol_build_packet(mac, pkt);
    CHECK(WOL_PACKET_LEN == 102);
    for (int i = 0; i < 6; i++) CHECK(pkt[i] == 0xFF);
    for (int r = 0; r < 16; r++) CHECK(!memcmp(pkt + 6 + 6 * r, mac, 6));

    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
