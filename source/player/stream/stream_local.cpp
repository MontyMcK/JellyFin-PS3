// Local MPEG-TS files -- index and seek.  See stream_local.h.

#include "stream_local.h"
#include "dl_ts.h"

#include <string.h>

#define PKT DL_TS_PACKET
#define PTS_MASK ((1ULL << 33) - 1)

static uint64_t align_down(uint64_t off) { return off - (off % PKT); }

// PTS difference with 33-bit wrap: how far b is after a.
static uint64_t pts_after(uint64_t a, uint64_t b) { return (b - a) & PTS_MASK; }

// Keyframe candidates: flagged (random_access_indicator) when the file flags
// them; otherwise a video PES that a PAT immediately precedes -- which is
// exactly where ffmpeg's muxer puts its PAT/PMT (before each keyframe).
#define PAT_NEAR (16u * PKT)

typedef struct {
    bool     any;                  // at least one candidate in the window
    uint64_t first_off, first_us;  // first candidate
    bool     best;                 // latest candidate at or before the limit
    uint64_t best_off, best_us;
    bool     after;                // first candidate past the limit
    uint64_t read;                 // bytes covered
} Window;

static uint64_t to_us(const StreamLocalIndex *idx, uint64_t pts) {
    return idx->ok ? pts_after(idx->first_pts, pts) * 100 / 9 : 0;
}

// One read at `from` (backed up a little so a PAT just before `from` is
// seen), classifying every candidate against limit_us.
static bool scan_window(StreamReadAtFn rd, void *ctx, const StreamLocalIndex *idx,
                        bool need_rai, uint64_t from, uint64_t limit_us,
                        uint8_t *buf, int cap, Window *w) {
    memset(w, 0, sizeof(*w));
    const int win = cap - (cap % PKT);
    if (cap < (int)STREAM_LOCAL_MIN_SCRATCH) return false;
    uint64_t off = align_down(from);
    off = off > PAT_NEAR ? off - PAT_NEAR : 0;
    int n = rd(ctx, off, buf, win);
    if (n <= 0) return false;
    n -= n % PKT;
    w->read = (uint64_t)n;
    uint64_t last_pat = UINT64_MAX;
    for (int i = 0; i < n; i += PKT) {
        DlTsPacketInfo p;
        dl_ts_packet_info(buf + i, &p);
        const uint64_t at = off + (uint64_t)i;
        if (!p.sync) continue;
        if (p.pid == 0 && p.pusi) { last_pat = at; continue; }
        if (!p.video_pts) continue;
        const bool pat_near = last_pat != UINT64_MAX && at - last_pat <= PAT_NEAR;
        const bool key = need_rai ? p.rai : pat_near;
        if (!key) continue;
        const uint64_t e_off = pat_near ? last_pat : at;
        const uint64_t e_us  = to_us(idx, p.pts);
        last_pat = UINT64_MAX;      // one entry per PAT
        if (!w->any) { w->any = true; w->first_off = e_off; w->first_us = e_us; }
        if (e_us <= limit_us) {
            if (!w->best || e_us >= w->best_us) { w->best = true; w->best_off = e_off; w->best_us = e_us; }
        } else if (!w->after) {
            w->after = true;
        }
    }
    return true;
}

bool stream_local_entry(StreamReadAtFn rd, void *ctx, const StreamLocalIndex *idx,
                        uint64_t from, uint8_t *buf, int cap,
                        uint64_t *entry_off, uint64_t *entry_us) {
    const bool need_rai = idx->has_rai;   // set by the index before any entry search
    // Forward, window by window, to the first candidate (bounded).
    uint64_t off = align_down(from), scanned = 0;
    while (off < idx->size && scanned < STREAM_LOCAL_SCAN_MAX) {
        Window w;
        if (!scan_window(rd, ctx, idx, need_rai, off, UINT64_MAX, buf, cap, &w)) break;
        if (w.any) { *entry_off = w.first_off; *entry_us = w.first_us; return true; }
        if (w.read <= 2 * PAT_NEAR) break;           // EOF (or a tiny tail)
        // The window started PAT_NEAR before `off` (except at 0): move on
        // to where it ended.
        off = (off > PAT_NEAR ? off - PAT_NEAR : 0) + w.read;
        scanned += w.read;
    }
    return false;
}

// Does the file flag its keyframes?  Looked for over the first windows.
static bool detect_rai(StreamReadAtFn rd, void *ctx, uint64_t size, uint8_t *buf, int cap) {
    const int win = cap - (cap % PKT);
    uint64_t off = 0;
    for (int k = 0; k < 8 && off < size; k++) {
        int n = rd(ctx, off, buf, win);
        if (n <= 0) break;
        n -= n % PKT;
        for (int i = 0; i < n; i += PKT) {
            DlTsPacketInfo p;
            dl_ts_packet_info(buf + i, &p);
            if (p.video_pts && p.rai) return true;
        }
        off += (uint64_t)n;
    }
    return false;
}

void stream_local_clock(const StreamLocalIndex *idx, uint64_t entry_us, uint64_t lead_us,
                        uint64_t *origin_us, uint64_t *play_base_us) {
    const uint64_t abs_us = idx->first_pts * 100 / 9 + entry_us;      // the entry's PTS, in microseconds
    const uint64_t origin = abs_us > lead_us ? abs_us - lead_us : 0;
    const uint64_t lead = abs_us - origin;                            // lead_us, or less near the start of time
    *origin_us = origin;
    *play_base_us = entry_us > lead ? entry_us - lead : 0;
}

