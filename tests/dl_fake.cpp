// Host implementation of source/offline/dl_platform.h for test_offline.
// See dl_fake.h.

#include "dl_fake.h"
#include "dl_platform.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

std::vector<FakeResp>    g_fake_queue;
FakeResp                 g_fake_default;
std::vector<std::string> g_fake_requests;
int64_t                  g_fake_total = 0;
uint64_t                 g_fake_free = 1ull << 40;
int                      g_fake_reserve_gb = 0;
int64_t                  g_fake_write_budget = -1;
uint64_t                 g_fake_now_ms = 1000000;
int                      g_fake_connects = 0;
bool                     g_fake_verbose = false;
int                      g_fake_data_recvs = 0;
std::vector<int>         g_fake_write_sizes;
void                   (*g_fake_on_sleep)(void) = nullptr;
std::map<std::string, FakeResp> g_fake_routes;
int                      g_fake_route_hits = 0;
int                      g_fake_thread_starts = 0;
int                      g_fake_thread_joins = 0;
bool                     g_fake_thread_fail = false;
bool                     g_fake_app_running = true;
void                   (*g_fake_on_join)(void) = nullptr;

void fake_reset(void) {
    g_fake_queue.clear();
    g_fake_default = FakeResp();
    g_fake_requests.clear();
    g_fake_total = 300000;
    g_fake_free = 1ull << 40;      // plenty: unknown would mean no downloads
    g_fake_reserve_gb = 0;
    g_fake_write_budget = -1;
    g_fake_connects = 0;
    g_fake_data_recvs = 0;
    g_fake_write_sizes.clear();
    g_fake_on_sleep = nullptr;
    g_fake_routes.clear();
    g_fake_route_hits = 0;
    g_fake_thread_fail = false;
    g_fake_app_running = true;
}

std::string fake_ts(int packets, double secs) {
    std::string out;
    const int every = 50;
    const int starts = (packets + every - 1) / every;
    for (int i = 0; i < packets; i++) {
        uint8_t p[188];
        memset(p, 0xFF, sizeof(p));
        p[0] = 0x47;
        p[1] = 0x01; p[2] = 0x00;        // PID 0x100
        p[3] = 0x10 | (i & 0x0F);        // payload only
        if (i % every == 0) {
            p[1] |= 0x40;                // PUSI
            uint64_t pts = 126000 + (uint64_t)((double)(i / every) *
                            (secs * 90000.0) / (starts > 1 ? starts - 1 : 1));
            uint8_t *e = p + 4;
            e[0] = 0; e[1] = 0; e[2] = 1; e[3] = 0xE0;   // video PES
            e[4] = 0; e[5] = 0; e[6] = 0x80; e[7] = 0x80; e[8] = 5;
            e[9]  = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
            e[10] = (uint8_t)(pts >> 22);
            e[11] = (uint8_t)(((pts >> 14) & 0xFE) | 1);
            e[12] = (uint8_t)(pts >> 7);
            e[13] = (uint8_t)(((pts << 1) & 0xFE) | 1);
        }
        out.append((const char *)p, 188);
    }
    return out;
}

std::string fake_jpeg(int bytes) {
    std::string j = "\xFF\xD8\xFF\xE0";
    while ((int)j.size() < bytes - 2) j.push_back((char)(j.size() * 7));
    j += "\xFF\xD9";
    return j;
}

uint8_t fake_byte(uint64_t i) {
    return (uint8_t)(((i * 2654435761u) >> 13) ^ (i >> 7));
}

bool fake_file_matches(const char *path, int64_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    int64_t i = 0;
    int c;
    bool ok = true;
    while ((c = fgetc(f)) != EOF) {
        if (i >= size || (uint8_t)c != fake_byte((uint64_t)i)) { ok = false; break; }
        i++;
    }
    fclose(f);
    return ok && i == size;
}

