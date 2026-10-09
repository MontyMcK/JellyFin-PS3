#include "stream.h"
#include "i18n.h"
#include "plog.h"
#include "jellyfin_api.h"
#include "http.h"
#include "timing.h"
#include "jf_paths.h"
#include "lclog.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ppu-types.h>
#include <net/net.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/poll.h>
#include <sysutil/sysutil.h>
#include "lfs.h"        // local files: the internal disk and USB drives
#include "local_probe.h" // local_m2ts_compact
#include "mkv_ts.h"      // a Matroska file as MPEG-TS

extern u32 running;

volatile bool g_stream_cancel = false;
static stream_wait_fn s_wait_cb = NULL;
void stream_set_wait_cb(stream_wait_fn cb) { s_wait_cb = cb; }

// How long to wait for the server's response headers.  A burn-in request
// (SubtitleMethod=Encode) makes Jellyfin extract the subtitle track from the
// source file before the transcode produces any output, and headers are only
// sent once it does — on a big MKV that can take a minute or more.
#define STREAM_HDR_DEADLINE_US  120000000ULL

static bool s_chunked      = false;
static int  s_chunk_remain = -1;
static char s_chdr[32];
static int  s_chdr_n      = 0;
static int  s_ctrail       = 0;

// Partial-read carry: bytes already consumed from the TCP stream when a
// receive timeout interrupts a packet read.  Without this, returning 0
// mid-packet DISCARDS those bytes — the TS stream desyncs and the decoder
// conceals the resulting garbage with stale macroblocks until the next IDR.
static u8   s_carry[188];   // one TS packet — the only read size callers use
static int  s_carry_n = 0;

// ---------------------------------------------------------------------------
// Socket read buffer.
//
// stream_read() is called once per 188-byte TS packet, and before this it did
// a netRecv() PER CALL -- plus, on a chunked response, one netRecv PER BYTE of
// every chunk header and its CRLF trailer.  At 30 Mbps that is ~20 000 packet
// reads a second before counting chunk framing, and each netRecv is an lv2
// syscall through the network PRX.  The PPU was spending its time in syscall
// entry rather than moving bytes: measured ceiling was roughly 10-19 Mbps,
// while the same server hands this LAN 599 Mbps to a PC.  That is why the ring
// drained at 20/30 Mbps and at Original, and why raising the bitrate cap made
// it worse rather than better.
//
// Reading 64 KB at a time and serving packets and chunk headers out of memory
// cuts the syscall count by ~350x for the same bytes.  It changes nothing about
// the TCP stream itself -- this is a buffer over the same sequential bytes, so
// chunk framing still parses exactly as it did.
//
// Deliberately NOT used for the response headers: stream_open() reads those a
// byte at a time precisely so it cannot over-read into the body, and it happens
// once per open, so it costs nothing worth reclaiming.  The buffer therefore
// starts empty at the first stream_read() and owns every byte after the header.
// 256 KB, was 64 KB, and how much of it one netRecv asks for is a RUNTIME
// knob -- jellyfin_netbuf.txt, in KB.
//
// Movian is the reference here: a mature PS3 player whose buffered-file layer
// uses a 256 KB minimum request for big/streaming content
// (bf_min_request, src/fileaccess/fa_buffer.c). Ours asked for 64 KB. That is
// not a syscall-count argument -- 64 KB reads at 25 Mbps is only ~48 calls a
// second and syscall overhead was measured and ruled out long ago -- it is
// about giving the stack a deep enough request to hand back a large burst in
// one go instead of returning whatever is in the socket right now.
//
// Runtime rather than compiled in because every network theory on this
// project has had to be A/B'd on hardware, and doing that over FTP costs
// seconds where a rebuild and reinstall costs twenty minutes.
#define SB_SIZE (256 * 1024)
static u8   s_sb[SB_SIZE] __attribute__((aligned(128)));   // 32-byte aligned: raw drive reads land in it directly
static int  s_sb_n = 0;      // valid bytes in s_sb
static int  s_sb_p = 0;      // read cursor
static int  s_sb_req = 0;    // bytes to ask netRecv for; 0 until resolved