bool stream_local_index(StreamReadAtFn rd, void *ctx, uint64_t size,
                        uint8_t *buf, int cap, StreamLocalIndex *out) {
    memset(out, 0, sizeof(*out));
    out->size = size;
    if (size < PKT || cap < (int)STREAM_LOCAL_MIN_SCRATCH) return false;
    out->has_rai = detect_rai(rd, ctx, size, buf, cap);

    // First entry's raw PTS: enter from byte 0 (base not known yet), then
    // read that entry's frame PTS.
    StreamLocalIndex tmp = *out;
    uint64_t eoff, eus;
    if (!stream_local_entry(rd, ctx, &tmp, 0, buf, cap, &eoff, &eus)) return false;
    uint64_t first = UINT64_MAX;
    {
        int n = rd(ctx, eoff, buf, cap - cap % PKT);
        if (n > 0) n -= n % PKT;
        for (int i = 0; i < n && first == UINT64_MAX; i += PKT) {
            DlTsPacketInfo p;
            dl_ts_packet_info(buf + i, &p);
            if (p.video_pts) first = p.pts;
        }
    }
    if (first == UINT64_MAX) return false;

    // Last video PTS: backwards from the end, one window at a time.
    const int win = cap - cap % PKT;
    uint64_t end = align_down(size);
    bool have_end = false;
    uint64_t end_pts = first, scanned = 0;
    while (end > 0 && !have_end && scanned < STREAM_LOCAL_SCAN_MAX) {
        uint64_t start = end > (uint64_t)win ? end - (uint64_t)win : 0;
        int n = rd(ctx, start, buf, (int)(end - start));
        if (n <= 0) break;
        n -= n % PKT;
        for (int i = 0; i < n; i += PKT) {
            DlTsPacketInfo p;
            dl_ts_packet_info(buf + i, &p);
            if (!p.video_pts) continue;
            // Highest PTS in presentation order (B-frames step back a little).
            if (!have_end || pts_after(end_pts, p.pts) < (1ULL << 32)) end_pts = p.pts;
            have_end = true;
        }
        scanned += end - start;
        end = start;
    }
    out->first_pts = first;
    out->end_pts = have_end ? end_pts : first;
    out->duration_secs = (uint32_t)(pts_after(first, out->end_pts) / 90000ULL);
    out->ok = true;
    return true;
}

bool stream_local_seek(StreamReadAtFn rd, void *ctx, const StreamLocalIndex *idx,
                       uint64_t target_us, uint8_t *buf, int cap,
                       uint64_t *entry_off, uint64_t *entry_us) {
    if (!idx->ok || cap < (int)STREAM_LOCAL_MIN_SCRATCH) return false;
    const bool need_rai = idx->has_rai;
    uint64_t best_off, best_us;
    if (!stream_local_entry(rd, ctx, idx, 0, buf, cap, &best_off, &best_us)) return false;
    const uint64_t slack = 500000;   // land at most 0.5 s past the target
    const uint64_t dur_us = (uint64_t)idx->duration_secs * 1000000ULL;
    if (target_us > dur_us) target_us = dur_us;
    if (target_us <= best_us + slack || dur_us == 0) {
        *entry_off = best_off; *entry_us = best_us;
        return true;
    }
    const uint64_t limit = target_us + slack;
    const uint64_t win = (uint64_t)(cap - cap % PKT);

    // 1) Narrow a bracket [lo, hi] around the target: interpolation first
    //    (bitrate is roughly steady), bisection when that stops paying.
    uint64_t lo_off = best_off, lo_us = best_us;
    uint64_t hi_off = idx->size, hi_us = dur_us;
    for (int probe = 0; probe < 24 && hi_off > lo_off + 2 * win; probe++) {
        uint64_t guess;
        if (probe < 6 && hi_us > lo_us) {
            const double frac = (double)(target_us - lo_us) / (double)(hi_us - lo_us);
            guess = lo_off + (uint64_t)((double)(hi_off - lo_off) * (frac < 0 ? 0 : frac > 1 ? 1 : frac));
        } else {
            guess = lo_off + (hi_off - lo_off) / 2;
        }
        // Keep the probe strictly inside the bracket so it always shrinks.
        if (guess < lo_off + win / 2) guess = lo_off + win / 2;
        if (guess > hi_off - win) guess = hi_off - win;
        Window w;
        if (!scan_window(rd, ctx, idx, need_rai, guess, limit, buf, cap, &w)) break;
        if (w.best && w.best_us >= best_us) { best_off = w.best_off; best_us = w.best_us; }
        if (w.best && w.after) break;              // bracketed inside one read
        if (!w.any) {                              // a long GOP: no candidate here
            uint64_t eo, eu;
            if (!stream_local_entry(rd, ctx, idx, guess, buf, cap, &eo, &eu) || eu > limit) {
                hi_off = guess; hi_us = target_us;
            } else {
                if (eu >= best_us) { best_off = eo; best_us = eu; }
                lo_off = eo; lo_us = eu;
            }
            continue;
        }
        if (w.first_us > limit) { hi_off = guess; hi_us = w.first_us; }
        else { lo_off = w.best_off; lo_us = w.best_us; }
    }

    // 2) Finish with a forward scan from the best entry so far: the last
    //    candidate at or before the limit, exactly.  Bounded.
    uint64_t off = best_off, scanned = 0;
    while (off < idx->size && scanned < STREAM_LOCAL_SCAN_MAX) {
        Window w;
        if (!scan_window(rd, ctx, idx, need_rai, off + PAT_NEAR, limit, buf, cap, &w)) break;
        if (w.best && w.best_us >= best_us) { best_off = w.best_off; best_us = w.best_us; }
        if (w.after || w.read < win) break;
        off += w.read - PAT_NEAR;          // read > 2*PAT_NEAR: MIN_SCRATCH
        scanned += w.read;
    }
    *entry_off = best_off;
    *entry_us  = best_us;
    return true;
}
