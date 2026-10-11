#pragma once
// Finding the Jellyfin server on the local network again when its address
// changes (a PC moving from Wi-Fi to Ethernet gets a new DHCP lease).
//
// Jellyfin answers a UDP broadcast of "who is JellyfinServer?" on port 7359
// with {"Address":"http://192.168.0.47:8096","Id":"...","Name":"..."}, the
// same discovery the official apps use.  A server found that way is only
// adopted when the saved login works against it: a token is valid on the
// server that issued it and nowhere else, so this cannot latch onto someone
// else's server on the same network.

#include <stdbool.h>

#define SD_MAX       8
#define SD_PORT      7359
#define SD_QUERY     "who is JellyfinServer?"

struct SdServer {
    char address[160];   // what the server says it is reached at
    char alt[160];       // the same port on the address the reply came from
    char id[64];
    char name[64];
};

// ---- pure (host-tested in tests/test_server_discovery.cpp) ----

// One reply datagram.  `from_ip` is the sender ("192.168.0.47"), or NULL.
// False for anything that is not a Jellyfin reply.
bool sd_parse_reply(const char *buf, int len, const char *from_ip, SdServer *out);

// "http://10.0.0.5:8096/jf" with host "192.168.0.47" -> "http://192.168.0.47:8096/jf".
bool sd_address_with_host(const char *address, const char *host, char *out, int cap);

// Same scheme, host and port (case-insensitive host, default ports filled in,
// a trailing slash ignored).
bool sd_same_server_url(const char *a, const char *b);

// The addresses worth trying from a set of replies, in order, without the
// one that stopped answering and without duplicates.  Returns the count.
int sd_candidates(const SdServer *servers, int n, const char *old_server,
                  char (*out)[160], int max);

// ---- PS3 ----

// Broadcast the query and collect replies for `wait_ms`.  Returns the count.
int sd_discover(SdServer *out, int max, int wait_ms);

// The saved server stopped answering: find it at a new address.  True with
// the new address in `out` when a server on the network accepts `token`.
// Only plain http servers move: an https address is left alone, because the
// discovery reply names an http one and that would quietly drop TLS.
bool sd_find_moved(const char *old_server, const char *userid, const char *token,
                   char *out, int cap);