// Receive telemetry.  Two hypotheses about the 20/30 Mbps ceiling (syscall
// count, then the TCP window) both turned out to be wrong, and both were
// guesses made without a number for what the client actually pulls.  These
// counters answer it directly: bytes in, and how much of the wall clock was
// spent blocked inside netRecv.  High Mbps with low wait = fine.  Low Mbps
// with HIGH wait = we are waiting on the network.  Low Mbps with LOW wait =
// nobody is asking for data and the bottleneck is elsewhere in the player.
static volatile u64 s_rx_bytes   = 0;
static volatile u64 s_rx_wait_us = 0;
static volatile u32 s_rx_calls   = 0;

void stream_rx_stats(u64 *bytes, u64 *wait_us, u32 *calls) {
    if (bytes)   *bytes   = s_rx_bytes;
    if (wait_us) *wait_us = s_rx_wait_us;
    if (calls)   *calls   = s_rx_calls;
}

static void sb_reset(void) { s_sb_n = 0; s_sb_p = 0; }

// Look at the socket without consuming anything: MSG_PEEK leaves every byte
// for stream_read.  Only call this while no other thread is reading `sock`.
void stream_probe(int sock, char *out, int outsz) {
    if (stream_is_file(sock)) {          // a file handle is not a socket: no peek
        snprintf(out, outsz, "file buffered=%d rx_total=%llu",
                 s_sb_n - s_sb_p, (unsigned long long)s_rx_bytes);
        return;
    }
    static u8 peek[64 * 1024];
    const int n = netRecv(sock, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT);
    const int e = n < 0 ? net_errno : 0;
    const char *st =
        n > 0 ? "data_waiting"
      : n == 0 ? "PEER_CLOSED"
      : (e == NET_EAGAIN || e == NET_EWOULDBLOCK || e == NET_ETIMEDOUT) ? "idle_open"
      : "ERROR";
    snprintf(out, outsz,
             "sock=%d %s peek=%d net_errno=%d buffered=%d carry=%d chunked=%d "
             "chunk_left=%d rx_total=%llu",
             sock, st, n, e, s_sb_n - s_sb_p, s_carry_n, (int)s_chunked,
             s_chunk_remain, (unsigned long long)s_rx_bytes);
}

