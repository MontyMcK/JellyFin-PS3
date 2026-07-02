#include "ts_demux.h"
#include "plog.h"

#include <stdio.h>
#include <string.h>
#include <codec/vdec.h>   // VDEC_TS_INVALID

#define TS_SYNC_BYTE     0x47
#define TS_PID_PAT       0x0000
#define TS_STREAM_H264   0x1B
#define TS_STREAM_MP3    0x03   // MPEG-1 audio (Layer 1/2/3)
#define TS_STREAM_MP3_2  0x04   // MPEG-2 audio

static void ts_parse_pat(TSState *ts, const u8 *data, int len) {
    if (len < 12 || data[0] != 0x00) return;
    u16 sec_len = ((u16)(data[1] & 0x0F) << 8) | data[2];
    int end = 3 + (int)sec_len - 4;
    if (end > len) end = len;
    int pos = 8;
    while (pos + 3 < end) {
        u16 prog = ((u16)data[pos] << 8) | data[pos+1];
        u16 pid  = ((u16)(data[pos+2] & 0x1F) << 8) | data[pos+3];
        if (prog != 0) { ts->pmt_pid = pid; return; }
        pos += 4;
    }
}

static void ts_parse_pmt(TSState *ts, const u8 *data, int len) {
    if (len < 16 || data[0] != 0x02) return;
    u16 sec_len   = ((u16)(data[1] & 0x0F) << 8) | data[2];
    int end       = 3 + (int)sec_len - 4;
    if (end > len) end = len;
    u16 prog_info = ((u16)(data[10] & 0x0F) << 8) | data[11];
    int pos       = 12 + prog_info;
    while (pos + 4 < end) {
        u8  stype  = data[pos];
        u16 epid   = ((u16)(data[pos+1] & 0x1F) << 8) | data[pos+2];
        u16 esinfo = ((u16)(data[pos+3] & 0x0F) << 8) | data[pos+4];
        if (stype == TS_STREAM_H264 && !ts->video_pid)
            ts->video_pid = epid;
        if ((stype == TS_STREAM_MP3 || stype == TS_STREAM_MP3_2) && !ts->audio_pid)
            ts->audio_pid = epid;
        pos += 5 + esinfo;
    }
}

// Accumulate one TS payload into a PES reassembly buffer.  When a new
// payload-unit start arrives, the previously assembled PES is emitted to
// `out`/`out_len` first.  Returns true if a complete PES was emitted.
static bool pes_accumulate(u8 *buf, int cap, int *len, bool *started,
                           bool pusi, const u8 *pay, int plen,
                           u8 *out, int *out_len,
                           int *trunc_log, const char *tag) {
    bool emitted = false;
    if (pusi && *started && *len > 0) {
        int copy = *len < cap ? *len : cap;
        memcpy(out, buf, copy);
        *out_len = copy;
        emitted  = true;
        *len     = 0;
    }
    if (pusi) { *started = true; *len = 0; }
    if (*started && plen > 0) {
        int room = cap - *len;
        int copy = plen < room ? plen : room;
        if (copy < plen && *trunc_log < 20) {
            // Truncated PES = corrupted AU = corruption until next IDR.
            (*trunc_log)++;
            char msg[64];
            snprintf(msg, sizeof(msg), "PES_TRUNC: %s PES > %d bytes", tag, cap);
            plog(msg);
        }
        memcpy(buf + *len, pay, copy);
        *len += copy;
    }
    return emitted;
}

int ts_process(TSState *ts, const u8 *pkt,
               u8 *out_vpes, int *out_vlen,
               u8 *out_apes, int *out_alen) {
    static int s_v_trunc_log = 0;
    static int s_a_trunc_log = 0;

    *out_vlen = 0;
    *out_alen = 0;
    if (pkt[0] != TS_SYNC_BYTE) return 0;

    bool pusi    = (pkt[1] & 0x40) != 0;
    u16  pid     = ((u16)(pkt[1] & 0x1F) << 8) | pkt[2];
    u8   afl     = (pkt[3] >> 4) & 3;
    bool has_pay = (afl & 1) != 0;
    if (!has_pay) return 0;

    int  off = 4;
    if (afl & 2) off += pkt[4] + 1;
    if (off >= TS_PACKET_SIZE) return 0;

    const u8 *pay  = pkt + off;
    int       plen = TS_PACKET_SIZE - off;

    if (pid == TS_PID_PAT && pusi && !ts->pmt_pid) {
        int ptr = pay[0];
        if (ptr + 1 < plen) ts_parse_pat(ts, pay + 1 + ptr, plen - 1 - ptr);
        return 0;
    }
    // Keep parsing PMT until both video and audio PIDs are known.
    if (ts->pmt_pid && pid == ts->pmt_pid && pusi &&
        (!ts->video_pid || !ts->audio_pid)) {
        int ptr = pay[0];
        if (ptr + 1 < plen) ts_parse_pmt(ts, pay + 1 + ptr, plen - 1 - ptr);
        return 0;
    }

    int result = 0;

    if (ts->video_pid && pid == ts->video_pid &&
        pes_accumulate(ts->pes_buf, (int)sizeof(ts->pes_buf),
                       &ts->pes_len, &ts->pes_started,
                       pusi, pay, plen, out_vpes, out_vlen,
                       &s_v_trunc_log, "video"))
        result |= 1;

    if (ts->audio_pid && pid == ts->audio_pid &&
        pes_accumulate(ts->a_pes_buf, (int)sizeof(ts->a_pes_buf),
                       &ts->a_pes_len, &ts->a_pes_started,
                       pusi, pay, plen, out_apes, out_alen,
                       &s_a_trunc_log, "audio"))
        result |= 2;

    return result;
}

bool pes_payload(const u8 *pes, int pes_len,
                 const u8 **es, int *es_len, u64 *pts_out) {
    if (pes_len < 9) return false;
    if (pes[0] != 0x00 || pes[1] != 0x00 || pes[2] != 0x01) return false;
    int hdr = 9 + pes[8];
    if (hdr >= pes_len) return false;
    *es     = pes + hdr;
    *es_len = pes_len - hdr;
    if ((pes[7] & 0x80) && pes_len >= 14) {
        *pts_out = ((u64)((pes[9]  & 0x0E) >> 1) << 30) |
                   ((u64)(pes[10])               << 22) |
                   ((u64)((pes[11] & 0xFE) >> 1) << 15) |
                   ((u64)(pes[12])               <<  7) |
                   ((u64)((pes[13] & 0xFE) >> 1));
    } else {
        *pts_out = (u64)VDEC_TS_INVALID;
    }
    return true;
}
