// Matroska as MPEG-TS: see mkv_ts.h.

#include "mkv_ts.h"
#include "aac_adts.h"
#include "flac_dec.h"
#include "resample.h"
#include "pcm_fmt.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_PKT           188
#define PID_PAT          0x0000
#define PID_PMT          0x1000
#define PID_VIDEO        0x0100
#define PID_AUDIO        0x0101
#define FRAME_BUF_SIZE   (1536 * 1024 + 4096)      // H.264 level 4.1's largest access unit (ts_demux.h) and headroom

// stream types the player's demuxer (ts_demux.cpp) selects an audio decoder by
// ST_FLAC and ST_PCM are this player's own private types (ts_demux.cpp): no transport stream has one for them
enum { ST_H264 = 0x1B, ST_MP3 = 0x03, ST_MP2 = 0x04, ST_AAC = 0x0F, ST_AC3 = 0x81, ST_DTS = 0x82, ST_TRUEHD = 0x83,
       ST_FLAC = 0xE1, ST_PCM = 0xE2 };

static uint8_t *s_frame_buf = NULL;

static void set_err(char *err, int cap, const char *msg) {
    if (err && cap > 0) snprintf(err, (size_t)cap, "%s", msg);
}

bool mkv_ts_audio_supported(MkvAudioCodec c) {
    return c == MKV_AC_AC3 || c == MKV_AC_DTS || c == MKV_AC_TRUEHD || c == MKV_AC_MP3 || c == MKV_AC_MP2 || c == MKV_AC_AAC ||
           c == MKV_AC_FLAC || c == MKV_AC_PCM;
}

// The 8-byte header adec_pcm.h puts before every PES of a PCM track, from the track's own fields.  False (with a
// reason) for a layout the player does not play.
static bool pcm_header(const MkvTrack *t, uint8_t hdr[PCM_HEADER_BYTES], const char **why) {
    const char *id = t->codec_id;
    const bool lit = !strcmp(id, "A_PCM/INT/LIT"), big = !strcmp(id, "A_PCM/INT/BIG"), fl = !strcmp(id, "A_PCM/FLOAT/IEEE");
    const int rate = (int)(t->sample_rate + 0.5), bits = t->bit_depth;
    *why = NULL;
    if (!lit && !big && !fl) *why = "unsupported PCM layout";
    else if (t->channels < 1 || t->channels > 8) *why = "unsupported PCM channels";
    else if (fl ? bits != 32 : !(bits == 8 || bits == 16 || bits == 24 || bits == 32)) *why = "unsupported PCM sample size";
    else if (!resample_rate_supported(rate)) *why = "unsupported sample rate";
    if (*why) return false;
    hdr[0] = 'P'; hdr[1] = 'C';
    hdr[2] = (uint8_t)t->channels; hdr[3] = (uint8_t)bits;
    hdr[4] = (uint8_t)((fl ? PCM_FLAG_FLOAT : 0) | (big ? PCM_FLAG_BIG : 0));
    hdr[5] = (uint8_t)(rate >> 16); hdr[6] = (uint8_t)(rate >> 8); hdr[7] = (uint8_t)rate;
    return true;
}

// The stream header adec_flac.h wants before the first frame: "fLaC" and the STREAMINFO block, which Matroska keeps as
// the track's CodecPrivate ("fLaC" and every metadata block).  42 bytes.
#define FLAC_HDR_BYTES 42
static bool flac_header(const MkvTrack *t, uint8_t hdr[FLAC_HDR_BYTES], const char **why) {
    FlacInfo si;
    *why = NULL;
    const uint8_t *cp = t->codec_private;
    const int blen = t->cp_len >= 8 ? (cp[5] << 16) | (cp[6] << 8) | cp[7] : 0;
    if (t->cp_len < 42 || memcmp(cp, "fLaC", 4) != 0 || (cp[4] & 0x7F) != 0 || blen < 34) {
        *why = "unreadable FLAC setup";                   // (other metadata blocks after STREAMINFO do not matter)
        return false;
    }
    memcpy(hdr, "fLaC", 4);
    hdr[4] = 0x80; hdr[5] = 0; hdr[6] = 0; hdr[7] = 34;               // the last (only) block: STREAMINFO, 34 bytes
    memcpy(hdr + 8, cp + 8, 34);
    if (flac_parse_header(hdr, FLAC_HDR_BYTES, &si) <= 0) *why = "unreadable FLAC setup";
    else if (si.channels > 8 || si.bps > 24 || si.bps < 4) *why = "unsupported FLAC format";
    else if (!resample_rate_supported(si.sample_rate)) *why = "unsupported sample rate";
    return *why == NULL;
}

