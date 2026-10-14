#pragma once
// Wake-on-LAN: the "magic packet" and the MAC address text (host-tested, no
// PS3 headers).  A sleeping PC whose network card is set to wake on a magic
// packet turns on when it sees 6 bytes of 0xFF followed by its own MAC address
// sixteen times, sent as a UDP broadcast (port 9 by convention).
//
// Jellyfin 10.11 no longer reports the server's MAC (its /System/WakeOnLanInfo
// is gone), so the viewer enters it once: Settings > System > Server MAC Address.

#include <stdbool.h>
#include <stdint.h>

#define WOL_PACKET_LEN 102          // 6 + 16 * 6

#ifdef __cplusplus
extern "C" {
#endif

// "1C:69:7A:12:34:56", "1c-69-7a-12-34-56", "1C697A123456" or "1c69.7a12.3456"
// (surrounding spaces ignored).  False for anything else, and for the two
// addresses that cannot be a machine's: all zeros and all ones.
bool wol_parse_mac(const char *text, uint8_t mac[6]);
// "1C:69:7A:12:34:56" (upper case).  `out` needs 18 bytes.
void wol_format_mac(const uint8_t mac[6], char *out, int cap);
// The magic packet for `mac`.
void wol_build_packet(const uint8_t mac[6], uint8_t pkt[WOL_PACKET_LEN]);

#ifdef __cplusplus
}
#endif
