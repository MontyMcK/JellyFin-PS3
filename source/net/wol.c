#include "wol.h"

#include <stdio.h>
#include <string.h>

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool wol_parse_mac(const char *text, uint8_t mac[6]) {
    if (!text) return false;
    while (*text == ' ' || *text == '\t') text++;
    int digits[12], n = 0;
    char sep = 0;
    int in_group = 0;
    const char *p = text;
    for (; *p && *p != '\r' && *p != '\n'; p++) {
        const int v = hexv(*p);
        if (v >= 0) {
            if (n >= 12) return false;
            digits[n++] = v;
            in_group++;
            continue;
        }
        if (*p == ' ' || *p == '\t') {
            // Only trailing spaces.
            for (const char *q = p; *q && *q != '\r' && *q != '\n'; q++)
                if (*q != ' ' && *q != '\t') return false;
            break;
        }
        if (*p != ':' && *p != '-' && *p != '.') return false;
        if (sep && *p != sep) return false;               // one kind of separator
        sep = *p;
        // ":" and "-" separate bytes (2 digits), "." separates groups of 4.
        if (in_group != (sep == '.' ? 4 : 2)) return false;
        in_group = 0;
    }
    if (n != 12) return false;
    if (sep && in_group != (sep == '.' ? 4 : 2)) return false;
    uint8_t m[6];
    for (int i = 0; i < 6; i++) m[i] = (uint8_t)(digits[2 * i] << 4 | digits[2 * i + 1]);
    bool zeros = true, ones = true;
    for (int i = 0; i < 6; i++) { if (m[i] != 0x00) zeros = false; if (m[i] != 0xFF) ones = false; }
    if (zeros || ones) return false;
    memcpy(mac, m, 6);
    return true;
}

void wol_format_mac(const uint8_t mac[6], char *out, int cap) {
    if (!out || cap <= 0) return;
    snprintf(out, (size_t)cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void wol_build_packet(const uint8_t mac[6], uint8_t pkt[WOL_PACKET_LEN]) {
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) memcpy(pkt + 6 + 6 * i, mac, 6);
}