// Read the two network knobs once per connection.  Both default to what
// Movian uses on this console.
static int netcfg_kb(const char *name, int def_kb, int max_kb) {
    FILE *f = fopen(jf_data_path(name), "r");
    if (!f) return def_kb;
    int v = 0;
    if (fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    if (v <= 0)      return def_kb;
    if (v > max_kb)  return max_kb;
    return v;
}

// Refill when empty.  Returns bytes available (>0), 0 if the peer closed, or
// -1 on receive timeout -- the same three outcomes netRecv gave the old code,
// so the carry/resume logic above is unchanged.
// The open local file (offline playback, a USB drive), or -1 when streaming a socket.
// One stream at a time is all this module has ever supported (every piece
// of its state is static); a file is simply the other kind of source.
enum { FILE_TS, FILE_M2TS, FILE_MKV };
static int      s_file_h   = -1;       // an lfs handle (a TS or an m2ts); -1 for a Matroska source and for a socket
static int      s_file_kind = FILE_TS;
static uint64_t s_file_pos = 0;        // where the next read of it starts (TS offsets, also for an m2ts)
static int      s_file_err = 0;        // the lfs error that ended the last read, 0 = a clean end

// A Matroska source: the parsed file is kept for the session (a seek or an audio change reopens
// the TS view at another key frame, it does not parse the file again), the TS view per open.
static MkvFile s_mkv;
static bool    s_mkv_open = false;
static int     s_mkv_h    = -1;
static char    s_mkv_path[256];
static MkvTs  *s_mkv_ts   = NULL;

static bool file_active(void) { return s_file_h >= 0 || s_mkv_ts != NULL; }

// Takes the lfs read of the Matroska parser; remembers why a read failed so a pulled drive is told apart.
static int mkv_lfs_read(void *ctx, uint64_t off, uint8_t *buf, int len) {
    const int got = lfs_read(*(const int *)ctx, off, buf, (uint32_t)len);
    if (got < 0) { s_file_err = got; return -1; }
    return got;
}

// What the open source holds, closed: the stream of a file, not the parsed Matroska file.
static void file_close_stream(void) {
    if (s_mkv_ts) { mkv_ts_close(s_mkv_ts); s_mkv_ts = NULL; }
    if (s_file_h >= 0) { lfs_close(s_file_h); s_file_h = -1; }
}

static int sb_fill(int sock) {
    if (s_sb_p < s_sb_n) return s_sb_n - s_sb_p;
    s_sb_p = s_sb_n = 0;
    if (file_active()) {
        // Local file: the same buffer, filled from the disk.  A file never
        // times out; its end is the stream's end (0 = "closed"), and so is a
        // read error, which stream_file_removed() then tells apart.
        int got;
        if (s_file_kind == FILE_MKV) {
            got = mkv_ts_read(s_mkv_ts, s_sb, SB_SIZE);
            if (got < 0 && !s_file_err) s_file_err = LFS_E_IO;
            if (got <= 0) return 0;
            s_rx_bytes += (u64)got;
            s_sb_n = got;
            return s_sb_n;
        }
        if (s_file_kind == FILE_M2TS) {
            // whole 192-byte packets, stripped in place to 188; positions count in the stripped stream
            got = lfs_read(s_file_h, s_file_pos / 188u * 192u, s_sb, SB_SIZE / 192 * 192);
            if (got <= 0) { s_file_err = got < 0 ? got : 0; return 0; }
            got = local_m2ts_compact(s_sb, got);
        } else {
            got = lfs_read(s_file_h, s_file_pos, s_sb, SB_SIZE);
            if (got <= 0) { s_file_err = got < 0 ? got : 0; return 0; }
        }
        if (got == 0) return 0;                      // a partial packet at the end of the file
        s_file_pos += (uint64_t)got;
        s_rx_bytes += (u64)got;
        s_sb_n = got;
        return s_sb_n;
    }
    u64 t0 = timing_get_us();
    int n = netRecv(sock, s_sb, s_sb_req ? s_sb_req : SB_SIZE, 0);
    u64 dt = timing_get_us() - t0;
    s_rx_wait_us += dt;
    s_rx_calls++;
    if (n < 0) {
        // Every n < 0 is treated as a receive timeout and retried, so a real
        // socket error (reset, not-connected) would otherwise look exactly
        // like a stalled server.  Say which it was -- rate-limited, since a
        // genuine timeout fires every 5 ms while the server is quiet.
        // netRecv is the sys_net export: its errno is net_errno (BSD codes),
        // not newlib's errno.
        const int e = net_errno;
        if (e != NET_EAGAIN && e != NET_EWOULDBLOCK && e != NET_ETIMEDOUT) {
            static int s_err_log = 0;
            if (s_err_log < 20) {
                s_err_log++;
                lc_logf("stream: netRecv ERROR rc=%d (0x%08x) net_errno=%d (not a timeout)",
                        n, (unsigned)n, e);
            }
        }
    }
    if (n <= 0) return n;
    s_rx_bytes += (u64)n;
    if (dt > 50000) {
        char lb[64];
        snprintf(lb, sizeof(lb), "net_stall: %llums bytes=%d",
                 (unsigned long long)(dt / 1000ULL), n);
        plog(lb);
    }
    s_sb_n = n;
    return n;
}

// One byte, for chunk-header and trailer parsing.  1 = got it, 0 = closed,
// -1 = timeout.
static int sb_getc(int sock, u8 *c) {
    int a = sb_fill(sock);
    if (a <= 0) return a;
    *c = s_sb[s_sb_p++];
    return 1;
}

// Up to `want` bytes.  Returns the count copied (>0), 0 closed, -1 timeout.
static int sb_read(int sock, u8 *dst, int want) {
    int a = sb_fill(sock);
    if (a <= 0) return a;
    int n = a < want ? a : want;
    memcpy(dst, s_sb + s_sb_p, n);
    s_sb_p += n;
    return n;
}

// Why the last stream_open() failed, for the error screen: "Stream connection
// failed" alone cannot tell a refused connection from a server that answered
// 400 because the MediaSourceId was wrong, and those need different fixes.
static char s_last_error[64] = "";      // English: logged, and read by the logic that looks for "Cancelled"
static char s_last_error_ui[128] = "";  // the same message in the interface language, for the error screen

static void clear_err(void) { s_last_error[0] = '\0'; s_last_error_ui[0] = '\0'; }

// The English into s_last_error and, formatted from the translated format string, into s_last_error_ui.
// Each call wraps its format in the TRN marker, which is how tools/i18n.py finds it.
static void set_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_err(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    vsnprintf(s_last_error, sizeof s_last_error, fmt, ap);
    vsnprintf(s_last_error_ui, sizeof s_last_error_ui, tr(fmt), ap2);
    va_end(ap2);
    va_end(ap);
}

static unsigned s_hdr_deadline_secs = 0;
void stream_set_header_deadline(unsigned secs) { s_hdr_deadline_secs = secs; }

int stream_open(const char *url) {
    const u64 hdr_deadline_us = s_hdr_deadline_secs
        ? (u64)s_hdr_deadline_secs * 1000000ULL : STREAM_HDR_DEADLINE_US;
    s_hdr_deadline_secs = 0;
    file_close_stream();
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        // The API works over TLS; the video socket path does not (see
        // net/https_client.h).  Say so instead of connecting in plaintext.
        plog("stream: https:// servers cannot be streamed -- use http:// for playback");
        return -1;
    }
    if (strncmp(p, "http://", 7) == 0) p += 7;

    char host[256]; int port = 8096; char path[512];
    const char *h = p;
    while (*p && *p != ':' && *p != '/') p++;
    int hl = p - h; if (hl >= 255) hl = 255;
    memcpy(host, h, hl); host[hl] = '\0';
    if (*p == ':') { p++; port = atoi(p); while (*p && *p != '/') p++; }
    strncpy(path, *p ? p : "/", sizeof(path)-1); path[sizeof(path)-1] = '\0';

    int sock = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    // TCP receive buffer.  This was never set, so the socket ran on the lv2
    // default -- and that default is what actually capped playback, not the
    // PPU and not the server.  Evidence: with a 64 KB application read, the
    // log showed `net_stall: 111ms bytes=2896`, i.e. netRecv came back after
    // 111 ms holding TWO 1448-byte segments, while the same Jellyfin transcode
    // endpoint hands a PC on this LAN 537 Mbps sustained.  A receive buffer
    // that small keeps the advertised window tiny, so the server may only have
    // a couple of segments in flight and throughput collapses to a fraction of
    // the link -- exactly the "ring drains to zero" symptom at 20/30 Mbps and
    // at Original, where the stream needs 30-53 Mbps to keep up.
    //
    // It MUST be set before netConnect(): the window scale factor is
    // negotiated in the SYN, so raising the buffer afterwards cannot widen the
    // window beyond 64 KB.
    {
        // 512 KB.  It was briefly cut to 128 to match libnet's pool, on the
        // reasoning that asking for four times the whole shared pool was
        // incoherent and that Movian asks for exactly 128 here on this same
        // console (net_psl1ght.c) while its POSIX backend asks for 192.
        // Sound reasoning. Wrong answer -- measured back to back at 1080p 25,
        // same film:
        //
        //             net median   frames on time   heartbeats with ring empty
        //   128 KB      24.7 Mbps        81%                  15%
        //   512 KB      31.1 Mbps        97%                   0%
        //
        // So the request is not clamped to the pool in any way that helps,
        // and a larger one measurably wins. Borrowing a constant from another
        // project is reasoning, not measurement; this is the measurement.
        //
        // getsockopt reports the request rather than what is funded, so this
        // cannot be settled by reading it back -- hence the knob
        // (jellyfin_rcvbuf.txt, in KB) rather than another guess.
        int rb = netcfg_kb("jellyfin_rcvbuf.txt", 512, 2048) * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
        s_sb_req = netcfg_kb("jellyfin_netbuf.txt", SB_SIZE / 1024,
                             SB_SIZE / 1024) * 1024;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u16)port);
    unsigned na=0,nb=0,nc=0,nd=0;
    sscanf(host, "%u.%u.%u.%u", &na, &nb, &nc, &nd);
    addr.sin_addr.s_addr = htonl((na<<24)|(nb<<16)|(nc<<8)|nd);

    if (netConnect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        set_err(TRN("Could not connect to %s:%d"), host, port);
        netClose(sock); return -1;
    }

    char auth[400];
    if (g_token[0])
        snprintf(auth, sizeof(auth),
            "MediaBrowser Client=\"PS3\", Device=\"PS3\","
            " DeviceId=\"%s\", Version=\"0.1\", Token=\"%s\"",
            jf_device_id(), g_token);
    else
        snprintf(auth, sizeof(auth),
            "MediaBrowser Client=\"PS3\", Device=\"PS3\","
            " DeviceId=\"%s\", Version=\"0.1\"", jf_device_id());

    char req[2048];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: %s\r\n"
        "Accept: video/mp2t\r\n"
        "User-Agent: " HTTP_USER_AGENT "\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port, auth);
    netSend(sock, req, rlen, 0);

    // 500 ms receive timeout — lets the header wait below poll instead of
    // blocking forever, and remains in effect for the stream (caller can
    // tighten it after connecting).
    // The REAL struct timeval (2026-09-27): a hand-rolled {u32, u32} is 8
    // bytes where the lv2 call expects 16, so the timeout never took --
    // reads blocked for seconds on a quiet server (the stalls, the judder,
    // the laggy seeks).  net/http.cpp always used this form.
    { struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 500000;
      netSetSockOpt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }

    char hdr[4096]; int htotal = 0;
    u64 hdr_t0     = timing_get_us();
    u64 hdr_log_us = hdr_t0;
    while (htotal < (int)sizeof(hdr)-1) {
        // Poll first (2026-09-27): a receive that blocked with no timeout
        // hung a seek's reopen for good -- the "crash" after subtitles.
        int n;
        {
            struct pollfd pfd; pfd.fd = sock; pfd.events = POLLIN; pfd.revents = 0;
            n = netPoll(&pfd, 1, 50) == 0 ? -1 : netRecv(sock, hdr + htotal, 1, MSG_DONTWAIT);
        }
        if (n == 1) {
            htotal++;
            if (htotal >= 4 && memcmp(hdr + htotal - 4, "\r\n\r\n", 4) == 0) break;
            continue;
        }
        if (n == 0) {
            plog("stream_open: closed before headers");
            set_err(TRN("Server closed the connection"));
            netClose(sock); return -1;
        }
        // n < 0: receive timeout — the server hasn't started responding yet.
        // Keep the system callback pumped so quit still works, and give up
        // at the deadline instead of freezing the player.
        sysUtilCheckCallback();
        if (!running || g_stream_cancel) { netClose(sock); return -1; }
        u64 now = timing_get_us();
        // Let the caller keep the screen alive and offer a way out.
        if (s_wait_cb && !s_wait_cb((unsigned)((now - hdr_t0) / 1000ULL))) {
            plog("stream_open: cancelled by user");
            set_err(TRN("Cancelled while waiting for the server"));
            netClose(sock); return -1;
        }
        if (now - hdr_t0 >= hdr_deadline_us) {
            plog("stream_open: header timeout");
            // Two minutes with no headers usually means the server is still
            // grinding on the transcode — a 4K HEVC source re-encoded to
            // H.264 is the classic case.
            set_err(TRN("Server did not respond in %us (still transcoding?)"),
                    (unsigned)(hdr_deadline_us / 1000000ULL));
            netClose(sock); return -1;
        }
        if (now - hdr_log_us >= 5000000ULL) {
            hdr_log_us = now;
            char buf[64];
            snprintf(buf, sizeof(buf), "stream_open: waiting for server (%llus)",
                     (unsigned long long)((now - hdr_t0) / 1000000ULL));
            plog(buf);
        }
    }
    hdr[htotal] = '\0';

    int status = 0;
    if (strncmp(hdr, "HTTP/", 5) == 0) {
        char *sp = strchr(hdr, ' ');
        if (sp) status = atoi(sp + 1);
    }

    s_chunked      = (strstr(hdr, "chunked") != NULL);
    s_chunk_remain = -1;
    s_chdr_n       = 0;
    s_ctrail       = 0;
    s_carry_n      = 0;
    sb_reset();     // new connection — drop anything buffered from the old one
    {
        char buf[64];
        // getsockopt(SO_RCVBUF) returns the REQUESTED size, not what libnet
        // has funded, so on its own it says little -- a 512 KB request reads
        // back as 524288 either way.  It earns its place as a HEALTH CHECK
        // instead: under the 4 MB libnet pool this same call failed outright
        // and returned -1, which is how that experiment was caught.
        //
        // recvq= is netGetSockInfo, and it returns -1 on this firmware whether
        // the pool is stock or enlarged -- it has never worked here, so do not
        // read a -1 there as a fault.  It stays only because a non-negative
        // value would be the real receive queue if a future build gets it
        // working.
        int rb_eff = 0; socklen_t rl = sizeof(rb_eff);
        if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rb_eff, &rl) != 0) rb_eff = -1;
        netSocketInfo si; memset(&si, 0, sizeof(si));
        int rq = (netGetSockInfo(sock, &si, 1) == 0) ? si.recv_queue_len : -1;
        snprintf(buf, sizeof(buf),
                 "stream_open: status=%d chunked=%d rcvbuf=%d recvq=%d read=%dK",
                 status, (int)s_chunked, rb_eff, rq, s_sb_req / 1024);
        plog(buf);
    }
    if (status != 200) {
        set_err(TRN("Server returned HTTP %d"), status);
        netClose(sock); return -1;
    }

    clear_err();
    return sock;
}