bool mkv_ts_audio_track_supported(const MkvTrack *t, char *reason, int cap) {
    const MkvAudioCodec mc = mkv_audio_codec(t);
    const char *why = NULL;
    uint8_t scratch[FLAC_HDR_BYTES];
    AacConfig aac;
    if (t->unsupported_encoding) why = "encrypted";
    else if (!mkv_ts_audio_supported(mc)) why = "no decoder for this audio";
    else if (mc == MKV_AC_AAC) { if (t->strip_len || !aac_parse_asc(t->codec_private, t->cp_len, &aac)) why = "unsupported AAC setup"; }
    else if (mc == MKV_AC_FLAC) flac_header(t, scratch, &why);
    else if (mc == MKV_AC_PCM) pcm_header(t, scratch, &why);
    if (reason && cap > 0) snprintf(reason, (size_t)cap, "%s", why ? why : "");
    return why == NULL;
}

bool mkv_ts_video_supported(const MkvFile *f, int video_track, char *reason, int cap) {
    const MkvTrack *t = mkv_track(f, video_track);
    const char *why = NULL;
    MkvAvcConfig avc;
    if (!t || t->type != MKV_TRACK_VIDEO) why = "no video";
    else if (t->unsupported_encoding) why = "encrypted";
    else if (t->strip_len) why = "unsupported video encoding";
    else switch (mkv_video_codec(t)) {
    case MKV_VC_AVC:
        if (!mkv_parse_avcc(t->codec_private, t->cp_len, &avc)) why = "unreadable H.264 setup";
        break;
    case MKV_VC_HEVC:  why = "HEVC"; break;
    case MKV_VC_VC1:   why = "VC-1"; break;
    case MKV_VC_MPEG2: why = "MPEG-2"; break;
    case MKV_VC_VP9:   why = "VP9"; break;
    case MKV_VC_AV1:   why = "AV1"; break;
    default:           why = "unsupported video"; break;
    }
    if (reason && cap > 0) snprintf(reason, (size_t)cap, "%s", why ? why : "");
    return why == NULL;
}

// ---- MPEG-2 CRC-32 (PSI sections) ------------------------------------------------------------