std::string fake_mkdtemp(void) {
    const char *base = getenv("TMPDIR");
    std::string t = std::string(base && *base ? base : "/tmp") + "/jfdl-XXXXXX";
    std::vector<char> b(t.begin(), t.end());
    b.push_back('\0');
    if (!mkdtemp(b.data())) { perror("mkdtemp"); exit(2); }
    return std::string(b.data());
}

void fake_rmtree(const std::string &dir) {
    DIR *d = opendir(dir.c_str());
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string p = dir + "/" + e->d_name;
            struct stat st;
            if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) fake_rmtree(p);
            else unlink(p.c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
}

// -------------------------------------------------------------------------
// Fake server connections
// -------------------------------------------------------------------------

struct Conn {
    bool        open = false;
    FakeResp    resp;
    std::string req;
    bool        built = false;
    std::string out;          // full response bytes
    size_t      pos = 0;
    size_t      body_at = 0;  // offset of the body within out
    int         reads = 0;
};

static Conn s_conns[8];

static uint64_t parse_range(const std::string &req, bool *has) {
    *has = false;
    size_t p = req.find("\r\nRange: bytes=");
    if (p == std::string::npos) return 0;
    *has = true;
    return strtoull(req.c_str() + p + 15, NULL, 10);
}

static void choose_response(Conn *c) {
    for (auto &kv : g_fake_routes) {
        if (c->req.find(kv.first) != std::string::npos) {
            c->resp = kv.second;
            g_fake_route_hits++;
            return;
        }
    }
    if (!g_fake_queue.empty()) {
        c->resp = g_fake_queue.front();
        g_fake_queue.erase(g_fake_queue.begin());
    } else {
        c->resp = g_fake_default;
    }
}

static void build_response(Conn *c) {
    choose_response(c);
    const FakeResp &r = c->resp;
    c->built = true;
    if (!r.raw.empty()) { c->out = r.raw; c->body_at = r.raw.size(); return; }
    const int64_t total = r.has_body ? (int64_t)r.body.size()
                        : r.total >= 0 ? r.total : g_fake_total;
    bool has_range = false;
    uint64_t from = parse_range(c->req, &has_range);
    int status = 200;
    uint64_t start = 0;
    char hdr[1024];
    int hn = 0;
    if (has_range && r.honor_range) {
        if ((int64_t)from >= total) {
            status = 416;
        } else {
            status = 206;
            start = from;
        }
    }
    if (r.status) status = r.status;
    const int64_t claim_total = r.total_lie >= 0 ? r.total_lie : total;
    std::string body;
    if (r.has_body && (status == 200 || status == 206)) {
        body = r.body.substr((size_t)start);
    } else if (status == 200 || status == 206) {
        for (int64_t i = (int64_t)start; i < total; i++) body.push_back((char)fake_byte((uint64_t)i));
    } else if (status != 416) {
        body = "{\"error\":\"nope\"}";
    }
    hn += snprintf(hdr + hn, sizeof(hdr) - hn, "HTTP/1.1 %d X\r\n", status);
    hn += snprintf(hdr + hn, sizeof(hdr) - hn, "Content-Type: %s\r\n", r.content_type.c_str());
    if (r.accept_ranges) hn += snprintf(hdr + hn, sizeof(hdr) - hn, "Accept-Ranges: bytes\r\n");
    if (status == 206) {
        uint64_t cs = r.range_start_lie >= 0 ? (uint64_t)r.range_start_lie : start;
        hn += snprintf(hdr + hn, sizeof(hdr) - hn, "Content-Range: bytes %llu-%llu/%lld\r\n",
                       (unsigned long long)cs, (unsigned long long)(total - 1),
                       (long long)claim_total);
    } else if (status == 416) {
        hn += snprintf(hdr + hn, sizeof(hdr) - hn, "Content-Range: bytes */%lld\r\n",
                       (long long)claim_total);
    }
    std::string framed;
    if (r.chunked) {
        hn += snprintf(hdr + hn, sizeof(hdr) - hn, "Transfer-Encoding: chunked\r\n");
        size_t i = 0, k = 0;
        while (i < body.size()) {
            size_t n = 1000 + (k++ * 7919) % 20000;   // uneven chunk sizes
            if (n > body.size() - i) n = body.size() - i;
            char line[32];
            snprintf(line, sizeof(line), "%zx\r\n", n);
            framed += line;
            framed.append(body, i, n);
            framed += "\r\n";
            i += n;
        }
        framed += "0\r\n\r\n";
    } else {
        framed = body;
        if (!r.no_length) {
            int64_t cl = (int64_t)body.size();
            if (r.total_lie >= 0 && status == 200) cl = r.total_lie;
            hn += snprintf(hdr + hn, sizeof(hdr) - hn, "Content-Length: %lld\r\n", (long long)cl);
        }
    }
    hn += snprintf(hdr + hn, sizeof(hdr) - hn, "\r\n");
    c->out.assign(hdr, (size_t)hn);
    c->body_at = c->out.size();
    c->out += framed;
}