// -------------------------------------------------------------------------
// Local files (offline playback)
// -------------------------------------------------------------------------

#define STREAM_FILE_TAG 0x40000000   // lv2 socket ids are small; never this

bool stream_is_file(int h) { return h >= 0 && (h & STREAM_FILE_TAG) != 0; }
bool stream_file_removed(void) { return s_file_err == LFS_E_REMOVED; }

// The state every file source starts from: no chunking, no carried packet, an empty buffer.
static void file_stream_reset(int kind) {
    s_file_kind    = kind;
    s_file_err     = 0;
    s_chunked      = false;
    s_chunk_remain = -1;
    s_chdr_n       = 0;
    s_ctrail       = 0;
    s_carry_n      = 0;
    sb_reset();
    clear_err();
}

static int file_open_at(const char *path, u64 offset, int kind) {
    file_close_stream();
    const int fd = lfs_open(path);
    if (fd < 0) {
        set_err(fd == LFS_E_REMOVED ? TRN("The drive was removed") : TRN("Could not open the file"));
        return -1;
    }
    // an m2ts offset counts in the stripped stream: 188 of every 192 bytes
    const uint64_t at = kind == FILE_M2TS ? offset / 188u * 192u : offset;
    if (at > lfs_size(fd)) {
        lfs_close(fd);
        set_err(TRN("Could not seek the file"));
        return -1;
    }
    s_file_h   = fd;
    s_file_pos = offset;
    file_stream_reset(kind);
    char b[96];
    snprintf(b, sizeof(b), "stream_open_file: fd=%d offset=%llu%s", (int)fd,
             (unsigned long long)offset, kind == FILE_M2TS ? " (m2ts)" : "");
    plog(b);
    return (int)(STREAM_FILE_TAG | (u32)fd);
}

