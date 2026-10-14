// Host test for the pure half of server discovery (source/net/server_discovery_parse.cpp).
//
// The first reply is the shape a Jellyfin 10.11 server sends to
// "who is JellyfinServer?" on UDP 7359 (captured 2026-10-11, name and id
// replaced).
//
//   make -f Makefile.host test_server_discovery && ./test_server_discovery
#include <stdio.h>
#include <string.h>

#include "server_discovery.h"

static int s_checks = 0, s_failed = 0;
#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool parse(const char *s, const char *from, SdServer *o) {
    return sd_parse_reply(s, (int)strlen(s), from, o);
}

int main() {
    SdServer sv;

    // A real reply, from the address it names.
    CHECK(parse("{\"Address\":\"http://192.168.0.47:8096\",\"Id\":\"0123456789abcdef0123456789abcdef\","
                "\"Name\":\"LIVING ROOM PC\",\"EndpointAddress\":null}", "192.168.0.47", &sv));
    CHECK(strcmp(sv.address, "http://192.168.0.47:8096") == 0);
    CHECK(strcmp(sv.id, "0123456789abcdef0123456789abcdef") == 0);
    CHECK(strcmp(sv.name, "LIVING ROOM PC") == 0);
    CHECK(sv.alt[0] == '\0');                         // same as the address: no second candidate

    // A server that advertises another adapter (a Hyper-V / WSL switch) than
    // the one that answered: the sender becomes the alternative.
    CHECK(parse("{\"Address\":\"http://172.20.16.1:8096/\",\"Id\":\"aa\",\"Name\":\"PC\"}", "192.168.0.47", &sv));
    CHECK(strcmp(sv.address, "http://172.20.16.1:8096") == 0);   // trailing slash dropped
    CHECK(strcmp(sv.alt, "http://192.168.0.47:8096") == 0);

    // A base URL survives the host swap.
    CHECK(parse("{\"Address\":\"http://10.0.0.5:8096/jellyfin\",\"Id\":\"bb\"}", "192.168.1.9", &sv));
    CHECK(strcmp(sv.alt, "http://192.168.1.9:8096/jellyfin") == 0);

    // BOM, whitespace, key order, escapes.
    CHECK(parse("\xEF\xBB\xBF {\n \"Name\" : \"A \\\"B\\\"\", \"Id\":\"cc\", \"Address\" : \"http://h:1\" }", NULL, &sv));
    CHECK(strcmp(sv.name, "A \"B\"") == 0);
    CHECK(strcmp(sv.address, "http://h:1") == 0);

    // Not Jellyfin replies.
    CHECK(!parse("who is JellyfinServer?", "1.2.3.4", &sv));            // our own broadcast echoed back
    CHECK(!parse("{\"Address\":\"http://h:1\"}", NULL, &sv));            // no Id
    CHECK(!parse("{\"Id\":\"x\"}", NULL, &sv));                          // no Address
    CHECK(!parse("{\"Address\":\"ftp://h\",\"Id\":\"x\"}", NULL, &sv)); // not http(s)
    CHECK(!parse("{\"Nested\":{\"Address\":\"http://h:1\",\"Id\":\"x\"}}", NULL, &sv));
    CHECK(!sd_parse_reply(NULL, 0, NULL, &sv));
    CHECK(!parse("", NULL, &sv));
    {   // cut short mid-string
        const char *s = "{\"Address\":\"http://192.168.0.47:8096\",\"Id\":\"ab";
        CHECK(!sd_parse_reply(s, 20, NULL, &sv));
    }

    // URL identity.
    CHECK(sd_same_server_url("http://192.168.0.194:8096", "http://192.168.0.194:8096/"));
    CHECK(sd_same_server_url("HTTP://MyPC:8096", "http://mypc:8096"));
    CHECK(sd_same_server_url("http://pc", "http://pc:80"));
    CHECK(!sd_same_server_url("http://pc:8096", "https://pc:8096"));
    CHECK(!sd_same_server_url("http://192.168.0.194:8096", "http://192.168.0.47:8096"));
    CHECK(!sd_same_server_url("http://pc:8096/a", "http://pc:8096/b"));
    CHECK(!sd_same_server_url("http://pc:99999", "http://pc:99999"));   // not a port
    CHECK(!sd_same_server_url("pc:8096", "pc:8096"));                   // no scheme

    char out[160];
    CHECK(!sd_address_with_host("http://h:1", "", out, sizeof out) && out[0] == '\0');
    CHECK(!sd_address_with_host("http://h:1", "192.168.0.1", out, 10) && out[0] == '\0');   // too small

    // Candidates: the moved server, without the dead address, the sender first,
    // https left out, duplicates folded.
    SdServer list[3];
    memset(list, 0, sizeof list);
    snprintf(list[0].address, sizeof list[0].address, "http://172.20.16.1:8096");
    snprintf(list[0].alt,     sizeof list[0].alt,     "http://192.168.0.47:8096");
    snprintf(list[1].address, sizeof list[1].address, "http://192.168.0.194:8096");   // the old one
    snprintf(list[2].address, sizeof list[2].address, "https://192.168.0.60:8920");
    snprintf(list[2].alt,     sizeof list[2].alt,     "HTTP://192.168.0.47:8096/");     // same as list[0].alt
    char cand[8][160];
    const int k = sd_candidates(list, 3, "http://192.168.0.194:8096/", cand, 8);
    CHECK(k == 2);
    CHECK(strcmp(cand[0], "http://192.168.0.47:8096") == 0);
    CHECK(strcmp(cand[1], "http://172.20.16.1:8096") == 0);
    CHECK(sd_candidates(list, 3, "http://x", cand, 1) == 1);   // capped
    CHECK(sd_candidates(list, 0, "http://x", cand, 8) == 0);

    printf("%s: %d checks, %d failed\n", s_failed ? "FAIL" : "ok", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