static uint32_t crc32_mpeg(const uint8_t *p, int n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i] << 24;
        for (int b = 0; b < 8; b++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

// ---- the generator ---------------------------------------------------------------------------

enum Unit { UNIT_NONE = 0, UNIT_PAT, UNIT_PMT, UNIT_PES };

struct MkvTs {
    MkvFile  *f;
    MkvReader r;
    int       vtrack, atrack;
    uint8_t   a_stream_type, a_stream_id;
    bool      aac;                                  // the audio is AAC: each frame gets an ADTS header
    AacConfig aac_cfg;
    uint8_t   adts[8];
    bool      flac;                                 // FLAC: the stream header goes before the first frame
    uint8_t   flac_hdr[FLAC_HDR_BYTES];
    bool      flac_hdr_pending;
    int       sub_track;                            // a subtitle track whose blocks go to sub_sink as the file is read (0 = none)
    MkvSubSink sub_sink;
    void     *sub_ctx;
    bool      pcm;                                  // uncompressed: every frame carries a header (adec_pcm.h)
    uint8_t   pcm_hdr[PCM_HEADER_BYTES];
    int       nal_len_size;
    uint8_t   pre_key[1024];  int pre_key_len;      // AUD + SPS + PPS: before key frames
    uint8_t   pre_aud[8];     int pre_aud_len;      // AUD only
    int64_t   origin_ns;
    bool      started, ended, error;
    bool      psi_due;
    int       unit;
    uint8_t   cc[4];                                // continuity counters: PAT, PMT, video, audio
    uint8_t   pat[TS_PKT], pmt[TS_PKT];

    // the PES being written
    uint16_t  pid;  int cc_idx;
    uint8_t   hdr[16]; int hdr_len;
    const uint8_t *pre; int pre_len;
    const uint8_t *body; uint32_t body_raw;         // the frame, as stored
    bool      annexb;                               // convert the body's length prefixes to start codes
    uint32_t  body_out;                             // bytes the body comes to
    uint32_t  total, sent;                          // PES bytes: header + prefix + body
    // Annex B conversion state
    uint32_t  rd;  uint32_t nal_left; int sc_left;
    bool      first_pkt;

    MkvTsStats st;
};

static void build_psi(MkvTs *t, bool with_audio) {
    // PAT: program 1 -> PMT_PID
    uint8_t sec[64];
    int n = 0;
    sec[n++] = 0x00;                                // table id
    sec[n++] = 0xB0; sec[n++] = 13;                 // section length: 5 + 4 + 4
    sec[n++] = 0x00; sec[n++] = 0x01;               // transport stream id
    sec[n++] = 0xC1; sec[n++] = 0x00; sec[n++] = 0x00;
    sec[n++] = 0x00; sec[n++] = 0x01;               // program 1
    sec[n++] = (uint8_t)(0xE0 | (PID_PMT >> 8)); sec[n++] = (uint8_t)(PID_PMT & 0xFF);
    uint32_t crc = crc32_mpeg(sec, n);
    sec[n++] = (uint8_t)(crc >> 24); sec[n++] = (uint8_t)(crc >> 16); sec[n++] = (uint8_t)(crc >> 8); sec[n++] = (uint8_t)crc;
    memset(t->pat, 0xFF, TS_PKT);
    t->pat[0] = 0x47; t->pat[1] = 0x40 | (PID_PAT >> 8); t->pat[2] = PID_PAT & 0xFF; t->pat[3] = 0x10;
    t->pat[4] = 0x00;                               // pointer field
    memcpy(t->pat + 5, sec, (size_t)n);

    // PMT: video, and the chosen audio
    n = 0;
    sec[n++] = 0x02;
    const int len_at = n; sec[n++] = 0xB0; sec[n++] = 0;
    sec[n++] = 0x00; sec[n++] = 0x01;               // program number
    sec[n++] = 0xC1; sec[n++] = 0x00; sec[n++] = 0x00;
    sec[n++] = (uint8_t)(0xE0 | (PID_VIDEO >> 8)); sec[n++] = (uint8_t)(PID_VIDEO & 0xFF);   // PCR pid
    sec[n++] = 0xF0; sec[n++] = 0x00;               // program info length 0
    sec[n++] = ST_H264; sec[n++] = (uint8_t)(0xE0 | (PID_VIDEO >> 8)); sec[n++] = (uint8_t)(PID_VIDEO & 0xFF);
    sec[n++] = 0xF0; sec[n++] = 0x00;
    if (with_audio) {
        sec[n++] = t->a_stream_type; sec[n++] = (uint8_t)(0xE0 | (PID_AUDIO >> 8)); sec[n++] = (uint8_t)(PID_AUDIO & 0xFF);
        sec[n++] = 0xF0; sec[n++] = 0x00;
    }
    sec[len_at + 1] = (uint8_t)(n - len_at - 2 + 4);   // section length: what follows its field, CRC included
    crc = crc32_mpeg(sec, n);
    sec[n++] = (uint8_t)(crc >> 24); sec[n++] = (uint8_t)(crc >> 16); sec[n++] = (uint8_t)(crc >> 8); sec[n++] = (uint8_t)crc;
    memset(t->pmt, 0xFF, TS_PKT);
    t->pmt[0] = 0x47; t->pmt[1] = 0x40 | (PID_PMT >> 8); t->pmt[2] = PID_PMT & 0xFF; t->pmt[3] = 0x10;
    t->pmt[4] = 0x00;
    memcpy(t->pmt + 5, sec, (size_t)n);
}

// The 33-bit PTS in its 5-byte PES form.
static void put_pts(uint8_t *p, uint64_t pts, uint8_t marker) {
    p[0] = (uint8_t)(marker | (((pts >> 30) & 7) << 1) | 1);
    p[1] = (uint8_t)(pts >> 22);
    p[2] = (uint8_t)((((pts >> 15) & 0x7F) << 1) | 1);
    p[3] = (uint8_t)(pts >> 7);
    p[4] = (uint8_t)(((pts & 0x7F) << 1) | 1);
}

// The access unit's size once its length prefixes are start codes.  A length that runs past the frame
// ends the unit there (a damaged frame is shortened, not read past).
static uint32_t annexb_size(const MkvTs *t, const uint8_t *p, uint32_t n) {
    uint32_t pos = 0, out = 0;
    while (pos + (uint32_t)t->nal_len_size <= n) {
        uint32_t len = 0;
        for (int i = 0; i < t->nal_len_size; i++) len = (len << 8) | p[pos + (uint32_t)i];
        pos += (uint32_t)t->nal_len_size;
        if (len == 0) continue;
        if (len > n - pos) break;
        out += 4 + len;
        pos += len;
    }
    return out;
}

// Up to `want` bytes of the PES payload at the generator's position.
static int pull_payload(MkvTs *t, uint8_t *dst, int want) {
    int w = 0;
    uint32_t pos = t->sent;                         // position within header + prefix + body
    // header
    if (pos < (uint32_t)t->hdr_len) {
        const int c = (int)std::min<uint32_t>((uint32_t)want, (uint32_t)t->hdr_len - pos);
        memcpy(dst, t->hdr + pos, (size_t)c); w += c; pos += (uint32_t)c;
    }
    // prefix
    if (w < want && pos < (uint32_t)(t->hdr_len + t->pre_len)) {
        const uint32_t off = pos - (uint32_t)t->hdr_len;
        const int c = (int)std::min<uint32_t>((uint32_t)(want - w), (uint32_t)t->pre_len - off);
        memcpy(dst + w, t->pre + off, (size_t)c); w += c; pos += (uint32_t)c;
    }
    // body
    if (!t->annexb) {
        if (w < want) {
            const uint32_t off = pos - (uint32_t)(t->hdr_len + t->pre_len);
            const int c = (int)std::min<uint32_t>((uint32_t)(want - w), t->body_out - off);
            memcpy(dst + w, t->body + off, (size_t)c); w += c;
        }
    } else {
        while (w < want && t->sent + (uint32_t)w < t->total) {
            if (t->sc_left > 0) {                    // 00 00 00 01
                dst[w++] = t->sc_left == 1 ? 1 : 0;
                t->sc_left--;
            } else if (t->nal_left > 0) {
                const int c = (int)std::min<uint32_t>((uint32_t)(want - w), t->nal_left);
                memcpy(dst + w, t->body + t->rd, (size_t)c);
                t->rd += (uint32_t)c; t->nal_left -= (uint32_t)c; w += c;
            } else {
                // the next NAL's length prefix
                uint32_t len = 0;
                for (;;) {
                    if (t->rd + (uint32_t)t->nal_len_size > t->body_raw) { return w; }
                    len = 0;
                    for (int i = 0; i < t->nal_len_size; i++) len = (len << 8) | t->body[t->rd + (uint32_t)i];
                    t->rd += (uint32_t)t->nal_len_size;
                    if (len == 0) continue;
                    if (len > t->body_raw - t->rd) { t->rd = t->body_raw; return w; }
                    break;
                }
                t->nal_left = len;
                t->sc_left = 4;
            }
        }
    }
    return w;
}

// Writes the next packet of the current PES.
static void emit_pes_packet(MkvTs *t, uint8_t *out) {
    const uint32_t remaining = t->total - t->sent;
    uint8_t pay[184];
    const int want = remaining < 184 ? (int)remaining : 184;
    const int got = pull_payload(t, pay, want);
    out[0] = 0x47;
    out[1] = (uint8_t)((t->first_pkt ? 0x40 : 0) | (t->pid >> 8));
    out[2] = (uint8_t)(t->pid & 0xFF);
    const uint8_t cc = t->cc[t->cc_idx]++ & 0x0F;
    if (got == 184) {
        out[3] = (uint8_t)(0x10 | cc);
        memcpy(out + 4, pay, 184);
    } else {
        // the last packet (or a short one): an adaptation field of stuffing takes the slack
        const int af = 183 - got;                    // adaptation_field_length
        out[3] = (uint8_t)(0x30 | cc);
        out[4] = (uint8_t)af;
        if (af > 0) {
            out[5] = 0x00;
            memset(out + 6, 0xFF, (size_t)(af - 1));
        }
        memcpy(out + 5 + af, pay, (size_t)got);
    }
    t->sent += (uint32_t)got;
    t->first_pkt = false;
    if (got < want) t->sent = t->total;              // a damaged frame ran short: finish the unit
}

// ---- frames -> PES --------------------------------------------------------------------------------

static void start_pes(MkvTs *t, uint16_t pid, int cc_idx, uint8_t stream_id, uint64_t pts90, bool video) {
    t->pid = pid; t->cc_idx = cc_idx;
    t->hdr[0] = 0; t->hdr[1] = 0; t->hdr[2] = 1; t->hdr[3] = stream_id;
    t->hdr[6] = 0x80; t->hdr[7] = 0x80; t->hdr[8] = 5;
    put_pts(t->hdr + 9, pts90, 0x20);
    t->hdr_len = 14;
    (void)video;
    t->sent = 0; t->first_pkt = true;
}

// Prepares a PES for the frame; false when it is to be skipped.
static bool prepare_frame(MkvTs *t, const MkvFrame *fr) {
    const int64_t rel = fr->pts_ns - t->origin_ns;
    uint64_t pts90 = (rel > 0 ? (uint64_t)rel * 9ULL / 100000ULL : 0) + MKV_TS_PTS_BASE_90K;
    pts90 &= 0x1FFFFFFFFULL;
    // header stripping: the track's removed bytes go back in front of each frame.  Audio only; a video
    // track that strips headers is not carried (mkv_ts_video_supported does not refuse it, so skip here).
    if (fr->prefix_len && fr->track == t->vtrack) return false;

    if (fr->track == t->vtrack) {
        start_pes(t, PID_VIDEO, 2, 0xE0, pts90, true);
        t->hdr[4] = 0; t->hdr[5] = 0;                // length 0: a video PES may be unbounded
        t->pre = fr->key ? t->pre_key : t->pre_aud;
        t->pre_len = fr->key ? t->pre_key_len : t->pre_aud_len;
        t->body = fr->data; t->body_raw = fr->size; t->annexb = true;
        t->body_out = annexb_size(t, fr->data, fr->size);
        t->rd = 0; t->nal_left = 0; t->sc_left = 0;
        t->total = (uint32_t)t->hdr_len + (uint32_t)t->pre_len + t->body_out;
        t->st.video_frames++;
    } else {
        start_pes(t, PID_AUDIO, 3, t->a_stream_id, pts90, false);
        const uint8_t *pre = fr->prefix; int pre_len = fr->prefix_len;
        if (t->aac) {
            if (!aac_make_adts(&t->aac_cfg, (int)fr->size, t->adts)) return false;     // too big for ADTS: skipped
            pre = t->adts; pre_len = 7;
        } else if (t->flac) {
            if (t->flac_hdr_pending) { pre = t->flac_hdr; pre_len = FLAC_HDR_BYTES; t->flac_hdr_pending = false; }
        } else if (t->pcm) {
            pre = t->pcm_hdr; pre_len = PCM_HEADER_BYTES;
        }
        const uint32_t plen = 3 + 5 + (uint32_t)pre_len + fr->size;
        t->hdr[4] = plen < 65536 ? (uint8_t)(plen >> 8) : 0;
        t->hdr[5] = plen < 65536 ? (uint8_t)plen : 0;
        t->pre = pre; t->pre_len = pre_len;
        t->body = fr->data; t->body_raw = fr->size; t->annexb = false; t->body_out = fr->size;
        t->total = (uint32_t)t->hdr_len + (uint32_t)t->pre_len + fr->size;
        t->st.audio_frames++;
    }
    return true;
}

// Reads frames until one is ready to send; false at the end (or on an error).
static bool next_pes(MkvTs *t) {
    MkvFrame fr;
    for (;;) {
        const int rc = mkv_next_frame(&t->r, &fr);
        if (rc < 0) { t->error = true; return false; }
        if (rc == 0) { t->ended = true; return false; }
        if (fr.track == t->vtrack) {
            if (!t->started) {
                if (!fr.key) continue;               // enter on a key frame
                t->started = true;
                t->origin_ns = fr.pts_ns;
            }
            if (fr.key) t->psi_due = true;
        } else if (t->atrack && fr.track == t->atrack) {
            if (!t->started || fr.pts_ns < t->origin_ns) continue;      // sound from before the picture starts
        } else if (t->sub_track && fr.track == t->sub_track) {
            if (t->sub_sink) t->sub_sink(t->sub_ctx, &fr);              // every block, even before the picture starts: a cue can span it
            continue;
        } else {
            continue;
        }
        t->st.dropped_oversize = t->r.oversize;
        t->st.dropped_damaged = t->r.damaged;
        if (!prepare_frame(t, &fr)) continue;
        return true;
    }
}

// ---- open / read / close -------------------------------------------------------------------------------

MkvTs *mkv_ts_open(MkvFile *f, int video_track, int audio_track, uint64_t start_ns,
                   uint64_t *actual_start_ns, char *err, int err_cap) {
    return mkv_ts_open_subs(f, video_track, audio_track, 0, NULL, NULL, start_ns, actual_start_ns, err, err_cap);
}

MkvTs *mkv_ts_open_subs(MkvFile *f, int video_track, int audio_track, int sub_track, MkvSubSink sink, void *sink_ctx,
                        uint64_t start_ns, uint64_t *actual_start_ns, char *err, int err_cap) {
    char why[48];
    if (!mkv_ts_video_supported(f, video_track, why, sizeof why)) { set_err(err, err_cap, why); return NULL; }
    const MkvTrack *vt = mkv_track(f, video_track);
    MkvAvcConfig avc;
    mkv_parse_avcc(vt->codec_private, vt->cp_len, &avc);

    uint8_t stype = 0, sid = 0xBD;
    bool is_aac = false, is_flac = false, is_pcm = false;
    AacConfig aac_cfg;
    uint8_t flac_hdr[FLAC_HDR_BYTES], pcm_hdr[PCM_HEADER_BYTES];
    memset(&aac_cfg, 0, sizeof aac_cfg);
    if (audio_track) {
        const MkvTrack *at = mkv_track(f, audio_track);
        if (!at || at->type != MKV_TRACK_AUDIO) { set_err(err, err_cap, "no such audio track"); return NULL; }
        if (at->unsupported_encoding) { set_err(err, err_cap, "encrypted audio"); return NULL; }
        switch (mkv_audio_codec(at)) {
        case MKV_AC_AC3:    stype = ST_AC3; break;
        case MKV_AC_DTS:    stype = ST_DTS; break;
        case MKV_AC_TRUEHD: stype = ST_TRUEHD; break;
        case MKV_AC_MP3:    stype = ST_MP3; sid = 0xC0; break;
        case MKV_AC_MP2:    stype = ST_MP2; sid = 0xC0; break;
        case MKV_AC_AAC:
            // the config in CodecPrivate becomes each frame's ADTS header
            if (at->strip_len || !aac_parse_asc(at->codec_private, at->cp_len, &aac_cfg)) { set_err(err, err_cap, "unsupported AAC setup"); return NULL; }
            stype = ST_AAC; sid = 0xC0; is_aac = true; break;
        case MKV_AC_FLAC: {
            const char *why = NULL;
            if (!flac_header(at, flac_hdr, &why)) { set_err(err, err_cap, why); return NULL; }
            stype = ST_FLAC; is_flac = true; break;
        }
        case MKV_AC_PCM: {
            const char *why = NULL;
            if (at->strip_len || !pcm_header(at, pcm_hdr, &why)) { set_err(err, err_cap, why ? why : "unsupported PCM"); return NULL; }
            stype = ST_PCM; is_pcm = true; break;
        }
        default: set_err(err, err_cap, "no decoder for this audio"); return NULL;
        }
    }
    if (!s_frame_buf) s_frame_buf = (uint8_t *)malloc(FRAME_BUF_SIZE);
    if (!s_frame_buf) { set_err(err, err_cap, "out of memory"); return NULL; }

    MkvTs *t = (MkvTs *)calloc(1, sizeof *t);
    if (!t) { set_err(err, err_cap, "out of memory"); return NULL; }
    t->f = f; t->vtrack = video_track; t->atrack = audio_track;
    t->sub_track = sub_track; t->sub_sink = sink; t->sub_ctx = sink_ctx;
    t->a_stream_type = stype; t->a_stream_id = sid;
    t->aac = is_aac; t->aac_cfg = aac_cfg;
    t->flac = is_flac; t->flac_hdr_pending = is_flac; memcpy(t->flac_hdr, flac_hdr, sizeof flac_hdr);
    t->pcm = is_pcm; memcpy(t->pcm_hdr, pcm_hdr, sizeof pcm_hdr);
    t->nal_len_size = avc.nal_length_size;
    // AUD (primary_pic_type 7: any slice types), and for key frames the parameter sets after it
    static const uint8_t aud[] = { 0, 0, 0, 1, 0x09, 0xF0 };
    memcpy(t->pre_aud, aud, sizeof aud); t->pre_aud_len = (int)sizeof aud;
    int n = 0;
    memcpy(t->pre_key, aud, sizeof aud); n = (int)sizeof aud;
    for (int i = 0; i < avc.n_sps && n + 4 + avc.sps_len[i] <= (int)sizeof t->pre_key; i++) {
        t->pre_key[n++] = 0; t->pre_key[n++] = 0; t->pre_key[n++] = 0; t->pre_key[n++] = 1;
        memcpy(t->pre_key + n, avc.sps[i], (size_t)avc.sps_len[i]); n += avc.sps_len[i];
    }
    for (int i = 0; i < avc.n_pps && n + 4 + avc.pps_len[i] <= (int)sizeof t->pre_key; i++) {
        t->pre_key[n++] = 0; t->pre_key[n++] = 0; t->pre_key[n++] = 0; t->pre_key[n++] = 1;
        memcpy(t->pre_key + n, avc.pps[i], (size_t)avc.pps_len[i]); n += avc.pps_len[i];
    }
    t->pre_key_len = n;
    build_psi(t, audio_track != 0);

    mkv_reader_init(&t->r, f, s_frame_buf, FRAME_BUF_SIZE);
    uint64_t cluster_time = 0;
    if (start_ns > 0) {
        if (!mkv_seek(f, &t->r, start_ns, &cluster_time)) { free(t); set_err(err, err_cap, "cannot seek"); return NULL; }
    }
    t->psi_due = true;
    // read up to the first key frame so the caller knows where the stream really starts
    t->unit = UNIT_NONE;
    if (!next_pes(t)) {
        const bool e = t->error;
        free(t);
        set_err(err, err_cap, e ? "read error" : "no picture to start from");
        return NULL;
    }
    t->unit = UNIT_PAT;                              // PSI first, then this frame
    t->psi_due = false;
    if (actual_start_ns) *actual_start_ns = t->origin_ns > 0 ? (uint64_t)t->origin_ns : 0;
    return t;
}

int mkv_ts_read(MkvTs *t, uint8_t *out, int n) {
    int w = 0;
    while (w + TS_PKT <= n) {
        if (t->unit == UNIT_NONE) {
            if (t->error) return w ? w : -1;
            if (t->ended) break;
            if (!next_pes(t)) { if (t->error && w == 0) return -1; break; }
            if (t->psi_due) { t->unit = UNIT_PAT; t->psi_due = false; }
            else            t->unit = UNIT_PES;
        }
        if (t->unit == UNIT_PAT) {
            memcpy(out + w, t->pat, TS_PKT);
            out[w + 3] = (uint8_t)(0x10 | (t->cc[0]++ & 0x0F));
            w += TS_PKT;
            t->unit = UNIT_PMT;
        } else if (t->unit == UNIT_PMT) {
            memcpy(out + w, t->pmt, TS_PKT);
            out[w + 3] = (uint8_t)(0x10 | (t->cc[1]++ & 0x0F));
            w += TS_PKT;
            t->unit = UNIT_PES;
        } else {
            emit_pes_packet(t, out + w);
            w += TS_PKT;
            if (t->sent >= t->total) t->unit = UNIT_NONE;
        }
    }
    return w;
}

void mkv_ts_close(MkvTs *t) { free(t); }

void mkv_ts_release(void) { free(s_frame_buf); s_frame_buf = NULL; }

void mkv_ts_stats(const MkvTs *t, MkvTsStats *out) { *out = t->st; }