int stream_open_file(const char *path, u64 offset) { return file_open_at(path, offset, FILE_TS); }
int stream_open_m2ts(const char *path, u64 ts_offset) { return file_open_at(path, ts_offset, FILE_M2TS); }

int stream_open_mkv(const char *path, int video_track, int audio_track, int sub_track, MkvSubSink sub_sink,
                    void *sub_ctx, u64 start_ns, u64 *actual_start_ns) {
    file_close_stream();
    if (s_mkv_open && strcmp(path, s_mkv_path) != 0) stream_local_release();
    if (!s_mkv_open) {
        const int fd = lfs_open(path);
        if (fd < 0) {
            set_err(fd == LFS_E_REMOVED ? TRN("The drive was removed") : TRN("Could not open the file"));
            return -1;
        }
        s_file_err = 0;
        s_mkv_h = fd;
        if (mkv_open(&s_mkv, mkv_lfs_read, &s_mkv_h, lfs_size(fd)) != 0) {
            if (s_file_err == LFS_E_REMOVED)  set_err(TRN("The drive was removed"));
            else if (s_mkv.err[0])            set_err("%s", s_mkv.err);
            else                              set_err(TRN("Could not read the Matroska file"));
            lfs_close(fd);
            s_mkv_h = -1;
            return -1;
        }
        s_mkv_open = true;
        snprintf(s_mkv_path, sizeof(s_mkv_path), "%s", path);
    }
    file_stream_reset(FILE_MKV);
    char why[64] = "";
    s_mkv_ts = mkv_ts_open_subs(&s_mkv, video_track, audio_track, sub_track, sub_sink, sub_ctx, start_ns, actual_start_ns,
                                why, sizeof why);
    if (!s_mkv_ts) {
        if (s_file_err == LFS_E_REMOVED)  set_err(TRN("The drive was removed"));
        else if (why[0])                  set_err("%s", why);
        else                              set_err(TRN("Could not start the Matroska file"));
        return -1;
    }
    char b[96];
    snprintf(b, sizeof(b), "stream_open_mkv: v=%d a=%d start=%llums", video_track, audio_track,
             (unsigned long long)(actual_start_ns ? *actual_start_ns / 1000000ULL : 0));
    plog(b);
    return (int)(STREAM_FILE_TAG | (u32)s_mkv_h);
}

