// The PS3 side of Wake-on-LAN.  See wol.h for what a magic packet is.
#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <net/net.h>
#include <net/socket.h>
#include <netinet/in.h>

#include "wol.h"
#include "wol_net.h"
#include "http.h"          // http_net_lock: socket setup shares the net stack's lock
#include "plog.h"
#include "jf_paths.h"

static const char *WOL_FILE = "jellyfin_wol.txt";

bool wol_saved(uint8_t mac[6]) {
    FILE *f = fopen(jf_data_path(WOL_FILE), "r");
    if (!f) return false;
    char line[64] = "";
    const bool got = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    return got && wol_parse_mac(line, mac);
}

bool wol_save(const uint8_t mac[6]) {
    FILE *f = fopen(jf_data_path(WOL_FILE), "w");
    if (!f) return false;
    char txt[24];
    wol_format_mac(mac, txt, sizeof txt);
    fprintf(f, "%s\n", txt);
    fclose(f);
    return true;
}

bool wol_send(const uint8_t mac[6]) {
    uint8_t pkt[WOL_PACKET_LEN];
    wol_build_packet(mac, pkt);

    http_net_lock();
    const int s = netSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { http_net_unlock(); plog("wol: no UDP socket"); return false; }
    int on = 1;
    netSetSockOpt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);

    static const u16 ports[2] = { 9, 7 };
    int sent = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 2; i++) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof to);
            to.sin_family      = AF_INET;
            to.sin_port        = htons(ports[i]);
            to.sin_addr.s_addr = 0xFFFFFFFFu;
            if (netSendTo(s, pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)sizeof pkt)
                sent++;
        }
    }
    netClose(s);
    http_net_unlock();
    char txt[24], b[80];
    wol_format_mac(mac, txt, sizeof txt);
    snprintf(b, sizeof b, "wol: %d packet(s) sent for %s", sent, txt);
    plog(b);
    return sent > 0;
}

bool wol_send_saved(void) {
    uint8_t mac[6];
    return wol_saved(mac) && wol_send(mac);
}