int dl_plat_connect(const char *host, int port) {
    (void)host; (void)port;
    g_fake_connects++;
    // Refusal is decided at connect (the request is not known yet); the
    // answer to an accepted connection is chosen from the request itself.
    if (!g_fake_queue.empty()) {
        if (g_fake_queue.front().refuse) { g_fake_queue.erase(g_fake_queue.begin()); return -1; }
    } else if (g_fake_default.refuse) {
        return -1;
    }
    for (int i = 0; i < 8; i++) {
        if (!s_conns[i].open) {
            s_conns[i] = Conn();
            s_conns[i].open = true;
            return i;
        }
    }
    return -1;
}

int dl_plat_send(int h, const void *buf, int len) {
    Conn *c = &s_conns[h];
    c->req.append((const char *)buf, (size_t)len);
    g_fake_requests.push_back(c->req);
    return len;
}

int dl_plat_recv(int h, void *buf, int cap) {
    Conn *c = &s_conns[h];
    const FakeResp &r = c->resp;
    if (!c->built) build_response(c);
    if (r.close_now) return DL_RECV_CLOSED;
    if (r.head_stall) { g_fake_now_ms += 1000; return DL_RECV_TIMEOUT; }
    int64_t body_sent = (int64_t)c->pos - (int64_t)c->body_at;
    if (body_sent < 0) body_sent = 0;
    if (r.on_body && c->pos >= c->body_at) r.on_body(body_sent);
    auto limit_hit = [&](int64_t lim) { return lim >= 0 && body_sent >= lim; };
    if (limit_hit(r.stall_after)) { g_fake_now_ms += 1000; return DL_RECV_TIMEOUT; }
    if (limit_hit(r.reset_after)) return DL_RECV_ERROR;
    if (limit_hit(r.drop_after))  return DL_RECV_CLOSED;
    if (r.timeout_every > 0 && c->pos < c->out.size()) {
        // A slow segment: every Nth read first comes back empty-handed.
        if (++c->reads % (r.timeout_every + 1) == 0) { g_fake_now_ms += 1000; return DL_RECV_TIMEOUT; }
    }
    if (c->pos >= c->out.size()) return DL_RECV_CLOSED;
    size_t n = c->out.size() - c->pos;
    if (n > (size_t)r.recv_chunk) n = (size_t)r.recv_chunk;
    if (n > (size_t)cap) n = (size_t)cap;
    // Never cross a scripted limit inside one read.
    int64_t lims[3] = { r.stall_after, r.reset_after, r.drop_after };
    for (int64_t lim : lims) {
        if (lim < 0 || c->pos < c->body_at) continue;
        int64_t left = lim - body_sent;
        if (left >= 0 && (int64_t)n > left) n = (size_t)left;
    }
    if (c->pos < c->body_at && c->pos + n > c->body_at) {
        // A head read may run into the body, as a real socket's would, but
        // not past a limit counted from the body start.
        int64_t allow = -1;
        for (int64_t lim : lims)
            if (lim >= 0 && (allow < 0 || lim < allow)) allow = lim;
        if (allow >= 0 && c->pos + n > c->body_at + (size_t)allow)
            n = c->body_at + (size_t)allow - c->pos;
    }
    if (n == 0) { g_fake_now_ms += 1000; return DL_RECV_TIMEOUT; }
    memcpy(buf, c->out.data() + c->pos, n);
    c->pos += n;
    g_fake_now_ms += 1;
    g_fake_data_recvs++;
    return (int)n;
}

