#pragma once

// -------------------------------------------------------------------------
//  Parallel-socket receive test
// -------------------------------------------------------------------------
//  Answers one question that has survived every other experiment in this
//  project: is the console's ~25 Mbps receive ceiling PER SOCKET or PER
//  CONSOLE?
//
//  Everything measured so far has been single-socket -- the player's own
//  stream, and the FTP transfer that gave 9.4 Mbps PC->PS3 against 113 the
//  other way. If the limit lives in one connection's path, N connections
//  pulling different byte ranges would multiply it, and direct play of a
//  53 Mbps remux stops being impossible. If the aggregate is flat at ~25 no
//  matter how many sockets, the limit is the console and that is the end of
//  it -- which is worth knowing just as much.
//
//  Deliberately a SELF-CONTAINED DIAGNOSTIC, not a feature: it runs before
//  any playback, touches nothing the player uses, and does nothing at all
//  unless /dev_hdd0/tmp/jellyfin_nettest.txt exists.  Two lines:
//
//      <sockets> <seconds>        e.g.  4 10
//      <url>                      e.g.  http://192.168.0.194:8099/testfile.bin
//
//  The server must honour Range; each socket asks for a different 64 MB
//  offset so they are genuinely independent transfers rather than N copies
//  of the same bytes arriving from one server-side read.
//
//  Results go to the log as `nettest:` lines -- per socket and aggregate.
//
//  BOUNDED: this is called before the UI loop exists, so it can never be left
//  waiting on a socket.  Connects use a 5 s non-blocking connect + poll, reads
//  and writes carry a 5 s idle timeout, and the whole run is capped at
//  <seconds> + 15 s of wall clock.  Past that it logs a `nettest: ABANDONED`
//  line saying why, closes every socket, and returns.  A stale gate file left
//  pointing at a server that has since stopped listening therefore costs a few
//  seconds and two log lines, not a hung boot.  The one remaining unbounded
//  call is netGetHostByName, so put a literal IP in the gate file.

#ifdef __cplusplus
extern "C" {
#endif

// No-op when the config file is absent.  Safe to call once at startup, after
// http_init() has brought the network up.
void net_selftest_run(void);

#ifdef __cplusplus
}
#endif
