# Network notes

Measurements behind the streaming socket settings in
`source/player/stream/stream.cpp`.  Knobs, all in KB, absent = shipped default:
`/dev_hdd0/tmp/jellyfin_rcvbuf.txt`, `jellyfin_netbuf.txt`, `jellyfin_netpool.txt`.

## SO_RCVBUF

2026-09-18, server PC reaching the console over **Wi-Fi**, same film, 1080p 25:

| SO_RCVBUF | net median | frames on time | heartbeats with ring empty |
|---|---|---|---|
| 128 KB | 24.7 Mbps | 81% | 15% |
| 512 KB | 31.1 Mbps | 97% | 0% |

2026-10-12, the same server PC now on **gigabit Ethernet**, 512 KB, a 24 Mbps
transcode (h264_amf 21.25 Mbps + DTS-HD MA copied; the server encoded at 4x the
whole time).  The heartbeat shows bursts of ~9,000 TS packets (~1.7 MB) every
6-9 s with `net=0.0M rxw=83%` in between: preroll took 65 s to fill 14.5 MB
(~1.8 Mbps), then the ring emptied within 10 s of playback.  A regular burst and
silence, with the reader blocked in netRecv the whole time, is a sender in
retransmission backoff, not a slow server.

libnet's pool is 128 KB for every socket (`LIBNET_MEMORY_SIZE`).  A 512 KB
window lets a sender that is not paced by its own link put more in flight than
the pool can hold, so one loss can leave no room for the retransmission.

Neither value is right for every home: which one wins depends on how the server
reaches the network, and the console cannot see that.  So each run of the app
starts at 512 KB; `rx_gap.h` watches every wait on the socket, and the first time
a connection that has already delivered 1 MB goes quiet for 3 s while the reader
is waiting through all of it, later connections in that run ask for 128 KB
(Movian's value on this console) and the player does a 0-delta reopen at the
same position.  The next run tries 512 KB again.  `jellyfin_rcvbuf.txt` fixes
the window and turns this off.  Built 2026-10-12, not yet measured on hardware:
look for `stream: ... the sender is backing off` in the log.

The read-ahead ring is sized from free memory (three quarters of it): 24.5 MB on
2026-09-18, 15.7 MB on 2026-10-12.