void dl_plat_close(int h) { if (h >= 0 && h < 8) s_conns[h].open = false; }

// -------------------------------------------------------------------------
// Filesystem: real, under the test's temp root
// -------------------------------------------------------------------------

bool dl_plat_mkdir(const char *path) {
    if (mkdir(path, 0755) == 0) return true;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool dl_plat_rmdir(const char *path) { return rmdir(path) == 0; }

bool dl_plat_remove(const char *path) {
    if (unlink(path) == 0) return true;
    struct stat st;
    return stat(path, &st) != 0;
}

bool dl_plat_rename(const char *from, const char *to) { return rename(from, to) == 0; }

int64_t dl_plat_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (int64_t)st.st_size;
}

bool dl_plat_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool dl_plat_truncate(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    close(fd);
    return true;
}

int dl_plat_read_file(const char *path, void *buf, int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    const int n = (int)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

bool dl_plat_write_file(const char *path, const void *data, int len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

int dl_plat_list_dirs(const char *path, void (*cb)(const char *, void *), void *ctx) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string p = std::string(path) + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { cb(e->d_name, ctx); n++; }
    }
    closedir(d);
    return n;
}

uint64_t dl_plat_free_bytes(const char *path) { (void)path; return g_fake_free; }
int      dl_plat_reserve_gb(void) { return g_fake_reserve_gb; }

int dl_plat_file_open_append(const char *path) {
    return open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

int dl_plat_file_write(int fh, const void *buf, int len) {
    g_fake_write_sizes.push_back(len);
    if (g_fake_write_budget >= 0) {
        if (g_fake_write_budget < len) {       // disk full part way
            if (g_fake_write_budget > 0) {
                ssize_t w = write(fh, buf, (size_t)g_fake_write_budget);
                (void)w;
            }
            g_fake_write_budget = 0;
            g_fake_free = 0;
            return -1;
        }
        g_fake_write_budget -= len;
    }
    return write(fh, buf, (size_t)len) == (ssize_t)len ? len : -1;
}

bool dl_plat_file_sync(int fh) { (void)fh; return true; }
void dl_plat_file_close(int fh) { close(fh); }

// -------------------------------------------------------------------------
// Time, lock, log
// -------------------------------------------------------------------------

uint64_t dl_plat_now_ms(void) { return g_fake_now_ms; }
void     dl_plat_sleep_ms(unsigned ms) {
    g_fake_now_ms += ms;
    if (g_fake_on_sleep) g_fake_on_sleep();
}
void     dl_plat_lock(void) {}
void     dl_plat_unlock(void) {}
void     dl_plat_log(const char *line) { if (g_fake_verbose) printf("    [%s]\n", line); }

// -------------------------------------------------------------------------
// Worker thread: recorded, never run (tests call dl_svc_tick themselves)
// -------------------------------------------------------------------------

bool dl_plat_thread_start(void (*fn)(void)) {
    (void)fn;
    if (g_fake_thread_fail) return false;
    g_fake_thread_starts++;
    return true;
}
void dl_plat_thread_join(void) {
    g_fake_thread_joins++;
    if (g_fake_on_join) g_fake_on_join();
}
bool dl_plat_app_running(void) { return g_fake_app_running; }
