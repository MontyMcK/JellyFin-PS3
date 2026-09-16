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
#define TS_STREAM_AC3    0x81   // ATSC AC-3 (what ffmpeg's mpegts muxer writes)
#define TS_STREAM_PRIV   0x06   // private data — DVB carries AC-3 here with
                                // an AC-3 descriptor in the ES info loop
#define TS_STREAM_EAC3   0x87   // ATSC E-AC-3 — NOT decodable here (liba52
                                // is AC-3 only); recognised only to log it
// DTS stream types.  ffmpeg's mpegts muxer — which is what Jellyfin runs —
// writes 0x82 for AV_CODEC_ID_DTS regardless of whether the copied track is
// plain DTS, DTS-HD HRA, DTS-HD MA or DTS:X (libavformat/mpegtsenc.c,
// get_dvb_stream_type(): `case AV_CODEC_ID_DTS: stream_type =
// STREAM_TYPE_BLURAY_AUDIO_DTS`).  The others appear in Blu-ray-derived
// streams and in ffmpeg's m2ts mode (mpegts.h:160-167,
// get_m2ts_stream_type()), and all of them carry a DTS core, so accept them
// too rather than fall back to silence on a stream we can actually play.
#define TS_STREAM_DTS     0x82  // DTS (what stream.ts actually carries)
#define TS_STREAM_DTS_HRA 0x85  // DTS-HD High Resolution Audio
#define TS_STREAM_DTS_MA  0x86  // DTS-HD Master Audio
#define TS_STREAM_DTS_HD  0x8A  // DTS-HD (mpegts.c:883 maps it to AV_CODEC_ID_DTS)
// Dolby TrueHD (and the TrueHD substream of a Dolby Atmos track).  ffmpeg's
// mpegts muxer writes 0x83 for AV_CODEC_ID_TRUEHD in both its DVB and m2ts
// paths (mpegtsenc.c get_dvb_stream_type()/get_m2ts_stream_type(),
// STREAM_TYPE_BLURAY_AUDIO_TRUEHD in mpegts.h).
#define TS_STREAM_TRUEHD  0x83
#define TS_DESC_REG       0x05  // registration descriptor ('AC-3'/'DTS1' format id)
#define TS_DESC_DVB_AC3   0x6A  // DVB AC-3 descriptor (ETSI EN 300 468 D.3)
#define TS_DESC_DVB_DTS   0x7B  // DVB DTS descriptor (ETSI EN 300 468 D.5);
                                // ffmpeg's demuxer keys on the same tag
                                // (mpegts.c:226 DTS_DESCRIPTOR, :925)

// Registration descriptor format identifier, as a 4-char compare.
static bool desc_is_reg(const u8 *desc, int pos, u8 dlen, const char *tag) {
    return desc[pos] == TS_DESC_REG && dlen >= 4 &&
           desc[pos+2] == (u8)tag[0] && desc[pos+3] == (u8)tag[1] &&
           desc[pos+4] == (u8)tag[2] && desc[pos+5] == (u8)tag[3];
}

// True if the ES descriptor loop marks this PID as AC-3: either a DVB AC-3
// descriptor (0x6A) or a registration descriptor with format 'AC-3'.
static bool es_info_has_ac3(const u8 *desc, int len) {
    int pos = 0;
    while (pos + 2 <= len) {
        u8 tag  = desc[pos];
        u8 dlen = desc[pos + 1];
        if (pos + 2 + dlen > len) break;
        if (tag == TS_DESC_DVB_AC3) return true;
        if (desc_is_reg(desc, pos, dlen, "AC-3")) return true;
        pos += 2 + dlen;
    }
    return false;
}

// Same for DTS: a DVB DTS descriptor (0x7B) or a registration descriptor with
// one of the DTS format ids.  'DTS1'/'DTS2'/'DTS3' are the ones ffmpeg's
// demuxer recognises (libavformat/mpegts.c:901-903); 'DTSH' and 'DTSE' appear
// on DTS-HD and DTS Express tracks from other muxers.  All of them carry a
// decodable core, so they select the same path.
static bool es_info_has_dts(const u8 *desc, int len) {
    int pos = 0;
    while (pos + 2 <= len) {
        u8 tag  = desc[pos];
        u8 dlen = desc[pos + 1];
        if (pos + 2 + dlen > len) break;
        if (tag == TS_DESC_DVB_DTS) return true;
        if (desc_is_reg(desc, pos, dlen, "DTS1") ||
            desc_is_reg(desc, pos, dlen, "DTS2") ||
            desc_is_reg(desc, pos, dlen, "DTS3") ||
            desc_is_reg(desc, pos, dlen, "DTSH") ||
            desc_is_reg(desc, pos, dlen, "DTSE")) return true;
        pos += 2 + dlen;
    }
    return false;
}

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
        if (!ts->audio_pid) {
            // Log every candidate audio stream type so a server/profile
            // mismatch shows up in player_log.txt instead of as silence.
            if (stype == TS_STREAM_MP3 || stype == TS_STREAM_MP3_2 ||
                stype == TS_STREAM_AC3 || stype == TS_STREAM_PRIV ||
                stype == TS_STREAM_EAC3 || stype == TS_STREAM_DTS ||
                stype == TS_STREAM_DTS_HRA || stype == TS_STREAM_DTS_MA ||
                stype == TS_STREAM_DTS_HD || stype == TS_STREAM_TRUEHD) {
                char b[64];
                snprintf(b, sizeof(b), "pmt_audio: stype=0x%02x pid=0x%x",
                         stype, epid);
                plog(b);
            }
            if (stype == TS_STREAM_MP3 || stype == TS_STREAM_MP3_2) {
                ts->audio_pid   = epid;
                ts->audio_codec = TS_AUDIO_MP3;
            } else if (stype == TS_STREAM_AC3) {
                ts->audio_pid   = epid;
                ts->audio_codec = TS_AUDIO_AC3;
            } else if (stype == TS_STREAM_DTS || stype == TS_STREAM_DTS_HRA ||
                       stype == TS_STREAM_DTS_MA || stype == TS_STREAM_DTS_HD) {
                ts->audio_pid   = epid;
                ts->audio_codec = TS_AUDIO_DTS;
            } else if (stype == TS_STREAM_TRUEHD) {
                ts->audio_pid   = epid;
                ts->audio_codec = TS_AUDIO_TRUEHD;
            } else if (stype == TS_STREAM_PRIV && pos + 5 + esinfo <= end &&
                       es_info_has_ac3(data + pos + 5, esinfo)) {
                ts->audio_pid   = epid;
                ts->audio_codec = TS_AUDIO_AC3;
            } else if (stype == TS_STREAM_PRIV && pos + 5 + esinfo <= end &&
                       es_info_has_dts(data + pos + 5, esinfo)) {
                ts->audio_pid   = epid;
                ts->audio_codec = TS_AUDIO_DTS;
            }
            // E-AC-3 (0x87) is deliberately NOT selected: liba52 cannot
            // decode it and the device profile never requests it.  The log
            // line above still records it if a server sends one anyway.
        }
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
