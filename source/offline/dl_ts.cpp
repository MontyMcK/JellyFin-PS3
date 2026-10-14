// Offline downloads -- MPEG-TS sanity scan.  See dl_ts.h.

#include "dl_ts.h"

#include <string.h>

void dl_ts_init(DlTsScan *t) { memset(t, 0, sizeof(*t)); }

static void on_pts(DlTsScan *t, uint64_t raw) {
    // 33-bit wrap: a drop of more than half the range is the counter rolling
    // over (B-frame reordering only ever steps back a few frames).
    if (t->have_pts && raw < t->prev_raw && t->prev_raw - raw > (1ULL << 32))
        t->wrap_add += 1ULL << 33;
    t->prev_raw = raw;
    const uint64_t pts = raw + t->wrap_add;
    if (!t->have_pts) {
        t->have_pts = true;
        t->first_pts = t->max_pts = pts;
        return;
    }
    if (pts > t->max_pts) t->max_pts = pts;
    if (pts < t->first_pts) t->first_pts = pts;   // reordered opening frames
}

void dl_ts_packet_info(const uint8_t *p, DlTsPacketInfo *o) {
    memset(o, 0, sizeof(*o));
    if (p[0] != 0x47) return;
    o->sync = true;
    o->pid  = (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
    o->pusi = (p[1] & 0x40) != 0;
    const int afc = (p[3] >> 4) & 3;
    int off = 4;
    if (afc & 2) {
        if (p[4] > 0) o->rai = (p[5] & 0x40) != 0;
        off += 1 + p[4];
    }
    if (!o->pusi || !(afc & 1)) return;         // no PES starts here
    if (off + 14 > DL_TS_PACKET) return;
    const uint8_t *e = p + off;
    if (e[0] != 0 || e[1] != 0 || e[2] != 1) return;
    if (e[3] < 0xE0 || e[3] > 0xEF) return;     // video PES only
    if (!(e[7] & 0x80)) return;                 // no PTS
    o->video_pts = true;
    o->pts = ((uint64_t)((e[9] >> 1) & 7) << 30) |
             ((uint64_t)e[10] << 22) |
             ((uint64_t)(e[11] >> 1) << 15) |
             ((uint64_t)e[12] << 7) |
             ((uint64_t)e[13] >> 1);
}

static void packet(DlTsScan *t, const uint8_t *p) {
    t->packets++;
    DlTsPacketInfo i;
    dl_ts_packet_info(p, &i);
    if (!i.sync) { t->sync_errors++; return; }
    if (i.video_pts) on_pts(t, i.pts);
}

void dl_ts_feed(DlTsScan *t, const uint8_t *d, int len) {
    int i = 0;
    if (t->carry_n > 0) {
        int need = DL_TS_PACKET - t->carry_n;
        int take = len < need ? len : need;
        memcpy(t->carry + t->carry_n, d, (size_t)take);
        t->carry_n += take;
        i = take;
        if (t->carry_n < DL_TS_PACKET) return;
        packet(t, t->carry);
        t->carry_n = 0;
    }
    for (; i + DL_TS_PACKET <= len; i += DL_TS_PACKET) packet(t, d + i);
    if (i < len) {
        t->carry_n = len - i;
        memcpy(t->carry, d + i, (size_t)t->carry_n);
    }
}

int64_t dl_ts_span_secs(const DlTsScan *t) {
    if (!t->have_pts) return -1;
    return (int64_t)((t->max_pts - t->first_pts) / 90000ULL);
}

bool dl_ts_plausible(const DlTsScan *t, uint64_t file_bytes,
                     uint32_t expect_secs, bool *short_out) {
    *short_out = false;
    if (file_bytes == 0 || file_bytes % DL_TS_PACKET != 0) return false;
    if (t->sync_errors > 0 || t->packets == 0) return false;
    const int64_t span = dl_ts_span_secs(t);
    if (span < 0) return false;                 // no video: nothing to play
    if (expect_secs > 0 && (uint64_t)span + 10 < (uint64_t)expect_secs * 9 / 10) {
        *short_out = true;
        return false;
    }
    return true;
}