void stream_local_release(void) {
    file_close_stream();
    if (s_mkv_open) {
        mkv_close(&s_mkv);
        s_mkv_open = false;
    }
    if (s_mkv_h >= 0) { lfs_close(s_mkv_h); s_mkv_h = -1; }
    s_mkv_path[0] = '\0';
    mkv_ts_release();
}

void stream_close(int h) {
    if (stream_is_file(h)) {
        file_close_stream();         // a Matroska file stays parsed until stream_local_release
        sb_reset();
        return;
    }
    if (h >= 0) netClose(h);
}

void stream_set_timeout(int h, u32 usec) {
    if (stream_is_file(h)) return;    // a file never waits on anyone
    // The lv2 call takes a 16-byte struct timeval; any smaller layout is
    // silently ignored and reads then block for seconds.
    struct timeval tv;
    tv.tv_sec  = usec / 1000000u;
    tv.tv_usec = usec % 1000000u;
    netSetSockOpt(h, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

u8 *stream_scratch(int *cap) {
    // Free exactly when nothing is being streamed: before an open, and
    // during a seek (the old stream is closed first).
    *cap = SB_SIZE;
    return s_sb;
}

const char *stream_last_error(void) {
    return s_last_error[0] ? s_last_error : "Could not reach the server";
}

const char *stream_last_error_ui(void) {
    return s_last_error_ui[0] ? s_last_error_ui : TR("Could not reach the server");
}

// Stash the partial packet so the next call resumes instead of losing bytes.
static int stream_save_carry(const u8 *buf, int got) {
    if (got > 0 && got <= (int)sizeof(s_carry)) {
        memcpy(s_carry, buf, got);
        s_carry_n = got;
        static int s_carry_log = 0;
        if (s_carry_log < 20) {
            s_carry_log++;
            char lb[48];
            snprintf(lb, sizeof(lb), "stream_carry: saved=%d", got);
            plog(lb);
        }
    }
    return 0;
}

// Is there anything for stream_read() to return without blocking?  Buffered
// bytes, a carried partial packet, or data (or a close) on the socket within
// timeout_ms.  (2026-09-27, see the decode loop.)
bool stream_readable(int sock, int timeout_ms) {
    if (s_carry_n > 0 || s_sb_p < s_sb_n) return true;
    struct pollfd pfd; pfd.fd = sock; pfd.events = POLLIN; pfd.revents = 0;
    return netPoll(&pfd, 1, timeout_ms) != 0;     // < 0: let the read report it
}

int stream_read(int sock, u8 *buf, int size) {
    int got = 0;
    // Resume a packet interrupted by a receive timeout on a previous call.
    if (s_carry_n > 0) {
        got = s_carry_n <= size ? s_carry_n : size;
        memcpy(buf, s_carry, got);
        s_carry_n = 0;
    }
    while (got < size) {
        if (!s_chunked) {
            int n = sb_read(sock, buf + got, size - got);
            if (n == 0) {
                plog("net_error: rc=0 (closed)");
                lc_logf("stream: peer closed (body)");
                return -1;
            }
            if (n < 0) return stream_save_carry(buf, got);   // timeout: resume later
            got += n;
            continue;
        }

        if (s_ctrail > 0) {
            u8 c;
            int n = sb_getc(sock, &c);
            if (n == 0) {
                lc_logf("stream: peer closed (chunk trailer)");
                return -1;
            }
            if (n <  0) return stream_save_carry(buf, got);   // timeout: resume later
            s_ctrail--;
            continue;
        }

        if (s_chunk_remain <= 0) {
            u8 c;
            int n = sb_getc(sock, &c);
            if (n == 0) {
                lc_logf("stream: peer closed (chunk header)");
                return -1;
            }
            if (n <  0) return stream_save_carry(buf, got);   // timeout: resume later
            if (c == '\n') {
                int llen = s_chdr_n;
                while (llen > 0 && (s_chdr[llen-1] == '\r' || s_chdr[llen-1] == ' '))
                    llen--;
                s_chdr[llen] = '\0';
                s_chunk_remain = (int)strtol(s_chdr, NULL, 16);
                s_chdr_n = 0;
                if (s_chunk_remain == 0) {
                    // The server ended the response on purpose: a 0-length
                    // chunk is the end of the body, not a network failure.
                    lc_logf("stream: server sent final 0-length chunk (response ended)");
                    return -1;
                }
            } else if (s_chdr_n < (int)sizeof(s_chdr) - 1) {
                s_chdr[s_chdr_n++] = (char)c;
            }
            continue;
        }

        int want = size - got;
        if (want > s_chunk_remain) want = s_chunk_remain;
        int n = sb_read(sock, buf + got, want);
        if (n == 0) {
            plog("net_error: rc=0 (closed)");
                lc_logf("stream: peer closed (body)");
            return -1;
        }
        if (n < 0) return stream_save_carry(buf, got);   // timeout: resume later
        got            += n;
        s_chunk_remain -= n;
        if (s_chunk_remain == 0)
            s_ctrail = 2;
    }
    return 1;
}
